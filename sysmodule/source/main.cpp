#include "common.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// This is the worker. It's started by the resident monitor (sysmodule/monitor) when a game closes or a retry is due,
// does one pass and exits. Everything it allocates, curl and TLS included, is gone again once it returns.

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace
{
    // minizip's zipOpen3 currently has a ~66 KiB stack frame and curl/TLS needs
    // considerably more than the old 1 MiB heap once an upload starts. This is
    // only allocated while the worker is running.
    constexpr size_t INNER_HEAP_SIZE = 0x400000;
    constexpr uint64_t MAX_RETRY_BACKOFF_SECONDS = 60 * 60;
    constexpr const char *INTERRUPTED_MARKER_PATH =
        "sdmc:/config/JKSV/background/.interrupted";

    // How long JKSV gets before the worker tries again when it finds JKSV open.
    constexpr unsigned long long JKSV_RUNNING_RETRY_SECONDS = 5 * 60;

    // Boot times worked out from the clock and uptime drift a little, and the clock can be corrected by the network.
    constexpr long long SAME_BOOT_TOLERANCE_SECONDS = 120;

    bool s_nsReady{};
    bool s_accountReady{};
    bool s_setReady{};
    bool s_pmdmntReady{};
    bool s_pdmReady{};

    bool initialize_title_services()
    {
        s_nsReady      = R_SUCCEEDED(nsInitialize());
        s_accountReady = R_SUCCEEDED(accountInitialize(AccountServiceType_System));
        s_setReady     = R_SUCCEEDED(setInitialize());
        s_pmdmntReady  = R_SUCCEEDED(pmdmntInitialize());
        s_pdmReady     = R_SUCCEEDED(pdmqryInitialize());
        return s_nsReady && s_accountReady && s_setReady && s_pmdmntReady && s_pdmReady;
    }

    bool same_boot(long long bootEpoch)
    {
        return std::llabs(bootEpoch - static_cast<long long>(background::boot_epoch())) <= SAME_BOOT_TOLERANCE_SECONDS;
    }

    // Markers hold the time the console booted, so the next run can tell a crash from a power-off.
    bool write_marker(const char *path)
    {
        FILE *file = std::fopen(path, "wb");
        if (!file) { return false; }
        const bool success = std::fprintf(file, "%lld\n", static_cast<long long>(background::boot_epoch())) > 0 &&
                             std::fflush(file) == 0;
        std::fclose(file);
        return success;
    }

    bool read_marker(const char *path, long long &bootEpoch)
    {
        FILE *file = std::fopen(path, "rb");
        if (!file) { return false; }
        const bool read = std::fscanf(file, "%lld", &bootEpoch) == 1;
        std::fclose(file);
        return read;
    }

    /// @return False if the worker should stay disabled.
    bool check_disabled_state()
    {
        if (!background::file_exists(background::BOOT_FLAG_CRASHED_PATH)) { return true; }

        // JKSV re-enables the worker by putting boot2.flag back. Start counting interruptions over from there.
        if (background::file_exists(background::BOOT_FLAG_PATH))
        {
            background::remove_file(background::BOOT_FLAG_CRASHED_PATH);
            background::remove_file(background::RUN_MARKER_PATH);
            background::remove_file(INTERRUPTED_MARKER_PATH);
            background::log("REENABLED boot2.flag was restored");
            return true;
        }

        background::log("IDLE sysmodule disabled; re-enable it in JKSV's settings");
        return false;
    }

    bool handle_previous_interruption()
    {
        long long bootEpoch{};
        if (!read_marker(background::RUN_MARKER_PATH, bootEpoch))
        {
            background::remove_file(background::RUN_MARKER_PATH);
            return true;
        }
        background::remove_file(background::RUN_MARKER_PATH);

        // Turning the console off in the middle of an upload isn't the worker's fault. Only a run that ended while the
        // console stayed on means the worker itself crashed.
        if (!same_boot(bootEpoch))
        {
            background::log("RECOVERY previous run was cut off by a reboot; queued files were preserved");
            return true;
        }

        if (!background::file_exists(INTERRUPTED_MARKER_PATH))
        {
            write_marker(INTERRUPTED_MARKER_PATH);
            background::log("RECOVERY previous run crashed; queued files were preserved");
            return true;
        }

        background::log("SELF_DISABLE two consecutive runs crashed");
        if (::rename(background::BOOT_FLAG_PATH, background::BOOT_FLAG_CRASHED_PATH) != 0 && errno != ENOENT)
        {
            background::logf("SELF_DISABLE_FAILED errno=%d", errno);
        }
        else
        {
            // The monitor stops starting the worker once boot2.flag.crashed exists without boot2.flag.
            FILE *crashed = std::fopen(background::BOOT_FLAG_CRASHED_PATH, "ab");
            if (crashed) { std::fclose(crashed); }
        }
        background::queue_ultrahand_message("后台自动备份连续异常退出，已停用。可在 JKSV 设置中重新启用");
        return false;
    }

    void begin_guarded_operation() { write_marker(background::RUN_MARKER_PATH); }

    void end_guarded_operation()
    {
        background::remove_file(background::RUN_MARKER_PATH);
        background::remove_file(INTERRUPTED_MARKER_PATH);
    }

    uint64_t retry_delay_seconds(const background::Settings &settings, unsigned failedBatches)
    {
        const uint64_t base = static_cast<uint64_t>(settings.retryMinutes) * 60;
        const uint64_t cap  = std::max(base, MAX_RETRY_BACKOFF_SECONDS);
        uint64_t delay      = base;
        for (unsigned failure = 1; failure < failedBatches && delay < cap; ++failure)
        {
            delay = std::min(delay * 2, cap);
        }
        return delay;
    }

    unsigned read_failed_batches()
    {
        FILE *file = std::fopen(background::RETRY_STATE_PATH, "rb");
        if (!file) { return 0; }
        unsigned failedBatches{};
        if (std::fscanf(file, "%u", &failedBatches) != 1) { failedBatches = 0; }
        std::fclose(file);
        return failedBatches;
    }

    void write_number(const char *path, unsigned long long value)
    {
        const std::string temporary = std::string(path) + ".tmp";
        FILE *file                  = std::fopen(temporary.c_str(), "wb");
        if (!file) { return; }
        const bool written = std::fprintf(file, "%llu\n", value) > 0 && std::fflush(file) == 0;
        std::fclose(file);
        if (!written || !background::atomic_replace(temporary, path)) { background::remove_file(temporary); }
    }

    // The monitor waits this long after the worker exits before starting it again. It only ever wakes up for this
    // while there's something left in the queue.
    void schedule_retry(const background::Settings &settings, size_t uploadedCount, bool newSaves)
    {
        if (!background::pending_backups_exist())
        {
            background::remove_file(background::RETRY_AFTER_PATH);
            background::remove_file(background::RETRY_STATE_PATH);
            return;
        }

        // A new save should get the base interval even if older queued work had
        // reached the retry backoff cap.
        unsigned failedBatches = newSaves ? 0 : read_failed_batches();
        if (uploadedCount != 0) { failedBatches = 1; }
        else { failedBatches = std::min(failedBatches + 1, 32U); }

        const uint64_t delay = retry_delay_seconds(settings, failedBatches);
        write_number(background::RETRY_STATE_PATH, failedBatches);
        write_number(background::RETRY_AFTER_PATH, delay);
        background::logf("RETRY_SCHEDULED delay=%llus consecutive-failures=%u",
                         static_cast<unsigned long long>(delay),
                         failedBatches);
    }

    /// @return False if a game was started during the delay.
    bool wait_for_close_delay(const background::Settings &settings)
    {
        for (int second = 0; second < settings.closeDelaySeconds; ++second)
        {
            if (background::application_running()) { return false; }
            background::sleep_seconds(1);
        }
        return !background::application_running();
    }

    void log_memory_usage()
    {
        uint64_t used{};
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        background::logf("WORKER_EXIT memory=%lluKiB", static_cast<unsigned long long>(used / 1024));
    }
} // namespace

