#include "imageedit.h"
#include "imageview.h"
#include "shapes.h"
#include "theme.h"
#include "resource.h"
#include "shell_utils.h"
#include <gdiplus.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {

namespace gp = Gdiplus;

const wchar_t kClass[] = L"ExplorerPlusEditor";
const UINT WM_TEXT_DONE = WM_APP + 60;      // the text box is done: wParam 1 keep, 0 drop the change

enum class Tool { Select, Rect, Ellipse, Text };

const COLORREF kColors[] = {
    RGB(232, 17, 35), RGB(247, 99, 12), RGB(255, 196, 0), RGB(16, 137, 62),
    RGB(0, 120, 215), RGB(136, 23, 152), RGB(0, 0, 0), RGB(255, 255, 255),
};
const wchar_t* const kColorNames[] = { L"Red", L"Orange", L"Yellow", L"Green", L"Blue", L"Purple", L"Black", L"White" };
constexpr int kColorCount = ARRAYSIZE(kColors);
// line widths and text sizes offered, in units of the picture's size (1: up to 1920 pixels)
const int kWidths[] = { 1, 2, 4, 6, 8, 12, 16 };
const int kSizes[] = { 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128 };

// the toolbar's buttons
enum Btn : int {
    B_SELECT, B_RECT, B_ELLIPSE, B_TEXT,
    B_COLOR,                                    // .. + kColorCount - 1
    B_MORE = B_COLOR + kColorCount,
    B_FILL, B_WIDTH, B_SIZE,
    B_UNDO, B_REDO, B_DELETE,
    B_COPY, B_SAVE, B_SAVEAS,
    B_COUNT
};

struct Editor {
    HWND wnd = nullptr, edit = nullptr, tip = nullptr;
    int dpi = 96;
    HFONT font = nullptr, glyphs = nullptr, editFont = nullptr;
    HBRUSH editBrush = nullptr;
    COLORREF editBrushColor = CLR_INVALID;
    // the picture
    std::shared_ptr<const std::vector<uint32_t>> px;    // premultiplied BGRA, w x h
    int w = 0, h = 0;
    std::wstring path;              // the file it is (saved to); empty: a screenshot not saved yet
    std::wstring name;              // in the title
    bool shot = false;              // made from a screenshot: the screenshot icon, its own taskbar button
    bool dirty = false;
    bool copied = false;            // (a screenshot copied, nothing changed since: closed without asking)
    // the view: picture pixels to client ones (x * k + ox)
    double k = 1, ox = 0, oy = 0;
    RECT canvas{};
    HBITMAP view = nullptr;         // the picture drawn at k
    int viewW = 0, viewH = 0;
    // the objects; the states before each change
    std::vector<Shape> shapes;
    std::vector<std::vector<Shape>> undo, redo;
    bool nudging = false;           // (arrow keys moving the selection: one undo step for them all)
    int sel = -1;
    // what new objects get (choosing one changes the selected object too)
    Tool tool = Tool::Rect;
    COLORREF color = kColors[0];
    bool fill = false;
    double unit = 1;                // kWidths / kSizes times this
    double width = 4, textSize = 24;
    // the mouse
    enum class Drag { None, New, Move, Resize, Button } drag = Drag::None;
    double ax = 0, ay = 0;          // where it started (picture pixels)
    int handle = -1;
    Shape before;                   // the selected object as the drag found it
    std::vector<Shape> beforeAll;
    bool moved = false;
    // the text box: a new text, or one changed (`editing`)
    int editing = -1;
    double editX = 0, editY = 0, editSize = 24;
    COLORREF editColor = 0;
    // the toolbar
    int barH = 0;
    RECT btn[B_COUNT]{};
    int hot = -1, pressed = -1;
    COLORREF custom[16]{};
};

std::vector<Editor*> s_editors;

int px(const Editor& e, int v) { return MulDiv(v, e.dpi, 96); }

// ------------------------------------------------------------------
// drawing the objects (on screen and into the file alike)
// ------------------------------------------------------------------
gp::FontFamily& text_family() {
    static gp::FontFamily* ff = [] {
        auto* f = new gp::FontFamily(L"Segoe UI Semibold");
        if (f->GetLastStatus() != gp::Ok) { delete f; f = new gp::FontFamily(L"Segoe UI"); }
        return f;
    }();
    return *ff;
}

const gp::StringFormat* text_format() {
    static gp::StringFormat* f = [] {
        auto* s = new gp::StringFormat(gp::StringFormat::GenericTypographic());
        s->SetFormatFlags(s->GetFormatFlags() | gp::StringFormatFlagsMeasureTrailingSpaces);
        return s;
    }();
    return f;
}

