#include "baidu.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <json-c/json.h>

namespace
{
    constexpr std::string_view URL_BAIDU_API   = "https://pan.baidu.com";
    constexpr std::string_view URL_BAIDU_OAUTH = "https://openapi.baidu.com";
    constexpr std::string_view URL_BAIDU_PCS   = "https://d.pcs.baidu.com";
    constexpr std::string_view ENDPOINT_FILE   = "/rest/2.0/xpan/file";
    constexpr std::string_view ENDPOINT_TOKEN  = "/oauth/2.0/token";
    constexpr std::string_view ENDPOINT_UPLOAD = "/rest/2.0/pcs/superfile2";

    constexpr size_t UPLOAD_SLICE_SIZE = 4 * 1024 * 1024;
    constexpr size_t MAX_UPLOAD_SLICES = 1280;
    constexpr size_t RESPONSE_LIMIT    = 512 * 1024;
    constexpr size_t HASH_BUFFER_SIZE  = 64 * 1024;
    constexpr std::string_view MULTIPART_BOUNDARY = "----------------JKSVBackgroundBoundary";
    bool s_socketReady{};
    bool s_nifmReady{};
    bool s_curlReady{};
    std::array<uint8_t, HASH_BUFFER_SIZE> s_hashBuffer{};

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

    int64_t get_int64(json_object *parent, const char *key, int64_t fallback = 0)
    {
        json_object *value = get_object(parent, key);
        return value ? json_object_get_int64(value) : fallback;
    }

    bool initialize_network()
    {
        if (s_socketReady && s_nifmReady && s_curlReady) { return true; }

        static const SocketInitConfig socketConfig = {
            .tcp_tx_buf_size     = 0x2000,
            .tcp_rx_buf_size     = 0x4000,
            .tcp_tx_buf_max_size = 0x8000,
            .tcp_rx_buf_max_size = 0x10000,
            .udp_tx_buf_size     = 0x800,
            .udp_rx_buf_size     = 0x1000,
            .sb_efficiency       = 1,
            .num_bsd_sessions    = 2,
            .bsd_service_type    = BsdServiceType_User,
        };

        if (!s_socketReady) { s_socketReady = R_SUCCEEDED(socketInitialize(&socketConfig)); }
        if (!s_nifmReady) { s_nifmReady = R_SUCCEEDED(nifmInitialize(NifmServiceType_User)); }
        if (s_socketReady && s_nifmReady && !s_curlReady)
        {
            s_curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        }
        return s_socketReady && s_nifmReady && s_curlReady;
    }

    void shutdown_network()
    {
        // Keeping these services open after a transfer can keep the networking
        // stack active across an otherwise idle/sleep period. A background
        // batch owns the only BaiduClient instance, so tear everything down
        // once the batch ends and initialize it again for the next retry.
        if (s_curlReady)
        {
            curl_global_cleanup();
            s_curlReady = false;
        }
        if (s_nifmReady)
        {
            nifmExit();
            s_nifmReady = false;
        }
        if (s_socketReady)
        {
            socketExit();
            s_socketReady = false;
        }
    }

    uint32_t rotate_left(uint32_t value, uint32_t amount) noexcept
    {
        return (value << amount) | (value >> (32U - amount));
    }

