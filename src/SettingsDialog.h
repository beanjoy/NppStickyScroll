// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include "Settings.h"

namespace sticky {
bool showSettingsDialog(HINSTANCE instance, HWND owner, Settings& settings);
}
