#include "remote/remote.hpp"

#include "StateManager.hpp"
#include "appstates/TaskState.hpp"
#include "data/data.hpp"
#include "error.hpp"
#include "fslib.hpp"
#include "input.hpp"
#include "json.hpp"
#include "logging/logger.hpp"
#include "remote/BaiduNetdisk.hpp"
#include "shared/BaiduConfigStore.hpp"
#include "shared/BaiduNaming.hpp"
#include "strings/strings.hpp"
#include "ui/PopMessageManager.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    // Keep these in sync with sysmodule/include/common.hpp.
    constexpr std::string_view PATH_WORKER_STATE    = "sdmc:/config/JKSV/background/state";
    constexpr std::string_view PATH_PLAY_LOG        = "sdmc:/config/JKSV/background/state/play_log.position";
    constexpr std::string_view PATH_LAUNCH_FAILED   = "sdmc:/config/JKSV/background/monitor_launch_failed";
    constexpr std::string_view PATH_JKSV_RUNNING    = "sdmc:/config/JKSV/background/.jksv_running";
    constexpr std::string_view PATH_BOOT_FLAG       = "sdmc:/atmosphere/contents/420000004A4B5344/flags/boot2.flag";
    constexpr std::string_view PATH_BOOT_FLAG_CRASH = "sdmc:/atmosphere/contents/420000004A4B5344/flags/boot2.flag.crashed";

    /// @brief How many QR codes in a row to show before giving up. Each one lasts several minutes.
    constexpr int MAX_QR_CODES = 5;

    /// @brief Baidu Netdisk is the only remote storage.
    std::unique_ptr<remote::Storage> s_storage{};
    std::mutex s_storageMutex{};

    /// @brief Set while something is connecting, signing in or signing out. Only one of those can run at a time.
    std::atomic_bool s_busy{};
    std::atomic_bool s_connectionFailed{};

    json_object *get_json_object(json_object *parent, std::string_view key)
    {
        return parent ? json_object_object_get(parent, key.data()) : nullptr;
    }

    std::string get_json_string(json_object *parent, std::string_view key)
    {
        json_object *value = get_json_object(parent, key);
        return value ? json_object_get_string(value) : "";
    }

    bool get_json_bool(json_object *parent, std::string_view key, bool fallback = false)
    {
        json_object *value = get_json_object(parent, key);
        return value ? json_object_get_boolean(value) : fallback;
    }

    const char *baidu_text(int index, const char *fallback)
    {
        const char *text = strings::get_by_name(strings::names::BAIDU_NETDISK, index);
        return text ? text : fallback;
    }

    void pop(const char *message) { ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS, message); }

    void pop_busy() { pop(baidu_text(24, "Connecting to Baidu Netdisk. Please try again in a moment.")); }

    // The caller has to hold baidu_config::ScopedLock.
    bool save_baidu_config(json_object *config)
    {
        return baidu_config::write(config, remote::PATH_BAIDU_NETDISK_CONFIG.data());
    }

    bool baidu_is_explicitly_signed_out()
    {
        if (fslib::file_exists(remote::PATH_BAIDU_NETDISK_LOGOUT)) { return true; }

        json::Object config = json::new_object(baidu_config::read, remote::PATH_BAIDU_NETDISK_CONFIG.data());
        return config && get_json_bool(config.get(), "signed_out");
    }

    void set_storage(std::unique_ptr<remote::Storage> storage)
    {
        std::unique_ptr<remote::Storage> previous{};
        {
            std::lock_guard storageGuard{s_storageMutex};
            previous = std::exchange(s_storage, std::move(storage));
        }
    }

    /// @brief What the worker remembers is about the account it uploaded to before. After a new sign in, the next game
    /// that closes should be backed up whether or not its save changed, and nothing played while signed out should be
    /// queued all at once.
    void reset_worker_state()
    {
        fslib::Directory stateDirectory{PATH_WORKER_STATE, false};
        for (int64_t i = 0; stateDirectory.is_open() && i < stateDirectory.get_count(); i++)
        {
            const fslib::DirectoryEntry &entry = stateDirectory[static_cast<int>(i)];
            const std::string_view name{entry.get_filename()};
            if (entry.is_directory() || !name.ends_with(".state")) { continue; }
            fslib::delete_file(fslib::Path{PATH_WORKER_STATE} / entry.get_filename());
        }
        if (fslib::file_exists(PATH_PLAY_LOG)) { fslib::delete_file(PATH_PLAY_LOG); }
    }

    // clang-format off
    struct BaiduStruct : sys::Task::DataStruct
    {
        std::unique_ptr<remote::BaiduNetdisk> baidu{};
    };
    // clang-format on

    enum class SignInResult
    {
        Authorized,
        Cancelled,
        Failed
    };

    /// @brief Waits about the given number of seconds. Returns false as soon as [B] is pressed.
    bool wait_unless_cancelled(int seconds)
    {
        static constexpr std::chrono::milliseconds STEP{50};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (input::button_pressed(HidNpadButton_B) || input::button_held(HidNpadButton_B)) { return false; }
            std::this_thread::sleep_for(STEP);
        }
        return true;
    }

    /// @brief Shows QR codes on the task's screen until one is scanned, B is pressed, or Baidu says no.
    SignInResult run_sign_in(sys::Task *task, remote::BaiduNetdisk *baidu)
    {
        for (int code = 0; code < MAX_QR_CODES; code++)
        {
            std::string message{}, deviceCode{};
            std::vector<uint8_t> qrImage{};
            std::time_t expiration{};
            int pollingInterval{};
            if (!baidu->get_sign_in_data(message, deviceCode, expiration, pollingInterval, qrImage))
            {
                logger::log("Error signing in to Baidu Netdisk: Getting device authorization data failed.");
                pop(baidu->connection_failed()
                        ? baidu_text(23, "Unable to reach Baidu Netdisk. Press [A] on the account row in settings to retry.")
                        : baidu_text(14, "Unable to start Baidu Netdisk sign in."));
                return SignInResult::Failed;
            }

            task->set_status(message);
            if (!qrImage.empty()) { task->set_image(std::move(qrImage)); }

            while (std::time(nullptr) < expiration)
            {
                if (!wait_unless_cancelled(pollingInterval)) { return SignInResult::Cancelled; }

                const remote::BaiduNetdisk::SignInPoll poll = baidu->poll_sign_in(deviceCode);
                if (poll == remote::BaiduNetdisk::SignInPoll::Authorized) { return SignInResult::Authorized; }
                if (poll == remote::BaiduNetdisk::SignInPoll::Failed)
                {
                    pop(baidu_text(2, "Baidu Netdisk sign in failed!"));
                    return SignInResult::Failed;
                }
                if (poll == remote::BaiduNetdisk::SignInPoll::Expired) { break; }
                if (poll == remote::BaiduNetdisk::SignInPoll::SlowDown) { pollingInterval += 5; }
            }
            // The code expired without being scanned. Show a fresh one instead of making the user start over.
        }

        pop(baidu_text(2, "Baidu Netdisk sign in failed!"));
        return SignInResult::Failed;
    }

    /// @brief Finishes a sign in that went through: the new login replaces whatever was in use before.
    void complete_sign_in(std::unique_ptr<remote::BaiduNetdisk> baidu)
    {
        reset_worker_state();
        const bool initialized = baidu->is_initialized();
        s_connectionFailed     = baidu->connection_failed();
        set_storage(std::move(baidu));
        pop(initialized ? baidu_text(1, "Successfully signed in to Baidu Netdisk!")
                        : baidu_text(22, "Authorization saved; cloud folders unavailable. Check connection and reopen JKSV."));
    }

    void sign_in_task(sys::threadpool::JobData taskData)
    {
        auto castData  = std::static_pointer_cast<BaiduStruct>(taskData);
        sys::Task *task = castData->task;

        if (run_sign_in(task, castData->baidu.get()) == SignInResult::Authorized)
        {
            complete_sign_in(std::move(castData->baidu));
        }
        s_busy = false;
        task->complete();
    }

    /// @brief Connects with the saved login and shows how that went. Has to be called with s_busy set.
    /// @param task Shows the QR code if one is needed. Without one, a sign-in screen is pushed instead.
    void connect(sys::Task *task)
    {
        auto baidu = std::make_unique<remote::BaiduNetdisk>();
        if (baidu->sign_in_required())
        {
            if (task)
            {
                if (run_sign_in(task, baidu.get()) == SignInResult::Authorized) { complete_sign_in(std::move(baidu)); }
                s_busy = false;
                return;
            }

            // s_busy stays set until the sign-in screen is done with it.
            auto baiduStruct   = std::make_shared<BaiduStruct>();
            baiduStruct->baidu = std::move(baidu);
            TaskState::create_push_fade(sign_in_task, baiduStruct);
            return;
        }

        const bool initialized = baidu->is_initialized();
        s_connectionFailed     = baidu->connection_failed();
        set_storage(std::move(baidu));
        pop(initialized          ? baidu_text(1, "Successfully signed in to Baidu Netdisk!")
            : s_connectionFailed ? baidu_text(23, "Unable to reach Baidu Netdisk. Press [A] on the account row in settings to retry.")
                                 : baidu_text(2, "Baidu Netdisk sign in failed!"));
        s_busy = false;
    }

    void connect_task(sys::threadpool::JobData taskData)
    {
        sys::Task *task = std::static_pointer_cast<sys::Task::DataStruct>(taskData)->task;
        task->set_status(baidu_text(35, "Connecting to Baidu Netdisk..."));
        connect(task);
        task->complete();
    }
} // namespace

