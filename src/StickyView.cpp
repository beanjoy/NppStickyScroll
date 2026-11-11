// SPDX-License-Identifier: GPL-3.0-or-later
#include "StickyView.h"
#include "SciLexer.h"
#include "Notepad_plus_msgs.h"
#include <windowsx.h>
#include <chrono>
#include <cstring>

namespace sticky {
namespace {
constexpr UINT_PTR subclassId = 0x53544943;
constexpr size_t maxTextBytes = 4096;
constexpr Line maxLinesPerSlice = 16384;

std::wstring decodeText(const char* text, int length, UINT codePage) {
    const int size = MultiByteToWideChar(codePage, 0, text, length, nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (size) MultiByteToWideChar(codePage, 0, text, length, result.data(), size);
    for (auto& ch : result) if (ch < L' ' && ch != L'\t') ch = L' ';
    return result;
}
}

LRESULT StickyView::sci(UINT message, WPARAM w, LPARAM l) const noexcept {
    // All callers run on the host UI thread. Avoid the Windows/host subclass chain
    // for Scintilla API calls, including the many small read-only scroll queries.
    return direct_ ? direct_(directPointer_, message, w, l) : SendMessageW(editor_, message, w, l);
}

bool StickyView::attach(HWND editor, RequestUpdate request) {
    if (editor_) return true;
    editor_ = editor;
    request_ = request;
    directPointer_ = static_cast<sptr_t>(sci(SCI_GETDIRECTPOINTER));
    direct_ = reinterpret_cast<SciFnDirect>(sci(SCI_GETDIRECTFUNCTION));
    GetWindowRect(editor_, &allocation_);
    MapWindowPoints(nullptr, GetParent(editor_), reinterpret_cast<POINT*>(&allocation_), 2);
    header_ = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Sticky Scroll",
        WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, GetParent(editor_), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!header_ || !SetWindowSubclass(header_, headerSubclass, subclassId, reinterpret_cast<DWORD_PTR>(this))) {
        if (header_) DestroyWindow(header_);
        header_ = nullptr;
        editor_ = nullptr;
        return false;
    }
    if (!SetWindowSubclass(editor_, subclass, subclassId, reinterpret_cast<DWORD_PTR>(this))) {
        DestroyWindow(header_);
        header_ = nullptr;
        editor_ = nullptr;
        return false;
    }
    originalVScroll_ = sci(SCI_GETVSCROLLBAR) != 0;
    scrollHost_ = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0, GetParent(editor_), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (scrollHost_ && SetWindowSubclass(scrollHost_, scrollSubclass, subclassId, reinterpret_cast<DWORD_PTR>(this))) {
        scrollbar_ = CreateWindowExW(WS_EX_NOACTIVATE, L"SCROLLBAR", L"", WS_CHILD | WS_VISIBLE | SBS_VERT,
            0, 0, 0, 0, scrollHost_, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (!scrollbar_ || !SetWindowSubclass(scrollbar_, scrollSubclass, subclassId, reinterpret_cast<DWORD_PTR>(this))) {
        detach();
        return false;
    }
    appearanceDirty_ = true;
    // Disable only Scintilla's built-in vertical scrollbar. Its document, folding,
    // horizontal scrollbar, selection and parent notification route remain intact.
    inLayout_ = true;
    sci(SCI_SETVSCROLLBAR, false);
    inLayout_ = false;
    appearanceChanged(true);
    return true;
}

void StickyView::detach() noexcept {
    scrollTracking_ = false;
    if (editor_ && IsWindow(editor_)) {
        // Restore the host's allocation before removing our layout adapter.
        lines_.clear();
        inset_ = scrollWidth_ = 0;
        inLayout_ = true;
        sci(SCI_SETVSCROLLBAR, originalVScroll_);
        inLayout_ = false;
        layoutEditor();
        RemoveWindowSubclass(editor_, subclass, subclassId);
    }
    editor_ = nullptr;
    if (header_ && IsWindow(header_)) DestroyWindow(header_);
    header_ = nullptr;
    if (scrollHost_ && IsWindow(scrollHost_)) DestroyWindow(scrollHost_);
    scrollHost_ = scrollbar_ = nullptr;
    direct_ = nullptr;
    directPointer_ = 0;
    document_ = 0;
    index_.clear();
    text_.clear();
    if (font_) DeleteObject(font_);
    font_ = nullptr;
    if (numberFont_) DeleteObject(numberFont_);
    numberFont_ = nullptr;
}

void StickyView::appearanceChanged(bool theme) {
    appearanceDirty_ = textDirty_ = true;
    if (theme && scrollbar_) {
        // Official host implementation supports applying its scrollbar theme directly.
        constexpr WPARAM themeDirectly = 0x10;
        SendMessageW(GetAncestor(editor_, GA_ROOT), NPPM_DARKMODESUBCLASSANDTHEME,
            themeDirectly, reinterpret_cast<LPARAM>(scrollbar_));
    }
    if (request_) request_();
}

void StickyView::configure(Settings settings) {
    settings.normalize();
    settings_ = settings;
    textDirty_ = true;
    if (request_) request_();
}

void StickyView::invalidate(Line from) noexcept {
#ifdef STICKY_HOST_TESTS
    if (!lines_.empty()) ++testRowClears;
#endif
    index_.invalidate(from);
    contextEnd_ = -1;
    textDirty_ = true;
    // Existing rows must never remain clickable after an edit/document switch.
    lines_.clear();
    text_.clear();
    redrawHeader();
}

void StickyView::modified(const SCNotification& n) {
    if (n.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
        const Line at = static_cast<Line>(sci(SCI_LINEFROMPOSITION, n.position));
        invalidate(std::max(Line{0}, at - 1));
    } else if (n.modificationType & SC_MOD_CHANGEFOLD) {
        // Idle lexing emits fold changes far below the viewport after scrolling stops.
        // Those lines cannot change the already-proven ancestry above them. Invalidate
        // only an indexed suffix; never blank the current rows for unrelated work.
        const Line from = std::max(Line{0}, n.line);
        if (from >= index_.size()) return;
        if (from <= contextEnd_) invalidate(from);
        else index_.invalidate(from);
    } else if (n.modificationType & SC_MOD_CHANGESTYLE) {
        // A lexer may restyle a header without changing its text or fold level.
        for (const Line line : lines_) {
            if (sci(SCI_POSITIONFROMLINE, line) < n.position + n.length &&
                sci(SCI_GETLINEENDPOSITION, line) > n.position) {
                textDirty_ = true;
                break;
            }
        }
    }
}

void StickyView::clear() {
    index_.clear();
    contextEnd_ = -1;
    setRows({});
}

bool StickyView::update() {
#ifdef STICKY_HOST_TESTS
    ++testUpdates;
#endif
    if (!editor_) return false;
    const auto document = static_cast<sptr_t>(sci(SCI_GETDOCPOINTER));
    const int lexer = static_cast<int>(sci(SCI_GETLEXER));
    if (document != document_ || lexer != lexer_) {
        invalidate();
        document_ = document;
        lexer_ = lexer;
        appearanceDirty_ = true;
    }
    if (appearanceDirty_) refreshAppearance();
    const int offset = static_cast<int>(sci(SCI_GETXOFFSET));
    if (xOffset_ != offset) { xOffset_ = offset; redrawHeader(); }
    syncScrollbar();
    if (!IsWindowVisible(editor_) || (lexer != SCLEX_JSON && lexer != SCLEX_XML) ||
        !sci(SCI_GETPROPERTYINT, reinterpret_cast<WPARAM>("fold"))) {
        clear();
        return false;
    }
    const Line firstDisplay = static_cast<Line>(sci(SCI_GETFIRSTVISIBLELINE));
    const Line first = static_cast<Line>(sci(SCI_DOCLINEFROMVISIBLE, firstDisplay));
    const Line count = static_cast<Line>(sci(SCI_GETLINECOUNT));
    if (first <= 0 || first >= count) {
        setRows({});
        return false;
    }
    // XML stores the outgoing level on the next line; JSON also stores it in the high word.
    // Require complete styling of the relevant line(s). Never force whole-document colourise.
    const Line required = std::min(first + (lexer == SCLEX_XML ? 1 : 0), count - 1);
    const auto length = sci(SCI_GETLENGTH);
    const auto styled = sci(SCI_GETENDSTYLED);
    const auto requiredEnd = required + 1 < count ? sci(SCI_POSITIONFROMLINE, required + 1) : length;
    if (styled < requiredEnd || (lexer == SCLEX_XML && first + 1 >= count)) {
        setRows({});
        return false; // A later SCN_PAINTED/SCN_MODIFIED will retry.
    }
    const auto start = std::chrono::steady_clock::now();
    Line budget = maxLinesPerSlice;
    while (index_.size() <= required && budget-- > 0) {
        const auto level = direct_ ? direct_(directPointer_, SCI_GETFOLDLEVEL, index_.size(), 0)
                                   : sci(SCI_GETFOLDLEVEL, index_.size());
        index_.append(static_cast<int>(level));
        if ((budget & 63) == 0 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2)) break;
    }
    if (index_.size() <= required) {
        setRows({});
        return true;
    }
    const int before = index_.level(first);
    const int after = lexer == SCLEX_JSON
        ? (static_cast<unsigned>(index_.raw(first)) >> 16) & SC_FOLDLEVELNUMBERMASK
        : index_.level(first + 1);
    if (before < SC_FOLDLEVELBASE || after < SC_FOLDLEVELBASE) {
        setRows({});
        return false;
    }
    const int paneHeight = static_cast<int>(allocation_.bottom - allocation_.top);
    const int availableRows = std::max(0, (paneHeight - topBorder_ -
        GetSystemMetricsForDpi(SM_CYHSCROLL, GetDpiForWindow(editor_)) - 3) / rowHeight_ - 1);
    int limit = std::min(settings_.maxRows, availableRows);
    auto rows = index_.ancestors(first, std::min(before, after), [&](Line parent) {
        if (!sci(SCI_GETLINEVISIBLE, parent)) return false;
        const Line display = static_cast<Line>(sci(SCI_VISIBLEFROMDOCLINE, parent));
        const Line wraps = static_cast<Line>(sci(SCI_WRAPCOUNT, parent));
        if (display < 0 || display + wraps > firstDisplay) return false;
        if (lexer == SCLEX_XML) {
            // LexHTML can also fold comments/CDATA, or choose the final attribute line
            // of a multiline start tag. Only show unambiguous element start lines.
            const auto text = lineText(parent);
            const auto begin = text.find_first_not_of(L" \t");
            if (begin == std::wstring::npos || begin + 1 >= text.size() || text[begin] != L'<') return false;
            const auto next = text[begin + 1];
            if (next == L'!' || next == L'?' || next == L'/' || next == L' ' || next == L'\t') return false;
        }
        return true;
    }, static_cast<size_t>(limit), settings_.nearest);
    contextEnd_ = required;
    setRows(std::move(rows));
    return false;
}

std::wstring StickyView::lineText(Line line) const {
    const auto start = sci(SCI_POSITIONFROMLINE, line);
    const auto end = sci(SCI_GETLINEENDPOSITION, line);
    const auto bytes = std::min<sptr_t>(end - start, maxTextBytes);
    if (start < 0 || bytes <= 0) return {};
    std::string text(static_cast<size_t>(bytes) + 1, '\0');
    Sci_TextRangeFull range{{start, start + bytes}, text.data()};
    sci(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&range));
    UINT codePage = static_cast<UINT>(sci(SCI_GETCODEPAGE));
    if (codePage == 0) codePage = CP_ACP;
    auto result = decodeText(text.data(), static_cast<int>(bytes), codePage);
    if (end - start > bytes) result += L"\u2026";
    return result;
}

StickyView::StyledLine StickyView::styledLine(Line line) const {
    const auto start = sci(SCI_POSITIONFROMLINE, line);
    const auto lineEnd = sci(SCI_GETLINEENDPOSITION, line);
    auto end = std::min<sptr_t>(lineEnd, start + maxTextBytes);
    if (start < 0 || end <= start) return {};
    // Keep the 4 KiB cap without cutting a UTF-8/DBCS character in half.
    if (end < lineEnd) end = sci(SCI_POSITIONBEFORE, end + 1);
    const auto bytes = static_cast<size_t>(end - start);
    std::string styled(bytes * 2 + 2, '\0');
    Sci_TextRangeFull range{{start, end}, styled.data()};
    sci(SCI_GETSTYLEDTEXTFULL, 0, reinterpret_cast<LPARAM>(&range));
    UINT codePage = static_cast<UINT>(sci(SCI_GETCODEPAGE));
    if (!codePage) codePage = CP_ACP;
    StyledLine result;
    for (size_t begin = 0; begin < bytes;) {
        const auto style = static_cast<unsigned char>(styled[begin * 2 + 1]);
        size_t next = begin;
        std::string text;
        do { text.push_back(styled[next * 2]); ++next; }
        while (next < bytes && static_cast<unsigned char>(styled[next * 2 + 1]) == style);
        result.push_back({decodeText(text.data(), static_cast<int>(text.size()), codePage), style});
        begin = next;
    }
    if (end < lineEnd) result.push_back({L"\u2026", STYLE_DEFAULT});
    return result;
}

void StickyView::refreshAppearance() {
#ifdef STICKY_HOST_TESTS
    ++testAppearanceRefreshes;
#endif
    appearanceDirty_ = false;
    const int trackWidth = originalVScroll_ ? GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(editor_)) : 0;
    if (scrollWidth_ != trackWidth) { scrollWidth_ = trackWidth; layoutEditor(); }
    const int oldHeight = rowHeight_;
    const int oldTopBorder = topBorder_;
    const int oldLeft = textLeft_, oldOffset = xOffset_, oldTab = tabWidth_;
    const COLORREF oldFore = foreground_, oldBack = background_;
    const COLORREF oldNumberFore = numberForeground_, oldNumberBack = numberBackground_;
    const int oldNumberLeft = numberLeft_, oldNumberRight = numberRight_;
    rowHeight_ = std::max(1, static_cast<int>(sci(SCI_TEXTHEIGHT, 0)));
    foreground_ = static_cast<COLORREF>(sci(SCI_STYLEGETFORE, STYLE_DEFAULT));
    background_ = static_cast<COLORREF>(sci(SCI_STYLEGETBACK, STYLE_DEFAULT));
    POINT clientOrigin{};
    ClientToScreen(editor_, &clientOrigin);
    RECT editorRect{};
    GetWindowRect(editor_, &editorRect);
    topBorder_ = std::max(0, static_cast<int>(clientOrigin.y - editorRect.top));
    textLeft_ = static_cast<int>(clientOrigin.x - editorRect.left);
    numberLeft_ = numberRight_ = 0;
    const int margins = static_cast<int>(sci(SCI_GETMARGINS));
    for (int m = 0; m < margins; ++m) {
        const int width = static_cast<int>(sci(SCI_GETMARGINWIDTHN, m));
        if (sci(SCI_GETMARGINTYPEN, m) == SC_MARGIN_NUMBER && width > 0) {
            numberLeft_ = textLeft_;
            numberRight_ = textLeft_ + width;
        }
        textLeft_ += width;
    }
    textLeft_ += static_cast<int>(sci(SCI_GETMARGINLEFT));
    numberForeground_ = static_cast<COLORREF>(sci(SCI_STYLEGETFORE, STYLE_LINENUMBER));
    numberBackground_ = static_cast<COLORREF>(sci(SCI_STYLEGETBACK, STYLE_LINENUMBER));
    xOffset_ = static_cast<int>(sci(SCI_GETXOFFSET));
    tabWidth_ = std::clamp(static_cast<int>(sci(SCI_GETTABWIDTH)), 1, 256);
    spaceWidth_ = std::max(1, static_cast<int>(sci(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<LPARAM>(" "))));
    const auto updateFont = [&](int style, HFONT& font, LOGFONTW& previous) {
        const auto nameLength = sci(SCI_STYLEGETFONT, style, 0);
        std::string name;
        if (nameLength > 0 && nameLength < 4096) {
            name.resize(static_cast<size_t>(nameLength) + 1, '\0');
            sci(SCI_STYLEGETFONT, style, reinterpret_cast<LPARAM>(name.data()));
        }
        LOGFONTW spec{};
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, spec.lfFaceName, LF_FACESIZE);
        const int points100 = static_cast<int>(sci(SCI_STYLEGETSIZEFRACTIONAL, style)) +
            static_cast<int>(sci(SCI_GETZOOM)) * 100;
        spec.lfHeight = -std::max(1, MulDiv(points100, static_cast<int>(GetDpiForWindow(editor_)), 7200));
        spec.lfWeight = static_cast<LONG>(sci(SCI_STYLEGETWEIGHT, style));
        spec.lfItalic = static_cast<BYTE>(sci(SCI_STYLEGETITALIC, style));
        spec.lfCharSet = static_cast<BYTE>(sci(SCI_STYLEGETCHARACTERSET, style));
        spec.lfQuality = CLEARTYPE_QUALITY;
        const bool changed = !font || std::memcmp(&previous, &spec, sizeof(spec)) != 0;
        if (changed) {
            HFONT replacement = CreateFontIndirectW(&spec);
            if (replacement) {
                if (font) DeleteObject(font);
                font = replacement;
                previous = spec;
            }
        }
        return changed;
    };
    const bool fontChanged = updateFont(STYLE_DEFAULT, font_, fontSpec_);
    const bool numberFontChanged = updateFont(STYLE_LINENUMBER, numberFont_, numberFontSpec_);
    if (fontChanged || numberFontChanged || oldNumberFore != numberForeground_ || oldNumberBack != numberBackground_ ||
        oldNumberLeft != numberLeft_ || oldNumberRight != numberRight_ ||
        oldTopBorder != topBorder_ || oldHeight != rowHeight_ || oldFore != foreground_ || oldBack != background_ ||
        oldLeft != textLeft_ || oldOffset != xOffset_ || oldTab != tabWidth_)
        textDirty_ = true;
}

