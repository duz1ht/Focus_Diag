# Alt-Tab Cursor Clipping Recovery — proxy dinput8.dll

Proxy DLL x86 for the confirmed Alt-Tab cursor clipping fix. It contains only:

- forwarding to the system `dinput8.dll`;
- interception of the game's `ClipCursor` import;
- window activation and display-change monitoring;
- restoration of the last active cursor clip after fullscreen returns.

It does not hook Direct3D or DirectInput objects, force window focus, call `SetCapture`, or create diagnostic logs. The working behavior is built in: display-change waiting is enabled and the stabilization delay is 250 ms. No INI file is required.

## Build

1. Install Visual Studio 2022 with **Desktop development with C++** and a Windows 10/11 SDK.
2. Open `FocusDiagnostic.sln`.
3. Select **Release** and **x86**.
4. Build the solution.

The output is `bin\Release\dinput8.dll`.

## Install

Place `dinput8.dll` next to the 32-bit game executable. The proxy loads the original `dinput8.dll` from the Windows system directory and forwards its five standard exports. Do not overwrite another mod's `dinput8.dll` without first checking compatibility.

## Recovery Flow

1. The `ClipCursor` hook records the last successful active rectangle requested by the game.
2. The window hook detects focus loss and remembers the fullscreen display dimensions.
3. After focus returns, it waits for the matching `WM_DISPLAYCHANGE` and then waits 250 ms. A 2.25-second timer is used as a fallback if that display message is not received.
4. If the game is foreground, focused, visible, and not minimized, the saved rectangle is reapplied only when the current clip differs.
5. A clip applied by the recovery code is released on the next focus loss.

The implementation details are documented in [`CursorClip_Recovery.md`](CursorClip_Recovery.md).
