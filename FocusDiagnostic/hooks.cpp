#include "hooks.h"

#include "diagnostics.h"
#include "logger.h"

#include <atomic>
#include <cstring>

namespace fd {
namespace {

constexpr UINT_PTR kDiagnosticTimer = 0xF0C05;
constexpr UINT_PTR kCursorRecoveryTimer = 0xF0C06;

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

HWND g_window{};
WNDPROC g_originalWndProc{};
ClipCursorFn g_clipCursor{};
thread_local bool g_insideClipHook = false;
bool g_restoreCursorClip = true;
bool g_waitForDisplayChange = true;
UINT g_restoreCursorClipDelayMs = 250;

BOOL WINAPI HookClipCursor(const RECT* rect);

struct Settings {
    bool restoreCursorClip = true;
    bool waitForDisplayChange = true;
    UINT delayMs = 250;
};

Settings LoadSettings(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(path, L"FocusDiagnostic.ini");

    Settings settings;
    settings.restoreCursorClip =
        GetPrivateProfileIntW(L"Recovery", L"RestoreCursorClip", 1, path) != 0;
    settings.waitForDisplayChange =
        GetPrivateProfileIntW(L"Recovery", L"WaitForDisplayChange", 1, path) != 0;
    const UINT delay = GetPrivateProfileIntW(
        L"Recovery", L"RestoreCursorClipDelayMs", 250, path);
    settings.delayMs = delay < 1 ? 1 : (delay > 10000 ? 10000 : delay);
    return settings;
}

bool PatchClipCursorImport(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* moduleName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, "USER32.dll") != 0) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk) : thunk;
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(import->Name), "ClipCursor") != 0) continue;

            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD oldProtect{};
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            g_clipCursor = reinterpret_cast<ClipCursorFn>(*slot);
            *slot = reinterpret_cast<void*>(HookClipCursor);
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

BOOL WINAPI HookClipCursor(const RECT* rect) {
    if (g_insideClipHook) return g_clipCursor(rect);
    g_insideClipHook = true;
    const BOOL result = g_clipCursor(rect);
    RECT actual{};
    const BOOL queried = GetClipCursor(&actual);
    RecordClipState(rect, actual, result != FALSE && queried != FALSE);
    if (rect) {
        Logger::Instance().Write("CLIPCURSOR",
            "requested=(%ld,%ld)-(%ld,%ld) result=%s query=%s actual=(%ld,%ld)-(%ld,%ld)",
            rect->left, rect->top, rect->right, rect->bottom,
            result ? "TRUE" : "FALSE", queried ? "OK" : "FAILED",
            actual.left, actual.top, actual.right, actual.bottom);
    } else {
        Logger::Instance().Write("CLIPCURSOR",
            "requested=NULL result=%s query=%s actual=(%ld,%ld)-(%ld,%ld)",
            result ? "TRUE" : "FALSE", queried ? "OK" : "FAILED",
            actual.left, actual.top, actual.right, actual.bottom);
    }
    g_insideClipHook = false;
    return result;
}

void CancelCursorRecovery(bool releaseAppliedClip) {
    if (g_window) KillTimer(g_window, kCursorRecoveryTimer);
    auto& state = State();
    state.clipRestorePending = false;
    if (releaseAppliedClip && state.clipAppliedByDiagnostic.exchange(false) && g_clipCursor) {
        const BOOL result = g_clipCursor(nullptr);
        Logger::Instance().Write("CLIPCURSOR/RECOVERY",
            "released DLL-applied clip -> %s", result ? "TRUE" : "FALSE");
    }
}

void FinishWithoutRestore(const char* reason) {
    auto& state = State();
    if (g_window) KillTimer(g_window, kCursorRecoveryTimer);
    state.clipRestorePending = false;
    state.recovering = false;
    Logger::Instance().Write("CLIPCURSOR/RECOVERY", "Attempt %lu: %s",
                             state.attempt.load(), reason);
    WriteSnapshot(reason);
}

void ArmCursorRecovery(bool displayConfirmed) {
    auto& state = State();
    if (!state.recovering || !state.focusReturned) return;
    if (!state.restoreClipExpected) {
        FinishWithoutRestore("NO ACTIVE CLIP CAPTURED BEFORE FOCUS LOSS");
        return;
    }
    if (!g_restoreCursorClip) {
        FinishWithoutRestore("RESTORATION DISABLED");
        return;
    }
    if (state.clipRestoredAttempt.load() == state.attempt.load()) return;
    if (state.clipRestorePending && !displayConfirmed) return;

    const UINT delay = displayConfirmed || !g_waitForDisplayChange
        ? g_restoreCursorClipDelayMs : 2000 + g_restoreCursorClipDelayMs;
    KillTimer(g_window, kCursorRecoveryTimer);
    if (SetTimer(g_window, kCursorRecoveryTimer, delay, nullptr)) {
        state.clipRestorePending = true;
        Logger::Instance().Write("CLIPCURSOR/RECOVERY",
            "Attempt %lu armed; delay=%u ms; display-confirmed=%s",
            state.attempt.load(), delay, displayConfirmed ? "YES" : "NO");
    } else {
        FinishWithoutRestore("FAILED TO CREATE RECOVERY TIMER");
    }
}

