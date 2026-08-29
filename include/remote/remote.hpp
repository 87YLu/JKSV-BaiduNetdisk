#pragma once
#include "remote/Storage.hpp"
#include "sys/threadpool.hpp"

#include <memory>
#include <string>

namespace remote
{
    // Remote provider configuration paths used during initialization and sign-in.
    static constexpr std::string_view PATH_GOOGLE_DRIVE_CONFIG = "sdmc:/config/JKSV/client_secret.json";
    static constexpr std::string_view PATH_BAIDU_NETDISK_CONFIG = "sdmc:/config/JKSV/baidu.json";
    static constexpr std::string_view PATH_BAIDU_NETDISK_LOGOUT = "sdmc:/config/JKSV/baidu.logged_out";
    static constexpr std::string_view PATH_WEBDAV_CONFIG       = "sdmc:/config/JKSV/webdav.json";

    struct BaiduAccountStatus
    {
        bool configured{};
        bool signedIn{};
        std::string displayName{};
    };

    /// @brief Returns whether or not the console has an active internet connection.
    bool has_internet_connection() noexcept;

    /// @brief Initializes the remote service according to the config on the sdmc.
    void initialize(sys::threadpool::JobData jobData);

    /// @brief Returns the pointer to the Storage instance.
    remote::Storage *get_remote_storage() noexcept;

    /// @brief Reads the cached Baidu account state without making a network request.
    BaiduAccountStatus get_baidu_account_status();

    /// @brief Starts an explicit Baidu device-code sign-in from the settings screen.
    bool begin_baidu_netdisk_sign_in();

    /// @brief Clears local Baidu OAuth tokens while preserving the app credentials and settings.
    bool sign_out_baidu_netdisk();
} // namespace remote
