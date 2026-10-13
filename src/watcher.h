#pragma once
#include "appstate.h"

// SHChangeNotifyRegister-based auto refresh of the folder a pane shows (debounced). Each pane
// has its own watch; the calls below act on g_pane's.
void watcher_init(HWND main);
int  watcher_pane_of_msg(UINT msg);              // the pane a registered change message is for, else -1
void watcher_register_for(PCIDLIST_ABSOLUTE folder);
void deregister_current_watch();
void watcher_on_message(WPARAM wp, LPARAM lp);   // the registered change message
void watcher_on_timer();                         // TIMER_WATCH_REFRESH (g_pane's)
// Our own file operation runs (synchronously, pumping messages): the watcher keeps the
// changes it hears of until the operation ends, so it can't act on a half-done state
// (e.g. leave a folder that is being renamed before the rename follows it). releaseDir:
// also close the directory watch's handle on the folder, which may be the one renamed
// or deleted; the next listing of the folder opens it again.
void watcher_hold_begin(bool releaseDir);
void watcher_hold_end();
