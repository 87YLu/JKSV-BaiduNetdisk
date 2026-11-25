#include "common.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

namespace
{
    constexpr const char *ULTRAHAND_CONFIG_PATH = "sdmc:/config/ultrahand/config.ini";
    constexpr const char *ULTRAHAND_NOTIFICATION_DIRECTORY =
        "sdmc:/config/ultrahand/notifications";

    bool path_has_type(const char *path, mode_t type)
    {
        struct stat info{};
        return ::stat(path, &info) == 0 && (info.st_mode & S_IFMT) == type;
    }
} // namespace

bool background::queue_ultrahand_message(std::string_view text)
{
    // nx-ovlloader alone does not consume .notify files. Requiring Ultrahand's
    // config prevents stale files from accumulating on Tesla-only setups.
    if (!path_has_type(ULTRAHAND_CONFIG_PATH, S_IFREG))
    {
        background::log("NOTIFY_SKIPPED backend=ultrahand reason=config-missing");
        return false;
    }
    if (!path_has_type(ULTRAHAND_NOTIFICATION_DIRECTORY, S_IFDIR))
    {
        background::log("NOTIFY_SKIPPED backend=ultrahand reason=notification-directory-missing");
        return false;
    }

    const unsigned long long uniqueId = static_cast<unsigned long long>(armGetSystemTick());
    char finalPath[128]{};
    char temporaryPath[128]{};
    const int finalLength = std::snprintf(finalPath,
                                          sizeof(finalPath),
                                          "%s/jksvbaidu-%016llx.notify",
                                          ULTRAHAND_NOTIFICATION_DIRECTORY,
                                          uniqueId);
    const int temporaryLength = std::snprintf(temporaryPath,
                                              sizeof(temporaryPath),
                                              "%s/jksvbaidu-%016llx.tmp",
                                              ULTRAHAND_NOTIFICATION_DIRECTORY,
                                              uniqueId);
    if (finalLength < 0 || static_cast<size_t>(finalLength) >= sizeof(finalPath) ||
        temporaryLength < 0 || static_cast<size_t>(temporaryLength) >= sizeof(temporaryPath))
    {
        background::log("NOTIFY_FAILED backend=ultrahand stage=path-too-long");
        return false;
    }

    // Game names can contain quotes and backslashes, so the JSON is built properly instead of with printf.
    json_object *root = json_object_new_object();
    if (!root)
    {
        background::log("NOTIFY_FAILED backend=ultrahand stage=payload");
        return false;
    }
    json_object_object_add(root, "title", json_object_new_string("JKSV 百度网盘"));
    json_object_object_add(root, "text", json_object_new_string_len(text.data(), static_cast<int>(text.size())));
    json_object_object_add(root, "font_size", json_object_new_int(22));
    json_object_object_add(root, "split_type", json_object_new_string("char"));
    json_object_object_add(root, "alignment", json_object_new_string("left"));
    json_object_object_add(root, "duration", json_object_new_int(3000));
    json_object_object_add(root, "show_time", json_object_new_string("false"));
    json_object_object_add(root, "priority", json_object_new_int(20));
    const std::string payload = std::string(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN)) + "\n";
    json_object_put(root);

    ::unlink(temporaryPath);
    FILE *file = std::fopen(temporaryPath, "wb");
    if (!file)
    {
        background::logf("NOTIFY_FAILED backend=ultrahand stage=open errno=%d", errno);
        return false;
    }

    const bool written = std::fwrite(payload.data(), 1, payload.size(), file) == payload.size();
    const bool flushed = written && std::fflush(file) == 0;
    const bool closed  = std::fclose(file) == 0;
    if (!flushed || !closed)
    {
        const int savedErrno = errno;
        ::unlink(temporaryPath);
        background::logf("NOTIFY_FAILED backend=ultrahand stage=write errno=%d", savedErrno);
        return false;
    }

    // Ultrahand only scans the .notify suffix, so publishing with a same-directory
    // rename prevents its 300 ms poller from observing a partially written JSON.
    if (::rename(temporaryPath, finalPath) != 0)
    {
        const int savedErrno = errno;
        ::unlink(temporaryPath);
        background::logf("NOTIFY_FAILED backend=ultrahand stage=publish errno=%d", savedErrno);
        return false;
    }

    background::log("NOTIFY_QUEUED backend=ultrahand");
    return true;
}

bool background::queue_ultrahand_notification(size_t uploadedCount)
{
    if (uploadedCount == 0) { return true; }

    char text[64]{};
    std::snprintf(text, sizeof(text), "已成功上传 %zu 个存档", uploadedCount);
    return background::queue_ultrahand_message(text);
}
