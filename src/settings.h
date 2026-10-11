#pragma once
#include "appstate.h"

struct SplitMode {
    int width = 280;
    int widthRight = 260;
    int winW = 0, winH = 0;
    int x = INT_MIN, y = INT_MIN;
    bool maximized = false;
    bool treeVisible = true;
    bool detailsVisible = true;
};

int  view_to_int(const View& v);        // persisted form (settings + tab lines)
View int_to_view(int x);

void settings_load_all();
bool settings_save_all();               // false: the file couldn't be written (changes kept for a retry)
View settings_default_view();
void settings_set_default_view(const View& v);
const View* settings_view_for(const std::wstring& folderKey);
void settings_set_view_for(const std::wstring& folderKey, const View& v);
const std::wstring* settings_columns_for(const std::wstring& folderKey);   // persisted column set
void settings_set_columns_for(const std::wstring& folderKey, const std::wstring& cols);   // empty: forget
void settings_forget_fs_folders();      // drop every file-system folder's own view and columns
bool settings_show_hidden();
void settings_set_show_hidden(bool b);
SplitMode settings_splitter();
void settings_set_splitter(const SplitMode& s);

// tabs persistence
void settings_tabs(std::wstring& tabsOut, int& active);      // read
bool settings_set_tabs(const std::vector<std::wstring>& lines, int active);   // written at once; false: kept for settings_save_all
bool settings_details_pane();
void settings_set_details_pane(bool v);

// [contextmenu] promoteN lines: "<item text or verb>[|<icon file>[,<index>]]"
std::vector<std::wstring> settings_menu_promotions();

// [tree] pinN: the navigation pane's Quick access pins as last read ("<hasKids 0|1>|<path>"),
// shown at once on the next start while a worker reads the current ones
std::vector<std::wstring> settings_tree_pins();
void settings_set_tree_pins(const std::vector<std::wstring>& pins);

// small integer preferences ([prefs]: checkBoxes, showExtensions, ...)
int  settings_pref(const wchar_t* key, int def);
void settings_set_pref(const wchar_t* key, int v);
