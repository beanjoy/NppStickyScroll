// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <string>
#include <vector>
#include "FoldIndex.h"

namespace sticky {
class StickyView {
public:
    using RequestUpdate = void (*)();
    bool attach(HWND editor, RequestUpdate request);
    void detach() noexcept;
    void invalidate(Line from = 0) noexcept;
    // Returns true only when bounded indexing work remains (not when waiting for styling).
    bool update();
    void clear();
    HWND window() const noexcept { return editor_; }
    LRESULT sci(UINT message, WPARAM w = 0, LPARAM l = 0) const noexcept;
    void modified(const SCNotification& notification);
    void jump(size_t row);
#ifdef STICKY_HOST_TESTS
    const std::vector<Line>& testLines() const { return lines_; }
    int testHeight() const { return inset_; }
    Line testIndexedLines() const { return index_.size(); }
    COLORREF testForeground() const { return foreground_; }
    COLORREF testBackground() const { return background_; }
    LOGFONTW testFont() const { return fontSpec_; }
    int testXOffset() const { return xOffset_; }
    unsigned long long testUpdates = 0;
#endif
private:
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT dispatch(UINT, WPARAM, LPARAM);
    void setRows(std::vector<Line> rows);
    void refreshAppearance();
    void setInset(int height);
    RECT barRect() const noexcept;
    int hitRow(LPARAM screenPoint) const noexcept;
    void paint(HDC dc = nullptr);
    std::wstring lineText(Line line) const;
    HWND editor_ = nullptr;
    SciFnDirect direct_ = nullptr;
    sptr_t directPointer_ = 0;
    RequestUpdate request_ = nullptr;
    FoldIndex index_;
    sptr_t document_ = 0;
    int lexer_ = 0;
    int inset_ = 0;
    int rowHeight_ = 1;
    int textLeft_ = 0;
    int xOffset_ = 0;
    int tabWidth_ = 4;
    int spaceWidth_ = 8;
    bool inLayout_ = false;
    bool textDirty_ = true;
    HFONT font_ = nullptr;
    LOGFONTW fontSpec_{};
    COLORREF foreground_ = RGB(0, 0, 0);
    COLORREF background_ = RGB(255, 255, 255);
    std::vector<Line> lines_;
    std::vector<std::wstring> text_;
};
}
