#pragma once

#include <atomic>
#include <windows.h>

namespace fd {

enum class DeactivationState : int {
    None, DeactivationPending, FocusTransitionConfirmed,
    CloseRequested, Shutdown, IntentionalRelease
};

struct DiagnosticState {
    std::atomic<HWND> gameWindow{nullptr};
    std::atomic<bool> recovering{false};
    std::atomic<unsigned long> attempt{0};
    std::atomic<bool> clipObserved{false};
    std::atomic<bool> clipActive{false};
    std::atomic<bool> restoreClipExpected{false};
    std::atomic<LONG> expectedClipLeft{0};
    std::atomic<LONG> expectedClipTop{0};
    std::atomic<LONG> expectedClipRight{0};
    std::atomic<LONG> expectedClipBottom{0};
    std::atomic<UINT> displayWidth{0};
    std::atomic<UINT> displayHeight{0};
    std::atomic<UINT> expectedDisplayWidth{0};
    std::atomic<UINT> expectedDisplayHeight{0};
    std::atomic<bool> focusReturned{false};
    std::atomic<bool> displayConfirmed{false};
    std::atomic<bool> clipRestorePending{false};
    std::atomic<bool> clipAppliedByDiagnostic{false};
    std::atomic<unsigned long> clipRestoredAttempt{0};
    std::atomic<ULONGLONG> focusReturnedAt{0};
    std::atomic<bool> nullClipPending{false};
    std::atomic<ULONGLONG> nullClipAt{0};
    std::atomic<HWND> nullClipForeground{nullptr};
    std::atomic<HWND> nullClipFocus{nullptr};
    std::atomic<bool> nullClipIconic{false};
    std::atomic<bool> nullClipVisible{false};
    std::atomic<UINT> nullClipDisplayWidth{0};
    std::atomic<UINT> nullClipDisplayHeight{0};
    std::atomic<bool> nullClipWasDllOwned{false};
    std::atomic<bool> nullClipHadActive{false};
    std::atomic<int> showCursorResult{0};
    std::atomic<HCURSOR> lastCursor{nullptr};
    std::atomic<LONG> lastCursorX{0};
    std::atomic<LONG> lastCursorY{0};
    std::atomic<ULONGLONG> lastCursorPositionAt{0};
    std::atomic<HRESULT> mouseAcquireResult{S_OK};
    std::atomic<HRESULT> mouseReadResult{S_OK};
    std::atomic<ULONGLONG> mouseAcquireAt{0};
    std::atomic<ULONGLONG> mouseReadAt{0};
    std::atomic<DeactivationState> deactivationState{DeactivationState::None};
    std::atomic<UINT> deactivationEvent{0};
};

DiagnosticState& State();
RECT ExpectedClip();
bool RectsEqual(const RECT& left, const RECT& right);
void BeginFocusLoss(UINT message);
void RecordFocusReturn(UINT message);
void RecordDisplayChange(UINT width, UINT height);
void RecordClipState(const RECT* requested, const RECT& actual, bool succeeded);
void RecordCloseRequested(UINT message);
void RecordCloseCancelled();
void RecordShutdown(UINT message);
void WriteSnapshot(const char* reason);
HWND WindowThreadFocus(HWND window);
void RecordDirectInputResult(bool mouse, bool acquire, HRESULT result);
const char* DeactivationStateName(DeactivationState state);
const char* DeactivationEventName(UINT message);

}  // namespace fd
