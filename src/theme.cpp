#include "theme.h"
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>
#include <uxtheme.h>

bool g_dark = false;

static Palette s_pal{};
static int s_dpi = 96;
static ULONG_PTR s_gdipToken = 0;
static HFONT s_bold = nullptr;
static HFONT s_content = nullptr, s_contentBold = nullptr;
static int s_textScale = 100;
static HFONT s_glyphFonts[64]{};     // per 96-dpi pixel size (index = px), current DPI
static bool s_fluent = true;         // Segoe Fluent Icons present (else MDL2 Assets)

static COLORREF accent_from_registry(bool dark) {
    // AccentPalette: 8 RGBA entries, Light3 Light2 Light1 Base Dark1 Dark2 Dark3 ...
    // Win11 controls use Dark1 on light backgrounds and Light2 on dark ones.
    BYTE pal8[32]{};
    DWORD sz = sizeof(pal8);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal8, &sz) == ERROR_SUCCESS && sz >= 28) {
        int i = dark ? 1 : 4;
        return RGB(pal8[i * 4], pal8[i * 4 + 1], pal8[i * 4 + 2]);
    }
    return dark ? RGB(153, 235, 255) : RGB(0, 95, 184);
}

static bool apps_dark() {
    // EXPLORERPLUS_THEME=light|dark overrides the system choice (screenshots, tests)
    WCHAR over[16]{};
    if (GetEnvironmentVariableW(L"EXPLORERPLUS_THEME", over, 16)) {
        if (lstrcmpiW(over, L"dark") == 0) return true;
        if (lstrcmpiW(over, L"light") == 0) return false;
    }
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

// High Contrast: the user's system colours, not ours
static bool s_highContrast = false;

static bool high_contrast_on() {
    HIGHCONTRASTW hc{ sizeof(hc) };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

static bool dark_now() {
    if (!s_highContrast) return apps_dark();
    COLORREF w = GetSysColor(COLOR_WINDOW);     // (a dark HC theme: dark scrollbars and menus)
    return GetRValue(w) * 299 + GetGValue(w) * 587 + GetBValue(w) * 114 < 128 * 1000;
}

static void build_palette() {
    Palette& p = s_pal;
    if (s_highContrast) {
        COLORREF window = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
        COLORREF face = GetSysColor(COLOR_BTNFACE);
        p.caption = p.captionInactive = p.layer = face;
        p.content = p.boxFill = window;
        p.text = p.textSecondary = text;
        p.textDisabled = GetSysColor(COLOR_GRAYTEXT);
        p.divider = p.boxBorder = p.boxBottom = text;
        // (hover / pressed fills sit under p.text: a shade of the background, not Highlight)
        p.hover = p.tabHover = blend(face, text, 0.15);
        p.pressed = blend(face, text, 0.25);
        p.accent = p.closeHover = GetSysColor(COLOR_HIGHLIGHT);
        return;
    }
    if (g_dark) {
        p.caption         = RGB(32, 32, 32);
        p.captionInactive = RGB(40, 40, 40);
        p.layer           = RGB(44, 44, 44);
        p.content         = RGB(25, 25, 25);
        p.text            = RGB(255, 255, 255);
        p.textSecondary   = RGB(200, 200, 200);
        p.textDisabled    = RGB(115, 115, 115);
        p.divider         = RGB(29, 29, 29);
        p.hover           = RGB(58, 58, 58);
        p.pressed         = RGB(50, 50, 50);
        p.tabHover        = RGB(45, 45, 45);
        p.boxFill         = RGB(55, 55, 55);
        p.boxBorder       = RGB(62, 62, 62);
        p.boxBottom       = RGB(150, 150, 150);
    } else {
        p.caption         = RGB(238, 243, 249);
        p.captionInactive = RGB(243, 243, 243);
        p.layer           = RGB(249, 250, 252);
        p.content         = RGB(255, 255, 255);
        p.text            = RGB(26, 26, 26);
        p.textSecondary   = RGB(96, 96, 96);
        p.textDisabled    = RGB(160, 160, 160);
        p.divider         = RGB(229, 229, 229);
        p.hover           = RGB(236, 237, 240);
        p.pressed         = RGB(228, 229, 232);
        p.tabHover        = RGB(226, 232, 240);
        p.boxFill         = RGB(255, 255, 255);
        p.boxBorder       = RGB(229, 229, 229);
        p.boxBottom       = RGB(134, 134, 134);
    }
    p.accent     = accent_from_registry(g_dark);
    p.closeHover = RGB(196, 43, 28);
}

const Palette& pal() { return s_pal; }

// --- undocumented but stable uxtheme exports for dark scrollbars/menus (1903+) ---
using SetPreferredAppModeFn = int (WINAPI*)(int);
using AllowDarkModeForWindowFn = bool (WINAPI*)(HWND, bool);
using FlushMenuThemesFn = void (WINAPI*)();
static SetPreferredAppModeFn s_setAppMode = nullptr;
static AllowDarkModeForWindowFn s_allowDark = nullptr;
static FlushMenuThemesFn s_flushMenus = nullptr;

static void load_dark_api() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    s_setAppMode = (SetPreferredAppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    s_allowDark  = (AllowDarkModeForWindowFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    s_flushMenus = (FlushMenuThemesFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
}

static void apply_app_mode() {
    if (s_setAppMode) s_setAppMode(s_highContrast ? 0 /*Default*/ : g_dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (s_flushMenus) s_flushMenus();        // popup menus follow the theme
}

void theme_init() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&s_gdipToken, &in, nullptr);
    s_highContrast = high_contrast_on();
    g_dark = dark_now();
    build_palette();
    load_dark_api();
    apply_app_mode();
    // is Segoe Fluent Icons installed (Win11)? otherwise use Segoe MDL2 Assets
    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
    HDC dc = GetDC(nullptr);
    s_fluent = false;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *(bool*)p = true; return 0; }, (LPARAM)&s_fluent, 0);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
}

void theme_shutdown() {
    for (auto& f : s_glyphFonts) if (f) { DeleteObject(f); f = nullptr; }
    if (s_bold) { DeleteObject(s_bold); s_bold = nullptr; }
    if (s_content) { DeleteObject(s_content); s_content = nullptr; }
    if (s_contentBold) { DeleteObject(s_contentBold); s_contentBold = nullptr; }
    if (s_gdipToken) Gdiplus::GdiplusShutdown(s_gdipToken);
}

bool theme_reload() {
    bool hc = high_contrast_on();
    bool hcChanged = hc != s_highContrast;
    s_highContrast = hc;
    bool dark = dark_now();
    Palette old = s_pal;
    bool changed = dark != g_dark;
    g_dark = dark;
    build_palette();
    if (changed || hcChanged) apply_app_mode();
    return changed || hcChanged || memcmp(&old, &s_pal, sizeof(old)) != 0;
}

bool theme_high_contrast() { return s_highContrast; }

int theme_dpi() { return s_dpi; }

static HFONT make_ui_font(int dpi, int weight, int pct = 100) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9 * pct, dpi, 72 * 100);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    for (const wchar_t* face : { L"Segoe UI Variable Text", L"Segoe UI" }) {
        wcscpy_s(lf.lfFaceName, face);
        // CreateFontIndirect never fails for a missing face (it substitutes): check it
        HFONT f = CreateFontIndirectW(&lf);
        HDC m = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(m, f);
        WCHAR got[LF_FACESIZE]{};
        GetTextFaceW(m, LF_FACESIZE, got);
        SelectObject(m, old);
        DeleteDC(m);
        if (lstrcmpiW(got, face) == 0) return f;
        DeleteObject(f);
    }
    lf.lfFaceName[0] = 0;
    return CreateFontIndirectW(&lf);
}

