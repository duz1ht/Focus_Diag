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

const char* NullClipClassificationName(NullClipClassification classification) {
    switch (classification) {
        case NullClipClassification::Pending: return "PENDING";
        case NullClipClassification::FocusTransition: return "FOCUS_TRANSITION";
        case NullClipClassification::IntentionalRelease: return "INTENTIONAL_RELEASE";
        default: return "NONE";
    }
}

namespace {
constexpr ULONGLONG kNullFocusTransitionWindowMs = 5000;

void ClassifyPendingNullForFocusLoss() {
    auto& state = State();
    if (!state.nullClipPending ||
        state.nullClipClassification == NullClipClassification::FocusTransition)
        return;

    const HWND game = state.gameWindow.load();
    const HWND foreground = GetForegroundWindow();
    const HWND focus = WindowThreadFocus(game);
    const bool displayChanged =
        state.displayWidth != state.nullClipDisplayWidth ||
        state.displayHeight != state.nullClipDisplayHeight;
    const ULONGLONG age = GetTickCount64() - state.nullClipAt.load();
    const bool transition = age <= kNullFocusTransitionWindowMs &&
        (foreground != game || focus != game || displayChanged);
    if (!transition) return;

    state.nullClipClassification = NullClipClassification::FocusTransition;
    state.restoreClipExpected = true;
    state.expectedDisplayWidth = state.nullClipDisplayWidth.load();
    state.expectedDisplayHeight = state.nullClipDisplayHeight.load();
    Logger::Instance().Write("CLIPCURSOR/NULL",
        "classified=FOCUS_TRANSITION foreground=%p focus=%p display=%ux%u "
        "null-foreground=%p null-focus=%p null-display=%ux%u age=%llu ms dll-owned=%s",
        foreground, focus, state.displayWidth.load(), state.displayHeight.load(),
        state.nullClipForeground.load(), state.nullClipFocus.load(),
        state.nullClipDisplayWidth.load(), state.nullClipDisplayHeight.load(),
        age,
        state.nullClipWasDllOwned ? "YES" : "NO");
}
}  // namespace

void BeginFocusLoss() {
    auto& state = State();
    if (state.recovering && !state.focusReturned) {
        ClassifyPendingNullForFocusLoss();
        return;
    }
    state.recovering = true;

    state.restoreClipExpected = state.clipActive.load();
    state.expectedDisplayWidth = state.displayWidth.load();
    state.expectedDisplayHeight = state.displayHeight.load();
    state.focusReturned = false;
    state.displayConfirmed = false;
    state.clipRestorePending = false;
    state.focusReturnedAt = 0;
    ++state.attempt;
    if (state.nullClipPending) {
        state.nullClipClassification = NullClipClassification::Pending;
        ClassifyPendingNullForFocusLoss();
    }
    Logger::Instance().Write("RECOVERY", "Attempt %lu: focus lost; restore expected=%s",
        state.attempt.load(), state.restoreClipExpected ? "YES" : "NO");
}

void RecordFocusReturn() {
    auto& state = State();
    if (!state.recovering) return;
    if (state.focusReturned.exchange(true)) return;
    ClassifyPendingNullForFocusLoss();
    if (state.nullClipPending &&
        state.nullClipClassification == NullClipClassification::Pending) {
        state.nullClipClassification = NullClipClassification::IntentionalRelease;
        state.nullClipPending = false;
        state.restoreClipExpected = false;
        Logger::Instance().Write("CLIPCURSOR/NULL",
            "classified=INTENTIONAL_RELEASE foreground-at-null=%p focus-at-null=%p "
            "display-at-null=%ux%u age=%llu ms dll-owned=%s",
            state.nullClipForeground.load(), state.nullClipFocus.load(),
            state.nullClipDisplayWidth.load(), state.nullClipDisplayHeight.load(),
            GetTickCount64() - state.nullClipAt.load(),
            state.nullClipWasDllOwned ? "YES" : "NO");
    }
    state.focusReturnedAt = GetTickCount64();
    Logger::Instance().Write("RECOVERY", "Attempt %lu: focus returned", state.attempt.load());
}

void RecordDisplayChange(UINT width, UINT height) {
    auto& state = State();
    state.displayWidth = width;
    state.displayHeight = height;
    if (state.recovering) ClassifyPendingNullForFocusLoss();
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
        state.nullClipClassification = NullClipClassification::None;
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
        state.nullClipClassification = state.nullClipPending
            ? NullClipClassification::Pending : NullClipClassification::IntentionalRelease;
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
        game, GetForegroundWindow(), WindowThreadFocus(game), game && IsIconic(game) ? "YES" : "NO",
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
    Logger::Instance().Write("SNAPSHOT",
        "null-pending=%s null-classification=%s null-at=%llu null-foreground=%p "
        "null-focus=%p null-iconic=%s null-visible=%s null-display=%ux%u "
        "null-had-active=%s null-dll-owned=%s",
        state.nullClipPending ? "YES" : "NO",
        NullClipClassificationName(state.nullClipClassification.load()), state.nullClipAt.load(),
        state.nullClipForeground.load(), state.nullClipFocus.load(),
        state.nullClipIconic ? "YES" : "NO", state.nullClipVisible ? "YES" : "NO",
        state.nullClipDisplayWidth.load(), state.nullClipDisplayHeight.load(),
        state.nullClipHadActive ? "YES" : "NO",
        state.nullClipWasDllOwned ? "YES" : "NO");
}

}  // namespace fd
