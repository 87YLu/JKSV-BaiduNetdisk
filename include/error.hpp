#pragma once
#include <initializer_list>
#include <source_location>
#include <switch.h>

namespace error
{
    /// @brief Logs and returns if a call from libnx fails.
    bool libnx(Result code, const std::source_location &location = std::source_location::current()) noexcept;

    /// @brief Logs and returns if an fslib function fails.
    bool fslib(bool result, const std::source_location &location = std::source_location::current()) noexcept;

    /// @brief Returns whether or not the pointer passed is null. Records the location in which this occurred.
    bool is_null(const void *pointer, const std::source_location &location = std::source_location::current()) noexcept;

    /// @brief Returns whether or not the span of pointers passed contains any nullptrs.
    bool is_null(std::initializer_list<const void *> pointers,
                 const std::source_location &location = std::source_location::current()) noexcept;

}