    class Md5 final
    {
        public:
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
                        transform(m_buffer.data());
                        m_bufferSize = 0;
                    }
                }
            }

            std::array<uint8_t, 16> finish() noexcept
            {
                const uint64_t bitLength = m_totalBytes * 8;
                std::array<uint8_t, 64> padding{};
                padding[0] = 0x80;
                const size_t paddingSize = m_bufferSize < 56 ? 56 - m_bufferSize : 120 - m_bufferSize;
                update(padding.data(), paddingSize);

                std::array<uint8_t, 8> lengthBytes{};
                for (size_t index = 0; index < lengthBytes.size(); ++index)
                {
                    lengthBytes[index] = static_cast<uint8_t>((bitLength >> (index * 8)) & 0xFF);
                }
                update(lengthBytes.data(), lengthBytes.size());

                std::array<uint8_t, 16> digest{};
                for (size_t index = 0; index < m_state.size(); ++index)
                {
                    for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
                    {
                        digest[(index * sizeof(uint32_t)) + byte] =
                            static_cast<uint8_t>((m_state[index] >> (byte * 8)) & 0xFF);
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
                    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
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
                for (size_t index = 0; index < words.size(); ++index)
                {
                    const size_t offset = index * sizeof(uint32_t);
                    words[index] = static_cast<uint32_t>(block[offset]) |
                                   (static_cast<uint32_t>(block[offset + 1]) << 8) |
                                   (static_cast<uint32_t>(block[offset + 2]) << 16) |
                                   (static_cast<uint32_t>(block[offset + 3]) << 24);
                }

                uint32_t a = m_state[0];
                uint32_t b = m_state[1];
                uint32_t c = m_state[2];
                uint32_t d = m_state[3];
                for (size_t index = 0; index < TABLE.size(); ++index)
                {
                    uint32_t function{};
                    size_t wordIndex{};
                    if (index < 16)
                    {
                        function = (b & c) | ((~b) & d);
                        wordIndex = index;
                    }
                    else if (index < 32)
                    {
                        function = (d & b) | ((~d) & c);
                        wordIndex = ((5 * index) + 1) % 16;
                    }
                    else if (index < 48)
                    {
                        function = b ^ c ^ d;
                        wordIndex = ((3 * index) + 5) % 16;
                    }
                    else
                    {
                        function = c ^ (b | (~d));
                        wordIndex = (7 * index) % 16;
                    }

                    const uint32_t previousD = d;
                    d = c;
                    c = b;
                    b += rotate_left(a + function + TABLE[index] + words[wordIndex], SHIFT[index]);
                    a = previousD;
                }
                m_state[0] += a;
                m_state[1] += b;
                m_state[2] += c;
                m_state[3] += d;
            }
    };

    std::string md5_string(const std::array<uint8_t, 16> &digest)
    {
        static constexpr std::string_view HEX = "0123456789abcdef";
        std::string output(digest.size() * 2, '0');
        for (size_t index = 0; index < digest.size(); ++index)
        {
            output[index * 2] = HEX[digest[index] >> 4];
            output[(index * 2) + 1] = HEX[digest[index] & 0x0F];
        }
        return output;
    }

    std::string normalize_remote_path(std::string_view input)
    {
        std::string path{input};
        if (path.empty()) { return {}; }
        if (path.front() != '/') { path.insert(path.begin(), '/'); }
        while (path.find("//") != std::string::npos) { path.erase(path.find("//"), 1); }
        while (path.size() > 1 && path.back() == '/') { path.pop_back(); }

        for (size_t begin = 1; begin <= path.size();)
        {
            const size_t end = path.find('/', begin);
            const std::string_view segment{
                path.data() + begin, (end == std::string::npos ? path.size() : end) - begin};
            if (segment.empty() || segment == "." || segment == "..") { return {}; }
            if (end == std::string::npos) { break; }
            begin = end + 1;
        }
        return path;
    }

    std::string parent_path(std::string_view path)
    {
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) { return "/"; }
        return std::string(path.substr(0, slash));
    }

    struct ResponseWriter
    {
        std::string *output{};
        bool overflow{};
    };

    size_t write_response(char *data, size_t size, size_t count, void *userdata)
    {
        auto *writer = static_cast<ResponseWriter *>(userdata);
        if (!writer || !writer->output || (count > 0 && size > std::numeric_limits<size_t>::max() / count))
        {
            return 0;
        }
        const size_t length = size * count;
        if (writer->output->size() > RESPONSE_LIMIT || length > RESPONSE_LIMIT - writer->output->size())
        {
            writer->overflow = true;
            return 0;
        }
        writer->output->append(data, length);
        return length;
    }

    struct MultipartReader
    {
        FILE *source{};
        std::string header{};
        std::string footer{};
        size_t headerOffset{};
        size_t footerOffset{};
        size_t remaining{};
    };

    size_t read_multipart(char *buffer, size_t size, size_t count, void *userdata)
    {
        auto *reader = static_cast<MultipartReader *>(userdata);
        if (!reader || !reader->source || (count > 0 && size > std::numeric_limits<size_t>::max() / count))
        {
            return CURL_READFUNC_ABORT;
        }

        const size_t capacity = size * count;
        size_t written{};
        auto copy_text = [&](const std::string &source, size_t &offset)
        {
            const size_t amount = std::min(capacity - written, source.size() - offset);
            std::memcpy(buffer + written, source.data() + offset, amount);
            offset += amount;
            written += amount;
        };

        if (reader->headerOffset < reader->header.size()) { copy_text(reader->header, reader->headerOffset); }
        if (written < capacity && reader->headerOffset == reader->header.size() && reader->remaining > 0)
        {
            const size_t wanted = std::min(capacity - written, reader->remaining);
            const size_t actual = std::fread(buffer + written, 1, wanted, reader->source);
            reader->remaining -= actual;
            written += actual;
            if (actual != wanted && std::ferror(reader->source)) { return CURL_READFUNC_ABORT; }
        }
        if (written < capacity && reader->headerOffset == reader->header.size() && reader->remaining == 0 &&
            reader->footerOffset < reader->footer.size())
        {
            copy_text(reader->footer, reader->footerOffset);
        }
        return written;
    }

    bool write_config_atomically(json_object *config, std::string_view suffix)
    {
        const char *contents = json_object_to_json_string_ext(config, JSON_C_TO_STRING_PRETTY);
        if (!contents) { return false; }

        const size_t length = std::strlen(contents);
        const std::string temporary = std::string(background::CONFIG_PATH) + std::string(suffix);
        FILE *file = std::fopen(temporary.c_str(), "wb");
        bool written{};
        if (file)
        {
            written = std::fwrite(contents, 1, length, file) == length && std::fflush(file) == 0;
            std::fclose(file);
        }
        if (!written)
        {
            background::remove_file(temporary);
            return false;
        }
        if (!background::atomic_replace(temporary, background::CONFIG_PATH))
        {
            background::remove_file(temporary);
            return false;
        }
        return true;
    }
} // namespace

