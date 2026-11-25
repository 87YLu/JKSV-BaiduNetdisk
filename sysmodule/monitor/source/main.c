// The resident half of the background sync. It stays loaded for as long as the console is on, so it does as close to
// nothing as possible: no stdio, sockets, curl or JSON, and no polling on any firmware with pgl. Its only thread sits in
// the kernel until a process exits, then it starts the worker (program 420000004A4B5345), which does the actual backup
// and upload and exits again.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#define PROGRAM_ID_WORKER 0x420000004A4B5345ULL

// Keep these in sync with sysmodule/include/common.hpp. The raw FS API takes paths from the root of the SD card.
#define PATH_CONFIG         "/config/JKSV/baidu.json"
#define PATH_CONFIG_TEMP    "/config/JKSV/baidu.json.tmp"
#define PATH_LOGOUT_MARKER  "/config/JKSV/baidu.logged_out"
#define PATH_QUEUE          "/config/JKSV/background/queue"
#define PATH_RETRY_AFTER    "/config/JKSV/background/retry_after"
#define PATH_LAUNCH_FAILED  "/config/JKSV/background/monitor_launch_failed"
#define PATH_BOOT_FLAG      "/atmosphere/contents/420000004A4B5344/flags/boot2.flag"
#define PATH_DISABLED_FLAG  "/atmosphere/contents/420000004A4B5344/flags/boot2.flag.crashed"

#define NS_STARTUP_GRACE 30000000000ULL

// Only used when pgl isn't available, which is firmware older than 10.0.0. A game that closes is noticed within this
// long, which is plenty for a backup that waits a few seconds after closing anyway.
#define NS_FALLBACK_POLL 5000000000ULL

// Doubles with every launch that fails in a row, so a worker that can't start doesn't keep the console busy.
#define NS_LAUNCH_RETRY       60000000000ULL
#define NS_LAUNCH_RETRY_MAX   3600000000000ULL
#define LAUNCH_FAILURES_NOTICE 3
#define NS_WAIT_FAILURE_DELAY 1000000000ULL
#define NS_PER_SECOND         1000000000ULL

// pm's Atmosphere extension that hands back a process's handle.
#define CMD_ATMOSPHERE_GET_PROCESS_INFO 65000

#define SIZE_INNER_HEAP 0x4000

u32 __nx_applet_type               = AppletType_None;
u32 __nx_fs_num_sessions           = 1;
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd        = false;

void __libnx_initheap(void)
{
    static char innerHeap[SIZE_INNER_HEAP];

    extern char *fake_heap_start;
    extern char *fake_heap_end;

    fake_heap_start = innerHeap;
    fake_heap_end   = innerHeap + sizeof(innerHeap);
}

// A fatal error over a backup that can wait for the next game would be far worse than skipping it.
static void exit_if_failed(Result result)
{
    if (R_FAILED(result)) { svcExitProcess(); }
}

void __appInit(void)
{
    exit_if_failed(smInitialize());

    if (R_SUCCEEDED(setsysInitialize()))
    {
        SetSysFirmwareVersion firmware;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware)))
        {
            hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
        }
        setsysExit();
    }

    exit_if_failed(fsInitialize());
    exit_if_failed(pmdmntInitialize());
    exit_if_failed(pmshellInitialize());
}

void __appExit(void)
{
    pmshellExit();
    pmdmntExit();
    fsExit();
    smExit();
}

static bool sd_entry_exists(FsFileSystem *sdCard, const char *path)
{
    FsDirEntryType type;
    return R_SUCCEEDED(fsFsGetEntryType(sdCard, path, &type));
}

/// Whether the worker has anything it could do. Signed out, never set up and disabled after crashes all mean no.
static bool worker_allowed(void)
{
    FsFileSystem sdCard;
    if (R_FAILED(fsOpenSdCardFileSystem(&sdCard))) { return false; }

    // An interrupted config write can leave only the .tmp behind. The worker reads that one too.
    const bool configured = sd_entry_exists(&sdCard, PATH_CONFIG) || sd_entry_exists(&sdCard, PATH_CONFIG_TEMP);

    // JKSV re-enables the worker by putting boot2.flag back. Until the next boot the .crashed one is still there.
    const bool disabled = sd_entry_exists(&sdCard, PATH_DISABLED_FLAG) && !sd_entry_exists(&sdCard, PATH_BOOT_FLAG);

    const bool allowed = configured && !sd_entry_exists(&sdCard, PATH_LOGOUT_MARKER) && !disabled;
    fsFsClose(&sdCard);
    return allowed;
}

