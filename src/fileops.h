#pragma once
#include "appstate.h"

// clipboard
bool clipboard_has_files();
void fileops_paste_into_current();
void fileops_set_cut_items(const std::vector<PCITEMID_CHILD>& children);   // empty = clear
bool fileops_is_cut(PCITEMID_CHILD child);    // ghost this row (cut, not yet pasted)

// FileOperations via IFileOperation
void fileops_delete_items(HWND hwnd, IShellFolder* parent,
                          const std::vector<PCITEMID_CHILD>& children, bool permanent);
// exactFileName: newName is the complete file name, even where Windows hides extensions
void fileops_rename_item(HWND hwnd, IShellFolder* parent, PCITEMID_CHILD child,
                         const std::wstring& newName, bool exactFileName = false);
void fileops_new_folder();