background::BaiduClient::~BaiduClient()
{
    if (m_curl)
    {
        curl_easy_cleanup(m_curl);
        m_curl = nullptr;
    }
    shutdown_network();
}

bool background::BaiduClient::initialize(bool verifyTls)
{
    m_verifyTls = verifyTls;
    if (!load_config()) { return false; }
    if (!initialize_network())
    {
        m_lastError = "network services unavailable";
        return false;
    }
    m_curl = curl_easy_init();
    if (!m_curl)
    {
        m_lastError = "curl initialization failed";
        return false;
    }
    return ensure_token();
}

const std::string &background::BaiduClient::last_error() const noexcept { return m_lastError; }

bool background::BaiduClient::load_config()
{
    if (background::file_exists(background::LOGOUT_MARKER_PATH))
    {
        m_lastError = "Baidu Netdisk is signed out";
        return false;
    }

    json_object *config = json_object_from_file(background::CONFIG_PATH);
    if (!config)
    {
        m_lastError = "missing or invalid config/JKSV/baidu.json";
        return false;
    }

    json_object *signedOutValue{};
    const bool signedOut = json_object_object_get_ex(config, "signed_out", &signedOutValue) &&
                           json_object_get_boolean(signedOutValue);
    m_appKey       = get_string(config, "app_key");
    m_secretKey    = get_string(config, "secret_key");
    m_accessToken  = get_string(config, "access_token");
    m_refreshToken = get_string(config, "refresh_token");
    m_tokenExpires = static_cast<std::time_t>(get_int64(config, "expires_at"));
    const std::string configuredBase = get_string(config, "basepath");
    m_basePath = normalize_remote_path(configuredBase.empty() ? "/apps/JKSV" : configuredBase);
    json_object_put(config);

    if (signedOut)
    {
        m_lastError = "Baidu Netdisk is signed out";
        return false;
    }

    if (m_appKey.empty() || m_secretKey.empty())
    {
        m_lastError = "app_key or secret_key is missing";
        return false;
    }
    if (m_basePath.empty())
    {
        m_lastError = "basepath is invalid";
        return false;
    }
    return true;
}

