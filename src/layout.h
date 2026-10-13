#pragma once
#include "appstate.h"

enum class DragKind { None, Left, Right, Middle };   // (Middle: between the two lists)

// Win11 rows, top to bottom: caption (tabs) | navigation (buttons, address,
// search) | command bar | tree + list + details | status bar
int layout_caption_h();
int layout_nav_h();
int layout_cmdbar_h();
int layout_status_h();

void layout_create_splitters();   // creates the tree (list created in listview.cpp)
void layout_resize(HWND main, int w, int h);
void layout_relayout();           // resize with the current client size
DragKind layout_splitter_kind(HWND hwnd, POINT pt);   // splitter under a client point
void layout_splitter_drag(HWND hwnd, POINT pt, DragKind kind);
void layout_paint(HDC dc, const RECT& client);         // nav row, search frame, splitter gaps
RECT layout_search_frame();                            // rounded box around the search edit
int  layout_caption_pane_at(POINT pt);                 // two lists: the list whose caption is there, else -1
void layout_pane_caption_changed();                    // a list's folder or the active list changed

extern int  g_splitterRight;       // details pane width
