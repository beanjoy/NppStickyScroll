// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <string>
#include <vector>
#include "FoldIndex.h"
#include "Settings.h"

namespace sticky {
class StickyView {
public:
    struct StyledRun {
        std::wstring text;
        unsigned char style;
    };
    using StyledLine = std::vector<StyledRun>;
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
    void configure(Settings settings);
    void appearanceChanged(bool theme = false);
#ifdef STICKY_HOST_TESTS
    const std::vector<Line>& testLines() const { return lines_; }
    int testHeight() const { return inset_; }
    int testTopBorder() const { return topBorder_; }
    const std::vector<StyledLine>& testStyledLines() const { return text_; }
    COLORREF testStyleColor(unsigned char style) const { return styleForeground_[style]; }
    Line testIndexedLines() const { return index_.size(); }
    COLORREF testForeground() const { return foreground_; }
    COLORREF testBackground() const { return background_; }
    LOGFONTW testFont() const { return fontSpec_; }
    int testXOffset() const { return xOffset_; }
    HWND testHeader() const { return header_; }
    int testNumberWidth() const { return numberRight_ - numberLeft_; }
    bool testTracking() const { return scrollTracking_; }
    HWND testScrollbar() const { return scrollbar_; }
    HWND testScrollHost() const { return scrollHost_; }
    unsigned long long testAppearanceRefreshes = 0;
    unsigned long long testUpdates = 0;
    unsigned long long testRowClears = 0;
    unsigned long long testBlankPaints = 0;
    unsigned long long testHeightChanges = 0;
#endif
private:
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK headerSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK scrollSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT dispatch(UINT, WPARAM, LPARAM);
    void setRows(std::vector<Line> rows);
    void refreshAppearance();
    void setInset(int height);
    void layoutHeader();
    void layoutEditor();
    void syncScrollbar();
    void scroll(UINT action);
    void redrawHeader();
    int hitRow(LPARAM clientPoint) const noexcept;
    void paint(HDC dc);
    std::wstring lineText(Line line) const;
    StyledLine styledLine(Line line) const;
    HWND editor_ = nullptr;
    HWND header_ = nullptr;
    HWND scrollHost_ = nullptr;
    HWND scrollbar_ = nullptr;
    RECT allocation_{}; // Full host-assigned pane rectangle, in parent client coordinates.
    Settings settings_;
    SciFnDirect direct_ = nullptr;
    sptr_t directPointer_ = 0;
    RequestUpdate request_ = nullptr;
    FoldIndex index_;
    Line contextEnd_ = -1; // Last fold line needed to prove the displayed ancestors (XML needs first + 1).
    sptr_t document_ = 0;
    int lexer_ = 0;
    int inset_ = 0;
    int rowHeight_ = 1;
    int topBorder_ = 0;
    int textLeft_ = 0;
    int xOffset_ = 0;
    int tabWidth_ = 4;
    int spaceWidth_ = 8;
    int numberLeft_ = 0;
    int numberRight_ = 0;
    bool inLayout_ = false;
    bool scrollTracking_ = false;
    bool originalVScroll_ = true;
    bool appearanceDirty_ = true;
    int scrollWidth_ = 0;
    bool textDirty_ = true;
    HFONT font_ = nullptr;
    LOGFONTW fontSpec_{};
    HFONT numberFont_ = nullptr;
    LOGFONTW numberFontSpec_{};
    COLORREF foreground_ = RGB(0, 0, 0);
    COLORREF background_ = RGB(255, 255, 255);
    COLORREF numberForeground_ = RGB(128, 128, 128);
    COLORREF numberBackground_ = RGB(240, 240, 240);
    std::vector<Line> lines_;
    std::array<COLORREF, 256> styleForeground_{};
    std::vector<StyledLine> text_;
};
}
