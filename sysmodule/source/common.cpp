#include "common.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

namespace
{
    std::mutex s_logMutex{};

    json_object *get_object(json_object *parent, const char *key)
    {
        json_object *value{};
        return parent && json_object_object_get_ex(parent, key, &value) ? value : nullptr;
    }

    bool get_bool(json_object *parent, const char *key, bool fallback)
    {
        json_object *value = get_object(parent, key);
        return value ? json_object_get_boolean(value) : fallback;
    }

    int get_int(json_object *parent, const char *key, int fallback)
    {
        json_object *value = get_object(parent, key);
        return value ? json_object_get_int(value) : fallback;
    }

    bool is_continuation(unsigned char value) { return (value & 0xC0) == 0x80; }

    size_t utf8_sequence_length(unsigned char lead)
    {
        if (lead < 0x80) { return 1; }
        if ((lead & 0xE0) == 0xC0) { return 2; }
        if ((lead & 0xF0) == 0xE0) { return 3; }
        if ((lead & 0xF8) == 0xF0) { return 4; }
        return 0;
    }
} // namespace

bool background::ensure_directory(std::string_view path)
{
    if (path.empty()) { return false; }

    std::string current{path};
    while (current.size() > 1 && current.back() == '/') { current.pop_back(); }

    const size_t deviceSeparator = current.find(":/");
    size_t offset = deviceSeparator == std::string::npos ? 1 : deviceSeparator + 2;
    while (offset < current.size())
    {
        const size_t slash = current.find('/', offset);
        const std::string partial = current.substr(0, slash);
        if (::mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST) { return false; }
        if (slash == std::string::npos) { break; }
        offset = slash + 1;
    }
    return ::mkdir(current.c_str(), 0777) == 0 || errno == EEXIST;
}

bool background::file_exists(std::string_view path)
{
    struct stat info{};
    const std::string value{path};
    return ::stat(value.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool background::remove_file(std::string_view path)
{
    const std::string value{path};
    return ::unlink(value.c_str()) == 0 || errno == ENOENT;
}

bool background::atomic_replace(std::string_view temporaryPath, std::string_view targetPath)
{
    const std::string temporary{temporaryPath};
    const std::string target{targetPath};
    const std::string backup = target + ".bak";

    ::unlink(backup.c_str());
    const bool hadTarget = background::file_exists(target);
    if (hadTarget && ::rename(target.c_str(), backup.c_str()) != 0) { return false; }

    if (::rename(temporary.c_str(), target.c_str()) != 0)
    {
        if (hadTarget) { ::rename(backup.c_str(), target.c_str()); }
        return false;
    }

    if (hadTarget) { ::unlink(backup.c_str()); }
    return true;
}

void background::log(std::string_view message)
{
    std::scoped_lock lock{s_logMutex};

    struct stat info{};
    if (::stat(background::LOG_PATH, &info) == 0 && info.st_size > 1024 * 1024)
    {
        const std::string oldPath = std::string(background::LOG_PATH) + ".old";
        ::unlink(oldPath.c_str());
        ::rename(background::LOG_PATH, oldPath.c_str());
    }

    FILE *file = std::fopen(background::LOG_PATH, "ab");
    if (!file) { return; }

    const std::string timestamp = background::format_time(std::time(nullptr), "%Y-%m-%d %H:%M:%S");
    std::fprintf(file, "[%s] %.*s\n", timestamp.c_str(), static_cast<int>(message.size()), message.data());
    std::fflush(file);
    std::fclose(file);
}

void background::logf(const char *format, ...)
{
    char buffer[1024]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    background::log(buffer);
}

void background::sleep_seconds(unsigned seconds)
{
    svcSleepThread(static_cast<int64_t>(seconds) * 1000000000LL);
}

std::string background::sanitize_component(std::string_view value, size_t maximumBytes)
{
    std::string output{};
    output.reserve(std::min(value.size(), maximumBytes));

    for (size_t index = 0; index < value.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        if (lead < 0x80)
        {
            char character = static_cast<char>(lead);
            if (lead < 0x20 || character == '/' || character == '\\' || character == ':' || character == '*' ||
                character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
            {
                character = '_';
            }
            if (output.size() + 1 > maximumBytes) { break; }
            output.push_back(character);
            ++index;
            continue;
        }

        const size_t sequenceLength = utf8_sequence_length(lead);
        bool valid = sequenceLength > 1 && index + sequenceLength <= value.size();
        for (size_t byte = 1; valid && byte < sequenceLength; ++byte)
        {
            valid = is_continuation(static_cast<unsigned char>(value[index + byte]));
        }
        if (!valid)
        {
            if (output.size() + 1 <= maximumBytes) { output.push_back('_'); }
            ++index;
            continue;
        }
        if (output.size() + sequenceLength > maximumBytes) { break; }
        output.append(value.substr(index, sequenceLength));
        index += sequenceLength;
    }

    const size_t first = output.find_first_not_of(" .");
    if (first == std::string::npos) { return "Unknown"; }
    output.erase(0, first);
    while (!output.empty() && (output.back() == ' ' || output.back() == '.')) { output.pop_back(); }
    return output.empty() ? "Unknown" : output;
}

std::string background::title_name(uint64_t titleId)
{
    auto control = std::unique_ptr<NsApplicationControlData>(new (std::nothrow) NsApplicationControlData{});
    if (control)
    {
        uint64_t actualSize{};
        if (R_SUCCEEDED(nsGetApplicationControlData(
                NsApplicationControlSource_Storage, titleId, control.get(), sizeof(*control), &actualSize)))
        {
            NacpLanguageEntry *entry{};
            if (R_SUCCEEDED(nacpGetLanguageEntry(&control->nacp, &entry)) && entry && entry->name[0] != '\0')
            {
                return background::sanitize_component(entry->name);
            }
        }
    }

    char fallback[17]{};
    std::snprintf(fallback, sizeof(fallback), "%016llX", static_cast<unsigned long long>(titleId));
    return fallback;
}

std::string background::account_name(const AccountUid &uid, uint8_t saveType)
{
    if (saveType == FsSaveDataType_Device) { return "Device"; }

    AccountProfile profile{};
    if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
    {
        AccountProfileBase base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base)))
        {
            accountProfileClose(&profile);
            if (base.nickname[0] != '\0') { return background::sanitize_component(base.nickname, 64); }
        }
        else { accountProfileClose(&profile); }
    }
    return "User";
}

