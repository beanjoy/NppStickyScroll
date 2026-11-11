// SPDX-License-Identifier: GPL-3.0-or-later
#include "StickyView.h"
#include "SciLexer.h"
#include <windowsx.h>
#include <chrono>
#include <cstring>

namespace sticky {
namespace {
constexpr UINT_PTR subclassId = 0x53544943;
constexpr size_t maxTextBytes = 4096;
constexpr Line maxLinesPerSlice = 16384;
}

LRESULT StickyView::sci(UINT message, WPARAM w, LPARAM l) const noexcept {
    return SendMessageW(editor_, message, w, l);
}

bool StickyView::attach(HWND editor, RequestUpdate request) {
    if (editor_) return true;
    editor_ = editor;
    request_ = request;
    direct_ = reinterpret_cast<SciFnDirect>(sci(SCI_GETDIRECTFUNCTION));
    directPointer_ = static_cast<sptr_t>(sci(SCI_GETDIRECTPOINTER));
    if (!SetWindowSubclass(editor_, subclass, subclassId, reinterpret_cast<DWORD_PTR>(this))) {
        editor_ = nullptr;
        return false;
    }
    return true;
}

void StickyView::detach() noexcept {
    if (editor_ && IsWindow(editor_)) {
        // Restore the original client geometry while our NCCALCSIZE handler still exists.
        lines_.clear();
        setInset(0);
        RemoveWindowSubclass(editor_, subclass, subclassId);
    }
    editor_ = nullptr;
    direct_ = nullptr;
    directPointer_ = 0;
    document_ = 0;
    index_.clear();
    text_.clear();
    if (font_) DeleteObject(font_);
    font_ = nullptr;
}

void StickyView::invalidate(Line from) noexcept {
    index_.invalidate(from);
    textDirty_ = true;
    // Existing rows must never remain clickable after an edit/document switch.
    lines_.clear();
    text_.clear();
    if (editor_ && inset_) RedrawWindow(editor_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}

void StickyView::modified(const SCNotification& n) {
    if (n.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
        const Line at = static_cast<Line>(sci(SCI_LINEFROMPOSITION, n.position));
        invalidate(std::max(Line{0}, at - 1));
    } else if (n.modificationType & SC_MOD_CHANGEFOLD) {
        // Fold notifications can arrive after a first paint or long after an edit.
        invalidate(std::max(Line{0}, n.line - 1));
    }
}

void StickyView::clear() {
    index_.clear();
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
    }
    if (!IsWindowVisible(editor_) || (lexer != SCLEX_JSON && lexer != SCLEX_XML) ||
        !sci(SCI_GETPROPERTYINT, reinterpret_cast<WPARAM>("fold"))) {
        clear();
        return false;
    }
    refreshAppearance();
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
    });
    // Keep at least one complete body line when the pane is extremely short.
    RECT window{};
    GetWindowRect(editor_, &window);
    const int availableRows = std::max(0, static_cast<int>(window.bottom - window.top -
        GetSystemMetricsForDpi(SM_CYHSCROLL, GetDpiForWindow(editor_)) - 1) / rowHeight_ - 1);
    if (rows.size() > static_cast<size_t>(availableRows))
        rows.erase(rows.begin(), rows.end() - availableRows);
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
    const int size = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(bytes), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(bytes), result.data(), size);
    for (auto& ch : result) if (ch < L' ' && ch != L'\t') ch = L' ';
    if (end - start > bytes) result += L"\u2026";
    return result;
}

