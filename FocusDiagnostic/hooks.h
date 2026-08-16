#pragma once

#include <windows.h>

#include "legacy_dx8.h"

namespace fd {

bool InstallHooks(HMODULE self);
void TrackDirectInputObject(IDirectInput8A* object);
void RemoveHooks();

}  // namespace fd