bool background::BaiduClient::save_tokens()
{
    if (background::file_exists(background::LOGOUT_MARKER_PATH))
    {
        m_lastError = "Baidu Netdisk was signed out while refreshing OAuth tokens";
        return false;
    }

    json_object *config = json_object_from_file(background::CONFIG_PATH);
    if (!config)
    {
        m_lastError = "unable to reload token configuration";
        return false;
    }
    json_object *signedOutValue{};
    const bool signedOut = json_object_object_get_ex(config, "signed_out", &signedOutValue) &&
                           json_object_get_boolean(signedOutValue);
    if (signedOut)
    {
        json_object_put(config);
        m_lastError = "Baidu Netdisk was signed out while refreshing OAuth tokens";
        return false;
    }

    json_object_object_add(config, "access_token", json_object_new_string(m_accessToken.c_str()));
    json_object_object_add(config, "refresh_token", json_object_new_string(m_refreshToken.c_str()));
    json_object_object_add(config, "expires_at", json_object_new_int64(m_tokenExpires));
    json_object_object_add(config, "signed_out", json_object_new_boolean(false));

    const bool written = write_config_atomically(config, ".tmp");
    json_object_put(config);

    if (!written)
    {
        m_lastError = "unable to write refreshed OAuth tokens";
        return false;
    }

    // Logout creates the marker before clearing baidu.json. Check again after
    // replacement so an in-flight refresh cannot resurrect the old login.
    if (background::file_exists(background::LOGOUT_MARKER_PATH))
    {
        json_object *signedOutConfig = json_object_from_file(background::CONFIG_PATH);
        if (signedOutConfig)
        {
            json_object_object_add(signedOutConfig, "access_token", json_object_new_string(""));
            json_object_object_add(signedOutConfig, "refresh_token", json_object_new_string(""));
            json_object_object_add(signedOutConfig, "expires_at", json_object_new_int64(0));
            json_object_object_add(signedOutConfig, "account_name", json_object_new_string(""));
            json_object_object_add(signedOutConfig, "signed_out", json_object_new_boolean(true));
            write_config_atomically(signedOutConfig, ".logout.tmp");
            json_object_put(signedOutConfig);
        }
        m_lastError = "Baidu Netdisk was signed out while refreshing OAuth tokens";
        return false;
    }
    return true;
}

bool background::BaiduClient::ensure_token()
{
    if (!m_accessToken.empty() && std::time(nullptr) < m_tokenExpires - 90) { return true; }
    return refresh_token();
}

bool background::BaiduClient::refresh_token()
{
    if (m_refreshToken.empty())
    {
        m_lastError = "OAuth refresh token is missing; open JKSV and scan the login QR code";
        return false;
    }

    const Parameters parameters = {{"grant_type", "refresh_token"},
                                   {"refresh_token", m_refreshToken},
                                   {"client_id", m_appKey},
                                   {"client_secret", m_secretKey}};
    Response response{};
    if (!get(URL_BAIDU_OAUTH, ENDPOINT_TOKEN, parameters, response, false)) { return false; }

    json_object *parser{};
    if (!parse_response(response, "token refresh", &parser)) { return false; }
    const std::string accessToken  = get_string(parser, "access_token");
    const std::string refreshToken = get_string(parser, "refresh_token");
    const int64_t expiresIn        = get_int64(parser, "expires_in");
    json_object_put(parser);
    if (accessToken.empty() || expiresIn <= 0)
    {
        m_lastError = "token refresh returned malformed data";
        return false;
    }

    m_accessToken = accessToken;
    if (!refreshToken.empty()) { m_refreshToken = refreshToken; }
    m_tokenExpires = std::time(nullptr) + expiresIn;
    return save_tokens();
}