gp::Color gcolor(COLORREF c, BYTE a = 255) { return gp::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// a text object's box from its text and size
void measure_text(Shape& s) {
    HDC dc = GetDC(nullptr);
    {
        gp::Graphics g(dc);
        g.SetPageUnit(gp::UnitPixel);
        gp::Font f(&text_family(), (gp::REAL)s.size, gp::FontStyleRegular, gp::UnitPixel);
        std::wstring t = s.text.empty() ? L" " : s.text;
        if (t.back() == L'\n') t += L' ';           // (a last empty line counts)
        gp::RectF box;
        g.MeasureString(t.c_str(), (INT)t.size(), &f, gp::PointF(0, 0), text_format(), &box);
        s.x1 = s.x0 + std::max<double>(box.Width, s.size * 0.3);
        s.y1 = s.y0 + std::max<double>(box.Height, s.size);
    }
    ReleaseDC(nullptr, dc);
}

void draw_shape(gp::Graphics& g, const Shape& s) {
    gp::Color c = gcolor(s.color);
    if (s.kind == ShapeKind::Text) {
        gp::Font f(&text_family(), (gp::REAL)s.size, gp::FontStyleRegular, gp::UnitPixel);
        gp::SolidBrush b(c);
        g.DrawString(s.text.c_str(), (INT)s.text.size(), &f, gp::PointF((gp::REAL)s.x0, (gp::REAL)s.y0), text_format(), &b);
        return;
    }
    gp::RectF r((gp::REAL)s.x0, (gp::REAL)s.y0, (gp::REAL)(s.x1 - s.x0), (gp::REAL)(s.y1 - s.y0));
    if (s.fill) {
        gp::SolidBrush b(c);
        if (s.kind == ShapeKind::Rect) g.FillRectangle(&b, r);
        else g.FillEllipse(&b, r);
    }
    gp::Pen pen(c, (gp::REAL)s.width);
    pen.SetLineJoin(gp::LineJoinMiter);
    if (s.kind == ShapeKind::Rect) g.DrawRectangle(&pen, r);
    else g.DrawEllipse(&pen, r);
}

void prepare(gp::Graphics& g) {
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
}

// the picture with the objects drawn in, at its own size (`opaque`: on white, for JPEG)
std::unique_ptr<gp::Bitmap> flatten(Editor& e, bool opaque) {
    auto out = std::make_unique<gp::Bitmap>(e.w, e.h, PixelFormat32bppARGB);
    gp::Graphics g(out.get());
    g.Clear(opaque ? gp::Color(255, 255, 255, 255) : gp::Color(0, 0, 0, 0));
    gp::Bitmap src(e.w, e.h, e.w * 4, PixelFormat32bppPARGB, (BYTE*)const_cast<uint32_t*>(e.px->data()));
    g.SetCompositingMode(opaque ? gp::CompositingModeSourceOver : gp::CompositingModeSourceCopy);
    g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.DrawImage(&src, gp::Rect(0, 0, e.w, e.h), 0, 0, e.w, e.h, gp::UnitPixel);
    g.SetCompositingMode(gp::CompositingModeSourceOver);
    prepare(g);
    for (auto& s : e.shapes) draw_shape(g, s);
    return out;
}

// ------------------------------------------------------------------
// files and the clipboard
// ------------------------------------------------------------------
bool encoder(const wchar_t* mime, CLSID& out) {
    UINT n = 0, size = 0;
    if (gp::GetImageEncodersSize(&n, &size) != gp::Ok || !size) return false;
    std::vector<BYTE> buf(size);
    auto* info = reinterpret_cast<gp::ImageCodecInfo*>(buf.data());
    if (gp::GetImageEncoders(n, size, info) != gp::Ok) return false;
    for (UINT i = 0; i < n; ++i)
        if (lstrcmpiW(info[i].MimeType, mime) == 0) { out = info[i].Clsid; return true; }
    return false;
}

// the type a file is written as, by its extension; nullptr: not one written here
const wchar_t* mime_for(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    static const struct { const wchar_t* ext; const wchar_t* mime; } kTypes[] = {
        { L".png", L"image/png" }, { L".jpg", L"image/jpeg" }, { L".jpeg", L"image/jpeg" }, { L".jpe", L"image/jpeg" },
        { L".jfif", L"image/jpeg" }, { L".bmp", L"image/bmp" }, { L".dib", L"image/bmp" }, { L".gif", L"image/gif" },
        { L".tif", L"image/tiff" }, { L".tiff", L"image/tiff" },
    };
    for (auto& t : kTypes)
        if (lstrcmpiW(ext, t.ext) == 0) return t.mime;
    return nullptr;
}

bool write_file(Editor& e, const std::wstring& file) {
    const wchar_t* mime = mime_for(file);
    CLSID clsid;
    if (!mime || !encoder(mime, clsid)) return false;
    bool jpeg = lstrcmpW(mime, L"image/jpeg") == 0;
    auto bmp = flatten(e, jpeg);
    // (written beside it, then put in its place: a failed write leaves the old one)
    std::wstring tmp = file + L".saving~";
    gp::Status st;
    if (jpeg) {
        ULONG quality = 95;
        gp::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = gp::EncoderQuality;
        params.Parameter[0].Type = gp::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &quality;
        st = bmp->Save(tmp.c_str(), &clsid, &params);
    } else {
        st = bmp->Save(tmp.c_str(), &clsid, nullptr);
    }
    bmp.reset();
    bool ok = st == gp::Ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::wstring title_of(const std::wstring& file) {
    std::wstring s = PathFindFileNameW(file.c_str());
    return s;
}

void update_title(Editor& e) {
    std::wstring t = (e.dirty ? L"*" : L"") + e.name + L" - Explorer Plus";
    SetWindowTextW(e.wnd, t.c_str());
}

void set_dirty(Editor& e, bool on) {
    if (on) e.copied = false;
    if (e.dirty == on) return;
    e.dirty = on;
    update_title(e);
}

// the Save dialog: the file chosen, empty when cancelled
std::wstring ask_file(Editor& e) {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    const COMDLG_FILTERSPEC types[] = {
        { L"PNG", L"*.png" }, { L"JPEG", L"*.jpg;*.jpeg;*.jpe;*.jfif" }, { L"Bitmap", L"*.bmp" },
        { L"GIF", L"*.gif" }, { L"TIFF", L"*.tif;*.tiff" },
    };
    static const wchar_t* const kExt[] = { L"png", L"jpg", L"bmp", L"gif", L"tif" };
    d->SetFileTypes(ARRAYSIZE(types), types);
    UINT type = 1;
    if (const wchar_t* m = mime_for(e.path)) {
        static const wchar_t* const kMime[] = { L"image/png", L"image/jpeg", L"image/bmp", L"image/gif", L"image/tiff" };
        for (UINT i = 0; i < ARRAYSIZE(kMime); ++i)
            if (lstrcmpW(m, kMime[i]) == 0) type = i + 1;
    }
    d->SetFileTypeIndex(type);
    d->SetDefaultExtension(kExt[type - 1]);
    d->SetTitle(e.path.empty() ? L"Save screenshot" : L"Save picture as");
    std::wstring base = e.name;
    if (!e.path.empty()) {
        size_t dot = base.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0) base.resize(dot);
    }
    d->SetFileName(base.c_str());
    FILEOPENDIALOGOPTIONS opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    // its own folder; a screenshot: Pictures\Screenshots, as Windows keeps them
    std::wstring dir;
    if (!e.path.empty()) {
        dir = e.path.substr(0, e.path.find_last_of(L"\\/"));
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &p)) ||
            SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)))
            dir = p;
        CoTaskMemFree(p);
    }
    IShellItem* folder = nullptr;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        d->SetFolder(folder);
        folder->Release();
    }
    std::wstring out;
    IShellItem* item = nullptr;
    if (SUCCEEDED(d->Show(e.wnd)) && SUCCEEDED(d->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
        item->Release();
    }
    d->Release();
    return out;
}

void end_text(Editor& e, bool keep);

bool save_as(Editor& e) {
    if (e.edit) end_text(e, true);
    std::wstring file = ask_file(e);
    if (file.empty()) return false;
    if (!mime_for(file)) file += L".png";
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = write_file(e, file);
    SetCursor(old);
    if (!ok) {
        MessageBoxW(e.wnd, (L"The picture couldn't be saved:\n" + file).c_str(), L"Explorer Plus", MB_OK | MB_ICONWARNING);
        return false;
    }
    e.path = file;
    e.name = title_of(file);
    e.dirty = true;                 // (the title changes either way)
    set_dirty(e, false);
    return true;
}

// Save: over its file where it is a type written here; else (a screenshot, a WebP...) Save as
bool save(Editor& e) {
    if (e.edit) end_text(e, true);
    if (e.path.empty() || !mime_for(e.path)) return save_as(e);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = write_file(e, e.path);
    SetCursor(old);
    if (!ok) {
        MessageBoxW(e.wnd, (L"The picture couldn't be saved:\n" + e.path).c_str(), L"Explorer Plus", MB_OK | MB_ICONWARNING);
        return false;
    }
    set_dirty(e, false);
    return true;
}