std::string background::format_time(std::time_t timestamp, const char *format)
{
    std::tm local{};
    if (!localtime_r(&timestamp, &local)) { return "time-unavailable"; }
    char buffer[64]{};
    if (std::strftime(buffer, sizeof(buffer), format, &local) == 0) { return "time-unavailable"; }
    return buffer;
}

std::string background::queue_key(const FsSaveDataInfo &saveInfo)
{
    char key[128]{};
    std::snprintf(key,
                  sizeof(key),
                  "%016llX_%02X_%02X_%02X_%04X_%016llX_%016llX",
                  static_cast<unsigned long long>(saveInfo.application_id),
                  saveInfo.save_data_space_id,
                  saveInfo.save_data_type,
                  saveInfo.save_data_rank,
                  saveInfo.save_data_index,
                  static_cast<unsigned long long>(saveInfo.uid.uid[0]),
                  static_cast<unsigned long long>(saveInfo.uid.uid[1]));
    return key;
}

bool background::load_settings(background::Settings &settings)
{
    json_object *config = json_object_from_file(background::CONFIG_PATH);
    if (!config) { return false; }

    const bool signedOut        = get_bool(config, "signed_out", false) ||
                                  background::file_exists(background::LOGOUT_MARKER_PATH);
    settings.enabled            = get_bool(config, "background_sync", true) && !signedOut;
    settings.includeDeviceSaves = get_bool(config, "background_include_device_saves", true);
    settings.keepLocal          = get_bool(config, "background_keep_local", false);
    settings.verifyTls          = get_bool(config, "background_verify_tls", true);
    settings.compressionLevel   = std::clamp(get_int(config, "background_compression", 1), 0, 9);
    settings.closeDelaySeconds  = std::clamp(get_int(config, "background_close_delay_seconds", 3), 1, 30);
    settings.retryMinutes       = std::clamp(get_int(config, "background_retry_minutes", 5), 1, 180);

    json_object_put(config);
    return true;
}
