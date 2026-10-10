#pragma once
#include "appstate.h"

// Custom Win11 window frame: the title bar is replaced by the tab row; caption
// buttons are drawn by us but hit-test as HTMINBUTTON/HTMAXBUTTON/HTCLOSE so
// snap layouts, double-click maximize and the system menu keep working.

void chrome_init(HWND hwnd);                       // after creation: frame change + DWM attributes
// frame / non-client messages; returns true when handled (*result set)
bool chrome_handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
void chrome_paint(HDC dc, const RECT& client);     // caption row + caption buttons
int  chrome_buttons_width();                       // total width of min/max/close
bool chrome_active();                              // window is active (caption colors)
void chrome_theme_changed(HWND hwnd);              // re-apply DWM attributes
