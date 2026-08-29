#include "remote/remote.hpp"

#include "StateManager.hpp"
#include "appstates/TaskState.hpp"
#include "error.hpp"
#include "fslib.hpp"
#include "input.hpp"
#include "json.hpp"
#include "logging/logger.hpp"
#include "remote/BaiduNetdisk.hpp"
#include "remote/GoogleDrive.hpp"
#include "remote/WebDav.hpp"
#include "strings/strings.hpp"
#include "ui/PopMessageManager.hpp"

#include <chrono>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace
{
    /// @brief This is just the string for finding and creating the JKSV dir.
    constexpr const char *STRING_JKSV_DIR = "JKSV";

    /// @brief This is the single (for now) instance of a storage class.
    std::unique_ptr<remote::Storage> s_storage{};

    enum class ActiveProvider
    {
        None,
        GoogleDrive,
        BaiduNetdisk,
        WebDav
    };

    ActiveProvider s_activeProvider{ActiveProvider::None};

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

    bool save_baidu_config(json_object *config)
    {
        const char *jsonString = json_object_to_json_string_ext(config, JSON_C_TO_STRING_PRETTY);
        if (!jsonString) { return false; }

        const size_t stringSize = std::char_traits<char>::length(jsonString);
        if (stringSize > static_cast<size_t>(std::numeric_limits<int64_t>::max())) { return false; }

        fslib::File configFile{remote::PATH_BAIDU_NETDISK_CONFIG,
                               FsOpenMode_Create | FsOpenMode_Write,
                               static_cast<int64_t>(stringSize)};
        return configFile && configFile.write(jsonString, stringSize) == static_cast<ssize_t>(stringSize) &&
               configFile.flush();
    }

    bool baidu_is_explicitly_signed_out()
    {
        if (fslib::file_exists(remote::PATH_BAIDU_NETDISK_LOGOUT)) { return true; }

        json::Object config = json::new_object(json_object_from_file, remote::PATH_BAIDU_NETDISK_CONFIG.data());
        return config && get_json_bool(config.get(), "signed_out");
    }

    // clang-format off
    struct DriveStruct : sys::Task::DataStruct
    {
        remote::GoogleDrive *drive{};
    };

    struct BaiduStruct : sys::Task::DataStruct
    {
        remote::BaiduNetdisk *baidu{};
    };
    // clang-format on
} // namespace

static void initialize_google_drive();
static bool initialize_baidu_netdisk(bool authorizeSignedOut = false);
static void initialize_webdav();

// Declarations here. Definitions at bottom.
/// @brief This is the thread function that handles logging into Google.
static void drive_sign_in(sys::threadpool::JobData taskData);
static void baidu_sign_in(sys::threadpool::JobData taskData);

/// @brief This creates (if needed) the JKSV folder for Google Drive and sets it as the root.
/// @param drive Pointer to the drive instance.
static void drive_set_jksv_root(remote::GoogleDrive *drive);

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
    const bool driveExists  = fslib::file_exists(remote::PATH_GOOGLE_DRIVE_CONFIG);
    const bool baiduExists  = fslib::file_exists(remote::PATH_BAIDU_NETDISK_CONFIG);
    const bool baiduSignedOut = baiduExists && baidu_is_explicitly_signed_out();
    const bool webdavExists = fslib::file_exists(remote::PATH_WEBDAV_CONFIG);
    if ((driveExists || (baiduExists && !baiduSignedOut) || webdavExists) && !remote::has_internet_connection())
    {
        const char *popNoInternet = strings::get_by_name(strings::names::REMOTE_POPS, 0);
        ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS, popNoInternet);
        return;
    }

    if (driveExists) { initialize_google_drive(); }
    else if (baiduExists && !baiduSignedOut) { initialize_baidu_netdisk(); }
    else if (webdavExists) { initialize_webdav(); }
}

