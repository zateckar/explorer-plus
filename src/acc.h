#pragma once
#include <windows.h>
#include <oleacc.h>
#include <functional>
#include <string>

// MSAA for the custom-drawn controls (tab strip, navigation buttons, breadcrumb, command
// bar, status bar, flyout menus). The window is a container of simple children; the
// control describes them through an AccSource, asked on demand (on the UI thread).

struct AccChild {
    std::wstring name;
    long role = ROLE_SYSTEM_PUSHBUTTON;
    long state = 0;                  // STATE_SYSTEM_* (FOCUSABLE etc. as the control sees fit)
    RECT rc{};                       // client coordinates
    std::wstring action;             // default action ("Press", "Switch"); empty: none
    std::wstring shortcut;           // keyboard shortcut text
    std::wstring value;
};

struct AccSource {
    std::wstring name;               // the container's name
    long role = ROLE_SYSTEM_TOOLBAR;
    std::function<int()> count;
    std::function<AccChild(int)> child;
    std::function<void(int)> action;           // optional: run child i's default action
    std::function<int()> focus;                // optional: the child with keyboard focus, -1 none
    std::function<std::wstring()> value;       // optional: the container's value
};

// WM_GETOBJECT handler: the accessible object for OBJID_CLIENT, else DefWindowProc's answer.
// `src` must outlive the window (a static in the control's module).
LRESULT acc_get_object(HWND hwnd, WPARAM wp, LPARAM lp, const AccSource* src);

// Tell screen readers: `child` is 0-based, -1 = the window itself (CHILDID_SELF).
void acc_notify(HWND hwnd, DWORD event, int child);
