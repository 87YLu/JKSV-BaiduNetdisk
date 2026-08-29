#include "common.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace background
{
    std::atomic<uint64_t> g_runningTitleId{};
    std::atomic<uint64_t> g_pendingTitleId{};
    std::atomic<bool> g_shutdownRequested{};
}

namespace
{
    // minizip's zipOpen3 currently has a ~66 KiB stack frame and curl/TLS needs
    // considerably more than the old 1 MiB heap once an upload starts.
    constexpr size_t INNER_HEAP_SIZE = 0x400000;
    constexpr int64_t STARTUP_GRACE_NS = 30'000'000'000LL;
    // Poll quickly only while an application is running so game-exit backups
    // stay responsive. With no application, a five-second poll avoids waking
    // the process (and pmdmnt) twice per second all night.
    constexpr int64_t MONITOR_ACTIVE_POLL_NS = 1'000'000'000LL;
    constexpr int64_t MONITOR_IDLE_POLL_NS   = 5'000'000'000LL;
    constexpr uint64_t MAIN_IDLE_WAIT_NS     = 30'000'000'000ULL;
    constexpr const char *INTERRUPTED_MARKER_PATH =
        "sdmc:/config/JKSV/background/.interrupted";

    bool s_nsReady{};
    bool s_accountReady{};
    bool s_setReady{};
    bool s_pmdmntReady{};
    bool s_pminfoReady{};
    Thread s_monitorThread{};
    alignas(0x1000) uint8_t s_monitorStack[0x4000]{};
    Event s_workEvent{};
    bool s_workEventReady{};

    void wait_for_work(uint64_t timeoutNs)
    {
        if (s_workEventReady) { eventWait(&s_workEvent, timeoutNs); }
        else { svcSleepThread(static_cast<int64_t>(timeoutNs)); }
    }

    bool valid_game_title(uint64_t titleId)
    {
        return titleId >= 0x0100000000010000ULL && titleId < 0x0200000000000000ULL;
    }

    bool initialize_title_services()
    {
        if (s_nsReady && s_accountReady && s_setReady && s_pmdmntReady && s_pminfoReady) { return true; }

        if (!s_nsReady) { s_nsReady = R_SUCCEEDED(nsInitialize()); }
        if (!s_accountReady)
        {
            s_accountReady = R_SUCCEEDED(accountInitialize(AccountServiceType_System));
        }
        if (!s_setReady) { s_setReady = R_SUCCEEDED(setInitialize()); }
        if (!s_pmdmntReady) { s_pmdmntReady = R_SUCCEEDED(pmdmntInitialize()); }
        if (!s_pminfoReady) { s_pminfoReady = R_SUCCEEDED(pminfoInitialize()); }
        return s_nsReady && s_accountReady && s_setReady && s_pmdmntReady && s_pminfoReady;
    }

    void signal_closed_title(uint64_t titleId)
    {
        if (!valid_game_title(titleId)) { return; }
        uint64_t empty{};
        if (background::g_pendingTitleId.compare_exchange_strong(
                empty, titleId, std::memory_order_release, std::memory_order_relaxed))
        {
            if (s_workEventReady) { eventFire(&s_workEvent); }
        }
        else
        {
            background::logf("GAME_CLOSE_DEFERRED title=%016llX pending=%016llX",
                             static_cast<unsigned long long>(titleId),
                             static_cast<unsigned long long>(empty));
        }
    }

    void game_monitor(void *)
    {
        uint64_t lastPid{};
        uint64_t lastTitle{};
        int failureCount{};
        int runningTicks{};

        while (!background::g_shutdownRequested.load(std::memory_order_acquire))
        {
            uint64_t pid{};
            const Result result = pmdmntGetApplicationProcessId(&pid);
            if (R_SUCCEEDED(result) && pid != 0)
            {
                failureCount = 0;
                if (pid != lastPid)
                {
                    lastPid = pid;
                    lastTitle = 0;
                    runningTicks = 0;
                    pminfoGetProgramId(&lastTitle, pid);
                    background::logf("GAME_START title=%016llX",
                                     static_cast<unsigned long long>(lastTitle));
                }
                ++runningTicks;
                // A PID alone is enough to block save access. If pminfo briefly
                // fails, use a non-zero sentinel instead of assuming no game runs.
                background::g_runningTitleId.store(lastTitle != 0 ? lastTitle : 1,
                                                   std::memory_order_release);
            }
            else
            {
                ++failureCount;
                if (failureCount >= 2)
                {
                    if (lastPid != 0 && lastTitle != 0 && runningTicks >= 2)
                    {
                        background::logf("GAME_CLOSE title=%016llX",
                                         static_cast<unsigned long long>(lastTitle));
                        signal_closed_title(lastTitle);
                    }
                    lastPid = 0;
                    lastTitle = 0;
                    runningTicks = 0;
                    failureCount = 0;
                    background::g_runningTitleId.store(0, std::memory_order_release);
                }
            }
            svcSleepThread(lastPid != 0 ? MONITOR_ACTIVE_POLL_NS : MONITOR_IDLE_POLL_NS);
        }
    }