void StickyView::setRows(std::vector<Line> rows) {
    const bool changed = rows != lines_ || textDirty_;
    if (changed) {
        lines_ = std::move(rows);
        text_.clear();
        std::array<bool, 256> fetched{};
        for (const Line line : lines_) {
            text_.push_back(styledLine(line));
            for (const auto& run : text_.back()) {
                if (!fetched[run.style]) {
                    styleForeground_[run.style] = static_cast<COLORREF>(sci(SCI_STYLEGETFORE, run.style));
                    fetched[run.style] = true;
                }
            }
        }
        textDirty_ = false;
    }
    const int wanted = lines_.empty() ? 0 : topBorder_ + static_cast<int>(lines_.size()) * rowHeight_ + 1;
    setInset(wanted);
    if (changed) redrawHeader();
    layoutHeader();
}

void StickyView::setInset(int height) {
    if (inset_ == height || !editor_) return;
#ifdef STICKY_HOST_TESTS
    ++testHeightChanges;
#endif
    inset_ = height;
    layoutEditor();
    layoutHeader();
    syncScrollbar();
    // Resizing at EOF can clamp the host's first visible line; recompute once after layout.
    if (request_) request_();
}

void StickyView::layoutEditor() {
    if (!editor_) return;
    inLayout_ = true;
    SetWindowPos(editor_, nullptr, allocation_.left, allocation_.top + inset_,
        std::max(0L, allocation_.right - allocation_.left - scrollWidth_),
        std::max(0L, allocation_.bottom - allocation_.top - inset_),
        SWP_NOZORDER | SWP_NOACTIVATE);
    inLayout_ = false;
}