static HFONT make_icon_font(const wchar_t* face, int pt, int dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt, dpi, 72);
    lf.lfWeight = FW_REGULAR;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

// the content fonts at the current DPI and text size; the old ones stay alive until
// the controls have the new ones
static void make_content_fonts(HFONT* oldContent, HFONT* oldBold) {
    *oldContent = s_content;
    *oldBold = s_contentBold;
    s_content = make_ui_font(s_dpi, FW_REGULAR, s_textScale);
    s_contentBold = make_ui_font(s_dpi, FW_SEMIBOLD, s_textScale);
}

// the panes' own windows get the content font, everything else the UI font. Only the
// main window's direct children: nested ones (the breadcrumb's edit, a preview
// handler's window) are their owners' business.
static void push_fonts() {
    if (!g_main) return;
    EnumChildWindows(g_main, [](HWND h, LPARAM) -> BOOL {
        if (GetParent(h) != g_main) return TRUE;
        bool content = h == g_tree || h == g_list;
        SendMessageW(h, WM_SETFONT, (WPARAM)(content ? s_content : g_font), TRUE);
        return TRUE;
    }, 0);
}

void theme_set_dpi(int dpi) {
    if (dpi <= 0) dpi = 96;
    bool first = !g_font;
    if (!first && dpi == s_dpi) return;
    s_dpi = dpi;
    HFONT oldUi = g_font, oldIcon = g_iconFont, oldMdl2 = g_mdl2Font, oldBold = s_bold;
    HFONT oldContent = nullptr, oldContentBold = nullptr;
    g_font = make_ui_font(dpi, FW_REGULAR);
    s_bold = make_ui_font(dpi, FW_SEMIBOLD);
    make_content_fonts(&oldContent, &oldContentBold);
    g_iconFont = make_icon_font(L"Segoe Fluent Icons", 11, dpi);
    g_mdl2Font = make_icon_font(L"Segoe MDL2 Assets", 11, dpi);
    for (auto& f : s_glyphFonts) if (f) { DeleteObject(f); f = nullptr; }
    push_fonts();
    if (oldUi) DeleteObject(oldUi);
    if (oldIcon) DeleteObject(oldIcon);
    if (oldMdl2) DeleteObject(oldMdl2);
    if (oldBold) DeleteObject(oldBold);
    if (oldContent) DeleteObject(oldContent);
    if (oldContentBold) DeleteObject(oldContentBold);
}

