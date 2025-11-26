#include "tasks/useroptions.hpp"

#include "config/config.hpp"
#include "error.hpp"
#include "fs/fs.hpp"
#include "remote/remote.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "tasks/backup.hpp"
#include "ui/ui.hpp"

void tasks::useroptions::backup_all_for_user_local(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<UserOptionState::DataStruct>(taskData);

    sys::ProgressTask *task = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user        = castData->user;

    if (error::is_null(task)) { return; }
    else if (error::is_null(user)) { TASK_FINISH_RETURN(task); }

    auto backupStruct      = std::make_shared<BackupMenuState::DataStruct>();
    backupStruct->task     = task;
    backupStruct->user     = user;
    backupStruct->killTask = false;

    const fslib::Path workDir{config::get_working_directory()};
    const bool exportToZip = config::get_by_key(config::keys::EXPORT_TO_ZIP);
    const int titleCount   = user->get_total_data_entries();
    for (int i = 0; i < titleCount; i++)
    {
        const FsSaveDataInfo *saveInfo = user->get_save_info_at(i);
        if (error::is_null(saveInfo)) { continue; }

        backupStruct->saveInfo = saveInfo;
        {
            fs::ScopedSaveMount scopedMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
            if (!scopedMount.is_open() || !fs::directory_has_contents(fs::DEFAULT_SAVE_ROOT)) { continue; }
        }

        data::TitleInfo *titleInfo = data::get_title_info_by_id(saveInfo->application_id);
        if (error::is_null(titleInfo)) { continue; }

        backupStruct->titleInfo = titleInfo;
        const fslib::Path targetDir{workDir / titleInfo->get_path_safe_title()};
        const bool targetExists = fslib::directory_exists(targetDir);
        const bool createError  = !targetExists && error::fslib(fslib::create_directories_recursively(targetDir));
        if (!targetExists && createError) { continue; }

        const char *pathSafe         = user->get_path_safe_nickname();
        const std::string dateString = stringutil::get_date_string();
        const std::string name       = stringutil::get_formatted_string("%s - %s", pathSafe, dateString.c_str());
        fslib::Path finalTarget{targetDir / name};
        if (exportToZip) { finalTarget += ".zip"; }
        else
        {
            const bool createError = error::fslib(fslib::create_directory(finalTarget));
            if (createError) { continue; }
        }

        backupStruct->path = std::move(finalTarget);
        tasks::backup::create_new_backup_local(backupStruct);
    }
    task->complete();
}

void tasks::useroptions::backup_all_for_user_remote(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<UserOptionState::DataStruct>(taskData);

    sys::ProgressTask *task = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user        = castData->user;
    remote::Storage *remote = remote::get_remote_storage();
    if (error::is_null(task)) { return; }
    else if (error::is_null({user, remote})) { TASK_FINISH_RETURN(task); }

    auto backupStruct      = std::make_shared<BackupMenuState::DataStruct>();
    backupStruct->task     = task;
    backupStruct->user     = user;
    backupStruct->killTask = false;

    const int titleCount = user->get_total_data_entries();
    for (int i = 0; i < titleCount; i++)
    {
        const FsSaveDataInfo *saveInfo = user->get_save_info_at(i);
        if (error::is_null(saveInfo)) { continue; }

        backupStruct->saveInfo = saveInfo;
        {
            fs::ScopedSaveMount scopedMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
            if (!scopedMount.is_open() || !fs::directory_has_contents(fs::DEFAULT_SAVE_ROOT)) { continue; }
        }

        data::TitleInfo *titleInfo = data::get_title_info_by_id(saveInfo->application_id);
        if (error::is_null(titleInfo)) { continue; }

        backupStruct->titleInfo = titleInfo;
        const std::string_view remoteTitle =
            remote->supports_utf8() ? titleInfo->get_title() : titleInfo->get_path_safe_title();
        const bool dirExists = remote->directory_exists(remoteTitle);
        const bool created   = !dirExists && remote->create_directory(remoteTitle);
        if (!dirExists && !created) { continue; }

        remote::Item *target = remote->get_directory_by_name(remoteTitle);
        remote->change_directory(target);

        const std::string dateString = stringutil::get_date_string();

        backupStruct->remoteName = stringutil::get_formatted_string("%s - %s.zip", user->get_nickname(), dateString.c_str());
        tasks::backup::create_new_backup_remote(backupStruct);

        remote->return_to_root();
    }
    task->complete();
}

void tasks::useroptions::create_all_save_data_for_user(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<UserOptionState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    data::User *user               = castData->user;
    UserOptionState *spawningState = castData->spawningState;
    if (error::is_null(task)) { return; }
    else if (error::is_null({user, spawningState})) { TASK_FINISH_RETURN(task); }

    data::TitleInfoList infoList{};
    data::get_title_info_list(infoList);
    const int popTicks       = ui::PopMessageManager::DEFAULT_TICKS;
    const char *statusFormat = strings::get_by_name(strings::names::USEROPTION_STATUS, 0);
    const char *popFailure   = strings::get_by_name(strings::names::SAVECREATE_POPS, 1);
    const uint8_t saveType   = user->get_account_save_type();

    for (data::TitleInfo *titleInfo : infoList)
    {
        const char *title      = titleInfo->get_title();
        const bool hasSaveType = titleInfo->has_save_data_type(saveType);
        if (!hasSaveType) { continue; }

        {
            std::string status = stringutil::get_formatted_string(statusFormat, title);
            task->set_status(status);
        }

        const bool saveCreated = fs::create_save_data_for(user, titleInfo);
        if (!saveCreated)
        {
            std::string popMessage = stringutil::get_formatted_string(popFailure, title);
            ui::PopMessageManager::push_message(popTicks, popMessage);
        }
    }

    spawningState->refresh_required();
    task->complete();
}

void tasks::useroptions::delete_all_save_data_for_user(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<UserOptionState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    data::User *user               = castData->user;
    UserOptionState *spawningState = castData->spawningState;
    if (error::is_null(task)) { return; }
    else if (error::is_null({user, spawningState}) || user->get_account_save_type() == FsSaveDataType_System)
    {
        TASK_FINISH_RETURN(task);
    }

    const int popTicks       = ui::PopMessageManager::DEFAULT_TICKS;
    const char *statusFormat = strings::get_by_name(strings::names::USEROPTION_STATUS, 1);
    const char *popFailed    = strings::get_by_name(strings::names::SAVECREATE_POPS, 2);

    std::vector<const FsSaveDataInfo *> saveInfos{};
    const int titleCount = user->get_total_data_entries();
    for (int i = 0; i < titleCount; i++)
    {
        const FsSaveDataInfo *saveInfo = user->get_save_info_at(i);
        if (error::is_null(saveInfo) || saveInfo->save_data_type == FsSaveDataType_System) { continue; }

        const uint64_t applicationID = saveInfo->application_id;
        {
            data::TitleInfo *titleInfo = data::get_title_info_by_id(applicationID);
            const char *title          = titleInfo->get_title();
            std::string status         = stringutil::get_formatted_string(statusFormat, title);
            task->set_status(status);
        }

        const bool saveDeleted = fs::delete_save_data(saveInfo);
        if (!saveDeleted)
        {
            ui::PopMessageManager::push_message(popTicks, popFailed);
            continue;
        }

        saveInfos.push_back(saveInfo);
    }

    for (const FsSaveDataInfo *saveInfo : saveInfos) { user->erase_save_info(saveInfo); }

    spawningState->refresh_required();
    task->complete();
}