bool background::application_running()
{
    uint64_t pid{};
    return s_pmdmntReady && R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
}

bool background::jksv_running()
{
    FILE *file = std::fopen(background::JKSV_RUNNING_PATH, "rb");
    if (!file) { return false; }
    unsigned long long pid{};
    long long bootEpoch{};
    const bool parsed = std::fscanf(file, "%llu %lld", &pid, &bootEpoch) == 2;
    std::fclose(file);

    // Process IDs only count up within a boot, so a live process with this ID from this boot is still JKSV. A file
    // left over from a crash or an earlier boot is removed so it can't hold uploads back forever.
    uint64_t programId{};
    const bool running = parsed && same_boot(bootEpoch) && s_pmdmntReady &&
                         R_SUCCEEDED(pmdmntGetProgramId(&programId, pid));
    if (!running) { background::remove_file(background::JKSV_RUNNING_PATH); }
    return running;
}

extern "C"
{
    u32 __nx_applet_type = AppletType_None;
    u32 __nx_fs_num_sessions = 2;

    extern void __libnx_init_time(void);

    void __libnx_initheap(void)
    {
        static uint8_t heap[INNER_HEAP_SIZE];
        extern void *fake_heap_start;
        extern void *fake_heap_end;
        fake_heap_start = heap;
        fake_heap_end   = heap + sizeof(heap);
    }

    void __appInit(void)
    {
        // A fatal error over a backup that can wait for the next game would be far worse than skipping it.
        if (R_FAILED(smInitialize())) { svcExitProcess(); }

        if (R_SUCCEEDED(setsysInitialize()))
        {
            SetSysFirmwareVersion firmware{};
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware)))
            {
                hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
            }
            setsysExit();
        }

        if (R_SUCCEEDED(timeInitialize())) { __libnx_init_time(); }
        if (R_FAILED(fsInitialize())) { svcExitProcess(); }
        fsdevMountSdmc();
        // Keep SM alive: DNS and TLS request services lazily during the first upload.
    }

    void __appExit(void)
    {
        if (s_pdmReady) { pdmqryExit(); }
        if (s_pmdmntReady) { pmdmntExit(); }
        if (s_setReady) { setExit(); }
        if (s_accountReady) { accountExit(); }
        if (s_nsReady) { nsExit(); }
        fsdevUnmountAll();
        timeExit();
        fsExit();
        smExit();
    }
}

