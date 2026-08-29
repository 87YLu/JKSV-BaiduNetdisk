#include "remote/BaiduNetdisk.hpp"

#include "curl/curl.hpp"
#include "error.hpp"
#include "logging/logger.hpp"
#include "remote/remote.hpp"
#include "strings/names.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace
{
    constexpr std::string_view URL_BAIDU_API   = "https://pan.baidu.com";
    constexpr std::string_view URL_BAIDU_OAUTH = "https://openapi.baidu.com";
    constexpr std::string_view URL_BAIDU_PCS   = "https://d.pcs.baidu.com";

    constexpr std::string_view ENDPOINT_FILE       = "/rest/2.0/xpan/file";
    constexpr std::string_view ENDPOINT_META       = "/rest/2.0/xpan/multimedia";
    constexpr std::string_view ENDPOINT_NAS        = "/rest/2.0/xpan/nas";
    constexpr std::string_view ENDPOINT_DEVICE     = "/oauth/2.0/device/code";
    constexpr std::string_view ENDPOINT_TOKEN      = "/oauth/2.0/token";
    constexpr std::string_view ENDPOINT_SUPERFILE2 = "/rest/2.0/pcs/superfile2";

    constexpr size_t SIZE_UPLOAD_SLICE = 4 * 1024 * 1024;
    constexpr size_t MAX_UPLOAD_SLICES = 10000;
    constexpr size_t MAX_QR_IMAGE_SIZE = 1024 * 1024;
    constexpr int LIST_PAGE_SIZE       = 1000;
    constexpr int ERROR_FILE_EXISTS    = -8;
    constexpr int ERROR_PATH_MISSING   = -9;

    constexpr std::string_view MULTIPART_BOUNDARY = "----------------JKSVBaiduNetdiskBoundary";

    uint32_t rotate_left(uint32_t value, uint32_t amount) noexcept
    {
        return (value << amount) | (value >> (32U - amount));
    }

    /// @brief Small self-contained MD5 implementation used for Baidu's required block hashes.
    class Md5 final
    {
        public:
            Md5() = default;

            void update(const uint8_t *data, size_t length) noexcept
            {
                if (!data && length > 0) { return; }

                m_totalBytes += length;
                while (length > 0)
                {
                    const size_t copySize = std::min(length, m_buffer.size() - m_bufferSize);
                    std::copy(data, data + copySize, m_buffer.begin() + m_bufferSize);
                    data += copySize;
                    length -= copySize;
                    m_bufferSize += copySize;

                    if (m_bufferSize == m_buffer.size())
                    {
                        Md5::transform(m_buffer.data());
                        m_bufferSize = 0;
                    }
                }
            }

            std::array<uint8_t, 16> finish() noexcept
            {
                const uint64_t bitLength = m_totalBytes * 8;

                std::array<uint8_t, 64> padding{};
                padding[0]               = 0x80;
                const size_t paddingSize = m_bufferSize < 56 ? 56 - m_bufferSize : 120 - m_bufferSize;
                Md5::update(padding.data(), paddingSize);

                std::array<uint8_t, 8> lengthBytes{};
                for (size_t i = 0; i < lengthBytes.size(); i++)
                {
                    lengthBytes[i] = static_cast<uint8_t>((bitLength >> (i * 8)) & 0xFF);
                }
                Md5::update(lengthBytes.data(), lengthBytes.size());

                std::array<uint8_t, 16> digest{};
                for (size_t i = 0; i < m_state.size(); i++)
                {
                    for (size_t byte = 0; byte < sizeof(uint32_t); byte++)
                    {
                        digest[(i * sizeof(uint32_t)) + byte] =
                            static_cast<uint8_t>((m_state[i] >> (byte * 8)) & 0xFF);
                    }
                }
                return digest;
            }

        private:
            std::array<uint32_t, 4> m_state = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
            std::array<uint8_t, 64> m_buffer{};
            uint64_t m_totalBytes{};
            size_t m_bufferSize{};

            void transform(const uint8_t *block) noexcept
            {
                static constexpr std::array<uint32_t, 64> SHIFT = {
                    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22,
                    5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20,
                    4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                    6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};
                static constexpr std::array<uint32_t, 64> TABLE = {
                    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
                    0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
                    0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
                    0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
                    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
                    0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
                    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
                    0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
                    0xeb86d391};

                std::array<uint32_t, 16> words{};
                for (size_t i = 0; i < words.size(); i++)
                {
                    const size_t offset = i * sizeof(uint32_t);
                    words[i]            = static_cast<uint32_t>(block[offset]) |
                               (static_cast<uint32_t>(block[offset + 1]) << 8) |
                               (static_cast<uint32_t>(block[offset + 2]) << 16) |
                               (static_cast<uint32_t>(block[offset + 3]) << 24);
                }

                uint32_t a = m_state[0];
                uint32_t b = m_state[1];
                uint32_t c = m_state[2];
                uint32_t d = m_state[3];

                for (size_t i = 0; i < TABLE.size(); i++)
                {
                    uint32_t function{};
                    size_t wordIndex{};
                    if (i < 16)
                    {
                        function  = (b & c) | ((~b) & d);
                        wordIndex = i;
                    }
                    else if (i < 32)
                    {
                        function  = (d & b) | ((~d) & c);
                        wordIndex = ((5 * i) + 1) % 16;
                    }
                    else if (i < 48)
                    {
                        function  = b ^ c ^ d;
                        wordIndex = ((3 * i) + 5) % 16;
                    }
                    else
                    {
                        function  = c ^ (b | (~d));
                        wordIndex = (7 * i) % 16;
                    }

                    const uint32_t oldD = d;
                    d                   = c;
                    c                   = b;
                    b += rotate_left(a + function + TABLE[i] + words[wordIndex], SHIFT[i]);
                    a = oldD;
                }

                m_state[0] += a;
                m_state[1] += b;
                m_state[2] += c;
                m_state[3] += d;
            }
    };

    std::string md5_to_string(const std::array<uint8_t, 16> &digest)
    {
        static constexpr std::string_view HEX = "0123456789abcdef";
        std::string output(digest.size() * 2, '0');
        for (size_t i = 0; i < digest.size(); i++)
        {
            output[i * 2]       = HEX[digest[i] >> 4];
            output[(i * 2) + 1] = HEX[digest[i] & 0x0F];
        }
        return output;
    }

    bool read_exact(fslib::File &file, uint8_t *buffer, size_t size)
    {
        size_t totalRead{};
        while (totalRead < size)
        {
            const ssize_t readSize = file.read(buffer + totalRead, size - totalRead);
            if (readSize <= 0) { return false; }
            totalRead += static_cast<size_t>(readSize);
        }
        return true;
    }

    std::string normalize_remote_path(std::string_view input)
    {
        std::string path{input};
        if (path.empty()) { return {}; }
        if (path.front() != '/') { path.insert(path.begin(), '/'); }

        for (size_t duplicate = path.find("//"); duplicate != path.npos; duplicate = path.find("//"))
        {
            path.erase(duplicate, 1);
        }
        while (path.size() > 1 && path.back() == '/') { path.pop_back(); }

        size_t segmentBegin = 1;
        while (segmentBegin <= path.size())
        {
            const size_t segmentEnd = path.find('/', segmentBegin);
            const std::string_view segment{path.data() + segmentBegin,
                                           (segmentEnd == path.npos ? path.size() : segmentEnd) - segmentBegin};
            if (segment == "." || segment == "..") { return {}; }
            if (segmentEnd == path.npos) { break; }
            segmentBegin = segmentEnd + 1;
        }
        return path;
    }

    std::string parent_path(std::string_view path)
    {
        const size_t separator = path.find_last_of('/');
        if (separator == path.npos || separator == 0) { return "/"; }
        return std::string(path.substr(0, separator));
    }

    std::string filename_from_path(std::string_view path)
    {
        const size_t separator = path.find_last_of('/');
        return separator == path.npos ? std::string(path) : std::string(path.substr(separator + 1));
    }

    std::string join_remote_path(std::string_view parent, std::string_view name)
    {
        if (parent.empty() || name.empty() || name == "." || name == ".." || name.find('/') != name.npos)
        {
            return {};
        }

        std::string path{parent};
        if (path.back() != '/') { path.push_back('/'); }
        path.append(name);
        return normalize_remote_path(path);
    }

    json_object *get_json_object(json_object *parent, std::string_view key)
    {
        return parent ? json_object_object_get(parent, key.data()) : nullptr;
    }

    std::string get_json_string(json_object *parent, std::string_view key)
    {
        json_object *value = get_json_object(parent, key);
        return value ? json_object_get_string(value) : "";
    }

    int64_t get_json_int64(json_object *parent, std::string_view key, int64_t fallback = 0)
    {
        json_object *value = get_json_object(parent, key);
        return value ? json_object_get_int64(value) : fallback;
    }

    bool get_json_bool(json_object *parent, std::string_view key, bool fallback = false)
    {
        json_object *value = get_json_object(parent, key);
        return value ? json_object_get_boolean(value) : fallback;
    }

    bool path_has_prefix(std::string_view path, std::string_view prefix)
    {
        if (path == prefix) { return true; }
        return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
               path[prefix.size()] == '/';
    }

    struct MultipartReader
    {
        std::string header{};
        const std::vector<uint8_t> *data{};
        std::string footer{};
        size_t headerOffset{};
        size_t dataOffset{};
        size_t footerOffset{};
        int64_t progressOffset{};
        sys::ProgressTask *task{};
    };

    size_t read_multipart(char *buffer, size_t size, size_t count, MultipartReader *reader)
    {
        if (!reader || !reader->data) { return CURL_READFUNC_ABORT; }

        const size_t capacity = size * count;
        size_t written{};

        auto copyString = [&](const std::string &source, size_t &offset)
        {
            const size_t copySize = std::min(capacity - written, source.size() - offset);
            std::copy(source.data() + offset, source.data() + offset + copySize, buffer + written);
            offset += copySize;
            written += copySize;
        };

        if (reader->headerOffset < reader->header.size()) { copyString(reader->header, reader->headerOffset); }

        if (written < capacity && reader->headerOffset == reader->header.size() &&
            reader->dataOffset < reader->data->size())
        {
            const size_t copySize = std::min(capacity - written, reader->data->size() - reader->dataOffset);
            std::copy(reader->data->data() + reader->dataOffset,
                      reader->data->data() + reader->dataOffset + copySize,
                      buffer + written);
            reader->dataOffset += copySize;
            written += copySize;
            if (reader->task)
            {
                reader->task->update_current(
                    static_cast<double>(reader->progressOffset + static_cast<int64_t>(reader->dataOffset)));
            }
        }

        if (written < capacity && reader->headerOffset == reader->header.size() &&
            reader->dataOffset == reader->data->size() && reader->footerOffset < reader->footer.size())
        {
            copyString(reader->footer, reader->footerOffset);
        }

        return written;
    }

    struct DownloadTarget
    {
        fslib::File *file{};
        sys::ProgressTask *task{};
        int64_t written{};
    };

    size_t write_download(const char *buffer, size_t size, size_t count, DownloadTarget *target)
    {
        if (!target || !target->file) { return 0; }
        const size_t writeSize = size * count;
        const ssize_t written  = target->file->write(buffer, writeSize);
        if (written <= 0) { return 0; }
        target->written += written;
        if (target->task) { target->task->update_current(static_cast<double>(target->written)); }
        return static_cast<size_t>(written);
    }

    struct BoundedDownload
    {
        std::vector<uint8_t> *data{};
        size_t maximumSize{};
    };

    size_t write_bounded_download(const char *buffer, size_t size, size_t count, BoundedDownload *target)
    {
        if (!target || !target->data || (count > 0 && size > std::numeric_limits<size_t>::max() / count))
        {
            return 0;
        }

        const size_t incomingSize = size * count;
        if (incomingSize > target->maximumSize - std::min(target->maximumSize, target->data->size())) { return 0; }

        target->data->insert(target->data->end(), buffer, buffer + incomingSize);
        return incomingSize;
    }
} // namespace

