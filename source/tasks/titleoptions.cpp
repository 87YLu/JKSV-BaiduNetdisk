#include "tasks/titleoptions.hpp"

#include "config/config.hpp"
#include "data/data.hpp"
#include "error.hpp"
#include "fs/fs.hpp"
#include "keyboard.hpp"
#include "logging/logger.hpp"
#include "remote/remote.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "ui/ui.hpp"

#include <array>

void tasks::titleoptions::blacklist_title(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<TitleOptionState::DataStruct>(taskData);

    sys::Task *task                 = castData->task;
    data::TitleInfo *titleInfo      = castData->titleInfo;
    TitleOptionState *spawningState = castData->spawningState;

    if (error::is_null(task)) { return; }
    else if (error::is_null({titleInfo, spawningState})) { TASK_FINISH_RETURN(task); }

    const uint64_t applicationID = titleInfo->get_application_id();
    config::add_remove_blacklist(applicationID);

    data::UserList list{};
    data::get_users(list);
    for (data::User *user : list) { user->erase_save_info_by_id(applicationID); }

    // We need to signal both since the title in question is no longer valid.
    spawningState->refresh_required();
    spawningState->close_on_update();

    task->complete();
}

void tasks::titleoptions::delete_all_local_backups_for_title(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<TitleOptionState::DataStruct>(taskData);

    sys::Task *task            = castData->task;
    data::TitleInfo *titleInfo = castData->titleInfo;

    if (error::is_null(task)) { return; }
    else if (error::is_null(titleInfo)) { TASK_FINISH_RETURN(task); }

    const int popTicks     = ui::PopMessageManager::DEFAULT_TICKS;
    const char *popSuccess = strings::get_by_name(strings::names::TITLEOPTION_POPS, 0);
    const char *popFailure = strings::get_by_name(strings::names::TITLEOPTION_POPS, 1);

    {
        const char *title        = titleInfo->get_title();
        const char *statusFormat = strings::get_by_name(strings::names::TITLEOPTION_STATUS, 0);
        std::string status       = stringutil::get_formatted_string(statusFormat, title);
        task->set_status(status);
    }

    const char *safeTitle = titleInfo->get_path_safe_title();
    const fslib::Path workingDir{config::get_working_directory()};
    const fslib::Path targetPath{workingDir / safeTitle};

    const bool dirExists    = fslib::directory_exists(targetPath);
    const bool deleteFailed = dirExists && error::fslib(fslib::delete_directory_recursively(targetPath));
    if (deleteFailed) { ui::PopMessageManager::push_message(popTicks, popFailure); }
    else
    {
        const char *title      = titleInfo->get_title();
        std::string popMessage = stringutil::get_formatted_string(popSuccess, title);
        ui::PopMessageManager::push_message(popTicks, popMessage);
    }

    task->complete();
}

void tasks::titleoptions::delete_all_remote_backups_for_title(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<TitleOptionState::DataStruct>(taskData);

    sys::Task *task            = castData->task;
    data::TitleInfo *titleInfo = castData->titleInfo;
    remote::Storage *remote    = remote::get_remote_storage();

    if (error::is_null(task)) { return; }
    else if (error::is_null({titleInfo, remote})) { TASK_FINISH_RETURN(task); }

    const char *title                  = titleInfo->get_title();
    const std::string_view remoteTitle = remote->supports_utf8() ? titleInfo->get_title() : titleInfo->get_path_safe_title();
    const bool exists                  = remote->directory_exists(remoteTitle);
    if (!exists) { TASK_FINISH_RETURN(task); }

    remote::Item *workDir = remote->get_directory_by_name(remoteTitle);
    if (!workDir) { TASK_FINISH_RETURN(task); }
    remote->change_directory(workDir);

    remote::Storage::DirectoryListing remoteListing{};
    remote->get_directory_listing(remoteListing);

    // This is needed because deleting one throws the vector out of whack.
    std::vector<std::string> ids{};
    for (remote::Item *item : remoteListing) { ids.emplace_back(item->get_id()); }

    {
        const char *statusFormat = strings::get_by_name(strings::names::TITLEOPTION_STATUS, 0);
        std::string status       = stringutil::get_formatted_string(statusFormat, title);
        task->set_status(status);
    }

    const int popTicks     = ui::PopMessageManager::DEFAULT_TICKS;
    const char *popSuccess = strings::get_by_name(strings::names::TITLEOPTION_POPS, 0);
    const char *popFailure = strings::get_by_name(strings::names::TITLEOPTION_POPS, 1);
    for (const std::string &id : ids)
    {
        remote::Item *item = remote->get_item_by_id(id);
        if (!item) { continue; }

        const bool deleted = remote->delete_item(item);
        if (!deleted)
        {
            ui::PopMessageManager::push_message(popTicks, popFailure);
            remote->return_to_root();
            TASK_FINISH_RETURN(task);
        }
    }

    remote->return_to_root();

    std::string popMessage = stringutil::get_formatted_string(popSuccess, title);
    ui::PopMessageManager::push_message(popTicks, popMessage);
    task->complete();
}

