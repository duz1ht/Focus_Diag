#include <windows.h>

#include "legacy_dx8.h"

namespace {

INIT_ONCE g_realModuleOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_realModule{};

BOOL CALLBACK LoadRealModule(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 13) return FALSE;
    if (path[length - 1] != L'\\') wcscat_s(path, L"\\");
    wcscat_s(path, L"dinput8.dll");
    g_realModule = LoadLibraryW(path);
    return g_realModule != nullptr;
}

FARPROC RealExport(const char* name) {
    if (!InitOnceExecuteOnce(&g_realModuleOnce, LoadRealModule, nullptr, nullptr)) return nullptr;
    return GetProcAddress(g_realModule, name);
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid,
                                               LPVOID* output, LPUNKNOWN outer) {
    const auto real = reinterpret_cast<DirectInput8CreateFn>(RealExport("DirectInput8Create"));
    if (!real) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return real(instance, version, iid, output, outer);
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    using Function = HRESULT(WINAPI*)();
    const auto real = reinterpret_cast<Function>(RealExport("DllCanUnloadNow"));
    return real ? real() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output) {
    using Function = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    const auto real = reinterpret_cast<Function>(RealExport("DllGetClassObject"));
    return real ? real(clsid, iid, output) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    using Function = HRESULT(WINAPI*)();
    const auto real = reinterpret_cast<Function>(RealExport("DllRegisterServer"));
    return real ? real() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    using Function = HRESULT(WINAPI*)();
    const auto real = reinterpret_cast<Function>(RealExport("DllUnregisterServer"));
    return real ? real() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