void initialize_google_drive()
{
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;

    s_activeProvider           = ActiveProvider::GoogleDrive;
    s_storage                  = std::make_unique<remote::GoogleDrive>();
    remote::GoogleDrive *drive = static_cast<remote::GoogleDrive *>(s_storage.get());
    if (drive->sign_in_required())
    {
        auto driveStruct   = std::make_shared<DriveStruct>();
        driveStruct->drive = drive;

        // To do: StateManager isn't thread safe. This might/probably will cause data race randomly.
        TaskState::create_push_fade(drive_sign_in, driveStruct);
        return;
    }

    // To do: Handle this better. Maybe retry somehow?
    if (!drive->is_initialized()) { return; }

    drive_set_jksv_root(drive);
    const char *popDriveSuccess = strings::get_by_name(strings::names::GOOGLE_DRIVE, 1);
    ui::PopMessageManager::push_message(popTicks, popDriveSuccess);
}

void initialize_webdav()
{
    s_activeProvider   = ActiveProvider::WebDav;
    s_storage          = std::make_unique<remote::WebDav>();
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    if (s_storage->is_initialized())
    {
        const char *popDavSuccess = strings::get_by_name(strings::names::WEBDAV, 0);
        ui::PopMessageManager::push_message(popTicks, popDavSuccess);
    }
    else
    {
        const char *popDavFailed = strings::get_by_name(strings::names::WEBDAV, 1);
        ui::PopMessageManager::push_message(popTicks, popDavFailed);
    }
}

bool initialize_baidu_netdisk(bool authorizeSignedOut)
{
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;

    s_activeProvider            = ActiveProvider::BaiduNetdisk;
    s_storage                   = std::make_unique<remote::BaiduNetdisk>(authorizeSignedOut);
    remote::BaiduNetdisk *baidu = static_cast<remote::BaiduNetdisk *>(s_storage.get());
    if (baidu->sign_in_required())
    {
        auto baiduStruct   = std::make_shared<BaiduStruct>();
        baiduStruct->baidu = baidu;
        TaskState::create_push_fade(baidu_sign_in, baiduStruct);
        return true;
    }

    const bool initialized  = baidu->is_initialized();
    const char *message     = strings::get_by_name(strings::names::BAIDU_NETDISK, initialized ? 1 : 2);
    const char *fallback    = initialized ? "Successfully signed in to Baidu Netdisk!"
                                          : "Baidu Netdisk sign in failed!";
    ui::PopMessageManager::push_message(popTicks, message ? message : fallback);
    return initialized;
}

remote::Storage *remote::get_remote_storage() noexcept
{
    if (!s_storage || !s_storage->is_initialized()) { return nullptr; }
    return s_storage.get();
}

