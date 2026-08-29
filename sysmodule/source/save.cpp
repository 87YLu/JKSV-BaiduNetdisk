#include "baidu.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <json-c/json.h>
#include <minizip/zip.h>
#include <zlib.h>

namespace
{
    std::array<uint8_t, background::COPY_BUFFER_SIZE> s_copyBuffer{};

    struct MountedSave
    {
        bool mounted{};

        ~MountedSave()
        {
            if (mounted) { fsdevUnmountDevice(background::SAVE_MOUNT); }
        }
    };

    json_object *get_object(json_object *parent, const char *key)
    {
        json_object *value{};
        return parent && json_object_object_get_ex(parent, key, &value) ? value : nullptr;
    }

    std::string get_string(json_object *parent, const char *key)
    {
        json_object *value = get_object(parent, key);
        const char *text   = value ? json_object_get_string(value) : nullptr;
        return text ? text : "";
    }

    uint64_t get_uint64(json_object *parent, const char *key)
    {
        json_object *value = get_object(parent, key);
        return value ? static_cast<uint64_t>(json_object_get_int64(value)) : 0;
    }

    uint32_t get_uint32(json_object *parent, const char *key)
    {
        return static_cast<uint32_t>(get_uint64(parent, key));
    }

    bool ends_with(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    std::string state_path(std::string_view key)
    {
        return std::string(background::STATE_DIRECTORY) + "/" + std::string(key) + ".state";
    }

    std::string queue_zip_path(std::string_view key)
    {
        return std::string(background::QUEUE_DIRECTORY) + "/" + std::string(key) + ".zip";
    }

    std::string queue_metadata_path(std::string_view key)
    {
        return std::string(background::QUEUE_DIRECTORY) + "/" + std::string(key) + ".json";
    }

    bool signatures_equal(const background::SaveSignature &left, const background::SaveSignature &right)
    {
        return left.commitId == right.commitId && left.timestamp == right.timestamp &&
               left.totalBytes == right.totalBytes && left.newestMtime == right.newestMtime &&
               left.fileCount == right.fileCount;
    }

    bool read_stored_signature(std::string_view key, background::SaveSignature &signature)
    {
        const std::string path = state_path(key);
        FILE *file             = std::fopen(path.c_str(), "rb");
        if (!file) { return false; }

        background::StoredSignature stored{};
        const bool read = std::fread(&stored, 1, sizeof(stored), file) == sizeof(stored);
        std::fclose(file);
        if (!read || stored.magic != background::STATE_MAGIC || stored.version != background::STATE_VERSION)
        {
            return false;
        }
        signature = stored.signature;
        return true;
    }

    bool write_stored_signature(std::string_view key, const background::SaveSignature &signature)
    {
        const std::string target = state_path(key);
        const std::string temp   = target + ".tmp";
        background::StoredSignature stored{};
        stored.signature = signature;

        FILE *file = std::fopen(temp.c_str(), "wb");
        if (!file) { return false; }
        const bool written = std::fwrite(&stored, 1, sizeof(stored), file) == sizeof(stored) &&
                             std::fflush(file) == 0;
        std::fclose(file);
        if (!written)
        {
            background::remove_file(temp);
            return false;
        }
        return background::atomic_replace(temp, target);
    }

    bool read_extra_data(const FsSaveDataInfo &saveInfo, FsSaveDataExtraData &extraData)
    {
        return R_SUCCEEDED(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
            &extraData,
            sizeof(extraData),
            static_cast<FsSaveDataSpaceId>(saveInfo.save_data_space_id),
            saveInfo.save_data_id));
    }

    bool fill_meta(const FsSaveDataInfo &saveInfo, background::SaveMetaData &meta, FsSaveDataExtraData &extraData)
    {
        if (!read_extra_data(saveInfo, extraData)) { return false; }

        meta = {.magic           = background::SAVE_META_MAGIC,
                .revision        = 0x01,
                .applicationID   = extraData.attr.application_id,
                .accountID       = extraData.attr.uid,
                .systemSaveID    = extraData.attr.system_save_data_id,
                .saveDataType    = extraData.attr.save_data_type,
                .saveDataRank    = extraData.attr.save_data_rank,
                .saveDataIndex   = extraData.attr.save_data_index,
                .ownerID         = extraData.owner_id,
                .timestamp       = extraData.timestamp,
                .flags           = extraData.flags,
                .saveDataSize    = extraData.data_size,
                .journalSize     = extraData.journal_size,
                .commitID        = extraData.commit_id,
                .saveDataSpaceID = saveInfo.save_data_space_id};
        return true;
    }

