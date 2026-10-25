#pragma once
#include "appstate.h"
#include <functional>

// clipboard
bool clipboard_has_files();
void fileops_paste_into_current();
void fileops_paste_into(PCIDLIST_ABSOLUTE folder);
// put the items on the clipboard as Copy or Cut; false when the clipboard was busy
bool fileops_set_clipboard(IShellFolder* parent, const std::vector<PCITEMID_CHILD>& children, bool cut);
// the items' paths, quoted, one per line, as text ("Copy as path"); false when the clipboard was busy
bool fileops_copy_paths(const std::vector<PCIDLIST_ABSOLUTE>& items);
void fileops_set_cut_items(const std::vector<PCITEMID_CHILD>& children);   // empty = clear
bool fileops_is_cut(PCITEMID_CHILD child);    // ghost this row (cut, not yet pasted)

// FileOperations via IFileOperation
// Paste and delete run on a thread of their own (the window takes input meanwhile); rename
// and New folder are synchronous.
// done: on the UI thread once the delete is over (none: the panes showing the folder are
// listed again); not when the window has closed meanwhile
void fileops_delete_items(HWND hwnd, IShellFolder* parent, const std::vector<PCITEMID_CHILD>& children,
                          bool permanent, std::function<void()> done = nullptr);
// a paste, delete or asynchronous drop still running (a closed window's process waits for
// it); the last one to end posts WM_FILEOPS_ENDED to the UI thread
bool fileops_running();
// (UI thread) one more running elsewhere: a drop target copying our dragged items on a
// thread of its own
void fileops_op_begin();
void fileops_op_end();
// exactFileName: newName is the complete file name, even where Windows hides extensions;
// true when the item was renamed (not cancelled, not failed)
bool fileops_rename_item(HWND hwnd, IShellFolder* parent, PCITEMID_CHILD child,
                         const std::wstring& newName, bool exactFileName = false);
void fileops_new_folder();
