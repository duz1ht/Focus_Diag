#pragma once

#include <atomic>
#include <windows.h>

namespace fd {

enum class NullClipClassification : int {
    None, Pending, FocusTransition, IntentionalRelease
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
    std::atomic<NullClipClassification> nullClipClassification{NullClipClassification::None};
};

DiagnosticState& State();
RECT ExpectedClip();
bool RectsEqual(const RECT& left, const RECT& right);
void BeginFocusLoss();
void RecordFocusReturn();
void RecordDisplayChange(UINT width, UINT height);
void RecordClipState(const RECT* requested, const RECT& actual, bool succeeded);
void WriteSnapshot(const char* reason);
HWND WindowThreadFocus(HWND window);
const char* NullClipClassificationName(NullClipClassification classification);

}  // namespace fd