void ApplyCursorRecovery() {
    auto& state = State();
    state.clipRestorePending = false;
    const unsigned long attempt = state.attempt.load();
    if (!state.recovering || !state.restoreClipExpected ||
        state.clipRestoredAttempt.load() == attempt || !g_clipCursor)
        return;

    const HWND game = state.gameWindow.load();
    const bool validWindow = game && GetForegroundWindow() == game && GetFocus() == game &&
                             !IsIconic(game) && IsWindowVisible(game);
    if (!validWindow) {
        FinishWithoutRestore("WINDOW VALIDATION FAILED");
        return;
    }

    RECT before{}, after{};
    const BOOL queriedBefore = GetClipCursor(&before);
    const RECT expected = ExpectedClip();
    const bool needed = !queriedBefore || !RectsEqual(before, expected);
    const BOOL applied = needed ? g_clipCursor(&expected) : TRUE;
    const BOOL queriedAfter = GetClipCursor(&after);
    const bool restored = applied && queriedAfter && RectsEqual(after, expected);

    state.clipRestoredAttempt = attempt;
    state.clipAppliedByDiagnostic = needed && restored;
    if (restored) state.clipActive = true;
    state.recovering = false;
    Logger::Instance().Write("CLIPCURSOR/RECOVERY",
        "Attempt %lu expected=(%ld,%ld)-(%ld,%ld) before=(%ld,%ld)-(%ld,%ld) "
        "after=(%ld,%ld)-(%ld,%ld) needed=%s call=%s query=%s restored=%s elapsed=%llu ms",
        attempt, expected.left, expected.top, expected.right, expected.bottom,
        before.left, before.top, before.right, before.bottom,
        after.left, after.top, after.right, after.bottom, needed ? "YES" : "NO",
        applied ? "TRUE" : "FALSE", queriedAfter ? "OK" : "FAILED",
        restored ? "YES" : "NO",
        state.focusReturnedAt ? GetTickCount64() - state.focusReturnedAt.load() : 0);
    WriteSnapshot(restored ? "CLIP RESTORATION SUCCESS" : "CLIP RESTORATION FAILURE");
}

void CheckRecoveryTimeout() {
    auto& state = State();
    const ULONGLONG returnedAt = state.focusReturnedAt.load();
    if (state.recovering && returnedAt && GetTickCount64() - returnedAt >= 5000)
        FinishWithoutRestore("CLIP RESTORATION TIMEOUT");
}

LRESULT CALLBACK HookWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            Logger::Instance().Write("FOCUS", "WM_ACTIVATE -> %u", LOWORD(wParam));
            if (LOWORD(wParam) == WA_INACTIVE) {
                CancelCursorRecovery(true);
                BeginFocusLoss();
            } else {
                RecordFocusReturn();
                ArmCursorRecovery(false);
            }
            break;
        case WM_ACTIVATEAPP:
            Logger::Instance().Write("FOCUS", "WM_ACTIVATEAPP -> %s", wParam ? "TRUE" : "FALSE");
            if (wParam) {
                RecordFocusReturn();
                ArmCursorRecovery(false);
            } else {
                CancelCursorRecovery(true);
                BeginFocusLoss();
            }
            break;
        case WM_SETFOCUS:
            Logger::Instance().Write("FOCUS", "WM_SETFOCUS");
            RecordFocusReturn();
            ArmCursorRecovery(false);
            break;
        case WM_KILLFOCUS:
            Logger::Instance().Write("FOCUS", "WM_KILLFOCUS");
            CancelCursorRecovery(true);
            BeginFocusLoss();
            break;
        case WM_DISPLAYCHANGE: {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            RecordDisplayChange(width, height);
            Logger::Instance().Write("DISPLAY", "WM_DISPLAYCHANGE -> %ux%u", width, height);
            if (State().displayConfirmed) ArmCursorRecovery(true);
            break;
        }
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
        case WM_DESTROY:
        case WM_NCDESTROY:
            CancelCursorRecovery(true);
            break;
    }
    return CallWindowProcW(g_originalWndProc, window, message, wParam, lParam);
}

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM output) {
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == nullptr) {
        *reinterpret_cast<HWND*>(output) = window;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

bool InstallHooks(HMODULE self) {
    const Settings settings = LoadSettings(self);
    g_restoreCursorClip = settings.restoreCursorClip;
    g_waitForDisplayChange = settings.waitForDisplayChange;
    g_restoreCursorClipDelayMs = settings.delayMs;

    const bool clipHooked = PatchClipCursorImport(GetModuleHandleW(nullptr));
    for (unsigned attempt = 0; attempt < 300 && !g_window; ++attempt) {
        EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&g_window));
        if (!g_window) Sleep(100);
    }
    if (g_window) {
        State().gameWindow = g_window;
        State().displayWidth = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
        State().displayHeight = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
        SetLastError(0);
        g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
        if (g_originalWndProc) SetTimer(g_window, kDiagnosticTimer, 1000, nullptr);
    }

    Logger::Instance().Write("DIAGNOSTIC",
        "ClipCursor-only mode: clip-hook=%s window-hook=%s restore=%s delay=%u wait-display=%s",
        clipHooked ? "YES" : "NO", g_originalWndProc ? "YES" : "NO",
        g_restoreCursorClip ? "YES" : "NO", g_restoreCursorClipDelayMs,
        g_waitForDisplayChange ? "YES" : "NO");
    if (!clipHooked)
        Logger::Instance().Write("DIAGNOSTIC", "ClipCursor import not found; restoration unavailable");
    if (!g_originalWndProc)
        Logger::Instance().Write("DIAGNOSTIC", "Game window hook unavailable; restoration unavailable");
    return clipHooked && g_originalWndProc;
}

}  // namespace fd
