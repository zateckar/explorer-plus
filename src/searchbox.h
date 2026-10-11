#pragma once
#include "appstate.h"

// Search box: a recursive search of the current folder (ISearchFolderItemFactory),
// shown as a "Search Results in X" location that streams in like any other folder.

void search_submit();                      // Enter / typing pause: search the box text
void search_cancel();                      // Esc / emptied box: back to the searched folder
void search_text_changed();                // EN_CHANGE: (re)start the typing-pause timer
void search_on_timer();                    // TIMER_SEARCH
void search_on_navigated();                // keep the box text in step with the location
bool search_is_results(PCIDLIST_ABSOLUTE abs);
PCIDLIST_ABSOLUTE search_scope_of(PCIDLIST_ABSOLUTE abs);   // folder a result set searched, or nullptr
PIDLIST_ABSOLUTE search_real_location(PCIDLIST_ABSOLUTE resultItem);   // new pidl of the item itself, or nullptr