void tasks::titleoptions::reset_save_data(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<TitleOptionState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, saveInfo})) { TASK_FINISH_RETURN(task); }

    const int popTicks     = ui::PopMessageManager::DEFAULT_TICKS;
    const char *popFailed  = strings::get_by_name(strings::names::TITLEOPTION_POPS, 2);
    const char *popSuccess = strings::get_by_name(strings::names::TITLEOPTION_POPS, 3);

    {
        const char *statusFormat = strings::get_by_name(strings::names::TITLEOPTION_STATUS, 1);
        const char *title        = titleInfo->get_title();
        std::string status       = stringutil::get_formatted_string(statusFormat, title);
        task->set_status(status);
    }

    {
        fs::ScopedSaveMount saveMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
        const bool resetFailed  = error::fslib(fslib::delete_directory_recursively(fs::DEFAULT_SAVE_ROOT));
        const bool commitFailed = error::fslib(fslib::commit_data_to_file_system(fs::DEFAULT_SAVE_MOUNT));
        if (resetFailed || commitFailed) { ui::PopMessageManager::push_message(popTicks, popFailed); }
        else { ui::PopMessageManager::push_message(popTicks, popSuccess); }
    }

    task->complete();
}

void tasks::titleoptions::delete_save_data_from_system(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<TitleOptionState::DataStruct>(taskData);

    sys::Task *task                 = castData->task;
    data::User *user                = castData->user;
    data::TitleInfo *titleInfo      = castData->titleInfo;
    const FsSaveDataInfo *saveInfo  = castData->saveInfo;
    TitleSelectCommon *titleSelect  = castData->titleSelect;
    TitleOptionState *spawningState = castData->spawningState;
    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, saveInfo, titleSelect, spawningState})) { TASK_FINISH_RETURN(task); }

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    {
        const char *statusFormat = strings::get_by_name(strings::names::TITLEOPTION_STATUS, 2);
        const char *nickname     = user->get_nickname();
        const char *title        = titleInfo->get_title();
        std::string status       = stringutil::get_formatted_string(statusFormat, nickname, title);
        task->set_status(status);
    }

    const bool saveDeleted = fs::delete_save_data(saveInfo);
    if (!saveDeleted)
    {
        const char *popError = strings::get_by_name(strings::names::SAVECREATE_POPS, 2);
        ui::PopMessageManager::push_message(popTicks, popError);
        return;
    }

    user->erase_save_info(saveInfo);
    titleSelect->refresh();
    spawningState->close_on_update();
    task->complete();
}

void tasks::titleoptions::extend_save_data(sys::threadpool::JobData taskData)
{
    static constexpr int SIZE_MB   = 0x100000;
    const int popTicks             = ui::PopMessageManager::DEFAULT_TICKS;
    auto castData                  = std::static_pointer_cast<TitleOptionState::DataStruct>(taskData);
    sys::Task *task                = castData->task;
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, saveInfo})) { TASK_FINISH_RETURN(task); }

    std::array<char, 5> sizeBuffer = {0};
    FsSaveDataExtraData extraData{};
    const bool readExtra = fs::read_save_extra_data(saveInfo, extraData);

    const int sizeMB                  = extraData.data_size / SIZE_MB;
    const char *keyboardHeader        = strings::get_by_name(strings::names::KEYBOARD, 8);
    const std::string keyboardDefault = stringutil::get_formatted_string("%lli", sizeMB + 1);

    const bool validInput = keyboard::get_input(SwkbdType_NumPad, keyboardDefault, keyboardHeader, sizeBuffer.data(), 5);
    if (!validInput) { TASK_FINISH_RETURN(task); }

    {
        const char *nickname        = user->get_nickname();
        const char *title           = titleInfo->get_title();
        const char *extendingFormat = strings::get_by_name(strings::names::TITLEOPTION_STATUS, 3);
        std::string status          = stringutil::get_formatted_string(extendingFormat, nickname, title);

        task->set_status(status);
    }

    const uint8_t saveType  = saveInfo->save_data_type;
    const int64_t size      = std::strtoll(sizeBuffer.data(), nullptr, 10) * SIZE_MB;
    const int64_t journal   = !readExtra ? titleInfo->get_journal_size(saveType) : extraData.journal_size;
    const bool saveExtended = fs::extend_save_data(saveInfo, size, journal);
    if (saveExtended)
    {
        const char *popSuccess = strings::get_by_name(strings::names::TITLEOPTION_POPS, 10);
        ui::PopMessageManager::push_message(popTicks, popSuccess);
    }
    else
    {
        const char *popFailed = strings::get_by_name(strings::names::TITLEOPTION_POPS, 11);
        ui::PopMessageManager::push_message(popTicks, popFailed);
    }

    task->complete();
}
