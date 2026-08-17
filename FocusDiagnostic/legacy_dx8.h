#pragma once

#include <windows.h>

// Minimal DirectX 8 ABI declarations. Visual Studio 2022 no longer ships d3d8.h;
// only the interfaces and structures used by this diagnostic are declared here.
using D3DFORMAT = DWORD;
using D3DDEVTYPE = DWORD;
using D3DSWAPEFFECT = DWORD;
using D3DMULTISAMPLE_TYPE = DWORD;

struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

struct IDirect3D8;
struct IDirect3DDevice8;
using Direct3DCreate8Fn = IDirect3D8* (WINAPI*)(UINT);

struct DIDEVICEINSTANCEA;
struct IDirectInput8A;
struct IDirectInputDevice8A;
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

// DirectInput GUIDs used only for device classification and CreateDevice hooking.
inline constexpr GUID kIID_IDirectInput8A =
    {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
inline constexpr GUID kGuidSysMouse =
    {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
inline constexpr GUID kGuidSysKeyboard =
    {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

inline constexpr HRESULT kD3DErrDeviceLost = static_cast<HRESULT>(0x88760868L);
inline constexpr HRESULT kD3DErrDeviceNotReset = static_cast<HRESULT>(0x88760869L);
inline constexpr HRESULT kDiErrOtherAppHasPriority = static_cast<HRESULT>(0x80070005L);
