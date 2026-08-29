#pragma once

#include "common.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>

struct json_object;

namespace background
{
    class BaiduClient final
    {
        public:
            BaiduClient() = default;
            ~BaiduClient();

            BaiduClient(const BaiduClient &)            = delete;
            BaiduClient &operator=(const BaiduClient &) = delete;

            bool initialize(bool verifyTls);
            bool upload_file(std::string_view localPath, std::string_view remotePath);
            const std::string &last_error() const noexcept;

        private:
            using Parameters = std::vector<std::pair<std::string, std::string>>;

            struct Response
            {
                std::string body{};
                long httpCode{};
                int errorNumber{};
            };

            std::string m_appKey{};
            std::string m_secretKey{};
            std::string m_accessToken{};
            std::string m_refreshToken{};
            std::string m_basePath{"/apps/JKSV"};
            std::string m_lastError{};
            std::time_t m_tokenExpires{};
            CURL *m_curl{};
            bool m_verifyTls{true};

            bool load_config();
            bool save_tokens();
            bool ensure_token();
            bool refresh_token();
            bool ensure_remote_parent(std::string_view remotePath);
            bool create_directory(std::string_view path);
            bool upload_file_once(std::string_view localPath, std::string_view remotePath);
            bool upload_slice(FILE *source,
                              uint64_t offset,
                              size_t length,
                              std::string_view remotePath,
                              std::string_view uploadId,
                              size_t partNumber,
                              std::string &md5Out);

            bool get(std::string_view baseUrl,
                     std::string_view endpoint,
                     const Parameters &parameters,
                     Response &response,
                     bool authenticated = true);
            bool post_form(std::string_view baseUrl,
                           std::string_view endpoint,
                           const Parameters &query,
                           const Parameters &form,
                           Response &response,
                           bool authenticated = true);
            std::string build_url(std::string_view baseUrl,
                                  std::string_view endpoint,
                                  const Parameters &parameters,
                                  bool authenticated);
            std::string build_form(const Parameters &parameters);
            std::string escape(std::string_view value);
            bool parse_response(Response &response,
                                std::string_view context,
                                json_object **parserOut,
                                bool allowAlreadyExists = false);
            void set_error(std::string_view context, const Response *response = nullptr);
    };
} // namespace background
