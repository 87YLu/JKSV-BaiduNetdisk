#include "appstates/SettingsState.hpp"

#include "appstates/BlacklistEditState.hpp"
#include "appstates/ConfirmState.hpp"
#include "appstates/MainMenuState.hpp"
#include "appstates/MessageState.hpp"
#include "config/config.hpp"
#include "data/data.hpp"
#include "error.hpp"
#include "fs/fs.hpp"
#include "fslib.hpp"
#include "graphics/colors.hpp"
#include "input.hpp"
#include "keyboard/keyboard.hpp"
#include "logging/logger.hpp"
#include "remote/remote.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "ui/PopMessageManager.hpp"

#include <algorithm>
#include <array>

namespace
{
    /// @brief Name of the target all "secondary" states share.
    constexpr std::string_view SECONDARY_TARGET = "SecondaryTarget";

    /// @brief This is used to place "NULL" keys in our local array
    constexpr std::string_view CONFIG_KEY_NULL = "NULL";

    /// @brief This makes it easier to work with key indexes. Anything NULL is something that is not easily toggled.
    constexpr std::array<std::string_view, 25> CONFIG_KEY_ARRAY = {CONFIG_KEY_NULL,
                                                                   CONFIG_KEY_NULL,
                                                                   config::keys::INCLUDE_DEVICE_SAVES,
                                                                   config::keys::AUTO_BACKUP_ON_RESTORE,
                                                                   config::keys::AUTO_NAME_BACKUPS,
                                                                   config::keys::AUTO_UPLOAD,
                                                                   config::keys::KEEP_LOCAL_BACKUPS,
                                                                   config::keys::USE_TITLE_IDS,
                                                                   config::keys::ENGLISH_SAFE_TITLES,
                                                                   config::keys::HOLD_FOR_DELETION,
                                                                   config::keys::HOLD_FOR_RESTORATION,
                                                                   config::keys::HOLD_FOR_OVERWRITE,
                                                                   config::keys::ONLY_LIST_MOUNTABLE,
                                                                   config::keys::LIST_ACCOUNT_SYS_SAVES,
                                                                   config::keys::ALLOW_WRITING_TO_SYSTEM,
                                                                   config::keys::EXPORT_TO_ZIP,
                                                                   config::keys::ZIP_COMPRESSION_LEVEL,
                                                                   config::keys::TITLE_SORT_TYPE,
                                                                   config::keys::JKSM_TEXT_MODE,
                                                                   config::keys::SHOW_DEVICE_USER,
                                                                   config::keys::SHOW_BCAT_USER,
                                                                   config::keys::SHOW_CACHE_USER,
                                                                   config::keys::SHOW_SYSTEM_USER,
                                                                   config::keys::ENABLE_TRASH_BIN,
                                                                   CONFIG_KEY_NULL};

    /// @brief These are the indexes used for case indexing.
    enum CaseIndexes
    {
        ChangeWorkDir = 0,
        EditBlacklist = 1,
        CycleZip      = 16,
        CycleSortType = 17,
        ToggleJKSM    = 18,
        ToggleTrash   = 23,
        CycleScaling  = 24,
        BaiduAccount  = 25,
        BaiduSignOut  = 26,
        BaiduSignIn   = 27,
        BackgroundSync = 28
    };

    enum BaiduStringIndexes
    {
        BaiduAccountFormat       = 3,
        BaiduSignOutOption       = 4,
        BaiduNotConfigured       = 5,
        BaiduSignedOut           = 6,
        BaiduNameUnavailable     = 7,
        BaiduAccountDescription  = 8,
        BaiduSignOutDescription  = 9,
        BaiduSignOutConfirmation = 10,
        BaiduSignOutStatus       = 11,
        BaiduSignOutSuccess      = 12,
        BaiduSignOutFailed       = 13,
        BaiduSignInStartFailed   = 14,
        BaiduNoSignedInAccount   = 15,
        BaiduSignInOption        = 16,
        BaiduSignInDescription   = 17,
        BaiduBusy                = 24,
        BaiduNotConnectedFormat  = 25,
        BackgroundSyncFormat     = 28,
        BackgroundSyncNormal     = 29,
        BackgroundSyncDisabled   = 30,
        BackgroundSyncEnabled    = 31,
        BackgroundSyncDescription = 34
    };

