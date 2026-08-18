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

void BeginFocusLoss() {
    auto& state = State();
    if (state.recovering && !state.focusReturned) return;
    state.recovering = true;

    state.restoreClipExpected = state.clipActive.load();
    state.expectedDisplayWidth = state.displayWidth.load();
    state.expectedDisplayHeight = state.displayHeight.load();
    state.focusReturned = false;
    state.displayConfirmed = false;
    state.clipRestorePending = false;
    state.focusReturnedAt = 0;
    ++state.attempt;
    Logger::Instance().Write("RECOVERY", "Attempt %lu: focus lost; restore expected=%s",
        state.attempt.load(), state.restoreClipExpected ? "YES" : "NO");
}

void RecordFocusReturn() {
    auto& state = State();
    if (!state.recovering) return;
    if (state.focusReturned.exchange(true)) return;
    state.focusReturnedAt = GetTickCount64();
    Logger::Instance().Write("RECOVERY", "Attempt %lu: focus returned", state.attempt.load());
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
    // A successful game call supersedes any clip previously owned by recovery.
    state.clipAppliedByDiagnostic = false;
    state.clipActive = requested != nullptr;
    if (requested) {
        state.expectedClipLeft = actual.left;
        state.expectedClipTop = actual.top;
        state.expectedClipRight = actual.right;
        state.expectedClipBottom = actual.bottom;
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
        "game=%p foreground=%p focus=%p iconic=%s visible=%s",
        game, GetForegroundWindow(), GetFocus(), game && IsIconic(game) ? "YES" : "NO",
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
}

}  // namespace fd
