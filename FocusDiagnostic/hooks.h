#pragma once

#include <windows.h>

namespace fd {

bool InstallHooks(HMODULE self);
void RemoveHooks();

}  // namespace fd
