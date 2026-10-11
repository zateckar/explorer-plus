#pragma once
#include "appstate.h"

// Win11 navigation pane: Home, Gallery, OneDrive | pinned Quick access folders |
// This PC, Network. Children are listed on a worker thread when a node expands.
void initialize_tree(HWND tree);
void tree_apply_metrics();             // row height for the DPI and text size
void tree_sync_to_current();           // expand & select current folder in tree
void tree_sync_to_current_deferred();  // async (posted) version for hot paths
void tree_prefetch(PCIDLIST_ABSOLUTE folder);   // startup: list the folder's ancestors before the window exists
void tree_refresh_all();               // rebuild (hidden items, pins changed); re-syncs to current
void tree_reread_pins();                    // Quick access pins changed (pin / unpin)
void tree_refresh_node(PCIDLIST_ABSOLUTE folder);   // re-list one node's children if shown
void tree_rename_selected();           // F2 in the navigation pane
void tree_rename_menu_node();          // Rename on the node a context menu is open for
void tree_delete_selected(bool permanent);    // Delete key in the navigation pane
void tree_delete_menu_node(bool permanent);   // Delete on the node a context menu is open for
LRESULT tree_notify(HWND hwnd, WPARAM wp, LPARAM lp);

// clone of the node pidl (empty pidl for the Desktop root); nullptr for separators
PIDLIST_ABSOLUTE tree_get_pidl_of_item(HTREEITEM h);
