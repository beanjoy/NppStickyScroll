// SPDX-License-Identifier: GPL-3.0-or-later
#include "SettingsDialog.h"
#include "resource.h"
#include <commctrl.h>

namespace sticky {
namespace {
INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    auto* settings = reinterpret_cast<Settings*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        settings = reinterpret_cast<Settings*>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SendDlgItemMessageW(dialog, IDC_MAX_SPIN, UDM_SETRANGE32, 1, Settings::maxAllowedRows);
        SetDlgItemInt(dialog, IDC_MAX_ROWS, settings->maxRows, FALSE);
        CheckRadioButton(dialog, IDC_NEAREST, IDC_OUTERMOST, settings->nearest ? IDC_NEAREST : IDC_OUTERMOST);
        RECT owner{}, rect{};
        GetWindowRect(GetParent(dialog), &owner);
        GetWindowRect(dialog, &rect);
        SetWindowPos(dialog, nullptr, owner.left + (owner.right - owner.left - rect.right + rect.left) / 2,
            owner.top + (owner.bottom - owner.top - rect.bottom + rect.top) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(w) == IDOK && settings) {
        BOOL valid = FALSE;
        const UINT rows = GetDlgItemInt(dialog, IDC_MAX_ROWS, &valid, FALSE);
        if (!valid || rows < 1 || rows > Settings::maxAllowedRows) {
            MessageBoxW(dialog, L"请输入 1 到 20 之间的最大行数。", L"Sticky Scroll 设置", MB_OK | MB_ICONINFORMATION);
            SetFocus(GetDlgItem(dialog, IDC_MAX_ROWS));
            return TRUE;
        }
        settings->maxRows = static_cast<int>(rows);
        settings->nearest = IsDlgButtonChecked(dialog, IDC_NEAREST) == BST_CHECKED;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(w) == IDCANCEL) || message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
}

bool showSettingsDialog(HINSTANCE instance, HWND owner, Settings& settings) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&controls);
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner, settingsProc,
        reinterpret_cast<LPARAM>(&settings)) == IDOK;
}
}