bool remote::has_internet_connection() noexcept
{
    NifmInternetConnectionType type{};
    uint32_t strength{};
    NifmInternetConnectionStatus status{};
    const bool getError = error::libnx(nifmGetInternetConnectionStatus(&type, &strength, &status));
    if (getError || status != NifmInternetConnectionStatus_Connected) { return false; }
    return true;
}

void remote::initialize(sys::threadpool::JobData jobData)
{
    const bool baiduExists = fslib::file_exists(remote::PATH_BAIDU_NETDISK_CONFIG);
    if (!baiduExists || baidu_is_explicitly_signed_out()) { return; }
    if (s_busy.exchange(true)) { return; }

    if (!remote::has_internet_connection())
    {
        s_connectionFailed = true;
        s_busy             = false;
        pop(strings::get_by_name(strings::names::REMOTE_POPS, 0));
        return;
    }

    connect(nullptr);
}

remote::Storage *remote::get_remote_storage() noexcept
{
    std::lock_guard storageGuard{s_storageMutex};
    if (!s_storage || !s_storage->is_initialized()) { return nullptr; }
    return s_storage.get();
}

bool remote::is_busy() noexcept { return s_busy; }

bool remote::connection_failed() noexcept { return s_connectionFailed; }

remote::BaiduAccountStatus remote::get_baidu_account_status()
{
    remote::BaiduAccountStatus status{};
    json::Object config = json::new_object(baidu_config::read, remote::PATH_BAIDU_NETDISK_CONFIG.data());
    if (!config) { return status; }

    const std::string appKey = [&]()
    {
        std::string value = get_json_string(config.get(), "app_key");
        return value.empty() ? get_json_string(config.get(), "client_id") : value;
    }();
    const std::string secretKey = [&]()
    {
        std::string value = get_json_string(config.get(), "secret_key");
        return value.empty() ? get_json_string(config.get(), "client_secret") : value;
    }();

    status.configured = !appKey.empty() && !secretKey.empty();
    const bool signedOut = get_json_bool(config.get(), "signed_out") ||
                           fslib::file_exists(remote::PATH_BAIDU_NETDISK_LOGOUT);
    const bool hasToken = !get_json_string(config.get(), "refresh_token").empty() ||
                          !get_json_string(config.get(), "access_token").empty();
    status.signedIn = status.configured && !signedOut && hasToken;
    if (status.signedIn) { status.displayName = get_json_string(config.get(), "account_name"); }
    return status;
}

