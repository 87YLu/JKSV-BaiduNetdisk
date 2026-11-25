#pragma once

#include <algorithm>
#include <string>
#include <string_view>

/// @brief Cloud folder names shared by JKSV and the background worker. Both must produce exactly the same name for the
/// same game, or backups made by one won't show up in the other.
namespace baidu_naming
{
    inline bool is_continuation(unsigned char value) { return (value & 0xC0) == 0x80; }

    inline size_t utf8_sequence_length(unsigned char lead)
    {
        if (lead < 0x80) { return 1; }
        if ((lead & 0xE0) == 0xC0) { return 2; }
        if ((lead & 0xF0) == 0xE0) { return 3; }
        if ((lead & 0xF8) == 0xF0) { return 4; }
        return 0;
    }

    /// @brief Keeps UTF-8 and replaces what Baidu doesn't allow in names with '_'. Invalid UTF-8 becomes '_' too.
    inline std::string sanitize_component(std::string_view value, size_t maximumBytes = 160)
    {
        std::string output{};
        output.reserve(std::min(value.size(), maximumBytes));

        for (size_t index = 0; index < value.size();)
        {
            const unsigned char lead = static_cast<unsigned char>(value[index]);
            if (lead < 0x80)
            {
                char character = static_cast<char>(lead);
                if (lead < 0x20 || character == '/' || character == '\\' || character == ':' || character == '*' ||
                    character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
                {
                    character = '_';
                }
                if (output.size() + 1 > maximumBytes) { break; }
                output.push_back(character);
                ++index;
                continue;
            }

            const size_t sequenceLength = utf8_sequence_length(lead);
            bool valid = sequenceLength > 1 && index + sequenceLength <= value.size();
            for (size_t byte = 1; valid && byte < sequenceLength; ++byte)
            {
                valid = is_continuation(static_cast<unsigned char>(value[index + byte]));
            }
            if (!valid)
            {
                if (output.size() + 1 <= maximumBytes) { output.push_back('_'); }
                ++index;
                continue;
            }
            if (output.size() + sequenceLength > maximumBytes) { break; }
            output.append(value.substr(index, sequenceLength));
            index += sequenceLength;
        }

        const size_t first = output.find_first_not_of(" .");
        if (first == std::string::npos) { return "Unknown"; }
        output.erase(0, first);
        while (!output.empty() && (output.back() == ' ' || output.back() == '.')) { output.pop_back(); }
        return output.empty() ? "Unknown" : output;
    }

    /// @brief The cloud folder for a game: its name in the console's language, made safe for Baidu.
    /// @param title The game's name from its control data. Both sides fall back to the title ID in hex without one.
    inline std::string title_directory(std::string_view title) { return sanitize_component(title); }
} // namespace baidu_naming
