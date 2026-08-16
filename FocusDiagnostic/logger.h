#pragma once

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>

namespace fd {

class Logger final {
public:
    bool Start(HMODULE module);
    void Stop();
    void Write(const char* category, const char* format, ...);
    void WriteV(const char* category, const char* format, va_list args);
    static Logger& Instance();

private:
    static DWORD WINAPI ThreadProc(void* context);
    void Run();
    std::string Prefix(const char* category) const;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    HANDLE thread_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    mutable LARGE_INTEGER frequency_{};
    LARGE_INTEGER started_{};
    std::mutex mutex_;
    std::deque<std::string> queue_;
};

const char* HResultName(HRESULT value);

}  // namespace fd
