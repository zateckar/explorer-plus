#pragma once
#include "appstate.h"
#include <unordered_set>

// Folder sizes in the Size column. Once a listing is complete, workers add up what is
// inside each subfolder (every level, hidden files too; junctions and symbolic links are
// not followed) and the sizes fill in as they are known. Navigating on, a refresh or
// switching it off stops the walk.
//
// Only folders on the machine's own fixed disks are walked: not shares or mapped drives,
// removable or USB disks, nor cloud placeholders (walking those would download them).
// Sizes are kept for a while, so going back to a folder doesn't walk it again.
bool foldersizes_enabled();
void foldersizes_set_enabled(bool on);       // View ▸ Show ▸ Folder sizes (the caller persists it)

enum class SizeReuse {
    None,       // walk everything again (folders that changed)
    Recent,     // reuse sizes worked out moments ago (a refresh: F5, or a burst of changes)
    Any,        // reuse sizes from the last few minutes (arriving in a folder)
};

// Walk the shown folder items: all of them (nullptr: a listing is complete) or the ones
// with these keys (they changed).
void foldersizes_start(const std::unordered_set<std::wstring>* keys, SizeReuse reuse = SizeReuse::None);