    const char *get_baidu_string(int index, const char *fallback)
    {
        const char *translated = strings::get_by_name(strings::names::BAIDU_NETDISK, index);
        return translated ? translated : fallback;
    }

    void baidu_sign_out_task(sys::threadpool::JobData taskData)
    {
        auto castData  = std::static_pointer_cast<sys::Task::DataStruct>(taskData);
        sys::Task *task = castData->task;
        task->set_status(get_baidu_string(BaiduSignOutStatus, "Signing out of Baidu Netdisk..."));

        const bool success = remote::sign_out_baidu_netdisk();
        const char *message = success
                                  ? get_baidu_string(BaiduSignOutSuccess, "Signed out of Baidu Netdisk.")
                                  : get_baidu_string(BaiduSignOutFailed, "Failed to sign out of Baidu Netdisk.");
        ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS, message);
        task->complete();
    }

} // namespace

//                      ---- Construction ----

SettingsState::SettingsState()
    : m_settingsMenu(ui::Menu::create(32, 10, 1000, 23, 555))
    , m_controlGuide(ui::ControlGuide::create(strings::get_by_name(strings::names::CONTROL_GUIDES, 3)))
    , m_renderTarget(sdl::TextureManager::load(SECONDARY_TARGET, 1080, 555, SDL_TEXTUREACCESS_TARGET))
{
    SettingsState::load_settings_menu();
    SettingsState::load_extra_strings();
    SettingsState::update_menu_options();
}

//                      ---- Public functions ----

void SettingsState::update()
{
    const bool hasFocus     = BaseState::has_focus();
    const bool aPressed     = input::button_pressed(HidNpadButton_A);
    const bool bPressed     = input::button_pressed(HidNpadButton_B);
    const bool xPressed     = input::button_pressed(HidNpadButton_X);
    const bool minusPressed = input::button_pressed(HidNpadButton_Minus);

    const bool busy      = remote::is_busy();
    const bool connected = remote::get_remote_storage() != nullptr;
    if (m_refreshBaiduStatus || busy != m_lastBusy || connected != m_lastConnected)
    {
        SettingsState::update_baidu_account_option();
    }

    m_settingsMenu->update(hasFocus);
    if (aPressed) { SettingsState::toggle_options(); }
    else if (xPressed) { SettingsState::reset_settings(); }
    else if (minusPressed) { SettingsState::create_push_description_message(); }
    else if (bPressed) { BaseState::deactivate(); }

    m_controlGuide->update(hasFocus);
}

void SettingsState::sub_update()
{
    m_refreshBaiduStatus = true;
    m_controlGuide->sub_update();
}

void SettingsState::render()
{
    const bool hasFocus = BaseState::has_focus();

    m_renderTarget->clear(colors::TRANSPARENT);
    m_settingsMenu->render(m_renderTarget, hasFocus);
    m_renderTarget->render(sdl::Texture::Null, 201, 91);
    m_controlGuide->render(sdl::Texture::Null, hasFocus);
}

//                      ---- Private functions ----

void SettingsState::load_settings_menu()
{
    const char *option{};
    for (int i = 0; (option = strings::get_by_name(strings::names::SETTINGS_MENU, i)); i++)
    {
        m_settingsMenu->add_option(option);
    }

    m_settingsMenu->add_option(
        get_baidu_string(BaiduAccountFormat, "Baidu Netdisk account: %s"));
    m_settingsMenu->add_option(
        get_baidu_string(BaiduSignOutOption, "Sign out of Baidu Netdisk"));
    m_settingsMenu->add_option(
        get_baidu_string(BaiduSignInOption, "Sign in again to Baidu Netdisk (QR code)"));
    m_settingsMenu->add_option(
        get_baidu_string(BackgroundSyncFormat, "Background backups: %s"));
}

void SettingsState::load_extra_strings()
{
    const char *onOff{}, *sortType{};
    for (int i = 0; (onOff = strings::get_by_name(strings::names::ON_OFF, i)); i++) { m_onOff[i] = onOff; }
    for (int i = 0; (sortType = strings::get_by_name(strings::names::SORT_TYPES, i)); i++) { m_sortTypes[i] = sortType; }
}