remote::BaiduNetdisk::BaiduNetdisk(bool authorizeSignedOut)
    : Storage("[BD]")
{
    static constexpr std::string_view DEFAULT_BASE_PATH = "/apps/JKSV";

    json::Object config = json::new_object(json_object_from_file, remote::PATH_BAIDU_NETDISK_CONFIG.data());
    if (!config)
    {
        logger::log("Error initializing Baidu Netdisk: Unable to read configuration file.");
        return;
    }

    auto getConfigString = [&](std::string_view primary, std::string_view fallback = {})
    {
        std::string value = get_json_string(config.get(), primary);
        if (value.empty() && !fallback.empty()) { value = get_json_string(config.get(), fallback); }
        return value;
    };

    m_appKey                   = getConfigString("app_key", "client_id");
    m_secretKey                = getConfigString("secret_key", "client_secret");
    m_accessToken              = getConfigString("access_token");
    m_refreshToken             = getConfigString("refresh_token");
    m_accountName              = getConfigString("account_name");
    const std::string basePath = getConfigString("basepath");
    m_basePath                 = basePath.empty() ? DEFAULT_BASE_PATH : normalize_remote_path(basePath);
    m_tokenExpires             = static_cast<std::time_t>(get_json_int64(config.get(), "expires_at"));
    const bool persistedSignedOut = get_json_bool(config.get(), "signed_out") ||
                                    fslib::file_exists(remote::PATH_BAIDU_NETDISK_LOGOUT);

    if (persistedSignedOut)
    {
        m_accessToken.clear();
        m_refreshToken.clear();
        m_accountName.clear();
        m_tokenExpires = 0;
    }
    m_signedOut = persistedSignedOut && !authorizeSignedOut;

    if (m_appKey.empty() || m_secretKey.empty())
    {
        logger::log("Error initializing Baidu Netdisk: app_key or secret_key is missing.");
        return;
    }
    if (m_basePath.empty())
    {
        logger::log("Error initializing Baidu Netdisk: basepath is invalid.");
        return;
    }

    m_root   = m_basePath;
    m_parent = m_basePath;

    if (m_signedOut) { return; }

    bool authenticated = BaiduNetdisk::token_is_valid();
    if (!authenticated && !m_refreshToken.empty())
    {
        authenticated = BaiduNetdisk::refresh_token();
    }
    if (authenticated)
    {
        m_isInitialized = BaiduNetdisk::initialize_storage();

        // Baidu can revoke an access token before its advertised expiry. The first
        // request marks it invalid on errno=-6; refresh once immediately so startup
        // does not require a second JKSV launch in that case.
        if (!m_isInitialized && !BaiduNetdisk::token_is_valid() && !m_refreshToken.empty() &&
            BaiduNetdisk::refresh_token())
        {
            m_isInitialized = BaiduNetdisk::initialize_storage();
        }

        if (m_isInitialized)
        {
            const std::string previousAccountName = m_accountName;
            if (BaiduNetdisk::request_account_info() && m_accountName != previousAccountName)
            {
                BaiduNetdisk::save_tokens();
            }
        }
    }
}

