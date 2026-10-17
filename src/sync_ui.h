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
int sync_view_item(int pane, const std::wstring& fileName);    // sync::ItemState (0: none)
void sync_draw_badge(HDC dc, const RECT& icon, int state, int dpi);

// the context menu's "Sync now" (the pairs the paths are in)
bool sync_paths_synced(const std::vector<std::wstring>& paths);
void sync_now_for(const std::vector<std::wstring>& paths);

// sync folders for the navigation pane: name, folder
std::vector<std::pair<std::wstring, std::wstring>> sync_roots();
