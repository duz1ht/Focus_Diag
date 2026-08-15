#include "logger.h"

#include "legacy_dx8.h"

#include <cstdio>
#include <vector>

namespace fd {

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

bool Logger::Start(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(path, L"FocusDiagnostic.log");
    file_ = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&started_);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return false;
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) return false;
    Write("DIAGNOSTIC", "Focus Diagnostic 0.1 started; PID=%lu; architecture=x86",
          GetCurrentProcessId());
    return true;
}

void Logger::Stop() {
    if (file_ == INVALID_HANDLE_VALUE) return;
    Write("DIAGNOSTIC", "Logger stopping");
    stopping_ = true;
    if (event_) SetEvent(event_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    FlushFileBuffers(file_);
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

std::string Logger::Prefix(const char* category) const {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double milliseconds = frequency_.QuadPart
        ? (now.QuadPart - started_.QuadPart) * 1000.0 / frequency_.QuadPart : 0.0;
    char text[128]{};
    sprintf_s(text, "[%012.3f] [TID %lu] [%s] ", milliseconds,
              GetCurrentThreadId(), category);
    return text;
}

void Logger::Write(const char* category, const char* format, ...) {
    va_list args;
    va_start(args, format);
    WriteV(category, format, args);
    va_end(args);
}

void Logger::WriteV(const char* category, const char* format, va_list args) {
    if (file_ == INVALID_HANDLE_VALUE || stopping_) return;
    char body[2048]{};
    vsprintf_s(body, format, args);
    std::string line = Prefix(category) + body + "\r\n";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= 8192) queue_.pop_front();
        queue_.push_back(std::move(line));
    }
    SetEvent(event_);
}

DWORD WINAPI Logger::ThreadProc(void* context) {
    static_cast<Logger*>(context)->Run();
    return 0;
}

void Logger::Run() {
    for (;;) {
        WaitForSingleObject(event_, 500);
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(queue_);
        }
        for (const auto& line : pending) {
            DWORD written = 0;
            WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
        if (stopping_ && pending.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) break;
        }
    }
}

const char* HResultName(HRESULT value) {
    if (value == S_OK) return "OK";
    if (value == kD3DErrDeviceLost) return "D3DERR_DEVICELOST";
    if (value == kD3DErrDeviceNotReset) return "D3DERR_DEVICENOTRESET";
    if (value == kDiErrOtherAppHasPriority) return "DIERR_OTHERAPPHASPRIO/E_ACCESSDENIED";
    return FAILED(value) ? "FAILED" : "SUCCESS";
}

}  // namespace fd
