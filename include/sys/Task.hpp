#pragma once
#include "sys/threadpool.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// This macro helps keep things a bit easier to read and cuts down on repetition.
#define TASK_FINISH_RETURN(x)                                                                                                  \
    x->complete();                                                                                                             \
    return

namespace sys
{
    /// @brief Class that runs tasks in a thread and automatically deactivates when finished.
    class Task
    {
        public:
            // clang-format off
            struct DataStruct : sys::threadpool::DataStruct
            {
                sys::Task *task{};
            };
            // clang-format on

            /// @brief This makes some things easier to type and read in other places.
            using TaskData = std::shared_ptr<Task::DataStruct>;

            /// @brief Default constructor.
            Task();

            /// @brief Starts a new task.
            Task(sys::threadpool::JobFunction function, sys::Task::TaskData taskData);

            /// @brief Returns if the thread has signaled it's finished running.
            /// @return True if the thread is still running. False if it isn't.
            bool is_running() const noexcept;

            /// @brief Allows thread to signal it's finished.
            /// @note Spawned task threads must call this when their work is finished.
            void complete() noexcept;

            /// @brief Sets the task/threads current status string. Thread safe.
            void set_status(std::string_view status);

            /// @brief Moves the status string instead of creating a copy.
            void set_status(std::string &status);

            /// @brief Returns the status string. Thread safe.
            /// @return Copy of the status string.
            std::string get_status() noexcept;

            /// @brief Publishes an encoded image for task states to display. Thread safe.
            void set_image(std::vector<uint8_t> image);

            /// @brief Moves the pending encoded image to the render thread. Thread safe.
            std::vector<uint8_t> take_image();

        protected:
            // Whether task is still running.
            std::atomic<bool> m_isRunning{};

        private:
            // Status string the thread can set that the main thread can display.
            std::string m_status{};

            // Mutex so that string doesn't get messed up.
            std::mutex m_statusLock{};

            // Optional encoded image supplied by a worker (for example an OAuth QR code).
            std::vector<uint8_t> m_image{};
            std::mutex m_imageLock{};
    };
} // namespace sys
