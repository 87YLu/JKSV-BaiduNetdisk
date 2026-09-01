#include "common.hpp"

#include <algorithm>
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
    // These intervals are only used when the process-event observer is not
    // available (for example on pre-10.0.0 firmware).
    constexpr int64_t MONITOR_ACTIVE_POLL_NS = 1'000'000'000LL;
    constexpr int64_t MONITOR_IDLE_POLL_NS   = 5'000'000'000LL;
    constexpr uint64_t MONITOR_EVENT_RECONCILE_NS = 900'000'000'000ULL;
    constexpr int64_t MONITOR_QUERY_RETRY_NS       = 250'000'000LL;
    constexpr uint64_t MAIN_MAINTENANCE_WAIT_NS    = 900'000'000'000ULL;
    constexpr uint64_t MAIN_FALLBACK_POLL_NS       = 30'000'000'000ULL;
    constexpr uint64_t MAX_RETRY_BACKOFF_SECONDS   = 60 * 60;
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
    PglEventObserver s_processObserver{};
    Event s_processEvent{};
    bool s_pglReady{};
    bool s_processObserverReady{};
    bool s_processEventReady{};

    struct MonitorState
    {
        uint64_t pid{};
        uint64_t title{};
        int runningTicks{};
    };

    void wait_for_work(uint64_t timeoutNs)
    {
        if (s_workEventReady) { eventWait(&s_workEvent, timeoutNs); }
        else
        {
            const uint64_t fallback = std::min(timeoutNs, MAIN_FALLBACK_POLL_NS);
            svcSleepThread(static_cast<int64_t>(fallback));
        }
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

    bool initialize_process_events()
    {
        Result result = pglInitialize();
        if (R_FAILED(result))
        {
            background::logf("MONITOR_EVENTS_UNAVAILABLE stage=pgl result=0x%08X", result);
            return false;
        }
        s_pglReady = true;

        result = pglGetEventObserver(&s_processObserver);
        if (R_FAILED(result))
        {
            background::logf("MONITOR_EVENTS_UNAVAILABLE stage=observer result=0x%08X", result);
            pglExit();
            s_pglReady = false;
            return false;
        }
        s_processObserverReady = true;

        result = pglEventObserverGetProcessEvent(&s_processObserver, &s_processEvent);
        if (R_FAILED(result))
        {
            background::logf("MONITOR_EVENTS_UNAVAILABLE stage=event result=0x%08X", result);
            pglEventObserverClose(&s_processObserver);
            s_processObserverReady = false;
            pglExit();
            s_pglReady = false;
            return false;
        }
        s_processEventReady = true;
        return true;
    }

    void signal_closed_title(uint64_t titleId)
    {
        if (valid_game_title(titleId))
        {
            uint64_t empty{};
            const bool queued = background::g_pendingTitleId.compare_exchange_strong(
                empty, titleId, std::memory_order_release, std::memory_order_relaxed);
            if (!queued)
            {
                background::logf("GAME_CLOSE_DEFERRED title=%016llX pending=%016llX",
                                 static_cast<unsigned long long>(titleId),
                                 static_cast<unsigned long long>(empty));
            }
        }
        // Wake the main loop even when another title is already pending. This
        // also resumes retries after a non-game application closes.
        if (s_workEventReady) { eventFire(&s_workEvent); }
    }

    void close_monitored_application(MonitorState &state, bool anotherApplicationRunning = false)
    {
        const uint64_t closedTitle = state.title;
        const bool shouldWake = state.pid != 0 && state.runningTicks >= 2;
        state = {};
        // Publish the state before firing the work event. When applications
        // transition directly, keep a sentinel set so queue work stays blocked.
        background::g_runningTitleId.store(anotherApplicationRunning ? 1 : 0,
                                           std::memory_order_release);
        if (shouldWake)
        {
            if (closedTitle != 0)
            {
                background::logf("GAME_CLOSE title=%016llX",
                                 static_cast<unsigned long long>(closedTitle));
            }
            signal_closed_title(closedTitle);
        }
    }

    void observe_running_application(MonitorState &state, uint64_t pid, bool eventConfirmed)
    {
        if (pid == 0) { return; }
        if (pid != state.pid)
        {
            if (state.pid != 0) { close_monitored_application(state, true); }
            state.pid = pid;
            pminfoGetProgramId(&state.title, pid);
            background::logf("GAME_START title=%016llX",
                             static_cast<unsigned long long>(state.title));
        }
        else if (state.title == 0)
        {
            pminfoGetProgramId(&state.title, pid);
        }

        if (eventConfirmed) { state.runningTicks = std::max(state.runningTicks, 2); }
        else { ++state.runningTicks; }
        background::g_runningTitleId.store(state.title != 0 ? state.title : 1,
                                           std::memory_order_release);
    }

    bool query_application_pid(uint64_t &pid)
    {
        pid = 0;
        return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
    }

    void reconcile_event_monitor(MonitorState &state, bool eventTriggered)
    {
        uint64_t pid{};
        if (!query_application_pid(pid) && (eventTriggered || state.pid != 0))
        {
            svcSleepThread(MONITOR_QUERY_RETRY_NS);
            query_application_pid(pid);
        }

        if (pid != 0) { observe_running_application(state, pid, true); }
        else if (state.pid != 0) { close_monitored_application(state); }
        else { background::g_runningTitleId.store(0, std::memory_order_release); }
    }

    bool drain_process_events()
    {
        bool received{};
        for (int index = 0; index < 64; ++index)
        {
            PmProcessEventInfo info{};
            const Result result = pglEventObserverGetProcessEventInfo(&s_processObserver, &info);
            if (R_FAILED(result)) { return received; }
            if (info.event == PmProcessEvent_None) { return true; }
            received = true;
        }
        return true;
    }

    bool monitor_with_process_events(MonitorState &state)
    {
        reconcile_event_monitor(state, false);
        int failureCount{};

        while (!background::g_shutdownRequested.load(std::memory_order_acquire))
        {
            const Result result = eventWait(&s_processEvent, MONITOR_EVENT_RECONCILE_NS);
            const bool timedOut = R_VALUE(result) == R_VALUE(KERNELRESULT(TimedOut));
            if (R_SUCCEEDED(result))
            {
                if (!drain_process_events())
                {
                    ++failureCount;
                    background::logf("MONITOR_EVENT_READ_FAILED count=%d", failureCount);
                }
                else { failureCount = 0; }
                reconcile_event_monitor(state, true);
            }
            else if (timedOut)
            {
                failureCount = 0;
                reconcile_event_monitor(state, false);
            }
            else
            {
                ++failureCount;
                background::logf("MONITOR_EVENT_WAIT_FAILED result=0x%08X count=%d", result, failureCount);
            }

            if (failureCount >= 3)
            {
                background::log("MONITOR_MODE fallback=polling reason=event-errors");
                return false;
            }
        }
        return true;
    }

    void monitor_with_polling(MonitorState state)
    {
        int failureCount{};

        while (!background::g_shutdownRequested.load(std::memory_order_acquire))
        {
            uint64_t pid{};
            const Result result = pmdmntGetApplicationProcessId(&pid);
            if (R_SUCCEEDED(result) && pid != 0)
            {
                failureCount = 0;
                observe_running_application(state, pid, false);
            }
            else
            {
                ++failureCount;
                if (failureCount >= 2)
                {
                    close_monitored_application(state);
                    failureCount = 0;
                }
            }
            svcSleepThread(state.pid != 0 ? MONITOR_ACTIVE_POLL_NS : MONITOR_IDLE_POLL_NS);
        }
    }

    void game_monitor(void *)
    {
        MonitorState state{};
        if (s_processEventReady && monitor_with_process_events(state)) { return; }
        monitor_with_polling(state);
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

    uint64_t retry_wait_ns(const background::Settings &settings,
                           std::time_t lastAttempt,
                           std::time_t now,
                           unsigned failedBatches)
    {
        if (lastAttempt == 0) { return 0; }
        const uint64_t delay = retry_delay_seconds(settings, failedBatches);
        const uint64_t elapsed = now >= lastAttempt ? static_cast<uint64_t>(now - lastAttempt) : 0;
        if (elapsed >= delay) { return 0; }
        return std::min<uint64_t>((delay - elapsed) * 1'000'000'000ULL,
                                  MAIN_MAINTENANCE_WAIT_NS);
    }

    void record_queue_attempt(const background::Settings &settings,
                              std::time_t &lastAttempt,
                              unsigned &failedBatches,
                              size_t uploadedCount)
    {
        lastAttempt = std::time(nullptr);
        if (!background::pending_backups_exist())
        {
            failedBatches = 0;
            return;
        }

        if (uploadedCount != 0) { failedBatches = 1; }
        else { failedBatches = std::min(failedBatches + 1, 32U); }
        background::logf("RETRY_SCHEDULED delay=%llus consecutive-failures=%u",
                         static_cast<unsigned long long>(retry_delay_seconds(settings, failedBatches)),
                         failedBatches);
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
        if (s_processEventReady)
        {
            eventClose(&s_processEvent);
            s_processEventReady = false;
        }
        if (s_processObserverReady)
        {
            pglEventObserverClose(&s_processObserver);
            s_processObserverReady = false;
        }
        if (s_pglReady)
        {
            pglExit();
            s_pglReady = false;
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
        "START version=1.0.2 heap=4096KiB stack=512KiB background-sync startup-grace=30s power-aware-idle");

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

    if (initialize_process_events())
    {
        background::log("MONITOR_MODE process-events reconcile=15m");
    }
    else
    {
        background::log("MONITOR_MODE polling active=1s idle=5s");
    }

    uint64_t initialPid{};
    if (query_application_pid(initialPid))
    {
        // Block queue work until the monitor resolves the title ID.
        background::g_runningTitleId.store(1, std::memory_order_release);
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
    unsigned failedQueueBatches{};
    uint64_t deferredTitle{};
    while (true)
    {
        background::Settings settings{};
        if (!background::load_settings(settings) || !settings.enabled)
        {
            wait_for_work(MAIN_MAINTENANCE_WAIT_NS);
            continue;
        }

        if (background::g_runningTitleId.load(std::memory_order_acquire) != 0)
        {
            // The monitor signals this event immediately after a confirmed
            // game exit, so the maintenance timeout does not delay backups.
            wait_for_work(MAIN_MAINTENANCE_WAIT_NS);
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

            // A new save should get an immediate upload attempt even if older
            // queued work had reached the retry backoff cap.
            failedQueueBatches = 0;
            size_t uploadedCount{};
            begin_guarded_operation();
            if (background::prepare_title_backups(title, settings))
            {
                uploadedCount = background::process_pending_backups(settings);
            }
            end_guarded_operation();
            if (uploadedCount != 0) { background::queue_ultrahand_notification(uploadedCount); }
            record_queue_attempt(settings, lastQueueAttempt, failedQueueBatches, uploadedCount);
            continue;
        }

        if (!background::pending_backups_exist())
        {
            lastQueueAttempt = 0;
            failedQueueBatches = 0;
            wait_for_work(MAIN_MAINTENANCE_WAIT_NS);
            continue;
        }

        const std::time_t now = std::time(nullptr);
        const uint64_t retryWait = retry_wait_ns(settings,
                                                 lastQueueAttempt,
                                                 now,
                                                 failedQueueBatches);
        if (retryWait != 0)
        {
            wait_for_work(retryWait);
            continue;
        }

        size_t uploadedCount{};
        begin_guarded_operation();
        uploadedCount = background::process_pending_backups(settings);
        end_guarded_operation();
        if (uploadedCount != 0) { background::queue_ultrahand_notification(uploadedCount); }
        record_queue_attempt(settings, lastQueueAttempt, failedQueueBatches, uploadedCount);
    }
}