// the picture as drawn: a DIB (any program), and a PNG (keeps transparency)
void copy(Editor& e) {
    if (e.edit) end_text(e, true);
    auto bmp = flatten(e, false);
    gp::Rect r(0, 0, e.w, e.h);
    gp::BitmapData bd{};
    if (bmp->LockBits(&r, gp::ImageLockModeRead, PixelFormat32bppARGB, &bd) != gp::Ok) return;
    size_t row = (size_t)e.w * 4, bytes = row * e.h;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (dib) {
        auto* bih = (BITMAPINFOHEADER*)GlobalLock(dib);
        *bih = { sizeof(BITMAPINFOHEADER), e.w, e.h, 1, 32, BI_RGB, (DWORD)bytes };
        BYTE* dst = (BYTE*)(bih + 1);
        for (int y = 0; y < e.h; ++y)                // (bottom-up)
            memcpy(dst + row * (e.h - 1 - y), (BYTE*)bd.Scan0 + (size_t)bd.Stride * y, row);
        GlobalUnlock(dib);
    }
    bmp->UnlockBits(&bd);
    HGLOBAL png = nullptr;
    CLSID clsid;
    IStream* st = nullptr;
    if (encoder(L"image/png", clsid) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, FALSE, &st))) {
        if (bmp->Save(st, &clsid, nullptr) != gp::Ok || FAILED(GetHGlobalFromStream(st, &png))) png = nullptr;
        st->Release();
    }
    bool put = false;
    if (OpenClipboard(e.wnd)) {
        EmptyClipboard();
        if (dib && SetClipboardData(CF_DIB, dib)) { dib = nullptr; put = true; }
        if (png && SetClipboardData(RegisterClipboardFormatW(L"PNG"), png)) { png = nullptr; put = true; }
        CloseClipboard();
    }
    if (dib) GlobalFree(dib);
    if (png) GlobalFree(png);
    if (put && e.path.empty()) e.copied = true;
}

// ------------------------------------------------------------------
// changes
// ------------------------------------------------------------------
void invalidate_canvas(Editor& e) { InvalidateRect(e.wnd, &e.canvas, FALSE); }
void invalidate_bar(Editor& e) {
    RECT r{ 0, 0, 0x7FFF, e.barH };
    InvalidateRect(e.wnd, &r, FALSE);
}

void push_undo(Editor& e, std::vector<Shape> state) {
    e.undo.push_back(std::move(state));
    if (e.undo.size() > 300) e.undo.erase(e.undo.begin());
    e.redo.clear();
    e.nudging = false;
    set_dirty(e, true);
    invalidate_bar(e);
}

void checkpoint(Editor& e) { push_undo(e, e.shapes); }

void select(Editor& e, int i) {
    e.sel = i >= 0 && i < (int)e.shapes.size() ? i : -1;
    if (e.sel >= 0) {                       // (the toolbar shows what it has)
        const Shape& s = e.shapes[e.sel];
        e.color = s.color;
        if (s.kind == ShapeKind::Text) e.textSize = s.size;
        else { e.fill = s.fill; e.width = s.width; }
    }
    e.nudging = false;
    invalidate_canvas(e);
    invalidate_bar(e);
}

void undo_redo(Editor& e, bool undo) {
    if (e.edit) end_text(e, true);
    auto& from = undo ? e.undo : e.redo;
    auto& to = undo ? e.redo : e.undo;
    if (from.empty()) return;
    to.push_back(std::move(e.shapes));
    e.shapes = std::move(from.back());
    from.pop_back();
    e.sel = -1;
    e.nudging = false;
    set_dirty(e, true);
    invalidate_canvas(e);
    invalidate_bar(e);
}

void delete_selected(Editor& e) {
    if (e.sel < 0) return;
    checkpoint(e);
    e.shapes.erase(e.shapes.begin() + e.sel);
    select(e, -1);
}

void text_font(Editor& e);

void set_color(Editor& e, COLORREF c) {
    e.color = c;
    if (e.edit) {
        e.editColor = c;
        InvalidateRect(e.edit, nullptr, TRUE);
    } else if (e.sel >= 0 && e.shapes[e.sel].color != c) {
        checkpoint(e);
        e.shapes[e.sel].color = c;
        invalidate_canvas(e);
    }
    invalidate_bar(e);
}

void set_fill(Editor& e, bool on) {
    e.fill = on;
    if (e.sel >= 0 && !e.edit && e.shapes[e.sel].kind != ShapeKind::Text && e.shapes[e.sel].fill != on) {
        checkpoint(e);
        e.shapes[e.sel].fill = on;
        invalidate_canvas(e);
    }
    invalidate_bar(e);
}

void set_width(Editor& e, double wd) {
    e.width = wd;
    if (e.sel >= 0 && !e.edit && e.shapes[e.sel].kind != ShapeKind::Text && e.shapes[e.sel].width != wd) {
        checkpoint(e);
        e.shapes[e.sel].width = wd;
        invalidate_canvas(e);
    }
    invalidate_bar(e);
}

void fit_text_box(Editor& e);

void set_size(Editor& e, double size) {
    e.textSize = size;
    if (e.edit) {
        e.editSize = size;
        text_font(e);
        fit_text_box(e);
    } else if (e.sel >= 0 && e.shapes[e.sel].kind == ShapeKind::Text && e.shapes[e.sel].size != size) {
        checkpoint(e);
        e.shapes[e.sel].size = size;
        measure_text(e.shapes[e.sel]);
        invalidate_canvas(e);
    }
    invalidate_bar(e);
}

void set_tool(Editor& e, Tool t) {
    if (e.edit) end_text(e, true);
    e.tool = t;
    // a drawing tool: the colour and fill picked next are for the next object, not the last one
    if (t != Tool::Select && e.sel >= 0) select(e, -1);
    invalidate_bar(e);
}

// ------------------------------------------------------------------
// the text box: typed into where the text goes, at its size and colour
// ------------------------------------------------------------------
LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    HWND owner = GetParent(h);
    bool shift = GetKeyState(VK_SHIFT) < 0;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { PostMessageW(owner, WM_TEXT_DONE, 0, 0); return 0; }
            if (wp == VK_RETURN && !shift) { PostMessageW(owner, WM_TEXT_DONE, 1, 0); return 0; }   // (Shift+Enter: a new line)
            break;
        case WM_CHAR:
            if (wp == VK_ESCAPE || (wp == L'\r' && !shift)) return 0;
            break;
        case WM_KILLFOCUS: PostMessageW(owner, WM_TEXT_DONE, 1, 0); break;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void text_font(Editor& e) {
    HFONT old = e.editFont;
    LOGFONTW lf{};
    lf.lfHeight = -std::max(6, (int)std::lround(e.editSize * e.k));
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    e.editFont = CreateFontIndirectW(&lf);
    if (e.edit) {
        SendMessageW(e.edit, WM_SETFONT, (WPARAM)e.editFont, TRUE);
        SendMessageW(e.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    if (old) DeleteObject(old);
}

std::wstring edit_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring t(n + 1, L'\0');
    GetWindowTextW(h, t.data(), n + 1);
    t.resize(n);
    std::wstring out;
    for (wchar_t c : t) if (c != L'\r') out += c;
    return out;
}

// the box as big as its text (and a little room to type on)
void fit_text_box(Editor& e) {
    if (!e.edit) return;
    std::wstring t = edit_text(e.edit);
    if (t.empty() || t.back() == L'\n') t += L'W';
    HDC dc = GetDC(e.edit);
    HGDIOBJ of = SelectObject(dc, e.editFont);
    RECT r{ 0, 0, 0, 0 };
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(e.edit, dc);
    int x = (int)std::lround(e.ox + e.editX * e.k), y = (int)std::lround(e.oy + e.editY * e.k);
    int wd = r.right + tm.tmAveCharWidth * 2 + px(e, 4), ht = r.bottom + px(e, 2);
    SetWindowPos(e.edit, HWND_TOP, x, y, wd, ht, SWP_NOACTIVATE);
}

// a text typed at (x, y) (picture pixels), or text object `shape` changed
void begin_text(Editor& e, int shape, double x, double y) {
    if (e.edit) end_text(e, true);
    e.editing = shape;
    if (shape >= 0) {
        select(e, shape);
        const Shape& s = e.shapes[shape];
        x = s.x0;
        y = s.y0;
        e.editSize = s.size;
        e.editColor = s.color;
    } else {
        select(e, -1);
        e.editSize = e.textSize;
        e.editColor = e.color;
    }
    e.editX = x;
    e.editY = y;
    e.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_WANTRETURN,
                             0, 0, 0, 0, e.wnd, nullptr, g_hinst, nullptr);
    if (!e.edit) return;
    SetWindowSubclass(e.edit, edit_proc, 1, 0);
    text_font(e);
    if (shape >= 0) {
        std::wstring t;
        for (wchar_t c : e.shapes[shape].text) { if (c == L'\n') t += L'\r'; t += c; }
        SetWindowTextW(e.edit, t.c_str());
        SendMessageW(e.edit, EM_SETSEL, 0, -1);
    }
    fit_text_box(e);
    ShowWindow(e.edit, SW_SHOW);
    SetFocus(e.edit);
    invalidate_canvas(e);
}

