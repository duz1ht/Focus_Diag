#include "diagnostics.h"

#include "logger.h"

namespace fd {

DiagnosticState& State() {
    static DiagnosticState state;
    return state;
}

RECT ExpectedClip() {
    const auto& state = State();
    return {state.expectedClipLeft.load(), state.expectedClipTop.load(),
            state.expectedClipRight.load(), state.expectedClipBottom.load()};
}

bool RectsEqual(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

HWND WindowThreadFocus(HWND window) {
    if (!window) return nullptr;
    GUITHREADINFO info{sizeof(info)};
    const DWORD thread = GetWindowThreadProcessId(window, nullptr);
    return thread && GetGUIThreadInfo(thread, &info) ? info.hwndFocus : nullptr;
}

const char* DeactivationStateName(DeactivationState state) {
    switch (state) {
        case DeactivationState::DeactivationPending: return "DEACTIVATION_PENDING";
        case DeactivationState::FocusTransitionConfirmed: return "FOCUS_TRANSITION_CONFIRMED";
        case DeactivationState::CloseRequested: return "CLOSE_REQUESTED";
        case DeactivationState::Shutdown: return "SHUTDOWN";
        case DeactivationState::IntentionalRelease: return "INTENTIONAL_RELEASE";
        default: return "NONE";
    }
}

const char* DeactivationEventName(UINT message) {
    switch (message) {
        case WM_ACTIVATE: return "WM_ACTIVATE";
        case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
        case WM_SETFOCUS: return "WM_SETFOCUS";
        case WM_KILLFOCUS: return "WM_KILLFOCUS";
        case WM_CLOSE: return "WM_CLOSE";
        case WM_QUERYENDSESSION: return "WM_QUERYENDSESSION";
        case WM_ENDSESSION: return "WM_ENDSESSION";
        case WM_DESTROY: return "WM_DESTROY";
        case WM_NCDESTROY: return "WM_NCDESTROY";
        default: return "NONE";
    }
}

void BeginFocusLoss(UINT message) {
    auto& state = State();
    const DeactivationState current = state.deactivationState.load();
    if (current == DeactivationState::CloseRequested ||
        current == DeactivationState::Shutdown)
        return;
    if (state.recovering && !state.focusReturned) return;
    state.recovering = true;

    state.restoreClipExpected = state.clipActive.load() || state.nullClipPending.load();
    state.expectedDisplayWidth = state.nullClipPending
        ? state.nullClipDisplayWidth.load() : state.displayWidth.load();
    state.expectedDisplayHeight = state.nullClipPending
        ? state.nullClipDisplayHeight.load() : state.displayHeight.load();
    state.focusReturned = false;
    state.displayConfirmed = false;
    state.clipRestorePending = false;
    state.focusReturnedAt = 0;
    state.deactivationState = DeactivationState::DeactivationPending;
    state.deactivationEvent = message;
    ++state.attempt;
    Logger::Instance().Write("RECOVERY",
        "Attempt %lu: deactivation pending; restore expected=%s",
        state.attempt.load(), state.restoreClipExpected ? "YES" : "NO");
}

void RecordFocusReturn(UINT message) {
    auto& state = State();
    const HWND game = state.gameWindow.load();
    const bool validReturn = game && IsWindow(game) && GetForegroundWindow() == game &&
        WindowThreadFocus(game) == game && IsWindowVisible(game) && !IsIconic(game);
    if (state.deactivationState == DeactivationState::CloseRequested && validReturn) {
        state.deactivationState = DeactivationState::None;
        state.deactivationEvent = 0;
        Logger::Instance().Write("RECOVERY", "CLOSE_REQUESTED cancelled; window remained active");
        return;
    }
    if (!state.recovering) return;
    if (state.deactivationState != DeactivationState::DeactivationPending) return;
    if (!validReturn) {
        Logger::Instance().Write("RECOVERY",
            "Attempt %lu: return not confirmed; game=%p valid=%s foreground=%p focus=%p "
            "visible=%s iconic=%s",
            state.attempt.load(), game, game && IsWindow(game) ? "YES" : "NO",
            GetForegroundWindow(), WindowThreadFocus(game),
            game && IsWindowVisible(game) ? "YES" : "NO",
            game && IsIconic(game) ? "YES" : "NO");
        return;
    }
    if (state.focusReturned.exchange(true)) return;
    state.deactivationState = DeactivationState::FocusTransitionConfirmed;
    state.deactivationEvent = message;
    state.displayConfirmed =
        state.displayWidth == state.expectedDisplayWidth &&
        state.displayHeight == state.expectedDisplayHeight;
    state.focusReturnedAt = GetTickCount64();
    Logger::Instance().Write("RECOVERY",
        "Attempt %lu: FOCUS_TRANSITION_CONFIRMED", state.attempt.load());
}

void RecordDisplayChange(UINT width, UINT height) {
    auto& state = State();
    state.displayWidth = width;
    state.displayHeight = height;
    if (state.recovering && state.focusReturned &&
        width == state.expectedDisplayWidth && height == state.expectedDisplayHeight)
        state.displayConfirmed = true;
}

void RecordClipState(const RECT* requested, const RECT& actual, bool succeeded) {
    if (!succeeded) return;
    auto& state = State();
    state.clipObserved = true;
    const bool hadActive = state.clipActive.load();
    const bool wasDllOwned = state.clipAppliedByDiagnostic.exchange(false);
    state.clipActive = requested != nullptr;
    if (requested) {
        state.nullClipPending = false;
        if (!state.recovering) {
            state.deactivationState = DeactivationState::None;
            state.deactivationEvent = 0;
        }
        state.expectedClipLeft = actual.left;
        state.expectedClipTop = actual.top;
        state.expectedClipRight = actual.right;
        state.expectedClipBottom = actual.bottom;
    } else {
        const HWND game = state.gameWindow.load();
        state.nullClipPending = hadActive || wasDllOwned;
        state.nullClipAt = GetTickCount64();
        state.nullClipForeground = GetForegroundWindow();
        state.nullClipFocus = WindowThreadFocus(game);
        state.nullClipIconic = game && IsIconic(game);
        state.nullClipVisible = game && IsWindowVisible(game);
        state.nullClipDisplayWidth = state.displayWidth.load();
        state.nullClipDisplayHeight = state.displayHeight.load();
        state.nullClipWasDllOwned = wasDllOwned;
        state.nullClipHadActive = hadActive;
        const DeactivationState transition = state.deactivationState.load();
        if (transition != DeactivationState::CloseRequested &&
            transition != DeactivationState::Shutdown && !state.recovering) {
            state.deactivationState = DeactivationState::IntentionalRelease;
            state.deactivationEvent = 0;
        }
        Logger::Instance().Write("CLIPCURSOR/NULL",
            "candidate=%s foreground=%p focus=%p iconic=%s visible=%s display=%ux%u "
            "had-active=%s dll-owned=%s",
            state.nullClipPending ? "YES" : "NO",
            state.nullClipForeground.load(), state.nullClipFocus.load(),
            state.nullClipIconic ? "YES" : "NO", state.nullClipVisible ? "YES" : "NO",
            state.nullClipDisplayWidth.load(), state.nullClipDisplayHeight.load(),
            hadActive ? "YES" : "NO",
            wasDllOwned ? "YES" : "NO");
    }
}

void RecordCloseRequested(UINT message) {
    auto& state = State();
    state.deactivationState = DeactivationState::CloseRequested;
    state.deactivationEvent = message;
    state.restoreClipExpected = false;
    state.clipRestorePending = false;
    Logger::Instance().Write("RECOVERY", "CLOSE_REQUESTED");
}

void RecordCloseCancelled() {
    auto& state = State();
    if (state.deactivationState != DeactivationState::CloseRequested) return;
    state.deactivationState = DeactivationState::None;
    state.deactivationEvent = 0;
    Logger::Instance().Write("RECOVERY", "CLOSE_REQUESTED cancelled");
}

void RecordShutdown(UINT message) {
    auto& state = State();
    state.deactivationState = DeactivationState::Shutdown;
    state.deactivationEvent = message;
    state.restoreClipExpected = false;
    state.clipRestorePending = false;
    state.recovering = false;
    Logger::Instance().Write("RECOVERY", "SHUTDOWN confirmed by %s",
                             DeactivationEventName(message));
}

void RecordDirectInputResult(bool mouse, bool acquire, HRESULT result) {
    if (!mouse) return;
    auto& state = State();
    if (acquire) {
        state.mouseAcquireResult = result;
        state.mouseAcquireAt = GetTickCount64();
    } else {
        state.mouseReadResult = result;
        state.mouseReadAt = GetTickCount64();
    }
}

void WriteSnapshot(const char* reason) {
    const auto& state = State();
    const HWND game = state.gameWindow.load();
    RECT current{};
    const BOOL queried = GetClipCursor(&current);
    const RECT expected = ExpectedClip();
    const bool restored = queried && state.restoreClipExpected && RectsEqual(current, expected);

    Logger::Instance().Write("SNAPSHOT", "========== %s ==========", reason);
    Logger::Instance().Write("SNAPSHOT",
        "attempt=%lu recovering=%s focus-returned=%s display-confirmed=%s pending=%s",
        state.attempt.load(), state.recovering ? "YES" : "NO",
        state.focusReturned ? "YES" : "NO", state.displayConfirmed ? "YES" : "NO",
        state.clipRestorePending ? "YES" : "NO");
    Logger::Instance().Write("SNAPSHOT",
        "game=%p valid=%s foreground=%p focus=%p iconic=%s visible=%s",
        game, game && IsWindow(game) ? "YES" : "NO",
        GetForegroundWindow(), WindowThreadFocus(game), game && IsIconic(game) ? "YES" : "NO",
        game && IsWindowVisible(game) ? "YES" : "NO");
    Logger::Instance().Write("SNAPSHOT",
        "clip-observed=%s restore-expected=%s applied-by-dll=%s restored=%s query=%s",
        state.clipObserved ? "YES" : "NO", state.restoreClipExpected ? "YES" : "NO",
        state.clipAppliedByDiagnostic ? "YES" : "NO", restored ? "YES" : "NO",
        queried ? "OK" : "FAILED");
    Logger::Instance().Write("SNAPSHOT",
        "expected=(%ld,%ld)-(%ld,%ld) actual=(%ld,%ld)-(%ld,%ld)",
        expected.left, expected.top, expected.right, expected.bottom,
        current.left, current.top, current.right, current.bottom);
    POINT cursor{};
    GetCursorPos(&cursor);
    Logger::Instance().Write("SNAPSHOT",
        "cursor=%p position=(%ld,%ld) last-set-position=(%ld,%ld) last-set-at=%llu "
        "show-cursor-result=%d",
        GetCursor(), cursor.x, cursor.y, state.lastCursorX.load(), state.lastCursorY.load(),
        state.lastCursorPositionAt.load(), state.showCursorResult.load());
    Logger::Instance().Write("SNAPSHOT",
        "dinput-mouse acquire=0x%08lX acquire-at=%llu read=0x%08lX read-at=%llu",
        static_cast<unsigned long>(state.mouseAcquireResult.load()), state.mouseAcquireAt.load(),
        static_cast<unsigned long>(state.mouseReadResult.load()), state.mouseReadAt.load());
    Logger::Instance().Write("SNAPSHOT",
        "deactivation-state=%s deactivation-event=%s null-pending=%s null-at=%llu null-foreground=%p "
        "null-focus=%p null-iconic=%s null-visible=%s null-display=%ux%u "
        "null-had-active=%s null-dll-owned=%s",
        DeactivationStateName(state.deactivationState.load()),
        DeactivationEventName(state.deactivationEvent.load()),
        state.nullClipPending ? "YES" : "NO", state.nullClipAt.load(),
        state.nullClipForeground.load(), state.nullClipFocus.load(),
        state.nullClipIconic ? "YES" : "NO", state.nullClipVisible ? "YES" : "NO",
        state.nullClipDisplayWidth.load(), state.nullClipDisplayHeight.load(),
        state.nullClipHadActive ? "YES" : "NO",
        state.nullClipWasDllOwned ? "YES" : "NO");
}

}  // namespace fd
