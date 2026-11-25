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
            /// @brief What one poll of a device-code sign-in found.
            enum class SignInPoll
            {
                Pending,    ///< Not scanned yet, or the request didn't go through. Keep polling.
                SlowDown,   ///< Polling too often. Keep polling, but wait longer.
                Expired,    ///< The QR code expired. A new one is needed.
                Authorized, ///< Signed in. The tokens are saved.
                Failed      ///< Denied, or the tokens couldn't be saved. Polling again won't help.
            };

            /// @brief Loads Baidu Open Platform credentials and cached OAuth tokens from SD.
            /// @param authorizeSignedOut Load even after a sign-out, for signing in again.
            /// @param connect Whether to check the saved login with Baidu. Not needed when about to scan a new QR code.
            explicit BaiduNetdisk(bool authorizeSignedOut = false, bool connect = true);

            bool create_directory(std::string_view name) override;
            bool refresh_directory() override;
            bool supports_directory_refresh() const noexcept override { return true; }
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
            SignInPoll poll_sign_in(std::string_view deviceCode);

            /// @brief Whether the last attempt to connect failed because Baidu couldn't be reached at all.
            bool connection_failed() const noexcept { return !m_isInitialized && m_connectionFailed; }

        private:
            using Parameters = std::vector<std::pair<std::string, std::string>>;

            struct FileInfo
            {
                std::string name{};
                std::string path{};
                int64_t fsId{};
                int64_t size{};
                int64_t modifiedTime{};
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
            bool m_verifyTls{true};
            bool m_connectionFailed{};
            std::unordered_map<std::string, int64_t> m_fsIds{};

            /// @brief Runs an operation again after a token refresh if Baidu rejected the access token partway through.
            template <typename Operation>
            bool retry_after_token_refresh(Operation operation);

            bool create_directory_once(std::string_view name);
            bool upload_file_once(const fslib::Path &source, std::string_view name, sys::ProgressTask *task);
            bool patch_file_once(remote::Item *file, const fslib::Path &source, sys::ProgressTask *task);
            bool download_file_once(const remote::Item *file, const fslib::Path &destination, sys::ProgressTask *task);
            bool delete_item_once(const remote::Item *item);
            bool rename_item_once(remote::Item *item, std::string_view newName);

            bool initialize_storage();
            bool request_account_info();
            bool token_is_valid() const noexcept;
            bool ensure_token();
            bool refresh_token();
            bool reload_tokens();
            bool save_tokens(bool allowLogoutMarker = false);
            void secure_request(bool allowHttpRedirects = false, bool alwaysVerify = false);
            std::string locate_upload_host(std::string_view remotePath, std::string_view uploadId);
            bool download_qr_image(std::string_view url, std::vector<uint8_t> &image);

            bool request_listing(int *errorNumber = nullptr, bool logErrors = true);
            bool request_directory_listing(std::string_view directory,
                                           int *errorNumber = nullptr,
                                           bool logErrors = true);
            bool get_file_info(std::string_view path, FileInfo &fileInfo);
            bool verify_uploaded_file(std::string_view path, int64_t size, FileInfo &fileInfo);
            bool create_directory_path(std::string_view path, FileInfo *fileInfo = nullptr);

            bool upload_file_internal(const fslib::Path &source,
                                      std::string_view remotePath,
                                      bool overwrite,
                                      sys::ProgressTask *task,
                                      FileInfo &uploadedFile);
            bool upload_slice(const std::vector<uint8_t> &slice,
                              std::string_view host,
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
