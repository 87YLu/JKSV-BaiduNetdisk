#include "tasks/backup.hpp"

#include "config/config.hpp"
#include "error.hpp"
#include "fs/fs.hpp"
#include "logging/logger.hpp"
#include "remote/remote.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "ui/PopMessageManager.hpp"

#include <cstring>

namespace
{
    constexpr const char *STRING_ZIP_EXT = ".zip";
    constexpr const char *PATH_JKSV_TEMP = "sdmc:/jksvTemp.zip"; // This is named this so if something fails, people know.
}

// Definitions at bottom.
static void auto_backup(sys::ProgressTask *task, BackupMenuState::TaskData taskData);
static bool read_and_process_meta(const fslib::Path &targetDir, BackupMenuState::TaskData taskData, sys::ProgressTask *task);
static bool read_and_process_meta(fs::MiniUnzip &unzip, BackupMenuState::TaskData taskData, sys::ProgressTask *task);
static void write_meta_file(const fslib::Path &target, const FsSaveDataInfo *saveInfo);
static void write_meta_zip(fs::MiniZip &zip, const FsSaveDataInfo *saveInfo);
static fs::ScopedSaveMount create_scoped_mount(const FsSaveDataInfo *saveInfo);

void tasks::backup::create_new_backup_local(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    const fslib::Path &target      = castData->path;
    BackupMenuState *spawningState = castData->spawningState;
    const bool killTask            = castData->killTask;

    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, saveInfo})) { TASK_FINISH_RETURN(task); }

    const std::string targetString = target.string();
    const bool hasZipExt           = std::strstr(targetString.c_str(), STRING_ZIP_EXT);
    if (hasZipExt) // At this point, this should have the zip extension appended if needed.
    {
        fs::MiniZip zip{target};
        if (!zip.is_open()) { TASK_FINISH_RETURN(task); }

        write_meta_zip(zip, saveInfo);
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_directory_to_zip(fs::DEFAULT_SAVE_ROOT, zip, task);
    }
    else
    {
        const bool needsDir    = !fslib::directory_exists(target);
        const bool createError = needsDir && error::fslib(fslib::create_directory(target));
        if (needsDir && createError) { TASK_FINISH_RETURN(task); }

        write_meta_file(target, saveInfo);
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_directory(fs::DEFAULT_SAVE_ROOT, target, task);
    }

    // This is like this so I can reuse this code.
    if (spawningState) { spawningState->refresh(); }
    if (killTask) { task->complete(); }
}

void tasks::backup::create_new_backup_remote(sys::threadpool::JobData taskData)
{

    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    const fslib::Path &path        = castData->path;
    const std::string &remoteName  = castData->remoteName;
    BackupMenuState *spawningState = castData->spawningState;
    const bool &killTask           = castData->killTask;
    const bool keepLocal           = config::get_by_key(config::keys::KEEP_LOCAL_BACKUPS);
    remote::Storage *remote        = remote::get_remote_storage();

    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, remote, saveInfo})) { TASK_FINISH_RETURN(task); }
    {
        TASK_FINISH_RETURN(task);
    }

    const fslib::Path zipPath{keepLocal ? path : PATH_JKSV_TEMP};
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;

    fs::MiniZip zip{zipPath};
    if (!zip.is_open())
    {
        const char *popErrorCreating = strings::get_by_name(strings::names::BACKUPMENU_POPS, 5);
        ui::PopMessageManager::push_message(popTicks, popErrorCreating);
        TASK_FINISH_RETURN(task);
    }

    write_meta_zip(zip, saveInfo);
    {
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_directory_to_zip(fs::DEFAULT_SAVE_ROOT, zip, task);
    }
    zip.close();

    {
        const char *uploadFormat = strings::get_by_name(strings::names::IO_STATUSES, 5);
        std::string status       = stringutil::get_formatted_string(uploadFormat, remoteName.data());
        task->set_status(status);
    }

    const bool uploaded    = remote->upload_file(zipPath, remoteName, task);
    const bool deleteError = uploaded && !keepLocal && error::fslib(fslib::delete_file(zipPath));
    if (!uploaded || deleteError)
    {
        const char *popErrorUploading = strings::get_by_name(strings::names::BACKUPMENU_POPS, 10);
        ui::PopMessageManager::push_message(popTicks, popErrorUploading);
    }

    if (spawningState) { spawningState->refresh(); }
    if (killTask) { task->complete(); }
}

