// Plugin entry points follow the official npp-plugins/plugintemplate ABI.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include "PluginInterface.h"
#include "StickyView.h"
#include "SettingsDialog.h"
#include "Version.h"
#include <array>
#include <string>

namespace {
constexpr wchar_t pluginName[] = L"NppStickyScroll";
constexpr wchar_t workerClass[] = L"NppStickyScroll.Worker";
constexpr UINT_PTR continuationTimer = 1;
HINSTANCE instance = nullptr;
NppData host{};
FuncItem menu[3]{};
sticky::Settings settings;
std::array<sticky::StickyView, 2> views;
HWND worker = nullptr;
bool enabled = true;
bool ready = false;
bool pending = false;
std::wstring configPath;

void requestUpdate() {
    if (worker && enabled && !pending) {
        // One event-triggered frame, not one posted task per intermediate thumb position.
        // WM_TIMER yields to input/paint and also runs inside the scrollbar modal loop.
        pending = SetTimer(worker, continuationTimer, 16, nullptr) != 0;
    }
}

void attachViews() {
    if (!ready || !enabled) return;
    for (auto& view : views) view.configure(settings);
    views[0].attach(host._scintillaMainHandle, requestUpdate);
    views[1].attach(host._scintillaSecondHandle, requestUpdate);
    requestUpdate();
}

void openSettings() {
    auto requested = settings;
    if (!sticky::showSettingsDialog(instance, host._nppHandle, requested)) return;
    settings = requested;
    for (auto& view : views) view.configure(settings);
    if (!configPath.empty()) {
        const auto rows = std::to_wstring(settings.maxRows);
        WritePrivateProfileStringW(L"StickyScroll", L"MaxRows", rows.c_str(), configPath.c_str());
        WritePrivateProfileStringW(L"StickyScroll", L"KeepNearest", settings.nearest ? L"1" : L"0", configPath.c_str());
    }
}

void showAbout() {
    MessageBoxW(host._nppHandle, L"NppStickyScroll " NPP_STICKY_VERSION_W L"\nWindows x64\n\nJSON/XML 粘性滚动插件",
        L"关于 NppStickyScroll", MB_OK | MB_ICONINFORMATION);
}

void toggle() {
    enabled = !enabled;
    SendMessageW(host._nppHandle, NPPM_SETMENUITEMCHECK, menu[0]._cmdID, enabled);
    if (enabled) attachViews();
    else {
        if (worker) KillTimer(worker, continuationTimer);
        pending = false;
        for (auto& view : views) view.detach();
    }
    if (!configPath.empty()) WritePrivateProfileStringW(L"StickyScroll", L"Enabled", enabled ? L"1" : L"0", configPath.c_str());
}

LRESULT CALLBACK workerProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_TIMER && w == continuationTimer) {
        pending = false;
        KillTimer(hwnd, continuationTimer);
        if (enabled) {
            bool remaining = false;
            try { for (auto& view : views) remaining = view.update() || remaining; }
            catch (...) { for (auto& view : views) view.clear(); }
            // One-shot work continuation, only while an event-triggered rebuild is incomplete.
            // Idle sessions have no timer and no polling. Input and paints run between slices.
            if (remaining) requestUpdate();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

void initialize() {
    WNDCLASSW klass{};
    klass.lpfnWndProc = workerProc;
    klass.hInstance = instance;
    klass.lpszClassName = workerClass;
    RegisterClassW(&klass);
    worker = CreateWindowExW(0, workerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    const auto length = SendMessageW(host._nppHandle, NPPM_GETPLUGINSCONFIGDIR, 0, 0);
    if (length > 0 && length < 32700) {
        std::wstring directory(static_cast<size_t>(length) + 1, L'\0');
        if (SendMessageW(host._nppHandle, NPPM_GETPLUGINSCONFIGDIR, directory.size(), reinterpret_cast<LPARAM>(directory.data()))) {
            directory.resize(wcslen(directory.c_str()));
            configPath = directory + L"\\NppStickyScroll.ini";
            enabled = GetPrivateProfileIntW(L"StickyScroll", L"Enabled", 1, configPath.c_str()) != 0;
            settings.maxRows = static_cast<int>(GetPrivateProfileIntW(L"StickyScroll", L"MaxRows", 5, configPath.c_str()));
            settings.nearest = GetPrivateProfileIntW(L"StickyScroll", L"KeepNearest", 1, configPath.c_str()) != 0;
            settings.normalize();
        }
    }
    SendMessageW(host._nppHandle, NPPM_ADDSCNMODIFIEDFLAGS, 0, SC_MOD_CHANGEFOLD | SC_MOD_CHANGESTYLE);
    SendMessageW(host._nppHandle, NPPM_SETMENUITEMCHECK, menu[0]._cmdID, enabled);
    ready = true;
    attachViews();
}

void shutdown() {
    ready = false;
    for (auto& view : views) view.detach();
    if (worker) { KillTimer(worker, continuationTimer); DestroyWindow(worker); }
    worker = nullptr;
    UnregisterClassW(workerClass, instance);
}
}

#ifdef STICKY_HOST_TESTS
void startHostTests(NppData, std::array<sticky::StickyView, 2>*, void (*)());
#endif

BOOL APIENTRY DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        instance = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
extern "C" __declspec(dllexport) void setInfo(NppData data) {
    host = data;
    wcscpy_s(menu[0]._itemName, L"启用 Sticky Scroll");
    menu[0]._pFunc = toggle;
    menu[0]._init2Check = true;
    wcscpy_s(menu[1]._itemName, L"设置...");
    menu[1]._pFunc = openSettings;
    wcscpy_s(menu[2]._itemName, L"关于...");
    menu[2]._pFunc = showAbout;
}
extern "C" __declspec(dllexport) const wchar_t* getName() { return pluginName; }
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) { *count = 3; return menu; }
extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
extern "C" __declspec(dllexport) void beNotified(SCNotification* n) {
    if (!n) return;
    try {
        switch (n->nmhdr.code) {
        case NPPN_READY:
            initialize();
#ifdef STICKY_HOST_TESTS
            startHostTests(host, &views, toggle);
#endif
            return;
        case NPPN_SHUTDOWN: shutdown(); return;
        default: break;
        }
        if (!ready || !enabled) return;
        switch (n->nmhdr.code) {
        case SCN_MODIFIED:
            if (!(n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_MOD_CHANGEFOLD | SC_MOD_CHANGESTYLE))) return;
            for (auto& view : views) {
                if (view.window() && (view.window() == n->nmhdr.hwndFrom ||
                    SendMessageW(static_cast<HWND>(n->nmhdr.hwndFrom), SCI_GETDOCPOINTER, 0, 0) == view.sci(SCI_GETDOCPOINTER)))
                    view.modified(*n);
            }
            break;
        case SCN_UPDATEUI:
            if (!(n->updated & (SC_UPDATE_CONTENT | SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL))) return;
            break;
        case SCN_PAINTED: break;
        case SCN_ZOOM:
            for (auto& view : views) if (view.window() == n->nmhdr.hwndFrom) view.appearanceChanged();
            break;
        case NPPN_BUFFERACTIVATED: case NPPN_LANGCHANGED: case NPPN_FILECLOSED:
        case NPPN_GLOBALMODIFIED: case NPPN_WORDSTYLESUPDATED: case NPPN_DARKMODECHANGED:
            for (auto& view : views) { view.invalidate(); view.appearanceChanged(n->nmhdr.code == NPPN_DARKMODECHANGED); }
            break;
        default: return;
        }
        requestUpdate();
    } catch (...) {
        // Safe failure: remove every inset instead of leaving stale navigation targets.
        for (auto& view : views) view.detach();
    }
}
