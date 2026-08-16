#include "hooks.h"

#include "diagnostics.h"
#include "legacy_dx8.h"
#include "logger.h"

#include <array>
#include <atomic>
#include <climits>
#include <cstring>

namespace fd {
namespace {

HMODULE g_self{};
HWND g_window{};
WNDPROC g_originalWndProc{};
constexpr UINT_PTR kDiagnosticTimer = 0xF0C05;

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
ClipCursorFn g_clipCursor{};
SetCursorPosFn g_setCursorPos{};
Direct3DCreate8Fn g_direct3DCreate8{};
struct ImportPatch { void** slot; void* original; void* replacement; };
std::array<ImportPatch, 4> g_importPatches{};
size_t g_importPatchCount{};

void** g_d3d8Object{};
void** g_d3d8OriginalVtable{};
void** g_d3d8HookVtable{};
void** g_d3dDeviceObject{};
void** g_d3dDeviceOriginalVtable{};
void** g_d3dDeviceHookVtable{};
void** g_dinputObject{};
void** g_dinputOriginalVtable{};
void** g_dinputHookVtable{};
void** g_mouseObject{};
void** g_mouseOriginalVtable{};
void** g_mouseHookVtable{};
void** g_keyboardObject{};
void** g_keyboardOriginalVtable{};
void** g_keyboardHookVtable{};

thread_local bool g_insideHook = false;
thread_local int g_lastCursorX = INT_MIN;
thread_local int g_lastCursorY = INT_MIN;
thread_local ULONGLONG g_lastCursorLogAt = 0;
thread_local unsigned g_suppressedCursorCalls = 0;
std::atomic<bool> g_enableDirectInput{false};
std::atomic<bool> g_enableD3DCreateDevice{false};
std::atomic<bool> g_enableD3DDevice{false};

struct HookSettings {
    bool window = false;
    bool d3d8 = false;
    bool d3d8CreateDevice = false;
    bool d3d8Device = false;
    bool directInput = false;
    bool cursor = false;
};

HookSettings LoadHookSettings(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(path, L"FocusDiagnostic.ini");
    HookSettings settings;
    settings.window = GetPrivateProfileIntW(L"Hooks", L"Window", 0, path) != 0;
    settings.d3d8 = GetPrivateProfileIntW(L"Hooks", L"D3D8", 0, path) != 0;
    settings.d3d8CreateDevice = GetPrivateProfileIntW(L"Hooks", L"D3D8CreateDevice", 0, path) != 0;
    settings.d3d8Device = GetPrivateProfileIntW(L"Hooks", L"D3D8Device", 0, path) != 0;
    settings.directInput = GetPrivateProfileIntW(L"Hooks", L"DirectInput", 0, path) != 0;
    settings.cursor = GetPrivateProfileIntW(L"Hooks", L"Cursor", 0, path) != 0;
    return settings;
}

template <size_t Count>
bool ReplaceVtable(void* object, void*** savedOriginal, void*** savedHook,
                   size_t index, void* replacement) {
    if (!object || *savedHook) return false;
    auto objectVtable = reinterpret_cast<void***>(object);
    void** original = *objectVtable;
    void** clone = static_cast<void**>(VirtualAlloc(nullptr, Count * sizeof(void*),
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!clone) return false;
    memcpy(clone, original, Count * sizeof(void*));
    clone[index] = replacement;
    *savedOriginal = original;
    *savedHook = clone;
    DWORD oldProtect{};
    VirtualProtect(objectVtable, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *objectVtable = clone;
    VirtualProtect(objectVtable, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

void RestoreVtable(void** object, void** original, void**& hook) {
    if (object && original && hook) {
        auto slot = reinterpret_cast<void***>(object);
        DWORD oldProtect{};
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect);
        *slot = original;
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
        VirtualFree(hook, 0, MEM_RELEASE);
        hook = nullptr;
    }
}

bool PatchImport(HMODULE module, const char* importedModule, const char* function,
                 void* replacement, void** original) {
    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(name, importedModule) != 0) continue;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk) : thunk;
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(import->Name), function) != 0) continue;
            auto slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD oldProtect{};
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            if (original) *original = *slot;
            if (g_importPatchCount < g_importPatches.size())
                g_importPatches[g_importPatchCount++] = {slot, *slot, replacement};
            *slot = replacement;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

BOOL WINAPI HookClipCursor(const RECT* rect) {
    if (g_insideHook) return g_clipCursor(rect);
    g_insideHook = true;
    if (rect) Logger::Instance().Write("CURSOR", "ClipCursor requested (%ld,%ld)-(%ld,%ld)",
        rect->left, rect->top, rect->right, rect->bottom);
    else Logger::Instance().Write("CURSOR", "ClipCursor(NULL)");
    const BOOL result = g_clipCursor(rect);
    RECT actual{};
    GetClipCursor(&actual);
    Logger::Instance().Write("CURSOR", "ClipCursor -> %s; actual=(%ld,%ld)-(%ld,%ld)",
        result ? "TRUE" : "FALSE", actual.left, actual.top, actual.right, actual.bottom);
    g_insideHook = false;
    return result;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (g_insideHook) return g_setCursorPos(x, y);
    g_insideHook = true;
    const ULONGLONG now = GetTickCount64();
    const bool repeated = x == g_lastCursorX && y == g_lastCursorY;
    if (repeated && now - g_lastCursorLogAt < 1000) {
        ++g_suppressedCursorCalls;
    } else {
        if (g_suppressedCursorCalls)
            Logger::Instance().Write("CURSOR", "%u repeated SetCursorPos(%d,%d) calls suppressed",
                                     g_suppressedCursorCalls, g_lastCursorX, g_lastCursorY);
        Logger::Instance().Write("CURSOR", "SetCursorPos(%d,%d)", x, y);
        g_lastCursorX = x;
        g_lastCursorY = y;
        g_lastCursorLogAt = now;
        g_suppressedCursorCalls = 0;
    }
    const BOOL result = g_setCursorPos(x, y);
    g_insideHook = false;
    return result;
}

using TestCooperativeLevelFn = HRESULT(WINAPI*)(void*);
using ResetFn = HRESULT(WINAPI*)(void*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(WINAPI*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);
using CreateDeviceFn = HRESULT(WINAPI*)(void*, UINT, D3DDEVTYPE, HWND, DWORD,
                                        D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);

HRESULT WINAPI HookTestCooperativeLevel(void* self) {
    auto original = reinterpret_cast<TestCooperativeLevelFn>(g_d3dDeviceOriginalVtable[3]);
    const HRESULT result = original(self);
    State().cooperativeLevel = result;
    Logger::Instance().Write("D3D8", "TestCooperativeLevel -> 0x%08lX (%s)", result, HResultName(result));
    return result;
}

HRESULT WINAPI HookReset(void* self, D3DPRESENT_PARAMETERS* parameters) {
    if (parameters) Logger::Instance().Write("D3D8",
        "Reset called: %ux%u format=%lu windowed=%s refresh=%u hDeviceWindow=%p",
        parameters->BackBufferWidth, parameters->BackBufferHeight, parameters->BackBufferFormat,
        parameters->Windowed ? "TRUE" : "FALSE", parameters->FullScreen_RefreshRateInHz,
        parameters->hDeviceWindow);
    auto original = reinterpret_cast<ResetFn>(g_d3dDeviceOriginalVtable[14]);
    const HRESULT result = original(self, parameters);
    State().lastReset = result;
    Logger::Instance().Write("D3D8", "Reset -> 0x%08lX (%s)", result, HResultName(result));
    return result;
}

HRESULT WINAPI HookPresent(void* self, const RECT* source, const RECT* destination,
                           HWND overrideWindow, const RGNDATA* dirtyRegion) {
    auto original = reinterpret_cast<PresentFn>(g_d3dDeviceOriginalVtable[15]);
    const HRESULT result = original(self, source, destination, overrideWindow, dirtyRegion);
    const bool wasRecovering = State().recovering;
    RecordPresent(result);
    if (FAILED(result) || wasRecovering)
        Logger::Instance().Write("D3D8", "Present -> 0x%08lX (%s); frame=%llu",
                                 result, HResultName(result), State().frames.load());
    return result;
}

HRESULT WINAPI HookCreateDevice(void* self, UINT adapter, D3DDEVTYPE type, HWND focusWindow,
                                DWORD behavior, D3DPRESENT_PARAMETERS* parameters,
                                IDirect3DDevice8** device) {
    auto original = reinterpret_cast<CreateDeviceFn>(g_d3d8OriginalVtable[14]);
    const HRESULT result = original(self, adapter, type, focusWindow, behavior, parameters, device);
    Logger::Instance().Write("D3D8", "CreateDevice(adapter=%u, window=%p) -> 0x%08lX (%s), device=%p",
                             adapter, focusWindow, result, HResultName(result), device ? *device : nullptr);
    if (SUCCEEDED(result) && device && *device) {
        g_d3dDeviceObject = reinterpret_cast<void**>(*device);
        if (g_enableD3DDevice.load(std::memory_order_acquire) &&
            ReplaceVtable<97>(*device, &g_d3dDeviceOriginalVtable, &g_d3dDeviceHookVtable,
                              3, reinterpret_cast<void*>(HookTestCooperativeLevel))) {
            g_d3dDeviceHookVtable[14] = reinterpret_cast<void*>(HookReset);
            g_d3dDeviceHookVtable[15] = reinterpret_cast<void*>(HookPresent);
            State().d3dObserved = true;
            Logger::Instance().Write("D3D8", "Device method hooks installed");
        }
        if (focusWindow) State().gameWindow = focusWindow;
    }
    return result;
}

IDirect3D8* WINAPI HookDirect3DCreate8(UINT sdkVersion) {
    IDirect3D8* result = g_direct3DCreate8(sdkVersion);
    Logger::Instance().Write("D3D8", "Direct3DCreate8(%u) -> %p", sdkVersion, result);
    if (result && g_enableD3DCreateDevice.load(std::memory_order_acquire)) {
        g_d3d8Object = reinterpret_cast<void**>(result);
        ReplaceVtable<15>(result, &g_d3d8OriginalVtable, &g_d3d8HookVtable,
                          14, reinterpret_cast<void*>(HookCreateDevice));
    }
    return result;
}

using DInputCreateDeviceFn = HRESULT(WINAPI*)(void*, REFGUID, IDirectInputDevice8A**, LPUNKNOWN);
using AcquireFn = HRESULT(WINAPI*)(void*);
using UnacquireFn = HRESULT(WINAPI*)(void*);

HRESULT WINAPI HookMouseAcquire(void* self) {
    auto original = reinterpret_cast<AcquireFn>(g_mouseOriginalVtable[7]);
    const HRESULT result = original(self);
    State().mouseAcquire = result;
    Logger::Instance().Write("DINPUT/MOUSE", "Acquire -> 0x%08lX (%s)", result, HResultName(result));
    return result;
}
HRESULT WINAPI HookMouseUnacquire(void* self) {
    auto original = reinterpret_cast<UnacquireFn>(g_mouseOriginalVtable[8]);
    const HRESULT result = original(self);
    Logger::Instance().Write("DINPUT/MOUSE", "Unacquire -> 0x%08lX (%s)", result, HResultName(result));
    return result;
}
HRESULT WINAPI HookKeyboardAcquire(void* self) {
    auto original = reinterpret_cast<AcquireFn>(g_keyboardOriginalVtable[7]);
    const HRESULT result = original(self);
    State().keyboardAcquire = result;
    Logger::Instance().Write("DINPUT/KEYBOARD", "Acquire -> 0x%08lX (%s)", result, HResultName(result));
    return result;
}
HRESULT WINAPI HookKeyboardUnacquire(void* self) {
    auto original = reinterpret_cast<UnacquireFn>(g_keyboardOriginalVtable[8]);
    const HRESULT result = original(self);
    Logger::Instance().Write("DINPUT/KEYBOARD", "Unacquire -> 0x%08lX (%s)", result, HResultName(result));
    return result;
}

HRESULT WINAPI HookDInputCreateDevice(void* self, REFGUID guid, IDirectInputDevice8A** device,
                                      LPUNKNOWN outer) {
    auto original = reinterpret_cast<DInputCreateDeviceFn>(g_dinputOriginalVtable[3]);
    const HRESULT result = original(self, guid, device, outer);
    const bool mouse = IsEqualGUID(guid, kGuidSysMouse);
    const bool keyboard = IsEqualGUID(guid, kGuidSysKeyboard);
    Logger::Instance().Write("DINPUT", "CreateDevice(%s) -> 0x%08lX (%s), device=%p",
        mouse ? "MOUSE" : keyboard ? "KEYBOARD" : "OTHER", result, HResultName(result), device ? *device : nullptr);
    if (SUCCEEDED(result) && device && *device && mouse) {
        g_mouseObject = reinterpret_cast<void**>(*device);
        if (ReplaceVtable<32>(*device, &g_mouseOriginalVtable, &g_mouseHookVtable, 7,
                             reinterpret_cast<void*>(HookMouseAcquire)))
            g_mouseHookVtable[8] = reinterpret_cast<void*>(HookMouseUnacquire);
    } else if (SUCCEEDED(result) && device && *device && keyboard) {
        g_keyboardObject = reinterpret_cast<void**>(*device);
        if (ReplaceVtable<32>(*device, &g_keyboardOriginalVtable, &g_keyboardHookVtable, 7,
                             reinterpret_cast<void*>(HookKeyboardAcquire)))
            g_keyboardHookVtable[8] = reinterpret_cast<void*>(HookKeyboardUnacquire);
    }
    return result;
}

LRESULT CALLBACK HookWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            Logger::Instance().Write("WINDOW", "WM_ACTIVATE -> %u", LOWORD(wParam));
            if (LOWORD(wParam) == WA_INACTIVE) BeginFocusLoss(); else FocusReturned();
            break;
        case WM_ACTIVATEAPP:
            Logger::Instance().Write("WINDOW", "WM_ACTIVATEAPP -> %s", wParam ? "TRUE" : "FALSE");
            if (wParam) FocusReturned(); else BeginFocusLoss();
            break;
        case WM_SETFOCUS: Logger::Instance().Write("WINDOW", "WM_SETFOCUS"); FocusReturned(); break;
        case WM_KILLFOCUS: Logger::Instance().Write("WINDOW", "WM_KILLFOCUS"); BeginFocusLoss(); break;
        case WM_SIZE: Logger::Instance().Write("WINDOW", "WM_SIZE -> %llu (%dx%d)",
            static_cast<unsigned long long>(wParam), LOWORD(lParam), HIWORD(lParam)); break;
        case WM_SYSCOMMAND: Logger::Instance().Write("WINDOW", "WM_SYSCOMMAND -> 0x%llX",
            static_cast<unsigned long long>(wParam)); break;
        case WM_DISPLAYCHANGE: Logger::Instance().Write("WINDOW", "WM_DISPLAYCHANGE -> %dx%d",
            LOWORD(lParam), HIWORD(lParam)); break;
        case WM_KEYDOWN:
            if (wParam == VK_F10) Logger::Instance().Write("MARKER", "========== USER MARKER ==========");
            if (wParam == VK_F11) WriteSnapshot("MANUAL SNAPSHOT");
            break;
        case WM_TIMER:
            if (wParam == kDiagnosticTimer) CheckRecoveryTimeout();
            break;
    }
    CheckRecoveryTimeout();
    return CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);
}

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM output) {
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        *reinterpret_cast<HWND*>(output) = window;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

bool InstallHooks(HMODULE self) {
    g_self = self;
    const HookSettings settings = LoadHookSettings(self);
    const bool enableD3DCreateDevice = settings.d3d8 && settings.d3d8CreateDevice;
    const bool enableD3DDevice = enableD3DCreateDevice && settings.d3d8Device;
    g_enableDirectInput.store(settings.directInput, std::memory_order_release);
    g_enableD3DCreateDevice.store(enableD3DCreateDevice, std::memory_order_release);
    g_enableD3DDevice.store(enableD3DDevice, std::memory_order_release);
    HMODULE executable = GetModuleHandleW(nullptr);
    if (settings.cursor) {
        PatchImport(executable, "USER32.dll", "ClipCursor", reinterpret_cast<void*>(HookClipCursor),
                    reinterpret_cast<void**>(&g_clipCursor));
        PatchImport(executable, "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(HookSetCursorPos),
                    reinterpret_cast<void**>(&g_setCursorPos));
    }
    const bool d3d = settings.d3d8 && PatchImport(executable, "d3d8.dll", "Direct3DCreate8",
        reinterpret_cast<void*>(HookDirect3DCreate8), reinterpret_cast<void**>(&g_direct3DCreate8));
    if (settings.window) {
        for (unsigned attempt = 0; attempt < 300 && !g_window; ++attempt) {
            EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&g_window));
            if (!g_window) Sleep(100);
        }
        if (g_window) {
            State().gameWindow = g_window;
            SetLastError(0);
            g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
            if (g_originalWndProc) SetTimer(g_window, kDiagnosticTimer, 1000, nullptr);
        }
    }
    Logger::Instance().Write("DIAGNOSTIC", "Hook mode: window=%s d3d8=%s create-device=%s device-methods=%s dinput8=%s cursor=%s",
        g_originalWndProc ? "YES" : "NO", d3d ? "YES" : "NO",
        enableD3DCreateDevice ? "YES" : "NO", enableD3DDevice ? "YES" : "NO",
        settings.directInput ? "YES" : "PROXY-ONLY",
        (g_clipCursor || g_setCursorPos) ? "YES" : "NO");
    return true;
}

