#include "tasks/mainmenu.hpp"

#include "config/config.hpp"
#include "data/data.hpp"
#include "error.hpp"
#include "fs/fs.hpp"
#include "remote/remote.hpp"
#include "stringutil.hpp"
#include "tasks/backup.hpp"

void tasks::mainmenu::backup_all_for_all_local(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<MainMenuState::DataStruct>(taskData);

    sys::ProgressTask *task  = static_cast<sys::ProgressTask *>(castData->task);
    data::UserList &userList = castData->userList;
    const bool exportZip     = config::get_by_key(config::keys::EXPORT_TO_ZIP);

    // This is to pass and use the already written backup function.
    auto backupStruct      = std::make_shared<BackupMenuState::DataStruct>();
    backupStruct->task     = castData->task;
    backupStruct->killTask = false; // Just to be sure.

    if (error::is_null(task)) { return; }

    for (data::User *user : userList)
    {
        backupStruct->user = user;

        const int64_t titleCount = user->get_total_data_entries();
        for (int64_t i = 0; i < titleCount; i++)
        {
            if (user->get_account_save_type() == FsSaveDataType_System) { continue; }

            const FsSaveDataInfo *saveInfo = user->get_save_info_at(i);
            if (error::is_null(saveInfo)) { continue; }

            {
                fs::ScopedSaveMount scopedMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
                if (!scopedMount.is_open() || !fs::directory_has_contents(fs::DEFAULT_SAVE_ROOT)) { continue; }
            }

            backupStruct->saveInfo       = saveInfo;
            const uint64_t applicationID = saveInfo->application_id;
            data::TitleInfo *titleInfo   = data::get_title_info_by_id(applicationID);
            if (error::is_null(titleInfo)) { continue; }

            backupStruct->titleInfo = titleInfo;
            const fslib::Path workDir{config::get_working_directory()};
            const fslib::Path targetDir{workDir / titleInfo->get_path_safe_title()};
            const bool exists      = fslib::directory_exists(targetDir);
            const bool createError = !exists && error::fslib(fslib::create_directories_recursively(targetDir));
            if (!exists && createError) { continue; }

            const char *pathSafe         = user->get_path_safe_nickname();
            const std::string dateString = stringutil::get_date_string();
            const std::string name       = stringutil::get_formatted_string("%s - %s", pathSafe, dateString.c_str());
            fslib::Path finalTarget{targetDir / name};
            if (exportZip) { finalTarget += ".zip"; }
            else
            {
                const bool createError = error::fslib(fslib::create_directory(finalTarget));
                if (createError) { continue; }
            }

            backupStruct->path = std::move(finalTarget);
            tasks::backup::create_new_backup_local(backupStruct);
        }
    }

    task->complete();
}

void tasks::mainmenu::backup_all_for_all_remote(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<MainMenuState::DataStruct>(taskData);

    sys::ProgressTask *task = static_cast<sys::ProgressTask *>(castData->task);
    if (error::is_null(task)) { return; }

    remote::Storage *remote = remote::get_remote_storage();
    if (error::is_null(remote)) { TASK_FINISH_RETURN(task); }

    auto backupStruct      = std::make_shared<BackupMenuState::DataStruct>();
    backupStruct->task     = task;
    backupStruct->killTask = false;

    const data::UserList &userList = castData->userList;
    for (data::User *user : userList)
    {
        if (user->get_account_save_type() == FsSaveDataType_System) { continue; }
        backupStruct->user = user;

        const int64_t titleCount = user->get_total_data_entries();
        for (int64_t i = 0; i < titleCount; i++)
        {
            const FsSaveDataInfo *saveInfo = user->get_save_info_at(i);
            if (error::is_null(saveInfo)) { continue; }

            backupStruct->saveInfo = saveInfo;
            {
                fs::ScopedSaveMount scopedMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
                if (!scopedMount.is_open() || !fs::directory_has_contents(fs::DEFAULT_SAVE_ROOT)) { continue; }
            }

            const uint64_t applicationID = saveInfo->application_id;
            data::TitleInfo *titleInfo   = data::get_title_info_by_id(applicationID);
            if (error::is_null(titleInfo)) { continue; }

            backupStruct->titleInfo = titleInfo;
            const std::string_view remoteTitle =
                remote->supports_utf8() ? titleInfo->get_title() : titleInfo->get_path_safe_title();
            const bool exists  = remote->directory_exists(remoteTitle);
            const bool created = !exists && remote->create_directory(remoteTitle);
            if (!exists && !created) { continue; }

            remote::Item *targetDir = remote->get_directory_by_name(remoteTitle);
            remote->change_directory(targetDir);
            if (!remote->refresh_directory())
            {
                remote->return_to_root();
                continue;
            }

            const char *pathSafe         = user->get_path_safe_nickname();
            const std::string dateString = stringutil::get_date_string();
            std::string remoteName       = stringutil::get_formatted_string("%s - %s.zip", pathSafe, dateString.c_str());
            backupStruct->remoteName     = std::move(remoteName);

            tasks::backup::create_new_backup_remote(backupStruct);
            remote->return_to_root();
        }
    }

    task->complete();
}