bool remote::BaiduNetdisk::create_directory(std::string_view name)
{
    const std::string path = join_remote_path(m_parent, name);
    if (path.empty())
    {
        logger::log("Error creating Baidu Netdisk directory: Invalid directory name.");
        return false;
    }

    FileInfo directory{};
    if (!BaiduNetdisk::create_directory_path(path, &directory)) { return false; }

    if (directory.fsId > 0) { m_fsIds[directory.path] = directory.fsId; }
    m_list.emplace_back(directory.name, directory.path, m_parent, 0, true);
    return true;
}

bool remote::BaiduNetdisk::upload_file(const fslib::Path &source,
                                       std::string_view name,
                                       sys::ProgressTask *task)
{
    const std::string remotePath = join_remote_path(m_parent, name);
    if (remotePath.empty())
    {
        logger::log("Error uploading to Baidu Netdisk: Invalid filename.");
        return false;
    }

    FileInfo uploaded{};
    if (!BaiduNetdisk::upload_file_internal(source, remotePath, false, task, uploaded)) { return false; }

    if (uploaded.fsId > 0) { m_fsIds[uploaded.path] = uploaded.fsId; }
    m_list.emplace_back(uploaded.name, uploaded.path, m_parent, uploaded.size, false);
    return true;
}

bool remote::BaiduNetdisk::patch_file(remote::Item *file,
                                      const fslib::Path &source,
                                      sys::ProgressTask *task)
{
    if (!file || file->is_directory()) { return false; }

    FileInfo uploaded{};
    const std::string path{file->get_id()};
    if (!BaiduNetdisk::upload_file_internal(source, path, true, task, uploaded)) { return false; }

    if (uploaded.fsId > 0) { m_fsIds[path] = uploaded.fsId; }
    else { m_fsIds.erase(path); }
    file->set_size(static_cast<size_t>(std::max<int64_t>(0, uploaded.size)));
    return true;
}

bool remote::BaiduNetdisk::download_file(const remote::Item *file,
                                         const fslib::Path &destination,
                                         sys::ProgressTask *task)
{
    if (!file || file->is_directory() || !BaiduNetdisk::ensure_token()) { return false; }

    const std::string path{file->get_id()};
    auto fsId = m_fsIds.find(path);
    if (fsId == m_fsIds.end() || fsId->second <= 0)
    {
        FileInfo info{};
        if (!BaiduNetdisk::get_file_info(path, info)) { return false; }
        fsId = m_fsIds.insert_or_assign(path, info.fsId).first;
    }

    json::Object fsIds = json::new_object(json_object_new_array);
    json_object_array_add(fsIds.get(), json_object_new_int64(fsId->second));
    const std::string fsIdList = json_object_to_json_string_ext(fsIds.get(), JSON_C_TO_STRING_PLAIN);

    std::string metaResponse{};
    const Parameters metaParameters = {{"method", "filemetas"}, {"dlink", "1"}, {"fsids", fsIdList}};
    if (!BaiduNetdisk::perform_get(URL_BAIDU_API, ENDPOINT_META, metaParameters, metaResponse)) { return false; }

    json::Object metaParser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(metaResponse, "download metadata", metaParser)) { return false; }

    json_object *list = get_json_object(metaParser.get(), "list");
    json_object *info = list && json_object_array_length(list) > 0 ? json_object_array_get_idx(list, 0) : nullptr;
    const std::string downloadLink = get_json_string(info, "dlink");
    if (downloadLink.empty())
    {
        logger::log("Error downloading from Baidu Netdisk: dlink is missing from metadata response.");
        return false;
    }

    const int64_t expectedSize = static_cast<int64_t>(file->get_size());
    fslib::File destinationFile{destination, FsOpenMode_Create | FsOpenMode_Write, expectedSize};
    if (!destinationFile)
    {
        logger::log("Error downloading from Baidu Netdisk: %s", fslib::error::get_string());
        return false;
    }
    if (task) { task->reset(static_cast<double>(expectedSize)); }

    const std::string url = BaiduNetdisk::build_url(downloadLink, {}, {}, true);
    DownloadTarget downloadTarget{.file = &destinationFile, .task = task};
    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, url.c_str());
    curl::set_option(m_curl, CURLOPT_USERAGENT, "pan.baidu.com");
    curl::set_option(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl::set_option(m_curl, CURLOPT_MAXREDIRS, 5L);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, write_download);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &downloadTarget);

    if (!curl::perform(m_curl)) { return false; }
    const long responseCode = curl::get_response_code(m_curl);
    if (responseCode != 200 && responseCode != 206)
    {
        logger::log("Error downloading from Baidu Netdisk: HTTP response code %li.", responseCode);
        return false;
    }
    return expectedSize == 0 || downloadTarget.written == expectedSize;
}

bool remote::BaiduNetdisk::delete_item(const remote::Item *item)
{
    if (!item || !BaiduNetdisk::ensure_token()) { return false; }

    const std::string path{item->get_id()};
    json::Object fileList = json::new_object(json_object_new_array);
    json_object_array_add(fileList.get(), json_object_new_string(path.c_str()));
    const std::string fileListString = json_object_to_json_string_ext(fileList.get(), JSON_C_TO_STRING_PLAIN);

    std::string response{};
    const Parameters query = {{"method", "filemanager"}, {"opera", "delete"}};
    const Parameters form  = {{"async", "0"}, {"filelist", fileListString}};
    if (!BaiduNetdisk::perform_post_form(URL_BAIDU_API, ENDPOINT_FILE, query, form, response)) { return false; }

    json::Object parser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(response, "delete", parser)) { return false; }

    json_object *infoList = get_json_object(parser.get(), "info");
    json_object *info = infoList && json_object_array_length(infoList) > 0 ? json_object_array_get_idx(infoList, 0) : nullptr;
    if (info && get_json_int64(info, "errno") != 0)
    {
        logger::log("Error deleting from Baidu Netdisk: item operation failed with errno=%lli.",
                    static_cast<long long>(get_json_int64(info, "errno")));
        return false;
    }

    std::erase_if(m_list, [&](const remote::Item &entry) { return path_has_prefix(entry.get_id(), path); });
    for (auto current = m_fsIds.begin(); current != m_fsIds.end();)
    {
        if (path_has_prefix(current->first, path)) { current = m_fsIds.erase(current); }
        else { ++current; }
    }
    return true;
}