HFONT ui_font_bold() { return s_bold ? s_bold : g_font; }

// Explorer-like zoom steps
static const int kTextScales[] = { 80, 90, 100, 110, 125, 150, 175, 200 };

int text_scale() { return s_textScale; }

int text_scale_step(int pct, int dir) {
    if (dir > 0) {
        for (int s : kTextScales) if (s > pct) return s;
        return kTextScales[ARRAYSIZE(kTextScales) - 1];
    }
    for (int i = ARRAYSIZE(kTextScales) - 1; i >= 0; --i) if (kTextScales[i] < pct) return kTextScales[i];
    return kTextScales[0];
}

void theme_set_text_scale(int pct) {
    pct = std::clamp(pct, kTextScales[0], kTextScales[ARRAYSIZE(kTextScales) - 1]);
    if (pct == s_textScale && s_content) return;
    s_textScale = pct;
    if (!g_font) return;                 // before the first theme_set_dpi: made there
    HFONT oldContent = nullptr, oldBold = nullptr;
    make_content_fonts(&oldContent, &oldBold);
    if (g_tree) SendMessageW(g_tree, WM_SETFONT, (WPARAM)s_content, TRUE);
    if (g_list) SendMessageW(g_list, WM_SETFONT, (WPARAM)s_content, TRUE);
    if (oldContent) DeleteObject(oldContent);
    if (oldBold) DeleteObject(oldBold);
}

HFONT content_font() { return s_content ? s_content : g_font; }
HFONT content_font_bold() { return s_contentBold ? s_contentBold : ui_font_bold(); }

HFONT glyph_font(int px96) {
    if (px96 <= 0 || px96 >= 64) px96 = 16;
    if (!s_glyphFonts[px96]) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(px96, s_dpi, 96);
        lf.lfWeight = FW_REGULAR;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcscpy_s(lf.lfFaceName, s_fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
        s_glyphFonts[px96] = CreateFontIndirectW(&lf);
    }
    return s_glyphFonts[px96];
}

