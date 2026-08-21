# ClipCursor Recovery — dinput8.dll proxy

An x86 DLL proxy dedicated exclusively to restoring cursor confinement in older
games after Alt-Tab and diagnosing every requirement involved in that recovery.
It does not intercept DirectInput, Direct3D, `SetCursorPos`, `ShowCursor`, or
`SetCursor`, and it does not force window focus or activation.

## Building

1. In Visual Studio 2022, install **Desktop development with C++** and the
   Windows 10/11 SDK.
2. Open `FocusDiagnostic.sln`.
3. Select **Release** and **x86**.
4. Use **Build > Build Solution**.

The output is `bin\Release\dinput8.dll`. The proxy forwards the five exports
from `dinput8.dll` to the original library in the system directory; it does not
inspect or modify the DirectInput objects returned to the game.

## Installation

Copy `bin\Release\dinput8.dll` and `bin\Release\FocusDiagnostic.ini` to the
folder containing the game's executable. The game must be x86 and import
`dinput8.dll`. Do not replace another DLL with the same name before identifying
which mod it belongs to.

When loaded, the DLL creates `FocusDiagnostic.log` next to it. Test only with a
local/offline copy of the game; proxies may be flagged by anti-cheat systems.

## Configuration

```ini
[Recovery]
RestoreCursorClip=1
RestoreCursorClipDelayMs=250
WaitForDisplayChange=1
```

- `RestoreCursorClip=1` enables recovery. Use `0` for a passive session that
  logs the process without calling `ClipCursor` to restore confinement.
- `RestoreCursorClipDelayMs` sets the stabilization delay after focus returns or
  the video mode is confirmed; the value is clamped to the 1–10000 ms range.
- `WaitForDisplayChange=1` waits for the screen dimensions from before Alt-Tab
  to return. If `WM_DISPLAYCHANGE` does not arrive, there is a two-second
  fallback.

There are no options for DirectInput, Direct3D, cursor visibility, capture, or
forced activation because those subsystems are outside the scope of this DLL.

## Recovery process

1. The passive `ClipCursor` hook logs successful requests from the game and
   queries the rectangle that was actually applied with `GetClipCursor`.
2. When focus is lost, the DLL records whether clipping was active, the last
   rectangle applied, and the current screen dimensions.
3. When focus returns, a scheduler thread waits for video mode confirmation or
   the fallback without depending on `WM_TIMER` or the game's message queue.
4. Before taking action, it requires the game window to be in the foreground,
   have focus, be visible, and not be minimized.
5. It compares the current rectangle with the expected one. `ClipCursor` is
   called only if the rectangles differ.
6. After the call, `GetClipCursor` confirms whether the expected rectangle was
   actually applied.
7. Each attempt is identified by a generation and processed only once.
   Rearming, another loss of focus, and subsequent attempts invalidate earlier
   schedules. On a new loss of focus, only clipping applied by the DLL itself is
   released.

Internal calls use the original function directly and are not logged as new
requests from the game.

## Diagnostics

The log contains only the information needed to evaluate recovery:

- `ClipCursor` calls, the call result, and the actual rectangle;
- `WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_SETFOCUS`, `WM_KILLFOCUS`, and `WM_SIZE`,
  including the state and dimensions of the window's client area;
- `WM_DISPLAYCHANGE` and a comparison with the previous dimensions;
- scheduling time, requested deadline, actual delay, and scheduler thread
  overshoot;
- validation of foreground status, focus, visibility, and minimization;
- expected, previous, and resulting rectangles;
- whether recovery was necessary, whether the call worked, and whether it was
  confirmed;
- deactivation state (`DEACTIVATION_PENDING`, confirmed return, close request,
  or `SHUTDOWN`), including foreground, focus, display, and clipping ownership;
- manual and automatic success or failure snapshots.

During testing:

1. Enter gameplay and press **F10** to insert a `USER MARKER`.
2. Alt-Tab away from the game and return to it.
3. Press **F11** to record a manual snapshot.
4. Close the game normally and preserve `FocusDiagnostic.log`.

A successful attempt ends with `CLIP RESTORATION SUCCESS` and `restored=YES`.
If no active clipping was observed before focus was lost, the log reports
`NO ACTIVE CLIP CAPTURED BEFORE FOCUS LOSS`; in that case, there is no valid
rectangle to restore.

When the game calls `ClipCursor(NULL)` before the formal focus-loss messages,
the DLL keeps the last active rectangle as a candidate. Focus loss,
deactivation, and display changes only produce `DEACTIVATION_PENDING`, because
they can also occur during shutdown. The transition becomes
`FOCUS_TRANSITION_CONFIRMED` only when the same valid window regains foreground
status and focus while remaining visible and not minimized. `WM_CLOSE` logs only
a request; `WM_DESTROY`, `WM_NCDESTROY`, or `WM_ENDSESSION TRUE` confirms
`SHUTDOWN` and cancels recovery.

## Limitations

- Only `ClipCursor` calls imported directly by the main executable are
  intercepted. Calls made by auxiliary modules or obtained through
  `GetProcAddress` are not observed.
- Window discovery considers the first top-level, visible, unowned window that
  belongs to the process.
- The DLL restores confinement, not cursor visibility or appearance.
