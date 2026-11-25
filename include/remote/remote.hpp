#pragma once
#include "remote/Storage.hpp"
#include "sys/threadpool.hpp"

#include <memory>
#include <string>

namespace data
{
    class TitleInfo;
}

namespace remote
{
    // Remote provider configuration paths used during initialization and sign-in.
    static constexpr std::string_view PATH_BAIDU_NETDISK_CONFIG = "sdmc:/config/JKSV/baidu.json";
    static constexpr std::string_view PATH_BAIDU_NETDISK_LOGOUT = "sdmc:/config/JKSV/baidu.logged_out";

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

    /// @brief Whether a connection, sign-in or sign-out is in progress. Those can't overlap.
    bool is_busy() noexcept;

    /// @brief Whether the last attempt to connect couldn't reach Baidu at all, as opposed to the login being rejected.
    bool connection_failed() noexcept;

    /// @brief Reads the cached Baidu account state without making a network request.
    BaiduAccountStatus get_baidu_account_status();

    /// @brief Starts an explicit Baidu device-code sign-in from the settings screen.
    /// @param forceReauthorize Scan a new QR code even while signed in. The current login stays until the new one is
    /// authorized, so cancelling changes nothing.
    /// @return False if sign in couldn't be started. A message has been shown either way.
    bool begin_baidu_netdisk_sign_in(bool forceReauthorize = false);

    /// @brief Connects again with the saved login, for when the first attempt failed.
    void retry_baidu_netdisk_connection();

    /// @brief Clears local Baidu OAuth tokens while preserving the app credentials and settings.
    bool sign_out_baidu_netdisk();

    /// @brief Enters a game's cloud folder from the root, creating it if needed.
    /// @note The folder name is shared with the background worker. A folder named the way older versions did is
    /// renamed to it the first time.
    bool enter_title_directory(remote::Storage *storage, const data::TitleInfo *titleInfo);

    /// @brief Whether the background worker turned itself off after crashing twice in a row.
    bool background_sync_disabled();

    /// @brief Turns the background worker back on. It takes effect the next time the console boots.
    bool enable_background_sync();

    /// @brief Whether the resident monitor has failed to start the worker several times.
    bool background_launch_failed();

    /// @brief Tells the background worker that JKSV is open, so it stays away from saves until JKSV exits.
    void mark_jksv_running(bool running);
} // namespace remote
