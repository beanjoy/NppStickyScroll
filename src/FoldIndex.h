// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Scintilla.h"

namespace sticky {
using Line = Sci_Position;

// A persistent ancestry chain for each indexed document line. No text is stored.
// Building is incremental; the caller imposes a time/line budget per UI work item.
class FoldIndex {
public:
    struct Entry { Line parent; int rawLevel; };
    Line size() const noexcept { return static_cast<Line>(entries_.size()); }
    void invalidate(Line from) noexcept {
        from = std::clamp(from, Line{0}, size());
        entries_.resize(static_cast<size_t>(from));
        top_ = from == 0 ? -1 : (header(from - 1) ? from - 1 : parent(from - 1));
    }
    void clear() noexcept { invalidate(0); }
    int level(Line line) const noexcept { return raw(line) & SC_FOLDLEVELNUMBERMASK; }
    int raw(Line line) const noexcept { return entries_[static_cast<size_t>(line)].rawLevel; }
    bool header(Line line) const noexcept { return (raw(line) & SC_FOLDLEVELHEADERFLAG) != 0; }
    Line parent(Line line) const noexcept { return entries_[static_cast<size_t>(line)].parent; }
    void append(int rawLevel) {
        const int number = rawLevel & SC_FOLDLEVELNUMBERMASK;
        while (top_ >= 0 && level(top_) >= number) top_ = parent(top_);
        entries_.push_back({top_, rawLevel});
        if (rawLevel & SC_FOLDLEVELHEADERFLAG) top_ = size() - 1;
    }
    template<class IsOffscreen>
    std::vector<Line> ancestors(Line line, int effectiveLevel, IsOffscreen offscreen,
                               size_t limit = 5, bool nearest = true) const {
        std::vector<Line> result;
        if (!limit || line < 0 || line >= size() || effectiveLevel < SC_FOLDLEVELBASE) return result;
        size_t next = 0;
        for (Line p = parent(line); p >= 0; p = parent(p)) {
            if (level(p) >= effectiveLevel || !offscreen(p)) continue;
            if (result.size() < limit) result.push_back(p);
            else { result[next] = p; next = (next + 1) % limit; }
            if (nearest && result.size() == limit) break;
        }
        // The bounded ring retains the outermost entries without storing every ancestor.
        if (!nearest && result.size() == limit) std::rotate(result.begin(), result.begin() + next, result.end());
        std::reverse(result.begin(), result.end());
        return result;
    }
private:
    std::vector<Entry> entries_;
    Line top_ = -1;
};
}
