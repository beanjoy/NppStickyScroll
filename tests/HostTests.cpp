// Test-only code: never linked into the delivered DLL. Runs against real Notepad++/Lexilla.
// Requires an explicit NPP_STICKY_TEST_DIR under the project's .tools directory.
#include <windows.h>
#include "PluginInterface.h"
#include "menuCmdID.h"
#include "StickyView.h"
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
std::wstring resultDirectory;
std::vector<std::function<void()>> steps;

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

void captureEditor() {
    // Exercise the real editor's WM_PRINT rendering path, independent of desktop capture tools.
    RECT bounds{}; GetWindowRect(npp._scintillaMainHandle, &bounds);
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
    SendMessageW(npp._scintillaMainHandle, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream image(resultDirectory + L"\\editor-render.bmp", std::ios::binary);
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
        [] { load(json(), L_JSON); },
        [] { expect({}, "JSON at document start hides bar"); GetWindowRect(npp._scintillaMainHandle, &originalWindow); GetClientRect(npp._scintillaMainHandle, &originalClient); at(40); },
        [] {
            expect({0, 1, 2}, "JSON scroll uses viewport with caret still at start");
            check(sci(SCI_GETCURRENTPOS) == 0, "scrolling does not move caret");
            RECT rect{}, client{}; GetWindowRect(npp._scintillaMainHandle, &rect); GetClientRect(npp._scintillaMainHandle, &client);
            POINT point{}; ClientToScreen(npp._scintillaMainHandle, &point);
            check(EqualRect(&rect, &originalWindow), "editor outer rectangle unchanged");
            check(point.y - rect.top >= mainView().testHeight() && originalClient.bottom - client.bottom == mainView().testHeight(), "body client begins below reserved sticky rows");
            check(GetFocus() == npp._scintillaMainHandle, "updates preserve input focus");
            at(154);
        },
        [] { expect({0, 1}, "JSON closing bracket excludes completed array"); at(156); },
        [] { expect({0}, "JSON closing object excludes completed DeviceLog"); at(157); },
        [] { expect({}, "JSON closing root hides all rows"); at(40); },
        [] {
            mainView().update();
            RECT rect{}; GetWindowRect(npp._scintillaMainHandle, &rect);
            POINT origin{}; ClientToScreen(npp._scintillaMainHandle, &origin);
            const LPARAM point = MAKELPARAM(static_cast<short>(origin.x + 90), static_cast<short>(origin.y - mainView().testHeight() + 4));
            check(SendMessageW(npp._scintillaMainHandle, WM_NCHITTEST, 0, point) == HTBORDER, "sticky row has independent nonclient hit target");
            SendMessageW(npp._scintillaMainHandle, WM_NCLBUTTONDOWN, HTBORDER, point);
        },
        [] { check(sci(SCI_LINEFROMPOSITION, sci(SCI_GETCURRENTPOS)) == 0 && sci(SCI_DOCLINEFROMVISIBLE, sci(SCI_GETFIRSTVISIBLELINE)) == 0, "click navigation reaches exact source line"); expect({}, "clicked source is not duplicated"); at(40); },
        [] { sci(SCI_INSERTTEXT, 0, reinterpret_cast<LPARAM>("\n\n")); sci(SCI_COLOURISE, 0, -1); at(42); },
        [] { expect({2, 3, 4}, "insert before ancestors invalidates all source line numbers"); sci(SCI_UNDO); sci(SCI_COLOURISE, 0, -1); at(40); savepoint(); },
        [] { expect({0, 1, 2}, "undo restores ancestor lines"); sci(SCI_FOLDLINE, 2, SC_FOLDACTION_CONTRACT); at(155); },
        [] { expect({0, 1}, "collapsed array does not leak stale parents"); sci(SCI_FOLDLINE, 2, SC_FOLDACTION_EXPAND); at(40); },
        [] { expect({0, 1, 2}, "expanding fold restores ancestry"); sci(SCI_SETXOFFSET, 80); },
        [] { expect({0, 1, 2}, "horizontal scrolling preserves parent identity"); check(mainView().testXOffset() == sci(SCI_GETXOFFSET), "sticky drawing uses host horizontal offset"); sci(SCI_SETXOFFSET, 0); sci(SCI_SETZOOM, 4); },
        [] { expect({0, 1, 2}, "zoom preserves ancestry"); check(mainView().testHeight() == 3 * sci(SCI_TEXTHEIGHT, 0) + 1, "sticky height follows host zoom"); sci(SCI_SETZOOM, 0); togglePlugin(); },
        [] { RECT client{}; GetClientRect(npp._scintillaMainHandle, &client); check(mainView().window() == nullptr && client.bottom == originalClient.bottom, "disable removes subclass and restores full client height"); togglePlugin(); },
        [] { expect({0, 1, 2}, "reenable restores sticky rows"); load("", L_JSON); },
        [] { expect({}, "empty file hides bar"); load("plain\ntext\n", L_TEXT); },
        [] { expect({}, "unsupported language hides bar");
            std::string text = "<DeviceLog>\n  <Windows>\n";
            for (int i = 0; i < 150; ++i) text += "    <Value>42</Value>\n";
            text += "  </Windows>\n  <After>true</After>\n</DeviceLog>\n";
            for (int i = 0; i < 70; ++i) text += '\n';
            load(text, L_XML); at(40);
        },
        [] { expect({0, 1}, "XML ancestors from built-in lexer"); at(152); },
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
        [] { expect({6, 7, 8, 9, 10}, "deep nesting keeps nearest five in outer to inner order");
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
            for (int i = 0; i < 1000; ++i) {
                const auto start = std::chrono::steady_clock::now();
                at(140000 - i);
                mainView().update();
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            report << "METRIC 1000 cached scroll+update calls median=" << times[500] << " ms p95=" << times[950] << " ms max=" << times.back() << " ms\n";
            check(times[950] < 16.0, "cached large-file update p95 below one 60 Hz frame");
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
            check(mainView().testHeight() == 3 * sci(SCI_TEXTHEIGHT, 0) + 1, "resize keeps required sticky height");
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
        [] { expect({0, 1, 2}, "restored fold information recovers bar"); captureEditor(); }
    };
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NppStickyScroll.HostTestRunner";
    RegisterClassW(&wc);
    runner = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetTimer(runner, 1, 500, nullptr);
}