void tasks::backup::overwrite_backup_local(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task   = static_cast<sys::ProgressTask *>(castData->task);
    const fslib::Path &target = castData->path;
    if (error::is_null(task)) { return; }

    const int popTicks     = ui::PopMessageManager::DEFAULT_TICKS;
    const bool isDirectory = fslib::directory_exists(target);
    const bool dirFailed   = isDirectory && error::fslib(fslib::delete_directory_recursively(target));
    const bool fileFailed  = !isDirectory && error::fslib(fslib::delete_file(target));
    if (dirFailed && fileFailed)
    {
        const char *popErrorDeleting = strings::get_by_name(strings::names::BACKUPMENU_POPS, 4);
        ui::PopMessageManager::push_message(popTicks, popErrorDeleting);
        TASK_FINISH_RETURN(task);
    }

    castData->killTask = true;
    tasks::backup::create_new_backup_local(castData);
}

void tasks::backup::overwrite_backup_remote(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    remote::Item *target           = castData->remoteItem;
    remote::Storage *remote        = remote::get_remote_storage();

    if (error::is_null(task)) { return; }
    else if (error::is_null({remote, target})) { TASK_FINISH_RETURN(task); }

    const fslib::Path tempPath{PATH_JKSV_TEMP};
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;

    fs::MiniZip zip{tempPath};
    if (!zip.is_open()) { TASK_FINISH_RETURN(task); }

    write_meta_zip(zip, saveInfo);
    {
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_directory_to_zip(fs::DEFAULT_SAVE_ROOT, zip, task);
    }
    zip.close();

    {
        const char *targetName   = target->get_name().data();
        const char *statusFormat = strings::get_by_name(strings::names::IO_STATUSES, 5);
        std::string status       = stringutil::get_formatted_string(statusFormat, targetName);
        task->set_status(status);
    }
    remote->patch_file(target, tempPath, task);

    const bool deleteError = error::fslib(fslib::delete_file(tempPath));
    if (deleteError)
    {
        const char *popErrorDeleting = strings::get_by_name(strings::names::BACKUPMENU_POPS, 4);
        ui::PopMessageManager::push_message(popTicks, popErrorDeleting);
    }

    task->complete();
}

void tasks::backup::restore_backup_local(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    const fslib::Path &target      = castData->path;
    BackupMenuState *spawningState = castData->spawningState;

    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, saveInfo, spawningState})) { TASK_FINISH_RETURN(task); }

    FsSaveDataExtraData extraData{};
    const uint8_t saveType    = saveInfo->save_data_type;
    const bool readExtra      = fs::read_save_extra_data(saveInfo, extraData);
    const int64_t journalSize = readExtra ? extraData.journal_size : titleInfo->get_journal_size(saveType);

    const std::string targetString = target.string();
    const bool autoBackup          = config::get_by_key(config::keys::AUTO_BACKUP_ON_RESTORE);
    const bool isDir               = fslib::directory_exists(target);
    const bool hasZipExt           = std::strstr(targetString.c_str(), STRING_ZIP_EXT);

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    if (autoBackup) { auto_backup(task, castData); }

    {
        auto scopedMount = create_scoped_mount(saveInfo);
        error::fslib(fslib::delete_directory_recursively(fs::DEFAULT_SAVE_ROOT));
        error::fslib(fslib::commit_data_to_file_system(fs::DEFAULT_SAVE_MOUNT));
    }

    if (!isDir && hasZipExt)
    {
        fs::MiniUnzip unzip{target};
        if (!unzip.is_open())
        {
            const char *popErrorOpenZip = strings::get_by_name(strings::names::EXTRASMENU_POPS, 7);
            ui::PopMessageManager::push_message(popTicks, popErrorOpenZip);
            TASK_FINISH_RETURN(task);
        }

        read_and_process_meta(unzip, castData, task);
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_zip_to_directory(unzip, fs::DEFAULT_SAVE_ROOT, journalSize, task);
    }
    else if (isDir)
    {
        read_and_process_meta(target, castData, task);
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_directory_commit(target, fs::DEFAULT_SAVE_ROOT, journalSize, task);
    }
    else
    {
        auto scopedMount = create_scoped_mount(saveInfo);
        fs::copy_file_commit(target, fs::DEFAULT_SAVE_ROOT, journalSize, task);
    }

    if (spawningState)
    {
        spawningState->save_data_written();
        spawningState->refresh();
    }

    task->complete();
}

