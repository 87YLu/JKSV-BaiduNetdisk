#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

#include <json-c/json.h>

/// @brief Reading and writing baidu.json, shared by JKSV and the background worker.
/// @note Both of them refresh OAuth tokens, and Baidu's refresh tokens only work once. A refresh has to happen under the
/// lock with tokens that were just read from disk, or the two processes can burn each other's tokens and end the login.
namespace baidu_config
{
    inline constexpr const char *PATH = "sdmc:/config/JKSV/baidu.json";

    /// @brief How long a lock stays valid. This has to be longer than the slowest token refresh either side can do
    /// (the worker allows 15 s to connect plus 90 s for the request), or a waiter would take it from a live holder.
    inline constexpr std::time_t SECONDS_LOCK_LIFETIME = 180;

    /// @brief Lock shared across processes through a file. Only meant to be held for a refresh or a single write.
    class ScopedLock
    {
        public:
            explicit ScopedLock(const char *configPath = PATH)
                : m_path(std::string(configPath) + ".lock")
            {
                static constexpr std::chrono::milliseconds RETRY_DELAY{500};

                // Give up after the lifetime plus a little: by then any real holder's lock has expired.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(SECONDS_LOCK_LIFETIME + 5);
                while (!try_acquire())
                {
                    if (is_abandoned()) { ::unlink(m_path.c_str()); }
                    else if (std::chrono::steady_clock::now() >= deadline)
                    {
                        ::unlink(m_path.c_str());
                        try_acquire();
                        return;
                    }
                    else { std::this_thread::sleep_for(RETRY_DELAY); }
                }
            }

            ~ScopedLock()
            {
                // Only remove the lock if it's still this one. Never delete a lock another process took over.
                if (m_held && read_lock() == m_contents) { ::unlink(m_path.c_str()); }
            }

            ScopedLock(const ScopedLock &)            = delete;
            ScopedLock &operator=(const ScopedLock &) = delete;

            bool held() const noexcept { return m_held; }

        private:
            std::string m_path{};
            std::string m_contents{};
            bool m_held{};

            bool try_acquire()
            {
                const int descriptor = ::open(m_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
                if (descriptor < 0) { return false; }

                // The expiration is stored instead of relying on timestamps, which aren't dependable on every
                // filesystem. The second number tells this lock apart from one another process takes later.
                const auto unique = std::chrono::steady_clock::now().time_since_epoch().count() ^
                                    reinterpret_cast<std::uintptr_t>(this);
                char contents[64]{};
                const int length = std::snprintf(contents,
                                                 sizeof(contents),
                                                 "%lld %llx",
                                                 static_cast<long long>(std::time(nullptr) + SECONDS_LOCK_LIFETIME),
                                                 static_cast<unsigned long long>(unique));
                const bool written = length > 0 && ::write(descriptor, contents, length) == length;
                ::close(descriptor);

                if (!written)
                {
                    ::unlink(m_path.c_str());
                    return false;
                }
                m_contents.assign(contents, length);
                m_held = true;
                return true;
            }

            std::string read_lock() const
            {
                FILE *file = std::fopen(m_path.c_str(), "rb");
                if (!file) { return {}; }
                char buffer[64]{};
                const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
                std::fclose(file);
                return std::string(buffer, read);
            }

            bool is_abandoned() const
            {
                const std::string contents = read_lock();
                long long expiration{};
                if (contents.empty() || std::sscanf(contents.c_str(), "%lld", &expiration) != 1)
                {
                    // Not written yet, or cut off. Only treat it as abandoned once it can't still be in progress.
                    return false;
                }

                // An expiration far in the future means the clock went backwards since it was written.
                const std::time_t now = std::time(nullptr);
                return now > static_cast<std::time_t>(expiration) ||
                       static_cast<std::time_t>(expiration) > now + SECONDS_LOCK_LIFETIME * 2;
            }
    };

    /// @brief Reads the config, recovering it if a write was interrupted. The caller owns the result.
    /// @note A .tmp that parses completely is always the newest: write() only leaves one behind when it was cut off
    /// before or during the renames, and by then the tokens in the old file may already be used up.
    inline json_object *read(const char *path = PATH)
    {
        const std::string temporary = std::string{path} + ".tmp";
        if (json_object *config = json_object_from_file(temporary.c_str())) { return config; }
        return json_object_from_file(path);
    }

    /// @brief Whether there's a config at all, counting one left in .tmp by an interrupted write.
    inline bool exists(const char *path = PATH)
    {
        const std::string temporary = std::string{path} + ".tmp";
        return ::access(path, F_OK) == 0 || ::access(temporary.c_str(), F_OK) == 0;
    }

    /// @brief Writes the config so that an interruption at any point leaves something read() can use.
    inline bool write(json_object *config, const char *path = PATH)
    {
        const char *contents = json_object_to_json_string_ext(config, JSON_C_TO_STRING_PRETTY);
        if (!contents) { return false; }

        const std::string target{path};
        const std::string temporary = target + ".tmp";
        const std::string backup    = target + ".bak";

        FILE *file = std::fopen(temporary.c_str(), "wb");
        if (!file) { return false; }
        const size_t length = std::strlen(contents);
        bool written        = std::fwrite(contents, 1, length, file) == length;
        written             = std::fflush(file) == 0 && written;
        written             = std::fclose(file) == 0 && written;
        if (!written)
        {
            ::unlink(temporary.c_str());
            return false;
        }

        // Renaming doesn't replace an existing file on the Switch, so the old one has to be moved out of the way first.
        ::unlink(backup.c_str());
        const bool hadTarget = ::access(target.c_str(), F_OK) == 0;
        if (hadTarget && std::rename(target.c_str(), backup.c_str()) != 0)
        {
            ::unlink(temporary.c_str());
            return false;
        }

        if (std::rename(temporary.c_str(), target.c_str()) != 0)
        {
            if (hadTarget) { std::rename(backup.c_str(), target.c_str()); }
            return false;
        }

        if (hadTarget) { ::unlink(backup.c_str()); }
        return true;
    }
} // namespace baidu_config