bool remote::BaiduNetdisk::rename_item(remote::Item *item, std::string_view newName)
{
    if (!item || newName.empty() || newName.find('/') != newName.npos || !BaiduNetdisk::ensure_token())
    {
        return false;
    }

    const std::string oldPath{item->get_id()};
    const std::string newPath = join_remote_path(parent_path(oldPath), newName);
    if (newPath.empty()) { return false; }

    json::Object renameItem = json::new_object(json_object_new_object);
    json_object_object_add(renameItem.get(), "path", json_object_new_string(oldPath.c_str()));
    const std::string newNameString{newName};
    json_object_object_add(renameItem.get(), "newname", json_object_new_string(newNameString.c_str()));

    json::Object fileList = json::new_object(json_object_new_array);
    json_object_array_add(fileList.get(), json_object_get(renameItem.get()));
    const std::string fileListString = json_object_to_json_string_ext(fileList.get(), JSON_C_TO_STRING_PLAIN);

    std::string response{};
    const Parameters query = {{"method", "filemanager"}, {"opera", "rename"}};
    const Parameters form  = {{"async", "0"}, {"filelist", fileListString}};
    if (!BaiduNetdisk::perform_post_form(URL_BAIDU_API, ENDPOINT_FILE, query, form, response)) { return false; }

    json::Object parser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(response, "rename", parser)) { return false; }

    json_object *infoList = get_json_object(parser.get(), "info");
    json_object *info = infoList && json_object_array_length(infoList) > 0 ? json_object_array_get_idx(infoList, 0) : nullptr;
    if (info && get_json_int64(info, "errno") != 0)
    {
        logger::log("Error renaming Baidu Netdisk item: item operation failed with errno=%lli.",
                    static_cast<long long>(get_json_int64(info, "errno")));
        return false;
    }

    std::unordered_map<std::string, int64_t> updatedFsIds{};
    updatedFsIds.reserve(m_fsIds.size());
    for (remote::Item &entry : m_list)
    {
        const std::string entryOldPath{entry.get_id()};
        const std::string entryOldParent{entry.get_parent_id()};
        std::string entryNewPath{entryOldPath};
        std::string entryNewParent{entryOldParent};

        if (path_has_prefix(entryOldPath, oldPath))
        {
            entryNewPath = newPath + entryOldPath.substr(oldPath.size());
            entry.set_id(entryNewPath);
        }
        if (path_has_prefix(entryOldParent, oldPath))
        {
            entryNewParent = newPath + entryOldParent.substr(oldPath.size());
            entry.set_parent_id(entryNewParent);
        }
        if (entryOldPath == oldPath) { entry.set_name(newName); }

        const auto oldFsId = m_fsIds.find(entryOldPath);
        if (oldFsId != m_fsIds.end()) { updatedFsIds[entryNewPath] = oldFsId->second; }
    }
    m_fsIds.swap(updatedFsIds);

    if (m_parent == oldPath || path_has_prefix(m_parent, oldPath))
    {
        m_parent = newPath + m_parent.substr(oldPath.size());
    }
    return true;
}

bool remote::BaiduNetdisk::sign_in_required() const noexcept
{
    const bool configValid = !m_appKey.empty() && !m_secretKey.empty() && !m_basePath.empty();
    return !m_signedOut && configValid && !BaiduNetdisk::token_is_valid() &&
           (m_refreshToken.empty() || m_refreshRejected);
}

bool remote::BaiduNetdisk::get_sign_in_data(std::string &message,
                                            std::string &deviceCode,
                                            std::time_t &expiration,
                                            int &pollingInterval,
                                            std::vector<uint8_t> &qrImage)
{
    const Parameters parameters = {
        {"response_type", "device_code"}, {"client_id", m_appKey}, {"scope", "basic,netdisk"}};

    std::string response{};
    if (!BaiduNetdisk::perform_get(URL_BAIDU_OAUTH, ENDPOINT_DEVICE, parameters, response, false)) { return false; }

    json::Object parser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(response, "device authorization", parser)) { return false; }

    deviceCode                       = get_json_string(parser.get(), "device_code");
    const std::string userCode       = get_json_string(parser.get(), "user_code");
    const std::string verification   = get_json_string(parser.get(), "verification_url");
    const std::string qrCodeUrl      = get_json_string(parser.get(), "qrcode_url");
    const int64_t expiresIn          = get_json_int64(parser.get(), "expires_in");
    const int64_t requestedInterval  = get_json_int64(parser.get(), "interval", 5);
    const char *translatedTemplate   = strings::get_by_name(strings::names::BAIDU_NETDISK, 0);
    const char *fallbackTemplate     = "Scan the QR code with the Baidu Netdisk app.\n"
                                       "Or go to #%s# and enter >%s>. Press [B] to cancel.";
    const char *authorizationMessage = translatedTemplate ? translatedTemplate : fallbackTemplate;

    if (deviceCode.empty() || userCode.empty() || verification.empty() || expiresIn <= 0)
    {
        logger::log("Error requesting Baidu Netdisk sign-in data: Malformed response.");
        return false;
    }

    message         = stringutil::get_formatted_string(authorizationMessage, verification.c_str(), userCode.c_str());
    expiration      = std::time(nullptr) + expiresIn;
    pollingInterval = static_cast<int>(
        std::clamp<int64_t>(requestedInterval, 3, std::numeric_limits<int>::max()));

    qrImage.clear();
    if (!qrCodeUrl.empty() && !BaiduNetdisk::download_qr_image(qrCodeUrl, qrImage))
    {
        logger::log("Unable to download the Baidu Netdisk authorization QR image; using URL and code fallback.");
    }
    return true;
}

bool remote::BaiduNetdisk::poll_sign_in(std::string_view deviceCode)
{
    const Parameters parameters = {{"grant_type", "device_token"},
                                   {"code", std::string(deviceCode)},
                                   {"client_id", m_appKey},
                                   {"client_secret", m_secretKey}};

    std::string response{};
    if (!BaiduNetdisk::perform_get(URL_BAIDU_OAUTH, ENDPOINT_TOKEN, parameters, response, false)) { return false; }

    json::Object parser{nullptr, json_object_put};
    int errorNumber{};
    if (!BaiduNetdisk::response_succeeded(response, "device token", parser, false, &errorNumber))
    {
        const std::string oauthError = get_json_string(parser.get(), "error");
        if (oauthError != "authorization_pending" && oauthError != "slow_down")
        {
            logger::log("Baidu Netdisk device authorization failed: %s.", oauthError.c_str());
        }
        return false;
    }

    const std::string accessToken  = get_json_string(parser.get(), "access_token");
    const std::string refreshToken = get_json_string(parser.get(), "refresh_token");
    const int64_t expiresIn        = get_json_int64(parser.get(), "expires_in");
    if (accessToken.empty() || refreshToken.empty() || expiresIn <= 0)
    {
        logger::log("Baidu Netdisk device authorization failed: Malformed token response.");
        return false;
    }

    m_accessToken  = accessToken;
    m_refreshToken = refreshToken;
    m_tokenExpires = std::time(nullptr) + expiresIn;
    m_refreshRejected = false;
    m_signedOut = false;
    m_accountName.clear();
    if (!BaiduNetdisk::save_tokens(true)) { return false; }
    if (BaiduNetdisk::request_account_info() && !BaiduNetdisk::save_tokens(true)) { return false; }

    const bool logoutMarkerExists = fslib::file_exists(remote::PATH_BAIDU_NETDISK_LOGOUT);
    if (logoutMarkerExists && !fslib::delete_file(remote::PATH_BAIDU_NETDISK_LOGOUT))
    {
        logger::log("Error completing Baidu Netdisk sign in: Unable to clear the local sign-out marker.");
        return false;
    }

    m_isInitialized = BaiduNetdisk::initialize_storage();
    return m_isInitialized;
}