void StickyView::layoutHeader() {
    if (!header_) return;
    const bool visible = inset_ && IsWindowVisible(editor_);
    RECT current{};
    GetWindowRect(header_, &current);
    MapWindowPoints(nullptr, GetParent(header_), reinterpret_cast<POINT*>(&current), 2);
    const RECT wanted{allocation_.left, allocation_.top, allocation_.right - scrollWidth_, allocation_.top + inset_};
    if (EqualRect(&current, &wanted) && (IsWindowVisible(header_) != FALSE) == visible) return;
    const UINT visibility = visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    SetWindowPos(header_, HWND_TOP, allocation_.left, allocation_.top,
        std::max(0L, allocation_.right - allocation_.left - scrollWidth_), inset_, SWP_NOACTIVATE | visibility);
}

void StickyView::syncScrollbar() {
    if (!scrollHost_ || !editor_) return;
    const bool visible = originalVScroll_ && IsWindowVisible(editor_);
    const int height = std::max(0L, allocation_.bottom - allocation_.top);
    RECT current{};
    GetWindowRect(scrollHost_, &current);
    MapWindowPoints(nullptr, GetParent(scrollHost_), reinterpret_cast<POINT*>(&current), 2);
    const RECT wanted{allocation_.right - scrollWidth_, allocation_.top, allocation_.right, allocation_.bottom};
    if (!EqualRect(&current, &wanted) || (IsWindowVisible(scrollHost_) != FALSE) != visible)
        SetWindowPos(scrollHost_, HWND_TOP, wanted.left, wanted.top, scrollWidth_, height,
            SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    const int bottom = (GetWindowLongPtrW(editor_, GWL_STYLE) & WS_HSCROLL)
        ? GetSystemMetricsForDpi(SM_CYHSCROLL, GetDpiForWindow(editor_)) : 0;
    const int trackHeight = std::max(0, height - topBorder_ * 2 - bottom);
    GetClientRect(scrollbar_, &current);
    if (!scrollTracking_ && (current.right != scrollWidth_ || current.bottom != trackHeight))
        SetWindowPos(scrollbar_, nullptr, 0, topBorder_, scrollWidth_, trackHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    // A thumb drag owns its range/page until release, independent of body resizing.
    // In particular, do not call SetScrollInfo while Windows tracks the thumb.
    if (scrollTracking_) return;
    const auto page = std::max<sptr_t>(1, sci(SCI_LINESONSCREEN));
    const auto displayed = sci(SCI_VISIBLEFROMDOCLINE, sci(SCI_GETLINECOUNT));
    const auto maxTop = std::max<sptr_t>(0, displayed - (sci(SCI_GETENDATLASTLINE) ? page : 1));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    info.nMax = static_cast<int>(std::min<sptr_t>(INT_MAX, maxTop + page - 1));
    info.nPage = static_cast<UINT>(page);
    info.nPos = static_cast<int>(sci(SCI_GETFIRSTVISIBLELINE));
    SCROLLINFO old{sizeof(old), SIF_RANGE | SIF_PAGE | SIF_POS};
    GetScrollInfo(scrollbar_, SB_CTL, &old);
    if (old.nMax != info.nMax || old.nPage != info.nPage || old.nPos != info.nPos)
        SetScrollInfo(scrollbar_, SB_CTL, &info, TRUE);
}

void StickyView::scroll(UINT action) {
    if (action == SB_THUMBTRACK || action == SB_THUMBPOSITION) {
        scrollTracking_ = true;
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
        GetScrollInfo(scrollbar_, SB_CTL, &info);
        // HIWORD(WPARAM) truncates at 65,535; SIF_TRACKPOS is the full 32-bit position.
        sci(SCI_SETFIRSTVISIBLELINE, info.nTrackPos);
    } else if (action == SB_ENDSCROLL) {
        scrollTracking_ = false;
        syncScrollbar();
    } else {
        // Line/page/top/bottom commands use the editor's own fold/wrap-aware behavior.
        SendMessageW(editor_, WM_VSCROLL, action, 0);
    }
    if (request_) request_();
}

LRESULT CALLBACK StickyView::scrollSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l,
                                           UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<StickyView*>(data);
    if (message == WM_VSCROLL && reinterpret_cast<HWND>(l) == self->scrollbar_) {
        self->scroll(LOWORD(w));
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST && hwnd == self->scrollHost_) return HTCLIENT;
    if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
        self->scrollTracking_ = false;
        if (self->request_) self->request_();
    }
    if (message == WM_CTLCOLORSCROLLBAR || message == WM_CTLCOLORSTATIC) {
        SetDCBrushColor(reinterpret_cast<HDC>(w), self->background_);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    if (message == WM_ERASEBKGND && hwnd == self->scrollHost_) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        SetDCBrushColor(reinterpret_cast<HDC>(w), self->background_);
        FillRect(reinterpret_cast<HDC>(w), &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return TRUE;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, scrollSubclass, subclassId);
    return DefSubclassProc(hwnd, message, w, l);
}

void StickyView::redrawHeader() {
    if (header_ && inset_) InvalidateRect(header_, nullptr, FALSE);
}

int StickyView::hitRow(LPARAM clientPoint) const noexcept {
    if (!inset_) return -1;
    POINT point{GET_X_LPARAM(clientPoint), GET_Y_LPARAM(clientPoint)};
    RECT rect{};
    GetClientRect(header_, &rect);
    if (!PtInRect(&rect, point)) return -1;
    if (point.y < topBorder_) return -1;
    const int row = (point.y - topBorder_) / rowHeight_;
    return row < static_cast<int>(lines_.size()) ? row : -1;
}

void StickyView::paint(HDC windowDC) {
    if (!inset_ || !header_ || !windowDC) return;
#ifdef STICKY_HOST_TESTS
    if (lines_.empty()) ++testBlankPaints;
#endif
    RECT bar{};
    GetClientRect(header_, &bar);
    const int width = bar.right - bar.left;
    if (width <= 0) return;
    HDC dc = CreateCompatibleDC(windowDC);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDC, width, inset_);
    if (dc && bitmap) {
        const auto oldBitmap = SelectObject(dc, bitmap);
        const auto oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(SYSTEM_FONT));
        RECT rect{0, 0, width, inset_};
        SetDCBrushColor(dc, background_);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        RECT gutter{0, topBorder_, textLeft_ - static_cast<int>(sci(SCI_GETMARGINLEFT)), inset_ - 1};
        SetDCBrushColor(dc, numberBackground_);
        FillRect(dc, &gutter, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (numberRight_ > numberLeft_) {
            SelectObject(dc, numberFont_ ? numberFont_ : font_);
            SetTextColor(dc, numberForeground_);
            const int padding = std::max(2, MulDiv(3, GetDpiForWindow(editor_), 96));
            for (size_t i = 0; i < lines_.size(); ++i) {
                const std::wstring number = std::to_wstring(lines_[i] + 1);
                RECT row{numberLeft_, topBorder_ + static_cast<LONG>(i) * rowHeight_, numberRight_ - padding,
                    topBorder_ + (static_cast<LONG>(i) + 1) * rowHeight_};
                DrawTextW(dc, number.c_str(), static_cast<int>(number.size()), &row,
                    DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            }
        }
        SelectObject(dc, font_ ? font_ : GetStockObject(SYSTEM_FONT));
        SetTextColor(dc, foreground_);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, textLeft_, topBorder_, width, inset_ - 1);
        for (size_t i = 0; i < text_.size(); ++i) {
            const int top = topBorder_ + static_cast<int>(i) * rowHeight_;
            // Expand tabs against the same text origin even after horizontal scrolling.
            int x = textLeft_ - xOffset_;
            const int tabPixels = tabWidth_ * spaceWidth_;
            for (const auto& run : text_[i]) {
                SetTextColor(dc, styleForeground_[run.style]);
                size_t begin = 0;
                while (begin < run.text.size() && x < width) {
                    const size_t end = run.text.find(L'\t', begin);
                    const size_t n = (end == std::wstring::npos ? run.text.size() : end) - begin;
                    SIZE extent{};
                    GetTextExtentPoint32W(dc, run.text.data() + begin, static_cast<int>(n), &extent);
                    RECT row{x, top, width, top + rowHeight_};
                    DrawTextW(dc, run.text.data() + begin, static_cast<int>(n), &row,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    x += extent.cx;
                    if (end == std::wstring::npos) break;
                    x = textLeft_ - xOffset_ + ((x - textLeft_ + xOffset_) / tabPixels + 1) * tabPixels;
                    begin = end + 1;
                }
                if (x >= width) break;
            }
        }
        RestoreDC(dc, saved);
        SetDCBrushColor(dc, RGB((GetRValue(foreground_) + 3 * GetRValue(background_)) / 4,
            (GetGValue(foreground_) + 3 * GetGValue(background_)) / 4,
            (GetBValue(foreground_) + 3 * GetBValue(background_)) / 4));
        RECT border{0, inset_ - 1, width, inset_};
        FillRect(dc, &border, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (topBorder_ > 0) {
            RECT edge{0, 0, width, topBorder_};
            if (GetWindowLongPtrW(editor_, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) {
                // The native editor uses this sunken edge in light mode. Its measured
                // height also supplies the space above the first sticky source line.
                DrawEdge(dc, &edge, EDGE_SUNKEN, BF_TOP);
            } else {
                // Notepad++ uses a flat WS_BORDER in dark mode.
                FillRect(dc, &edge, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            }
        }
        BitBlt(windowDC, bar.left, bar.top, width, inset_, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
}

void StickyView::jump(size_t row) {
    // Revalidate against the current viewport/document before using a stored line number.
    const auto oldRows = lines_;
    update();
    if (oldRows != lines_ || row >= lines_.size()) return;
    const Line line = lines_[row];
    sci(SCI_ENSUREVISIBLE, line);
    sci(SCI_GOTOLINE, line);
    const Line display = static_cast<Line>(sci(SCI_VISIBLEFROMDOCLINE, line));
    if (display >= 0) sci(SCI_SETFIRSTVISIBLELINE, display);
    // Clicking is an explicit navigation action; only then activate the associated editor.
    SetFocus(editor_);
    request_();
}

LRESULT CALLBACK StickyView::subclass(HWND hwnd, UINT message, WPARAM w, LPARAM l,
                                     UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<StickyView*>(data);
    try { return self->dispatch(message, w, l); }
    catch (...) {
        // Never let allocation/GDI failures unwind into Notepad++'s window procedure.
        self->lines_.clear();
        self->text_.clear();
        self->setInset(0);
        return DefSubclassProc(hwnd, message, w, l);
    }
}

LRESULT CALLBACK StickyView::headerSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l,
                                           UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<StickyView*>(data);
    try {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            try { self->paint(dc); } catch (...) { EndPaint(hwnd, &ps); throw; }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINT: case WM_PRINTCLIENT:
            self->paint(reinterpret_cast<HDC>(w));
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            const int row = self->hitRow(l);
            if (row >= 0) self->jump(static_cast<size_t>(row));
            return 0;
        }
        case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            return SendMessageW(self->editor_, message, w, l);
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, headerSubclass, subclassId);
            self->header_ = nullptr;
            break;
        default: break;
        }
    } catch (...) { self->lines_.clear(); self->text_.clear(); }
    return DefSubclassProc(hwnd, message, w, l);
}

LRESULT StickyView::dispatch(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_WINDOWPOSCHANGING: {
        if (inLayout_) break;
        auto* position = reinterpret_cast<WINDOWPOS*>(l);
        const LONG oldWidth = allocation_.right - allocation_.left;
        const LONG oldHeight = allocation_.bottom - allocation_.top;
        if (!(position->flags & SWP_NOMOVE)) {
            allocation_.left = position->x;
            allocation_.top = position->y;
            allocation_.right = allocation_.left + oldWidth;
            allocation_.bottom = allocation_.top + oldHeight;
            position->y += inset_;
        }
        if (!(position->flags & SWP_NOSIZE)) {
            allocation_.right = allocation_.left + position->cx;
            allocation_.bottom = allocation_.top + position->cy;
            position->cy = std::max(0, position->cy - inset_);
            position->cx = std::max(0, position->cx - scrollWidth_);
        }
        break;
    }
    case WM_NCDESTROY: {
        HWND old = editor_;
        RemoveWindowSubclass(old, subclass, subclassId);
        editor_ = nullptr;
        inset_ = 0;
        scrollTracking_ = false;
        if (header_ && IsWindow(header_)) DestroyWindow(header_);
        header_ = nullptr;
        if (scrollHost_ && IsWindow(scrollHost_)) DestroyWindow(scrollHost_);
        scrollHost_ = scrollbar_ = nullptr;
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        if (numberFont_) DeleteObject(numberFont_);
        numberFont_ = nullptr;
        return DefSubclassProc(old, message, w, l);
    }
    case SCI_SETDOCPOINTER:
    case SCI_SETILEXER:
        invalidate();
        break;
    case SCI_STYLESETFORE: case SCI_STYLESETBACK: case SCI_STYLECLEARALL:
        textDirty_ = true;
        break;
    case SCI_SETVSCROLLBAR:
        originalVScroll_ = w != 0;
        appearanceDirty_ = true;
        if (request_) request_();
        return DefSubclassProc(editor_, message, FALSE, l);
    default: break;
    }
    const auto result = DefSubclassProc(editor_, message, w, l);
    if (message == WM_WINDOWPOSCHANGED && !inLayout_) { layoutHeader(); syncScrollbar(); }
    switch (message) {
    case WM_SIZE: case WM_DPICHANGED_AFTERPARENT: case WM_THEMECHANGED:
    case WM_SETTINGCHANGE: case WM_SETFONT: case WM_STYLECHANGED:
    case SCI_STYLESETFORE: case SCI_STYLESETBACK: case SCI_STYLECLEARALL:
    case SCI_STYLESETFONT: case SCI_STYLESETSIZE: case SCI_STYLESETSIZEFRACTIONAL:
    case SCI_STYLESETWEIGHT: case SCI_STYLESETBOLD: case SCI_STYLESETITALIC:
    case SCI_STYLESETCHARACTERSET: case SCI_SETZOOM:
    case SCI_SETMARGINWIDTHN: case SCI_SETMARGINTYPEN: case SCI_SETMARGINS:
    case SCI_SETMARGINLEFT: case SCI_SETMARGINRIGHT: case SCI_SETTABWIDTH:
    case SCI_SETEXTRAASCENT: case SCI_SETEXTRADESCENT:
        if (!inLayout_) appearanceChanged(message == WM_THEMECHANGED);
        break;
    default: break;
    }
    switch (message) {
    case WM_SIZE: case WM_SHOWWINDOW: case WM_DPICHANGED_AFTERPARENT:
    case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SETFONT:
    case WM_HSCROLL: case WM_VSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case SCI_SETDOCPOINTER: case SCI_SETILEXER: case SCI_SETWRAPMODE:
    case SCI_SETFIRSTVISIBLELINE: case SCI_SETXOFFSET:
    case SCI_SETFOLDEXPANDED: case SCI_TOGGLEFOLD: case SCI_FOLDLINE: case SCI_FOLDALL:
    case SCI_HIDELINES: case SCI_SHOWLINES: case SCI_SETPROPERTY:
    case SCI_STYLESETFORE: case SCI_STYLESETBACK: case SCI_STYLECLEARALL:
    case WM_STYLECHANGED:
        if (!inLayout_ && request_) request_();
        break;
    default: break;
    }
    return result;
}
}
