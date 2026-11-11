// Test-only code: never linked into the delivered DLL. Runs against real Notepad++/Lexilla.
// Requires an explicit NPP_STICKY_TEST_DIR under the project's .tools directory.
#include <windows.h>
#include "PluginInterface.h"
#include "menuCmdID.h"
#include "StickyView.h"
#include "resource.h"
#include "Version.h"
#include "SciLexer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {
NppData npp;
std::array<sticky::StickyView, 2>* views;
void (*togglePlugin)();
HWND runner;
std::ofstream report;
int stage = 0;
int failures = 0;
int checks = 0;
std::chrono::steady_clock::time_point coldStart;
RECT originalWindow{}, originalClient{};
RECT hostWindow{};
RECT dragEditor{}, dragScrollbar{};
int dragHeight = 0;
SCROLLINFO dragInfo{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE};
std::wstring resultDirectory;
std::vector<std::function<void()>> steps;
std::vector<sticky::Line> varyingTargets;
unsigned long long burstUpdates = 0;
unsigned long long lazyClears = 0, lazyBlankPaints = 0, lazyHeightChanges = 0, lazyUpdates = 0;
int lazyScrollStep = 0;
int lazyIdleStep = 0;

auto& mainView() { return (*views)[0]; }
LRESULT sci(UINT msg, WPARAM w = 0, LPARAM l = 0) { return SendMessageW(npp._scintillaMainHandle, msg, w, l); }
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    report << (ok ? "PASS " : "FAIL ") << name << '\n';
    report.flush();
}
void expect(const std::vector<sticky::Line>& lines, const char* name) {
    mainView().update();
    check(mainView().testLines() == lines, name);
    if (mainView().testLines() != lines) {
        report << "  actual:";
        for (auto line : mainView().testLines()) report << ' ' << line;
        report << "; first=" << sci(SCI_GETFIRSTVISIBLELINE) << "; doc=" << sci(SCI_DOCLINEFROMVISIBLE, sci(SCI_GETFIRSTVISIBLELINE))
            << "; styled=" << sci(SCI_GETENDSTYLED) << "; len=" << sci(SCI_GETLENGTH) << "; lexer=" << sci(SCI_GETLEXER)
            << "; fold=" << sci(SCI_GETPROPERTYINT, reinterpret_cast<WPARAM>("fold")) << '\n';
        for (int i = 0; i < 5; ++i) report << "  level[" << i << "]=" << std::hex << sci(SCI_GETFOLDLEVEL, i) << std::dec << '\n';
    }
}
std::string json(int lines = 150) {
    std::string text = "{\n  \"DeviceLog\": {\n    \"Windows\": [\n";
    for (int i = 0; i < lines; ++i) text += "      " + std::to_string(i) + ",\n";
    text += "      0\n    ],\n    \"After\": 1\n  }\n}\n";
    for (int i = 0; i < 70; ++i) text += '\n'; // Make the closing lines independently scrollable.
    return text;
}

std::string varyingJson() {
    varyingTargets.clear();
    std::string text = "[\n";
    sticky::Line line = 1;
    const auto append = [&](const std::string& value) { text += value + '\n'; ++line; };
    for (int group = 0; group < 256; ++group) {
        const int depth = 1 + group % 8;
        append("{");
        for (int d = 1; d < depth; ++d) append("\"nested\": {");
        append("\"values\": [");
        varyingTargets.push_back(line + 200);
        for (int i = 0; i < 600; ++i) append("123,");
        append("0");
        append("]");
        for (int d = 1; d < depth; ++d) append("}");
        append(group == 255 ? "}" : "},");
    }
    append("]");
    return text;
}
void load(const std::string& text, LangType language) {
    SendMessageW(npp._nppHandle, WM_COMMAND, IDM_FILE_NEW, 0);
    sci(SCI_SETCODEPAGE, SC_CP_UTF8);
    sci(SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
    const auto buffer = SendMessageW(npp._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0);
    SendMessageW(npp._nppHandle, NPPM_SETBUFFERLANGTYPE, buffer, language);
    sci(SCI_COLOURISE, 0, -1);
    sci(SCI_SETSAVEPOINT);
}
void at(sticky::Line docLine) { sci(SCI_SETFIRSTVISIBLELINE, sci(SCI_VISIBLEFROMDOCLINE, docLine)); }
void savepoint() { sci(SCI_SETSAVEPOINT); }

void changeSettings(int maxRows, bool nearest, bool cancel = false) {
    static int requestedRows;
    static bool requestedNearest, shouldCancel, dialogHandled;
    requestedRows = maxRows;
    requestedNearest = nearest;
    shouldCancel = cancel;
    dialogHandled = false;
    SetTimer(runner, 2, 30, [](HWND hwnd, UINT, UINT_PTR timer, DWORD) {
        HWND dialog = GetLastActivePopup(npp._nppHandle);
        wchar_t caption[64]{};
        GetWindowTextW(dialog, caption, 64);
        if (wcscmp(caption, L"Sticky Scroll 设置") != 0) return;
        KillTimer(hwnd, timer);
        SetDlgItemInt(dialog, IDC_MAX_ROWS, requestedRows, FALSE);
        CheckRadioButton(dialog, IDC_NEAREST, IDC_OUTERMOST, requestedNearest ? IDC_NEAREST : IDC_OUTERMOST);
        dialogHandled = true;
        SendMessageW(dialog, WM_COMMAND, shouldCancel ? IDCANCEL : IDOK, 0);
    });
    int count = 0;
    FuncItem* commands = getFuncsArray(&count);
    if (count >= 2) commands[1]._pFunc();
    KillTimer(runner, 2);
    check(dialogHandled, cancel ? "settings dialog cancels" : "settings dialog accepts configured rows and preference");
}

RECT scrollbarRect() {
    SCROLLBARINFO info{sizeof(info)};
    check(GetScrollBarInfo(mainView().testScrollbar(), OBJID_CLIENT, &info) != FALSE, "independent Win32 scrollbar geometry available");
    return info.rcScrollBar;
}

void verifyVersionResource() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(GetModuleHandleW(L"NppStickyScroll.dll"), path, MAX_PATH);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    const std::array<WORD, 4> expected{NPP_STICKY_VERSION_PARTS};
    const bool read = size && GetFileVersionInfoW(path, 0, size, data.data()) &&
        VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length);
    check(read && length >= sizeof(*fixed) && fixed->dwFileVersionMS == static_cast<DWORD>(MAKELONG(expected[1], expected[0])) &&
        fixed->dwFileVersionLS == static_cast<DWORD>(MAKELONG(expected[3], expected[2])), "DLL has standard Windows version matching CMake");
}