void StickyView::refreshAppearance() {
    const int oldHeight = rowHeight_;
    const int oldLeft = textLeft_, oldOffset = xOffset_, oldTab = tabWidth_;
    const COLORREF oldFore = foreground_, oldBack = background_;
    rowHeight_ = std::max(1, static_cast<int>(sci(SCI_TEXTHEIGHT, 0)));
    foreground_ = static_cast<COLORREF>(sci(SCI_STYLEGETFORE, STYLE_DEFAULT));
    background_ = static_cast<COLORREF>(sci(SCI_STYLEGETBACK, STYLE_DEFAULT));
    textLeft_ = static_cast<int>(sci(SCI_GETMARGINLEFT));
    const int margins = static_cast<int>(sci(SCI_GETMARGINS));
    for (int m = 0; m < margins; ++m) textLeft_ += static_cast<int>(sci(SCI_GETMARGINWIDTHN, m));
    xOffset_ = static_cast<int>(sci(SCI_GETXOFFSET));
    tabWidth_ = std::clamp(static_cast<int>(sci(SCI_GETTABWIDTH)), 1, 256);
    spaceWidth_ = std::max(1, static_cast<int>(sci(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<LPARAM>(" "))));
    const auto nameLength = sci(SCI_STYLEGETFONT, STYLE_DEFAULT, 0);
    std::string name;
    if (nameLength > 0 && nameLength < 4096) {
        name.resize(static_cast<size_t>(nameLength) + 1, '\0');
        sci(SCI_STYLEGETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>(name.data()));
    }
    LOGFONTW spec{};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, spec.lfFaceName, LF_FACESIZE);
    const int points100 = static_cast<int>(sci(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT)) +
        static_cast<int>(sci(SCI_GETZOOM)) * 100;
    spec.lfHeight = -std::max(1, MulDiv(points100, static_cast<int>(GetDpiForWindow(editor_)), 7200));
    spec.lfWeight = static_cast<LONG>(sci(SCI_STYLEGETWEIGHT, STYLE_DEFAULT));
    spec.lfItalic = static_cast<BYTE>(sci(SCI_STYLEGETITALIC, STYLE_DEFAULT));
    spec.lfCharSet = static_cast<BYTE>(sci(SCI_STYLEGETCHARACTERSET, STYLE_DEFAULT));
    spec.lfQuality = CLEARTYPE_QUALITY;
    const bool fontChanged = !font_ || std::memcmp(&fontSpec_, &spec, sizeof(spec)) != 0;
    if (fontChanged) {
        HFONT replacement = CreateFontIndirectW(&spec);
        if (replacement) {
            if (font_) DeleteObject(font_);
            font_ = replacement;
            fontSpec_ = spec;
        }
    }
    if (fontChanged || oldHeight != rowHeight_ || oldFore != foreground_ || oldBack != background_ ||
        oldLeft != textLeft_ || oldOffset != xOffset_ || oldTab != tabWidth_)
        textDirty_ = true;
}

