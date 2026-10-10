#pragma once
#include "appstate.h"

// Win11-style context menu ("flyout"): a rounded popup with an optional row of icon
// buttons (Cut, Copy, Rename, Share, Delete) above a list of entries. Submenus open as
// classic popup menus owned by `owner` (so shell submenus get their menu messages).

struct FlyEntry {
    enum Kind { Item, Separator, Submenu } kind = Item;
    UINT id = 0;                    // returned when chosen
    std::wstring text;              // '&' marks are dropped
    std::wstring accel;             // "Ctrl+Shift+C"
    wchar_t glyph = 0;              // Segoe Fluent Icons glyph, or
    HBITMAP bitmap = nullptr;       // a menu item bitmap (not owned; monochrome ones are tinted)
    HICON icon = nullptr;           // or an icon (not owned)
    bool bold = false;              // the default verb
    bool disabled = false;
    bool checked = false;           // a check mark in the icon slot (toggles)
    HMENU sub = nullptr;            // Submenu: shown as a classic popup (not owned)
};

struct FlyButton {
    UINT id = 0;
    wchar_t glyph = 0;
    std::wstring tip;               // accessible name / tooltip ("Cut (Ctrl+X)")
    bool disabled = false;
};

// Shows the menu at `pt` (screen) and returns the chosen id (an entry's, a button's or
// one picked in a submenu), 0 when dismissed.
UINT flyout_track(HWND owner, POINT pt, const std::vector<FlyButton>& row,
                  const std::vector<FlyEntry>& entries);

HWND flyout_window();               // the open flyout, or nullptr (tests, diagnostics)