// ------------------------------------------------------------------
// drawing

COLORREF blend(COLORREF a, COLORREF b, double t) {
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}

static Gdiplus::Color gp(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void rrect_path(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2;
    if (d <= 0.5f) { path.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void fill_rrect(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    if (fill != CLR_NONE) {
        Gdiplus::GraphicsPath path;
        rrect_path(path, x, y, w, h, (float)radius);
        Gdiplus::SolidBrush b(gp(fill));
        g.FillPath(&b, &path);
    }
    if (border != CLR_NONE) {
        Gdiplus::GraphicsPath path;
        rrect_path(path, x + 0.5f, y + 0.5f, w - 1, h - 1, (float)radius);
        Gdiplus::Pen p(gp(border), 1.0f);
        g.DrawPath(&p, &path);
    }
}

void fill_tab_shape(HDC hdc, const RECT& r, int radius, COLORREF fill) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    float d = (float)radius * 2;                            // feet are `radius` wide
    Gdiplus::GraphicsPath path;
    path.AddArc(x - d, y + h - d, d, d, 90, -90);           // left foot (concave)
    path.AddArc(x, y, d, d, 180, 90);                       // top-left
    path.AddArc(x + w - d, y, d, d, 270, 90);               // top-right
    path.AddArc(x + w, y + h - d, d, d, 180, -90);          // right foot (concave)
    path.CloseFigure();
    Gdiplus::SolidBrush b(gp(fill));
    g.FillPath(&b, &path);
}

int text_width(HDC hdc, const wchar_t* s, int len) {
    RECT m{ 0, 0, 0, 0 };
    DrawTextW(hdc, s, len, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return m.right + 1;
}

void fill_rect(HDC hdc, const RECT& r, COLORREF c) {
    SetDCBrushColor(hdc, c);
    FillRect(hdc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void draw_glyph(HDC hdc, wchar_t glyph, const RECT& r, COLORREF color, int px96) {
    HGDIOBJ of = SelectObject(hdc, glyph_font(px96));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    RECT rr = r;
    wchar_t s[2] = { glyph, 0 };
    DrawTextW(hdc, s, 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
}

BufferedPaint::BufferedPaint(HWND h) : hwnd(h) {
    win = BeginPaint(h, &ps);
    GetClientRect(h, &rc);
    dc = CreateCompatibleDC(win);
    bmp = CreateCompatibleBitmap(win, max(1L, rc.right), max(1L, rc.bottom));
    oldBmp = SelectObject(dc, bmp);
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
}

BufferedPaint::~BufferedPaint() {
    BitBlt(win, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------------
// standard controls

void theme_apply_controls() {
    const Palette& p = pal();
    auto dark_win = [](HWND h, const wchar_t* darkTheme, const wchar_t* lightTheme) {
        if (!h) return;
        bool dark = g_dark && !s_highContrast;        // (High Contrast: the system's own look)
        if (s_allowDark) s_allowDark(h, dark);
        SetWindowTheme(h, dark ? darkTheme : lightTheme, nullptr);
        SendMessageW(h, WM_THEMECHANGED, 0, 0);
    };
    if (g_list) {
        dark_win(g_list, L"DarkMode_Explorer", L"Explorer");      // dark scrollbars
        if (HWND hdr = ListView_GetHeader(g_list)) dark_win(hdr, L"DarkMode_ItemsView", L"Explorer");
        ListView_SetBkColor(g_list, p.content);
        ListView_SetTextBkColor(g_list, p.content);
        ListView_SetTextColor(g_list, p.text);
        InvalidateRect(g_list, nullptr, TRUE);
    }
    if (g_tree) {
        dark_win(g_tree, L"DarkMode_Explorer", L"Explorer");
        TreeView_SetBkColor(g_tree, p.content);
        TreeView_SetTextColor(g_tree, p.text);
        InvalidateRect(g_tree, nullptr, TRUE);
    }
}
