#include "remote/Item.hpp"

//                      ---- Construction ----

remote::Item::Item(std::string_view name, std::string_view id, std::string_view parent, size_t size, bool directory,
                   int64_t modifiedTime)
    : m_name{name}
    , m_id{id}
    , m_parent{parent}
    , m_size{size}
    , m_modifiedTime{modifiedTime}
    , m_isDirectory{directory} {};

//                      ---- Public functions ----

std::string_view remote::Item::get_name() const noexcept { return m_name; }

std::string_view remote::Item::get_id() const noexcept { return m_id; }

std::string_view remote::Item::get_parent_id() const noexcept { return m_parent; }

size_t remote::Item::get_size() const noexcept { return m_size; }

int64_t remote::Item::get_modified_time() const noexcept { return m_modifiedTime; }

void remote::Item::set_modified_time(int64_t modifiedTime) noexcept { m_modifiedTime = modifiedTime; }

bool remote::Item::is_directory() const noexcept { return m_isDirectory; }

void remote::Item::set_name(std::string_view name) { m_name = name; }

void remote::Item::set_id(std::string_view id) { m_id = id; }

void remote::Item::set_parent_id(std::string_view parent) { m_parent = parent; }

void remote::Item::set_size(size_t size) noexcept { m_size = size; }

void remote::Item::set_is_directory(bool directory) noexcept { m_isDirectory = directory; }