void end_text(Editor& e, bool keep) {
    if (!e.edit) return;
    HWND ed = e.edit;
    e.edit = nullptr;                       // (first: losing the focus posts WM_TEXT_DONE again)
    std::wstring t = edit_text(ed);
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (GetFocus() == ed) SetFocus(e.wnd);
    DestroyWindow(ed);
    int i = e.editing;
    e.editing = -1;
    if (keep) {
        if (i >= 0 && i < (int)e.shapes.size()) {
            Shape s = e.shapes[i];
            if (t.empty()) {
                checkpoint(e);
                e.shapes.erase(e.shapes.begin() + i);
                e.sel = -1;
            } else if (t != s.text || s.size != e.editSize || s.color != e.editColor) {
                checkpoint(e);
                s.text = t;
                s.size = e.editSize;
                s.color = e.editColor;
                measure_text(s);
                e.shapes[i] = s;
            }
        } else if (!t.empty()) {
            checkpoint(e);
            Shape s;
            s.kind = ShapeKind::Text;
            s.x0 = e.editX;
            s.y0 = e.editY;
            s.color = e.editColor;
            s.size = e.editSize;
            s.text = t;
            measure_text(s);
            e.shapes.push_back(s);
            e.sel = (int)e.shapes.size() - 1;
        }
    }
    invalidate_canvas(e);
    invalidate_bar(e);
}

// ------------------------------------------------------------------
// layout and painting
// ------------------------------------------------------------------
// the buttons from the left, copy and save at the right end; `compact`: narrower, for a narrow
// window. Returns the width it needs.
int layout_bar(Editor& e, int width, bool compact) {
    int bh = px(e, 36), y = (e.barH - bh) / 2, x = px(e, 8);
    int tool = px(e, compact ? 32 : 40), sp = px(e, 2);
    auto put = [&](int b, int wd) { e.btn[b] = { x, y, x + wd, y + bh }; x += wd + sp; };
    auto gap = [&] { x += px(e, compact ? 4 : 14); };
    for (int b = B_SELECT; b <= B_TEXT; ++b) put(b, tool);
    gap();
    for (int i = 0; i < kColorCount; ++i) put(B_COLOR + i, px(e, compact ? 26 : 30));
    put(B_MORE, px(e, compact ? 32 : 36));
    gap();
    put(B_FILL, tool);
    put(B_WIDTH, px(e, compact ? 52 : 60));
    put(B_SIZE, px(e, compact ? 60 : 66));
    gap();
    put(B_UNDO, tool);
    put(B_REDO, tool);
    put(B_DELETE, tool);
    int right = 3 * tool + 2 * sp + px(e, 8);
    int need = x + px(e, 12) + right;
    x = std::max(x + px(e, 12), width - right);
    put(B_COPY, tool);
    put(B_SAVE, tool);
    put(B_SAVEAS, tool);
    return need;
}

void layout(Editor& e) {
    RECT rc{};
    GetClientRect(e.wnd, &rc);
    e.barH = px(e, 52);
    if (layout_bar(e, rc.right, false) > rc.right) layout_bar(e, rc.right, true);
    for (int b = 0; b < B_COUNT; ++b) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = e.wnd;
        ti.uId = (UINT_PTR)b + 1;
        ti.rect = e.btn[b];
        SendMessageW(e.tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
    // the picture fitted to the rest, never enlarged
    e.canvas = { 0, e.barH, rc.right, rc.bottom };
    int m = px(e, 16);
    double aw = std::max(1L, rc.right - 2 * m), ah = std::max(1L, rc.bottom - e.barH - 2 * m);
    e.k = std::min({ 1.0, aw / e.w, ah / e.h });
    double dw = std::max(1.0, std::round(e.w * e.k)), dh = std::max(1.0, std::round(e.h * e.k));
    e.ox = std::floor((rc.right - dw) / 2);
    e.oy = std::floor(e.barH + (rc.bottom - e.barH - dh) / 2);
    if (e.edit) { text_font(e); fit_text_box(e); }
}

void make_view(Editor& e, int dw, int dh) {
    if (e.view && e.viewW == dw && e.viewH == dh) return;
    if (e.view) DeleteObject(e.view);
    e.view = nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), dw, -dh, 1, 32, BI_RGB };
    void* bits = nullptr;
    e.view = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!e.view) return;
    gp::Bitmap src(e.w, e.h, e.w * 4, PixelFormat32bppPARGB, (BYTE*)const_cast<uint32_t*>(e.px->data()));
    gp::Bitmap out(dw, dh, dw * 4, PixelFormat32bppPARGB, (BYTE*)bits);
    gp::Graphics g(&out);
    g.SetCompositingMode(gp::CompositingModeSourceCopy);
    g.SetInterpolationMode(dw == e.w && dh == e.h ? gp::InterpolationModeNearestNeighbor : gp::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHighQuality);
    gp::ImageAttributes clampEdges;
    clampEdges.SetWrapMode(gp::WrapModeTileFlipXY);
    g.DrawImage(&src, gp::Rect(0, 0, dw, dh), 0, 0, e.w, e.h, gp::UnitPixel, &clampEdges);
    e.viewW = dw;
    e.viewH = dh;
}

bool enabled(const Editor& e, int b) {
    switch (b) {
        case B_UNDO: return !e.undo.empty();
        case B_REDO: return !e.redo.empty();
        case B_DELETE: return e.sel >= 0 && !e.edit;
    }
    return true;
}

bool toggled(const Editor& e, int b) {
    switch (b) {
        case B_SELECT: return e.tool == Tool::Select;
        case B_RECT: return e.tool == Tool::Rect;
        case B_ELLIPSE: return e.tool == Tool::Ellipse;
        case B_TEXT: return e.tool == Tool::Text;
        case B_FILL: return e.fill;
    }
    return false;
}

