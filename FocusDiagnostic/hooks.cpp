#include "hooks.h"

#include "diagnostics.h"
#include "legacy_dx8.h"
#include "logger.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace fd {
namespace {

using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ShowCursorFn = int(WINAPI*)(BOOL);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);

HWND g_window{};
WNDPROC g_originalWndProc{};
ClipCursorFn g_clipCursor{};
SetCursorPosFn g_setCursorPos{};
ShowCursorFn g_showCursor{};
SetCursorFn g_setCursor{};
thread_local bool g_insideClipHook = false;
bool g_restoreCursorClip = true;
bool g_waitForDisplayChange = true;
bool g_cursorTelemetry = true;
bool g_revalidateCursorClip = true;
UINT g_revalidationWindowMs = 5000;
UINT g_maxClipReapplications = 3;
UINT g_restoreCursorClipDelayMs = 250;
HANDLE g_scheduleEvent{};
HANDLE g_schedulerThread{};
std::atomic<bool> g_scheduleArmed{false};
std::atomic<unsigned long> g_scheduledAttempt{0};
std::atomic<ULONGLONG> g_scheduledAt{0};
std::atomic<ULONGLONG> g_scheduledDue{0};
std::atomic<UINT> g_scheduledDelay{0};
std::recursive_mutex g_recoveryMutex;

BOOL WINAPI HookClipCursor(const RECT* rect);

const char* WindowSizeStateName(WPARAM state) {
    switch (state) {
        case SIZE_RESTORED: return "SIZE_RESTORED";
        case SIZE_MINIMIZED: return "SIZE_MINIMIZED";
        case SIZE_MAXIMIZED: return "SIZE_MAXIMIZED";
        case SIZE_MAXSHOW: return "SIZE_MAXSHOW";
        case SIZE_MAXHIDE: return "SIZE_MAXHIDE";
        default: return "SIZE_UNKNOWN";
    }
}

struct Settings {
    bool restoreCursorClip = true;
    bool waitForDisplayChange = true;
    UINT delayMs = 250;
    bool cursorTelemetry = true;
    bool directInputDiagnostics = true;
    bool revalidateCursorClip = true;
    UINT revalidationWindowMs = 5000;
    UINT maxClipReapplications = 3;
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
    settings.cursorTelemetry =
        GetPrivateProfileIntW(L"Diagnostics", L"CursorTelemetry", 1, path) != 0;
    settings.directInputDiagnostics =
        GetPrivateProfileIntW(L"Diagnostics", L"DirectInput", 1, path) != 0;
    settings.revalidateCursorClip =
        GetPrivateProfileIntW(L"Recovery", L"RevalidateCursorClip", 1, path) != 0;
    const UINT window = GetPrivateProfileIntW(
        L"Recovery", L"RevalidationWindowMs", 5000, path);
    settings.revalidationWindowMs = window < 500 ? 500 : (window > 30000 ? 30000 : window);
    const UINT retries = GetPrivateProfileIntW(
        L"Recovery", L"MaxClipReapplications", 3, path);
    settings.maxClipReapplications = retries > 10 ? 10 : retries;
    return settings;
}