std::wstring childText(HWND parent) {
    std::wstring text;
    EnumChildWindows(parent, [](HWND child, LPARAM result) -> BOOL {
        const int length = GetWindowTextLengthW(child);
        if (length > 0 && length < 32768) {
            std::wstring value(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(child, value.data(), length + 1);
            value.resize(length);
            *reinterpret_cast<std::wstring*>(result) += value;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&text));
    return text;
}

void verifyAboutDialog() {
    static bool checked;
    checked = false;
    SetTimer(runner, 3, 30, [](HWND hwnd, UINT, UINT_PTR timer, DWORD) {
        HWND dialog = GetLastActivePopup(npp._nppHandle);
        wchar_t title[64]{};
        GetWindowTextW(dialog, title, 64);
        if (wcscmp(title, L"关于 NppStickyScroll") != 0) return;
        KillTimer(hwnd, timer);
        checked = true;
        check(childText(dialog).find(NPP_STICKY_VERSION_W) != std::wstring::npos, "About menu displays the actual build version");
        PostMessageW(dialog, WM_CLOSE, 0, 0);
    });
    int count = 0;
    auto* commands = getFuncsArray(&count);
    if (count == 3) commands[2]._pFunc();
    KillTimer(runner, 3);
    check(checked, "About menu opens a Notepad++ owned dialog");
}

void verifyHostVersionDisplay() {
    SendMessageW(npp._nppHandle, WM_COMMAND, IDM_DEBUGINFO, 0);
    HWND dialog = GetLastActivePopup(npp._nppHandle);
    const auto text = childText(dialog);
    // Notepad++ omits trailing zero components when formatting version resources.
    std::wstring version = NPP_STICKY_VERSION_W;
    while (version.size() > 2 && version.compare(version.size() - 2, 2, L".0") == 0) version.resize(version.size() - 2);
    check(dialog != npp._nppHandle && text.find(L"NppStickyScroll (" + version + L")") != std::wstring::npos,
        "Notepad++ Debug Info displays the version from the DLL resource");
    if (dialog != npp._nppHandle) SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
}

void verifyNativeDrag() {
    // Exercise the real SCROLLBAR control's modal tracking loop with window input
    // messages. This does not move the desktop pointer or use injected device input.
    static HWND bar;
    static int phase, x, targetY;
    static bool captured, moved;
    bar = mainView().testScrollbar();
    phase = 0;
    captured = moved = false;
    SCROLLBARINFO info{sizeof(info)};
    GetScrollBarInfo(bar, OBJID_CLIENT, &info);
    x = (info.rcScrollBar.right - info.rcScrollBar.left) / 2;
    const int thumbY = (info.xyThumbTop + info.xyThumbBottom) / 2;
    targetY = (info.rcScrollBar.bottom - info.rcScrollBar.top) / 2;
    SCROLLINFO range{sizeof(range), SIF_ALL};
    GetScrollInfo(bar, SB_CTL, &range);
    report << "METRIC native drag start enabled=" << IsWindowEnabled(bar) << " visible=" << IsWindowVisible(bar)
        << " rect=" << info.rcScrollBar.left << ',' << info.rcScrollBar.top << ',' << info.rcScrollBar.right << ',' << info.rcScrollBar.bottom
        << " thumb=" << info.xyThumbTop << ',' << info.xyThumbBottom << " range=" << range.nMax
        << " page=" << range.nPage << " pos=" << range.nPos << '\n';
    SetTimer(runner, 4, 40, [](HWND hwnd, UINT, UINT_PTR timer, DWORD) {
        if (phase++ == 0) {
            captured = GetCapture() == bar;
            PostMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, targetY));
        } else {
            moved = sci(SCI_GETFIRSTVISIBLELINE) > 65535 && sci(SCI_GETFIRSTVISIBLELINE) < 100000;
            report << "METRIC native modal drag position=" << sci(SCI_GETFIRSTVISIBLELINE)
                << " capture=" << captured << '\n';
            PostMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(x, targetY));
            KillTimer(hwnd, timer);
        }
    });
    SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, thumbY));
    report << "METRIC native down returned phase=" << phase << " captured=" << (GetCapture() == bar) << '\n';
    KillTimer(runner, 4);
    if (!phase && GetCapture() != bar) {
        report << "SKIP native modal drag: synthetic button message did not start capture; physical input still required\n";
        return;
    }
    check(captured && moved, "native scrollbar modal drag preserves full position beyond 65535 lines");
    check(GetCapture() != bar && !mainView().testTracking(), "native drag releases capture and frozen mapping");
}