void glyph(Editor& e, HDC dc, wchar_t g, const RECT& r, COLORREF c) {
    HGDIOBJ of = SelectObject(dc, e.glyphs);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT rr = r;
    wchar_t s[2] = { g, 0 };
    DrawTextW(dc, s, 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

// the tools' pictures: an arrow, a rectangle, an ellipse (drawn: no glyph says them well)
void tool_icon(Editor& e, gp::Graphics& g, int b, const RECT& r, COLORREF c) {
    float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f, u = (float)e.dpi / 96.0f;
    gp::Pen pen(gcolor(c), 1.4f * u);
    pen.SetLineJoin(gp::LineJoinRound);
    if (b == B_SELECT) {
        const float pts[][2] = { { -4, -8 }, { -4, 6 }, { -0.5f, 2.5f }, { 2, 8 }, { 4, 7 }, { 1.5f, 1.5f }, { 6, 1.5f } };
        gp::PointF p[ARRAYSIZE(pts)];
        for (size_t i = 0; i < ARRAYSIZE(pts); ++i) p[i] = gp::PointF(cx + pts[i][0] * u, cy + pts[i][1] * u);
        g.DrawPolygon(&pen, p, ARRAYSIZE(pts));
    } else if (b == B_RECT) {
        g.DrawRectangle(&pen, cx - 8 * u, cy - 6 * u, 16 * u, 12 * u);
    } else if (b == B_ELLIPSE) {
        g.DrawEllipse(&pen, cx - 8.5f * u, cy - 6.5f * u, 17 * u, 13 * u);
    }
}

void paint_bar(Editor& e, HDC dc, const RECT& rc) {
    const Palette& c = pal();
    RECT bar{ 0, 0, rc.right, e.barH };
    fill_rect(dc, bar, c.layer);
    RECT line{ 0, e.barH - 1, rc.right, e.barH };
    fill_rect(dc, line, c.divider);
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    for (int b = 0; b < B_COUNT; ++b) {
        const RECT& r = e.btn[b];
        bool en = enabled(e, b), hot = e.hot == b && en, down = hot && e.pressed == b;
        COLORREF fg = en ? c.text : c.textDisabled;
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (toggled(e, b)) fill_rrect(dc, r, px(e, 4), down ? c.pressed : blend(c.layer, c.text, hot ? 0.16 : 0.11));
        else if (hot) fill_rrect(dc, r, px(e, 4), down ? c.pressed : c.hover);
        if (b >= B_COLOR && b < B_COLOR + kColorCount) {
            COLORREF col = kColors[b - B_COLOR];
            int d = std::min(px(e, 18), (int)(r.right - r.left) - px(e, 8));
            RECT sw{ cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d };
            if (col == e.color) {           // (the colour in use: ringed)
                RECT ring = sw;
                InflateRect(&ring, px(e, 4), px(e, 4));
                fill_rrect(dc, ring, (ring.right - ring.left) / 2, c.text);
                InflateRect(&ring, -px(e, 2), -px(e, 2));
                fill_rrect(dc, ring, (ring.right - ring.left) / 2, hot ? c.hover : c.layer);
            }
            fill_rrect(dc, sw, d / 2, col, blend(col, c.text, 0.45));
            continue;
        }
        switch (b) {
            case B_SELECT: case B_RECT: case B_ELLIPSE: tool_icon(e, g, b, r, fg); break;
            case B_TEXT: glyph(e, dc, L'\xE8D2', r, fg); break;
            case B_MORE: glyph(e, dc, L'\xE790', r, fg); break;
            case B_FILL: {                  // a square in the colour: filled or not
                int d = px(e, 16);
                RECT sq{ cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d };
                COLORREF edge = e.color == c.layer ? c.text : e.color;
                fill_rrect(dc, sq, px(e, 2), e.fill ? e.color : CLR_NONE, edge);
                if (!e.fill) { InflateRect(&sq, -1, -1); fill_rrect(dc, sq, px(e, 2), CLR_NONE, edge); }
                break;
            }
            case B_WIDTH: {                 // a line as thick as the width (its place among the choices)
                int i = 0;
                for (int j = 0; j < (int)ARRAYSIZE(kWidths); ++j)
                    if (kWidths[j] * e.unit <= e.width + 0.01) i = j;
                float t = (1.0f + i * 0.9f) * e.dpi / 96.0f;
                gp::Pen pen(gcolor(fg), t);
                g.DrawLine(&pen, (gp::REAL)r.left + px(e, 8), (gp::REAL)cy, (gp::REAL)r.right - px(e, 22), (gp::REAL)cy);
                RECT ch{ r.right - px(e, 20), r.top, r.right - px(e, 4), r.bottom };
                glyph(e, dc, L'\xE70D', ch, fg);
                break;
            }
            case B_SIZE: {
                RECT tr{ r.left + px(e, 6), r.top, r.right - px(e, 20), r.bottom };
                HGDIOBJ of = SelectObject(dc, e.font);
                SetTextColor(dc, fg);
                SetBkMode(dc, TRANSPARENT);
                std::wstring t = L"A " + std::to_wstring((int)std::lround(e.textSize));
                DrawTextW(dc, t.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, of);
                RECT ch{ r.right - px(e, 20), r.top, r.right - px(e, 4), r.bottom };
                glyph(e, dc, L'\xE70D', ch, fg);
                break;
            }
            case B_UNDO: glyph(e, dc, L'\xE7A7', r, fg); break;
            case B_REDO: glyph(e, dc, L'\xE7A6', r, fg); break;
            case B_DELETE: glyph(e, dc, L'\xE74D', r, fg); break;
            case B_COPY: glyph(e, dc, L'\xE8C8', r, fg); break;
            case B_SAVE: glyph(e, dc, L'\xE74E', r, fg); break;
            case B_SAVEAS: glyph(e, dc, L'\xE792', r, fg); break;
        }
    }
}

void paint(Editor& e) {
    BufferedPaint bp(e.wnd);
    HDC dc = bp.dc;
    const Palette& c = pal();
    RECT canvas{ 0, e.barH, bp.rc.right, bp.rc.bottom };
    fill_rect(dc, canvas, blend(c.content, RGB(128, 128, 128), 0.12));
    int dw = std::max(1, (int)std::lround(e.w * e.k)), dh = std::max(1, (int)std::lround(e.h * e.k));
    int x = (int)e.ox, y = (int)e.oy;
    make_view(e, dw, dh);
    if (e.view) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, e.view);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };      // (a transparent picture: on the background)
        AlphaBlend(dc, x, y, dw, dh, mem, 0, 0, dw, dh, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    {
        gp::Graphics g(dc);
        prepare(g);
        g.SetClip(gp::Rect(x, y, dw, dh));
        g.TranslateTransform((gp::REAL)e.ox, (gp::REAL)e.oy);
        g.ScaleTransform((gp::REAL)e.k, (gp::REAL)e.k);
        for (int i = 0; i < (int)e.shapes.size(); ++i)
            if (!(e.edit && i == e.editing)) draw_shape(g, e.shapes[i]);
        g.ResetTransform();
        g.ResetClip();
        // the selected object: a frame, and the handles that resize it
        if (e.sel >= 0 && e.sel < (int)e.shapes.size() && !(e.edit && e.sel == e.editing)) {
            const Shape& s = e.shapes[e.sel];
            bool text = s.kind == ShapeKind::Text;
            double pad = text ? px(e, 4) : 0;
            gp::RectF box((gp::REAL)(e.ox + s.x0 * e.k - pad), (gp::REAL)(e.oy + s.y0 * e.k - pad),
                          (gp::REAL)((s.x1 - s.x0) * e.k + 2 * pad), (gp::REAL)((s.y1 - s.y0) * e.k + 2 * pad));
            gp::Pen under(gp::Color(160, 255, 255, 255), 1.0f), dash(gcolor(c.accent), 1.0f);
            dash.SetDashStyle(gp::DashStyleDash);
            g.DrawRectangle(&under, box);
            g.DrawRectangle(&dash, box);
            if (!text) {
                gp::SolidBrush face(gp::Color(255, 255, 255, 255));
                gp::Pen edge(gcolor(c.accent), 1.0f);
                float hs = (float)px(e, 8);
                for (int h = 0; h < kShapeHandles; ++h) {
                    double hx, hy;
                    shape_handle_pos(s, h, hx, hy);
                    gp::RectF hr((gp::REAL)(e.ox + hx * e.k - hs / 2), (gp::REAL)(e.oy + hy * e.k - hs / 2), hs, hs);
                    g.FillRectangle(&face, hr);
                    g.DrawRectangle(&edge, hr);
                }
            }
        }
    }
    paint_bar(e, dc, bp.rc);
}

// ------------------------------------------------------------------
// the mouse
// ------------------------------------------------------------------
void to_picture(const Editor& e, int x, int y, double& X, double& Y) {
    X = (x - e.ox) / e.k;
    Y = (y - e.oy) / e.k;
}

int bar_hit(const Editor& e, POINT pt) {
    for (int b = 0; b < B_COUNT; ++b)
        if (PtInRect(&e.btn[b], pt)) return b;
    return -1;
}

void begin_drag(Editor& e, Editor::Drag d, double X, double Y) {
    e.drag = d;
    e.ax = X;
    e.ay = Y;
    e.beforeAll = e.shapes;
    e.before = e.sel >= 0 ? e.shapes[e.sel] : Shape();
    e.moved = false;
    SetCapture(e.wnd);
}

void menu_choice(Editor& e, int b);

void on_down(Editor& e, int x, int y, bool dbl) {
    POINT pt{ x, y };
    if (y < e.barH) {
        int b = bar_hit(e, pt);
        if (b >= 0 && enabled(e, b)) {
            e.pressed = b;
            e.drag = Editor::Drag::Button;
            SetCapture(e.wnd);
            invalidate_bar(e);
        }
        return;
    }
    if (e.edit) { end_text(e, true); return; }     // (a click elsewhere ends the typing)
    SetFocus(e.wnd);
    double X, Y;
    to_picture(e, x, y, X, Y);
    double tol = px(e, 5) / e.k;
    if (e.sel >= 0) {                           // a handle of the one selected: resized
        int h = shape_handle_at(e.shapes[e.sel], X, Y, tol);
        if (h >= 0) {
            begin_drag(e, Editor::Drag::Resize, X, Y);
            e.handle = h;
            return;
        }
    }
    int hit = shapes_hit(e.shapes, X, Y, tol);
    if (dbl && hit >= 0 && e.shapes[hit].kind == ShapeKind::Text) { begin_text(e, hit, 0, 0); return; }
    switch (e.tool) {
        case Tool::Select:
            select(e, hit);
            if (hit >= 0) begin_drag(e, Editor::Drag::Move, X, Y);
            break;
        case Tool::Text:
            if (hit >= 0 && e.shapes[hit].kind == ShapeKind::Text) begin_text(e, hit, 0, 0);
            else begin_text(e, -1, std::clamp(X, 0.0, (double)e.w), std::clamp(Y, 0.0, (double)e.h));
            break;
        default: {
            X = std::clamp(X, 0.0, (double)e.w);
            Y = std::clamp(Y, 0.0, (double)e.h);
            e.sel = -1;
            begin_drag(e, Editor::Drag::New, X, Y);         // (the shapes before it: the undo step)
            Shape s;
            s.kind = e.tool == Tool::Rect ? ShapeKind::Rect : ShapeKind::Ellipse;
            s.x0 = s.x1 = X;
            s.y0 = s.y1 = Y;
            s.color = e.color;
            s.fill = e.fill;
            s.width = e.width;
            e.shapes.push_back(s);
            e.sel = (int)e.shapes.size() - 1;
            e.before = s;
            invalidate_bar(e);
            break;
        }
    }
}

void on_move(Editor& e, int x, int y, bool shift) {
    POINT pt{ x, y };
    if (e.drag == Editor::Drag::None || e.drag == Editor::Drag::Button) {
        int h = y < e.barH ? bar_hit(e, pt) : -1;
        if (h != e.hot) { e.hot = h; invalidate_bar(e); }
        if (e.drag == Editor::Drag::None) {
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, e.wnd, 0 };
            TrackMouseEvent(&te);
        }
        return;
    }
    double X, Y;
    to_picture(e, x, y, X, Y);
    if (e.sel < 0) return;
    Shape s = e.before;
    switch (e.drag) {
        case Editor::Drag::New:
            shape_span(s, e.ax, e.ay, std::clamp(X, 0.0, (double)e.w), std::clamp(Y, 0.0, (double)e.h), shift);
            break;
        case Editor::Drag::Move: {
            double dx = X - e.ax, dy = Y - e.ay;
            if (!e.moved && std::hypot(dx * e.k, dy * e.k) < 2) return;     // (a click isn't a move)
            shape_move(s, dx, dy);
            break;
        }
        case Editor::Drag::Resize:
            shape_resize(s, e.handle, X, Y, shift);
            break;
        default: return;
    }
    e.moved = true;
    e.shapes[e.sel] = s;
    invalidate_canvas(e);
}

