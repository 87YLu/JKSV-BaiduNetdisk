#pragma once
#include <string>

namespace stringutil
{
    /// @brief Enum for creating date strings.
    enum class DateFormat : uint8_t
    {
        Year_Month_Day,
        Year_Day_Month,
        YearMonthDay,
        YearDayMonth,
        AscTime
    };

    /// @brief Returns a formatted string as a C++ string. This uses C instead of std::format because std::format bloats the NRO
    /// up another whole MB.
    /// @param format Format of string.
    /// @param arguments Arguments for string.
    /// @return Formatted C++ string.
    std::string get_formatted_string(const char *format, ...);

    /// @brief Replaces a sequence of characters in a string.
    /// @param target Target string.
    /// @param find Sequence to search for.
    /// @param replace What to replace the sequence with.
    void replace_in_string(std::string &target, std::string_view find, std::string_view replace);

    /// @brief Strips a character from the target string.
    /// @param c Character to strip.
    /// @param target Target to strip the character from.
    void strip_character(char c, std::string &target);

    /// @brief Attempts to sanitize the string for use with the SD card.
    /// @param stringIn String to attempt to sanitize.
    /// @param stringOut Buffer to write result to.
    /// @param stringOutSize Size of buffer.
    /// @return True if the string was able to be sanitized. False if it's impossible.
    bool sanitize_string_for_path(const char *stringIn, char *stringOut, size_t stringOutSize);

    /// @brief Returns a date string.
    /// @param format Optional. Format to use. Default is Year_Month_Day-Time
    /// @return Date string.
    std::string get_date_string(stringutil::DateFormat format = stringutil::DateFormat::Year_Month_Day);
} // namespace stringutil
