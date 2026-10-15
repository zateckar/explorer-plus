#pragma once
#include "appstate.h"

// Win11 navigation buttons: Back, Forward, Up, Refresh; the Notepad and the player
HWND navbar_create(HWND parent);   // id = IDC_NAVBAR
void navbar_refresh();             // recompute enabled states + repaint
int  navbar_width();               // at the current DPI
POINT navbar_menu_point();         // screen point under the "..." button (its menu's corner)
