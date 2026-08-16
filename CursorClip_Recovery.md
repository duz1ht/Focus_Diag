# Alt-Tab Cursor Clip Recovery

## Problem

After Alt-Tab, the game regains focus and fullscreen mode but behaves as if it were inactive, with reduced performance.

## Cause

Windows releases `ClipCursor` when the fullscreen game loses focus. The game does not restore its previous clipping rectangle. ReShade avoids the problem by saving and reapplying that rectangle.

## Solution

Save the last valid clip requested by the game and restore it after the window regains focus and the display mode is restored.

```cpp
static RECT lastClip = {};
static bool hasLastClip = false;

BOOL WINAPI HookClipCursor(const RECT *rect)
{
    if (rect != nullptr && rect->right > rect->left && rect->bottom > rect->top)
    {
        lastClip = *rect;
        hasLastClip = true;
    }

    return OriginalClipCursor(rect);
}

void RestoreCursorClip(HWND gameWindow)
{
    if (hasLastClip &&
        GetForegroundWindow() == gameWindow &&
        GetFocus() == gameWindow &&
        !IsIconic(gameWindow) &&
        IsWindowVisible(gameWindow))
    {
        OriginalClipCursor(&lastClip);
    }
}
```

Call `RestoreCursorClip` shortly after `WM_ACTIVATEAPP(TRUE)` or `WM_SETFOCUS`, preferably after the fullscreen `WM_DISPLAYCHANGE` event.

```ini
[Hooks]
Window=1
Cursor=1

[Recovery]
RestoreCursorClip=1
RestoreCursorClipDelayMs=250
WaitForDisplayChange=1
```
