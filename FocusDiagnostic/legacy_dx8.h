#pragma once

#include <windows.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

// DirectInput8Create is the only DirectInput declaration required by this
// dinput8 proxy. No DirectInput object or input method is intercepted.
using DirectInput8CreateFn = HRESULT(WINAPI*)(
    HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

namespace fd {
void ObserveDirectInput8(void* object);
void SetDirectInputDiagnosticsEnabled(bool enabled);
}
