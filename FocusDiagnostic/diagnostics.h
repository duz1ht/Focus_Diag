#pragma once

#include <windows.h>
#include <atomic>

namespace fd {

enum class FailureArea {
    None, WindowActivation, D3DDeviceLost, D3DReset, RenderLoop,
    KeyboardAcquire, MouseAcquire, CursorState, Inconclusive
};

struct DiagnosticState {
    std::atomic<HWND> gameWindow{nullptr};
    std::atomic<bool> recovering{false};
    std::atomic<unsigned long> attempt{0};
    std::atomic<unsigned long long> frames{0};
    std::atomic<bool> d3dObserved{false};
    std::atomic<HRESULT> cooperativeLevel{S_OK};
    std::atomic<HRESULT> lastReset{S_OK};
    std::atomic<HRESULT> mouseAcquire{S_OK};
    std::atomic<HRESULT> keyboardAcquire{S_OK};
    std::atomic<bool> mouseObserved{false};
    std::atomic<bool> keyboardObserved{false};
    std::atomic<bool> cursorObserved{false};
    std::atomic<bool> cursorVisibilityObserved{false};
    std::atomic<bool> showCursorObserved{false};
    std::atomic<int> lastShowCursorResult{0};
    std::atomic<bool> clipActive{false};
    std::atomic<bool> restoreClipExpected{false};
    std::atomic<LONG> expectedClipLeft{0};
    std::atomic<LONG> expectedClipTop{0};
    std::atomic<LONG> expectedClipRight{0};
    std::atomic<LONG> expectedClipBottom{0};
    std::atomic<unsigned long> clipRestoredAttempt{0};
    std::atomic<bool> clipRestorePending{false};
    std::atomic<bool> clipAppliedByDiagnostic{false};
    std::atomic<UINT> displayWidth{0};
    std::atomic<UINT> displayHeight{0};
    std::atomic<UINT> expectedDisplayWidth{0};
    std::atomic<UINT> expectedDisplayHeight{0};
    std::atomic<bool> displayChangeAfterFocus{false};
    std::atomic<ULONGLONG> focusReturnedAt{0};
    std::atomic<ULONGLONG> lastPresentAt{0};
};

DiagnosticState& State();
void BeginFocusLoss();
void FocusReturned();
void RecordPresent(HRESULT result);
void RecordClipState(const RECT* requested, const RECT& actual, bool succeeded);
void RecordDisplayChange(UINT width, UINT height);
void WriteSnapshot(const char* reason);
void CheckRecoveryTimeout();
const char* FailureAreaName(FailureArea area);

}  // namespace fd