std::string background::BaiduClient::escape(std::string_view value)
{
    char *escaped = curl_easy_escape(m_curl, value.data(), static_cast<int>(value.size()));
    if (!escaped) { return {}; }
    std::string output{escaped};
    curl_free(escaped);
    return output;
}

std::string background::BaiduClient::build_url(std::string_view baseUrl,
                                                std::string_view endpoint,
                                                const Parameters &parameters,
                                                bool authenticated)
{
    std::string url{baseUrl};
    url.append(endpoint);
    bool hasParameters = url.find('?') != std::string::npos;
    auto append = [&](std::string_view key, std::string_view value)
    {
        url.push_back(hasParameters ? '&' : '?');
        hasParameters = true;
        url.append(escape(key));
        url.push_back('=');
        url.append(escape(value));
    };
    for (const auto &[key, value] : parameters) { append(key, value); }
    if (authenticated)
    {
        if (m_accessToken.empty()) { return {}; }
        append("access_token", m_accessToken);
    }
    return url;
}

std::string background::BaiduClient::build_form(const Parameters &parameters)
{
    std::string form{};
    for (const auto &[key, value] : parameters)
    {
        if (!form.empty()) { form.push_back('&'); }
        form.append(escape(key));
        form.push_back('=');
        form.append(escape(value));
    }
    return form;
}

bool background::BaiduClient::get(std::string_view baseUrl,
                                  std::string_view endpoint,
                                  const Parameters &parameters,
                                  Response &response,
                                  bool authenticated)
{
    const std::string url = build_url(baseUrl, endpoint, parameters, authenticated);
    if (url.empty())
    {
        m_lastError = "unable to build request URL";
        return false;
    }

    response = {};
    ResponseWriter writer{.output = &response.body};
    curl_easy_reset(m_curl);
    curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(m_curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(m_curl, CURLOPT_USERAGENT, "JKSV-Baidu-Background/1.0");
    curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, m_verifyTls ? 2L : 0L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, m_verifyTls ? 1L : 0L);
    curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &writer);

    const CURLcode result = curl_easy_perform(m_curl);
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &response.httpCode);
    if (result != CURLE_OK)
    {
        m_lastError = writer.overflow ? "server response exceeded safety limit"
                                      : std::string("network request failed: ") + curl_easy_strerror(result);
        return false;
    }
    return true;
}

bool background::BaiduClient::post_form(std::string_view baseUrl,
                                        std::string_view endpoint,
                                        const Parameters &query,
                                        const Parameters &form,
                                        Response &response,
                                        bool authenticated)
{
    const std::string url  = build_url(baseUrl, endpoint, query, authenticated);
    const std::string body = build_form(form);
    if (url.empty())
    {
        m_lastError = "unable to build request URL";
        return false;
    }

    response = {};
    ResponseWriter writer{.output = &response.body};
    curl_slist *headers{};
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_reset(m_curl);
    curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(m_curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(m_curl, CURLOPT_USERAGENT, "JKSV-Baidu-Background/1.0");
    curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, m_verifyTls ? 2L : 0L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, m_verifyTls ? 1L : 0L);
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &writer);

    const CURLcode result = curl_easy_perform(m_curl);
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &response.httpCode);
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
    {
        m_lastError = writer.overflow ? "server response exceeded safety limit"
                                      : std::string("network request failed: ") + curl_easy_strerror(result);
        return false;
    }
    return true;
}

void background::BaiduClient::set_error(std::string_view context, const Response *response)
{
    char details[256]{};
    if (response)
    {
        std::snprintf(details,
                      sizeof(details),
                      "%.*s failed (errno=%d, HTTP=%ld)",
                      static_cast<int>(context.size()),
                      context.data(),
                      response->errorNumber,
                      response->httpCode);
    }
    else
    {
        std::snprintf(
            details, sizeof(details), "%.*s failed", static_cast<int>(context.size()), context.data());
    }
    m_lastError = details;
}

