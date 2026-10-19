#pragma once
#include <windows.h>
#include <string>
#include <utility>
#include <vector>

// Sync folders in Explorer Plus (docs/sync-plan.md, "Explorer Plus UI"). The work is the
// agent's (sync_agent.h); this is the face of it.

// "..." > Sync folders: the pairs, adding one (first-time keys, unlocking), devices and
// keys. Modal; owner may be null (`--sync-folders`, from the tray).
void sync_folders_dialog(HWND owner);

// The lists' view of it (UI thread). A pane showing a folder inside a sync folder asks the
// agent for its state now and then; the answers come back as status bar text and badges.
void sync_view_folder(int pane, const std::wstring& fsPath);   // "" = not a file system folder
std::wstring sync_view_status(int pane);                       // "" outside sync folders
int sync_view_item(int pane, const std::wstring& fileName);    // sync::ItemState (0: none); a sync folder's root too
void sync_draw_badge(HDC dc, const RECT& icon, int state);    // on an item's icon, at its bottom left
// the sync folders' roots (lowercased) with their states; `gen` changes when they do
std::vector<std::pair<std::wstring, int>> sync_root_states(unsigned* gen);

// the context menu's sync entries for the chosen items (folders[i]: paths[i] is a folder)
struct SyncMenu {
    bool syncNow = false;           // any item inside a sync folder
    bool exclude = false;           // one folder, synced: "Exclude from sync"
    bool include = false;           // one folder, left out: "Include in sync"
    bool versions = false;          // one file, on a store that keeps versions: "Version history..."
};
SyncMenu sync_menu_for(const std::vector<std::wstring>& paths, const std::vector<bool>& folders);
void sync_now_for(const std::vector<std::wstring>& paths);
void sync_exclude(const std::wstring& folder, bool exclude);
void sync_version_history(HWND owner, const std::wstring& file);
// the "Sync status" column's text for an item of a pane (inside a sync folder)
std::wstring sync_status_text(int pane, const std::wstring& fileName);
// a folder inside a sync folder (any thread; the pairs re-read when pairs.ini changes)
bool sync_in_sync_folder(const std::wstring& path);

// sync folders for the navigation pane: name, folder
std::vector<std::pair<std::wstring, std::wstring>> sync_roots();