void tasks::backup::restore_backup_remote(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    const FsSaveDataInfo *saveInfo = castData->saveInfo;
    BackupMenuState *spawningState = castData->spawningState;
    remote::Storage *remote        = remote::get_remote_storage();
    const bool autoBackup          = config::get_by_key(config::keys::AUTO_BACKUP_ON_RESTORE);

    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, saveInfo, remote})) { TASK_FINISH_RETURN(task); }

    if (autoBackup) { auto_backup(task, castData); }

    {
        auto scopedMount = create_scoped_mount(saveInfo);
        error::fslib(fslib::delete_directory_recursively(fs::DEFAULT_SAVE_ROOT));
        error::fslib(fslib::commit_data_to_file_system(fs::DEFAULT_SAVE_MOUNT));
    }

    const int popTicks   = ui::PopMessageManager::DEFAULT_TICKS;
    remote::Item *target = castData->remoteItem;
    const fslib::Path tempPath{PATH_JKSV_TEMP};
    {
        const char *name              = target->get_name().data();
        const char *downloadingFormat = strings::get_by_name(strings::names::IO_STATUSES, 4);
        std::string status            = stringutil::get_formatted_string(downloadingFormat, name);
        task->set_status(status);
    }

    const bool downloaded = remote->download_file(target, tempPath, task);
    if (!downloaded)
    {
        const char *popErrorDownloading = strings::get_by_name(strings::names::BACKUPMENU_POPS, 9);
        ui::PopMessageManager::push_message(popTicks, popErrorDownloading);
        TASK_FINISH_RETURN(task);
    }

    fs::MiniUnzip backup{tempPath};
    if (!backup.is_open())
    {
        const char *popErrorOpeningZip = strings::get_by_name(strings::names::BACKUPMENU_POPS, 3);
        ui::PopMessageManager::push_message(popTicks, popErrorOpeningZip);
        TASK_FINISH_RETURN(task);
    }

    {
        auto scopedMount       = create_scoped_mount(saveInfo);
        const bool deleteError = error::fslib(fslib::delete_directory_recursively(fs::DEFAULT_SAVE_ROOT));
        const bool commitError = error::fslib(fslib::commit_data_to_file_system(fs::DEFAULT_SAVE_MOUNT));
        if (deleteError || commitError)
        {
            const char *popErrorResetting = strings::get_by_name(strings::names::BACKUPMENU_POPS, 2);
            ui::PopMessageManager::push_message(popTicks, popErrorResetting);
            TASK_FINISH_RETURN(task);
        }
    }

    read_and_process_meta(backup, castData, task);
    {
        FsSaveDataExtraData extraData{};
        const bool readExtra      = fs::read_save_extra_data(saveInfo, extraData);
        const uint8_t saveType    = user->get_account_save_type();
        const int64_t journalSize = readExtra ? extraData.journal_size : titleInfo->get_journal_size(saveType);
        fs::ScopedSaveMount saveMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
        fs::copy_zip_to_directory(backup, fs::DEFAULT_SAVE_ROOT, journalSize, task);
    }
    backup.close();

    const bool deleteError = error::fslib(fslib::delete_file(tempPath));
    if (deleteError)
    {
        const char *popErrorDeleting = strings::get_by_name(strings::names::BACKUPMENU_POPS, 4);
        ui::PopMessageManager::push_message(popTicks, popErrorDeleting);
    }

    spawningState->save_data_written();
    task->complete();
}

