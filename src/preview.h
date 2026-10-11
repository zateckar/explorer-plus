#pragma once
#include "appstate.h"

// Shell preview handlers for the Preview pane.
bool preview_open(HWND host, PCIDLIST_ABSOLUTE item);   // true: a handler now draws into `host`
void preview_close();                                   // unload the current handler
bool preview_active();
void preview_resize();                                  // the host was resized
void preview_colors_changed();                          // light/dark switch
bool preview_has_focus();                               // the keyboard focus is in the preview
bool preview_focus();                                   // give the preview the focus (Tab / F6)
bool preview_translate_accelerator(MSG* msg);           // message loop: the handler's own keys first