/// Leaves a note for JKSV to show, since the monitor itself has no way to tell anyone. The worker removes it.
static void report_launch_failure(void)
{
    FsFileSystem sdCard;
    if (R_FAILED(fsOpenSdCardFileSystem(&sdCard))) { return; }
    fsFsCreateFile(&sdCard, PATH_LAUNCH_FAILED, 0, 0);
    fsFsClose(&sdCard);
}

static bool ends_with_json(const char *name)
{
    const size_t length = strlen(name);
    return length >= 5 && strcmp(name + length - 5, ".json") == 0;
}

/// Seconds until the worker wants to try again, or zero if it doesn't. The worker writes this as a plain number.
static u64 read_retry_seconds(void)
{
    FsFileSystem sdCard;
    if (R_FAILED(fsOpenSdCardFileSystem(&sdCard))) { return 0; }

    u64 seconds = 0;
    FsFile file;
    if (R_SUCCEEDED(fsFsOpenFile(&sdCard, PATH_RETRY_AFTER, FsOpenMode_Read, &file)))
    {
        char text[24];
        u64 read = 0;
        if (R_SUCCEEDED(fsFileRead(&file, 0, text, sizeof(text) - 1, FsReadOption_None, &read)))
        {
            for (u64 i = 0; i < read && text[i] >= '0' && text[i] <= '9'; i++) { seconds = seconds * 10 + (text[i] - '0'); }
        }
        fsFileClose(&file);
    }

    fsFsClose(&sdCard);
    return seconds;
}

/// Anything left from before a reboot: queued backups or a retry that never happened.
static bool has_unfinished_work(void)
{
    if (read_retry_seconds() != 0) { return true; }

    FsFileSystem sdCard;
    if (R_FAILED(fsOpenSdCardFileSystem(&sdCard))) { return false; }

    // Only metadata means a queued backup. A leftover temporary file or ZIP on its own would start the worker for
    // nothing on every boot.
    bool unfinished = false;
    FsDir queue;
    if (R_SUCCEEDED(fsFsOpenDirectory(&sdCard, PATH_QUEUE, FsDirOpenMode_ReadFiles, &queue)))
    {
        FsDirectoryEntry entry;
        s64 read = 0;
        while (!unfinished && R_SUCCEEDED(fsDirRead(&queue, &read, 1, &entry)) && read > 0)
        {
            unfinished = ends_with_json(entry.name);
        }
        fsDirClose(&queue);
    }

    fsFsClose(&sdCard);
    return unfinished;
}

static bool application_running(void)
{
    u64 processID = 0;
    return R_SUCCEEDED(pmdmntGetApplicationProcessId(&processID)) && processID != 0;
}

/// Observers each get their own queue, so this can't take events away from the system the way reading pm:shell's
/// event would. pm's application launch hook isn't an option either: dmnt's cheat manager always holds the only one.
static bool open_process_events(PglEventObserver *observerOut, Event *eventOut)
{
    if (R_FAILED(pglInitialize())) { return false; }

    bool opened = false;
    if (R_SUCCEEDED(pglGetEventObserver(observerOut)))
    {
        opened = R_SUCCEEDED(pglEventObserverGetProcessEvent(observerOut, eventOut));
        if (!opened) { pglEventObserverClose(observerOut); }
    }

    // Only the observer is needed from here on, and pgl has a limited number of sessions to go around.
    pglExit();
    return opened;
}

/// Empties the observer's queue, which would otherwise fill up and start dropping events.
/// @return Whether anything exited.
static bool drain_process_events(PglEventObserver *observer)
{
    bool exited = false;

    PmProcessEventInfo info;
    while (R_SUCCEEDED(pglEventObserverGetProcessEventInfo(observer, &info)) && info.event != PmProcessEvent_None)
    {
        if (info.event == PmProcessEvent_Exit) { exited = true; }
    }
    return exited;
}

/// Starts the worker and gets a handle that's signaled when it finishes.
static bool launch_worker(Handle *handleOut)
{
    // None makes loader use the copy in atmosphere/contents, since the worker isn't installed on the system.
    const NcmProgramLocation location = {.program_id = PROGRAM_ID_WORKER, .storageID = NcmStorageId_None};

    *handleOut   = INVALID_HANDLE;
    u64 workerID = 0;
    if (R_FAILED(pmshellLaunchProgram(0, &location, &workerID))) { return false; }

    struct
    {
            NcmProgramLocation location;
            u64 overrideStatus[2];
    } out;

    // If this fails, the worker has most likely already finished. Worst case, a retry waits for the next game.
    Handle handle = INVALID_HANDLE;
    if (R_SUCCEEDED(serviceDispatchInOut(pmdmntGetServiceSession(),
                                         CMD_ATMOSPHERE_GET_PROCESS_INFO,
                                         workerID,
                                         out,
                                         .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                                         .out_handles      = &handle)))
    {
        *handleOut = handle;
    }
    return true;
}