void tasks::backup::delete_backup_local(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    const fslib::Path &path        = castData->path;
    BackupMenuState *spawningState = castData->spawningState;

    if (error::is_null(task)) { return; }
    else if (error::is_null(spawningState)) { TASK_FINISH_RETURN(task); }

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    {
        const std::string pathString = path.string();
        const char *statusFormat     = strings::get_by_name(strings::names::IO_STATUSES, 3);
        std::string status           = stringutil::get_formatted_string(statusFormat, pathString.c_str());
        task->set_status(status);
    }

    const bool trashEnabled = config::get_by_key(config::keys::ENABLE_TRASH_BIN);
    const bool isDir        = fslib::directory_exists(path);
    bool dirError{}, fileError{};
    if (trashEnabled)
    {
        const fslib::Path newPath{config::get_working_directory() / "_TRASH_" / path.get_filename()};
        dirError  = isDir && error::fslib(fslib::rename_directory(path, newPath));
        fileError = !isDir && error::fslib(fslib::rename_file(path, newPath));
    }
    else
    {
        dirError  = isDir && error::fslib(fslib::delete_directory_recursively(path));
        fileError = !isDir && error::fslib(fslib::delete_file(path));
    }

    if (dirError || fileError)
    {
        const char *popFailed = strings::get_by_name(strings::names::BACKUPMENU_POPS, 4);
        ui::PopMessageManager::push_message(popTicks, popFailed);
    }

    spawningState->refresh();
    task->complete();
}

void tasks::backup::delete_backup_remote(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    remote::Item *target           = castData->remoteItem;
    BackupMenuState *spawningState = castData->spawningState;
    remote::Storage *remote        = remote::get_remote_storage();

    if (error::is_null(task)) { return; }
    else if (error::is_null({target, spawningState, remote})) { TASK_FINISH_RETURN(task); }

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    {
        const char *targetName     = target->get_name().data();
        const char *statusTemplate = strings::get_by_name(strings::names::IO_STATUSES, 3);
        const std::string status   = stringutil::get_formatted_string(statusTemplate, targetName);
        task->set_status(status);
    }

    const bool deleted = remote->delete_item(target);
    if (!deleted)
    {
        const char *popFailed = strings::get_by_name(strings::names::BACKUPMENU_POPS, 4);
        ui::PopMessageManager::push_message(popTicks, popFailed);
    }

    spawningState->refresh();
    task->complete();
}

void tasks::backup::upload_backup(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    const fslib::Path &path        = castData->path;
    BackupMenuState *spawningState = castData->spawningState;
    remote::Storage *remote        = remote::get_remote_storage();

    if (error::is_null(task)) { return; }
    else if (error::is_null({spawningState, remote})) { TASK_FINISH_RETURN(task); }

    {
        const char *filename     = path.get_filename();
        const char *statusFormat = strings::get_by_name(strings::names::IO_STATUSES, 5);
        std::string status       = stringutil::get_formatted_string(statusFormat, filename);
        task->set_status(status);
    }

    // The backup menu should've made sure the remote is pointing to the correct location.
    remote->upload_file(path, path.get_filename(), task);
    spawningState->refresh();
    task->complete();
}

void tasks::backup::patch_backup(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<BackupMenuState::DataStruct>(taskData);

    sys::ProgressTask *task        = static_cast<sys::ProgressTask *>(castData->task);
    const fslib::Path &path        = castData->path;
    remote::Item *remoteItem       = castData->remoteItem;
    BackupMenuState *spawningState = castData->spawningState;
    remote::Storage *remote        = remote::get_remote_storage();
    if (error::is_null(task)) { return; }
    else if (error::is_null({remoteItem, spawningState, remote})) { TASK_FINISH_RETURN(task); }

    {
        const char *filename     = path.get_filename();
        const char *statusFormat = strings::get_by_name(strings::names::IO_STATUSES, 5);
        std::string status       = stringutil::get_formatted_string(statusFormat, filename);
        task->set_status(status);
    }

    remote->patch_file(remoteItem, path, task);
    task->complete();
}

