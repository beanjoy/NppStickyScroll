#include "FoldIndex.h"
#include <cstdlib>
#include <iostream>

using sticky::FoldIndex;
using sticky::Line;
void require(bool ok, const char* name) {
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
int main() {
    constexpr int base = SC_FOLDLEVELBASE, header = SC_FOLDLEVELHEADERFLAG;
    FoldIndex index;
    index.append(base | header);       // 0 root
    index.append((base + 1) | header); // 1 DeviceLog
    index.append((base + 2) | header); // 2 Windows
    index.append(base + 3);           // 3 body
    index.append(base + 3);           // 4 closing Windows
    index.append(base + 2);           // 5 next sibling
    auto all = [](Line) { return true; };
    require(index.ancestors(3, base + 3, all) == std::vector<Line>({0, 1, 2}), "ordered parents");
    require(index.ancestors(4, base + 2, all) == std::vector<Line>({0, 1}), "closing scope excluded");
    require(index.ancestors(5, base + 2, all) == std::vector<Line>({0, 1}), "no expired sibling");
    require(index.ancestors(3, base + 3, [](Line p) { return p != 2; }) == std::vector<Line>({0, 1}), "visible/wrapped header excluded");
    require(index.ancestors(0, base, all).empty(), "document start");
    index.invalidate(2);
    index.append(base + 1);
    index.append(base + 1);
    require(index.ancestors(3, base + 1, all) == std::vector<Line>({0}), "edit invalidates suffix and stack");
    index.clear();
    for (int i = 0; i < 20; ++i) index.append((base + i) | header);
    index.append(base + 20);
    require(index.ancestors(20, base + 20, all) == std::vector<Line>({15, 16, 17, 18, 19}), "nearest five");
    index.clear();
    index.append(base);
    require(index.ancestors(0, base, all).empty(), "no fold info");
    std::cout << "8 fold ancestry checks passed\n";
}