bool remote::begin_baidu_netdisk_sign_in(bool forceReauthorize)
{
    if (s_busy)
    {
        pop_busy();
        return true;
    }

    const remote::BaiduAccountStatus status = remote::get_baidu_account_status();
    if (!status.configured || (!forceReauthorize && status.signedIn)) { return false; }
    if (!remote::has_internet_connection())
    {
        pop(strings::get_by_name(strings::names::REMOTE_POPS, 0));
        return true;
    }
    if (s_busy.exchange(true))
    {
        pop_busy();
        return true;
    }

    // Nothing is checked with Baidu before the new QR code is up. The current login keeps working until the new one is
    // saved over it, so cancelling leaves everything as it was.
    auto baiduStruct   = std::make_shared<BaiduStruct>();
    baiduStruct->baidu = std::make_unique<remote::BaiduNetdisk>(true, false);
    TaskState::create_push_fade(sign_in_task, baiduStruct);
    return true;
}

void remote::retry_baidu_netdisk_connection()
{
    if (!remote::has_internet_connection())
    {
        pop(strings::get_by_name(strings::names::REMOTE_POPS, 0));
        return;
    }
    if (s_busy.exchange(true))
    {
        pop_busy();
        return;
    }
    TaskState::create_push_fade(connect_task, std::make_shared<sys::Task::DataStruct>());
}

