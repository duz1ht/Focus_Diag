#include <windows.h>

#include "hooks.h"
#include "logger.h"

namespace {
HMODULE g_module{};

DWORD WINAPI Initialize(void*) {
    if (fd::Logger::Instance().Start(g_module)) fd::InstallHooks(g_module);
    return 0;
}
}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