void end_drag(Editor& e, bool keep) {
    Editor::Drag d = e.drag;
    e.drag = Editor::Drag::None;
    if (GetCapture() == e.wnd) ReleaseCapture();
    if (d == Editor::Drag::Button) {
        int b = e.pressed;
        e.pressed = -1;
        invalidate_bar(e);
        if (keep && b >= 0 && b == e.hot) menu_choice(e, b);
        return;
    }
    if (d == Editor::Drag::None) return;
    if (!keep) {
        e.shapes = e.beforeAll;
        e.sel = std::min(e.sel, (int)e.shapes.size() - 1);
        if (d == Editor::Drag::New) e.sel = -1;
    } else if (d == Editor::Drag::New) {
        const Shape& s = e.shapes[e.sel];
        if ((s.x1 - s.x0) * e.k < 3 && (s.y1 - s.y0) * e.k < 3) {     // (a click: nothing drawn)
            e.shapes.pop_back();
            e.sel = -1;
        } else {
            push_undo(e, std::move(e.beforeAll));
        }
    } else if (e.moved) {
        push_undo(e, std::move(e.beforeAll));
    }
    e.beforeAll.clear();
    invalidate_canvas(e);
    invalidate_bar(e);
}

void set_cursor(Editor& e) {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(e.wnd, &pt);
    LPCWSTR cur = IDC_ARROW;
    if (pt.y >= e.barH) {
        double X, Y;
        to_picture(e, pt.x, pt.y, X, Y);
        double tol = px(e, 5) / e.k;
        int h = e.drag == Editor::Drag::Resize ? e.handle
              : e.drag == Editor::Drag::None && e.sel >= 0 ? shape_handle_at(e.shapes[e.sel], X, Y, tol) : -1;
        if (h >= 0) cur = h == 0 || h == 4 ? IDC_SIZENWSE : h == 2 || h == 6 ? IDC_SIZENESW : h == 1 || h == 5 ? IDC_SIZENS : IDC_SIZEWE;
        else if (e.drag == Editor::Drag::Move) cur = IDC_SIZEALL;
        else if (e.tool == Tool::Select) cur = shapes_hit(e.shapes, X, Y, tol) >= 0 ? IDC_SIZEALL : IDC_ARROW;
        else if (e.tool == Tool::Text) cur = IDC_IBEAM;
        else cur = IDC_CROSS;
    }
    SetCursor(LoadCursorW(nullptr, cur));
}

