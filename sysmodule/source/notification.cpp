#include "common.hpp"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

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

bool background::queue_ultrahand_notification(size_t uploadedCount)
{
    if (uploadedCount == 0) { return true; }

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

    char payload[256]{};
    const int payloadLength = std::snprintf(
        payload,
        sizeof(payload),
        "{\"title\":\"JKSV 百度网盘\",\"text\":\"已成功上传 %zu 个存档\","
        "\"font_size\":22,\"split_type\":\"char\",\"alignment\":\"left\","
        "\"duration\":3000,\"show_time\":\"false\",\"priority\":20}\n",
        uploadedCount);
    if (payloadLength < 0 || static_cast<size_t>(payloadLength) >= sizeof(payload))
    {
        background::log("NOTIFY_FAILED backend=ultrahand stage=payload-too-long");
        return false;
    }

    ::unlink(temporaryPath);
    FILE *file = std::fopen(temporaryPath, "wb");
    if (!file)
    {
        background::logf("NOTIFY_FAILED backend=ultrahand stage=open errno=%d", errno);
        return false;
    }

    const size_t expected = static_cast<size_t>(payloadLength);
    const bool written = std::fwrite(payload, 1, expected, file) == expected;
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

    background::logf("NOTIFY_QUEUED backend=ultrahand count=%zu", uploadedCount);
    return true;
}
