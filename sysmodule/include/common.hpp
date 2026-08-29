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
    };

    extern std::atomic<uint64_t> g_runningTitleId;
    extern std::atomic<uint64_t> g_pendingTitleId;
    extern std::atomic<bool> g_shutdownRequested;

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
} // namespace background