/// The kernel can't free a process's memory until every handle to it is closed, so this has to be checked and the
/// handle closed as soon as the worker is gone.
static bool worker_has_exited(Handle worker)
{
    svcResetSignal(worker);

    s64 state = 0;
    return R_FAILED(svcGetProcessInfo(&state, worker, ProcessInfoType_ProcessState)) || state == ProcessState_Exited;
}

static u64 now_ns(void) { return armTicksToNs(armGetSystemTick()); }

int main(void)
{
    svcSleepThread(NS_STARTUP_GRACE);

    PglEventObserver observer;
    Event processEvent;
    const bool eventsReady = open_process_events(&observer, &processEvent);
    smExit();

    Handle worker        = INVALID_HANDLE;
    u64 retryDeadline    = 0;
    u64 launchRetry      = NS_LAUNCH_RETRY;
    u32 launchFailures   = 0;
    bool workerNeeded    = has_unfinished_work();
    bool applicationWas  = application_running();

    while (true)
    {
        if (workerNeeded && worker == INVALID_HANDLE)
        {
            workerNeeded = false;
            if (worker_allowed())
            {
                if (launch_worker(&worker))
                {
                    launchFailures = 0;
                    launchRetry    = NS_LAUNCH_RETRY;
                }
                else
                {
                    if (++launchFailures == LAUNCH_FAILURES_NOTICE) { report_launch_failure(); }
                    retryDeadline = now_ns() + launchRetry;
                    launchRetry   = launchRetry * 2 > NS_LAUNCH_RETRY_MAX ? NS_LAUNCH_RETRY_MAX : launchRetry * 2;
                }
            }
        }

        // With pgl, this only ever wakes up by itself while the worker has a retry scheduled.
        u64 timeout = UINT64_MAX;
        if (retryDeadline != 0 && worker == INVALID_HANDLE)
        {
            const u64 now = now_ns();
            timeout       = retryDeadline > now ? retryDeadline - now : 0;
        }
        if (!eventsReady && NS_FALLBACK_POLL < timeout) { timeout = NS_FALLBACK_POLL; }

        Handle handles[2];
        s32 handleCount = 0;
        s32 eventIndex  = -1;
        s32 workerIndex = -1;
        if (eventsReady)
        {
            eventIndex            = handleCount;
            handles[handleCount++] = processEvent.revent;
        }
        if (worker != INVALID_HANDLE)
        {
            workerIndex           = handleCount;
            handles[handleCount++] = worker;
        }

        s32 signaled  = -1;
        bool timedOut = false;
        if (handleCount == 0)
        {
            svcSleepThread(timeout);
            timedOut = true;
        }
        else
        {
            const Result result = svcWaitSynchronization(&signaled, handles, handleCount, timeout);
            timedOut            = R_VALUE(result) == KERNELRESULT(TimedOut);
            if (R_FAILED(result) && !timedOut)
            {
                svcSleepThread(NS_WAIT_FAILURE_DELAY);
                continue;
            }
        }

        if (timedOut)
        {
            if (retryDeadline != 0 && now_ns() >= retryDeadline)
            {
                retryDeadline = 0;
                // A game being open means it'll close eventually, and that starts the worker anyway.
                if (!application_running()) { workerNeeded = true; }
            }

            if (!eventsReady)
            {
                const bool applicationIs = application_running();
                if (applicationWas && !applicationIs) { workerNeeded = true; }
                applicationWas = applicationIs;
            }
            continue;
        }

        if (signaled == eventIndex)
        {
            eventClear(&processEvent);

            // Things like the keyboard close while games are open. Which game closed is up to the worker to find out,
            // since the process is already gone by now.
            if (drain_process_events(&observer) && !application_running()) { workerNeeded = true; }
        }
        else if (signaled == workerIndex && worker_has_exited(worker))
        {
            svcCloseHandle(worker);
            worker = INVALID_HANDLE;

            const u64 retrySeconds = read_retry_seconds();
            retryDeadline          = retrySeconds != 0 ? now_ns() + retrySeconds * NS_PER_SECOND : 0;
        }
    }

    return 0;
}
