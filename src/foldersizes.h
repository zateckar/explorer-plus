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
// What is inside the shown folder's subfolders is watched: a change anywhere inside one
// walks it again. A refresh (F5) walks every folder again.
bool foldersizes_enabled();
void foldersizes_set_enabled(bool on);       // View ▸ Show ▸ Folder sizes (the caller persists it)

enum class SizeReuse {
    None,       // walk everything again (folders that changed)
    Recent,     // reuse sizes worked out moments ago (a refresh in place: a burst of changes)
    Any,        // reuse sizes from the last few minutes (arriving in a folder)
};

// Walk the shown folder items: all of them (nullptr: a listing is complete) or the ones
// with these keys (they changed).
void foldersizes_start(const std::unordered_set<std::wstring>* keys, SizeReuse reuse = SizeReuse::None);

void foldersizes_on_timer();                 // TIMER_FOLDER_SIZES: changed folders waiting to be walked
void foldersizes_release_watch();            // let go of the folder (another folder, or a file operation)
void foldersizes_forget_shown();             // a refresh by the user: the shown folders' sizes are walked again
