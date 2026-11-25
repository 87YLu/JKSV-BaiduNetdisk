#include "common.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace
{
    constexpr int32_t PLAY_LOG_BATCH = 16;

    // Without a saved position this is the first run since installing or signing in. Only the most recent game should
    // count then, not everything that's ever been played.
    constexpr int32_t FIRST_RUN_EVENTS = 32;

    // The process exit can reach the monitor before the matching entry reaches the play log.
    constexpr int PLAY_LOG_WAIT_SECONDS = 5;

    // A bound on how much of the log one run reads, in case entry indexes ever stop increasing.
    constexpr int32_t MAX_EVENTS_PER_RUN = 4096;

    bool valid_game_title(uint64_t titleId)
    {
        return titleId >= 0x0100000000010000ULL && titleId < 0x0200000000000000ULL;
    }

    bool read_position(int32_t &position)
    {
        FILE *file = std::fopen(background::PLAY_LOG_POSITION_PATH, "rb");
        if (!file) { return false; }
        const bool read = std::fscanf(file, "%d", &position) == 1;
        std::fclose(file);
        return read;
    }

    bool is_exit(uint8_t eventType)
    {
        return eventType == PdmAppletEventType_Exit || eventType == PdmAppletEventType_Exit5 ||
               eventType == PdmAppletEventType_Exit6;
    }

    /// @return False if the play log couldn't be read at all.
    bool read_closed_titles(std::vector<uint64_t> &titles, int32_t &newestEntry)
    {
        int32_t total{}, first{}, last{};
        if (R_FAILED(pdmqryGetAvailablePlayEventRange(&total, &first, &last)))
        {
            background::log("PLAY_LOG_UNAVAILABLE stage=range");
            return false;
        }

        int32_t previous{};
        bool firstRun = !read_position(previous);

        // A position outside of the log means it isn't this log's position: a system reset, or switching between
        // sysMMC and emuMMC, which share the SD card but not the play log. Starting over beats never finding a game.
        if (!firstRun && (previous > last || previous < first - 1))
        {
            background::logf("PLAY_LOG_RESET saved=%d range=%d-%d", previous, first, last);
            firstRun = true;
        }

        int32_t index       = firstRun ? std::max(first, last - FIRST_RUN_EVENTS) : std::max(first, previous + 1);
        newestEntry         = firstRun ? last : previous;
        int32_t eventsRead  = 0;

        std::array<PdmAppletEvent, PLAY_LOG_BATCH> events{};
        while (true)
        {
            int32_t count{};
            if (R_FAILED(pdmqryQueryAppletEvent(index, true, events.data(), PLAY_LOG_BATCH, &count)))
            {
                background::log("PLAY_LOG_UNAVAILABLE stage=query");
                return false;
            }
            if (count <= 0 || (eventsRead += count) > MAX_EVENTS_PER_RUN) { break; }

            for (int32_t i = 0; i < count; ++i)
            {
                const PdmAppletEvent &event = events[i];
                const int32_t entry         = static_cast<int32_t>(event.entry_index);
                if (entry < index) { return true; }
                newestEntry = std::max(newestEntry, entry);
                index       = entry + 1;
                if (!is_exit(event.event_type) || !valid_game_title(event.program_id)) { continue; }

                if (firstRun) { titles.clear(); }
                if (std::find(titles.begin(), titles.end(), event.program_id) == titles.end())
                {
                    titles.push_back(event.program_id);
                }
            }
        }
        return true;
    }
} // namespace

// Process events only say a process ID, and the process is already gone by then. Retail games never get a start event, so
// there's no earlier chance to find out which game it was. The play log has the title, and since the position is saved,
// a closed game that something missed is still picked up the next time this runs.
bool background::closed_titles_since_last_run(std::vector<uint64_t> &titlesOut, int32_t &newestEntryOut)
{
    titlesOut.clear();
    for (int second = 0; second <= PLAY_LOG_WAIT_SECONDS && titlesOut.empty(); ++second)
    {
        if (second > 0) { background::sleep_seconds(1); }
        if (!read_closed_titles(titlesOut, newestEntryOut)) { return false; }
    }

    for (const uint64_t title : titlesOut)
    {
        background::logf("GAME_CLOSE title=%016llX source=play-log", static_cast<unsigned long long>(title));
    }
    return true;
}

void background::save_play_log_position(int32_t entry)
{
    const std::string temporary = std::string(background::PLAY_LOG_POSITION_PATH) + ".tmp";
    FILE *file                  = std::fopen(temporary.c_str(), "wb");
    if (!file) { return; }
    const bool written = std::fprintf(file, "%d\n", entry) > 0 && std::fflush(file) == 0;
    std::fclose(file);

    if (!written || !background::atomic_replace(temporary, background::PLAY_LOG_POSITION_PATH))
    {
        background::remove_file(temporary);
    }
}
