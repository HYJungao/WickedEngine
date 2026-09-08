#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <string>

namespace wicked_newpipeline
{
// Opt-in local diagnostic recording. Use different paths for the two peers.
// Buffered CSV avoids the GUI / per-line console overhead during a capture.
// Call sites keep process-lifetime recorders: WebRTC callbacks can outlive
// normal static destruction. Periodic flush bounds the tail lost on exit.
class PacingTrace
{
    FILE* file = nullptr;
    char* buffer = nullptr;
    std::mutex write_mutex;
    uint64_t last_flush_usec = 0;
public:
    explicit PacingTrace(const char* columns, const char* suffix = "")
    {
        const char* path = std::getenv("NP_PACING_TRACE");
        if (path && *path)
        {
            const std::string filename = std::string(path) + suffix;
            file = std::fopen(filename.c_str(), "w");
            if (file)
            {
                buffer = new char[256 * 1024];
                std::setvbuf(file, buffer, _IOFBF, 256 * 1024);
                std::fprintf(file, "%s\n", columns);
            }
        }
    }
    ~PacingTrace() { if (file) std::fclose(file); delete[] buffer; }
    PacingTrace(const PacingTrace&) = delete;
    PacingTrace& operator=(const PacingTrace&) = delete;
    static uint64_t NowUsec()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    bool Enabled() const { return file != nullptr; }
    void Record(std::initializer_list<uint64_t> values)
    {
        if (!file || values.size() == 0) return;
        std::lock_guard lock(write_mutex);
        bool first = true;
        for (uint64_t value : values)
        {
            std::fprintf(file, "%s%llu", first ? "" : ",", static_cast<unsigned long long>(value));
            first = false;
        }
        std::fputc('\n', file);
        const uint64_t now = *values.begin();
        if (now - last_flush_usec >= 1'000'000u)
        {
            std::fflush(file);
            last_flush_usec = now;
        }
    }
};
} // namespace wicked_newpipeline