void SettingsState::update_menu_options()
{
    // These are the indexes that are a really simple toggle.
    static constexpr std::array<int, 20> TOGGLE_INDEXES = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                                           12, 13, 14, 15, 18, 19, 20, 21, 22, 23};

    for (const int index : TOGGLE_INDEXES)
    {
        const char *optionFormat = strings::get_by_name(strings::names::SETTINGS_MENU, index);
        const uint8_t value      = config::get_by_key(CONFIG_KEY_ARRAY[index]);
        const char *status       = SettingsState::get_status_text(value);
        const std::string option = stringutil::get_formatted_string(optionFormat, status);
        m_settingsMenu->edit_option(index, option);
    }

    {
        const char *zipCompFormat   = strings::get_by_name(strings::names::SETTINGS_MENU, CaseIndexes::CycleZip);
        const uint8_t zipLevel      = config::get_by_key(CONFIG_KEY_ARRAY[CaseIndexes::CycleZip]);
        const std::string zipOption = stringutil::get_formatted_string(zipCompFormat, zipLevel);
        m_settingsMenu->edit_option(CaseIndexes::CycleZip, zipOption);
    }

    {
        const char *titleSortFormat      = strings::get_by_name(strings::names::SETTINGS_MENU, CaseIndexes::CycleSortType);
        const uint8_t sortType           = config::get_by_key(CONFIG_KEY_ARRAY[CaseIndexes::CycleSortType]);
        const char *typeText             = SettingsState::get_sort_type_text(sortType);
        const std::string sortTypeOption = stringutil::get_formatted_string(titleSortFormat, typeText);
        m_settingsMenu->edit_option(CaseIndexes::CycleSortType, sortTypeOption);
    }

    {
        const char *scalingFormat       = strings::get_by_name(strings::names::SETTINGS_MENU, CaseIndexes::CycleScaling);
        const double scaling            = config::get_animation_scaling();
        const std::string scalingOption = stringutil::get_formatted_string(scalingFormat, scaling);
        m_settingsMenu->edit_option(CaseIndexes::CycleScaling, scalingOption);
    }

    SettingsState::update_baidu_account_option();
    SettingsState::update_background_sync_option();
}

void SettingsState::update_baidu_account_option()
{
    const remote::BaiduAccountStatus account = remote::get_baidu_account_status();
    m_lastBusy      = remote::is_busy();
    m_lastConnected = remote::get_remote_storage() != nullptr;
    std::string status{};
    if (!account.configured)
    {
        status = get_baidu_string(BaiduNotConfigured, "Not configured");
    }
    else if (!account.signedIn)
    {
        status = get_baidu_string(BaiduSignedOut, "Signed out - press [A] to sign in");
    }
    else if (account.displayName.empty())
    {
        status = get_baidu_string(BaiduNameUnavailable, "Signed in (name unavailable)");
    }
    else
    {
        status = account.displayName;
        std::replace_if(status.begin(), status.end(), [](unsigned char character) { return character < 0x20; }, ' ');
    }

    // Signed in, but the startup connection didn't go through. Say so, or it looks like everything is fine while
    // nothing gets uploaded.
    if (account.signedIn && !m_lastConnected && !m_lastBusy)
    {
        const char *notConnected = get_baidu_string(BaiduNotConnectedFormat, "%s (not connected - press [A] to retry)");
        status = stringutil::get_formatted_string(notConnected, status.c_str());
    }

    const char *format = get_baidu_string(BaiduAccountFormat, "Baidu Netdisk account: %s");
    const std::string option = stringutil::get_formatted_string(format, status.c_str());
    m_settingsMenu->edit_option(CaseIndexes::BaiduAccount, option);
    m_refreshBaiduStatus = false;
}

void SettingsState::update_background_sync_option()
{
    const bool disabled      = remote::background_sync_disabled();
    const char *state        = disabled ? get_baidu_string(BackgroundSyncDisabled, "Disabled after repeated crashes - press [A] to enable")
                                        : get_baidu_string(BackgroundSyncNormal, "Normal");
    const char *format       = get_baidu_string(BackgroundSyncFormat, "Background backups: %s");
    const std::string option = stringutil::get_formatted_string(format, state);
    m_settingsMenu->edit_option(CaseIndexes::BackgroundSync, option);
}

void SettingsState::enable_background_sync()
{
    if (!remote::background_sync_disabled()) { return; }

    const bool enabled = remote::enable_background_sync();
    ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS,
                                        enabled ? get_baidu_string(BackgroundSyncEnabled, "Background backups enabled. Restart the console for it to take effect.")
                                                : strings::get_by_name(strings::names::SETTINGS_POPS, 2));
}

