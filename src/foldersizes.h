#pragma once
#include "appstate.h"
#include <unordered_set>

// Folder sizes in the Size column. Once a listing is complete, workers add up what is
// inside each subfolder (every level, hidden files too; junctions and symbolic links are
// not followed) and the sizes fill in as they are known. Navigating on, a refresh or
// switching it off stops the walk.
bool foldersizes_enabled();
void foldersizes_set_enabled(bool on);       // View ▸ Show ▸ Folder sizes (the caller persists it)

// Walk the shown folder items: all of them (nullptr: a listing is complete) or the ones
// with these keys (they changed).
void foldersizes_start(const std::unordered_set<std::wstring>* keys);