static void auto_backup(sys::ProgressTask *task, BackupMenuState::TaskData taskData)
{
    if (error::is_null(task)) { return; }

    remote::Storage *remote        = remote::get_remote_storage();
    data::User *user               = taskData->user;
    data::TitleInfo *titleInfo     = taskData->titleInfo;
    const FsSaveDataInfo *saveInfo = taskData->saveInfo;
    fslib::Path &target            = taskData->path;
    if (error::is_null({user, titleInfo, saveInfo})) { return; }

    {
        fs::ScopedSaveMount testMount{fs::DEFAULT_SAVE_MOUNT, saveInfo, false};
        const bool hasData = fs::directory_has_contents(fs::DEFAULT_SAVE_ROOT);
        if (!testMount.is_open() || !hasData) { return; }
    }

    const bool autoUpload = config::get_by_key(config::keys::AUTO_UPLOAD);
    const bool exportZip  = config::get_by_key(config::keys::EXPORT_TO_ZIP);
    const bool zip        = autoUpload || exportZip;

    const char *safeNickname     = user->get_path_safe_nickname();
    const std::string dateString = stringutil::get_date_string();
    std::string backupName       = stringutil::get_formatted_string("AUTO - %s - %s", safeNickname, dateString.c_str());
    if (zip) { backupName += STRING_ZIP_EXT; }

    taskData->killTask = false;

    if (autoUpload && remote)
    {
        taskData->remoteName = std::move(backupName);

        tasks::backup::create_new_backup_remote(taskData);
    }
    else
    {
        // We're going to get the target dir from the path passed.
        const size_t lastSlash = target.find_last_of('/');
        if (lastSlash == target.NOT_FOUND) { return; }

        fslib::Path autoTarget{target.sub_path(lastSlash) / backupName};

        // This is used to move and store the path before using the local auto path.
        fslib::Path storePath = std::move(taskData->path);
        // Swap em.
        taskData->path = std::move(autoTarget);

        tasks::backup::create_new_backup_local(taskData);

        // Swap em back.
        taskData->path = std::move(storePath);
    }
}

static bool read_and_process_meta(const fslib::Path &targetDir, BackupMenuState::TaskData taskData, sys::ProgressTask *task)
{
    data::User *user               = taskData->user;
    data::TitleInfo *titleInfo     = taskData->titleInfo;
    const FsSaveDataInfo *saveInfo = taskData->saveInfo;
    if (error::is_null(task)) { return false; }
    else if (error::is_null({user, titleInfo, saveInfo})) { return false; }

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    {
        const char *statusProcessing = strings::get_by_name(strings::names::BACKUPMENU_STATUS, 0);
        task->set_status(statusProcessing);
    }

    const fslib::Path metaPath{targetDir / fs::NAME_SAVE_META};
    fslib::File metaFile{metaPath, FsOpenMode_Read};
    if (!metaFile.is_open())
    {
        const char *popErrorProcessing = strings::get_by_name(strings::names::BACKUPMENU_POPS, 16);
        ui::PopMessageManager::push_message(popTicks, popErrorProcessing);
        return false;
    }

    fs::SaveMetaData metaData{};
    const bool metaRead  = metaFile.read(&metaData, fs::SIZE_SAVE_META) <= fs::SIZE_SAVE_META;
    const bool processed = metaRead && fs::process_save_meta_data(saveInfo, metaData);
    if (!metaRead || !processed)
    {
        const char *popErrorProcessing = strings::get_by_name(strings::names::BACKUPMENU_POPS, 11);
        ui::PopMessageManager::push_message(popTicks, popErrorProcessing);
        return false;
    }

    return true;
}

