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
    std::atomic<bool> clipActive{false};
    std::atomic<bool> restoreClipExpected{false};
    std::atomic<LONG> expectedClipLeft{0};
    std::atomic<LONG> expectedClipTop{0};
    std::atomic<LONG> expectedClipRight{0};
    std::atomic<LONG> expectedClipBottom{0};
    std::atomic<ULONGLONG> focusReturnedAt{0};
    std::atomic<ULONGLONG> lastPresentAt{0};
};

DiagnosticState& State();
void BeginFocusLoss();
void FocusReturned();
void RecordPresent(HRESULT result);
void RecordClipState(const RECT* requested, const RECT& actual, bool succeeded);
void WriteSnapshot(const char* reason);
void CheckRecoveryTimeout();
const char* FailureAreaName(FailureArea area);

}  // namespace fd
