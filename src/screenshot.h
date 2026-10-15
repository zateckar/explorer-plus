#pragma once
#include <windows.h>

// Screenshots: Alt+Q (anywhere, while Explorer Plus runs) or "..." > Take a screenshot
// freezes the screen; drag across a region, or click for the whole screen under the mouse
// (Enter too; Esc or a right-click: none). The picture opens in the picture editor.

// the Alt+Q hotkey (a window of its own: it outlives the main window). Another process
// holding it: tried again now and then. [prefs] screenshotHotkey=0 turns it off.
void screenshot_init();
void screenshot_start(UINT delayMs = 0);     // (a delay: a menu just closed fades first)
bool screenshot_owns(HWND h);                // the region picker