bool remote::BaiduNetdisk::initialize_storage()
{
    m_root   = m_basePath;
    m_parent = m_basePath;

    int errorNumber{};
    if (BaiduNetdisk::request_listing(&errorNumber)) { return true; }
    if (errorNumber != ERROR_PATH_MISSING) { return false; }

    FileInfo baseDirectory{};
    if (!BaiduNetdisk::create_directory_path(m_basePath, &baseDirectory)) { return false; }
    return BaiduNetdisk::request_listing();
}

bool remote::BaiduNetdisk::request_account_info()
{
    if (m_accessToken.empty()) { return false; }

    std::string response{};
    const Parameters parameters = {{"method", "uinfo"}, {"vip_version", "v2"}};
    if (!BaiduNetdisk::perform_get(URL_BAIDU_API, ENDPOINT_NAS, parameters, response, true, "pan.baidu.com"))
    {
        return false;
    }

    json::Object parser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(response, "account information", parser)) { return false; }

    std::string accountName = get_json_string(parser.get(), "netdisk_name");
    if (accountName.empty()) { accountName = get_json_string(parser.get(), "baidu_name"); }
    if (accountName.empty())
    {
        const int64_t userId = get_json_int64(parser.get(), "uk");
        if (userId > 0) { accountName = "UK " + std::to_string(userId); }
    }
    if (accountName.empty())
    {
        logger::log("Baidu Netdisk account information did not include an account name or user ID.");
        return false;
    }

    m_accountName = std::move(accountName);
    return true;
}

bool remote::BaiduNetdisk::token_is_valid() const noexcept
{
    return !m_signedOut && !m_accessToken.empty() && std::time(nullptr) < m_tokenExpires - 60;
}

bool remote::BaiduNetdisk::ensure_token()
{
    return !m_signedOut && (BaiduNetdisk::token_is_valid() || BaiduNetdisk::refresh_token());
}

bool remote::BaiduNetdisk::refresh_token()
{
    if (m_refreshToken.empty()) { return false; }
    m_refreshRejected = false;

    const Parameters parameters = {{"grant_type", "refresh_token"},
                                   {"refresh_token", m_refreshToken},
                                   {"client_id", m_appKey},
                                   {"client_secret", m_secretKey}};
    std::string response{};
    if (!BaiduNetdisk::perform_get(URL_BAIDU_OAUTH, ENDPOINT_TOKEN, parameters, response, false)) { return false; }

    json::Object parser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(response, "token refresh", parser))
    {
        const std::string oauthError = get_json_string(parser.get(), "error");
        m_refreshRejected = oauthError == "invalid_grant" || oauthError == "invalid_client" ||
                            oauthError == "unauthorized_client";
        return false;
    }

    const std::string accessToken  = get_json_string(parser.get(), "access_token");
    const std::string refreshToken = get_json_string(parser.get(), "refresh_token");
    const int64_t expiresIn        = get_json_int64(parser.get(), "expires_in");
    if (accessToken.empty() || expiresIn <= 0)
    {
        logger::log("Baidu Netdisk token refresh failed: Malformed response.");
        return false;
    }

    m_accessToken = accessToken;
    if (!refreshToken.empty()) { m_refreshToken = refreshToken; }
    m_tokenExpires = std::time(nullptr) + expiresIn;
    m_refreshRejected = false;
    return BaiduNetdisk::save_tokens();
}

bool remote::BaiduNetdisk::save_tokens(bool allowLogoutMarker)
{
    json::Object config = json::new_object(json_object_from_file, remote::PATH_BAIDU_NETDISK_CONFIG.data());
    if (!config) { return false; }

    const bool signedOut = get_json_bool(config.get(), "signed_out") ||
                           fslib::file_exists(remote::PATH_BAIDU_NETDISK_LOGOUT);
    if (signedOut && !allowLogoutMarker)
    {
        m_signedOut = true;
        return false;
    }

    json_object_object_add(config.get(), "access_token", json_object_new_string(m_accessToken.c_str()));
    json_object_object_add(config.get(), "refresh_token", json_object_new_string(m_refreshToken.c_str()));
    json_object_object_add(config.get(), "expires_at", json_object_new_int64(m_tokenExpires));
    json_object_object_add(config.get(), "account_name", json_object_new_string(m_accountName.c_str()));
    json_object_object_add(config.get(), "signed_out", json_object_new_boolean(false));

    const char *jsonString  = json_object_to_json_string_ext(config.get(), JSON_C_TO_STRING_PRETTY);
    const size_t stringSize = std::char_traits<char>::length(jsonString);
    if (stringSize > static_cast<size_t>(std::numeric_limits<int64_t>::max())) { return false; }
    const int64_t jsonSize = static_cast<int64_t>(stringSize);
    fslib::File configFile{remote::PATH_BAIDU_NETDISK_CONFIG, FsOpenMode_Create | FsOpenMode_Write, jsonSize};
    if (!configFile || configFile.write(jsonString, static_cast<uint64_t>(jsonSize)) != jsonSize ||
        !configFile.flush())
    {
        logger::log("Error saving Baidu Netdisk OAuth tokens: %s", fslib::error::get_string());
        return false;
    }
    return true;
}

bool remote::BaiduNetdisk::download_qr_image(std::string_view url, std::vector<uint8_t> &image)
{
    static constexpr std::array<std::string_view, 2> ALLOWED_PREFIXES = {
        "https://openapi.baidu.com/", "http://openapi.baidu.com/"};
    const bool trustedUrl = std::ranges::any_of(
        ALLOWED_PREFIXES, [&](std::string_view prefix) { return url.starts_with(prefix); });
    if (!trustedUrl) { return false; }

    const std::string urlString{url};
    image.clear();
    image.reserve(64 * 1024);
    BoundedDownload download{.data = &image, .maximumSize = MAX_QR_IMAGE_SIZE};

    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, urlString.c_str());
    curl::set_option(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl::set_option(m_curl, CURLOPT_MAXREDIRS, 5L);
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, write_bounded_download);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &download);

    if (!curl::perform(m_curl))
    {
        image.clear();
        return false;
    }

    const long responseCode = curl::get_response_code(m_curl);
    if (responseCode < 200 || responseCode >= 300 || image.empty())
    {
        image.clear();
        return false;
    }
    return true;
}

bool remote::BaiduNetdisk::request_listing(int *errorNumber, bool logErrors)
{
    if (!BaiduNetdisk::ensure_token()) { return false; }

    m_list.clear();
    m_fsIds.clear();
    m_parent = m_root;
    return BaiduNetdisk::request_directory_listing(m_root, true, errorNumber, logErrors);
}