// a button pressed (released over it)
void menu_choice(Editor& e, int b) {
    if (b >= B_COLOR && b < B_COLOR + kColorCount) { set_color(e, kColors[b - B_COLOR]); return; }
    switch (b) {
        case B_SELECT: set_tool(e, Tool::Select); break;
        case B_RECT: set_tool(e, Tool::Rect); break;
        case B_ELLIPSE: set_tool(e, Tool::Ellipse); break;
        case B_TEXT: set_tool(e, Tool::Text); break;
        case B_MORE: {
            CHOOSECOLORW cc{ sizeof(cc) };
            cc.hwndOwner = e.wnd;
            cc.rgbResult = e.color;
            cc.lpCustColors = e.custom;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
            if (ChooseColorW(&cc)) set_color(e, cc.rgbResult);
            break;
        }
        case B_FILL: set_fill(e, !e.fill); break;
        case B_WIDTH: case B_SIZE: {
            HMENU m = CreatePopupMenu();
            bool width = b == B_WIDTH;
            int n = width ? (int)ARRAYSIZE(kWidths) : (int)ARRAYSIZE(kSizes);
            for (int i = 0; i < n; ++i) {
                double v = (width ? kWidths[i] : kSizes[i]) * e.unit;
                double cur = width ? e.width : e.textSize;
                std::wstring t = std::to_wstring((int)std::lround(v)) + L" px";
                AppendMenuW(m, MF_STRING | (std::fabs(v - cur) < 0.01 ? MF_CHECKED : 0), (UINT_PTR)i + 1, t.c_str());
            }
            POINT pt{ e.btn[b].left, e.btn[b].bottom };
            ClientToScreen(e.wnd, &pt);
            UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, e.wnd, nullptr);
            DestroyMenu(m);
            if (id) {
                if (width) set_width(e, kWidths[id - 1] * e.unit);
                else set_size(e, kSizes[id - 1] * e.unit);
            }
            if (e.edit) SetFocus(e.edit);
            break;
        }
        case B_UNDO: undo_redo(e, true); break;
        case B_REDO: undo_redo(e, false); break;
        case B_DELETE: delete_selected(e); break;
        case B_COPY: copy(e); break;
        case B_SAVE: save(e); break;
        case B_SAVEAS: save_as(e); break;
    }
}