bool remote::sign_out_baidu_netdisk()
{
    if (s_busy.exchange(true))
    {
        pop_busy();
        return false;
    }

    // Waits out any refresh in progress, so it can't write the old tokens back after this clears them.
    bool signedOut{};
    {
        baidu_config::ScopedLock lock{remote::PATH_BAIDU_NETDISK_CONFIG.data()};
        json::Object config = json::new_object(baidu_config::read, remote::PATH_BAIDU_NETDISK_CONFIG.data());

        bool markerCreated{};
        if (lock.held() && config)
        {
            fslib::File marker{remote::PATH_BAIDU_NETDISK_LOGOUT, FsOpenMode_Create | FsOpenMode_Write, 0};
            markerCreated = marker && marker.flush();
        }

        if (markerCreated)
        {
            set_storage(nullptr);
            s_connectionFailed = false;

            json_object_object_add(config.get(), "access_token", json_object_new_string(""));
            json_object_object_add(config.get(), "refresh_token", json_object_new_string(""));
            json_object_object_add(config.get(), "expires_at", json_object_new_int64(0));
            json_object_object_add(config.get(), "account_name", json_object_new_string(""));
            json_object_object_add(config.get(), "signed_out", json_object_new_boolean(true));
            signedOut = save_baidu_config(config.get());
        }
    }

    s_busy = false;
    return signedOut;
}

bool remote::enter_title_directory(remote::Storage *storage, const data::TitleInfo *titleInfo)
{
    if (!storage || !titleInfo) { return false; }
    storage->return_to_root();

    const std::string folder = baidu_naming::title_directory(titleInfo->get_title());
    if (!storage->directory_exists(folder))
    {
        // Older versions used JKSV's path safe title, which turns names that aren't plain ASCII into the title ID. The
        // worker never did, so the two ended up in different folders. Moving the old one keeps its backups in view.
        const std::string_view legacy  = titleInfo->get_path_safe_title();
        remote::Item *legacyDirectory  = legacy != folder ? storage->get_directory_by_name(legacy) : nullptr;
        const bool renamed             = legacyDirectory && storage->rename_item(legacyDirectory, folder);
        if (renamed) { logger::log("Moved cloud folder \"%s\" to \"%s\".", std::string(legacy).c_str(), folder.c_str()); }
        if (!renamed && !storage->create_directory(folder)) { return false; }
    }

    const remote::Item *directory = storage->get_directory_by_name(folder);
    if (!directory) { return false; }
    storage->change_directory(directory);
    return true;
}

bool remote::background_sync_disabled()
{
    return fslib::file_exists(PATH_BOOT_FLAG_CRASH) && !fslib::file_exists(PATH_BOOT_FLAG);
}

bool remote::enable_background_sync()
{
    // The worker sees boot2.flag back next to the .crashed one and clears its crash count. Until the console reboots,
    // the monitor is still in the state it was booted in.
    return fslib::rename_file(PATH_BOOT_FLAG_CRASH, PATH_BOOT_FLAG);
}

bool remote::background_launch_failed() { return fslib::file_exists(PATH_LAUNCH_FAILED); }

void remote::mark_jksv_running(bool running)
{
    if (!running)
    {
        if (fslib::file_exists(PATH_JKSV_RUNNING)) { fslib::delete_file(PATH_JKSV_RUNNING); }
        return;
    }

    uint64_t processId{};
    if (R_FAILED(svcGetProcessId(&processId, CUR_PROCESS_HANDLE))) { return; }

    const int64_t uptime    = static_cast<int64_t>(armTicksToNs(armGetSystemTick()) / 1000000000ULL);
    const int64_t bootEpoch = static_cast<int64_t>(std::time(nullptr)) - uptime;
    fslib::File marker{PATH_JKSV_RUNNING, FsOpenMode_Create | FsOpenMode_Write, 0};
    if (marker) { marker.writef("%llu %lld\n", static_cast<unsigned long long>(processId), static_cast<long long>(bootEpoch)); }
}