bool hasStyledToken(size_t row, const wchar_t* token, unsigned char style) {
    const auto& lines = mainView().testStyledLines();
    if (row >= lines.size()) return false;
    for (const auto& run : lines[row])
        if (run.style == style && run.text.find(token) != std::wstring::npos) return true;
    return false;
}

void captureEditor(const wchar_t* fileName = L"editor-render.bmp") {
    // Exercise the real editor's WM_PRINT rendering path, independent of desktop capture tools.
    RECT bounds{}; GetWindowRect(mainView().testHeader(), &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) { check(false, "capture resources"); return; }
    const auto previous = SelectObject(dc, bitmap);
    SendMessageW(mainView().testHeader(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream image(resultDirectory + L"\\" + fileName, std::ios::binary);
    image.write(reinterpret_cast<const char*>(&file), sizeof(file));
    image.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    image.write(static_cast<const char*>(pixels), width * height * 4);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TIMER) {
        KillTimer(hwnd, 1);
        if (stage < static_cast<int>(steps.size())) {
            try { steps[static_cast<size_t>(stage++)](); }
            catch (...) { check(false, "unexpected exception"); stage = static_cast<int>(steps.size()); }
            SetTimer(hwnd, 1, 200, nullptr);
        } else {
            report << "RESULT " << checks << " checks, " << failures << " failures\n";
            report.flush();
            report.close();
            DestroyWindow(hwnd);
            runner = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
}

void startHostTests(NppData host, std::array<sticky::StickyView, 2>* target, void (*toggle)()) {
    wchar_t directory[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"NPP_STICKY_TEST_DIR", directory, 32768);
    if (!length || length >= 32768 || std::wstring(directory).find(L"\\.tools\\") == std::wstring::npos) return;
    npp = host;
    views = target;
    togglePlugin = toggle;
    resultDirectory = directory;
    report.open(std::wstring(directory) + L"\\host-results.txt", std::ios::trunc);
    report << "Real Notepad++ host tests. Version=" << std::hex << SendMessageW(npp._nppHandle, NPPM_GETNPPVERSION, TRUE, 0) << std::dec << '\n';
    steps = {
        [] { verifyVersionResource(); verifyAboutDialog(); verifyHostVersionDisplay(); load(json(), L_JSON); },
        [] { expect({}, "JSON at document start hides bar"); GetWindowRect(npp._scintillaMainHandle, &originalWindow); GetClientRect(npp._scintillaMainHandle, &originalClient); at(40); },
        [] {
            expect({0, 1, 2}, "JSON scroll uses viewport with caret still at start");
            check(sci(SCI_GETCURRENTPOS) == 0, "scrolling does not move caret");
            RECT rect{}, client{}; GetWindowRect(npp._scintillaMainHandle, &rect); GetClientRect(npp._scintillaMainHandle, &client);
            POINT point{}; ClientToScreen(npp._scintillaMainHandle, &point);
            RECT bar{}; GetWindowRect(mainView().testHeader(), &bar);
            check(rect.top == originalWindow.top + mainView().testHeight() && rect.bottom == originalWindow.bottom &&
                rect.left == originalWindow.left && rect.right == originalWindow.right, "editor occupies body portion of host allocation");
            check(bar.top == originalWindow.top && bar.bottom == rect.top && bar.right == rect.right &&
                originalClient.bottom - client.bottom == mainView().testHeight(), "header and body are adjacent with no overlap");
            const RECT scrollbar = scrollbarRect();
            check(scrollbar.left >= bar.right && scrollbar.top < bar.bottom,
                "independent scrollbar stays to the right of header and body");
            check(!sci(SCI_GETVSCROLLBAR) && !(GetWindowLongPtrW(npp._scintillaMainHandle, GWL_STYLE) & WS_VSCROLL),
                "editor native vertical scrollbar is disabled while adapter owns it");
            check((GetWindowLongPtrW(mainView().testHeader(), GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)) == 0, "sticky header has no scrollbar");
            check(mainView().testNumberWidth() == sci(SCI_GETMARGINWIDTHN, 0), "source line number gutter matches host margin");
            check(hasStyledToken(1, L"DeviceLog", SCE_JSON_PROPERTYNAME) && hasStyledToken(1, L"{", SCE_JSON_OPERATOR),
                "JSON fields and punctuation retain their source lexer styles");
            check(mainView().testStyleColor(SCE_JSON_PROPERTYNAME) == sci(SCI_STYLEGETFORE, SCE_JSON_PROPERTYNAME),
                "JSON field foreground equals the editor color");
            check(mainView().testTopBorder() == point.y - rect.top && mainView().testTopBorder() > 0,
                "header restores measured native editor top border thickness");
            captureEditor(L"json-render.bmp");
            check(GetFocus() == npp._scintillaMainHandle, "updates preserve input focus");
            at(154);
        },
        [] { expect({0, 1}, "JSON closing bracket excludes completed array"); at(156); },
        [] { expect({0}, "JSON closing object excludes completed DeviceLog"); at(157); },
        [] { expect({}, "JSON closing root hides all rows"); at(40); },
        [] {
            mainView().update();
            check(IsWindowVisible(mainView().testHeader()) != FALSE, "sticky row has independent child window hit target");
            RECT bar{}; GetWindowRect(mainView().testHeader(), &bar);
            check(SendMessageW(mainView().testHeader(), WM_NCHITTEST, 0, MAKELPARAM(bar.left + 15, bar.top + 4)) == HTCLIENT,
                "physical pointer targets header instead of falling through STATIC");
            SendMessageW(mainView().testHeader(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 4));
        },
        [] { check(sci(SCI_LINEFROMPOSITION, sci(SCI_GETCURRENTPOS)) == 0 && sci(SCI_DOCLINEFROMVISIBLE, sci(SCI_GETFIRSTVISIBLELINE)) == 0, "click navigation reaches exact source line"); expect({}, "clicked source is not duplicated"); at(40); },
        [] { sci(SCI_INSERTTEXT, 0, reinterpret_cast<LPARAM>("\n\n")); sci(SCI_COLOURISE, 0, -1); at(42); },
        [] { expect({2, 3, 4}, "insert before ancestors invalidates all source line numbers"); sci(SCI_UNDO); sci(SCI_COLOURISE, 0, -1); at(40); savepoint(); },
        [] { expect({0, 1, 2}, "undo restores ancestor lines"); sci(SCI_FOLDLINE, 2, SC_FOLDACTION_CONTRACT); at(155); },
        [] { expect({0, 1}, "collapsed array does not leak stale parents"); sci(SCI_FOLDLINE, 2, SC_FOLDACTION_EXPAND); at(40); },
        [] { expect({0, 1, 2}, "expanding fold restores ancestry"); sci(SCI_SETXOFFSET, 80); },
        [] { expect({0, 1, 2}, "horizontal scrolling preserves parent identity"); check(mainView().testXOffset() == sci(SCI_GETXOFFSET), "sticky drawing uses host horizontal offset"); sci(SCI_SETXOFFSET, 0); sci(SCI_SETZOOM, 4); },
        [] { expect({0, 1, 2}, "zoom preserves ancestry"); check(mainView().testHeight() == mainView().testTopBorder() + 3 * sci(SCI_TEXTHEIGHT, 0) + 1, "sticky height follows host zoom"); sci(SCI_SETZOOM, 0); togglePlugin(); },
        [] { RECT client{}; GetClientRect(npp._scintillaMainHandle, &client); check(mainView().window() == nullptr && client.bottom == originalClient.bottom && sci(SCI_GETVSCROLLBAR), "disable restores full client height and original vertical scrollbar"); togglePlugin(); },
        [] { expect({0, 1, 2}, "reenable restores sticky rows"); load("", L_JSON); },
        [] { expect({}, "empty file hides bar"); load("plain\ntext\n", L_TEXT); },
        [] { expect({}, "unsupported language hides bar");
            std::string text = "<DeviceLog>\n  <Windows>\n";
            for (int i = 0; i < 150; ++i) text += "    <Value>42</Value>\n";
            text += "  </Windows>\n  <After>true</After>\n</DeviceLog>\n";
            for (int i = 0; i < 70; ++i) text += '\n';
            load(text, L_XML); at(40);
        },
        [] { expect({0, 1}, "XML ancestors from built-in lexer");
            check(hasStyledToken(0, L"DeviceLog", SCE_H_TAG), "XML element names retain source lexer styles");
            check(mainView().testStyleColor(SCE_H_TAG) == sci(SCI_STYLEGETFORE, SCE_H_TAG), "XML tag foreground equals the editor color");
            captureEditor(L"xml-render.bmp");
            const auto nextLevel = sci(SCI_GETFOLDLEVEL, 41);
            sci(SCI_SETFOLDLEVEL, 41, nextLevel ^ SC_FOLDLEVELWHITEFLAG);
            check(mainView().testLines().empty(), "XML lookahead fold change still invalidates current context");
            sci(SCI_SETFOLDLEVEL, 41, nextLevel);
            expect({0, 1}, "XML lookahead invalidation recovers correct parents");
            at(152);
        },
        [] { expect({0}, "XML closing tag excludes completed Windows"); at(154); },
        [] { expect({}, "XML root closing tag hides bar");
            std::string text = "<Root>\n  <Multiline\n    attribute=\"value\">\n";
            for (int i = 0; i < 80; ++i) text += "    <Empty />\n";
            text += "  </Multiline>\n</Root>\n";
            load(text, L_XML); at(40);
        },
        [] { expect({0}, "ambiguous multiline XML fold header is safely omitted");
            std::string text = "<Root>\n  <!--\n";
            for (int i = 0; i < 80; ++i) text += "  comment\n";
            text += "  -->\n</Root>\n";
            load(text, L_XML); at(40);
        },
        [] { expect({0}, "XML comment folding is not shown as element ancestry");
            std::string text = "{\n";
            for (int i = 0; i < 10; ++i) text += "\"level\": {\n";
            for (int i = 0; i < 80; ++i) text += "\"value\": 1,\n";
            text += "\"last\": 1\n";
            for (int i = 0; i < 11; ++i) text += "}\n";
            load(text, L_JSON); at(40);
        },
        [] { expect({6, 7, 8, 9, 10}, "deep nesting keeps nearest five in outer to inner order"); changeSettings(3, false); },
        [] { expect({0, 1, 2}, "configured outermost three appear in source order"); changeSettings(8, true); },
        [] { expect({3, 4, 5, 6, 7, 8, 9, 10}, "configured nearest eight exceed previous fixed limit"); changeSettings(2, false, true); },
        [] { expect({3, 4, 5, 6, 7, 8, 9, 10}, "cancel leaves settings unchanged");
            const auto path = resultDirectory + L"\\plugins\\Config\\NppStickyScroll.ini";
            check(GetPrivateProfileIntW(L"StickyScroll", L"MaxRows", 0, path.c_str()) == 8 &&
                GetPrivateProfileIntW(L"StickyScroll", L"KeepNearest", 0, path.c_str()) == 1, "confirmed settings persist in portable INI");
            togglePlugin(); togglePlugin();
        },
        [] { expect({3, 4, 5, 6, 7, 8, 9, 10}, "settings survive disable and reenable"); changeSettings(5, true);
            GetWindowRect(npp._scintillaMainHandle, &dragEditor);
            dragScrollbar = scrollbarRect();
            dragHeight = mainView().testHeight();
            GetScrollInfo(mainView().testScrollbar(), SB_CTL, &dragInfo);
            SendMessageW(mainView().testScrollHost(), WM_VSCROLL, SB_THUMBTRACK, reinterpret_cast<LPARAM>(mainView().testScrollbar()));
            at(0);
        },
        [] { expect({}, "drag across document start hides parents immediately");
            RECT rect{}; GetWindowRect(npp._scintillaMainHandle, &rect);
            RECT scroll = scrollbarRect();
            check(mainView().testTracking() && mainView().testHeight() == 0 && rect.top == dragEditor.top - dragHeight &&
                EqualRect(&scroll, &dragScrollbar), "header shrinks during drag while track geometry stays fixed");
            SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
            GetScrollInfo(mainView().testScrollbar(), SB_CTL, &info);
            check(info.nPage == dragInfo.nPage && info.nMax == dragInfo.nMax,
                "thumb mapping retains range and page despite live body resize");
            at(3);
        },
        [] { expect({0, 1, 2}, "parents continue updating during thumb tracking");
            const auto scroll = scrollbarRect();
            check(mainView().testHeight() == mainView().testTopBorder() + 3 * sci(SCI_TEXTHEIGHT, 0) + 1 &&
                EqualRect(&scroll, &dragScrollbar), "header grows during drag while track stays fixed");
            SendMessageW(mainView().testScrollHost(), WM_VSCROLL, SB_ENDSCROLL, reinterpret_cast<LPARAM>(mainView().testScrollbar()));
        },
        [] { expect({0, 1, 2}, "ending drag preserves current context");
            check(!mainView().testTracking() && mainView().testHeight() == mainView().testTopBorder() + 3 * sci(SCI_TEXTHEIGHT, 0) + 1, "release needs no deferred header resize");
            at(0);
        },
        [] { expect({}, "header hidden before drag starting at document top");
            SendMessageW(mainView().testScrollHost(), WM_VSCROLL, SB_THUMBTRACK, reinterpret_cast<LPARAM>(mainView().testScrollbar())); at(40);
        },
        [] { expect({6, 7, 8, 9, 10}, "drag starting without header displays parents before release");
            SendMessageW(mainView().testScrollHost(), WM_VSCROLL, SB_ENDSCROLL, reinterpret_cast<LPARAM>(mainView().testScrollbar()));
        },
        [] { expect({6, 7, 8, 9, 10}, "release restores nearest parents after initially hidden header");
            std::string text = "{\n  \"" + std::string(6000, 'x') + "\": {\n";
            for (int i = 0; i < 150; ++i) text += "    \"value\": 1,\n";
            text += "    \"last\": 1\n  }\n}\n";
            load(text, L_JSON); sci(SCI_SETWRAPMODE, SC_WRAP_WORD); at(0);
        },
        [] { check(sci(SCI_WRAPCOUNT, 1) > 1, "host actually wraps long parent line"); sci(SCI_SETFIRSTVISIBLELINE, sci(SCI_VISIBLEFROMDOCLINE, 1) + 1); },
        [] { expect({0}, "partly visible wrapped header is not duplicated"); at(50); },
        [] { expect({0, 1}, "offscreen long wrapped header appears safely"); sci(SCI_SETWRAPMODE, SC_WRAP_NONE); load(json(), L_JSON); at(40); },
        [] { SendMessageW(npp._nppHandle, WM_COMMAND, IDM_VIEW_CLONE_TO_ANOTHER_VIEW, 0); },
        [] { at(40); auto& second = (*views)[1]; second.sci(SCI_SETFIRSTVISIBLELINE, second.sci(SCI_VISIBLEFROMDOCLINE, 155)); },
        [] { expect({0, 1, 2}, "main split view retains its own scroll context"); (*views)[1].update(); check((*views)[1].testLines() == std::vector<sticky::Line>({0, 1}), "second split view has independent context");
            SendMessageW(npp._nppHandle, WM_COMMAND, IDM_FILE_CLOSE, 0);
        },
        [] { (*views)[1].update(); check((*views)[1].testHeight() == 0, "closing second view leaves no sticky region"); expect({0, 1, 2}, "remaining view survives second-view close"); load(json(150000), L_JSON); },
        [] { at(140000); coldStart = std::chrono::steady_clock::now(); },
        [] {
            if (mainView().testLines() != std::vector<sticky::Line>({0, 1, 2}) &&
                std::chrono::steady_clock::now() - coldStart < std::chrono::seconds(8)) { --stage; return; }
            const auto coldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - coldStart).count();
            expect({0, 1, 2}, "150000-line cold jump eventually resolves ancestors");
            report << "METRIC cold jump observed after " << coldMs << " ms (200 ms observation granularity)\n";
            std::vector<double> times;
            const auto appearanceBefore = mainView().testAppearanceRefreshes;
            for (int i = 0; i < 1000; ++i) {
                const auto start = std::chrono::steady_clock::now();
                at(140000 - i);
                mainView().update();
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            report << "METRIC 1000 cached scroll+update calls median=" << times[500] << " ms p95=" << times[950] << " ms max=" << times.back() << " ms\n";
            check(times[950] < 16.0, "cached large-file update p95 below one 60 Hz frame");
            check(mainView().testAppearanceRefreshes == appearanceBefore, "1000 scroll updates reuse font and margin cache");
            std::vector<double> baselineTimes;
            togglePlugin();
            for (int i = 0; i < 1000; ++i) {
                const auto start = std::chrono::steady_clock::now();
                at(140000 - i);
                baselineTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(baselineTimes.begin(), baselineTimes.end());
            report << "METRIC disabled baseline 1000 scroll calls median=" << baselineTimes[500]
                << " ms p95=" << baselineTimes[950] << " ms max=" << baselineTimes.back() << " ms\n";
            togglePlugin();
            // Native modal drag must use the 32-bit nTrackPos, never HIWORD.
            mainView().update();
            SCROLLINFO position{sizeof(position), SIF_POS};
            GetScrollInfo(mainView().testScrollbar(), SB_CTL, &position);
            check(position.nPos == sci(SCI_GETFIRSTVISIBLELINE) && position.nPos > 65535,
                "independent scrollbar stores positions beyond 65535 without truncation");
            verifyNativeDrag();
            check(sci(SCI_GETMODIFY) == 0, "sticky operations never modify document");
            sci(SCI_INSERTTEXT, 0, reinterpret_cast<LPARAM>("\n"));
            sci(SCI_COLOURISE, 0, -1);
            at(140001);
            coldStart = std::chrono::steady_clock::now();
        },
        [] {
            if (mainView().testLines() != std::vector<sticky::Line>({1, 2, 3}) &&
                std::chrono::steady_clock::now() - coldStart < std::chrono::seconds(8)) { --stage; return; }
            expect({1, 2, 3}, "large-file edit rebuilds source lines without stale cache");
            savepoint(); at(0);
        },
        [] { expect({}, "large file scroll back to start hides bar");
            // Leave a small clean document for a separate visual inspection.
            load(json(), L_JSON); at(40);
        },
        [] {
            GetWindowRect(npp._nppHandle, &hostWindow);
            SetWindowPos(npp._nppHandle, nullptr, 0, 0, 760, 480, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        },
        [] { expect({0, 1, 2}, "window resize keeps correct ancestors");
            check(mainView().testHeight() == mainView().testTopBorder() + 3 * sci(SCI_TEXTHEIGHT, 0) + 1, "resize keeps required sticky height");
            RECT body{}, bar{};
            GetWindowRect(npp._scintillaMainHandle, &body);
            GetWindowRect(mainView().testHeader(), &bar);
            RECT track{}; GetWindowRect(mainView().testScrollHost(), &track);
            RECT allocation{body.left, bar.top, track.right, body.bottom};
            MapWindowPoints(nullptr, GetParent(npp._scintillaMainHandle), reinterpret_cast<POINT*>(&allocation), 2);
            for (int i = 0; i < 20; ++i) {
                SetWindowPos(npp._scintillaMainHandle, nullptr, allocation.left, allocation.top,
                    allocation.right - allocation.left, allocation.bottom - allocation.top, SWP_NOZORDER | SWP_NOACTIVATE);
                mainView().update();
            }
            RECT after{}; GetWindowRect(npp._scintillaMainHandle, &after);
            check(EqualRect(&body, &after), "repeated host layout does not accumulate sticky inset");
            SetWindowPos(npp._nppHandle, nullptr, 0, 0, hostWindow.right - hostWindow.left, hostWindow.bottom - hostWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            sci(SCI_STYLESETFORE, STYLE_DEFAULT, RGB(220, 220, 220));
            sci(SCI_STYLESETBACK, STYLE_DEFAULT, RGB(30, 30, 30));
            sci(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>("Consolas"));
            sci(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, 1400);
            mainView().update();
        },
        [] {
            expect({0, 1, 2}, "theme and font changes keep correct ancestors");
            check(mainView().testForeground() == RGB(220, 220, 220) && mainView().testBackground() == RGB(30, 30, 30), "sticky colors follow editor theme styles");
            const auto font = mainView().testFont();
            check(wcscmp(font.lfFaceName, L"Consolas") == 0 && font.lfHeight == -MulDiv(1400, GetDpiForWindow(npp._scintillaMainHandle), 7200), "font family and fractional size use current window DPI");
            sci(SCI_SETSEL, sci(SCI_POSITIONFROMLINE, 40), sci(SCI_POSITIONFROMLINE, 42));
            mainView().update();
            check(sci(SCI_GETSELECTIONSTART) == sci(SCI_POSITIONFROMLINE, 40) && sci(SCI_GETSELECTIONEND) == sci(SCI_POSITIONFROMLINE, 42), "sticky update preserves body text selection");
            sci(SCI_SETPROPERTY, reinterpret_cast<WPARAM>("fold"), reinterpret_cast<LPARAM>("0"));
        },
        [] { expect({}, "disabled fold information safely hides bar");
            sci(SCI_SETPROPERTY, reinterpret_cast<WPARAM>("fold"), reinterpret_cast<LPARAM>("1"));
            sci(SCI_COLOURISE, 0, -1); at(40);
        },
        [] { expect({0, 1, 2}, "restored fold information recovers bar");
            const auto before = sci(SCI_GETFIRSTVISIBLELINE);
            SendMessageW(mainView().testScrollHost(), WM_VSCROLL, SB_LINEDOWN, reinterpret_cast<LPARAM>(mainView().testScrollbar()));
            check(sci(SCI_GETFIRSTVISIBLELINE) == before + 1, "independent scrollbar arrow uses editor display lines");
            SendMessageW(mainView().testScrollHost(), WM_VSCROLL, SB_PAGEUP, reinterpret_cast<LPARAM>(mainView().testScrollbar()));
            check(sci(SCI_GETFIRSTVISIBLELINE) < before, "independent scrollbar page command uses body page size");
            at(40);
            sci(SCI_SETVSCROLLBAR, false);
            mainView().update();
            check(!IsWindowVisible(mainView().testScrollHost()), "host can hide the independent scrollbar");
            sci(SCI_SETVSCROLLBAR, true);
            mainView().update();
            check(IsWindowVisible(mainView().testScrollHost()) && !sci(SCI_GETVSCROLLBAR),
                "host can restore independent scrollbar without duplicating native bar");
            sci(SCI_STYLESETFORE, SCE_JSON_PROPERTYNAME, RGB(255, 120, 200));
        },
        [] { mainView().update();
            check(mainView().testStyleColor(SCE_JSON_PROPERTYNAME) == RGB(255, 120, 200), "changing a syntax color refreshes unchanged sticky rows");
            captureEditor();
            const auto position = sci(SCI_POSITIONFROMLINE, 1) + 3;
            sci(SCI_STARTSTYLING, position);
            sci(SCI_SETSTYLING, 9, SCE_JSON_STRING);
            // The rest of the document was already styled. Keep that frontier so the
            // lexer does not immediately overwrite this deliberate style-only change.
            sci(SCI_STARTSTYLING, sci(SCI_GETLENGTH));
            sci(SCI_SETSTYLING, 0, SCE_JSON_DEFAULT);
        },
        [] { mainView().update();
            check(hasStyledToken(1, L"DeviceLog", SCE_JSON_STRING), "style-only notification refreshes cached lexer runs");
            std::string text = "<Root>\n  <Window index=\"5\" enabled=\"true\">\n";
            for (int i = 0; i < 100; ++i) text += "    <Value>42</Value>\n";
            text += "  </Window>\n</Root>\n";
            load(text, L_XML); at(40);
        },
        [] { expect({0, 1}, "XML attribute header retains context");
            const auto start = sci(SCI_POSITIONFROMLINE, 1);
            const auto attributeStyle = static_cast<unsigned char>(sci(SCI_GETSTYLEINDEXAT, start + 10));
            const auto valueStyle = static_cast<unsigned char>(sci(SCI_GETSTYLEINDEXAT, start + 17));
            check(hasStyledToken(1, L"index", attributeStyle) && hasStyledToken(1, L"5", valueStyle) && attributeStyle != valueStyle,
                "XML attributes and quoted values retain distinct source styles");
            check(mainView().testStyleColor(valueStyle) == sci(SCI_STYLEGETFORE, valueStyle), "XML attribute value color follows the editor");
            captureEditor(L"xml-attributes-render.bmp");
            std::string text = "{\n\t\"设备🙂\": {\n";
            for (int i = 0; i < 100; ++i) text += "\t\t\"value\": 1,\n";
            text += "\t\t\"last\": 1\n\t}\n}\n";
            load(text, L_JSON); at(40);
        },
        [] { expect({0, 1}, "Unicode and tabbed headers retain context");
            check(hasStyledToken(1, L"设备🙂", SCE_JSON_PROPERTYNAME), "UTF-8 Chinese and supplementary characters survive styled decoding");
            captureEditor(L"unicode-render.bmp");
            std::string text = "{\n  \"" + std::string(4090, 'x') + "🙂\": {\n";
            for (int i = 0; i < 100; ++i) text += "    \"value\": 1,\n";
            text += "    \"last\": 1\n  }\n}\n";
            load(text, L_JSON); at(40);
        },
        [] { expect({0, 1}, "truncated UTF-8 header retains ancestry");
            std::wstring text;
            for (const auto& run : mainView().testStyledLines().at(1)) text += run.text;
            check(!text.empty() && text.back() == L'\u2026' && text.find(L'\ufffd') == std::wstring::npos,
                "styled line byte cap does not split a multibyte character");
            burstUpdates = mainView().testUpdates;
            for (int i = 0; i < 1000; ++i) at(40 + i % 20);
        },
        [] {
            check(mainView().testUpdates - burstUpdates < 8, "1000 queued scroll events coalesce without an idle update loop");
            load(varyingJson(), L_JSON);
            at(varyingTargets.back());
            coldStart = std::chrono::steady_clock::now();
        },
        [] {
            if (mainView().testIndexedLines() <= varyingTargets.back() &&
                std::chrono::steady_clock::now() - coldStart < std::chrono::seconds(8)) { --stage; return; }
            check(mainView().testIndexedLines() > varyingTargets.back(), "varying-depth 156000-line log is indexed");
            std::vector<double> times;
            int changedHeights = 0;
            // Include body, header and scrollbar painting in these measurements.
            for (int i = 0; i < 256; ++i) {
                const auto previous = mainView().testHeight();
                const auto start = std::chrono::steady_clock::now();
                at(varyingTargets[static_cast<size_t>(i)]);
                mainView().update();
                RedrawWindow(npp._scintillaMainHandle, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                UpdateWindow(mainView().testHeader());
                UpdateWindow(mainView().testScrollbar());
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                if (previous != mainView().testHeight()) ++changedHeights;
            }
            std::sort(times.begin(), times.end());
            report << "METRIC varying-depth scroll+update+paint median=" << times[128]
                << " ms p95=" << times[243] << " ms max=" << times.back() << " ms heightChanges=" << changedHeights << '\n';
            check(changedHeights >= 90, "varying-depth benchmark exercises frequent live height changes");
            std::vector<double> baseline;
            togglePlugin();
            for (int i = 0; i < 256; ++i) {
                const auto start = std::chrono::steady_clock::now();
                at(varyingTargets[static_cast<size_t>(i)]);
                RedrawWindow(npp._scintillaMainHandle, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                baseline.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(baseline.begin(), baseline.end());
            report << "METRIC varying-depth disabled scroll+paint median=" << baseline[128]
                << " ms p95=" << baseline[243] << " ms max=" << baseline.back() << '\n';
            togglePlugin();
        },
        [] {
            // Open a real file and let the host style it naturally. Do not pre-colourise.
            const auto path = resultDirectory + L"\\Large.json";
            std::ofstream file(path, std::ios::binary);
            file << "{\n  \"DeviceLog\": {\n    \"Windows\": [\n";
            for (int i = 0; i < 150000; ++i) file << "      {\"Index\":" << i << ",\"Value\":42},\n";
            file << "      {\"Last\":true}\n    ]\n  }\n}\n";
            file.close();
            check(SendMessageW(npp._nppHandle, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str())) != FALSE,
                "open real Large.json without forced whole-document styling");
            at(40);
        },
        [] {
            expect({0, 1, 2}, "naturally styled Large.json resolves initial parents");
            report << "METRIC natural styling frontier=" << sci(SCI_GETENDSTYLED) << " bytes of " << sci(SCI_GETLENGTH) << '\n';
            lazyClears = mainView().testRowClears;
            lazyBlankPaints = mainView().testBlankPaints;
            lazyHeightChanges = mainView().testHeightChanges;
            lazyScrollStep = 0;
            lazyIdleStep = 0;
        },
        [] {
            // Stop scrolling while Notepad++ is still doing idle lexing below the viewport.
            UpdateWindow(mainView().testHeader());
            if (++lazyIdleStep < 10) { --stage; return; }
        },
        [] {
            report << "METRIC stopped during background styling rowClears=" << mainView().testRowClears - lazyClears
                << " blankPaints=" << mainView().testBlankPaints - lazyBlankPaints
                << " heightChanges=" << mainView().testHeightChanges - lazyHeightChanges
                << " styled=" << sci(SCI_GETENDSTYLED) << '\n';
            check(mainView().testRowClears == lazyClears && mainView().testBlankPaints == lazyBlankPaints &&
                mainView().testHeightChanges == lazyHeightChanges,
                "stopped Large.json keeps parents visible during background styling");
            lazyClears = mainView().testRowClears;
            lazyBlankPaints = mainView().testBlankPaints;
            lazyHeightChanges = mainView().testHeightChanges;
        },
        [] {
            at(60 + lazyScrollStep * 40);
            RedrawWindow(npp._scintillaMainHandle, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            UpdateWindow(mainView().testHeader());
            if (++lazyScrollStep < 60) { --stage; return; }
        },
        [] {
            report << "METRIC natural scroll rowClears=" << mainView().testRowClears - lazyClears
                << " blankPaints=" << mainView().testBlankPaints - lazyBlankPaints
                << " heightChanges=" << mainView().testHeightChanges - lazyHeightChanges << '\n';
            check(mainView().testRowClears == lazyClears && mainView().testBlankPaints == lazyBlankPaints &&
                mainView().testHeightChanges == lazyHeightChanges,
                "natural Large.json scrolling never blanks unchanged ancestors or resizes header");
            lazyUpdates = mainView().testUpdates;
        },
        [] {
            check(mainView().testUpdates - lazyUpdates < 4 && mainView().testHeightChanges == lazyHeightChanges,
                "stationary Large.json settles without a layout or update loop");
            const auto rootLevel = sci(SCI_GETFOLDLEVEL, 2);
            sci(SCI_SETFOLDLEVEL, 2, rootLevel & ~SC_FOLDLEVELHEADERFLAG);
            check(mainView().testLines().empty(), "changing a displayed ancestor still invalidates click targets immediately");
            expect({0, 1}, "ancestor fold change recomputes the current context");
            sci(SCI_SETFOLDLEVEL, 2, rootLevel);
            expect({0, 1, 2}, "restoring ancestor fold information recovers correct rows");
            at(4000);
            expect({0, 1, 2}, "forward scroll populates a larger cached fold suffix");
            at(40);
            expect({0, 1, 2}, "backward scroll uses the valid cached prefix");
            const auto laterLevel = sci(SCI_GETFOLDLEVEL, 100);
            sci(SCI_SETFOLDLEVEL, 100, laterLevel ^ SC_FOLDLEVELWHITEFLAG);
            check(mainView().testLines() == std::vector<sticky::Line>({0, 1, 2}) && mainView().testIndexedLines() == 100,
                "later fold change trims cached suffix without clearing current parents");
            sci(SCI_SETFOLDLEVEL, 100, laterLevel);
            at(4000);
            expect({0, 1, 2}, "returning to invalidated suffix rebuilds correct ancestors");
        }
    };
    wchar_t largeOnly[2]{};
    if (GetEnvironmentVariableW(L"NPP_STICKY_LARGE_ONLY", largeOnly, 2) && largeOnly[0] == L'1')
        steps.erase(steps.begin(), steps.end() - 7);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NppStickyScroll.HostTestRunner";
    RegisterClassW(&wc);
    runner = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetTimer(runner, 1, 500, nullptr);
}