    bool mount_save(const FsSaveDataInfo &saveInfo, MountedSave &mount)
    {
        const FsSaveDataAttribute attribute = {.application_id      = saveInfo.application_id,
                                               .uid                 = saveInfo.uid,
                                               .system_save_data_id = saveInfo.system_save_data_id,
                                               .save_data_type      = saveInfo.save_data_type,
                                               .save_data_rank      = saveInfo.save_data_rank,
                                               .save_data_index     = saveInfo.save_data_index};

        for (int attempt = 0; attempt < 10; ++attempt)
        {
            if (background::g_runningTitleId.load(std::memory_order_acquire) != 0) { return false; }

            FsFileSystem fileSystem{};
            Result result = fsOpenSaveDataFileSystem(
                &fileSystem, static_cast<FsSaveDataSpaceId>(saveInfo.save_data_space_id), &attribute);
            if (R_SUCCEEDED(result))
            {
                mount.mounted = fsdevMountDevice(background::SAVE_MOUNT, fileSystem) != -1;
                return mount.mounted;
            }
            background::sleep_seconds(1);
        }
        return false;
    }

    void collect_signature(const std::string &directory,
                           background::SaveSignature &signature,
                           int depth = 0)
    {
        if (depth > 64) { return; }
        DIR *handle = ::opendir(directory.c_str());
        if (!handle) { return; }

        while (dirent *entry = ::readdir(handle))
        {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) { continue; }
            const std::string path = directory + (directory.back() == '/' ? "" : "/") + entry->d_name;
            struct stat info{};
            if (::stat(path.c_str(), &info) != 0) { continue; }
            if (S_ISDIR(info.st_mode)) { collect_signature(path, signature, depth + 1); }
            else if (S_ISREG(info.st_mode))
            {
                ++signature.fileCount;
                signature.totalBytes += static_cast<uint64_t>(std::max<off_t>(0, info.st_size));
                signature.newestMtime = std::max<uint64_t>(signature.newestMtime, info.st_mtime);
            }
        }
        ::closedir(handle);
    }

    zip_fileinfo make_zip_info()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        return {.tmz_date    = {.tm_sec  = local.tm_sec,
                                .tm_min  = local.tm_min,
                                .tm_hour = local.tm_hour,
                                .tm_mday = local.tm_mday,
                                .tm_mon  = local.tm_mon,
                                .tm_year = local.tm_year + 1900},
                .dosDate     = 0,
                .internal_fa = 0,
                .external_fa = 0};
    }

    bool add_buffer(zipFile zip, std::string_view name, const void *data, size_t size, int compressionLevel)
    {
        const zip_fileinfo info = make_zip_info();
        const int method        = compressionLevel == 0 ? 0 : Z_DEFLATED;
        if (zipOpenNewFileInZip64(zip,
                                  std::string(name).c_str(),
                                  &info,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0,
                                  nullptr,
                                  method,
                                  compressionLevel,
                                  size > 0xFFFFFFFFULL) != ZIP_OK)
        {
            return false;
        }
        const bool written = size == 0 || zipWriteInFileInZip(zip, data, static_cast<unsigned>(size)) == ZIP_OK;
        return zipCloseFileInZip(zip) == ZIP_OK && written;
    }

    bool add_directory_to_zip(zipFile zip,
                              const std::string &absoluteDirectory,
                              const std::string &relativeDirectory,
                              int compressionLevel,
                              int depth = 0)
    {
        if (depth > 64) { return false; }
        DIR *handle = ::opendir(absoluteDirectory.c_str());
        if (!handle) { return false; }

        bool success = true;
        while (success)
        {
            dirent *entry = ::readdir(handle);
            if (!entry) { break; }
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) { continue; }

            const std::string absolute =
                absoluteDirectory + (absoluteDirectory.back() == '/' ? "" : "/") + entry->d_name;
            const std::string relative =
                relativeDirectory.empty() ? entry->d_name : relativeDirectory + "/" + entry->d_name;
            struct stat info{};
            if (::stat(absolute.c_str(), &info) != 0)
            {
                success = false;
                break;
            }

            if (S_ISDIR(info.st_mode))
            {
                success = add_buffer(zip, relative + "/", nullptr, 0, 0) &&
                          add_directory_to_zip(zip, absolute, relative, compressionLevel, depth + 1);
                continue;
            }
            if (!S_ISREG(info.st_mode)) { continue; }

            FILE *source = std::fopen(absolute.c_str(), "rb");
            if (!source)
            {
                success = false;
                break;
            }

            const zip_fileinfo zipInfo = make_zip_info();
            const int method           = compressionLevel == 0 ? 0 : Z_DEFLATED;
            success = zipOpenNewFileInZip64(zip,
                                            relative.c_str(),
                                            &zipInfo,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            method,
                                            compressionLevel,
                                            info.st_size > 0xFFFFFFFFLL) == ZIP_OK;
            while (success)
            {
                const size_t read = std::fread(s_copyBuffer.data(), 1, s_copyBuffer.size(), source);
                if (read > 0 && zipWriteInFileInZip(zip, s_copyBuffer.data(), static_cast<unsigned>(read)) != ZIP_OK)
                {
                    success = false;
                    break;
                }
                if (read < s_copyBuffer.size())
                {
                    if (std::ferror(source)) { success = false; }
                    break;
                }
            }
            std::fclose(source);
            if (zipCloseFileInZip(zip) != ZIP_OK) { success = false; }
        }

        ::closedir(handle);
        return success;
    }

    bool create_zip(std::string_view outputPath,
                    const background::SaveMetaData &meta,
                    int compressionLevel)
    {
        const std::string output{outputPath};
        background::remove_file(output);
        zipFile zip = zipOpen64(output.c_str(), APPEND_STATUS_CREATE);
        if (!zip) { return false; }

        const bool success = add_buffer(zip, ".nx_save_meta.bin", &meta, sizeof(meta), compressionLevel) &&
                             add_directory_to_zip(zip, background::SAVE_ROOT, "", compressionLevel);
        const bool closed = zipClose(zip, nullptr) == ZIP_OK;
        if (!success || !closed) { background::remove_file(output); }
        return success && closed;
    }

    std::string configured_base_path()
    {
        std::string basePath = "/apps/JKSV";
        json_object *config  = json_object_from_file(background::CONFIG_PATH);
        if (config)
        {
            const std::string configured = get_string(config, "basepath");
            if (!configured.empty()) { basePath = configured; }
            json_object_put(config);
        }

        if (basePath.empty() || basePath.front() != '/') { basePath.insert(basePath.begin(), '/'); }
        while (basePath.size() > 1 && basePath.back() == '/') { basePath.pop_back(); }
        return basePath;
    }

    bool write_pending_metadata(const background::PendingBackup &pending)
    {
        json_object *root = json_object_new_object();
        if (!root) { return false; }
        json_object_object_add(root, "key", json_object_new_string(pending.key.c_str()));
        json_object_object_add(root, "zip_path", json_object_new_string(pending.zipPath.c_str()));
        json_object_object_add(root, "remote_path", json_object_new_string(pending.remotePath.c_str()));
        json_object_object_add(root, "title", json_object_new_string(pending.title.c_str()));
        json_object_object_add(root, "user", json_object_new_string(pending.user.c_str()));

        json_object *signature = json_object_new_object();
        json_object_object_add(
            signature, "commit_id", json_object_new_int64(static_cast<int64_t>(pending.signature.commitId)));
        json_object_object_add(
            signature, "timestamp", json_object_new_int64(static_cast<int64_t>(pending.signature.timestamp)));
        json_object_object_add(
            signature, "total_bytes", json_object_new_int64(static_cast<int64_t>(pending.signature.totalBytes)));
        json_object_object_add(
            signature, "newest_mtime", json_object_new_int64(static_cast<int64_t>(pending.signature.newestMtime)));
        json_object_object_add(signature, "file_count", json_object_new_int64(pending.signature.fileCount));
        json_object_object_add(root, "signature", signature);

        const std::string temporary = pending.metadataPath + ".tmp";
        const char *text            = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
        FILE *file                  = std::fopen(temporary.c_str(), "wb");
        bool written                = false;
        if (file)
        {
            const size_t length = std::strlen(text);
            written = std::fwrite(text, 1, length, file) == length && std::fflush(file) == 0;
            std::fclose(file);
        }
        json_object_put(root);

        if (!written)
        {
            background::remove_file(temporary);
            return false;
        }
        return background::atomic_replace(temporary, pending.metadataPath);
    }

    void migrate_legacy_remote_path(background::PendingBackup &pending)
    {
        // v1.0-v1.1 used "Title [TitleID]" as the cloud directory. JKSV's
        // foreground browser deliberately matches the exact title instead.
        const size_t fileSeparator = pending.remotePath.find_last_of('/');
        if (fileSeparator == std::string::npos || fileSeparator == 0 || pending.key.size() < 16 ||
            pending.title.empty())
        {
            return;
        }
        const size_t parentSeparator = pending.remotePath.find_last_of('/', fileSeparator - 1);
        if (parentSeparator == std::string::npos) { return; }

        const std::string parent = pending.remotePath.substr(
            parentSeparator + 1, fileSeparator - parentSeparator - 1);
        const std::string legacyParent = pending.title + " [" + pending.key.substr(0, 16) + "]";
        if (parent != legacyParent) { return; }

        const std::string previous = pending.remotePath;
        pending.remotePath = pending.remotePath.substr(0, parentSeparator + 1) +
                             background::sanitize_component(pending.title) +
                             pending.remotePath.substr(fileSeparator);
        if (!write_pending_metadata(pending))
        {
            background::logf("QUEUE_PATH_MIGRATION_WRITE_FAILED old=%s new=%s",
                             previous.c_str(),
                             pending.remotePath.c_str());
            return;
        }
        background::logf("QUEUE_PATH_MIGRATED old=%s new=%s",
                         previous.c_str(),
                         pending.remotePath.c_str());
    }

    bool read_pending_metadata(std::string_view metadataPath, background::PendingBackup &pending)
    {
        const std::string path{metadataPath};
        json_object *root = json_object_from_file(path.c_str());
        if (!root) { return false; }

        pending.metadataPath = path;
        pending.key          = get_string(root, "key");
        pending.zipPath      = get_string(root, "zip_path");
        pending.remotePath   = get_string(root, "remote_path");
        pending.title        = get_string(root, "title");
        pending.user         = get_string(root, "user");
        json_object *signature = get_object(root, "signature");
        pending.signature.commitId   = get_uint64(signature, "commit_id");
        pending.signature.timestamp  = get_uint64(signature, "timestamp");
        pending.signature.totalBytes = get_uint64(signature, "total_bytes");
        pending.signature.newestMtime = get_uint64(signature, "newest_mtime");
        pending.signature.fileCount   = get_uint32(signature, "file_count");
        json_object_put(root);

        migrate_legacy_remote_path(pending);

        return !pending.key.empty() && !pending.zipPath.empty() && !pending.remotePath.empty() &&
               background::file_exists(pending.zipPath);
    }

    bool pending_signature_matches(std::string_view key, const background::SaveSignature &signature)
    {
        background::PendingBackup pending{};
        return read_pending_metadata(queue_metadata_path(key), pending) &&
               signatures_equal(pending.signature, signature);
    }

    bool prepare_one(const FsSaveDataInfo &saveInfo, const background::Settings &settings)
    {
        background::SaveMetaData meta{};
        FsSaveDataExtraData extraData{};
        if (!fill_meta(saveInfo, meta, extraData))
        {
            background::logf("SAVE_META_FAILED title=%016llX save=%016llX",
                             static_cast<unsigned long long>(saveInfo.application_id),
                             static_cast<unsigned long long>(saveInfo.save_data_id));
            return false;
        }

        MountedSave mount{};
        if (!mount_save(saveInfo, mount))
        {
            background::logf("SAVE_MOUNT_FAILED title=%016llX type=%u",
                             static_cast<unsigned long long>(saveInfo.application_id),
                             saveInfo.save_data_type);
            return false;
        }

        background::SaveSignature signature{.commitId = extraData.commit_id, .timestamp = extraData.timestamp};
        collect_signature(background::SAVE_ROOT, signature);
        if (signature.fileCount == 0) { return true; }

        const std::string key = background::queue_key(saveInfo);
        background::SaveSignature stored{};
        if ((read_stored_signature(key, stored) && signatures_equal(stored, signature)) ||
            pending_signature_matches(key, signature))
        {
            background::logf("SAVE_UNCHANGED title=%016llX user=%016llX",
                             static_cast<unsigned long long>(saveInfo.application_id),
                             static_cast<unsigned long long>(saveInfo.uid.uid[0]));
            return true;
        }

        const std::string title = background::title_name(saveInfo.application_id);
        const std::string user  = background::account_name(saveInfo.uid, saveInfo.save_data_type);
        const std::string date  = background::format_time(std::time(nullptr), "%Y-%m-%d_%H-%M-%S");
        char saveTag[48]{};
        if (saveInfo.save_data_type == FsSaveDataType_Account)
        {
            std::snprintf(saveTag,
                          sizeof(saveTag),
                          "%016llX%016llX-%04X",
                          static_cast<unsigned long long>(saveInfo.uid.uid[0]),
                          static_cast<unsigned long long>(saveInfo.uid.uid[1]),
                          saveInfo.save_data_index);
        }
        else
        {
            std::snprintf(saveTag, sizeof(saveTag), "DEVICE-%04X", saveInfo.save_data_index);
        }
        const std::string filename =
            "AUTO - " + user + " [" + saveTag + "] - " + date + ".zip";
        const std::string remoteTitle = background::sanitize_component(title);

        background::PendingBackup pending{};
        pending.key          = key;
        pending.zipPath      = queue_zip_path(key);
        pending.metadataPath = queue_metadata_path(key);
        pending.remotePath   = configured_base_path() + "/" + remoteTitle + "/" + filename;
        pending.title        = title;
        pending.user         = user;
        pending.signature    = signature;

        const std::string temporaryZip = pending.zipPath + ".tmp";
        background::logf("BACKUP_START title=%s user=%s files=%u bytes=%llu",
                         title.c_str(),
                         user.c_str(),
                         signature.fileCount,
                         static_cast<unsigned long long>(signature.totalBytes));
        if (!create_zip(temporaryZip, meta, settings.compressionLevel))
        {
            background::remove_file(temporaryZip);
            background::logf("BACKUP_FAILED title=%s user=%s", title.c_str(), user.c_str());
            return false;
        }
        // Never leave old metadata pointing at newly replaced ZIP contents.
        background::remove_file(pending.metadataPath);
        if (!background::atomic_replace(temporaryZip, pending.zipPath) || !write_pending_metadata(pending))
        {
            background::remove_file(temporaryZip);
            background::remove_file(pending.zipPath);
            background::remove_file(pending.metadataPath);
            background::logf("BACKUP_FAILED title=%s user=%s", title.c_str(), user.c_str());
            return false;
        }

        background::logf("BACKUP_QUEUED title=%s user=%s", title.c_str(), user.c_str());
        return true;
    }

    std::vector<background::PendingBackup> list_pending_backups()
    {
        std::vector<background::PendingBackup> pending{};
        DIR *directory = ::opendir(background::QUEUE_DIRECTORY);
        if (!directory) { return pending; }

        while (dirent *entry = ::readdir(directory))
        {
            const std::string name = entry->d_name;
            if (!ends_with(name, ".json")) { continue; }
            background::PendingBackup item{};
            if (read_pending_metadata(std::string(background::QUEUE_DIRECTORY) + "/" + name, item))
            {
                pending.push_back(std::move(item));
            }
        }
        ::closedir(directory);
        std::sort(pending.begin(), pending.end(), [](const auto &left, const auto &right)
                  { return left.metadataPath < right.metadataPath; });
        return pending;
    }

    void keep_local_copy(const background::PendingBackup &pending)
    {
        const std::string directory = "sdmc:/JKSV/Background/" + background::sanitize_component(pending.title);
        if (!background::ensure_directory(directory)) { return; }
        const size_t slash = pending.remotePath.find_last_of('/');
        const std::string filename =
            slash == std::string::npos ? pending.key + ".zip" : pending.remotePath.substr(slash + 1);
        const std::string target = directory + "/" + background::sanitize_component(filename, 220);
        background::remove_file(target);
        if (::rename(pending.zipPath.c_str(), target.c_str()) != 0)
        {
            background::logf("KEEP_LOCAL_FAILED path=%s", target.c_str());
            background::remove_file(pending.zipPath);
        }
    }
} // namespace