int main(int, char **)
{
    background::ensure_directory(background::QUEUE_DIRECTORY);
    background::ensure_directory(background::STATE_DIRECTORY);
    background::log("WORKER_START version=" APP_VERSION " heap=4096KiB stack=512KiB");

    // The monitor managed to start the worker, so whatever kept it from doing that is fixed.
    background::remove_file(background::MONITOR_LAUNCH_FAILED_PATH);

    if (!check_disabled_state() || !handle_previous_interruption()) { return 0; }

    if (!initialize_title_services())
    {
        // The monitor tries again with the next game that closes.
        background::log("SERVICE_UNAVAILABLE title services could not be opened");
        return 0;
    }

    background::Settings settings{};
    if (!background::load_settings(settings) || !settings.enabled)
    {
        background::remove_file(background::RETRY_AFTER_PATH);
        return 0;
    }

    // JKSV might be restoring the very save this would read, and it uploads on its own anyway. Nothing is read from
    // the play log yet, so whatever closed is still found when this runs again.
    if (background::jksv_running())
    {
        background::log("WORKER_DEFERRED jksv-running");
        write_number(background::RETRY_AFTER_PATH, JKSV_RUNNING_RETRY_SECONDS);
        return 0;
    }

    // Closing a game and starting another right away shouldn't have to wait on this.
    if (!wait_for_close_delay(settings))
    {
        background::log("WORKER_DEFERRED game-running");
        return 0;
    }

    int32_t newestEntry{};
    std::vector<uint64_t> titles{};
    const bool playLogRead = background::closed_titles_since_last_run(titles, newestEntry);

    size_t uploadedCount{};
    size_t failedTitles{};
    bool titlesDeferred{};
    begin_guarded_operation();
    for (const uint64_t title : titles)
    {
        // Left in the play log for the next run. The game that just started will close again eventually.
        if (background::application_running())
        {
            titlesDeferred = true;
            break;
        }
        if (!background::prepare_title_backups(title, settings))
        {
            if (background::application_running())
            {
                titlesDeferred = true;
                break;
            }
            ++failedTitles;
        }
    }

    // The position only moves past games that were dealt with. A game that failed outright would most likely fail
    // again, so it's reported instead of retried with every game that closes from now on.
    if (playLogRead && !titlesDeferred) { background::save_play_log_position(newestEntry); }
    if (failedTitles != 0)
    {
        background::logf("BACKUP_SUMMARY failed-titles=%zu", failedTitles);
        char text[128]{};
        std::snprintf(text, sizeof(text), "有 %zu 个游戏的存档未能备份，详见 background-sync.log", failedTitles);
        background::queue_ultrahand_message(text);
    }

    if (!background::application_running() && !background::jksv_running())
    {
        uploadedCount = background::process_pending_backups(settings);
    }
    end_guarded_operation();

    if (uploadedCount != 0) { background::queue_ultrahand_notification(uploadedCount); }
    schedule_retry(settings, uploadedCount, !titles.empty());
    log_memory_usage();
    return 0;
}