bool background::BaiduClient::parse_response(Response &response,
                                              std::string_view context,
                                              json_object **parserOut,
                                              bool allowAlreadyExists)
{
    json_object *parser = json_tokener_parse(response.body.c_str());
    if (!parser)
    {
        response.errorNumber = std::numeric_limits<int>::min();
        set_error(context, &response);
        return false;
    }

    int64_t error = get_int64(parser, "errno");
    if (error == 0) { error = get_int64(parser, "error_code"); }
    if (error == 0) { error = get_int64(parser, "error_no"); }
    if (!get_string(parser, "error").empty() && error == 0) { error = -1; }
    response.errorNumber = static_cast<int>(error);

    const bool httpSuccess = response.httpCode >= 200 && response.httpCode < 300;
    const bool succeeded = httpSuccess && (error == 0 || (allowAlreadyExists && error == -8));
    if (!succeeded)
    {
        if (error == -6) { m_tokenExpires = 0; }
        set_error(context, &response);
        json_object_put(parser);
        return false;
    }

    if (parserOut) { *parserOut = parser; }
    else { json_object_put(parser); }
    return true;
}

bool background::BaiduClient::create_directory(std::string_view path)
{
    const Parameters query = {{"method", "create"}};
    const Parameters form  = {{"path", std::string(path)}, {"isdir", "1"}, {"rtype", "0"}};
    Response response{};
    if (!post_form(URL_BAIDU_API, ENDPOINT_FILE, query, form, response)) { return false; }
    return parse_response(response, "create directory", nullptr, true);
}

bool background::BaiduClient::ensure_remote_parent(std::string_view remotePath)
{
    const std::string parent = normalize_remote_path(parent_path(remotePath));
    if (parent.empty() || parent == "/") { return !parent.empty(); }

    std::string current{};
    for (size_t begin = 1; begin <= parent.size();)
    {
        const size_t end = parent.find('/', begin);
        const std::string segment =
            parent.substr(begin, (end == std::string::npos ? parent.size() : end) - begin);
        current.push_back('/');
        current.append(segment);
        if (current != "/apps" && !create_directory(current)) { return false; }
        if (end == std::string::npos) { break; }
        begin = end + 1;
    }
    return true;
}

bool background::BaiduClient::upload_file(std::string_view localPath, std::string_view remotePath)
{
    const std::string normalized = normalize_remote_path(remotePath);
    if (normalized.empty())
    {
        m_lastError = "remote upload path is invalid";
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!ensure_token()) { return false; }
        if (ensure_remote_parent(normalized) && upload_file_once(localPath, normalized)) { return true; }
        if (m_tokenExpires != 0 || m_refreshToken.empty()) { break; }
    }
    return false;
}

