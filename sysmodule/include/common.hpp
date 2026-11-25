#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <switch.h>

namespace background
{
    inline constexpr const char *ROOT_DIRECTORY         = "sdmc:/config/JKSV/background";
    inline constexpr const char *QUEUE_DIRECTORY        = "sdmc:/config/JKSV/background/queue";
    inline constexpr const char *STATE_DIRECTORY        = "sdmc:/config/JKSV/background/state";
    inline constexpr const char *LOG_PATH               = "sdmc:/config/JKSV/background-sync.log";
    inline constexpr const char *CONFIG_PATH            = "sdmc:/config/JKSV/baidu.json";
    inline constexpr const char *LOGOUT_MARKER_PATH     = "sdmc:/config/JKSV/baidu.logged_out";
    inline constexpr const char *SAVE_MOUNT             = "jksvbg";
    inline constexpr const char *SAVE_ROOT              = "jksvbg:/";
    inline constexpr const char *BOOT_FLAG_PATH         =
        "sdmc:/atmosphere/contents/420000004A4B5344/flags/boot2.flag";
    inline constexpr const char *BOOT_FLAG_CRASHED_PATH =
        "sdmc:/atmosphere/contents/420000004A4B5344/flags/boot2.flag.crashed";
    inline constexpr const char *RUN_MARKER_PATH = "sdmc:/config/JKSV/background/.running";

    // The resident monitor can't parse JSON, so what it reads is kept to plain files. Keep these in sync with
    // sysmodule/monitor/source/main.c.
    inline constexpr const char *RETRY_AFTER_PATH       = "sdmc:/config/JKSV/background/retry_after";
    inline constexpr const char *RETRY_STATE_PATH       = "sdmc:/config/JKSV/background/state/retry.state";
    inline constexpr const char *PLAY_LOG_POSITION_PATH = "sdmc:/config/JKSV/background/state/play_log.position";
    inline constexpr const char *MONITOR_LAUNCH_FAILED_PATH = "sdmc:/config/JKSV/background/monitor_launch_failed";

    // JKSV writes its process ID and boot time here while it runs. In applet mode it isn't an "application", so this is
    // the only way to tell the worker to stay out of the way of saves JKSV might be restoring.
    inline constexpr const char *JKSV_RUNNING_PATH = "sdmc:/config/JKSV/background/.jksv_running";

    // Queue items that keep failing are moved here instead of being retried forever. The ZIP is kept for the user.
    inline constexpr const char *FAILED_DIRECTORY = "sdmc:/config/JKSV/background/failed";
    inline constexpr int MAX_UPLOAD_FAILURES       = 8;

    inline constexpr uint32_t SAVE_META_MAGIC = 0x56534B4A;
    inline constexpr uint32_t STATE_MAGIC     = 0x53424B4A;
    inline constexpr uint16_t STATE_VERSION   = 1;
    inline constexpr size_t COPY_BUFFER_SIZE  = 64 * 1024;

    // Binary-compatible with JKSV's .nx_save_meta.bin format.
    struct SaveMetaData
    {
        uint32_t magic{};
        uint8_t revision{};
        uint64_t applicationID{};
        AccountUid accountID{};
        uint64_t systemSaveID{};
        uint8_t saveDataType{};
        uint8_t saveDataRank{};
        uint16_t saveDataIndex{};
        uint64_t ownerID{};
        uint64_t timestamp{};
        uint32_t flags{};
        int64_t saveDataSize{};
        int64_t journalSize{};
        uint64_t commitID{};
        uint8_t saveDataSpaceID{};
    } __attribute__((packed));

    static_assert(sizeof(SaveMetaData) == 86, "JKSV save metadata layout changed");

    struct SaveSignature
    {
        uint64_t commitId{};
        uint64_t timestamp{};
        uint64_t totalBytes{};
        uint64_t newestMtime{};
        uint32_t fileCount{};
    };

    struct StoredSignature
    {
        uint32_t magic{STATE_MAGIC};
        uint16_t version{STATE_VERSION};
        uint16_t reserved{};
        SaveSignature signature{};
    };

    struct Settings
    {
        bool enabled{true};
        bool includeDeviceSaves{true};
        bool keepLocal{false};
        bool verifyTls{true};
        int compressionLevel{1};
        int closeDelaySeconds{3};
        int retryMinutes{5};
    };

    struct PendingBackup
    {
        std::string key{};
        std::string zipPath{};
        std::string metadataPath{};
        std::string remotePath{};
        std::string title{};
        std::string user{};
        SaveSignature signature{};
        int failures{};
    };

    /// @brief Whether any game is open. Nothing that holds a save or uses the network should run while one is.
    bool application_running();

    /// @brief Whether JKSV is open, including in applet mode where application_running() can't see it.
    bool jksv_running();

    /// @brief Approximate wall clock time the console booted at. Used to tell a crash from a power-off.
    int64_t boot_epoch();

    /// @brief Reads games that closed since the last run from the system's play log.
    /// @param titlesOut Games that closed, oldest first.
    /// @param newestEntryOut Set to the newest log entry seen, for save_play_log_position.
    /// @return False if the play log couldn't be read. The position must not be saved then.
    bool closed_titles_since_last_run(std::vector<uint64_t> &titlesOut, int32_t &newestEntryOut);

    /// @brief Remembers how far into the play log has been handled.
    void save_play_log_position(int32_t entry);

    bool ensure_directory(std::string_view path);
    bool file_exists(std::string_view path);
    bool remove_file(std::string_view path);
    bool atomic_replace(std::string_view temporaryPath, std::string_view targetPath);
    void log(std::string_view message);
    void logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
    void sleep_seconds(unsigned seconds);
    std::string sanitize_component(std::string_view value, size_t maximumBytes = 160);
    std::string title_name(uint64_t titleId);
    std::string account_name(const AccountUid &uid, uint8_t saveType);
    std::string format_time(std::time_t timestamp, const char *format);
    std::string queue_key(const FsSaveDataInfo &saveInfo);
    bool load_settings(Settings &settings);
    bool prepare_title_backups(uint64_t titleId, const Settings &settings);
    size_t process_pending_backups(const Settings &settings);
    bool pending_backups_exist();
    bool queue_ultrahand_notification(size_t uploadedCount);
    bool queue_ultrahand_message(std::string_view text);
} // namespace background