bool remote::BaiduNetdisk::request_directory_listing(std::string_view directory,
                                                      bool recursive,
                                                      int *errorNumber,
                                                      bool logErrors)
{
    std::vector<std::string> childDirectories{};
    int start{};

    while (true)
    {
        const Parameters parameters = {{"method", "list"},
                                       {"dir", std::string(directory)},
                                       {"order", "name"},
                                       {"desc", "0"},
                                       {"start", std::to_string(start)},
                                       {"limit", std::to_string(LIST_PAGE_SIZE)}};
        std::string response{};
        if (!BaiduNetdisk::perform_get(URL_BAIDU_API, ENDPOINT_FILE, parameters, response)) { return false; }

        json::Object parser{nullptr, json_object_put};
        if (!BaiduNetdisk::response_succeeded(response, "directory listing", parser, logErrors, errorNumber))
        {
            return false;
        }

        json_object *list = get_json_object(parser.get(), "list");
        if (!list || !json_object_is_type(list, json_type_array))
        {
            logger::log("Error reading Baidu Netdisk directory listing: Missing list array.");
            return false;
        }

        const size_t listLength = json_object_array_length(list);
        for (size_t i = 0; i < listLength; i++)
        {
            json_object *entry      = json_object_array_get_idx(list, i);
            const std::string path  = get_json_string(entry, "path");
            const std::string name  = get_json_string(entry, "server_filename");
            const int64_t fsId      = get_json_int64(entry, "fs_id");
            const int64_t size      = get_json_int64(entry, "size");
            const bool isDirectory  = get_json_int64(entry, "isdir") == 1;
            if (path.empty() || name.empty()) { continue; }

            m_fsIds[path] = fsId;
            m_list.emplace_back(name, path, directory, size, isDirectory);
            if (isDirectory) { childDirectories.push_back(path); }
        }

        start += static_cast<int>(listLength);
        if (listLength < LIST_PAGE_SIZE) { break; }
    }

    if (recursive)
    {
        for (const std::string &child : childDirectories)
        {
            if (!BaiduNetdisk::request_directory_listing(child, true, errorNumber, logErrors)) { return false; }
        }
    }
    return true;
}

bool remote::BaiduNetdisk::get_file_info(std::string_view path, FileInfo &fileInfo)
{
    if (!BaiduNetdisk::ensure_token()) { return false; }

    const std::string parent = parent_path(path);
    int start{};
    while (true)
    {
        const Parameters parameters = {{"method", "list"},
                                       {"dir", parent},
                                       {"start", std::to_string(start)},
                                       {"limit", std::to_string(LIST_PAGE_SIZE)}};
        std::string response{};
        if (!BaiduNetdisk::perform_get(URL_BAIDU_API, ENDPOINT_FILE, parameters, response)) { return false; }

        json::Object parser{nullptr, json_object_put};
        if (!BaiduNetdisk::response_succeeded(response, "file lookup", parser)) { return false; }

        json_object *list = get_json_object(parser.get(), "list");
        const size_t listLength = list && json_object_is_type(list, json_type_array)
                                      ? json_object_array_length(list)
                                      : 0;
        for (size_t i = 0; i < listLength; i++)
        {
            json_object *entry     = json_object_array_get_idx(list, i);
            const std::string itemPath = get_json_string(entry, "path");
            if (itemPath != path) { continue; }

            fileInfo.name        = get_json_string(entry, "server_filename");
            fileInfo.path        = itemPath;
            fileInfo.fsId        = get_json_int64(entry, "fs_id");
            fileInfo.size        = get_json_int64(entry, "size");
            fileInfo.isDirectory = get_json_int64(entry, "isdir") == 1;
            return true;
        }

        start += static_cast<int>(listLength);
        if (listLength < LIST_PAGE_SIZE) { break; }
    }
    return false;
}

bool remote::BaiduNetdisk::create_directory_path(std::string_view path, FileInfo *fileInfo)
{
    if (!BaiduNetdisk::ensure_token()) { return false; }

    const Parameters query = {{"method", "create"}};
    const Parameters form  = {{"path", std::string(path)}, {"isdir", "1"}, {"rtype", "0"}};
    std::string response{};
    if (!BaiduNetdisk::perform_post_form(URL_BAIDU_API, ENDPOINT_FILE, query, form, response)) { return false; }

    json::Object parser{nullptr, json_object_put};
    int errorNumber{};
    if (!BaiduNetdisk::response_succeeded(response, "create directory", parser, false, &errorNumber))
    {
        if (errorNumber == ERROR_FILE_EXISTS)
        {
            FileInfo existing{};
            if (!BaiduNetdisk::get_file_info(path, existing) || !existing.isDirectory) { return false; }
            if (fileInfo) { *fileInfo = std::move(existing); }
            return true;
        }
        logger::log("Error creating Baidu Netdisk directory: errno=%i.", errorNumber);
        return false;
    }

    if (fileInfo)
    {
        fileInfo->path        = get_json_string(parser.get(), "path");
        if (fileInfo->path.empty()) { fileInfo->path = path; }
        fileInfo->name        = filename_from_path(fileInfo->path);
        fileInfo->fsId        = get_json_int64(parser.get(), "fs_id");
        fileInfo->size        = 0;
        fileInfo->isDirectory = true;
    }
    return true;
}