bool background::BaiduClient::upload_slice(FILE *source,
                                            uint64_t offset,
                                            size_t length,
                                            std::string_view remotePath,
                                            std::string_view uploadId,
                                            size_t partNumber,
                                            std::string &md5Out)
{
    if (std::fseek(source, static_cast<long>(offset), SEEK_SET) != 0)
    {
        m_lastError = "unable to seek upload source";
        return false;
    }

    const Parameters parameters = {{"method", "upload"},
                                   {"type", "tmpfile"},
                                   {"path", std::string(remotePath)},
                                   {"uploadid", std::string(uploadId)},
                                   {"partseq", std::to_string(partNumber)}};
    const std::string url = build_url(URL_BAIDU_PCS, ENDPOINT_UPLOAD, parameters, true);
    if (url.empty())
    {
        m_lastError = "unable to build slice upload URL";
        return false;
    }

    MultipartReader reader{};
    reader.source = source;
    reader.header = "--" + std::string(MULTIPART_BOUNDARY) +
                    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"file\"\r\n"
                    "Content-Type: application/octet-stream\r\n\r\n";
    reader.footer    = "\r\n--" + std::string(MULTIPART_BOUNDARY) + "--\r\n";
    reader.remaining = length;

    Response response{};
    ResponseWriter writer{.output = &response.body};
    curl_slist *headers{};
    const std::string contentType =
        "Content-Type: multipart/form-data; boundary=" + std::string(MULTIPART_BOUNDARY);
    headers = curl_slist_append(headers, contentType.c_str());
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_reset(m_curl);
    curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(m_curl, CURLOPT_USERAGENT, "JKSV-Baidu-Background/1.0");
    curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, m_verifyTls ? 2L : 0L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, m_verifyTls ? 1L : 0L);
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(m_curl, CURLOPT_READFUNCTION, read_multipart);
    curl_easy_setopt(m_curl, CURLOPT_READDATA, &reader);
    curl_easy_setopt(m_curl,
                     CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(reader.header.size() + length + reader.footer.size()));
    curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &writer);

    const CURLcode result = curl_easy_perform(m_curl);
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &response.httpCode);
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
    {
        m_lastError = writer.overflow ? "slice response exceeded safety limit"
                                      : std::string("slice upload failed: ") + curl_easy_strerror(result);
        return false;
    }

    json_object *parser{};
    if (!parse_response(response, "slice upload", &parser)) { return false; }
    md5Out = get_string(parser, "md5");
    json_object_put(parser);
    if (md5Out.empty())
    {
        m_lastError = "slice upload response is missing md5";
        return false;
    }
    return true;
}

