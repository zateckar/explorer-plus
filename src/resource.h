#pragma once

#define IDI_APP 1
#define IDI_PLAYER 2        // the media player's window (res/player.ico, tools/make_icon.py --player)
#define IDI_NOTEPAD 3       // the Notepad's window (res/notepad.ico, tools/make_icon.py --notepad)

#define IDC_TREE      1001
#define IDC_LIST      1002
#define IDC_ADDRESS   1003
#define IDC_SEARCH    1004
#define IDC_NAVBAR    1005
#define IDC_STATUS    1006
#define IDC_TABSTRIP  1007
#define IDC_DETAILSPANE 1008

// ---- File ----
#define IDM_FILE_NEWFOLDER   40010
#define IDM_FILE_NEWTAB      40011
#define IDM_FILE_CLOSETAB    40012
#define IDM_FILE_OPEN        40013
#define IDM_FILE_RENAME      40014
#define IDM_FILE_DELETE      40015
#define IDM_FILE_PROPERTIES  40016
#define IDM_FILE_EXIT        40017

// ---- Edit ----
#define IDM_EDIT_CUT      40030
#define IDM_EDIT_COPY     40031
#define IDM_EDIT_PASTE    40032
#define IDM_EDIT_SELALL   40033
#define IDM_EDIT_SELNONE  40034
#define IDM_EDIT_INVERT   40035
#define IDM_EDIT_DELETE   40036
#define IDM_EDIT_RENAME   40037

// ---- View ----
#define IDM_VIEW_DETAILS  40050
#define IDM_VIEW_TILES    40051
#define IDM_VIEW_LARGE    40052
#define IDM_VIEW_MEDIUM   40053
#define IDM_VIEW_SMALL    40054
#define IDM_VIEW_LIST     40055
#define IDM_VIEW_HIDDEN   40056
#define IDM_VIEW_DETAILSPANE 40057
#define IDM_VIEW_REFRESH  40058

// ---- Go ----
#define IDM_NAV_BACK     40070
#define IDM_NAV_FORWARD  40071
#define IDM_NAV_UP       40072
#define IDM_NAV_REFRESH  40073
#define IDM_ABOUT_APP    40080

// ---- Sort (background context menu) ----
#define IDM_SORT_NAME   40090
#define IDM_SORT_DATE   40091
#define IDM_SORT_TYPE   40092
#define IDM_SORT_SIZE   40093
#define IDM_SORT_ASC    40094
#define IDM_SORT_DESC   40095

// ---- focus / misc accelerators ----
#define IDM_FOCUS_ADDRESS 40096
#define IDM_FOCUS_SEARCH  40097
#define IDM_FOCUS_LIST    40099     // (Tab / F6 out of the preview)

// ---- command bar ----
#define IDC_CMDBAR        1009
#define IDM_SHARE         40098
#define IDM_NEW_MENU      40130
#define IDM_SORT_MENU     40131
#define IDM_VIEW_MENU     40132
#define IDM_MORE_MENU     40133
#define IDM_OPTIONS       40134
#define IDM_COPY_PATH     40135
#define IDM_VIEW_NAVPANE  40136
#define IDM_NEW_WINDOW    40137
#define IDM_VIEW_APPLY_ALL 40138    // this folder's view + columns become every folder's
#define IDM_VIEW_RESET_ALL 40139    // forget every folder's own view + columns
// text size of the navigation pane, the list and the details pane
#define IDM_TEXT_LARGER   40100
#define IDM_TEXT_SMALLER  40101
#define IDM_TEXT_RESET    40102
// Delete / Shift+Delete on the navigation pane's selected folder
#define IDM_TREE_DELETE      40103
#define IDM_TREE_DELETE_PERM 40104
// a second file list beside the first (this tab)
#define IDM_VIEW_DUALPANE    40105
#define IDC_LIST2            1010
// Group by: IDM_GROUP_BASE + GB_* (GB_AUTO .. GB_SIZE)
#define IDM_GROUP_BASE    40140
#define IDM_GROUP_LAST    40145
#define IDM_VIEW_XLARGE      40150
#define IDM_VIEW_CHECKBOXES  40152
#define IDM_VIEW_EXTENSIONS  40153
#define IDM_VIEW_PREVIEWPANE 40154
#define IDM_VIEW_COMPACT     40155
#define IDM_VIEW_CONTENT     40151
#define IDM_TOOLS_MENU       40156
#define IDM_VIEW_CMDBAR      40157
#define IDM_PERF_MARK        40158
#define IDM_VIEW_FOLDERSIZES 40159
#define IDM_SCREENSHOT       40106  // the region picker (Alt+Q from anywhere)
#define IDM_MEDIA_TYPES      40107  // the media file types Explorer Plus opens
#define IDM_NOTEPAD          40108  // the Notepad window (its tabs as left)
#define IDM_PLAYER           40109  // the media player's window (brought forward; an empty one when none)
// Sort by the folder's columns: IDM_SORT_COL_BASE + column (subitem) index
#define IDM_SORT_COL_BASE 40160
#define IDM_SORT_COL_LAST 40199

// ---- tabs ----
#define IDM_TABNEXT   40110
#define IDM_TABPREV   40111
#define IDM_TABSEL1   40120
#define IDM_TABSEL2   40121
#define IDM_TABSEL3   40122
#define IDM_TABSEL4   40123
#define IDM_TABSEL5   40124
#define IDM_TABSEL6   40125
#define IDM_TABSEL7   40126
#define IDM_TABSEL8   40127
#define IDM_TABSEL9   40128