remote::BaiduAccountStatus remote::get_baidu_account_status()
{
    remote::BaiduAccountStatus status{};
    json::Object config = json::new_object(json_object_from_file, remote::PATH_BAIDU_NETDISK_CONFIG.data());
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

bool remote::begin_baidu_netdisk_sign_in()
{
    const remote::BaiduAccountStatus status = remote::get_baidu_account_status();
    if (!status.configured || status.signedIn || !remote::has_internet_connection()) { return false; }
    return initialize_baidu_netdisk(true);
}

bool remote::sign_out_baidu_netdisk()
{
    json::Object config = json::new_object(json_object_from_file, remote::PATH_BAIDU_NETDISK_CONFIG.data());
    if (!config) { return false; }

    bool markerCreated{};
    {
        fslib::File marker{remote::PATH_BAIDU_NETDISK_LOGOUT, FsOpenMode_Create | FsOpenMode_Write, 0};
        markerCreated = marker && marker.flush();
    }
    if (!markerCreated) { return false; }

    if (s_activeProvider == ActiveProvider::BaiduNetdisk)
    {
        s_storage.reset();
        s_activeProvider = ActiveProvider::None;
    }

    json_object_object_add(config.get(), "access_token", json_object_new_string(""));
    json_object_object_add(config.get(), "refresh_token", json_object_new_string(""));
    json_object_object_add(config.get(), "expires_at", json_object_new_int64(0));
    json_object_object_add(config.get(), "account_name", json_object_new_string(""));
    json_object_object_add(config.get(), "signed_out", json_object_new_boolean(true));
    return save_baidu_config(config.get());
}

static void drive_sign_in(sys::threadpool::JobData taskData)
{
    static constexpr const char *STRING_ERROR_SIGNING_IN = "Error signing into Google Drive: %s";

    auto castData = std::static_pointer_cast<DriveStruct>(taskData);

    sys::Task *task            = castData->task;
    remote::GoogleDrive *drive = castData->drive;

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    std::string message{}, deviceCode{};
    std::time_t expiration{};
    int pollingInterval{};
    if (!drive->get_sign_in_data(message, deviceCode, expiration, pollingInterval))
    {
        logger::log(STRING_ERROR_SIGNING_IN, "Getting sign in data failed!");
        TASK_FINISH_RETURN(task);
    }

    task->set_status(message);

    while (std::time(NULL) < expiration && !drive->poll_sign_in(deviceCode))
    {
        const bool bPressed = input::button_pressed(HidNpadButton_B);
        const bool bHeld    = input::button_held(HidNpadButton_B);
        if (bPressed || bHeld) { break; }

        std::this_thread::sleep_for(std::chrono::seconds(pollingInterval));
    }

    if (drive->is_initialized())
    {
        drive_set_jksv_root(drive);
        const char *popDriveSuccess = strings::get_by_name(strings::names::GOOGLE_DRIVE, 1);
        ui::PopMessageManager::push_message(popTicks, popDriveSuccess);
    }
    else
    {
        const char *popDriveFailed = strings::get_by_name(strings::names::GOOGLE_DRIVE, 2);
        ui::PopMessageManager::push_message(popTicks, popDriveFailed);
    }

    task->complete();
}

static void baidu_sign_in(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BaiduStruct>(taskData);

    sys::Task *task             = castData->task;
    remote::BaiduNetdisk *baidu = castData->baidu;
    const int popTicks          = ui::PopMessageManager::DEFAULT_TICKS;
    std::string message{}, deviceCode{};
    std::vector<uint8_t> qrImage{};
    std::time_t expiration{};
    int pollingInterval{};
    if (!baidu->get_sign_in_data(message, deviceCode, expiration, pollingInterval, qrImage))
    {
        logger::log("Error signing in to Baidu Netdisk: Getting device authorization data failed.");
        TASK_FINISH_RETURN(task);
    }

    task->set_status(message);
    if (!qrImage.empty()) { task->set_image(std::move(qrImage)); }
    while (std::time(nullptr) < expiration && !baidu->poll_sign_in(deviceCode))
    {
        const bool bPressed = input::button_pressed(HidNpadButton_B);
        const bool bHeld    = input::button_held(HidNpadButton_B);
        if (bPressed || bHeld) { break; }
        std::this_thread::sleep_for(std::chrono::seconds(pollingInterval));
    }

    const bool initialized  = baidu->is_initialized();
    const char *popMessage = strings::get_by_name(strings::names::BAIDU_NETDISK, initialized ? 1 : 2);
    const char *fallback   = initialized ? "Successfully signed in to Baidu Netdisk!"
                                         : "Baidu Netdisk sign in failed!";
    ui::PopMessageManager::push_message(popTicks, popMessage ? popMessage : fallback);
    task->complete();
}

static void drive_set_jksv_root(remote::GoogleDrive *drive)
{
    const bool jksvExists  = drive->directory_exists(STRING_JKSV_DIR);
    const bool jksvCreated = !jksvExists && drive->create_directory(STRING_JKSV_DIR);
    if (!jksvExists && !jksvCreated) { return; }

    const remote::Item *jksvDir = drive->get_directory_by_name(STRING_JKSV_DIR);
    if (!jksvDir) { return; }

    drive->set_root_directory(jksvDir);
    drive->change_directory(jksvDir);
}
