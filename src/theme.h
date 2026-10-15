#pragma once
#include "appstate.h"

// Win11 look: palette (light/dark + system accent), DPI scaling, fonts and
// anti-aliased drawing helpers shared by the custom-drawn chrome.

struct Palette {
    COLORREF caption;          // tab row (title bar) background, active window
    COLORREF captionInactive;
    COLORREF layer;            // selected tab, navigation row, command bar
    COLORREF content;          // tree / list / details / status background
    COLORREF text;
    COLORREF textSecondary;
    COLORREF textDisabled;
    COLORREF divider;          // 1px separators
    COLORREF hover;            // button hover fill (on `layer`)
    COLORREF pressed;
    COLORREF tabHover;         // inactive tab hover (on `caption`)
    COLORREF boxFill;          // address / search box
    COLORREF boxBorder;
    COLORREF boxBottom;        // text-box bottom stroke when not focused
    COLORREF accent;           // selection / focus accent for this theme
    COLORREF closeHover;       // caption close button
};

extern bool g_dark;                  // apps use the dark theme

void theme_init();                   // read theme + accent, start GDI+
void theme_shutdown();
bool theme_reload();                 // WM_SETTINGCHANGE: true when light/dark, accent or system colours changed
// counts the changes theme_reload found: a window that compares it with the one it last
// applied catches a change another window's theme_reload saw first
unsigned theme_generation();
bool theme_high_contrast();          // High Contrast is on (the palette is the system colours)
const Palette& pal();

// --- DPI ---
int  theme_dpi();                    // DPI of the main window
void theme_set_dpi(int dpi);         // recreates fonts and pushes them to all children
inline int S(int px) { return MulDiv(px, theme_dpi(), 96); }

HFONT glyph_font(int px96);          // Segoe Fluent Icons (MDL2 fallback) at a 96-dpi pixel size
HFONT ui_font_bold();                // semibold UI font (headings)
// for a window with a DPI of its own (the media player): the caller owns the font
HFONT theme_create_font(int dpi, int weight, int pct = 100);   // the UI font at `pct` %
const wchar_t* theme_glyph_face();   // Segoe Fluent Icons, or Segoe MDL2 Assets (Windows 10)

// --- text size of the content (navigation pane, list, details pane) ---
// a percentage of the UI font; the chrome (tabs, address, command bar) keeps its size
int   text_scale();
void  theme_set_text_scale(int pct); // recreates the content fonts and pushes them to tree + list
int   text_scale_step(int pct, int dir);   // the next / previous preset size
HFONT content_font();
HFONT content_font_bold();
inline int SC(int px) { return MulDiv(S(px), text_scale(), 100); }   // content metric

// --- drawing ---
// rounded rectangle (anti-aliased); border = CLR_NONE for none
void fill_rrect(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border = CLR_NONE);
// rounded top corners only, plus Win11's outward "feet" at the bottom (tabs)
void fill_tab_shape(HDC hdc, const RECT& r, int radius, COLORREF fill);
void fill_rect(HDC hdc, const RECT& r, COLORREF c);
void draw_glyph(HDC hdc, wchar_t glyph, const RECT& r, COLORREF color, int px96 = 16);
COLORREF blend(COLORREF a, COLORREF b, double t);
// width of single-line text exactly as DrawText renders it (GetTextExtentPoint32
// under-reports Segoe UI Variable)
int text_width(HDC hdc, const wchar_t* s, int len = -1);

// Double-buffered paint helper: BeginPaint + memory DC, blitted on destruction.
struct BufferedPaint {
    HWND hwnd; PAINTSTRUCT ps; HDC win = nullptr, dc = nullptr;
    HBITMAP bmp = nullptr; HGDIOBJ oldBmp = nullptr; RECT rc{};
    explicit BufferedPaint(HWND h);
    ~BufferedPaint();
};

// apply light/dark to the standard controls (list, tree, header, scrollbars)
void theme_apply_controls();
void theme_style_list(HWND list);    // one list view: dark scrollbars, the palette's colours
void theme_style_control(HWND h);    // a button (or other standard control): light or dark
