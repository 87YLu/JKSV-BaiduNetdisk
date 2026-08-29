#pragma once
#include "json.hpp"
#include "remote/Storage.hpp"

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remote
{
    /// @brief Baidu Netdisk storage backed by the official Open Platform APIs.
    class BaiduNetdisk final : public remote::Storage
    {
        public:
            /// @brief Loads Baidu Open Platform credentials and cached OAuth tokens from SD.
            explicit BaiduNetdisk(bool authorizeSignedOut = false);

            bool create_directory(std::string_view name) override;
            bool upload_file(const fslib::Path &source,
                             std::string_view name,
                             sys::ProgressTask *task = nullptr) override;
            bool patch_file(remote::Item *file,
                            const fslib::Path &source,
                            sys::ProgressTask *task = nullptr) override;
            bool download_file(const remote::Item *file,
                               const fslib::Path &destination,
                               sys::ProgressTask *task = nullptr) override;
            bool delete_item(const remote::Item *item) override;
            bool rename_item(remote::Item *item, std::string_view newName) override;

            /// @brief Returns true when device-code authorization must be completed.
            bool sign_in_required() const noexcept;

            /// @brief Requests a device code and formats the authorization instructions.
            bool get_sign_in_data(std::string &message,
                                  std::string &deviceCode,
                                  std::time_t &expiration,
                                  int &pollingInterval,
                                  std::vector<uint8_t> &qrImage);

            /// @brief Polls Baidu for completion of device-code authorization.
            bool poll_sign_in(std::string_view deviceCode);

        private:
            using Parameters = std::vector<std::pair<std::string, std::string>>;

            struct FileInfo
            {
                std::string name{};
                std::string path{};
                int64_t fsId{};
                int64_t size{};
                bool isDirectory{};
            };

            std::string m_appKey{};
            std::string m_secretKey{};
            std::string m_accessToken{};
            std::string m_refreshToken{};
            std::string m_basePath{};
            std::string m_accountName{};
            std::time_t m_tokenExpires{};
            bool m_refreshRejected{};
            bool m_signedOut{};
            std::unordered_map<std::string, int64_t> m_fsIds{};

            bool initialize_storage();
            bool request_account_info();
            bool token_is_valid() const noexcept;
            bool ensure_token();
            bool refresh_token();
            bool save_tokens(bool allowLogoutMarker = false);
            bool download_qr_image(std::string_view url, std::vector<uint8_t> &image);

            bool request_listing(int *errorNumber = nullptr, bool logErrors = true);
            bool request_directory_listing(std::string_view directory,
                                           bool recursive,
                                           int *errorNumber = nullptr,
                                           bool logErrors = true);
            bool get_file_info(std::string_view path, FileInfo &fileInfo);
            bool create_directory_path(std::string_view path, FileInfo *fileInfo = nullptr);

            bool upload_file_internal(const fslib::Path &source,
                                      std::string_view remotePath,
                                      bool overwrite,
                                      sys::ProgressTask *task,
                                      FileInfo &uploadedFile);
            bool upload_slice(const std::vector<uint8_t> &slice,
                              std::string_view remotePath,
                              std::string_view uploadId,
                              size_t partNumber,
                              int64_t progressOffset,
                              sys::ProgressTask *task,
                              std::string &md5Out);

            bool perform_get(std::string_view baseUrl,
                             std::string_view endpoint,
                             const Parameters &parameters,
                             std::string &response,
                             bool authenticated = true,
                             std::string_view userAgent = {});
            bool perform_post_form(std::string_view baseUrl,
                                   std::string_view endpoint,
                                   const Parameters &query,
                                   const Parameters &form,
                                   std::string &response,
                                   bool authenticated = true);
            std::string build_url(std::string_view baseUrl,
                                  std::string_view endpoint,
                                  const Parameters &parameters,
                                  bool authenticated);
            std::string build_form(const Parameters &parameters);
            std::string escape(std::string_view value);

            bool response_succeeded(std::string_view response,
                                    std::string_view context,
                                    json::Object &parser,
                                    bool logError = true,
                                    int *errorNumber = nullptr);
    };
} // namespace remote