bool PatchUser32Import(HMODULE module, const char* function, void* hook, void** original) {
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
            if (strcmp(reinterpret_cast<char*>(import->Name), function) != 0) continue;

            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD oldProtect{};
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            *original = *slot;
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    const BOOL result = g_setCursorPos(x, y);
    auto& state = State();
    state.lastCursorX = x;
    state.lastCursorY = y;
    state.lastCursorPositionAt = GetTickCount64();
    RECT client{};
    const HWND game = state.gameWindow.load();
    POINT center{};
    if (game && GetClientRect(game, &client)) {
        center = {(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(game, &center);
    }
    const bool centered = game && x == center.x && y == center.y;
    Logger::Instance().Write("CURSOR/SET-POS",
        "position=(%d,%d) result=%s centered-client=%s negative=%s attempt=%lu",
        x, y, result ? "TRUE" : "FALSE", centered ? "YES" : "NO",
        x < 0 || y < 0 ? "YES" : "NO", state.attempt.load());
    return result;
}

int WINAPI HookShowCursor(BOOL show) {
    const int result = g_showCursor(show);
    State().showCursorResult = result;
    Logger::Instance().Write("CURSOR/SHOW", "show=%s result-count=%d attempt=%lu",
        show ? "TRUE" : "FALSE", result, State().attempt.load());
    return result;
}

HCURSOR WINAPI HookSetCursor(HCURSOR cursor) {
    const HCURSOR previous = g_setCursor(cursor);
    State().lastCursor = cursor;
    Logger::Instance().Write("CURSOR/SET",
        "requested=%p previous=%p current=%p attempt=%lu", cursor, previous,
        GetCursor(), State().attempt.load());
    return previous;
}

BOOL WINAPI HookClipCursor(const RECT* rect) {
    if (g_insideClipHook) return g_clipCursor(rect);
    std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
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
    std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
    g_scheduleArmed = false;
    if (g_scheduleEvent) SetEvent(g_scheduleEvent);
    auto& state = State();
    state.clipRestorePending = false;
    if (releaseAppliedClip && state.clipAppliedByDiagnostic.exchange(false) && g_clipCursor) {
        const BOOL result = g_clipCursor(nullptr);
        Logger::Instance().Write("CLIPCURSOR/RECOVERY",
            "released DLL-applied clip -> %s", result ? "TRUE" : "FALSE");
    }
}

void FinishWithoutRestore(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
    auto& state = State();
    g_scheduleArmed = false;
    if (g_scheduleEvent) SetEvent(g_scheduleEvent);
    state.clipRestorePending = false;
    state.recovering = false;
    Logger::Instance().Write("CLIPCURSOR/RECOVERY", "Attempt %lu: %s",
                             state.attempt.load(), reason);
    WriteSnapshot(reason);
}

void ArmCursorRecovery(bool displayConfirmed) {
    std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
    auto& state = State();
    if (!state.recovering || !state.focusReturned ||
        state.deactivationState != DeactivationState::FocusTransitionConfirmed)
        return;
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
    if (!g_scheduleEvent || !g_schedulerThread) {
        FinishWithoutRestore("RECOVERY SCHEDULER UNAVAILABLE");
        return;
    }
    const ULONGLONG scheduledAt = GetTickCount64();
    g_scheduledAttempt = state.attempt.load();
    g_scheduledAt = scheduledAt;
    g_scheduledDelay = delay;
    g_scheduledDue = scheduledAt + delay;
    g_scheduleArmed = true;
    state.clipRestorePending = true;
    SetEvent(g_scheduleEvent);
    Logger::Instance().Write("CLIPCURSOR/RECOVERY",
        "Attempt %lu armed; scheduler=WORKER scheduled-at=%llu delay=%u due=%llu "
        "display-confirmed=%s",
        state.attempt.load(), scheduledAt, delay, scheduledAt + delay,
        displayConfirmed ? "YES" : "NO");
}

struct RevalidationContext {
    unsigned long attempt;
    RECT expected;
};

DWORD WINAPI RevalidationThread(void* parameter) {
    const RevalidationContext context = *static_cast<RevalidationContext*>(parameter);
    delete static_cast<RevalidationContext*>(parameter);
    const UINT checkpoints[] = {500, 2000, g_revalidationWindowMs};
    UINT previous = 0;
    UINT reapplied = 0;
    for (const UINT checkpoint : checkpoints) {
        if (checkpoint <= previous || checkpoint > g_revalidationWindowMs) continue;
        Sleep(checkpoint - previous);
        previous = checkpoint;
        std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
        auto& state = State();
        if (context.attempt != state.attempt.load() || !state.recovering ||
            state.deactivationState != DeactivationState::FocusTransitionConfirmed) {
            Logger::Instance().Write("CLIPCURSOR/REVALIDATION",
                "Attempt %lu cancelled as stale at %u ms", context.attempt, checkpoint);
            return 0;
        }
        const HWND game = state.gameWindow.load();
        if (!game || GetForegroundWindow() != game || WindowThreadFocus(game) != game ||
            IsIconic(game) || !IsWindowVisible(game)) {
            Logger::Instance().Write("CLIPCURSOR/REVALIDATION",
                "Attempt %lu validation stopped at %u ms; window-state-invalid",
                context.attempt, checkpoint);
            state.recovering = false;
            return 0;
        }
        RECT before{}, after{};
        const BOOL queriedBefore = GetClipCursor(&before);
        const bool diverged = !queriedBefore || !RectsEqual(before, context.expected);
        BOOL applied = TRUE;
        bool didReapply = false;
        if (diverged && reapplied < g_maxClipReapplications && g_clipCursor) {
            applied = g_clipCursor(&context.expected);
            ++reapplied;
            didReapply = true;
        }
        const BOOL queriedAfter = GetClipCursor(&after);
        const bool restored = queriedAfter && RectsEqual(after, context.expected);
        if (diverged && restored) state.clipAppliedByDiagnostic = true;
        Logger::Instance().Write("CLIPCURSOR/REVALIDATION",
            "Attempt %lu checkpoint=%u before=(%ld,%ld)-(%ld,%ld) "
            "after=(%ld,%ld)-(%ld,%ld) diverged=%s reapplied=%s count=%u/%u "
            "call=%s restored=%s",
            context.attempt, checkpoint, before.left, before.top, before.right, before.bottom,
            after.left, after.top, after.right, after.bottom, diverged ? "YES" : "NO",
            didReapply ? "YES" : "NO", reapplied,
            g_maxClipReapplications, applied ? "TRUE" : "FALSE", restored ? "YES" : "NO");
    }
    std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
    if (context.attempt == State().attempt.load()) {
        State().recovering = false;
        Logger::Instance().Write("CLIPCURSOR/REVALIDATION",
            "Attempt %lu observation complete after %u ms", context.attempt,
            g_revalidationWindowMs);
    }
    return 0;
}

void ApplyCursorRecovery(unsigned long scheduledAttempt, ULONGLONG scheduledAt,
                         UINT scheduledDelay, ULONGLONG startedAt) {
    std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
    auto& state = State();
    if (scheduledAttempt != state.attempt.load() || !state.clipRestorePending) {
        Logger::Instance().Write("CLIPCURSOR/RECOVERY",
            "Discarded stale schedule attempt=%lu current-attempt=%lu",
            scheduledAttempt, state.attempt.load());
        return;
    }
    state.clipRestorePending = false;
    const unsigned long attempt = state.attempt.load();
    if (!state.recovering || !state.restoreClipExpected ||
        state.deactivationState != DeactivationState::FocusTransitionConfirmed ||
        state.clipRestoredAttempt.load() == attempt || !g_clipCursor)
        return;

    const HWND game = state.gameWindow.load();
    const bool validWindow = game && IsWindow(game) && GetForegroundWindow() == game &&
                             WindowThreadFocus(game) == game &&
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
    state.nullClipPending = false;
    state.recovering = g_revalidateCursorClip && restored;
    const ULONGLONG actualDelay = startedAt - scheduledAt;
    const LONGLONG lateness = static_cast<LONGLONG>(actualDelay) - scheduledDelay;
    Logger::Instance().Write("CLIPCURSOR/RECOVERY",
        "Attempt %lu expected=(%ld,%ld)-(%ld,%ld) before=(%ld,%ld)-(%ld,%ld) "
        "after=(%ld,%ld)-(%ld,%ld) needed=%s call=%s query=%s restored=%s "
        "scheduled-delay=%u actual-delay=%llu timer-lateness=%lld elapsed=%llu ms",
        attempt, expected.left, expected.top, expected.right, expected.bottom,
        before.left, before.top, before.right, before.bottom,
        after.left, after.top, after.right, after.bottom, needed ? "YES" : "NO",
        applied ? "TRUE" : "FALSE", queriedAfter ? "OK" : "FAILED",
        restored ? "YES" : "NO", scheduledDelay, actualDelay, lateness,
        state.focusReturnedAt ? GetTickCount64() - state.focusReturnedAt.load() : 0);
    WriteSnapshot(restored ? "CLIP RESTORATION SUCCESS" : "CLIP RESTORATION FAILURE");
    if (state.recovering) {
        auto* context = new RevalidationContext{attempt, expected};
        HANDLE thread = CreateThread(nullptr, 0, RevalidationThread, context, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            delete context;
            state.recovering = false;
            Logger::Instance().Write("CLIPCURSOR/REVALIDATION",
                "Attempt %lu could not start observation thread", attempt);
        }
    }
}

DWORD WINAPI RecoverySchedulerThread(void*) {
    for (;;) {
        DWORD timeout = INFINITE;
        if (g_scheduleArmed.load(std::memory_order_acquire)) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG due = g_scheduledDue.load(std::memory_order_acquire);
            timeout = due <= now ? 0 : static_cast<DWORD>(due - now);
        }
        const DWORD wait = WaitForSingleObject(g_scheduleEvent, timeout);
        if (wait != WAIT_TIMEOUT) continue;
        std::lock_guard<std::recursive_mutex> lock(g_recoveryMutex);
        const ULONGLONG startedAt = GetTickCount64();
        if (!g_scheduleArmed || g_scheduledDue.load() > startedAt) continue;
        const unsigned long attempt = g_scheduledAttempt.load();
        const ULONGLONG scheduledAt = g_scheduledAt.load();
        const UINT delay = g_scheduledDelay.load();
        g_scheduleArmed = false;
        ApplyCursorRecovery(attempt, scheduledAt, delay, startedAt);
    }
}

LRESULT CALLBACK HookWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            Logger::Instance().Write("FOCUS", "WM_ACTIVATE -> %u", LOWORD(wParam));
            if (LOWORD(wParam) == WA_INACTIVE) {
                CancelCursorRecovery(true);
                BeginFocusLoss(message);
            } else {
                RecordFocusReturn(message);
                ArmCursorRecovery(State().displayConfirmed);
            }
            break;
        case WM_ACTIVATEAPP:
            Logger::Instance().Write("FOCUS", "WM_ACTIVATEAPP -> %s", wParam ? "TRUE" : "FALSE");
            if (wParam) {
                RecordFocusReturn(message);
                ArmCursorRecovery(State().displayConfirmed);
            } else {
                CancelCursorRecovery(true);
                BeginFocusLoss(message);
            }
            break;
        case WM_SETFOCUS:
            Logger::Instance().Write("FOCUS", "WM_SETFOCUS");
            RecordFocusReturn(message);
            ArmCursorRecovery(State().displayConfirmed);
            break;
        case WM_KILLFOCUS:
            Logger::Instance().Write("FOCUS", "WM_KILLFOCUS");
            CancelCursorRecovery(true);
            BeginFocusLoss(message);
            break;
        case WM_SIZE:
            Logger::Instance().Write("WINDOW", "WM_SIZE -> %s (%ux%u)",
                WindowSizeStateName(wParam), LOWORD(lParam), HIWORD(lParam));
            break;
        case WM_DISPLAYCHANGE: {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            RecordDisplayChange(width, height);
            Logger::Instance().Write("DISPLAY", "WM_DISPLAYCHANGE -> %ux%u", width, height);
            if (State().displayConfirmed) ArmCursorRecovery(true);
            break;
        }
        case WM_SETCURSOR: {
            POINT position{};
            GetCursorPos(&position);
            Logger::Instance().Write("CURSOR/WM_SETCURSOR",
                "hit-test=%u message=0x%04X cursor=%p position=(%ld,%ld) attempt=%lu",
                LOWORD(lParam), HIWORD(lParam), GetCursor(), position.x, position.y,
                State().attempt.load());
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_F10) Logger::Instance().Write("MARKER", "========== USER MARKER ==========");
            if (wParam == VK_F11) WriteSnapshot("MANUAL SNAPSHOT");
            break;
        case WM_CLOSE:
            Logger::Instance().Write("WINDOW", "WM_CLOSE");
            CancelCursorRecovery(true);
            RecordCloseRequested(message);
            break;
        case WM_QUERYENDSESSION:
            Logger::Instance().Write("WINDOW", "WM_QUERYENDSESSION");
            CancelCursorRecovery(true);
            RecordCloseRequested(message);
            break;
        case WM_ENDSESSION:
            Logger::Instance().Write("WINDOW", "WM_ENDSESSION -> %s", wParam ? "TRUE" : "FALSE");
            if (wParam) {
                CancelCursorRecovery(true);
                RecordShutdown(message);
            } else RecordCloseCancelled();
            break;
        case WM_DESTROY:
            Logger::Instance().Write("WINDOW", "WM_DESTROY");
            CancelCursorRecovery(true);
            RecordShutdown(message);
            break;
        case WM_NCDESTROY:
            Logger::Instance().Write("WINDOW", "WM_NCDESTROY");
            CancelCursorRecovery(true);
            RecordShutdown(message);
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
    g_cursorTelemetry = settings.cursorTelemetry;
    g_revalidateCursorClip = settings.revalidateCursorClip;
    g_revalidationWindowMs = settings.revalidationWindowMs;
    g_maxClipReapplications = settings.maxClipReapplications;
    SetDirectInputDiagnosticsEnabled(settings.directInputDiagnostics);

    const HMODULE executable = GetModuleHandleW(nullptr);
    void* original{};
    const bool clipHooked = PatchUser32Import(executable, "ClipCursor",
        reinterpret_cast<void*>(HookClipCursor), &original);
    g_clipCursor = reinterpret_cast<ClipCursorFn>(original);
    bool setPositionHooked = false;
    bool showCursorHooked = false;
    bool setCursorHooked = false;
    if (g_cursorTelemetry) {
        original = nullptr;
        setPositionHooked = PatchUser32Import(executable, "SetCursorPos",
            reinterpret_cast<void*>(HookSetCursorPos), &original);
        g_setCursorPos = reinterpret_cast<SetCursorPosFn>(original);
        original = nullptr;
        showCursorHooked = PatchUser32Import(executable, "ShowCursor",
            reinterpret_cast<void*>(HookShowCursor), &original);
        g_showCursor = reinterpret_cast<ShowCursorFn>(original);
        original = nullptr;
        setCursorHooked = PatchUser32Import(executable, "SetCursor",
            reinterpret_cast<void*>(HookSetCursor), &original);
        g_setCursor = reinterpret_cast<SetCursorFn>(original);
    }
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
        if (g_originalWndProc) {
            g_scheduleEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (g_scheduleEvent)
                g_schedulerThread = CreateThread(
                    nullptr, 0, RecoverySchedulerThread, nullptr, 0, nullptr);
        }
    }

    Logger::Instance().Write("DIAGNOSTIC",
        "ClipCursor-only mode: clip-hook=%s window-hook=%s scheduler=%s restore=%s delay=%u wait-display=%s",
        clipHooked ? "YES" : "NO", g_originalWndProc ? "YES" : "NO",
        g_schedulerThread ? "WORKER" : "UNAVAILABLE",
        g_restoreCursorClip ? "YES" : "NO", g_restoreCursorClipDelayMs,
        g_waitForDisplayChange ? "YES" : "NO");
    Logger::Instance().Write("DIAGNOSTIC",
        "Extended diagnostics: cursor=%s set-pos=%s show-cursor=%s set-cursor=%s "
        "dinput=%s revalidate=%s window=%u max-reapplications=%u",
        g_cursorTelemetry ? "YES" : "NO", setPositionHooked ? "YES" : "NO",
        showCursorHooked ? "YES" : "NO", setCursorHooked ? "YES" : "NO",
        settings.directInputDiagnostics ? "YES" : "NO",
        g_revalidateCursorClip ? "YES" : "NO", g_revalidationWindowMs,
        g_maxClipReapplications);
    if (!clipHooked)
        Logger::Instance().Write("DIAGNOSTIC", "ClipCursor import not found; restoration unavailable");
    if (!g_originalWndProc)
        Logger::Instance().Write("DIAGNOSTIC", "Game window hook unavailable; restoration unavailable");
    return clipHooked && g_originalWndProc && g_schedulerThread;
}

}  // namespace fd