void SettingsState::handle_baidu_account_option(bool forceReauthorize)
{
    const remote::BaiduAccountStatus account = remote::get_baidu_account_status();
    if (account.signedIn && !forceReauthorize)
    {
        if (remote::is_busy())
        {
            ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS,
                                                get_baidu_string(BaiduBusy, "Connecting to Baidu Netdisk. Please try again in a moment."));
        }
        else if (!remote::get_remote_storage()) { remote::retry_baidu_netdisk_connection(); }
        return;
    }

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    if (!account.configured)
    {
        ui::PopMessageManager::push_message(
            popTicks, get_baidu_string(BaiduNotConfigured, "Baidu Netdisk is not configured."));
        return;
    }
    if (!remote::begin_baidu_netdisk_sign_in(forceReauthorize))
    {
        ui::PopMessageManager::push_message(
            popTicks, get_baidu_string(BaiduSignInStartFailed, "Unable to start Baidu Netdisk sign in."));
    }
}

void SettingsState::confirm_baidu_sign_out()
{
    const remote::BaiduAccountStatus account = remote::get_baidu_account_status();
    if (!account.signedIn)
    {
        ui::PopMessageManager::push_message(
            ui::PopMessageManager::DEFAULT_TICKS,
            get_baidu_string(BaiduNoSignedInAccount, "No Baidu Netdisk account is currently signed in."));
        return;
    }
    if (remote::is_busy())
    {
        ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS,
                                            get_baidu_string(BaiduBusy, "Connecting to Baidu Netdisk. Please try again in a moment."));
        return;
    }

    const char *query = get_baidu_string(
        BaiduSignOutConfirmation,
        "Sign out of Baidu Netdisk on this console? Files already stored in the cloud will not be deleted.");
    auto taskData = std::make_shared<sys::Task::DataStruct>();
    ConfirmTask::create_push_fade(query, false, baidu_sign_out_task, nullptr, taskData);
}

void SettingsState::change_working_directory()
{
    const int popTicks           = ui::PopMessageManager::DEFAULT_TICKS;
    const char *inputHeader      = strings::get_by_name(strings::names::KEYBOARD, 2);
    const char *popSuccessFormat = strings::get_by_name(strings::names::SETTINGS_POPS, 1);
    const char *popFailed        = strings::get_by_name(strings::names::SETTINGS_POPS, 2);

    std::array<char, FS_MAX_PATH> pathBuffer = {0};
    const fslib::Path oldPath                = config::get_working_directory();

    const bool input =
        keyboard::get_input(SwkbdType_Normal, oldPath.string().c_str(), inputHeader, pathBuffer.data(), FS_MAX_PATH);
    if (!input) { return; }

    const fslib::Path newPath{pathBuffer.data()};
    if (!newPath.is_valid())
    {
        ui::PopMessageManager::push_message(popTicks, popFailed);
        return;
    }

    const bool exists = fslib::directory_exists(newPath);
    bool moved{};
    if (exists)
    {
        moved = fs::move_directory_recursively(oldPath, newPath);
        error::fslib(fslib::delete_directory_recursively(oldPath));
    }
    else { moved = fslib::rename_directory(oldPath, newPath); }

    if (!moved)
    {
        ui::PopMessageManager::push_message(popTicks, popFailed);
        return;
    }

    // Since the path was validated before, this shouldn't need to be checked.
    config::set_working_directory(newPath);

    const std::string newPathString = newPath.string();
    std::string popMessage          = stringutil::get_formatted_string(popSuccessFormat, newPathString.c_str());
    ui::PopMessageManager::push_message(popTicks, popMessage);
}

void SettingsState::create_push_blacklist_edit()
{
    const bool isEmpty = config::blacklist_is_empty();
    if (isEmpty)
    {
        const int popTicks   = ui::PopMessageManager::DEFAULT_TICKS;
        const char *popEmpty = strings::get_by_name(strings::names::SETTINGS_POPS, 0);
        ui::PopMessageManager::push_message(popTicks, popEmpty);
        return;
    }

    BlacklistEditState::create_and_push();
}

