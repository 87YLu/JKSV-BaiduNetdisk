#include "stringutil.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <span>
#include <string>
#include <switch.h>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr std::array<uint32_t, 13> FORBIDDEN =
        {L',', L'/', L'\\', L'<', L'>', L':', L'"', L'|', L'?', L'*', L'™', L'©', L'®'};
}

// Defined at bottom.
static const std::unordered_map<uint32_t, std::string_view> &get_replacement_table();

std::string stringutil::get_formatted_string(const char *format, ...)
{
    static constexpr size_t VA_BUFFER_SIZE = 0x1000;

    std::array<char, VA_BUFFER_SIZE> vaBuffer = {0};

    std::va_list vaList;
    va_start(vaList, format);
    vsnprintf(vaBuffer.data(), VA_BUFFER_SIZE, format, vaList);
    va_end(vaList);

    return std::string(vaBuffer.data());
}

void stringutil::replace_in_string(std::string &target, std::string_view find, std::string_view replace)
{
    const size_t findLength    = find.length();
    const size_t replaceLength = replace.length();

    for (size_t i = target.find(find); i != target.npos; i = target.find(find, i + replaceLength))
    {
        target.replace(i, findLength, replace);
    }
}

void stringutil::strip_character(char c, std::string &target)
{
    for (size_t i = target.find_first_of(c); i != target.npos; i = target.find_first_of(c, i))
    {
        target.erase(target.begin() + i);
    }
}

bool stringutil::sanitize_string_for_path(const char *stringIn, char *stringOut, size_t stringOutSize)
{
    uint32_t codepoint{};
    const int length             = std::char_traits<char>::length(stringIn);
    const auto &replacementTable = get_replacement_table();

    for (int i = 0, outOffset = 0; i < length;)
    {
        const uint8_t *point = reinterpret_cast<const uint8_t *>(&stringIn[i]);
        const ssize_t count  = decode_utf8(&codepoint, point);
        if (count <= 0 || outOffset + count >= static_cast<int>(stringOutSize)) { return false; }

        // If it's forbidden, skip.
        const bool isForbidden = std::find(FORBIDDEN.begin(), FORBIDDEN.end(), codepoint) != FORBIDDEN.end();
        if (isForbidden)
        {
            i += count;
            continue;
        }

        // Check for replacing.
        const auto replace = replacementTable.find(codepoint);
        if (replace != replacementTable.end())
        {
            const auto &[tablePoint, replacement] = *replace;
            const size_t replacementLength        = replacement.length();

            const size_t remainingSize = stringOutSize - outOffset;
            const std::span<char> stringSpan{&stringOut[outOffset], remainingSize};
            std::copy(replacement.begin(), replacement.end(), stringSpan.begin());

            outOffset += replacementLength;
            i += count;
            continue;
        }

        // Final valid ASCII check.
        const bool asciiCheck = codepoint < 0x20 || codepoint >= 0x7F;
        if (asciiCheck) { return false; }

        // Just copy it over.
        const size_t remainingSpace = stringOutSize - outOffset;
        const std::span<const char> stringSpan{&stringIn[i], static_cast<size_t>(count)};
        const std::span<char> stringOutSpan{&stringOut[outOffset], remainingSpace};
        std::copy(stringSpan.begin(), stringSpan.end(), stringOutSpan.begin());

        outOffset += count;
        i += count;
    }

    const int outLength = std::char_traits<char>::length(stringOut) - 1;
    for (int i = outLength; i > 0 && (stringOut[i] == ' ' || stringOut[i] == '.'); i--) { stringOut[i] = '\0'; }

    return true;
}

std::string stringutil::get_date_string(stringutil::DateFormat format)
{
    // This is the size of the buffer used for C functions.
    static constexpr size_t STRING_BUFFER_SIZE = 0x80;

    // Grab local system time.
    std::time_t timer{};
    std::time(&timer);
    const std::tm localTime = *std::localtime(&timer);

    // String to return
    std::string returnString{};

    switch (format)
    {
        case stringutil::DateFormat::Year_Month_Day:
        {
            // These are like this because the final one needs a character stripped from the string to be safe.
            char buffer[STRING_BUFFER_SIZE]{};
            std::strftime(buffer, STRING_BUFFER_SIZE, "%Y-%m-%d_%H-%M-%S", &localTime);
            returnString = buffer;
        }
        break;

        case stringutil::DateFormat::Year_Day_Month:
        {
            char buffer[STRING_BUFFER_SIZE]{};
            std::strftime(buffer, STRING_BUFFER_SIZE, "%Y-%d-%m_%H-%M-%S", &localTime);
            returnString = buffer;
        }
        break;

        case stringutil::DateFormat::YearMonthDay:
        {
            char buffer[STRING_BUFFER_SIZE]{};
            std::strftime(buffer, STRING_BUFFER_SIZE, "%Y%d%m_%H%M%S", &localTime);
            returnString = buffer;
        }
        break;

        case stringutil::DateFormat::YearDayMonth:
        {
            char buffer[STRING_BUFFER_SIZE]{};
            std::strftime(buffer, STRING_BUFFER_SIZE, "%Y%m%d_%H%M%S", &localTime);
            returnString = buffer;
        }
        break;

        case stringutil::DateFormat::AscTime:
        {
            // Just assign to asctime.
            returnString = std::asctime(&localTime);
            // Strip the colons.
            stringutil::replace_in_string(returnString, ":", "-");
            stringutil::strip_character('\n', returnString);
        }
        break;
    }

    return returnString;
}

