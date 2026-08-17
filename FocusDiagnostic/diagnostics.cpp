#include "diagnostics.h"

#include "legacy_dx8.h"
#include "logger.h"

namespace fd {

namespace {
bool RectsEqual(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

RECT ExpectedClip(const DiagnosticState& state) {
    return {state.expectedClipLeft.load(), state.expectedClipTop.load(),
            state.expectedClipRight.load(), state.expectedClipBottom.load()};
}
}  // namespace

DiagnosticState& State() {
    static DiagnosticState state;
    return state;
}

const char* FailureAreaName(FailureArea area) {
    switch (area) {
        case FailureArea::None: return "NONE";
        case FailureArea::WindowActivation: return "WINDOW_ACTIVATION";
        case FailureArea::D3DDeviceLost: return "D3D_DEVICE_LOST";
        case FailureArea::D3DReset: return "D3D_RESET";
        case FailureArea::RenderLoop: return "RENDER_LOOP";
        case FailureArea::KeyboardAcquire: return "KEYBOARD_ACQUIRE";
        case FailureArea::MouseAcquire: return "MOUSE_ACQUIRE";
        case FailureArea::CursorState: return "CURSOR_STATE";
        default: return "INCONCLUSIVE";
    }
}

void BeginFocusLoss() {
    auto& s = State();
    if (!s.recovering.exchange(true)) {
        s.restoreClipExpected = s.clipActive.load();
        s.expectedDisplayWidth = s.displayWidth.load();
        s.expectedDisplayHeight = s.displayHeight.load();
        s.displayChangeAfterFocus = false;
        s.clipRestorePending = false;
        ++s.attempt;
        s.focusReturnedAt = 0;
        Logger::Instance().Write("RECOVERY", "Attempt %lu: focus lost", s.attempt.load());
    }
}

void RecordDisplayChange(UINT width, UINT height) {
    auto& s = State();
    s.displayWidth = width;
    s.displayHeight = height;
    if (s.recovering && s.focusReturnedAt) s.displayChangeAfterFocus = true;
}

void RecordClipState(const RECT* requested, const RECT& actual, bool succeeded) {
    if (!succeeded) return;
    auto& s = State();
    s.cursorObserved = true;
    s.clipActive = requested != nullptr;
    if (requested) {
        s.expectedClipLeft = actual.left;
        s.expectedClipTop = actual.top;
        s.expectedClipRight = actual.right;
        s.expectedClipBottom = actual.bottom;
    }
}

void FocusReturned() {
    auto& s = State();
    if (!s.recovering) return;
    s.focusReturnedAt = GetTickCount64();
    Logger::Instance().Write("RECOVERY", "Attempt %lu: focus returned", s.attempt.load());
}

void RecordPresent(HRESULT result) {
    auto& s = State();
    if (SUCCEEDED(result)) {
        ++s.frames;
        s.lastPresentAt = GetTickCount64();
        if (s.recovering && s.focusReturnedAt) {
            s.recovering = false;
            Logger::Instance().Write("RECOVERY", "Attempt %lu SUCCESS; frame=%llu",
                                     s.attempt.load(), s.frames.load());
        }
    }
}

static FailureArea Diagnose() {
    auto& s = State();
    HWND game = s.gameWindow;
    if (!game || (GetForegroundWindow() != game && GetFocus() != game))
        return FailureArea::WindowActivation;
    if (s.d3dObserved && s.cooperativeLevel == kD3DErrDeviceLost) return FailureArea::D3DDeviceLost;
    if (s.d3dObserved && FAILED(s.lastReset)) return FailureArea::D3DReset;
    if (s.keyboardObserved && FAILED(s.keyboardAcquire)) return FailureArea::KeyboardAcquire;
    if (s.mouseObserved && FAILED(s.mouseAcquire)) return FailureArea::MouseAcquire;
    RECT client{}, clip{};
    if (s.cursorObserved && s.restoreClipExpected && GetClipCursor(&clip) &&
        !RectsEqual(clip, ExpectedClip(s)))
        return FailureArea::CursorState;
    if (game && GetClientRect(game, &client) && GetClipCursor(&clip)) {
        POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
        if (ClientToScreen(game, &center) && !PtInRect(&clip, center))
            return FailureArea::CursorState;
    }
    if (s.d3dObserved && s.focusReturnedAt && s.lastPresentAt < s.focusReturnedAt)
        return FailureArea::RenderLoop;
    return FailureArea::Inconclusive;
}

void WriteSnapshot(const char* reason) {
    auto& s = State();
    HWND game = s.gameWindow;
    RECT window{}, client{}, clip{};
    if (game) {
        GetWindowRect(game, &window);
        GetClientRect(game, &client);
    }
    GetClipCursor(&clip);
    const RECT expectedClip = ExpectedClip(s);
    const bool clipRestored = s.restoreClipExpected && RectsEqual(clip, expectedClip);
    const FailureArea failure = Diagnose();
    Logger::Instance().Write("SNAPSHOT", "========== %s ==========", reason);
    Logger::Instance().Write("SNAPSHOT", "attempt=%lu frame=%llu recovering=%s",
        s.attempt.load(), s.frames.load(), s.recovering ? "YES" : "NO");
    Logger::Instance().Write("SNAPSHOT", "game=%p foreground=%p focus=%p iconic=%s visible=%s",
        game, GetForegroundWindow(), GetFocus(), game && IsIconic(game) ? "YES" : "NO",
        game && IsWindowVisible(game) ? "YES" : "NO");
    Logger::Instance().Write("SNAPSHOT", "window=(%ld,%ld)-(%ld,%ld) client=%ldx%ld clip=(%ld,%ld)-(%ld,%ld)",
        window.left, window.top, window.right, window.bottom, client.right, client.bottom,
        clip.left, clip.top, clip.right, clip.bottom);
    const HRESULT cooperative = s.cooperativeLevel.load();
    const HRESULT reset = s.lastReset.load();
    const HRESULT keyboard = s.keyboardAcquire.load();
    const HRESULT mouse = s.mouseAcquire.load();
    Logger::Instance().Write("SNAPSHOT", "cooperative=0x%08lX (%s) reset=0x%08lX (%s)",
        cooperative, HResultName(cooperative), reset, HResultName(reset));
    Logger::Instance().Write("SNAPSHOT", "d3d monitoring=%s",
                             s.d3dObserved ? "ACTIVE" : "NOT ACTIVE");
    Logger::Instance().Write("SNAPSHOT", "keyboard=%s mouse=%s",
        s.keyboardObserved ? HResultName(keyboard) : "NOT OBSERVED",
        s.mouseObserved ? HResultName(mouse) : "NOT OBSERVED");
    Logger::Instance().Write("SNAPSHOT", "cursor observed=%s clip expected=%s clip active=%s clip restored=%s expected=(%ld,%ld)-(%ld,%ld) actual=(%ld,%ld)-(%ld,%ld)",
        s.cursorObserved ? "YES" : "NO", s.restoreClipExpected ? "YES" : "NO",
        s.clipActive ? "YES" : "NO", clipRestored ? "YES" : "NO",
        expectedClip.left, expectedClip.top, expectedClip.right, expectedClip.bottom,
        clip.left, clip.top, clip.right, clip.bottom);
    const bool showCursorObserved = s.showCursorObserved.load();
    Logger::Instance().Write("SNAPSHOT", "cursor visibility observed=%s ShowCursor observed=%s last result=%d (%s)",
        s.cursorVisibilityObserved ? "YES" : "NO", showCursorObserved ? "YES" : "NO",
        s.lastShowCursorResult.load(), !showCursorObserved ? "UNKNOWN" :
        (s.lastShowCursorResult.load() >= 0 ? "VISIBLE" : "HIDDEN"));
    Logger::Instance().Write("SNAPSHOT", "LIKELY FAILURE AREA: %s", FailureAreaName(failure));
}

void CheckRecoveryTimeout() {
    auto& s = State();
    const ULONGLONG returned = s.focusReturnedAt;
    if (s.recovering && returned && GetTickCount64() - returned >= 5000) {
        if (!s.d3dObserved) {
            const FailureArea observedFailure = Diagnose();
            if (observedFailure != FailureArea::Inconclusive) {
                Logger::Instance().Write("RECOVERY", "Attempt %lu FAILURE; first divergence=%s",
                                         s.attempt.load(), FailureAreaName(observedFailure));
                WriteSnapshot("RECOVERY FAILURE - D3D NOT MONITORED");
                s.recovering = false;
                return;
            }
            Logger::Instance().Write("RECOVERY",
                "Attempt %lu PARTIAL; window recovered; D3D device methods not monitored",
                s.attempt.load());
            WriteSnapshot("WINDOW RECOVERED - D3D NOT MONITORED");
            s.recovering = false;
            return;
        }
        const FailureArea failure = Diagnose();
        Logger::Instance().Write("RECOVERY", "Attempt %lu FAILURE; first divergence=%s",
                                 s.attempt.load(), FailureAreaName(failure));
        WriteSnapshot("RECOVERY TIMEOUT");
        s.recovering = false;
    }
}

}  // namespace fd
