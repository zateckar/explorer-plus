#pragma once
#include "appstate.h"

// SHChangeNotifyRegister-based auto refresh of the current folder (debounced).
void watcher_init(HWND main);
UINT watcher_change_msg();
void watcher_register_for(PCIDLIST_ABSOLUTE folder);
void deregister_current_watch();
void watcher_on_message(WPARAM wp, LPARAM lp);   // the registered change message
void watcher_on_timer();                         // TIMER_WATCH_REFRESH
