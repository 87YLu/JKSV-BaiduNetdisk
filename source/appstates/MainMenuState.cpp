#include "appstates/MainMenuState.hpp"

#include "StateManager.hpp"
#include "appstates/ConfirmState.hpp"
#include "appstates/ExtrasMenuState.hpp"
#include "appstates/SettingsState.hpp"
#include "appstates/TextTitleSelectState.hpp"
#include "appstates/TitleSelectCommon.hpp"
#include "appstates/TitleSelectState.hpp"
#include "appstates/UserOptionState.hpp"
#include "config/config.hpp"
#include "graphics/colors.hpp"
#include "input.hpp"
#include "logging/logger.hpp"
#include "remote/remote.hpp"
#include "sdl.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "sys/sys.hpp"
#include "tasks/mainmenu.hpp"
#include "tasks/update.hpp"
#include "ui/PopMessageManager.hpp"

//                      ---- Construction ----

MainMenuState::MainMenuState()
    : m_renderTarget(sdl::TextureManager::load("mainMenuTarget", 200, 555, SDL_TEXTUREACCESS_TARGET))
    , m_background(sdl::TextureManager::load("mainBackground", "romfs:/Textures/MenuBackground.png"))
    , m_settingsIcon(sdl::TextureManager::load("settingsIcon", "romfs:/Textures/SettingsIcon.png"))
    , m_extrasIcon(sdl::TextureManager::load("extrasIcon", "romfs:/Textures/ExtrasIcon.png"))
    , m_mainMenu(ui::IconMenu::create(50, 15, 555))
    , m_controlGuide(ui::ControlGuide::create(strings::get_by_name(strings::names::CONTROL_GUIDES, 0)))
    , m_dataStruct(std::make_shared<MainMenuState::DataStruct>())
{
    MainMenuState::initialize_settings_extras();
    MainMenuState::initialize_menu();
    MainMenuState::initialize_view_states();
    MainMenuState::initialize_data_struct();
    MainMenuState::check_for_update();
}

//                      ---- Public functions ----

void MainMenuState::update()
{
    const int selected  = m_mainMenu->get_selected();
    const bool hasFocus = BaseState::has_focus();
    const bool aPressed = input::button_pressed(HidNpadButton_A);
    const bool xPressed = input::button_pressed(HidNpadButton_X);
    const bool yPressed = input::button_pressed(HidNpadButton_Y);

    const bool toUserOptions = xPressed && selected < sm_userCount;

    if (aPressed) { MainMenuState::push_target_state(); }
    else if (toUserOptions) { MainMenuState::create_user_options(); }
    else if (yPressed) { MainMenuState::backup_all_for_all(); }
    else if (m_updateFound.load())
    {
        m_updateFound.store(false);
        MainMenuState::confirm_update();
    }

    m_mainMenu->update(hasFocus);
    m_controlGuide->update(hasFocus);

    for (auto &state : sm_states) { state->sub_update(); }
}

void MainMenuState::sub_update() { m_controlGuide->sub_update(); }

void MainMenuState::render()
{
    const bool hasFocus = BaseState::has_focus();
    const int selected  = m_mainMenu->get_selected();

    m_background->render(m_renderTarget, 0, 0);
    m_mainMenu->render(m_renderTarget, hasFocus);
    m_renderTarget->render(sdl::Texture::Null, 0, 91);
    m_controlGuide->render(sdl::Texture::Null, hasFocus);

    if (hasFocus)
    {
        BaseState *target = sm_states[selected].get();
        target->render();
    }
}

void MainMenuState::signal_update_found() { m_updateFound.store(true); }

void MainMenuState::initialize_view_states()
{
    const bool jksmMode = config::get_by_key(config::keys::JKSM_TEXT_MODE);

    sm_states.clear();
    for (data::User *user : sm_users)
    {
        std::shared_ptr<BaseState> state{};

        if (jksmMode) { state = TextTitleSelectState::create(user); }
        else { state = TitleSelectState::create(user); }

        sm_states.push_back(state);
    }
    sm_states.push_back(sm_settingsState);
    sm_states.push_back(sm_extrasState);
}

void MainMenuState::refresh_view_states()
{
    for (int i = 0; i < sm_userCount; i++)
    {
        TitleSelectCommon *target = static_cast<TitleSelectCommon *>(sm_states.at(i).get());
        target->refresh();
    }
}

//                      ---- Private functions ----

void MainMenuState::initialize_settings_extras()
{
    if (!sm_settingsState || !sm_extrasState)
    {
        sm_settingsState = SettingsState::create();
        sm_extrasState   = ExtrasMenuState::create();
    }
}

void MainMenuState::initialize_menu()
{
    data::get_users(sm_users);
    sm_userCount = sm_users.size();
    for (data::User *user : sm_users) { m_mainMenu->add_option(user->get_icon()); }
    m_mainMenu->add_option(m_settingsIcon);
    m_mainMenu->add_option(m_extrasIcon);
}

void MainMenuState::initialize_data_struct()
{
    m_dataStruct->userList      = sm_users;
    m_dataStruct->spawningState = this;
}

// The upstream updater would replace this build with J-D-K's, which has no Baidu Netdisk support and no worker.
void MainMenuState::check_for_update() {}

void MainMenuState::push_target_state()
{
    const int popTicks          = ui::PopMessageManager::DEFAULT_TICKS;
    const char *popNoSaveFormat = strings::get_by_name(strings::names::MAINMENU_POPS, 0);

    const int selected  = m_mainMenu->get_selected();
    const int userCount = sm_users.size();
    if (selected < userCount)
    {
        const data::User *user = sm_users[selected];
        const int titleCount   = user->get_total_data_entries();
        if (titleCount <= 0)
        {
            const char *nickname = user->get_nickname();
            std::string popError = stringutil::get_formatted_string(popNoSaveFormat, nickname);
            ui::PopMessageManager::push_message(popTicks, popError);
            return;
        }
    }

    auto &target = sm_states[selected];
    target->reactivate();
    StateManager::push_state(target);
    m_mainMenu->play_sound();
}

void MainMenuState::create_user_options()
{
    const int selected             = m_mainMenu->get_selected();
    data::User *user               = sm_users[selected];
    TitleSelectCommon *titleSelect = static_cast<TitleSelectCommon *>(sm_states[selected].get());

    UserOptionState::create_and_push(user, titleSelect);
}

void MainMenuState::backup_all_for_all()
{
    remote::Storage *remote = remote::get_remote_storage();
    const bool autoUpload   = config::get_by_key(config::keys::AUTO_UPLOAD);
    const char *query       = strings::get_by_name(strings::names::MAINMENU_CONFS, 0);
    if (remote && autoUpload)
    {
        ConfirmProgress::create_push_fade(query, true, tasks::mainmenu::backup_all_for_all_remote, nullptr, m_dataStruct);
    }
    else { ConfirmProgress::create_push_fade(query, true, tasks::mainmenu::backup_all_for_all_local, nullptr, m_dataStruct); }
}

void MainMenuState::confirm_update()
{
    const char *confirmUpdate = strings::get_by_name(strings::names::UPDATE_CONFIRMATION, 0);
    auto taskData             = std::make_shared<sys::Task::DataStruct>();

    ConfirmProgress::create_push_fade(confirmUpdate, false, tasks::update::download_update, nullptr, taskData);
}
