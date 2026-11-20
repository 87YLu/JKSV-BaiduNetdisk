#include "baidu.hpp"
#include <cstdarg>
#include <unistd.h>

namespace background
{
    bool file_exists(std::string_view path) { return access(std::string(path).c_str(), F_OK) == 0; }
    bool remove_file(std::string_view path) { return std::remove(std::string(path).c_str()) == 0; }
    bool atomic_replace(std::string_view from, std::string_view to)
    {
        return std::rename(std::string(from).c_str(), std::string(to).c_str()) == 0;
    }
    void sleep_seconds(unsigned) {}
    void logf(const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
        std::fputc('\n', stderr);
    }
}

int main()
{
    background::BaiduClient client;
    if (!client.initialize(true)) { return 2; }
    const bool uploaded = client.upload_file("backup.zip", "/apps/JKSV/Test/backup.zip");
    std::printf("%s\n%s\n", uploaded ? "SUCCESS" : "FAILED", client.last_error().c_str());
    return 0;
}