    bool write_marker(const char *path)
    {
        FILE *file = std::fopen(path, "wb");
        if (!file) { return false; }
        const std::time_t now = std::time(nullptr);
        const bool success = std::fprintf(file, "%lld\n", static_cast<long long>(now)) > 0 &&
                             std::fflush(file) == 0;
        std::fclose(file);
        return success;
    }

    bool handle_previous_interruption()
    {
        if (!background::file_exists(background::RUN_MARKER_PATH)) { return true; }

        background::remove_file(background::RUN_MARKER_PATH);
        if (!background::file_exists(INTERRUPTED_MARKER_PATH))
        {
            write_marker(INTERRUPTED_MARKER_PATH);
            background::log("RECOVERY previous operation was interrupted; queued files were preserved");
            return true;
        }

        background::log("SELF_DISABLE two consecutive operations were interrupted");
        if (::rename(background::BOOT_FLAG_PATH, background::BOOT_FLAG_CRASHED_PATH) != 0 && errno != ENOENT)
        {
            background::logf("SELF_DISABLE_FAILED errno=%d", errno);
        }
        return false;
    }

    void begin_guarded_operation() { write_marker(background::RUN_MARKER_PATH); }

    void end_guarded_operation()
    {
        background::remove_file(background::RUN_MARKER_PATH);
        background::remove_file(INTERRUPTED_MARKER_PATH);
    }

} // namespace

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
        Result result = smInitialize();
        if (R_FAILED(result)) { diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM)); }

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
        result = fsInitialize();
        if (R_FAILED(result)) { diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS)); }
        fsdevMountSdmc();
        // Keep SM alive: DNS and TLS request services lazily during the first upload.
    }

    void __appExit(void)
    {
        background::g_shutdownRequested.store(true, std::memory_order_release);
        if (s_workEventReady)
        {
            eventFire(&s_workEvent);
            eventClose(&s_workEvent);
            s_workEventReady = false;
        }
        if (s_pminfoReady) { pminfoExit(); }
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
    background::log(
        "START version=1.0.1 heap=4096KiB stack=512KiB background-sync startup-grace=30s low-power-idle=30s");

    if (!handle_previous_interruption())
    {
        background::log("IDLE sysmodule disabled; rename boot2.flag.crashed to boot2.flag after inspection");
        while (true) { background::sleep_seconds(3600); }
    }
    svcSleepThread(STARTUP_GRACE_NS);
    while (!initialize_title_services())
    {
        background::log("SERVICE_RETRY title monitoring services unavailable");
        background::sleep_seconds(10);
    }

    const Result eventResult = eventCreate(&s_workEvent, true);
    s_workEventReady = R_SUCCEEDED(eventResult);
    if (!s_workEventReady)
    {
        background::logf("WORK_EVENT_CREATE_FAILED result=0x%08X; using timed waits", eventResult);
    }

    const Result monitorResult = threadCreate(&s_monitorThread,
                                               game_monitor,
                                               nullptr,
                                               s_monitorStack,
                                               sizeof(s_monitorStack),
                                               44,
                                               3);
    if (R_FAILED(monitorResult) || R_FAILED(threadStart(&s_monitorThread)))
    {
        background::logf("MONITOR_START_FAILED result=0x%08X", monitorResult);
        while (true) { background::sleep_seconds(60); }
    }
    background::log("READY monitoring application exits");

    std::time_t lastQueueAttempt{};
    uint64_t deferredTitle{};
    while (true)
    {
        background::Settings settings{};
        if (!background::load_settings(settings) || !settings.enabled)
        {
            wait_for_work(MAIN_IDLE_WAIT_NS);
            continue;
        }

        if (background::g_runningTitleId.load(std::memory_order_acquire) != 0)
        {
            // The monitor signals this event immediately after a confirmed
            // game exit, so this does not add a 30-second backup delay.
            wait_for_work(MAIN_IDLE_WAIT_NS);
            continue;
        }

        if (deferredTitle == 0)
        {
            deferredTitle = background::g_pendingTitleId.exchange(0, std::memory_order_acq_rel);
        }

        if (deferredTitle != 0)
        {
            const uint64_t title = deferredTitle;
            deferredTitle = 0;
            for (int second = 0; second < settings.closeDelaySeconds; ++second)
            {
                if (background::g_runningTitleId.load(std::memory_order_acquire) != 0)
                {
                    deferredTitle = title;
                    break;
                }
                background::sleep_seconds(1);
            }
            if (deferredTitle != 0) { continue; }

            size_t uploadedCount{};
            begin_guarded_operation();
            if (background::prepare_title_backups(title, settings))
            {
                uploadedCount = background::process_pending_backups(settings);
            }
            end_guarded_operation();
            if (uploadedCount != 0) { background::queue_ultrahand_notification(uploadedCount); }
            lastQueueAttempt = std::time(nullptr);
            continue;
        }

        const std::time_t now = std::time(nullptr);
        if (background::pending_backups_exist() &&
            (lastQueueAttempt == 0 || now - lastQueueAttempt >= settings.retryMinutes * 60))
        {
            size_t uploadedCount{};
            begin_guarded_operation();
            uploadedCount = background::process_pending_backups(settings);
            end_guarded_operation();
            if (uploadedCount != 0) { background::queue_ultrahand_notification(uploadedCount); }
            lastQueueAttempt = now;
        }

        wait_for_work(MAIN_IDLE_WAIT_NS);
    }
}
