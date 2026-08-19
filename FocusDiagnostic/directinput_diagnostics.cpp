#include "legacy_dx8.h"

#include "diagnostics.h"
#include "logger.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace fd {
namespace {

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);
using DeviceSimpleFn = HRESULT(STDMETHODCALLTYPE*)(void*);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA,
                                                    LPDWORD, DWORD);
using SetCooperativeLevelFn = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);

// Enabled by default so a DirectInput object created concurrently with the
// initialization thread is not missed. InstallHooks applies the INI override.
std::atomic<bool> g_enabled{true};
CreateDeviceFn g_createDevice{};
DeviceSimpleFn g_acquire{};
DeviceSimpleFn g_unacquire{};
GetDeviceStateFn g_getDeviceState{};
GetDeviceDataFn g_getDeviceData{};
SetCooperativeLevelFn g_setCooperativeLevel{};
std::mutex g_devicesMutex;
std::unordered_map<void*, bool> g_devices;

constexpr GUID kSystemMouse =
    {0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kSystemKeyboard =
    {0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

bool SameGuid(REFGUID left, REFGUID right) {
    return InlineIsEqualGUID(left, right) != FALSE;
}

bool IsMouse(void* device) {
    std::lock_guard<std::mutex> lock(g_devicesMutex);
    const auto found = g_devices.find(device);
    return found != g_devices.end() && found->second;
}

const char* DeviceName(void* device) {
    std::lock_guard<std::mutex> lock(g_devicesMutex);
    const auto found = g_devices.find(device);
    if (found == g_devices.end()) return "UNKNOWN";
    return found->second ? "MOUSE" : "KEYBOARD";
}

template <typename Function>
bool Patch(void** slot, void* hook, Function& original) {
    DWORD protection{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) return false;
    if (*slot != hook) {
        if (!original) original = reinterpret_cast<Function>(*slot);
        *slot = hook;
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    }
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), protection, &ignored);
    return true;
}

HRESULT STDMETHODCALLTYPE HookAcquire(void* device) {
    const HRESULT result = g_acquire(device);
    RecordDirectInputResult(IsMouse(device), true, result);
    Logger::Instance().Write("DINPUT", "%s Acquire -> 0x%08lX attempt=%lu",
        DeviceName(device), static_cast<unsigned long>(result), State().attempt.load());
    return result;
}

HRESULT STDMETHODCALLTYPE HookUnacquire(void* device) {
    const HRESULT result = g_unacquire(device);
    Logger::Instance().Write("DINPUT", "%s Unacquire -> 0x%08lX attempt=%lu",
        DeviceName(device), static_cast<unsigned long>(result), State().attempt.load());
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetDeviceState(void* device, DWORD size, LPVOID data) {
    const HRESULT result = g_getDeviceState(device, size, data);
    RecordDirectInputResult(IsMouse(device), false, result);
    Logger::Instance().Write("DINPUT", "%s GetDeviceState(size=%lu data=%p) -> 0x%08lX "
        "attempt=%lu", DeviceName(device), size, data, static_cast<unsigned long>(result),
        State().attempt.load());
    return result;
}

HRESULT STDMETHODCALLTYPE HookGetDeviceData(void* device, DWORD objectSize,
                                             LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                             DWORD flags) {
    const DWORD requested = count ? *count : 0;
    const HRESULT result = g_getDeviceData(device, objectSize, data, count, flags);
    RecordDirectInputResult(IsMouse(device), false, result);
    Logger::Instance().Write("DINPUT", "%s GetDeviceData(size=%lu data=%p requested=%lu "
        "returned=%lu flags=0x%08lX) -> 0x%08lX attempt=%lu", DeviceName(device),
        objectSize, data, requested, count ? *count : 0, flags,
        static_cast<unsigned long>(result), State().attempt.load());
    return result;
}

HRESULT STDMETHODCALLTYPE HookSetCooperativeLevel(void* device, HWND window, DWORD flags) {
    const HRESULT result = g_setCooperativeLevel(device, window, flags);
    Logger::Instance().Write("DINPUT",
        "%s SetCooperativeLevel(hwnd=%p flags=0x%08lX) -> 0x%08lX",
        DeviceName(device), window, flags, static_cast<unsigned long>(result));
    return result;
}

void PatchDevice(void* device, bool mouse) {
    auto** table = *reinterpret_cast<void***>(device);
    const bool patched =
        Patch(&table[7], reinterpret_cast<void*>(HookAcquire), g_acquire) &&
        Patch(&table[8], reinterpret_cast<void*>(HookUnacquire), g_unacquire) &&
        Patch(&table[9], reinterpret_cast<void*>(HookGetDeviceState), g_getDeviceState) &&
        Patch(&table[10], reinterpret_cast<void*>(HookGetDeviceData), g_getDeviceData) &&
        Patch(&table[13], reinterpret_cast<void*>(HookSetCooperativeLevel),
              g_setCooperativeLevel);
    {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        g_devices[device] = mouse;
    }
    Logger::Instance().Write("DINPUT", "%s device=%p hooks=%s",
                             mouse ? "MOUSE" : "KEYBOARD", device,
                             patched ? "YES" : "NO");
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(void* input, REFGUID guid, void** output,
                                            IUnknown* outer) {
    const HRESULT result = g_createDevice(input, guid, output, outer);
    const bool mouse = SameGuid(guid, kSystemMouse);
    const bool keyboard = SameGuid(guid, kSystemKeyboard);
    Logger::Instance().Write("DINPUT", "CreateDevice(type=%s) -> 0x%08lX object=%p",
        mouse ? "MOUSE" : (keyboard ? "KEYBOARD" : "OTHER"),
        static_cast<unsigned long>(result), output ? *output : nullptr);
    if (SUCCEEDED(result) && output && *output && (mouse || keyboard))
        PatchDevice(*output, mouse);
    return result;
}

}  // namespace

void SetDirectInputDiagnosticsEnabled(bool enabled) {
    g_enabled = enabled;
}

void ObserveDirectInput8(void* object) {
    if (!g_enabled || !object) return;
    auto** table = *reinterpret_cast<void***>(object);
    const bool patched = Patch(&table[3], reinterpret_cast<void*>(HookCreateDevice),
                               g_createDevice);
    Logger::Instance().Write("DINPUT", "DirectInput8 object=%p create-device-hook=%s",
                             object, patched ? "YES" : "NO");
}

}  // namespace fd