static bool read_and_process_meta(fs::MiniUnzip &unzip, BackupMenuState::TaskData taskData, sys::ProgressTask *task)
{
    if (error::is_null(task)) { return false; }

    data::User *user               = taskData->user;
    data::TitleInfo *titleInfo     = taskData->titleInfo;
    const FsSaveDataInfo *saveInfo = taskData->saveInfo;
    if (error::is_null({user, titleInfo, saveInfo})) { return false; }

    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;
    {
        const char *statusProcessing = strings::get_by_name(strings::names::BACKUPMENU_STATUS, 0);
        task->set_status(statusProcessing);
    }

    fs::SaveMetaData saveMeta{};
    const bool metaFound = unzip.locate_file(fs::NAME_SAVE_META);
    if (!metaFound)
    {
        const char *popNotFound = strings::get_by_name(strings::names::BACKUPMENU_POPS, 16);
        ui::PopMessageManager::push_message(popTicks, popNotFound);
        return false;
    }

    const bool metaRead      = metaFound && unzip.read(&saveMeta, fs::SIZE_SAVE_META) <= fs::SIZE_SAVE_META;
    const bool metaProcessed = metaRead && fs::process_save_meta_data(saveInfo, saveMeta);
    if (!metaRead || !metaProcessed)
    {
        const char *popErrorProcessing = strings::get_by_name(strings::names::BACKUPMENU_POPS, 11);
        ui::PopMessageManager::push_message(popTicks, popErrorProcessing);
        return false;
    }

    return true;
}

static void write_meta_file(const fslib::Path &target, const FsSaveDataInfo *saveInfo)
{
    const int popTicks              = ui::PopMessageManager::DEFAULT_TICKS;
    const char *popErrorWritingMeta = strings::get_by_name(strings::names::BACKUPMENU_POPS, 8);

    fs::SaveMetaData saveMeta{};
    const fslib::Path metaPath{target / fs::NAME_SAVE_META};
    const bool hasMeta = fs::fill_save_meta_data(saveInfo, saveMeta);
    fslib::File metaFile{metaPath, FsOpenMode_Create | FsOpenMode_Write, fs::SIZE_SAVE_META};
    if (!metaFile.is_open() || !hasMeta)
    {
        ui::PopMessageManager::push_message(popTicks, popErrorWritingMeta);
        return;
    }

    const bool metaWritten = metaFile.write(&saveMeta, fs::SIZE_SAVE_META) <= fs::SIZE_SAVE_META;
    if (!metaWritten) { ui::PopMessageManager::push_message(popTicks, popErrorWritingMeta); }
}

static void write_meta_zip(fs::MiniZip &zip, const FsSaveDataInfo *saveInfo)
{
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;

    fs::SaveMetaData saveMeta{};
    const bool hasMeta   = fs::fill_save_meta_data(saveInfo, saveMeta);
    const bool openMeta  = hasMeta && zip.open_new_file(fs::NAME_SAVE_META);
    const bool writeMeta = openMeta && zip.write(&saveMeta, fs::SIZE_SAVE_META);
    const bool closeMeta = openMeta && zip.close_current_file();
    if (hasMeta && (!openMeta || !writeMeta || !closeMeta))
    {
        const char *popErrorWritingMeta = strings::get_by_name(strings::names::BACKUPMENU_POPS, 8);
        ui::PopMessageManager::push_message(popTicks, popErrorWritingMeta);
    }
}

static fs::ScopedSaveMount create_scoped_mount(const FsSaveDataInfo *saveInfo)
{
    const int popTicks = ui::PopMessageManager::DEFAULT_TICKS;

    fs::ScopedSaveMount saveMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
    if (!saveMount.is_open())
    {

        const char *popErrorMounting = strings::get_by_name(strings::names::BACKUPMENU_POPS, 14);
        ui::PopMessageManager::push_message(popTicks, popErrorMounting);
    }
    return saveMount;
}
