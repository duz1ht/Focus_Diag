#include "hooks.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>

namespace fd {
namespace {

constexpr UINT_PTR kCursorRecoveryTimer = 0xF0C06;
constexpr UINT kCursorRecoveryDelayMs = 250;
constexpr UINT kDisplayChangeFallbackMs = 2000;

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

struct ImportPatch {
    void** slot;
    void* original;
    void* replacement;
};

struct RecoveryState {
    std::atomic<bool> clipActive{false};
    std::atomic<bool> restoreClipExpected{false};
    std::atomic<bool> recovering{false};
    std::atomic<bool> focusReturned{false};
    std::atomic<bool> clipAppliedByRecovery{false};
    std::atomic<LONG> clipLeft{0};
    std::atomic<LONG> clipTop{0};
    std::atomic<LONG> clipRight{0};
    std::atomic<LONG> clipBottom{0};
    std::atomic<UINT> displayWidth{0};
    std::atomic<UINT> displayHeight{0};
    std::atomic<UINT> expectedDisplayWidth{0};
    std::atomic<UINT> expectedDisplayHeight{0};
};

HWND g_window{};
WNDPROC g_originalWndProc{};
ClipCursorFn g_clipCursor{};
std::array<ImportPatch, 1> g_importPatches{};
size_t g_importPatchCount{};
RecoveryState g_state;
thread_local bool g_insideClipHook = false;

bool RectsEqual(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

RECT SavedClip() {
    return {g_state.clipLeft.load(), g_state.clipTop.load(),
            g_state.clipRight.load(), g_state.clipBottom.load()};
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
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk)
            : thunk;
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(import->Name), function) != 0) continue;

            auto slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD oldProtect{};
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            *original = *slot;
            g_importPatches[g_importPatchCount++] = {slot, *slot, replacement};
            *slot = replacement;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

BOOL WINAPI HookClipCursor(const RECT* requested) {
    if (g_insideClipHook) return g_clipCursor(requested);
    g_insideClipHook = true;
    const BOOL result = g_clipCursor(requested);
    if (result) {
        g_state.clipActive = requested != nullptr;
        if (requested) {
            RECT actual{};
            if (GetClipCursor(&actual)) {
                g_state.clipLeft = actual.left;
                g_state.clipTop = actual.top;
                g_state.clipRight = actual.right;
                g_state.clipBottom = actual.bottom;
            }
        }
    }
    g_insideClipHook = false;
    return result;
}

void CancelCursorRecovery(bool releaseAppliedClip) {
    if (g_window) KillTimer(g_window, kCursorRecoveryTimer);
    if (releaseAppliedClip && g_state.clipAppliedByRecovery.exchange(false) && g_clipCursor)
        g_clipCursor(nullptr);
    g_state.focusReturned = false;
    g_state.recovering = false;
}

void BeginFocusLoss() {
    if (g_state.recovering.exchange(true)) return;
    g_state.focusReturned = false;
    g_state.restoreClipExpected = g_state.clipActive.load();
    g_state.expectedDisplayWidth = g_state.displayWidth.load();
    g_state.expectedDisplayHeight = g_state.displayHeight.load();
}

void ArmCursorRecovery(bool displayModeConfirmed) {
    if (!g_window || !g_clipCursor || !g_state.recovering ||
        !g_state.focusReturned || !g_state.restoreClipExpected)
        return;

    const UINT delay = displayModeConfirmed
        ? kCursorRecoveryDelayMs
        : kDisplayChangeFallbackMs + kCursorRecoveryDelayMs;
    KillTimer(g_window, kCursorRecoveryTimer);
    SetTimer(g_window, kCursorRecoveryTimer, delay, nullptr);
}

void ApplyCursorRecovery() {
    if (!g_state.recovering.exchange(false) || !g_state.restoreClipExpected || !g_clipCursor) return;

    const HWND game = g_window;
    if (!game || GetForegroundWindow() != game || GetFocus() != game ||
        IsIconic(game) || !IsWindowVisible(game))
        return;

    const RECT expected = SavedClip();
    RECT current{};
    if (!GetClipCursor(&current) || RectsEqual(current, expected)) return;

    const BOOL result = g_clipCursor(&expected);
    RECT actual{};
    g_state.clipAppliedByRecovery = result && GetClipCursor(&actual) && RectsEqual(actual, expected);
}

LRESULT CALLBACK HookWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                CancelCursorRecovery(true);
                BeginFocusLoss();
            } else {
                g_state.focusReturned = true;
                ArmCursorRecovery(false);
            }
            break;
        case WM_ACTIVATEAPP:
            if (wParam) {
                g_state.focusReturned = true;
                ArmCursorRecovery(false);
            } else {
                CancelCursorRecovery(true);
                BeginFocusLoss();
            }
            break;
        case WM_SETFOCUS:
            g_state.focusReturned = true;
            ArmCursorRecovery(false);
            break;
        case WM_KILLFOCUS:
            CancelCursorRecovery(true);
            BeginFocusLoss();
            break;
        case WM_DISPLAYCHANGE: {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            g_state.displayWidth = width;
            g_state.displayHeight = height;
            if (g_state.recovering && g_state.focusReturned &&
                width == g_state.expectedDisplayWidth && height == g_state.expectedDisplayHeight)
                ArmCursorRecovery(true);
            break;
        }
        case WM_DESTROY:
        case WM_NCDESTROY:
            CancelCursorRecovery(true);
            break;
        case WM_TIMER:
            if (wParam == kCursorRecoveryTimer) {
                KillTimer(window, kCursorRecoveryTimer);
                ApplyCursorRecovery();
            }
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

bool InstallHooks() {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!PatchImport(executable, "USER32.dll", "ClipCursor",
                     reinterpret_cast<void*>(HookClipCursor),
                     reinterpret_cast<void**>(&g_clipCursor)))
        return false;

    for (unsigned attempt = 0; attempt < 300 && !g_window; ++attempt) {
        EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&g_window));
        if (!g_window) Sleep(100);
    }
    if (!g_window) {
        RemoveHooks();
        return false;
    }

    g_state.displayWidth = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    g_state.displayHeight = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
    if (!g_originalWndProc) {
        RemoveHooks();
        return false;
    }
    return true;
}

void RemoveHooks() {
    CancelCursorRecovery(true);
    if (g_window && g_originalWndProc) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
    }
    while (g_importPatchCount) {
        const ImportPatch patch = g_importPatches[--g_importPatchCount];
        if (!patch.slot || *patch.slot != patch.replacement) continue;
        DWORD oldProtect{};
        if (VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *patch.slot = patch.original;
            VirtualProtect(patch.slot, sizeof(void*), oldProtect, &oldProtect);
        }
    }
}

}  // namespace fd
