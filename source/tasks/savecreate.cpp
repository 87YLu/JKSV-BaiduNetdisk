#include "tasks/savecreate.hpp"

#include "error.hpp"
#include "fs/fs.hpp"
#include "strings/strings.hpp"
#include "stringutil.hpp"
#include "ui/PopMessageManager.hpp"

void tasks::savecreate::create_save_data_for(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<SaveCreateState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    SaveCreateState *spawningState = castData->spawningState;

    const bool isCache       = user->get_account_save_type() == FsSaveDataType_Cache;
    const uint16_t saveIndex = isCache ? castData->saveDataIndex : 0;

    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, spawningState})) { TASK_FINISH_RETURN(task); }

    const int popTicks       = ui::PopMessageManager::DEFAULT_TICKS;
    const char *statusFormat = strings::get_by_name(strings::names::USEROPTION_STATUS, 0);
    const char *popSuccess   = strings::get_by_name(strings::names::SAVECREATE_POPS, 0);
    const char *popFailed    = strings::get_by_name(strings::names::SAVECREATE_POPS, 1);
    const char *title        = titleInfo->get_title();

    {
        std::string status = stringutil::get_formatted_string(statusFormat, title);
        task->set_status(status);
    }

    const bool saveCreated = fs::create_save_data_for(user, titleInfo, saveIndex);
    if (!saveCreated) { ui::PopMessageManager::push_message(popTicks, popFailed); }
    else
    {
        std::string popMessage = stringutil::get_formatted_string(popSuccess, title);
        ui::PopMessageManager::push_message(popTicks, popMessage);
        spawningState->refresh_required();
    }

    task->complete();
}

void tasks::savecreate::create_save_data_for_sd(sys::threadpool::JobData taskData)
{
    auto castData = std::static_pointer_cast<SaveCreateState::DataStruct>(taskData);

    sys::Task *task                = castData->task;
    data::User *user               = castData->user;
    data::TitleInfo *titleInfo     = castData->titleInfo;
    SaveCreateState *spawningState = castData->spawningState;

    bool isCache             = user->get_account_save_type() == FsSaveDataType_Cache;
    const uint16_t saveIndex = isCache ? castData->saveDataIndex : 0;

    if (error::is_null(task)) { return; }
    else if (error::is_null({user, titleInfo, spawningState})) { TASK_FINISH_RETURN(task); }

    const int popTicks       = ui::PopMessageManager::DEFAULT_TICKS;
    const char *statusFormat = strings::get_by_name(strings::names::USEROPTION_STATUS, 0);
    const char *popSuccess   = strings::get_by_name(strings::names::SAVECREATE_POPS, 0);
    const char *popFailed    = strings::get_by_name(strings::names::SAVECREATE_POPS, 1);
    const char *title        = titleInfo->get_title();

    {
        std::string status = stringutil::get_formatted_string(statusFormat, title);
        task->set_status(status);
    }

    const bool saveCreated = fs::create_save_data_for(user, titleInfo, saveIndex, 4);
    if (!saveCreated) { ui::PopMessageManager::push_message(popTicks, popFailed); }
    else
    {
        std::string popMessage = stringutil::get_formatted_string(popSuccess, title);
        ui::PopMessageManager::push_message(popTicks, popMessage);
        spawningState->refresh_required();
    }

    task->complete();
}