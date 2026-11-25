#include "remote/Storage.hpp"

#include "logging/logger.hpp"

#include <algorithm>
#include <cstring>

//                      ---- Construction ----

remote::Storage::Storage(std::string_view prefix)
    : m_curl(curl::new_handle())
    , m_prefix(prefix) {};

//                      ---- Public functions ----

bool remote::Storage::is_initialized() const noexcept { return m_isInitialized; }

bool remote::Storage::directory_exists(std::string_view name) const noexcept
{
    return Storage::find_directory_by_name(name) != m_list.end();
}

void remote::Storage::return_to_root() { m_parent = m_root; }

void remote::Storage::set_root_directory(const remote::Item *root) { m_root = root->get_id(); }

void remote::Storage::change_directory(const remote::Item *item) { m_parent = item->get_id(); }

remote::Item *remote::Storage::get_directory_by_name(std::string_view name) noexcept
{
    auto findDirectory = Storage::find_directory_by_name(name);
    if (findDirectory == m_list.end()) { return nullptr; }
    return &(*findDirectory);
}

void remote::Storage::get_directory_listing(remote::Storage::DirectoryListing &listOut)
{
    listOut.clear();

    auto current = m_list.begin();
    while ((current = Storage::find_by_parent_id(current, m_parent)) != m_list.end())
    {
        listOut.push_back(&(*current));
        ++current;
    }
}

void remote::Storage::get_directory_listing_with_parent(const remote::Item *item, remote::Storage::DirectoryListing &listOut)
{
    listOut.clear();

    const std::string_view parentId = item->get_id();
    auto current                    = m_list.begin();
    while ((current = Storage::find_by_parent_id(current, parentId)) != m_list.end())
    {
        listOut.push_back(&(*current));
        ++current;
    }
}

bool remote::Storage::file_exists(std::string_view name) const noexcept
{
    return Storage::find_file_by_name(name) != m_list.end();
}

remote::Item *remote::Storage::get_file_by_name(std::string_view name) noexcept
{
    auto findFile = Storage::find_file_by_name(name);
    if (findFile == m_list.end()) { return nullptr; }
    return &(*findFile);
}

remote::Item *remote::Storage::get_item_by_id(std::string_view id) noexcept
{
    auto is_match = [&](const remote::Item &item) { return item.get_id() == id; };

    auto findMatch = std::find_if(m_list.begin(), m_list.end(), is_match);
    if (findMatch == m_list.end()) { return nullptr; }

    return &(*findMatch);
}

std::string_view remote::Storage::get_prefix() const noexcept { return m_prefix; }

//                      ---- Private functions ----

remote::Storage::List::iterator remote::Storage::find_directory_by_name(std::string_view name) noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isDir       = item.is_directory();
        const bool parentMatch = isDir && item.get_parent_id() == m_parent;
        const bool nameMatch   = parentMatch && item.get_name() == name;

        return isDir && parentMatch && nameMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::const_iterator remote::Storage::find_directory_by_name(std::string_view name) const noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isDir       = item.is_directory();
        const bool parentMatch = isDir && item.get_parent_id() == m_parent;
        const bool nameMatch   = parentMatch && item.get_name() == name;

        return isDir && parentMatch && nameMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::iterator remote::Storage::find_directory_by_id(std::string_view id) noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isDir   = item.is_directory();
        const bool idMatch = isDir && item.get_id() == id;

        return isDir && idMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::const_iterator remote::Storage::find_directory_by_id(std::string_view id) const noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isDir   = item.is_directory();
        const bool idMatch = isDir && item.get_id() == id;

        return isDir && idMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::iterator remote::Storage::find_file_by_name(std::string_view name) noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isFile      = !item.is_directory();
        const bool parentMatch = isFile && item.get_parent_id() == m_parent;
        const bool nameMatch   = parentMatch && item.get_name() == name;

        return isFile && parentMatch && nameMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::const_iterator remote::Storage::find_file_by_name(std::string_view name) const noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isFile      = !item.is_directory();
        const bool parentMatch = isFile && item.get_parent_id() == m_parent;
        const bool nameMatch   = parentMatch && item.get_name() == name;

        return isFile && parentMatch && nameMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::iterator remote::Storage::find_file_by_id(std::string_view id) noexcept
{
    auto is_match = [&](const Item &item) noexcept
    {
        const bool isFile  = !item.is_directory();
        const bool isMatch = isFile && item.get_id() == id;

        return isFile && isMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::const_iterator remote::Storage::find_file_by_id(std::string_view id) const noexcept
{
    auto is_match = [&](const remote::Item &item) noexcept
    {
        const bool isFile  = !item.is_directory();
        const bool isMatch = isFile && item.get_id() == id;

        return isFile && isMatch;
    };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::iterator remote::Storage::find_item_by_id(std::string_view id) noexcept
{
    auto is_match = [&](const Item &item) { return item.get_id() == id; };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::const_iterator remote::Storage::find_item_by_id(std::string_view id) const noexcept
{
    auto is_match = [&](const remote::Item &item) { return item.get_id() == id; };

    return std::find_if(m_list.begin(), m_list.end(), is_match);
}

remote::Storage::List::iterator remote::Storage::find_by_parent_id(remote::Storage::List::iterator start,
                                                                   std::string_view parentID) noexcept
{
    auto is_match = [&](const Item &item) { return item.get_parent_id() == parentID; };

    return std::find_if(start, m_list.end(), is_match);
}

remote::Storage::List::const_iterator remote::Storage::find_by_parent_id(Storage::List::const_iterator start,
                                                                         std::string_view parentID) const noexcept
{
    auto is_match = [&](const remote::Item &item) { return item.get_parent_id() == parentID; };

    return std::find_if(start, m_list.end(), is_match);
}