static const std::unordered_map<uint32_t, std::string_view> &get_replacement_table()
{
    static const std::unordered_map<uint32_t, std::string_view> replacementTable = {
        {L'Á', "A"},  {L'À', "A"},  {L'Â', "A"},   {L'Ä', "A"},    {L'Ã', "A"},  {L'Å', "A"},  {L'Ā', "A"},   {L'Ă', "A"},
        {L'Ą', "A"},  {L'Ǎ', "A"},  {L'Ǻ', "A"},   {L'Ȁ', "A"},    {L'Ȃ', "A"},  {L'Ǟ', "A"},  {L'Ǡ', "A"},   {L'á', "a"},
        {L'à', "a"},  {L'â', "a"},  {L'ä', "a"},   {L'ã', "a"},    {L'å', "a"},  {L'ā', "a"},  {L'ă', "a"},   {L'ą', "a"},
        {L'ǎ', "a"},  {L'ǻ', "a"},  {L'ȁ', "a"},   {L'ȃ', "a"},    {L'ǟ', "a"},  {L'ǡ', "a"},  {L'Æ', "AE"},  {L'æ', "ae"},
        {L'Ǣ', "AE"}, {L'ǣ', "ae"}, {L'Ɓ', "B"},   {L'ƀ', "b"},    {L'Ƃ', "B"},  {L'ƃ', "b"},  {L'Ç', "C"},   {L'ç', "c"},
        {L'Ć', "C"},  {L'ć', "c"},  {L'Ĉ', "C"},   {L'ĉ', "c"},    {L'Ċ', "C"},  {L'ċ', "c"},  {L'Č', "C"},   {L'č', "c"},
        {L'Ƈ', "C"},  {L'ƈ', "c"},  {L'Ď', "D"},   {L'ď', "d"},    {L'Đ', "D"},  {L'đ', "d"},  {L'Ɖ', "D"},   {L'ƌ', "d"},
        {L'Ḑ', "D"},  {L'ḑ', "d"},  {L'Ḓ', "D"},   {L'ḓ', "d"},    {L'É', "E"},  {L'È', "E"},  {L'Ê', "E"},   {L'Ë', "E"},
        {L'Ē', "E"},  {L'Ĕ', "E"},  {L'Ė', "E"},   {L'Ę', "E"},    {L'Ě', "E"},  {L'Ȅ', "E"},  {L'Ȇ', "E"},   {L'é', "e"},
        {L'è', "e"},  {L'ê', "e"},  {L'ë', "e"},   {L'ē', "e"},    {L'ĕ', "e"},  {L'ė', "e"},  {L'ę', "e"},   {L'ě', "e"},
        {L'ȅ', "e"},  {L'ȇ', "e"},  {L'Ĝ', "G"},   {L'Ğ', "G"},    {L'Ġ', "G"},  {L'Ģ', "G"},  {L'Ǵ', "G"},   {L'Ǧ', "G"},
        {L'Ǥ', "G"},  {L'ĝ', "g"},  {L'ğ', "g"},   {L'ġ', "g"},    {L'ģ', "g"},  {L'ǵ', "g"},  {L'ǧ', "g"},   {L'ǥ', "g"},
        {L'Ĥ', "H"},  {L'Ħ', "H"},  {L'Ȟ', "H"},   {L'Ḩ', "H"},    {L'ḩ', "h"},  {L'ĥ', "h"},  {L'ħ', "h"},   {L'ȟ', "h"},
        {L'Í', "I"},  {L'Ì', "I"},  {L'Î', "I"},   {L'Ï', "I"},    {L'Ī', "I"},  {L'Ĭ', "I"},  {L'Į', "I"},   {L'İ', "I"},
        {L'Ǐ', "I"},  {L'Ȉ', "I"},  {L'Ȋ', "I"},   {L'í', "i"},    {L'ì', "i"},  {L'î', "i"},  {L'ï', "i"},   {L'ī', "i"},
        {L'ĭ', "i"},  {L'į', "i"},  {L'ı', "i"},   {L'ǐ', "i"},    {L'ȉ', "i"},  {L'ȋ', "i"},  {L'Ĵ', "J"},   {L'ĵ', "j"},
        {L'Ķ', "K"},  {L'ķ', "k"},  {L'Ǩ', "K"},   {L'ǩ', "k"},    {L'Ĺ', "L"},  {L'Ļ', "L"},  {L'Ľ', "L"},   {L'Ŀ', "L"},
        {L'Ł', "L"},  {L'ĺ', "l"},  {L'ļ', "l"},   {L'ľ', "l"},    {L'ŀ', "l"},  {L'ł', "l"},  {L'Ñ', "N"},   {L'Ń', "N"},
        {L'Ņ', "N"},  {L'Ň', "N"},  {L'Ǹ', "N"},   {L'ñ', "n"},    {L'ń', "n"},  {L'ņ', "n"},  {L'ň', "n"},   {L'ǹ', "n"},
        {L'Ó', "O"},  {L'Ò', "O"},  {L'Ô', "O"},   {L'Ö', "O"},    {L'Õ', "O"},  {L'Ø', "O"},  {L'Ō', "O"},   {L'Ŏ', "O"},
        {L'Ő', "O"},  {L'Ǒ', "O"},  {L'Ȍ', "O"},   {L'Ȏ', "O"},    {L'Ǫ', "O"},  {L'Ǭ', "O"},  {L'ó', "o"},   {L'ò', "o"},
        {L'ô', "o"},  {L'ö', "o"},  {L'õ', "o"},   {L'ø', "o"},    {L'ō', "o"},  {L'ŏ', "o"},  {L'ő', "o"},   {L'ǒ', "o"},
        {L'ȍ', "o"},  {L'ȏ', "o"},  {L'ǫ', "o"},   {L'ǭ', "o"},    {L'Œ', "OE"}, {L'œ', "oe"}, {L'Ŕ', "R"},   {L'Ŗ', "R"},
        {L'Ř', "R"},  {L'ŕ', "r"},  {L'ŗ', "r"},   {L'ř', "r"},    {L'Ś', "S"},  {L'Ŝ', "S"},  {L'Ş', "S"},   {L'Š', "S"},
        {L'ś', "s"},  {L'ŝ', "s"},  {L'ş', "s"},   {L'š', "s"},    {L'ẞ', "Ss"}, {L'ß', "ss"}, {L'Ţ', "T"},   {L'Ť', "T"},
        {L'Ŧ', "T"},  {L'ţ', "t"},  {L'ť', "t"},   {L'ŧ', "t"},    {L'Ú', "U"},  {L'Ù', "U"},  {L'Û', "U"},   {L'Ü', "U"},
        {L'Ū', "U"},  {L'Ŭ', "U"},  {L'Ů', "U"},   {L'Ű', "U"},    {L'Ų', "U"},  {L'Ǔ', "U"},  {L'Ǖ', "U"},   {L'Ǘ', "U"},
        {L'Ǚ', "U"},  {L'Ǜ', "U"},  {L'Ȕ', "U"},   {L'Ȗ', "U"},    {L'ú', "u"},  {L'ù', "u"},  {L'û', "u"},   {L'ü', "u"},
        {L'ū', "u"},  {L'ŭ', "u"},  {L'ů', "u"},   {L'ű', "u"},    {L'ų', "u"},  {L'ǔ', "u"},  {L'ǖ', "u"},   {L'ǘ', "u"},
        {L'ǚ', "u"},  {L'ǜ', "u"},  {L'ȕ', "u"},   {L'ȗ', "u"},    {L'Ŵ', "W"},  {L'Ẁ', "W"},  {L'Ẃ', "W"},   {L'Ẅ', "W"},
        {L'ŵ', "w"},  {L'ẁ', "w"},  {L'ẃ', "w"},   {L'ẅ', "w"},    {L'Ý', "Y"},  {L'Ÿ', "Y"},  {L'Ŷ', "Y"},   {L'Ȳ', "Y"},
        {L'ý', "y"},  {L'ÿ', "y"},  {L'ŷ', "y"},   {L'ȳ', "y"},    {L'Ź', "Z"},  {L'Ż', "Z"},  {L'Ž', "Z"},   {L'ź', "z"},
        {L'ż', "z"},  {L'ž', "z"},  {L'‐', "-"},   {L'–', "-"},    {L'—', "-"},  {L'―', "-"},  {L'‘', "'"},   {L'’', "'"},
        {L'‛', "'"},  {L'′', "'"},  {L'ʼ', "'"},   {L'“', "\""},   {L'”', "\""}, {L'„', "\""}, {L'″', "\""},  {L'※', "*"},
        {L'×', "x"},  {L' ', " "},  {L' ', " "},   {L' ', " "},    {L'Ⅰ', "I"},  {L'Ⅱ', "II"}, {L'Ⅲ', "III"}, {L'Ⅳ', "IV"},
        {L'Ⅴ', "V"},  {L'Ⅵ', "VI"}, {L'Ⅶ', "VII"}, {L'Ⅷ', "VIII"}, {L'Ⅸ', "IX"}, {L'Ⅹ', "X"}};

    return replacementTable;
}