bool background::BaiduClient::upload_file_once(std::string_view localPath, std::string_view remotePath)
{
    const std::string sourcePath{localPath};
    struct stat info{};
    if (::stat(sourcePath.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
    {
        m_lastError = "local backup file is unavailable";
        return false;
    }

    const uint64_t fileSize = static_cast<uint64_t>(info.st_size);
    const uint64_t maximumSize = static_cast<uint64_t>(UPLOAD_SLICE_SIZE) * MAX_UPLOAD_SLICES;
    if (fileSize > maximumSize)
    {
        m_lastError = "backup exceeds the 5 GiB background upload limit";
        return false;
    }

    using FilePointer = std::unique_ptr<FILE, decltype(&std::fclose)>;
    FilePointer source{std::fopen(sourcePath.c_str(), "rb"), std::fclose};
    if (!source)
    {
        m_lastError = "unable to open local backup file";
        return false;
    }

    const size_t blockCount = static_cast<size_t>(
        std::max<uint64_t>(1, (fileSize + UPLOAD_SLICE_SIZE - 1) / UPLOAD_SLICE_SIZE));
    std::vector<std::string> blockHashes{};
    blockHashes.reserve(blockCount);
    Md5 contentHasher{};
    Md5 firstSliceHasher{};
    size_t firstSliceRemaining = 256 * 1024;
    uint64_t offset{};

    for (size_t block = 0; block < blockCount; ++block)
    {
        Md5 blockHasher{};
        size_t blockRemaining =
            fileSize == 0 ? 0 : static_cast<size_t>(std::min<uint64_t>(UPLOAD_SLICE_SIZE, fileSize - offset));
        while (blockRemaining > 0)
        {
            const size_t wanted = std::min(blockRemaining, s_hashBuffer.size());
            const size_t actual = std::fread(s_hashBuffer.data(), 1, wanted, source.get());
            if (actual != wanted)
            {
                m_lastError = "unable to read backup while hashing";
                return false;
            }
            blockHasher.update(s_hashBuffer.data(), actual);
            contentHasher.update(s_hashBuffer.data(), actual);
            const size_t firstAmount = std::min(firstSliceRemaining, actual);
            firstSliceHasher.update(s_hashBuffer.data(), firstAmount);
            firstSliceRemaining -= firstAmount;
            blockRemaining -= actual;
            offset += actual;
        }
        blockHashes.push_back(md5_string(blockHasher.finish()));
    }

    const std::string contentMd5 = md5_string(contentHasher.finish());
    const std::string sliceMd5   = md5_string(firstSliceHasher.finish());
    json_object *blockList = json_object_new_array();
    if (!blockList)
    {
        m_lastError = "unable to allocate block list";
        return false;
    }
    for (const std::string &hash : blockHashes)
    {
        json_object_array_add(blockList, json_object_new_string(hash.c_str()));
    }
    const std::string blockListText =
        json_object_to_json_string_ext(blockList, JSON_C_TO_STRING_PLAIN);

    const Parameters precreateQuery = {{"method", "precreate"}};
    const Parameters precreateForm = {{"path", std::string(remotePath)},
                                      {"size", std::to_string(fileSize)},
                                      {"isdir", "0"},
                                      {"autoinit", "1"},
                                      {"rtype", "3"},
                                      {"block_list", blockListText},
                                      {"content-md5", contentMd5},
                                      {"slice-md5", sliceMd5}};
    Response precreateResponse{};
    if (!post_form(URL_BAIDU_API, ENDPOINT_FILE, precreateQuery, precreateForm, precreateResponse))
    {
        json_object_put(blockList);
        return false;
    }

    json_object *precreate{};
    if (!parse_response(precreateResponse, "upload precreate", &precreate))
    {
        json_object_put(blockList);
        return false;
    }
    if (get_int64(precreate, "return_type") == 2)
    {
        json_object_put(precreate);
        json_object_put(blockList);
        return true;
    }

    const std::string uploadId = get_string(precreate, "uploadid");
    if (uploadId.empty())
    {
        json_object_put(precreate);
        json_object_put(blockList);
        m_lastError = "upload precreate response is missing uploadid";
        return false;
    }

    std::vector<size_t> requiredParts{};
    json_object *requiredList = get_object(precreate, "block_list");
    if (requiredList && json_object_is_type(requiredList, json_type_array))
    {
        const size_t count = json_object_array_length(requiredList);
        requiredParts.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            const int64_t part = json_object_get_int64(json_object_array_get_idx(requiredList, index));
            if (part >= 0 && static_cast<size_t>(part) < blockHashes.size())
            {
                requiredParts.push_back(static_cast<size_t>(part));
            }
        }
    }
    else
    {
        requiredParts.resize(blockHashes.size());
        for (size_t index = 0; index < requiredParts.size(); ++index) { requiredParts[index] = index; }
    }
    json_object_put(precreate);

    for (const size_t part : requiredParts)
    {
        const uint64_t partOffset = static_cast<uint64_t>(part) * UPLOAD_SLICE_SIZE;
        const size_t length = fileSize == 0
                                  ? 0
                                  : static_cast<size_t>(
                                        std::min<uint64_t>(UPLOAD_SLICE_SIZE, fileSize - partOffset));
        bool uploaded{};
        std::string returnedMd5{};
        for (int attempt = 0; attempt < 3 && !uploaded; ++attempt)
        {
            uploaded = upload_slice(
                source.get(), partOffset, length, remotePath, uploadId, part, returnedMd5);
            if (!uploaded && attempt < 2) { background::sleep_seconds(1U << attempt); }
        }
        if (!uploaded || returnedMd5 != blockHashes[part])
        {
            json_object_put(blockList);
            if (uploaded) { m_lastError = "uploaded slice failed MD5 verification"; }
            return false;
        }
    }

    const Parameters createQuery = {{"method", "create"}};
    const Parameters createForm = {{"path", std::string(remotePath)},
                                   {"size", std::to_string(fileSize)},
                                   {"isdir", "0"},
                                   {"rtype", "3"},
                                   {"uploadid", uploadId},
                                   {"block_list", blockListText}};
    Response createResponse{};
    const bool requested =
        post_form(URL_BAIDU_API, ENDPOINT_FILE, createQuery, createForm, createResponse);
    json_object_put(blockList);
    if (!requested) { return false; }
    return parse_response(createResponse, "upload create", nullptr);
}
