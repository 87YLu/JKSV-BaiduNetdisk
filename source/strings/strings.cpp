#include "strings/strings.hpp"

#include "config/config.hpp"
#include "error.hpp"
#include "fslib.hpp"
#include "json.hpp"
#include "logging/logger.hpp"
#include "stringutil.hpp"
#include "sys/defines.hpp"

#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <zlib.h>

namespace
{
    // This is the actual map where the strings are.
    std::map<std::pair<std::string, int>, std::string> s_stringMap;

    /// @brief Simplified Chinese is the only language.
    constexpr std::string_view PATH_STRINGS = "romfs:/Text/ZHCN.json.z";
} // namespace

// Definitions at bottom.
static fslib::Path get_file_path();
static void replace_buttons_in_string(std::string &target);

bool strings::initialize()
{
    const std::string stringPath = get_file_path().string();

    // This is in the romfs, so fslib can't touch this.
    std::FILE *textFile = std::fopen(stringPath.c_str(), "rb");
    if (!textFile) { return false; }

    uLongf uncompressedSize{};
    uLongf compressedSize{};
    const bool readFullSize = std::fread(&uncompressedSize, sizeof(uint32_t), 1, textFile) == 1;
    const bool readCompSize = std::fread(&compressedSize, sizeof(uint32_t), 1, textFile) == 1;
    if (!readFullSize || !readCompSize) { return false; }

    auto readBuffer   = std::make_unique<sys::Byte[]>(compressedSize);
    auto uncompBuffer = std::make_unique<sys::Byte[]>(uncompressedSize);
    if (!readBuffer || !uncompBuffer) { return false; }

    const bool goodRead = std::fread(readBuffer.get(), 1, compressedSize, textFile) == compressedSize;
    const bool decompressed =
        goodRead && uncompress(uncompBuffer.get(), &uncompressedSize, readBuffer.get(), compressedSize) == Z_OK;
    if (!goodRead || !decompressed) { return false; }

    json::Object stringJSON = json::new_object(json_tokener_parse, reinterpret_cast<const char *>(uncompBuffer.get()));
    if (!stringJSON) { return false; }

    json_object_iterator stringIterator = json::iter_begin(stringJSON);
    json_object_iterator stringEnd      = json::iter_end(stringJSON);
    while (!json_object_iter_equal(&stringIterator, &stringEnd))
    {
        // Get name of string(s) and pointer to array
        const char *name   = json_object_iter_peek_name(&stringIterator);
        json_object *array = json_object_iter_peek_value(&stringIterator);

        // Loop through array and add them to map so I can be lazier and not have to edit code or do anything to add more
        // strings.
        size_t length = json_object_array_length(array);
        for (size_t i = 0; i < length; i++)
        {
            json_object *string     = json_object_array_get_idx(array, i);
            std::string_view slicer = json_object_get_string(string);
            const auto mapPair      = std::make_pair(name, i);

            const size_t begin = slicer.find(": ");
            if (begin != slicer.npos) { slicer = slicer.substr(begin + 2); }

            s_stringMap[mapPair] = slicer;
        }
        json_object_iter_next(&stringIterator);
    }

    // Loop through entire map and replace the buttons.
    for (auto &[key, string] : s_stringMap) { replace_buttons_in_string(string); }

    return true;
}

const char *strings::get_by_name(std::string_view name, int index) noexcept
{
    const auto mapPair  = std::make_pair(name.data(), index);
    const auto findPair = s_stringMap.find(mapPair);

    if (findPair == s_stringMap.end()) { return nullptr; }
    return findPair->second.c_str();
}

static fslib::Path get_file_path() { return fslib::Path{PATH_STRINGS}; }

static void replace_buttons_in_string(std::string &target)
{
    stringutil::replace_in_string(target, "[A]", "\ue0a0");
    stringutil::replace_in_string(target, "[B]", "\ue0a1");
    stringutil::replace_in_string(target, "[X]", "\ue0a2");
    stringutil::replace_in_string(target, "[Y]", "\ue0a3");
    stringutil::replace_in_string(target, "[L]", "\ue0a4");
    stringutil::replace_in_string(target, "[R]", "\ue0a5");
    stringutil::replace_in_string(target, "[ZL]", "\ue0a6");
    stringutil::replace_in_string(target, "[ZR]", "\ue0a7");
    stringutil::replace_in_string(target, "[SL]", "\ue0a8");
    stringutil::replace_in_string(target, "[SR]", "\ue0a9");
    stringutil::replace_in_string(target, "[DPAD]", "\ue0d0");
    stringutil::replace_in_string(target, "[DUP]", "\ue0d1");
    stringutil::replace_in_string(target, "[DDOWN]", "\ue0d2");
    stringutil::replace_in_string(target, "[DLEFT]", "\ue0d3");
    stringutil::replace_in_string(target, "[DRIGHT]", "\ue0d4");
    stringutil::replace_in_string(target, "[+]", "\ue0b3");
    stringutil::replace_in_string(target, "[-]", "\ue0b4");
}