bool remote::BaiduNetdisk::upload_file_internal(const fslib::Path &source,
                                                std::string_view remotePath,
                                                bool overwrite,
                                                sys::ProgressTask *task,
                                                FileInfo &uploadedFile)
{
    if (!BaiduNetdisk::ensure_token()) { return false; }

    fslib::File sourceFile{source, FsOpenMode_Read};
    if (!sourceFile)
    {
        logger::log("Error uploading to Baidu Netdisk: %s", fslib::error::get_string());
        return false;
    }

    const int64_t fileSize = sourceFile.get_size();
    if (fileSize < 0 || static_cast<uint64_t>(fileSize) > SIZE_UPLOAD_SLICE * MAX_UPLOAD_SLICES)
    {
        logger::log("Error uploading to Baidu Netdisk: File exceeds the supported slice count.");
        return false;
    }

    std::vector<std::string> blockHashes{};
    const uint64_t unsignedFileSize = static_cast<uint64_t>(fileSize);
    const size_t blockCount = static_cast<size_t>(
        std::max<uint64_t>(1, (unsignedFileSize + SIZE_UPLOAD_SLICE - 1) / SIZE_UPLOAD_SLICE));
    blockHashes.reserve(blockCount);
    std::vector<uint8_t> hashBuffer(SIZE_UPLOAD_SLICE);
    Md5 contentHasher{};
    Md5 firstSliceHasher{};
    size_t firstSliceRemaining = 256 * 1024;

    for (int64_t offset = 0; offset < fileSize; offset += SIZE_UPLOAD_SLICE)
    {
        const size_t currentSize = static_cast<size_t>(std::min<int64_t>(SIZE_UPLOAD_SLICE, fileSize - offset));
        if (!read_exact(sourceFile, hashBuffer.data(), currentSize))
        {
            logger::log("Error uploading to Baidu Netdisk: Failed to read source while hashing.");
            return false;
        }

        Md5 blockHasher{};
        blockHasher.update(hashBuffer.data(), currentSize);
        blockHashes.push_back(md5_to_string(blockHasher.finish()));
        contentHasher.update(hashBuffer.data(), currentSize);

        const size_t firstSliceSize = std::min(firstSliceRemaining, currentSize);
        firstSliceHasher.update(hashBuffer.data(), firstSliceSize);
        firstSliceRemaining -= firstSliceSize;
    }

    if (fileSize == 0)
    {
        Md5 emptyHasher{};
        blockHashes.push_back(md5_to_string(emptyHasher.finish()));
    }

    const std::string contentMd5 = md5_to_string(contentHasher.finish());
    const std::string sliceMd5   = md5_to_string(firstSliceHasher.finish());
    sourceFile.seek(0, fslib::Stream::BEGINNING);

    json::Object blockList = json::new_object(json_object_new_array);
    for (const std::string &hash : blockHashes)
    {
        json_object_array_add(blockList.get(), json_object_new_string(hash.c_str()));
    }
    const std::string blockListString = json_object_to_json_string_ext(blockList.get(), JSON_C_TO_STRING_PLAIN);

    const std::string remotePathString{remotePath};
    const std::string rtype = overwrite ? "3" : "0";
    const Parameters precreateQuery = {{"method", "precreate"}};
    const Parameters precreateForm  = {{"path", remotePathString},
                                       {"size", std::to_string(fileSize)},
                                       {"isdir", "0"},
                                       {"autoinit", "1"},
                                       {"rtype", rtype},
                                       {"block_list", blockListString},
                                       {"content-md5", contentMd5},
                                       {"slice-md5", sliceMd5}};
    std::string precreateResponse{};
    if (!BaiduNetdisk::perform_post_form(
            URL_BAIDU_API, ENDPOINT_FILE, precreateQuery, precreateForm, precreateResponse))
    {
        return false;
    }

    json::Object precreateParser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(precreateResponse, "upload precreate", precreateParser)) { return false; }

    const int returnType = static_cast<int>(get_json_int64(precreateParser.get(), "return_type"));
    if (returnType == 2)
    {
        json_object *rapidInfo = get_json_object(precreateParser.get(), "info");
        if (rapidInfo)
        {
            uploadedFile.name        = get_json_string(rapidInfo, "server_filename");
            if (uploadedFile.name.empty()) { uploadedFile.name = filename_from_path(remotePath); }
            uploadedFile.path        = get_json_string(rapidInfo, "path");
            if (uploadedFile.path.empty()) { uploadedFile.path = remotePath; }
            uploadedFile.fsId        = get_json_int64(rapidInfo, "fs_id");
            uploadedFile.size        = get_json_int64(rapidInfo, "size", fileSize);
            uploadedFile.isDirectory = false;
            if (uploadedFile.fsId == 0)
            {
                FileInfo refreshed{};
                if (BaiduNetdisk::get_file_info(remotePath, refreshed)) { uploadedFile = std::move(refreshed); }
            }
            return true;
        }
        if (BaiduNetdisk::get_file_info(remotePath, uploadedFile)) { return true; }

        uploadedFile.name = filename_from_path(remotePath);
        uploadedFile.path = remotePath;
        uploadedFile.size = fileSize;
        const auto existingFsId = m_fsIds.find(uploadedFile.path);
        if (existingFsId != m_fsIds.end()) { uploadedFile.fsId = existingFsId->second; }
        return true;
    }

    const std::string uploadId = get_json_string(precreateParser.get(), "uploadid");
    if (uploadId.empty())
    {
        logger::log("Error uploading to Baidu Netdisk: Precreate response is missing uploadid.");
        return false;
    }

    std::vector<size_t> requiredParts{};
    json_object *requiredList = get_json_object(precreateParser.get(), "block_list");
    if (requiredList && json_object_is_type(requiredList, json_type_array))
    {
        const size_t requiredCount = json_object_array_length(requiredList);
        for (size_t i = 0; i < requiredCount; i++)
        {
            const int64_t part = json_object_get_int64(json_object_array_get_idx(requiredList, i));
            if (part >= 0 && static_cast<size_t>(part) < blockHashes.size())
            {
                requiredParts.push_back(static_cast<size_t>(part));
            }
        }
    }
    if (!requiredList)
    {
        requiredParts.resize(blockHashes.size());
        for (size_t i = 0; i < requiredParts.size(); i++) { requiredParts[i] = i; }
    }

    if (task) { task->reset(static_cast<double>(fileSize)); }
    int64_t uploadedBytes{};
    for (const size_t part : requiredParts)
    {
        const int64_t offset = static_cast<int64_t>(part * SIZE_UPLOAD_SLICE);
        const size_t currentSize = fileSize == 0
                                       ? 0
                                       : static_cast<size_t>(std::min<int64_t>(SIZE_UPLOAD_SLICE, fileSize - offset));
        std::vector<uint8_t> slice(currentSize);
        sourceFile.seek(offset, fslib::Stream::BEGINNING);
        if (currentSize > 0 && !read_exact(sourceFile, slice.data(), currentSize))
        {
            logger::log("Error uploading to Baidu Netdisk: Failed to read source slice.");
            return false;
        }

        std::string returnedMd5{};
        bool uploaded{};
        for (int attempt = 0; attempt < 3 && !uploaded; attempt++)
        {
            uploaded = BaiduNetdisk::upload_slice(
                slice, remotePath, uploadId, part, uploadedBytes, task, returnedMd5);
            if (!uploaded && attempt < 2)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
            }
        }
        if (!uploaded || (!returnedMd5.empty() && returnedMd5 != blockHashes[part]))
        {
            logger::log("Error uploading to Baidu Netdisk: Slice %zu failed validation.", part);
            return false;
        }
        uploadedBytes += currentSize;
    }

    const Parameters createQuery = {{"method", "create"}};
    const Parameters createForm  = {{"path", remotePathString},
                                    {"size", std::to_string(fileSize)},
                                    {"isdir", "0"},
                                    {"rtype", rtype},
                                    {"uploadid", uploadId},
                                    {"block_list", blockListString}};
    std::string createResponse{};
    if (!BaiduNetdisk::perform_post_form(URL_BAIDU_API, ENDPOINT_FILE, createQuery, createForm, createResponse))
    {
        return false;
    }

    json::Object createParser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(createResponse, "upload create", createParser)) { return false; }

    uploadedFile.path = get_json_string(createParser.get(), "path");
    if (uploadedFile.path.empty()) { uploadedFile.path = remotePath; }
    uploadedFile.name = get_json_string(createParser.get(), "server_filename");
    if (uploadedFile.name.empty()) { uploadedFile.name = filename_from_path(uploadedFile.path); }
    uploadedFile.fsId        = get_json_int64(createParser.get(), "fs_id");
    uploadedFile.size        = get_json_int64(createParser.get(), "size", fileSize);
    uploadedFile.isDirectory = false;
    if (task) { task->update_current(static_cast<double>(fileSize)); }
    return true;
}

