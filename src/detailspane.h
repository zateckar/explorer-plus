#pragma once
#include "appstate.h"

// The right-hand pane: Details (properties of the selection) or Preview (the file's
// shell preview handler), like Win11 Explorer.
enum PaneMode : int { PANE_DETAILS = 0, PANE_PREVIEW = 1 };

HWND detailspane_create(HWND parent);  // id = IDC_DETAILSPANE
void detailspane_update();             // rebuild from current selection (no-op while hidden)
void detailspane_set_visible(bool v);
bool detailspane_visible();
void detailspane_set_mode(PaneMode m);
PaneMode detailspane_mode();
void detailspane_toggle(PaneMode m);   // Alt+Shift+P / Alt+P: show that pane, or hide it when shown
void detailspane_theme_changed();
void detailspane_release_files();      // unload the preview handler (before delete / rename / move)
HWND detailspane_hwnd();