bool background::prepare_title_backups(uint64_t titleId, const background::Settings &settings)
{
    if (titleId == 0 || background::g_runningTitleId.load(std::memory_order_acquire) != 0) { return false; }

    bool found{};
    bool success = true;
    static constexpr std::array<FsSaveDataSpaceId, 2> SPACES = {FsSaveDataSpaceId_User, FsSaveDataSpaceId_SdUser};
    for (const FsSaveDataSpaceId space : SPACES)
    {
        FsSaveDataInfoReader reader{};
        if (R_FAILED(fsOpenSaveDataInfoReader(&reader, space))) { continue; }

        while (true)
        {
            std::array<FsSaveDataInfo, 16> entries{};
            int64_t count{};
            if (R_FAILED(fsSaveDataInfoReaderRead(&reader, entries.data(), entries.size(), &count)) || count <= 0)
            {
                break;
            }
            for (int64_t index = 0; index < count; ++index)
            {
                const FsSaveDataInfo &saveInfo = entries[index];
                if (saveInfo.application_id != titleId) { continue; }
                const bool accountSave = saveInfo.save_data_type == FsSaveDataType_Account;
                const bool deviceSave  = saveInfo.save_data_type == FsSaveDataType_Device;
                if (!accountSave && !(deviceSave && settings.includeDeviceSaves)) { continue; }
                found   = true;
                success = prepare_one(saveInfo, settings) && success;
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }

    if (!found)
    {
        background::logf("NO_SUPPORTED_SAVE title=%016llX", static_cast<unsigned long long>(titleId));
    }
    return success;
}

bool background::pending_backups_exist()
{
    DIR *directory = ::opendir(background::QUEUE_DIRECTORY);
    if (!directory) { return false; }
    bool found{};
    while (dirent *entry = ::readdir(directory))
    {
        if (ends_with(entry->d_name, ".json"))
        {
            found = true;
            break;
        }
    }
    ::closedir(directory);
    return found;
}

void background::process_pending_backups(const background::Settings &settings)
{
    std::vector<background::PendingBackup> pending = list_pending_backups();
    if (pending.empty()) { return; }

    background::BaiduClient client{};
    if (!client.initialize(settings.verifyTls))
    {
        background::logf("BAIDU_NOT_READY error=%s", client.last_error().c_str());
        return;
    }

    for (const background::PendingBackup &item : pending)
    {
        if (background::g_runningTitleId.load(std::memory_order_acquire) != 0)
        {
            background::log("UPLOAD_DEFERRED game-running");
            return;
        }

        background::logf("UPLOAD_START title=%s user=%s", item.title.c_str(), item.user.c_str());
        if (!client.upload_file(item.zipPath, item.remotePath))
        {
            background::logf("UPLOAD_FAILED title=%s user=%s error=%s",
                             item.title.c_str(),
                             item.user.c_str(),
                             client.last_error().c_str());
            continue;
        }

        if (!write_stored_signature(item.key, item.signature))
        {
            background::logf("STATE_WRITE_FAILED key=%s", item.key.c_str());
        }
        if (settings.keepLocal) { keep_local_copy(item); }
        else { background::remove_file(item.zipPath); }
        background::remove_file(item.metadataPath);
        background::logf("UPLOAD_OK title=%s user=%s remote=%s",
                         item.title.c_str(),
                         item.user.c_str(),
                         item.remotePath.c_str());
    }
}
