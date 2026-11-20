#pragma once
#include <cstdint>
#include <string>

namespace remote
{
    class Item
    {
        public:
            /// @brief Remote item constructor.
            /// @param name Item's name.
            /// @param id Item's ID.
            /// @param parent Item's parent.
            /// @param size Size of the time.
            /// @param directory Whether or not the item is a directory.
            Item(std::string_view name, std::string_view id, std::string_view parent, size_t size, bool directory,
                 int64_t modifiedTime = 0);

            /// @brief Returns the name of the item.
            /// @return Name of the item.
            std::string_view get_name() const noexcept;

            /// @brief Returns the id of the item.
            /// @return ID of the item.
            std::string_view get_id() const noexcept;

            /// @brief Returns the parent id of the item.
            /// @return Parent ID of the item.
            std::string_view get_parent_id() const noexcept;

            /// @brief Gets the size of the item.
            /// @return Size of the item in bytes.
            size_t get_size() const noexcept;

            /// @brief Modification time in Unix seconds, or zero when unavailable.
            int64_t get_modified_time() const noexcept;
            void set_modified_time(int64_t modifiedTime) noexcept;

            /// @brief Returns whether or not the item is a directory.
            /// @return Whether or not the item is a directory.
            bool is_directory() const noexcept;

            /// @brief Sets the name of the item.
            /// @param name New name of the item.
            void set_name(std::string_view name);

            /// @brief Sets the ID of the item.
            /// @param id New ID of the item.
            void set_id(std::string_view id);

            /// @brief Sets the parent ID of the item.
            /// @param parent Parent ID of the item.
            void set_parent_id(std::string_view parent);

            /// @brief Sets the size of the item.
            /// @param size Size of the item.
            void set_size(size_t size) noexcept;

            /// @brief Sets whether or not the item is a directory.
            /// @param directory Whether or not the item is a directory.
            void set_is_directory(bool directory) noexcept;

        private:
            /// @brief The name of the item.
            std::string m_name{};

            /// @brief The ID of the item.
            std::string m_id{};

            /// @brief Parent ID of the item.
            std::string m_parent{};

            /// @brief Size of the item.
            size_t m_size{};
            int64_t m_modifiedTime{};

            /// @brief Whether or not the item is a directory.
            bool m_isDirectory{};
    };
} // namespace remote
