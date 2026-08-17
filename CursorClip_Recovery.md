# Alt-Tab Cursor Clipping Recovery

## Classification

This is a **focus-recovery-related fix**, not a window-focus fix. The game regains foreground status and keyboard focus, so the solution must not call `SetFocus`, `SetForegroundWindow`, or `SetActiveWindow`. It restores the cursor confinement state associated with activation.

## Problem and Cause

After Alt-Tab, Windows releases the fullscreen game's `ClipCursor` rectangle. The game regains focus and fullscreen mode but does not reapply its previous rectangle, so it behaves as if it were still inactive. Restoring that rectangle returns the game to its normal state.

## Required State

Keep the following state, using atomics or equivalent synchronization if hooks and recovery events can run on different threads:

- game window handle;
- whether focus recovery is in progress;
- a monotonically increasing recovery-attempt identifier;
- whether a clip was active when focus was lost;
- the last successfully applied non-null clip rectangle;
- display dimensions captured before focus loss;
- focus-return timestamp;
- pending-timer and restored-attempt state;
- whether the recovery code, rather than the game, applied the current clip.

## Complete Recovery Flow

### 1. Record the game's clip

`HookClipCursor` must call the original `ClipCursor`, query the resulting rectangle with `GetClipCursor`, and update the saved state only if the call succeeds. A non-null request marks clipping as active and stores the actual applied rectangle; a null request marks it inactive.

Internal recovery calls must invoke the saved original `ClipCursor` function directly, bypassing `HookClipCursor`. This prevents restoration or release operations from being recorded as new requests from the game. A reentrancy guard is still required around the hook.

### 2. Capture focus loss

In `HookWndProc`, treat the following as focus loss:

- `WM_ACTIVATE` with `WA_INACTIVE`;
- `WM_ACTIVATEAPP(FALSE)`;
- `WM_KILLFOCUS`.

Call `CancelCursorRecovery(true)` and then `BeginFocusLoss`. `BeginFocusLoss` starts a new attempt, records whether clipping was active, saves the current display dimensions, clears pending recovery state, and records that focus has not yet returned.

### 3. Detect focus return

Treat these events as possible focus return:

- active `WM_ACTIVATE`;
- `WM_ACTIVATEAPP(TRUE)`;
- `WM_SETFOCUS`.

Call `FocusReturned` to timestamp the event, then call `ArmCursorRecovery(false)`. This initial arm acts as a fallback; when display confirmation is required, it uses a longer timeout so restoration is not performed during the fullscreen mode transition.

### 4. Confirm fullscreen restoration

On `WM_DISPLAYCHANGE`, record the new display dimensions. If recovery is active, focus has returned, and the dimensions match those captured before focus loss, call `ArmCursorRecovery(true)`. Replace any existing recovery timer with a short stabilization delay before applying the clip.

If display confirmation is not required, use the short delay immediately after focus returns.

### 5. Apply the saved clip

When the recovery timer fires, `ApplyCursorRecovery` must reject stale, duplicate, disabled, or incomplete attempts. Before changing the clip, require all of the following:

```text
GetForegroundWindow() == gameWindow
GetFocus() == gameWindow
IsIconic(gameWindow) == FALSE
IsWindowVisible(gameWindow) == TRUE
```

Read the current rectangle with `GetClipCursor` and compare it with the saved rectangle. Call the original `ClipCursor` only when they differ, then query the rectangle again to verify the result. Mark the attempt as processed and remember whether recovery successfully applied the clip.

### 6. Cancel and release safely

`CancelCursorRecovery` must stop the pending timer and clear the pending flag. On focus loss, window destruction, or shutdown, call `ClipCursor(nullptr)` only if the recovery code previously applied the clip. Never release a clip merely observed or applied by the game.

## Function Responsibilities

- `HookClipCursor`: observes successful game requests and saves the actual clip state.
- `BeginFocusLoss`: creates and snapshots a recovery attempt.
- `FocusReturned`: records when activation returns.
- `HookWndProc`: translates activation and display messages into recovery actions.
- `ArmCursorRecovery`: schedules or replaces the recovery timer.
- `ApplyCursorRecovery`: validates window state, restores the rectangle, and verifies it.
- `CancelCursorRecovery`: cancels pending work and releases only a clip applied by recovery.

Direct3D hooks, DirectInput hooks, forced window activation, and forced input acquisition are not part of this fix.