bool remote::BaiduNetdisk::upload_slice(const std::vector<uint8_t> &slice,
                                        std::string_view remotePath,
                                        std::string_view uploadId,
                                        size_t partNumber,
                                        int64_t progressOffset,
                                        sys::ProgressTask *task,
                                        std::string &md5Out)
{
    const Parameters parameters = {{"method", "upload"},
                                   {"type", "tmpfile"},
                                   {"path", std::string(remotePath)},
                                   {"uploadid", std::string(uploadId)},
                                   {"partseq", std::to_string(partNumber)}};
    const std::string url = BaiduNetdisk::build_url(URL_BAIDU_PCS, ENDPOINT_SUPERFILE2, parameters, true);

    MultipartReader reader{};
    reader.header = "--" + std::string(MULTIPART_BOUNDARY) +
                    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"file\"\r\n"
                    "Content-Type: application/octet-stream\r\n\r\n";
    reader.data           = &slice;
    reader.footer         = "\r\n--" + std::string(MULTIPART_BOUNDARY) + "--\r\n";
    reader.progressOffset = progressOffset;
    reader.task           = task;

    const std::string contentType = "Content-Type: multipart/form-data; boundary=" + std::string(MULTIPART_BOUNDARY);
    curl::HeaderList headers      = curl::new_header_list();
    curl::append_header(headers, contentType);

    std::string response{};
    curl::prepare_post(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, url.c_str());
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, headers.get());
    curl::set_option(m_curl, CURLOPT_READFUNCTION, read_multipart);
    curl::set_option(m_curl, CURLOPT_READDATA, &reader);
    curl::set_option(m_curl,
                     CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(reader.header.size() + slice.size() + reader.footer.size()));
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);

    if (!curl::perform(m_curl)) { return false; }

    json::Object parser{nullptr, json_object_put};
    if (!BaiduNetdisk::response_succeeded(response, "slice upload", parser)) { return false; }
    md5Out = get_json_string(parser.get(), "md5");
    return !md5Out.empty();
}

bool remote::BaiduNetdisk::perform_get(std::string_view baseUrl,
                                       std::string_view endpoint,
                                       const Parameters &parameters,
                                       std::string &response,
                                       bool authenticated,
                                       std::string_view userAgent)
{
    const std::string url = BaiduNetdisk::build_url(baseUrl, endpoint, parameters, authenticated);
    if (url.empty()) { return false; }

    response.clear();
    curl::prepare_get(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, url.c_str());
    if (!userAgent.empty()) { curl::set_option(m_curl, CURLOPT_USERAGENT, userAgent.data()); }
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);
    return curl::perform(m_curl);
}

bool remote::BaiduNetdisk::perform_post_form(std::string_view baseUrl,
                                             std::string_view endpoint,
                                             const Parameters &query,
                                             const Parameters &form,
                                             std::string &response,
                                             bool authenticated)
{
    const std::string url  = BaiduNetdisk::build_url(baseUrl, endpoint, query, authenticated);
    const std::string body = BaiduNetdisk::build_form(form);
    if (url.empty()) { return false; }

    curl::HeaderList headers = curl::new_header_list();
    curl::append_header(headers, "Content-Type: application/x-www-form-urlencoded");

    response.clear();
    curl::prepare_post(m_curl);
    curl::set_option(m_curl, CURLOPT_URL, url.c_str());
    curl::set_option(m_curl, CURLOPT_HTTPHEADER, headers.get());
    curl::set_option(m_curl, CURLOPT_POSTFIELDS, body.c_str());
    curl::set_option(m_curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl::set_option(m_curl, CURLOPT_WRITEFUNCTION, curl::write_response_string);
    curl::set_option(m_curl, CURLOPT_WRITEDATA, &response);
    return curl::perform(m_curl);
}

std::string remote::BaiduNetdisk::build_url(std::string_view baseUrl,
                                            std::string_view endpoint,
                                            const Parameters &parameters,
                                            bool authenticated)
{
    std::string url{baseUrl};
    url.append(endpoint);
    bool hasParameters = url.find('?') != url.npos;

    auto appendParameter = [&](std::string_view key, std::string_view value)
    {
        url.push_back(hasParameters ? '&' : '?');
        hasParameters = true;
        url.append(BaiduNetdisk::escape(key));
        url.push_back('=');
        url.append(BaiduNetdisk::escape(value));
    };

    for (const auto &[key, value] : parameters) { appendParameter(key, value); }
    if (authenticated)
    {
        if (m_accessToken.empty()) { return {}; }
        appendParameter("access_token", m_accessToken);
    }
    return url;
}

std::string remote::BaiduNetdisk::build_form(const Parameters &parameters)
{
    std::string form{};
    for (const auto &[key, value] : parameters)
    {
        if (!form.empty()) { form.push_back('&'); }
        form.append(BaiduNetdisk::escape(key));
        form.push_back('=');
        form.append(BaiduNetdisk::escape(value));
    }
    return form;
}

std::string remote::BaiduNetdisk::escape(std::string_view value)
{
    std::string escaped{};
    if (!curl::escape_string(m_curl, value, escaped)) { return {}; }
    return escaped;
}

bool remote::BaiduNetdisk::response_succeeded(std::string_view response,
                                              std::string_view context,
                                              json::Object &parser,
                                              bool logError,
                                              int *errorNumber)
{
    const std::string responseString{response};
    parser = json::new_object(json_tokener_parse, responseString.c_str());
    if (!parser)
    {
        if (logError)
        {
            logger::log("Baidu Netdisk %s failed: Server response was not valid JSON.", std::string(context).c_str());
        }
        if (errorNumber) { *errorNumber = std::numeric_limits<int>::min(); }
        return false;
    }

    int64_t error{};
    std::string message{};
    json_object *errnoObject = get_json_object(parser.get(), "errno");
    if (errnoObject) { error = json_object_get_int64(errnoObject); }
    if (error == 0)
    {
        json_object *errorCode = get_json_object(parser.get(), "error_code");
        if (errorCode) { error = json_object_get_int64(errorCode); }
    }
    if (error == 0)
    {
        json_object *errorNo = get_json_object(parser.get(), "error_no");
        if (errorNo) { error = json_object_get_int64(errorNo); }
    }

    const std::string oauthError = get_json_string(parser.get(), "error");
    if (!oauthError.empty())
    {
        error   = -1;
        message = oauthError;
        const std::string description = get_json_string(parser.get(), "error_description");
        if (!description.empty()) { message += ": " + description; }
    }
    if (message.empty()) { message = get_json_string(parser.get(), "errmsg"); }
    if (message.empty()) { message = get_json_string(parser.get(), "error_msg"); }

    const long responseCode = curl::get_response_code(m_curl);
    const bool httpSuccess  = responseCode >= 200 && responseCode < 300;
    const bool succeeded    = error == 0 && httpSuccess;
    if (errorNumber)
    {
        *errorNumber = error != 0 ? static_cast<int>(error) : (httpSuccess ? 0 : static_cast<int>(responseCode));
    }

    if (!succeeded && logError)
    {
        logger::log("Baidu Netdisk %s failed: errno=%lli, HTTP=%li, %s",
                    std::string(context).c_str(),
                    static_cast<long long>(error),
                    responseCode,
                    message.empty() ? "No error description." : message.c_str());
    }
    if (error == -6) { m_tokenExpires = 0; }
    return succeeded;
}
