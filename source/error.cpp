#include "error.hpp"

#include "fslib.hpp"
#include "logging/logger.hpp"

#include <cstdio>
#include <string_view>

/// @brief Prepares and makes sure the strings match the format I actually want!
static void prep_locations(std::string_view &file, std::string_view &function, const std::source_location &location) noexcept;

bool error::libnx(Result code, const std::source_location &location) noexcept
{
    if (code == 0) { return false; }

    std::string_view file{}, function{};
    prep_locations(file, function, location);

    logger::log("%s::%s::%u::%u:%X", file.data(), function.data(), location.line(), location.column(), code);
    return true;
}

bool error::fslib(bool result, const std::source_location &location) noexcept
{
    if (result) { return false; }

    std::string_view file{}, function{};
    prep_locations(file, function, location);

    logger::log("%s::%s::%u::%u::%s",
                file.data(),
                function.data(),
                location.line(),
                location.column(),
                fslib::error::get_string());

    return true;
}

bool error::is_null(const void *pointer, const std::source_location &location) noexcept
{
    if (pointer) { return false; }

    std::string_view file{}, function{};
    prep_locations(file, function, location);

    logger::log("%s::%s::%u::%u::nullptr!", file.data(), function.data(), location.line(), location.column());

    return true;
}

bool error::is_null(std::initializer_list<const void *> pointers, const std::source_location &location) noexcept
{
    // Loop through the list. Using index so the error logged can be tracked.
    int index{};
    for (const void *pointer : pointers)
    {
        // If it's null, log and return true.
        if (!pointer)
        {
            std::string_view file{}, function{};
            prep_locations(file, function, location);
            logger::log("%s::%s::%u::%u::nullptr{%i}!",
                        file.data(),
                        function.data(),
                        location.line(),
                        location.column(),
                        index);

            return true;
        }
        ++index;
    }
    // Nothing was null.
    return false;
}

static void prep_locations(std::string_view &file, std::string_view &function, const std::source_location &location) noexcept
{
    file     = location.file_name();
    function = location.function_name();

    const size_t fileBegin = file.find_last_of('/');
    if (fileBegin != file.npos) { file = file.substr(fileBegin + 1); }

    const size_t functionBegin = function.find_first_of(' ');
    if (functionBegin != function.npos) { function = function.substr(functionBegin + 1); }
}
