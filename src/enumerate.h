#pragma once
#include "appstate.h"

// Start enumerating `folderAbs` (cloned) on a background thread. The worker binds the
// folder itself so no apartment-threaded COM object crosses threads.
// keepView = true for a refresh of the folder already shown: the current items stay
// visible until the new listing arrives, and selection/focus/scroll are restored.
void enumerate_start(PCIDLIST_ABSOLUTE folderAbs, bool keepView);

// After the next listing is applied, select (and optionally start renaming) this item.
void enum_set_pending_select(const std::wstring& name, bool beginRename);

// UI-thread entry points (called from main.cpp)
void enum_apply_op(HeapEnumOp* op);      // WM_ENUM_DONE handler (takes ownership of op)
void enum_on_stream_timer();             // TIMER_ENUM_STREAM: show the rows found so far
void enum_sort_items();                  // re-sort / re-group g_items (g_sortColumn, g_view.groupBy) and refresh
void enum_update_header_arrows();        // put sort arrows in the details header
void enum_clear_items();                 // drop all items (navigation away)
const std::wstring& enum_last_error();   // non-empty when the current folder failed to list
