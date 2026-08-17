#include <windows.h>

#include "hooks.h"

DWORD WINAPI Initialize(void*) {
    fd::InstallHooks();
    return 0;
}

extern "C" __declspec(dllexport) void WINAPI FocusDiagnosticShutdown() {
    fd::RemoveHooks();
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
