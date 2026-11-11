// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

namespace sticky {
struct Settings {
    static constexpr int maxAllowedRows = 20;
    int maxRows = 5;
    bool nearest = true;
    void normalize() noexcept { maxRows = std::clamp(maxRows, 1, maxAllowedRows); }
};
}
