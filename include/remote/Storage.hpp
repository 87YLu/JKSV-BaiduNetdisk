#pragma once
#include "curl/curl.hpp"
#include "fslib.hpp"
#include "remote/Item.hpp"
#include "sys/sys.hpp"

#include <ctime>
#include <string>
#include <vector>

namespace remote
{
    class Storage
    {
        public:
            /// @brief Definition to make things easier to type.
            using DirectoryListing = std::vector<remote::Item *>;

            /// @brief This makes writing some stuff for these classes way easier.
            using List = std::vector<remote::Item>;

            /// @brief This just allocates the curl::Handle. Never mind.
            Storage(std::string_view prefix, bool supportsUtf8 = false);

            /// @brief Storage is owned polymorphically by the remote service manager.
            virtual ~Storage() = default;

            /// @brief Returns whether or not the Storage type was successfully. initialized.
            bool is_initialized() const noexcept;

            // Directory functions.
            /// @brief Returns whether or not a directory with name exists within the current parent.
            /// @param name Name of the directory to search for.
            bool directory_exists(std::string_view name) const noexcept;

            /// @brief Returns the parent to the root directory.
            void return_to_root();

            /// @brief This allows the root to be set to something other than what it originally was at construction.
            /// @param root Item to be used as the new root.
            void set_root_directory(const remote::Item *root);

            /// @brief Changes the current parent directory.
            /// @param Item Item to use as the current parent directory.
            void change_directory(const remote::Item *item);

            /// @brief Creates a directory in the current parent directory.
            /// @param name Name of the directory to create.
            virtual bool create_directory(std::string_view name) = 0;

            /// @brief Searches the list for a directory matching name and the current parent.
            /// @param name Name of the directory to search for.
            /// @return Pointer to the item representing the directory on success. nullptr on failure/not found.
            remote::Item *get_directory_by_name(std::string_view name) noexcept;

            /// @brief Retrieves a listing of the items in the current parent directory.
            /// @param listOut List to fill.
            void get_directory_listing(Storage::DirectoryListing &listOut);

            /// @brief Same as above, but allows any parent to be used.
            /// @param item Item that is to be treated as a parent.
            /// @param listOut List to fill.
            void get_directory_listing_with_parent(const remote::Item *item, Storage::DirectoryListing &listOut);

            // File functions.
            /// @brief Returns whether a file with name exists within the current directory.
            /// @param name Name of the file.
            bool file_exists(std::string_view name) const noexcept;

            /// @brief Searches the list for a file matching name and the current parent.
            /// @param name Name of the file to search for.
            /// @return Pointer to the item if located. nullptr if not.
            remote::Item *get_file_by_name(std::string_view name) noexcept;

            /// @brief Searches for and returns the item with id. Returns nullptr on failure.
            remote::Item *get_item_by_id(std::string_view id) noexcept;

            /// @brief Uploads a file from the SD card to the remote.
            /// @param source Path to the file to upload.
            virtual bool upload_file(const fslib::Path &source, std::string_view name, sys::ProgressTask *task = nullptr) = 0;

            /// @brief Patches or updates a file on the remote.
            /// @param item Item to be updated.
            /// @param source Path to the file to update with.
            virtual bool patch_file(remote::Item *file, const fslib::Path &source, sys::ProgressTask *task = nullptr) = 0;

            /// @brief Downloads a file from the remote.
            /// @param item Item to download.
            /// @param destination Path to download the file to.
            virtual bool download_file(const remote::Item *file,
                                       const fslib::Path &destination,
                                       sys::ProgressTask *task = nullptr) = 0;

            // General functions that apply to both.
            /// @brief Deletes a file or folder from the remote.
            /// @param item Item to delete.
            virtual bool delete_item(const remote::Item *item) = 0;

            /// @brief Renames a file on the remote server.
            /// @param item Target item to rename.
            /// @param newName New name of the target item.
            virtual bool rename_item(remote::Item *item, std::string_view newName) = 0;

            /// @brief Returns whether or not the remote storage type supports UTF-8 for names or requires path safe titles.
            bool supports_utf8() const noexcept;

            /// @brief Returns the prefix for menus.
            std::string_view get_prefix() const noexcept;

        protected:
            /// @brief This is the size of the buffers used for snprintf'ing URLs together.
            static constexpr size_t SIZE_URL_BUFFER = 0x401;

            /// @brief This is the size used for uploads.
            static constexpr size_t SIZE_UPLOAD_BUFFER = 0x10000;

            /// @brief Curl handle.
            curl::Handle m_curl;

            /// @brief This allows JKSV to know whether or not the storage type supports UTF-8.
            bool m_utf8Paths{};

            /// @brief This is the prefix used for menus.
            std::string m_prefix{};

            /// @brief This stores whether or not the instance was initialized successfully.
            bool m_isInitialized{};

            /// @brief This is the root directory of the remote storage.
            std::string m_root{};

            /// @brief This stores the current parent.
            std::string m_parent{};

            /// @brief This is the main remote listing.
            Storage::List m_list{};

            /// @brief Searches the list for a directory matching name and the current parent.
            /// @param name Name to search for.
            Storage::List::iterator find_directory_by_name(std::string_view name) noexcept;
            Storage::List::const_iterator find_directory_by_name(std::string_view name) const noexcept;

            /// @brief Searches the list for a directory matching ID.
            /// @param id ID of the directory to search for.
            Storage::List::iterator find_directory_by_id(std::string_view id) noexcept;
            Storage::List::const_iterator find_directory_by_id(std::string_view id) const noexcept;

            /// @brief Searches to find if a file with name exists within the current parent.
            /// @param name Name of the file to search for.
            Storage::List::iterator find_file_by_name(std::string_view name) noexcept;
            Storage::List::const_iterator find_file_by_name(std::string_view name) const noexcept;

            /// @brief Searches the list for a file matching ID.
            /// @param id ID to search for.s
            Storage::List::iterator find_file_by_id(std::string_view id) noexcept;
            Storage::List::const_iterator find_file_by_id(std::string_view id) const noexcept;

            /// @brief Locates any item (directory/file) by the id passed.
            /// @param id ID to search for.
            Storage::List::iterator find_item_by_id(std::string_view id) noexcept;
            Storage::List::const_iterator find_item_by_id(std::string_view id) const noexcept;

            /// @brief Searches starting with the iterator start for items that belong to parentID
            /// @param start Beginning iterator for search.
            /// @param parentID ParentID to match.
            Storage::List::iterator find_by_parent_id(Storage::List::iterator start, std::string_view parentID) noexcept;
            Storage::List::const_iterator find_by_parent_id(Storage::List::const_iterator start,
                                                            std::string_view parentID) const noexcept;
    };
} // namespace remote
