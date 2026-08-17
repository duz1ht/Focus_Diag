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
constexpr UINT_PTR kCursorRecoveryTimer = 0xF0C06;

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ShowCursorFn = int(WINAPI*)(BOOL);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);
ClipCursorFn g_clipCursor{};
SetCursorPosFn g_setCursorPos{};
ShowCursorFn g_showCursor{};
SetCursorFn g_setCursor{};
Direct3DCreate8Fn g_direct3DCreate8{};
struct ImportPatch { void** slot; void* original; void* replacement; };
std::array<ImportPatch, 6> g_importPatches{};
size_t g_importPatchCount{};

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
bool g_restoreCursorClip = false;
bool g_waitForDisplayChange = true;
UINT g_restoreCursorClipDelayMs = 250;

struct HookSettings {
    bool window = false;
    bool d3d8 = false;
    bool d3d8CreateDevice = false;
    bool d3d8Device = false;
    bool directInput = false;
    bool cursor = false;
    bool restoreCursorClip = false;
    bool waitForDisplayChange = true;
    bool useSetCapture = false;
    bool forceWindowActivation = false;
    bool forceDirectInputAcquire = false;
    UINT restoreCursorClipDelayMs = 250;
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
    settings.restoreCursorClip = GetPrivateProfileIntW(L"Recovery", L"RestoreCursorClip", 0, path) != 0;
    settings.waitForDisplayChange = GetPrivateProfileIntW(L"Recovery", L"WaitForDisplayChange", 1, path) != 0;
    const UINT delay = GetPrivateProfileIntW(L"Recovery", L"RestoreCursorClipDelayMs", 250, path);
    settings.restoreCursorClipDelayMs = delay < 1 ? 1 : (delay > 10000 ? 10000 : delay);
    settings.useSetCapture = GetPrivateProfileIntW(L"Recovery", L"UseSetCapture", 0, path) != 0;
    settings.forceWindowActivation = GetPrivateProfileIntW(L"Recovery", L"ForceWindowActivation", 0, path) != 0;
    settings.forceDirectInputAcquire = GetPrivateProfileIntW(L"Recovery", L"ForceDirectInputAcquire", 0, path) != 0;
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
    RecordClipState(rect, actual, result != FALSE);
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

int WINAPI HookShowCursor(BOOL show) {
    const int result = g_showCursor(show);
    State().cursorVisibilityObserved = true;
    State().showCursorObserved = true;
    State().lastShowCursorResult = result;
    Logger::Instance().Write("CURSOR/VISIBILITY", "ShowCursor(%s) -> %d",
                             show ? "TRUE" : "FALSE", result);
    return result;
}

HCURSOR WINAPI HookSetCursor(HCURSOR cursor) {
    const HCURSOR previous = g_setCursor(cursor);
    State().cursorVisibilityObserved = true;
    Logger::Instance().Write("CURSOR/VISIBILITY", "SetCursor(%p) -> previous=%p",
                             cursor, previous);
    return previous;
}

IDirect3D8* WINAPI HookDirect3DCreate8(UINT sdkVersion) {
    IDirect3D8* result = g_direct3DCreate8(sdkVersion);
    Logger::Instance().Write("D3D8", "Direct3DCreate8(%u) -> %p", sdkVersion, result);
    return result;
}

using DInputCreateDeviceFn = HRESULT(WINAPI*)(void*, REFGUID, IDirectInputDevice8A**, LPUNKNOWN);
using AcquireFn = HRESULT(WINAPI*)(void*);
using UnacquireFn = HRESULT(WINAPI*)(void*);

HRESULT WINAPI HookMouseAcquire(void* self) {
    auto original = reinterpret_cast<AcquireFn>(g_mouseOriginalVtable[7]);
    const HRESULT result = original(self);
    State().mouseObserved = true;
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
    State().keyboardObserved = true;
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

bool RectsEqual(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

void CancelCursorRecovery(bool releaseAppliedClip) {
    if (g_window) KillTimer(g_window, kCursorRecoveryTimer);
    auto& state = State();
    state.clipRestorePending = false;
    if (releaseAppliedClip && state.clipAppliedByDiagnostic.exchange(false) && g_clipCursor) {
        const BOOL result = g_clipCursor(nullptr);
        Logger::Instance().Write("CURSOR RECOVERY ACTION",
            "Released diagnostic clip after focus loss -> %s", result ? "TRUE" : "FALSE");
    }
}

void ArmCursorRecovery(bool displayModeConfirmed) {
    if (!g_restoreCursorClip || !g_window || !g_clipCursor) return;
    auto& state = State();
    const unsigned long attempt = state.attempt.load();
    if (!state.recovering || !state.restoreClipExpected ||
        state.clipRestoredAttempt.load() == attempt)
        return;

    const UINT delay = displayModeConfirmed || !g_waitForDisplayChange
        ? g_restoreCursorClipDelayMs : 2000 + g_restoreCursorClipDelayMs;
    KillTimer(g_window, kCursorRecoveryTimer);
    if (SetTimer(g_window, kCursorRecoveryTimer, delay, nullptr)) {
        state.clipRestorePending = true;
        Logger::Instance().Write("CURSOR RECOVERY ACTION",
            "Attempt %lu armed; delay=%u ms; display-confirmed=%s", attempt, delay,
            displayModeConfirmed ? "YES" : "NO");
    }
}

void ApplyCursorRecovery() {
    auto& state = State();
    state.clipRestorePending = false;
    const unsigned long attempt = state.attempt.load();
    if (!g_restoreCursorClip || !state.recovering || !state.restoreClipExpected ||
        state.clipRestoredAttempt.load() == attempt || !g_clipCursor)
        return;

    HWND game = state.gameWindow.load();
    const bool validWindow = game && GetForegroundWindow() == game && GetFocus() == game &&
                             !IsIconic(game) && IsWindowVisible(game);
    RECT before{}, after{};
    GetClipCursor(&before);
    const RECT expected{state.expectedClipLeft.load(), state.expectedClipTop.load(),
                        state.expectedClipRight.load(), state.expectedClipBottom.load()};
    if (!validWindow) {
        Logger::Instance().Write("CURSOR RECOVERY ACTION",
            "Attempt %lu cancelled; game=%p foreground=%p focus=%p", attempt, game,
            GetForegroundWindow(), GetFocus());
        return;
    }

    BOOL result = TRUE;
    const bool needed = !RectsEqual(before, expected);
    if (needed) result = g_clipCursor(&expected);
    GetClipCursor(&after);
    state.clipRestoredAttempt = attempt;
    state.clipAppliedByDiagnostic = needed && result && RectsEqual(after, expected);
    Logger::Instance().Write("CURSOR RECOVERY ACTION",
        "Attempt %lu expected=(%ld,%ld)-(%ld,%ld) before=(%ld,%ld)-(%ld,%ld) after=(%ld,%ld)-(%ld,%ld) needed=%s result=%s restored=%s hwnd=%p elapsed=%llu ms",
        attempt, expected.left, expected.top, expected.right, expected.bottom,
        before.left, before.top, before.right, before.bottom,
        after.left, after.top, after.right, after.bottom, needed ? "YES" : "NO",
        result ? "TRUE" : "FALSE", RectsEqual(after, expected) ? "YES" : "NO", game,
        state.focusReturnedAt ? GetTickCount64() - state.focusReturnedAt.load() : 0);
}

LRESULT CALLBACK HookWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETCURSOR) {
        CheckRecoveryTimeout();
        const LRESULT result = CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);
        State().cursorVisibilityObserved = true;
        Logger::Instance().Write("CURSOR/VISIBILITY",
            "WM_SETCURSOR hwnd=%p hit-test=%u mouse-message=%u -> %lld",
            reinterpret_cast<HWND>(wParam), static_cast<unsigned>(LOWORD(lParam)),
            static_cast<unsigned>(HIWORD(lParam)), static_cast<long long>(result));
        return result;
    }
    switch (message) {
        case WM_ACTIVATE:
            Logger::Instance().Write("WINDOW", "WM_ACTIVATE -> %u", LOWORD(wParam));
            if (LOWORD(wParam) == WA_INACTIVE) {
                CancelCursorRecovery(true);
                BeginFocusLoss();
            } else {
                FocusReturned();
                ArmCursorRecovery(false);
            }
            break;
        case WM_ACTIVATEAPP:
            Logger::Instance().Write("WINDOW", "WM_ACTIVATEAPP -> %s", wParam ? "TRUE" : "FALSE");
            if (wParam) {
                FocusReturned();
                ArmCursorRecovery(false);
            } else {
                CancelCursorRecovery(true);
                BeginFocusLoss();
            }
            break;
        case WM_SETFOCUS:
            Logger::Instance().Write("WINDOW", "WM_SETFOCUS");
            FocusReturned();
            ArmCursorRecovery(false);
            break;
        case WM_KILLFOCUS:
            Logger::Instance().Write("WINDOW", "WM_KILLFOCUS");
            CancelCursorRecovery(true);
            BeginFocusLoss();
            break;
        case WM_SIZE: Logger::Instance().Write("WINDOW", "WM_SIZE -> %llu (%dx%d)",
            static_cast<unsigned long long>(wParam), LOWORD(lParam), HIWORD(lParam)); break;
        case WM_SYSCOMMAND: Logger::Instance().Write("WINDOW", "WM_SYSCOMMAND -> 0x%llX",
            static_cast<unsigned long long>(wParam)); break;
        case WM_DISPLAYCHANGE: {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            Logger::Instance().Write("WINDOW", "WM_DISPLAYCHANGE -> %ux%u", width, height);
            RecordDisplayChange(width, height);
            auto& state = State();
            if (state.recovering && state.focusReturnedAt &&
                width == state.expectedDisplayWidth && height == state.expectedDisplayHeight)
                ArmCursorRecovery(true);
            break;
        }
        case WM_DESTROY:
        case WM_NCDESTROY:
            CancelCursorRecovery(true);
            break;
        case WM_KEYDOWN:
            if (wParam == VK_F10) Logger::Instance().Write("MARKER", "========== USER MARKER ==========");
            if (wParam == VK_F11) WriteSnapshot("MANUAL SNAPSHOT");
            break;
        case WM_TIMER:
            if (wParam == kDiagnosticTimer) CheckRecoveryTimeout();
            if (wParam == kCursorRecoveryTimer) {
                KillTimer(window, kCursorRecoveryTimer);
                ApplyCursorRecovery();
            }
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
    // Delta Force 1.5.0.5 calls through this D3D8 vtable with an ABI that does
    // not match either the documented COM __stdcall ABI or MSVC __thiscall.
    // Both variants corrupt ESP in the game's Debug runtime. Keep factory-only
    // observation until a target-specific assembly bridge is available.
    g_enableDirectInput.store(settings.directInput, std::memory_order_release);
    g_restoreCursorClip = settings.restoreCursorClip;
    g_waitForDisplayChange = settings.waitForDisplayChange;
    g_restoreCursorClipDelayMs = settings.restoreCursorClipDelayMs;
    HMODULE executable = GetModuleHandleW(nullptr);
    if (settings.cursor) {
        PatchImport(executable, "USER32.dll", "ClipCursor", reinterpret_cast<void*>(HookClipCursor),
                    reinterpret_cast<void**>(&g_clipCursor));
        PatchImport(executable, "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(HookSetCursorPos),
                    reinterpret_cast<void**>(&g_setCursorPos));
        PatchImport(executable, "USER32.dll", "ShowCursor", reinterpret_cast<void*>(HookShowCursor),
                    reinterpret_cast<void**>(&g_showCursor));
        PatchImport(executable, "USER32.dll", "SetCursor", reinterpret_cast<void*>(HookSetCursor),
                    reinterpret_cast<void**>(&g_setCursor));
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
        settings.d3d8CreateDevice ? "DISABLED-ABI" : "NO",
        settings.d3d8Device ? "DISABLED-ABI" : "NO",
        settings.directInput ? "YES" : "PROXY-ONLY",
        (g_clipCursor || g_setCursorPos) ? "YES" : "NO");
    Logger::Instance().Write("DIAGNOSTIC",
        "Recovery mode: restore-clip=%s delay=%u wait-display=%s set-capture=%s force-window=%s force-acquire=%s",
        settings.restoreCursorClip ? "YES" : "NO", settings.restoreCursorClipDelayMs,
        settings.waitForDisplayChange ? "YES" : "NO", settings.useSetCapture ? "RESERVED" : "NO",
        settings.forceWindowActivation ? "RESERVED" : "NO",
        settings.forceDirectInputAcquire ? "RESERVED" : "NO");
    if (settings.restoreCursorClip && (!settings.cursor || !settings.window))
        Logger::Instance().Write("DIAGNOSTIC",
            "RestoreCursorClip requires Hooks.Window=1 and Hooks.Cursor=1; action disabled");
    if (!settings.cursor || !settings.window) g_restoreCursorClip = false;
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
        CancelCursorRecovery(true);
        KillTimer(g_window, kDiagnosticTimer);
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
    RestoreVtable(g_keyboardObject, g_keyboardOriginalVtable, g_keyboardHookVtable);
    RestoreVtable(g_mouseObject, g_mouseOriginalVtable, g_mouseHookVtable);
    RestoreVtable(g_dinputObject, g_dinputOriginalVtable, g_dinputHookVtable);
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