void SettingsState::toggle_options()
{
    const int selected = m_settingsMenu->get_selected();

    switch (selected)
    {
        case CaseIndexes::ChangeWorkDir: SettingsState::change_working_directory(); break;
        case CaseIndexes::EditBlacklist: SettingsState::create_push_blacklist_edit(); break;
        case CaseIndexes::CycleZip:      SettingsState::cycle_zip_level(); break;
        case CaseIndexes::CycleSortType: SettingsState::cycle_sort_type(); break;
        case CaseIndexes::ToggleJKSM:    SettingsState::toggle_jksm_mode(); break;
        case CaseIndexes::ToggleTrash:   SettingsState::toggle_trash_folder(); break;
        case CaseIndexes::CycleScaling:  SettingsState::cycle_anim_scaling(); break;
        case CaseIndexes::BaiduAccount:  SettingsState::handle_baidu_account_option(); break;
        case CaseIndexes::BaiduSignOut:  SettingsState::confirm_baidu_sign_out(); break;
        case CaseIndexes::BaiduSignIn:   SettingsState::handle_baidu_account_option(true); break;
        case CaseIndexes::BackgroundSync: SettingsState::enable_background_sync(); break;
        default:                         config::toggle_by_key(CONFIG_KEY_ARRAY[selected]); break;
    }

    SettingsState::update_menu_options();
}

void SettingsState::reset_settings()
{
    config::reset_to_default();
    SettingsState::update_menu_options();
}

void SettingsState::create_push_description_message()
{
    const int selected      = m_settingsMenu->get_selected();
    const char *description{};
    if (selected == CaseIndexes::BaiduAccount)
    {
        description = get_baidu_string(
            BaiduAccountDescription,
            "Shows the Baidu Netdisk account authorized on this console. Press [A] here to sign in again after signing out.");
    }
    else if (selected == CaseIndexes::BaiduSignOut)
    {
        description = get_baidu_string(
            BaiduSignOutDescription,
            "Clears local Baidu OAuth tokens and pauses background uploads. App credentials, settings, queued backups, and cloud files are preserved.");
    }
    else if (selected == CaseIndexes::BaiduSignIn)
    {
        description = get_baidu_string(
            BaiduSignInDescription,
            "Opens a new login QR code, even if already signed in. The current login is kept until the new one is authorized, so canceling changes nothing.");
    }
    else if (selected == CaseIndexes::BackgroundSync)
    {
        description = get_baidu_string(
            BackgroundSyncDescription,
            "Background backups turn themselves off if they crash twice in a row. Press [A] here to turn them back on after a restart.");
    }
    else { description = strings::get_by_name(strings::names::SETTINGS_DESCRIPTIONS, selected); }

    MessageState::create_push_fade(description);
}

void SettingsState::cycle_zip_level()
{
    uint8_t zipLevel = config::get_by_key(config::keys::ZIP_COMPRESSION_LEVEL);
    if (++zipLevel % 10 == 0) { zipLevel = 0; }

    config::set_by_key(config::keys::ZIP_COMPRESSION_LEVEL, zipLevel);
}

void SettingsState::cycle_sort_type()
{
    uint8_t sortType = config::get_by_key(config::keys::TITLE_SORT_TYPE);
    if (++sortType % 3 == 0) { sortType = 0; }
    config::set_by_key(config::keys::TITLE_SORT_TYPE, sortType);

    data::UserList users{};
    data::get_users(users);
    for (data::User *user : users) { user->sort_data(); }

    MainMenuState::refresh_view_states();
}

void SettingsState::toggle_jksm_mode()
{
    config::toggle_by_key(config::keys::JKSM_TEXT_MODE);

    MainMenuState::initialize_view_states();
}

void SettingsState::toggle_trash_folder()
{
    const bool trashEnabled = config::get_by_key(config::keys::ENABLE_TRASH_BIN);
    const fslib::Path trashPath{config::get_working_directory() / "_TRASH_"};
    config::toggle_by_key(config::keys::ENABLE_TRASH_BIN);

    if (trashEnabled) { error::fslib(fslib::delete_directory_recursively(trashPath)); }
    else { error::fslib(fslib::create_directory(trashPath)); }
}

void SettingsState::cycle_anim_scaling()
{
    double scaling = config::get_animation_scaling();
    if ((scaling += 0.25f) > 4.0f) { scaling = 1.0f; }

    config::set_animation_scaling(scaling);
    ui::Transition::update_scaling();
}

const char *SettingsState::get_status_text(uint8_t value)
{
    if (value > 1) { return nullptr; }

    return m_onOff[value];
}

const char *SettingsState::get_sort_type_text(uint8_t value)
{
    if (value > 2) { return nullptr; }

    return m_sortTypes[value];
}
