#include "fs/directory_functions.hpp"

#include "error.hpp"
#include "fs/SaveMetaData.hpp"
#include "logging/logger.hpp"

bool fs::get_directory_information(const fslib::Path &directoryPath,
                                   int64_t &subDirCount,
                                   int64_t &fileCount,
                                   int64_t &totalSize)
{
    fslib::Directory dir{directoryPath};
    if (error::fslib(dir.is_open())) { return false; }

    for (const fslib::DirectoryEntry &entry : dir)
    {
        if (entry.is_directory())
        {
            const fslib::Path newPath{directoryPath / entry};
            const bool getInfo = fs::get_directory_information(newPath, subDirCount, fileCount, totalSize);
            if (!getInfo) { return false; }
            ++subDirCount;
        }
        else
        {
            totalSize += entry.get_size();
            ++fileCount;
        }
    }

    return true;
}

bool fs::directory_has_contents(const fslib::Path &directoryPath)
{
    fslib::Directory testDir{directoryPath};
    if (!testDir.is_open()) { return false; }

    for (const fslib::DirectoryEntry &entry : testDir)
    {
        if (entry.get_filename() != fs::NAME_SAVE_META) { return true; }
    }

    return false;
}

bool fs::move_directory_recursively(const fslib::Path &oldPath, const fslib::Path &newPath)
{
    fslib::Directory sourceDir{oldPath};
    if (!sourceDir.is_open()) { return false; }

    for (const fslib::DirectoryEntry &entry : sourceDir)
    {
        const fslib::Path fullSource{oldPath / entry};
        const fslib::Path fullDest{newPath / entry};

        if (entry.is_directory())
        {
            const bool exists      = fslib::directory_exists(fullDest);
            const bool renameError = !exists && error::fslib(fslib::rename_directory(fullSource, fullDest));
            const bool moved       = exists && fs::move_directory_recursively(fullSource, fullDest);
            if (renameError && !moved) { return false; }
        }
        else
        {
            const bool exists  = fslib::file_exists(fullDest);
            const bool renamed = !exists && fslib::rename_file(fullSource, fullDest);
            if (!exists && !renamed) { return false; }
        }
    }
    return true;
}