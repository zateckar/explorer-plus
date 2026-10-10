#pragma once
#include "appstate.h"

// Details columns: every folder shows its own column set (the shell's defaults —
// Recycle Bin: Original location / Date deleted, This PC: Total size / Free space),
// user-chosen columns and widths are remembered per folder.

// --- any thread (COM initialized) ---
// file-system folders: Name / Date modified / Type / Size; virtual folders (This PC,
// Recycle Bin, Home, ...): the shell folder's own default columns
ColumnSet columns_defaults(IShellFolder* folder, PCIDLIST_ABSOLUTE abs);
ColumnSet columns_parse(const std::wstring& persisted);      // empty when unusable
std::wstring columns_serialize(const ColumnSet& cols);
void columns_fill(IShellItem2* si, const ColumnSet& cols, Item* it);   // extra values of one item

// --- UI thread ---
extern ColumnSet g_columns;                    // columns of the shown listing
ColumnSet columns_for_folder(PCIDLIST_ABSOLUTE folder);   // persisted set or empty (= defaults)
// "Apply to all folders": the shown set (order, widths) becomes the file-system folders' default
bool columns_can_apply_to_all();               // the current folder is a file-system folder
void columns_set_default_from_current();
void columns_forget_default();
void columns_apply(const ColumnSet& cols);     // rebuild the header when the set changed
const wchar_t* columns_text(const Item* it, int subItem);   // LVN_GETDISPINFO
const UINT* columns_tile_lines(UINT* count);   // tile view: type + size sub-items
int  columns_sort_display_index();             // header column showing the sort arrow
void columns_header_click(int subItem);        // sort by that column (toggles direction)
void columns_sort_by(int subItem);             // Sort menu: that column, its default direction
int  columns_compare_extra(const Item* a, const Item* b);   // g_sortColumn == COL_EXTRA
void columns_prepare_sort();                   // resolve g_sortKey before a sort
void columns_header_menu(POINT screen);        // header right-click: column chooser
void columns_widths_changed();                 // the user resized a column
void columns_rescale();                        // DPI or text size changed: widths follow
