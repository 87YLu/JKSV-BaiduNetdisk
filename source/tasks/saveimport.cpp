#include "tasks/saveimport.hpp"

#include "appstates/MainMenuState.hpp"
#include "appstates/SaveImportState.hpp"
#include "error.hpp"
#include "fs/fs.hpp"
#include "fslib.hpp"
#include "logging/logger.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "ui/PopMessageManager.hpp"

// Defined at bottom.
static bool read_save_meta(const fslib::Path &path, fs::SaveMetaData &metaOut);
static bool test_for_save(data::User *user, const fs::SaveMetaData &saveMeta);
static bool create_save_data_from_meta(sys::ProgressTask *task, data::User *user, const fs::SaveMetaData &saveMeta);

void tasks::saveimport::import_save_backup(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<SaveImportState::DataStruct>(taskData);

    sys::ProgressTask *task   = static_cast<sys::ProgressTask *>(castData->task);
    data::User *user          = castData->user;
    const fslib::Path &target = castData->path;
    if (error::is_null(task)) { return; }
    else if (error::is_null(user)) { TASK_FINISH_RETURN(task); }

    fs::SaveMetaData metaData{};
    const bool metaRead = read_save_meta(target, metaData);
    if (!metaRead || metaData.revision < 1) { TASK_FINISH_RETURN(task); }

    const bool saveExists  = test_for_save(user, metaData);
    const bool saveCreated = !saveExists && create_save_data_from_meta(task, user, metaData);
    if (!saveExists && !saveCreated) { TASK_FINISH_RETURN(task); }

    const FsSaveDataInfo *saveInfo = user->get_save_info_by_id(metaData.applicationID);
    if (error::is_null(saveInfo)) { TASK_FINISH_RETURN(task); }

    {
        fs::ScopedSaveMount saveMount{fs::DEFAULT_SAVE_MOUNT, saveInfo};
        if (!saveMount.is_open()) { TASK_FINISH_RETURN(task); }

        const bool isDir = fslib::directory_exists(target);
        if (isDir) { fs::copy_directory_commit(target, fs::DEFAULT_SAVE_ROOT, metaData.journalSize, task); }
        else
        {
            fs::MiniUnzip unzip{target};
            if (!unzip.is_open()) { TASK_FINISH_RETURN(task); }

            fs::copy_zip_to_directory(unzip, fs::DEFAULT_SAVE_ROOT, metaData.journalSize, task);
        }
    }

    task->complete();
}

static bool read_save_meta(const fslib::Path &path, fs::SaveMetaData &metaOut)
{
    const bool isDir = fslib::directory_exists(path);
    if (isDir)
    {
        const fslib::Path metaPath{path / fs::NAME_SAVE_META};
        const bool exists = fslib::file_exists(metaPath);
        if (!exists) { return false; }

        fslib::File metaFile{metaPath, FsOpenMode_Read};
        const bool metaRead = metaFile.is_open() && metaFile.get_size() <= fs::SIZE_SAVE_META &&
                              metaFile.read(&metaOut, fs::SIZE_SAVE_META) <= fs::SIZE_SAVE_META;
        const bool magicMatch = metaRead && metaOut.magic == fs::SAVE_META_MAGIC;
        if (!metaRead || !magicMatch) { return false; }
    }
    else
    {
        fs::MiniUnzip zip{path};
        if (!zip.is_open()) { return false; }

        const bool located    = zip.locate_file(fs::NAME_SAVE_META);
        const bool sizeMatch  = located && zip.get_uncompressed_size() <= fs::SIZE_SAVE_META;
        const bool metaRead   = sizeMatch && zip.read(&metaOut, fs::SIZE_SAVE_META) <= fs::SIZE_SAVE_META;
        const bool magicMatch = metaRead && metaOut.magic == fs::SAVE_META_MAGIC;
        if (!metaRead || !magicMatch) { return false; }
    }

    return true;
}

static bool test_for_save(data::User *user, const fs::SaveMetaData &saveMeta)
{
    const FsSaveDataType saveType = user->get_account_save_type();

    bool mounted{};
    switch (saveType)
    {
        case FsSaveDataType_Account:
        {
            mounted =
                fslib::open_account_save_file_system(fs::DEFAULT_SAVE_MOUNT, saveMeta.applicationID, user->get_account_id());
        }
        break;

        case FsSaveDataType_Bcat:
        {
            mounted = fslib::open_bcat_save_file_system(fs::DEFAULT_SAVE_MOUNT, saveMeta.applicationID);
        }
        break;

        case FsSaveDataType_Device:
        {
            mounted = fslib::open_device_save_file_system(fs::DEFAULT_SAVE_MOUNT, saveMeta.applicationID);
        }
        break;

        case FsSaveDataType_Cache:
        {
            mounted = fslib::open_cache_save_file_system(fs::DEFAULT_SAVE_MOUNT,
                                                         saveMeta.applicationID,
                                                         saveMeta.saveDataIndex,
                                                         static_cast<FsSaveDataSpaceId>(saveMeta.saveDataIndex));
        }
        break;

        default: return false;
    }

    if (mounted) { fslib::close_file_system(fs::DEFAULT_SAVE_MOUNT); }

    return mounted;
}

static bool create_save_data_from_meta(sys::ProgressTask *task, data::User *user, const fs::SaveMetaData &saveMeta)
{
    if (error::is_null({task, user})) { return false; }

    // Gonna borrow this.
    const char *statusFormat = strings::get_by_name(strings::names::USEROPTION_STATUS, 0);
    const std::string hexID  = stringutil::get_formatted_string("%016llX", saveMeta.applicationID);
    std::string status       = stringutil::get_formatted_string(statusFormat, hexID.c_str());
    task->set_status(status);

    const bool saveCreated = fs::create_save_data_for(user, saveMeta);
    if (!saveCreated)
    {
        // Gonna borrow this too.
        const char *popFailed = strings::get_by_name(strings::names::SAVECREATE_POPS, 1);
        ui::PopMessageManager::push_message(ui::PopMessageManager::DEFAULT_TICKS, popFailed);
        return false;
    }

    // Need to reload and refresh to reflect changes.
    user->clear_data_entries();
    user->load_user_data();
    MainMenuState::refresh_view_states();

    return true;
}