void TrackDirectInputObject(IDirectInput8A* object) {
    if (!object || !g_enableDirectInput.load(std::memory_order_acquire)) return;
    g_dinputObject = reinterpret_cast<void**>(object);
    if (ReplaceVtable<11>(object, &g_dinputOriginalVtable, &g_dinputHookVtable, 3,
                          reinterpret_cast<void*>(HookDInputCreateDevice))) {
        Logger::Instance().Write("DINPUT", "IDirectInput8 object captured through dinput8 proxy: %p", object);
    }
}

void RemoveHooks() {
    if (g_window && g_originalWndProc) {
        KillTimer(g_window, kDiagnosticTimer);
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
    RestoreVtable(g_keyboardObject, g_keyboardOriginalVtable, g_keyboardHookVtable);
    RestoreVtable(g_mouseObject, g_mouseOriginalVtable, g_mouseHookVtable);
    RestoreVtable(g_dinputObject, g_dinputOriginalVtable, g_dinputHookVtable);
    RestoreVtable(g_d3dDeviceObject, g_d3dDeviceOriginalVtable, g_d3dDeviceHookVtable);
    RestoreVtable(g_d3d8Object, g_d3d8OriginalVtable, g_d3d8HookVtable);
    while (g_importPatchCount) {
        const ImportPatch patch = g_importPatches[--g_importPatchCount];
        if (!patch.slot || *patch.slot != patch.replacement) continue;
        DWORD oldProtect{};
        if (VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *patch.slot = patch.original;
            VirtualProtect(patch.slot, sizeof(void*), oldProtect, &oldProtect);
        }
    }
    Logger::Instance().Write("DIAGNOSTIC", "Hooks removed");
}

}  // namespace fd
