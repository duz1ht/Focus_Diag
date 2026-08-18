#pragma once

#include <windows.h>

// DirectInput8Create is the only DirectInput declaration required by this
// dinput8 proxy. No DirectInput object or input method is intercepted.
using DirectInput8CreateFn = HRESULT(WINAPI*)(
    HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