void StickyView::setRows(std::vector<Line> rows) {
    const bool changed = rows != lines_ || textDirty_;
    if (changed) {
        lines_ = std::move(rows);
        text_.clear();
        for (const Line line : lines_) text_.push_back(lineText(line));
        textDirty_ = false;
    }
    const int wanted = lines_.empty() ? 0 : static_cast<int>(lines_.size()) * rowHeight_ + 1;
    setInset(wanted);
    if (changed && inset_) RedrawWindow(editor_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}

void StickyView::setInset(int height) {
    if (inset_ == height || !editor_) return;
    inset_ = height;
    inLayout_ = true;
    SetWindowPos(editor_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    inLayout_ = false;
    RedrawWindow(editor_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    // Resizing at EOF can clamp the host's first visible line; recompute once after layout.
    if (request_) request_();
}

RECT StickyView::barRect() const noexcept {
    RECT window{}, client{};
    GetWindowRect(editor_, &window);
    GetClientRect(editor_, &client);
    POINT origin{};
    ClientToScreen(editor_, &origin);
    return {origin.x - window.left, origin.y - window.top - inset_,
        origin.x - window.left + client.right, origin.y - window.top};
}

int StickyView::hitRow(LPARAM screenPoint) const noexcept {
    if (!inset_) return -1;
    RECT window{};
    GetWindowRect(editor_, &window);
    POINT point{GET_X_LPARAM(screenPoint) - window.left, GET_Y_LPARAM(screenPoint) - window.top};
    const RECT rect = barRect();
    if (!PtInRect(&rect, point)) return -1;
    const int row = (point.y - rect.top) / rowHeight_;
    return row < static_cast<int>(lines_.size()) ? row : -1;
}

void StickyView::paint(HDC target) {
    if (!inset_ || !editor_) return;
    const RECT bar = barRect();
    const int width = bar.right - bar.left;
    if (width <= 0) return;
    HDC windowDC = target ? target : GetWindowDC(editor_);
    if (!windowDC) return;
    HDC dc = CreateCompatibleDC(windowDC);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDC, width, inset_);
    if (dc && bitmap) {
        const auto oldBitmap = SelectObject(dc, bitmap);
        const auto oldFont = SelectObject(dc, font_ ? font_ : GetStockObject(SYSTEM_FONT));
        RECT rect{0, 0, width, inset_};
        SetDCBrushColor(dc, background_);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetTextColor(dc, foreground_);
        SetBkMode(dc, TRANSPARENT);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, textLeft_, 0, width, inset_ - 1);
        for (size_t i = 0; i < text_.size(); ++i) {
            const int top = static_cast<int>(i) * rowHeight_;
            // Expand tabs against the same text origin even after horizontal scrolling.
            int x = textLeft_ - xOffset_;
            size_t begin = 0;
            const auto& text = text_[i];
            const int tabPixels = tabWidth_ * spaceWidth_;
            while (begin < text.size()) {
                const size_t end = text.find(L'\t', begin);
                const size_t n = (end == std::wstring::npos ? text.size() : end) - begin;
                SIZE extent{};
                GetTextExtentPoint32W(dc, text.data() + begin, static_cast<int>(n), &extent);
                RECT row{x, top, width, top + rowHeight_};
                DrawTextW(dc, text.data() + begin, static_cast<int>(n), &row,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                x += extent.cx;
                if (end == std::wstring::npos) break;
                x = textLeft_ - xOffset_ + ((x - textLeft_ + xOffset_) / tabPixels + 1) * tabPixels;
                begin = end + 1;
                if (x >= width) break;
            }
        }
        RestoreDC(dc, saved);
        SetDCBrushColor(dc, RGB((GetRValue(foreground_) + 3 * GetRValue(background_)) / 4,
            (GetGValue(foreground_) + 3 * GetGValue(background_)) / 4,
            (GetBValue(foreground_) + 3 * GetBValue(background_)) / 4));
        RECT border{0, inset_ - 1, width, inset_};
        FillRect(dc, &border, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        BitBlt(windowDC, bar.left, bar.top, width, inset_, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
    }
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    if (!target) ReleaseDC(editor_, windowDC);
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

LRESULT StickyView::dispatch(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_NCCALCSIZE: {
        const LRESULT result = DefSubclassProc(editor_, message, w, l);
        auto* rect = w ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] : reinterpret_cast<RECT*>(l);
        rect->top = std::min(rect->bottom, rect->top + inset_);
        return inset_ ? 0 : result;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        const auto result = DefSubclassProc(editor_, message, w, l);
        paint();
        return result;
    }
    case WM_NCHITTEST:
        if (hitRow(l) >= 0) return HTBORDER;
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK: {
        const int row = hitRow(l);
        if (row >= 0) { jump(static_cast<size_t>(row)); return 0; }
        break;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTBORDER && inset_) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_PRINT:
        { const auto result = DefSubclassProc(editor_, message, w, l);
          if (l & PRF_NONCLIENT) paint(reinterpret_cast<HDC>(w));
          return result; }
    case WM_NCDESTROY: {
        HWND old = editor_;
        RemoveWindowSubclass(old, subclass, subclassId);
        editor_ = nullptr;
        inset_ = 0;
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        return DefSubclassProc(old, message, w, l);
    }
    case SCI_SETDOCPOINTER:
    case SCI_SETILEXER:
        invalidate();
        break;
    default: break;
    }
    const auto result = DefSubclassProc(editor_, message, w, l);
    switch (message) {
    case WM_SIZE: case WM_SHOWWINDOW: case WM_DPICHANGED_AFTERPARENT:
    case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SETFONT:
    case WM_HSCROLL: case WM_VSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case SCI_SETDOCPOINTER: case SCI_SETILEXER: case SCI_SETWRAPMODE:
    case SCI_SETFIRSTVISIBLELINE: case SCI_SETXOFFSET:
    case SCI_SETFOLDEXPANDED: case SCI_TOGGLEFOLD: case SCI_FOLDLINE: case SCI_FOLDALL:
    case SCI_HIDELINES: case SCI_SHOWLINES: case SCI_SETPROPERTY:
        if (!inLayout_ && request_) request_();
        break;
    default: break;
    }
    return result;
}
}