bool key(Editor& e, UINT vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl) {
        switch (vk) {
            case 'Z': undo_redo(e, !shift); return true;
            case 'Y': undo_redo(e, false); return true;
            case 'C': copy(e); return true;
            case 'S': if (shift) save_as(e); else save(e); return true;
        }
        return false;
    }
    switch (vk) {
        case VK_DELETE: case VK_BACK: delete_selected(e); return true;
        case VK_ESCAPE:
            if (e.drag != Editor::Drag::None) end_drag(e, false);
            else select(e, -1);
            return true;
        case 'V': set_tool(e, Tool::Select); return true;
        case 'R': set_tool(e, Tool::Rect); return true;
        case 'E': set_tool(e, Tool::Ellipse); return true;
        case 'T': set_tool(e, Tool::Text); return true;
        case 'F': set_fill(e, !e.fill); return true;
        case VK_RETURN:
            if (e.sel >= 0 && e.shapes[e.sel].kind == ShapeKind::Text) begin_text(e, e.sel, 0, 0);
            return true;
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
            if (e.sel < 0 || e.drag != Editor::Drag::None) return true;
            double step = (shift ? 10 : 1) / std::min(1.0, e.k);      // (screen pixels)
            if (!e.nudging) { checkpoint(e); e.nudging = true; }
            shape_move(e.shapes[e.sel], vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0,
                       vk == VK_UP ? -step : vk == VK_DOWN ? step : 0);
            invalidate_canvas(e);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------
// the window
// ------------------------------------------------------------------
bool can_close(Editor& e) {
    if (e.edit) end_text(e, true);
    if (!e.dirty || e.copied) return true;
    std::wstring q = e.path.empty() ? L"Save the screenshot before closing?"
                                    : L"Save the changes to " + e.name + L" before closing?";
    int r = MessageBoxW(e.wnd, q.c_str(), L"Explorer Plus", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    return r == IDNO || save(e);
}

void make_fonts(Editor& e) {
    if (e.font) DeleteObject(e.font);
    if (e.glyphs) DeleteObject(e.glyphs);
    e.font = theme_create_font(e.dpi, FW_REGULAR);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(16, e.dpi, 96);
    lf.lfWeight = FW_REGULAR;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, theme_glyph_face());
    e.glyphs = CreateFontIndirectW(&lf);
}

void apply_theme(Editor& e) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(e.wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    InvalidateRect(e.wnd, nullptr, FALSE);
}

const wchar_t* tip_text(int b) {
    if (b >= B_COLOR && b < B_COLOR + kColorCount) return kColorNames[b - B_COLOR];
    switch (b) {
        case B_SELECT: return L"Select and move (V)";
        case B_RECT: return L"Rectangle (R) - Shift: square";
        case B_ELLIPSE: return L"Ellipse (E) - Shift: circle";
        case B_TEXT: return L"Text (T)";
        case B_MORE: return L"More colors";
        case B_FILL: return L"Fill with the color (F)";
        case B_WIDTH: return L"Line width";
        case B_SIZE: return L"Text size";
        case B_UNDO: return L"Undo (Ctrl+Z)";
        case B_REDO: return L"Redo (Ctrl+Y)";
        case B_DELETE: return L"Delete (Del)";
        case B_COPY: return L"Copy (Ctrl+C)";
        case B_SAVE: return L"Save (Ctrl+S)";
        case B_SAVEAS: return L"Save as (Ctrl+Shift+S)";
    }
    return L"";
}

void on_create(Editor& e) {
    e.dpi = (int)GetDpiForWindow(e.wnd);
    make_fonts(e);
    e.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            e.wnd, nullptr, g_hinst, nullptr);
    for (int b = 0; b < B_COUNT; ++b) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = e.wnd;
        ti.uId = (UINT_PTR)b + 1;
        ti.lpszText = const_cast<wchar_t*>(tip_text(b));
        SendMessageW(e.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    apply_theme(e);
    update_title(e);
}

void on_destroy(Editor& e) {
    if (e.view) DeleteObject(e.view);
    if (e.font) DeleteObject(e.font);
    if (e.glyphs) DeleteObject(e.glyphs);
    if (e.editFont) DeleteObject(e.editFont);
    if (e.editBrush) DeleteObject(e.editBrush);
    s_editors.erase(std::remove(s_editors.begin(), s_editors.end(), &e), s_editors.end());
}

LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* ed = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        ed->wnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ed);
    }
    auto* pe = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!pe) return DefWindowProcW(hwnd, msg, wp, lp);
    Editor& e = *pe;
    switch (msg) {
        case WM_CREATE: on_create(e); return 0;
        case WM_SIZE: layout(e); InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = { px(e, 800), px(e, 420) };      // (the bar, compact)
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(e); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            on_down(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_MOUSEMOVE: on_move(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_SHIFT) != 0); return 0;
        case WM_MOUSELEAVE:
            if (e.hot >= 0 && e.drag == Editor::Drag::None) { e.hot = -1; invalidate_bar(e); }
            return 0;
        case WM_LBUTTONUP: end_drag(e, true); return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd && e.drag != Editor::Drag::None) end_drag(e, e.drag != Editor::Drag::Button);
            return 0;
        case WM_SETCURSOR:
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) { set_cursor(e); return TRUE; }
            break;
        case WM_KEYDOWN:
            if (key(e, (UINT)wp)) return 0;
            break;
        case WM_COMMAND:
            if ((HWND)lp == e.edit && e.edit && HIWORD(wp) == EN_CHANGE) fit_text_box(e);
            return 0;
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == e.edit && e.edit) {
                // the text in its colour, on what sets it apart
                double lum = (0.299 * GetRValue(e.editColor) + 0.587 * GetGValue(e.editColor) + 0.114 * GetBValue(e.editColor)) / 255;
                COLORREF bg = lum > 0.6 ? RGB(45, 45, 45) : RGB(255, 255, 255);
                if (!e.editBrush || e.editBrushColor != bg) {
                    if (e.editBrush) DeleteObject(e.editBrush);
                    e.editBrush = CreateSolidBrush(bg);
                    e.editBrushColor = bg;
                }
                SetTextColor((HDC)wp, e.editColor);
                SetBkColor((HDC)wp, bg);
                return (LRESULT)e.editBrush;
            }
            break;
        case WM_TEXT_DONE: end_text(e, wp != 0); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp)) SetFocus(e.edit ? e.edit : hwnd);
            return 0;
        case WM_SETTINGCHANGE:
            if (!((lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST)) break;
            [[fallthrough]];
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            theme_reload();
            apply_theme(e);
            break;
        case WM_DPICHANGED: {
            e.dpi = HIWORD(wp);
            make_fonts(e);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(e);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE:
            if (can_close(e)) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (e.shot) window_app_id(hwnd, nullptr);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            on_destroy(e);
            delete pe;
            app_window_closed();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_class() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_hinst;
    wc.lpfnWndProc = editor_proc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
    LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
    RegisterClassExW(&wc);
}

bool open(Editor* e) {
    register_class();
    // line widths and text sizes in proportion to the picture (a photo of 6000 pixels: thrice)
    e->unit = std::max(1.0, std::round(std::max(e->w, e->h) / 1920.0));
    e->width = 4 * e->unit;
    e->textSize = 24 * e->unit;
    s_editors.push_back(e);
    HWND w = CreateWindowExW(0, kClass, L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             800, 600, nullptr, nullptr, g_hinst, e);
    if (!w) {
        s_editors.erase(std::remove(s_editors.begin(), s_editors.end(), e), s_editors.end());
        delete e;
        return false;
    }
    if (e->shot) {                          // (before it's shown: the taskbar button is made then)
        static HICON big = nullptr, small = nullptr;
        if (!big) {
            LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_SCREENSHOT), LIM_LARGE, &big);
            LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_SCREENSHOT), LIM_SMALL, &small);
        }
        SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)small);
        window_app_id(w, L"RadimZatecka.ExplorerPlus.Screenshot");
    }
    // on the monitor with the mouse (its DPI comes with the move), the picture at its own size
    // where that fits, centred
    POINT cur{};
    GetCursorPos(&cur);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    RECT work = mi.rcWork;
    SetWindowPos(w, nullptr, work.left + 40, work.top + 40, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT want{ 0, 0, e->w + 2 * px(*e, 16), e->h + px(*e, 52) + 2 * px(*e, 16) };
    AdjustWindowRectExForDpi(&want, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)e->dpi);
    int ww = want.right - want.left, wh = want.bottom - want.top;
    int workW = work.right - work.left, workH = work.bottom - work.top;
    bool big = ww > workW || wh > workH;
    ww = std::clamp(ww, std::min(px(*e, 960), workW * 9 / 10), workW * 9 / 10);   // (the whole bar)
    wh = std::clamp(wh, std::min(px(*e, 480), workH), workH * 9 / 10);
    SetWindowPos(w, nullptr, work.left + (workW - ww) / 2, work.top + (workH - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(w, big ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    SetForegroundWindow(w);
    return true;
}

} // namespace

void imageedit_open_file(const std::wstring& path) {
    for (Editor* e : s_editors)
        if (lstrcmpiW(e->path.c_str(), path.c_str()) == 0) {
            if (IsIconic(e->wnd)) ShowWindow(e->wnd, SW_RESTORE);
            SetForegroundWindow(e->wnd);
            return;
        }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    ImageNote img;
    bool ok = image_decode_full(path, img) && img.pixels && img.w > 0 && img.h > 0;
    SetCursor(old);
    if (!ok) {
        MessageBoxW(GetActiveWindow(), (L"Explorer Plus can't open this picture:\n" + path).c_str(), L"Explorer Plus",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto* e = new Editor;
    e->px = img.pixels;
    e->w = img.w;
    e->h = img.h;
    e->path = path;
    e->name = title_of(path);
    open(e);
}

void imageedit_open_pixels(std::vector<uint32_t> pixels, int w, int h, const std::wstring& name) {
    if (w <= 0 || h <= 0 || pixels.size() < (size_t)w * h) return;
    auto* e = new Editor;
    e->px = std::make_shared<const std::vector<uint32_t>>(std::move(pixels));
    e->w = w;
    e->h = h;
    e->name = name;
    e->shot = true;
    e->dirty = true;                        // (not saved anywhere yet)
    // on the clipboard at once, as Windows' own screenshots: pasted anywhere as it is, and closed
    // unchanged without a question
    if (open(e)) copy(*e);
}

bool imageedit_any() { return !s_editors.empty(); }

bool imageedit_owns(HWND h) {
    if (!h) return false;
    HWND root = GetAncestor(h, GA_ROOT);
    for (Editor* e : s_editors)
        if (h == e->wnd || root == e->wnd) return true;
    return false;
}
