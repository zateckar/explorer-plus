#include "sync_ui.h"
#include "crypto.h"
#include "resource.h"
#include "sync_agent.h"
#include "sync_config.h"
#include "sync_engine.h"
#include "sync_ipc.h"
#include "theme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using syncipc::Msg;
std::string u8(const std::wstring& w) { return sync::to_utf8(w); }
std::wstring w16(const std::string& s) { return sync::from_utf8(s); }
std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}
size_t num(const std::string& s) { return (size_t)strtoull(s.c_str(), nullptr, 10); }

std::wstring exe_path() {
    WCHAR b[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return b;
}

// tests run with their own settings folder: nothing of theirs goes into the user's Run key
bool settings_isolated() { return GetEnvironmentVariableW(L"EXPLORERPLUS_SETTINGS", nullptr, 0) != 0; }

const wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"ExplorerPlusSync";
bool autostart_on() {
    return RegGetValueW(HKEY_CURRENT_USER, kRun, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
void autostart_set(bool on) {
    if (settings_isolated()) return;
    if (on) {
        std::wstring cmd = L"\"" + exe_path() + L"\" --sync";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRun, kRunValue, REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRun, kRunValue);
    }
}

// ------------------------------------------------------------------
// a small form window: heading, text, fields, an optional list, buttons
// ------------------------------------------------------------------
enum FKind { F_TEXT, F_SECRET, F_BOOL, F_CHOICE, F_FOLDER, F_CODE };

struct FField {
    std::wstring key, label, value, help;
    FKind kind = F_TEXT;
    std::vector<std::wstring> choices, values;     // F_CHOICE: shown, kept
    HWND lab = nullptr, ctl = nullptr, btn = nullptr, hint = nullptr;
};

struct FButton {
    int id;
    std::wstring text;
    bool left = false;
    HWND h = nullptr;
};

constexpr UINT WM_FORM_REPLY = WM_APP + 40;       // lp = Msg* (the form deletes it)
constexpr UINT WM_FORM_LATER = WM_APP + 41;       // runs Form::later
enum { ID_LIST = 900, ID_FIELD0 = 1000 };

struct Form {
    std::wstring title, heading, text;
    std::vector<FField> fields;
    std::vector<FButton> buttons;
    std::vector<std::wstring> columns;
    std::vector<int> widths;                       // per mille of the list's width
    int width = 560;                               // DIPs
    int listHeight = 0;                            // DIPs; 0: no list
    UINT timerMs = 0;
    std::function<bool(Form&, int)> onButton;      // true: the form closes
    std::function<void(Form&, size_t)> onChange;   // a field changed
    std::function<void(Form&)> onShown, onTimer, onSelect, onActivate;
    std::function<void(Form&, Msg&, int)> onReply;
    std::function<void(Form&)> later;              // run on WM_FORM_LATER

    HWND wnd = nullptr, owner = nullptr, list = nullptr;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr, big = nullptr;
    bool done = false, busy = false;
    int result = IDCANCEL;
    RECT headRc{}, textRc{};

    int px(int v) const { return MulDiv(v, dpi, 96); }
    FField* field(const std::wstring& key) {
        for (auto& f : fields) if (f.key == key) return &f;
        return nullptr;
    }
    std::wstring get(const std::wstring& key) {
        FField* f = field(key);
        if (!f) return {};
        if (!f->ctl) return f->value;
        if (f->kind == F_BOOL) return SendMessageW(f->ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
        if (f->kind == F_CHOICE) {
            int i = (int)SendMessageW(f->ctl, CB_GETCURSEL, 0, 0);
            return i >= 0 && i < (int)f->values.size() ? f->values[i] : std::wstring();
        }
        int n = GetWindowTextLengthW(f->ctl);
        std::wstring s(n, L'\0');
        GetWindowTextW(f->ctl, s.data(), n + 1);
        return s;
    }
    void set(const std::wstring& key, const std::wstring& v) {
        FField* f = field(key);
        if (!f) return;
        f->value = v;
        if (f->ctl && (f->kind == F_TEXT || f->kind == F_SECRET || f->kind == F_FOLDER || f->kind == F_CODE)) SetWindowTextW(f->ctl, v.c_str());
    }
    int selected() const { return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1; }
    HWND button(int id) {
        for (auto& b : buttons) if (b.id == id) return b.h;
        return nullptr;
    }
    void set_rows(const std::vector<std::vector<std::wstring>>& rows) {
        if (!list) return;
        int sel = selected();
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        int have = ListView_GetItemCount(list);
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (i >= have) {
                LVITEMW it{};
                it.mask = LVIF_TEXT;
                it.iItem = i;
                it.pszText = const_cast<wchar_t*>(L"");
                ListView_InsertItem(list, &it);
            }
            for (int c = 0; c < (int)rows[i].size() && c < (int)columns.size(); ++c) {
                wchar_t now[512] = L"";
                ListView_GetItemText(list, i, c, now, ARRAYSIZE(now));
                if (rows[i][c] != now) ListView_SetItemText(list, i, c, const_cast<wchar_t*>(rows[i][c].c_str()));
            }
        }
        for (int i = have - 1; i >= (int)rows.size(); --i) ListView_DeleteItem(list, i);
        if (sel < 0 && !rows.empty()) sel = 0;
        if (sel >= (int)rows.size()) sel = (int)rows.size() - 1;
        if (sel >= 0) ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, FALSE);
    }
};

HBRUSH s_bg = nullptr, s_box = nullptr;
COLORREF s_bgColor = CLR_INVALID, s_boxColor = CLR_INVALID;
HBRUSH brush(HBRUSH& b, COLORREF& have, COLORREF want) {
    if (!b || have != want) {
        if (b) DeleteObject(b);
        b = CreateSolidBrush(want);
        have = want;
    }
    return b;
}
HBRUSH bg_brush() { return brush(s_bg, s_bgColor, pal().content); }
HBRUSH box_brush() { return brush(s_box, s_boxColor, pal().boxFill); }

int text_height(Form& f, HFONT font, const std::wstring& s, int width, UINT flags = DT_WORDBREAK) {
    if (s.empty()) return 0;
    HDC dc = GetDC(f.wnd);
    HGDIOBJ of = SelectObject(dc, font);
    RECT r{ 0, 0, width, 0 };
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(f.wnd, dc);
    return r.bottom;
}

int button_width(Form& f, const std::wstring& s) {
    HDC dc = GetDC(f.wnd);
    HGDIOBJ of = SelectObject(dc, f.font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, of);
    ReleaseDC(f.wnd, dc);
    return std::max(f.px(96), (int)sz.cx + f.px(28));
}

// positions everything for the client width; returns the height the form wants
int layout(Form& f, bool move) {
    RECT rc{};
    GetClientRect(f.wnd, &rc);
    int m = f.px(24), cw = rc.right - 2 * m, y = m;
    auto place = [&](HWND h, int x, int yy, int w, int hh) { if (h && move) MoveWindow(h, x, yy, w, hh, TRUE); };
    if (!f.heading.empty()) {
        int hh = text_height(f, f.bold, f.heading, cw, DT_SINGLELINE);
        f.headRc = { m, y, m + cw, y + hh };
        y += hh + f.px(8);
    }
    if (!f.text.empty()) {
        int th = text_height(f, f.font, f.text, cw);
        f.textRc = { m, y, m + cw, y + th };
        y += th + f.px(16);
    }
    for (auto& fl : f.fields) {
        if (fl.lab) {
            place(fl.lab, m, y, cw, f.px(20));
            y += f.px(22);
        }
        int h = fl.kind == F_CODE ? f.px(46) : fl.kind == F_BOOL ? f.px(24) : f.px(28);
        if (fl.btn) {
            int bw = button_width(f, L"Browse...");
            place(fl.ctl, m, y, cw - bw - f.px(8), h);
            place(fl.btn, m + cw - bw, y, bw, h);
        } else if (fl.kind == F_CHOICE) {
            place(fl.ctl, m, y, cw, f.px(240));          // (a combo box's height includes its drop-down)
        } else {
            place(fl.ctl, m, y, cw, h);
        }
        y += h;
        if (fl.hint) {
            int hh = text_height(f, f.font, fl.help, cw);
            place(fl.hint, m, y + f.px(4), cw, hh);
            y += f.px(4) + hh;
        }
        y += f.px(14);
    }
    int bh = f.px(32), by = rc.bottom - m - bh;
    int want = y;
    if (f.list) {
        int bottom = by - f.px(16);
        place(f.list, m, y, cw, std::max(f.px(60), bottom - y));
        want += f.px(f.listHeight) + f.px(16);
        if (move) {
            int w = cw - GetSystemMetricsForDpi(SM_CXVSCROLL, f.dpi) - f.px(4), used = 0;
            for (size_t c = 0; c < f.columns.size(); ++c) {
                int cwid = c + 1 == f.columns.size() ? w - used : w * f.widths[c] / 1000;
                ListView_SetColumnWidth(f.list, (int)c, cwid);
                used += cwid;
            }
        }
    } else {
        by = std::max(by, y + f.px(6));
    }
    int lx = m, rx = rc.right - m;
    for (auto& b : f.buttons) if (b.left) { int w = button_width(f, b.text); place(b.h, lx, by, w, bh); lx += w + f.px(8); }
    for (auto it = f.buttons.rbegin(); it != f.buttons.rend(); ++it)
        if (!it->left) { int w = button_width(f, it->text); rx -= w; place(it->h, rx, by, w, bh); rx -= f.px(8); }
    if (move) InvalidateRect(f.wnd, nullptr, TRUE);
    return want + bh + m + f.px(6);
}

void apply_fonts(Form& f) {
    for (HFONT* h : { &f.font, &f.bold, &f.big }) if (*h) DeleteObject(*h);
    f.font = theme_create_font(f.dpi, FW_REGULAR);
    f.bold = theme_create_font(f.dpi, FW_SEMIBOLD, 140);
    f.big = theme_create_font(f.dpi, FW_SEMIBOLD, 170);
    EnumChildWindows(f.wnd, [](HWND h, LPARAM lp) { SendMessageW(h, WM_SETFONT, (WPARAM)((Form*)lp)->font, TRUE); return TRUE; }, (LPARAM)&f);
    for (auto& fl : f.fields) if (fl.kind == F_CODE && fl.ctl) SendMessageW(fl.ctl, WM_SETFONT, (WPARAM)f.big, TRUE);
}

void apply_theme(Form& f) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(f.wnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (f.list) theme_style_list(f.list);
    for (auto& b : f.buttons) theme_style_control(b.h);
    for (auto& fl : f.fields) {
        if (fl.btn) theme_style_control(fl.btn);
        if (fl.kind == F_BOOL) theme_style_control(fl.ctl);
        else if (fl.ctl) SetWindowTheme(fl.ctl, g_dark ? L"DarkMode_CFD" : nullptr, nullptr);
    }
    RedrawWindow(f.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

void create_fields(Form& f) {
    for (size_t i = 0; i < f.fields.size(); ++i) {
        FField& fl = f.fields[i];
        int id = ID_FIELD0 + (int)i * 4;
        auto mk = [&](const wchar_t* cls, const std::wstring& text, DWORD style, int cid, DWORD ex = 0) {
            return CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, f.wnd, (HMENU)(INT_PTR)cid, g_hinst, nullptr);
        };
        if (fl.kind != F_BOOL && !fl.label.empty()) fl.lab = mk(L"STATIC", fl.label, SS_NOPREFIX, id + 3);
        switch (fl.kind) {
            case F_BOOL:
                fl.ctl = mk(L"BUTTON", fl.label, BS_AUTOCHECKBOX | WS_TABSTOP, id);
                SendMessageW(fl.ctl, BM_SETCHECK, fl.value == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
                break;
            case F_CHOICE: {
                fl.ctl = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
                int sel = 0;
                for (size_t c = 0; c < fl.choices.size(); ++c) {
                    SendMessageW(fl.ctl, CB_ADDSTRING, 0, (LPARAM)fl.choices[c].c_str());
                    if (c < fl.values.size() && fl.values[c] == fl.value) sel = (int)c;
                }
                SendMessageW(fl.ctl, CB_SETCURSEL, sel, 0);
                break;
            }
            case F_CODE:
                fl.ctl = mk(L"EDIT", fl.value, ES_READONLY | ES_CENTER | WS_TABSTOP, id, WS_EX_CLIENTEDGE);
                break;
            default:
                fl.ctl = mk(L"EDIT", fl.value, ES_AUTOHSCROLL | WS_TABSTOP | (fl.kind == F_SECRET ? ES_PASSWORD : 0), id, WS_EX_CLIENTEDGE);
                if (fl.kind == F_FOLDER) fl.btn = mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, id + 1);
                break;
        }
        if (!fl.help.empty()) fl.hint = mk(L"STATIC", fl.help, SS_NOPREFIX, id + 2);
    }
}

// the fields again (a form that changes with a choice); the window grows or shrinks to them
void set_fields(Form& f, std::vector<FField> fields) {
    for (auto& fl : f.fields) for (HWND h : { fl.lab, fl.ctl, fl.btn, fl.hint }) if (h) DestroyWindow(h);
    f.fields = std::move(fields);
    create_fields(f);
    apply_fonts(f);
    apply_theme(f);
    // (tab order: the fields before the buttons)
    HWND after = HWND_TOP;
    for (auto& fl : f.fields) for (HWND h : { fl.lab, fl.ctl, fl.btn, fl.hint }) if (h) { SetWindowPos(h, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); after = h; }
    int want = layout(f, false);
    RECT wr{}, cr{};
    GetWindowRect(f.wnd, &wr);
    GetClientRect(f.wnd, &cr);
    if (!f.list) SetWindowPos(f.wnd, nullptr, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) - cr.bottom + want, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    layout(f, true);
}

bool pick_folder(HWND owner, std::wstring& path) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!path.empty()) {
        IShellItem* at = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&at)))) { d->SetFolder(at); at->Release(); }
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; ok = true; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return ok;
}

void press(Form& f, int id) {
    if (f.busy) return;
    if (!f.onButton || f.onButton(f, id)) {
        f.result = id;
        f.done = true;
    }
}

LRESULT CALLBACK list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {        // the header's text in the theme's colour
        auto* cd = (NMCUSTOMDRAW*)lp;
        if (cd->hdr.hwndFrom == ListView_GetHeader(h) && cd->hdr.code == NM_CUSTOMDRAW) {
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) SetTextColor(cd->hdc, pal().text);
            return CDRF_DODEFAULT;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void on_create(Form& f, HWND hwnd) {
    f.wnd = hwnd;
    f.dpi = (int)GetDpiForWindow(hwnd);
    if (f.listHeight) {
        f.list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS |
                                 LVS_SINGLESEL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LIST, g_hinst, nullptr);
        ListView_SetExtendedListViewStyle(f.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowSubclass(f.list, list_proc, 1, 0);
        for (size_t c = 0; c < f.columns.size(); ++c) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(f.columns[c].c_str());
            col.cx = 100;
            ListView_InsertColumn(f.list, (int)c, &col);
        }
    }
    create_fields(f);
    bool def = false;
    for (auto& b : f.buttons) {
        bool isDef = b.id == IDOK && !def;
        def |= isDef;
        b.h = CreateWindowExW(0, L"BUTTON", b.text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | (isDef ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                              0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)b.id, g_hinst, nullptr);
    }
    apply_fonts(f);
    apply_theme(f);
    if (f.timerMs) SetTimer(hwnd, 1, f.timerMs, nullptr);
}

LRESULT CALLBACK form_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Form* pf = (Form*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        pf = (Form*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pf);
    }
    if (!pf) return DefWindowProcW(hwnd, msg, wp, lp);
    Form& f = *pf;
    switch (msg) {
        case WM_CREATE: on_create(f, hwnd); return 0;
        case WM_SIZE: if (f.wnd) layout(f, true); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = { f.px(420), f.px(f.listHeight ? 360 : 200) };
            return 0;
        }
        case WM_SETCURSOR:
            if (f.busy) { SetCursor(LoadCursorW(nullptr, IDC_WAIT)); return TRUE; }
            break;
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, bg_brush());
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, f.bold);
            SetTextColor(dc, pal().text);
            if (!f.heading.empty()) DrawTextW(dc, f.heading.c_str(), -1, &f.headRc, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, f.font);
            SetTextColor(dc, pal().textSecondary);
            if (!f.text.empty()) DrawTextW(dc, f.text.c_str(), -1, &f.textRc, DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            HWND c = (HWND)lp;
            for (auto& fl : f.fields)
                if (c == fl.ctl && (fl.kind == F_CODE || fl.kind == F_TEXT || fl.kind == F_SECRET || fl.kind == F_FOLDER)) {
                    SetTextColor(dc, pal().text);       // a read-only edit
                    SetBkColor(dc, pal().boxFill);
                    return (LRESULT)box_brush();
                }
            bool hint = false;
            for (auto& fl : f.fields) hint |= c == fl.hint;
            SetTextColor(dc, hint ? pal().textSecondary : pal().text);
            SetBkColor(dc, pal().content);
            return (LRESULT)bg_brush();
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)wp, pal().text);
            SetBkColor((HDC)wp, pal().boxFill);
            return (LRESULT)box_brush();
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wp, pal().content);
            return (LRESULT)bg_brush();
        case DM_GETDEFID:
            return f.button(IDOK) ? MAKELRESULT(IDOK, DC_HASDEFID) : 0;
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id >= ID_FIELD0) {
                size_t i = (size_t)(id - ID_FIELD0) / 4;
                int part = (id - ID_FIELD0) % 4;
                if (i >= f.fields.size()) return 0;
                if (part == 1 && code == BN_CLICKED) {
                    std::wstring p = f.get(f.fields[i].key);
                    if (pick_folder(hwnd, p)) {
                        f.set(f.fields[i].key, p);
                        if (f.onChange) f.onChange(f, i);
                    }
                } else if (part == 0 && f.onChange && (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED)) {
                    f.onChange(f, i);
                }
                return 0;
            }
            if (id == IDCANCEL || f.button(id)) press(f, id);
            return 0;
        }
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->hwndFrom == f.list) {
                if (hdr->code == LVN_ITEMCHANGED && (((NMLISTVIEW*)lp)->uChanged & LVIF_STATE) && f.onSelect) f.onSelect(f);
                if ((hdr->code == NM_DBLCLK || hdr->code == NM_RETURN) && f.onActivate && f.selected() >= 0 && !f.busy) f.onActivate(f);
            }
            break;
        }
        case WM_TIMER:
            if (wp == 1 && f.onTimer && !f.busy) f.onTimer(f);
            return 0;
        case WM_FORM_REPLY: {
            std::unique_ptr<Msg> m((Msg*)lp);
            if (f.onReply) f.onReply(f, *m, (int)wp);
            return 0;
        }
        case WM_FORM_LATER:
            if (auto fn = std::move(f.later)) { f.later = nullptr; fn(f); }
            return 0;
        case WM_SETTINGCHANGE:
            if (!((lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) || wp == SPI_SETHIGHCONTRAST)) break;
            [[fallthrough]];
        case WM_SYSCOLORCHANGE:
            theme_reload();
            apply_theme(f);
            break;
        case WM_DPICHANGED: {
            f.dpi = HIWORD(wp);
            apply_fonts(f);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(f, true);
            return 0;
        }
        case WM_CLOSE: press(f, IDCANCEL); return 0;
        case WM_DESTROY:
            for (HFONT* h : { &f.font, &f.bold, &f.big }) if (*h) { DeleteObject(*h); *h = nullptr; }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// modal: its own loop until closed (a WM_QUIT is passed on); returns the button that closed it
int run_form(HWND owner, Form& f) {
    static bool registered = false;
    if (!registered) {
        registered = true;
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = g_hinst;
        wc.lpfnWndProc = form_proc;
        wc.lpszClassName = L"ExplorerPlusSyncForm";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        RegisterClassExW(&wc);
    }
    f.owner = owner;
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR mon = owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int dpi = owner ? (int)GetDpiForWindow(owner) : (int)GetDpiForSystem();
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | (f.listHeight ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
    DWORD ex = WS_EX_CONTROLPARENT | (owner ? WS_EX_DLGMODALFRAME : WS_EX_APPWINDOW);
    int w = MulDiv(f.width, dpi, 96);
    HWND wnd = CreateWindowExW(ex, L"ExplorerPlusSyncForm", f.title.c_str(), style, 0, 0, w, MulDiv(300, dpi, 96), owner, nullptr, g_hinst, &f);
    if (!wnd) return IDCANCEL;
    // the height its contents want, centred on the owner (or on the screen)
    RECT wr{}, cr{};
    GetWindowRect(wnd, &wr);
    GetClientRect(wnd, &cr);
    int h = (wr.bottom - wr.top) - cr.bottom + layout(f, false);
    w = std::min(w, (int)(mi.rcWork.right - mi.rcWork.left));
    h = std::min(h, (int)(mi.rcWork.bottom - mi.rcWork.top));
    RECT orc = mi.rcWork;
    if (owner) GetWindowRect(owner, &orc);
    int x = std::clamp((int)(orc.left + orc.right - w) / 2, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    int y = std::clamp((int)(orc.top + orc.bottom - h) / 2, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
    SetWindowPos(wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    layout(f, true);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    for (auto& fl : f.fields) if (fl.ctl && fl.kind != F_CODE) { SetFocus(fl.ctl); break; }
    if (f.onShown) f.onShown(f);
    MSG m{};
    bool quit = false;
    while (!f.done) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) { quit = true; break; }
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DestroyWindow(wnd);
    f.wnd = nullptr;
    if (quit) PostQuitMessage((int)m.wParam);
    return f.result;
}

// ------------------------------------------------------------------
// talking to the agent
// ------------------------------------------------------------------
// one request with the form kept alive (and deaf to input) until the reply; the agent is
// started first if it isn't running
bool agent_call(Form& f, const Msg& q, Msg& r, DWORD timeout = 120000) {
    std::atomic<bool> fin{ false };
    bool ok = false;
    HWND w = f.wnd;
    std::thread t([&] {
        ok = sync_agent_start() && syncipc::call(q, r, timeout);
        fin = true;
        PostMessageW(w, WM_NULL, 0, 0);
    });
    f.busy = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool quit = false;
    WPARAM code = 0;
    while (!fin) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = true; code = m.wParam; continue; }
            bool input = (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST) || (m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST) ||
                         (m.message >= WM_NCMOUSEMOVE && m.message <= WM_NCXBUTTONDBLCLK);
            if (input) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    t.join();
    f.busy = false;
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (quit) PostQuitMessage((int)code);
    return ok;
}

// the reply was "ok", else the reason is shown
bool agent_ok(Form& f, const Msg& q, Msg& r, const wchar_t* what, DWORD timeout = 120000) {
    if (!agent_call(f, q, r, timeout)) {
        MessageBoxW(f.wnd, L"The sync agent didn't answer (explorer-plus.exe --sync). Try again in a moment.", what, MB_OK | MB_ICONWARNING);
        return false;
    }
    if (syncipc::get(r, "ok") == "1") return true;
    MessageBoxW(f.wnd, w16(syncipc::get(r, "error", "It didn't work.")).c_str(), what, MB_OK | MB_ICONWARNING);
    return false;
}

void copy_text(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    // a key: kept out of the clipboard history and its cloud sync
    for (const wchar_t* fmt : { L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard" }) {
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
            *(DWORD*)GlobalLock(g) = 0;
            GlobalUnlock(g);
            SetClipboardData(RegisterClipboardFormatW(fmt), g);
        }
    }
    CloseClipboard();
}

void save_text(HWND owner, const std::wstring& name, const std::wstring& s) {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC spec[] = { { L"Text file", L"*.txt" } };
    d->SetFileTypes(1, spec);
    d->SetDefaultExtension(L"txt");
    d->SetFileName(name.c_str());
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* it = nullptr;
        PWSTR p = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            std::string body = u8(s);
            HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wr = 0;
            if (h != INVALID_HANDLE_VALUE) { WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr); CloseHandle(h); }
            CoTaskMemFree(p);
        }
        if (it) it->Release();
    }
    d->Release();
}

// ------------------------------------------------------------------
// the pages
// ------------------------------------------------------------------
enum { ID_COPY = 10, ID_SAVE, ID_TEST, ID_ADD, ID_REMOVE, ID_PAUSE, ID_SYNCNOW, ID_DEVICES, ID_ADDDEV, ID_REMOVEDEV, ID_NEWREC };

// A code to write down. recovery: the recovery key, typed back once before the form closes.
void show_code(HWND owner, const std::string& code, bool recovery) {
    Form f;
    std::wstring shown = w16(crypto::format_code(code));
    if (recovery) {
        f.title = L"Recovery key";
        f.heading = L"Your recovery key";
        f.text = L"Write it down, or save it somewhere only you can reach. With it, a new PC opens your sync folders "
                 L"when no other device of yours is at hand. Your files are encrypted before they leave this PC, and "
                 L"nobody - Explorer Plus included - can open them without this key or one of your devices.";
    } else {
        f.title = L"Add a device";
        f.heading = L"Code for your other device";
        f.text = L"On the other PC, open Sync folders in Explorer Plus, add a folder with this same storage location and "
                 L"type this code when it asks. It works once, within 15 minutes.";
    }
    FField c;
    c.key = L"code";
    c.kind = F_CODE;
    c.value = shown;
    f.fields.push_back(c);
    if (recovery) {
        FField a;
        a.key = L"again";
        a.label = L"Type it again, to be sure it's written down right";
        f.fields.push_back(a);
    }
    f.buttons = { { ID_COPY, L"Copy", true } };
    if (recovery) f.buttons.push_back({ ID_SAVE, L"Save as...", true });
    f.buttons.push_back({ IDOK, recovery ? L"Done" : L"Close" });
    f.onButton = [&](Form& f, int id) {
        if (id == ID_COPY) { copy_text(f.wnd, shown); return false; }
        if (id == ID_SAVE) {
            save_text(f.wnd, L"Explorer Plus recovery key", L"Explorer Plus sync - recovery key\r\n\r\n" + shown + L"\r\n");
            return false;
        }
        if (!recovery) return true;
        if (id == IDOK) {
            std::string typed = crypto::normalize_code(u8(f.get(L"again")));
            if (!typed.empty() && typed == crypto::normalize_code(code)) return true;
            MessageBoxW(f.wnd, L"That isn't the same key. Compare it with the one above, character by character.", L"Recovery key", MB_OK | MB_ICONWARNING);
            return false;
        }
        return MessageBoxW(f.wnd, L"Close without typing the key back?\n\nIf it wasn't written down, make a new one: Sync folders > Devices and keys > New recovery key.",
                           L"Recovery key", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    };
    run_form(owner, f);
}

// a pair this device has no key for yet: a code from another device, or the recovery key
bool unlock_form(HWND owner, const std::string& pairId, const std::wstring& name) {
    Form f;
    f.title = L"Unlock " + name;
    f.heading = L"Unlock " + name;
    f.text = L"Its storage was set up on another device, encrypted with your keys. On that device open Sync folders > "
             L"Devices and keys > Add a device and type the code it shows here - or type your recovery key.";
    FField mode;
    mode.key = L"mode";
    mode.label = L"Unlock with";
    mode.kind = F_CHOICE;
    mode.choices = { L"A code from my other device", L"My recovery key" };
    mode.values = { L"code", L"recovery" };
    FField code;
    code.key = L"code";
    code.label = L"Code";
    code.help = L"15 letters and digits; dashes and spaces don't matter.";
    f.fields = { mode, code };
    f.buttons = { { IDOK, L"Unlock" }, { IDCANCEL, L"Cancel" } };
    f.onButton = [&](Form& f, int id) {
        if (id != IDOK) return true;
        if (crypto::normalize_code(u8(f.get(L"code"))).empty()) {
            MessageBoxW(f.wnd, L"That isn't a code: 15 letters and digits.", f.title.c_str(), MB_OK | MB_ICONWARNING);
            return false;
        }
        Msg q = syncipc::request("unlock"), r;
        q.push_back({ "pair", pairId });
        q.push_back({ "mode", u8(f.get(L"mode")) });
        q.push_back({ "code", u8(f.get(L"code")) });
        return agent_ok(f, q, r, f.title.c_str());
    };
    return run_form(owner, f) == IDOK;
}

std::vector<FField> type_fields(const std::wstring& type, bool secretsOnly = false) {
    std::vector<FField> out;
    const sync::BackendType* t = sync::find_backend(type);
    if (!t) return out;
    for (auto& c : t->fields) {
        if (secretsOnly && c.kind != sync::ConfigField::Secret) continue;
        FField f;
        f.key = L"cfg." + c.key;
        f.label = c.label + (c.required || c.kind == sync::ConfigField::Bool ? L"" : L" (optional)");
        f.help = c.help;
        f.value = c.def;
        switch (c.kind) {
            case sync::ConfigField::Secret: f.kind = F_SECRET; break;
            case sync::ConfigField::Bool: f.kind = F_BOOL; break;
            case sync::ConfigField::Choice: f.kind = F_CHOICE; f.choices = f.values = c.choices; break;
            case sync::ConfigField::Folder: f.kind = F_FOLDER; break;
            default: f.kind = F_TEXT; break;
        }
        out.push_back(f);
    }
    return out;
}

// a new token, key... for a pair the storage refused
void secrets_form(HWND owner, const syncconf::Pair& p) {
    Form f;
    f.title = L"Sign in again";
    f.heading = L"Sign in to " + p.name + L"'s storage";
    f.text = L"The storage refused the saved sign-in (an expired or revoked token, a changed key). Type the new one: "
             L"it's kept in Windows' Credential Manager for this user.";
    f.fields = type_fields(p.type, true);
    if (f.fields.empty()) {
        MessageBoxW(owner, L"This storage has no sign-in to change here. Check its settings and the server.", f.title.c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    f.buttons = { { IDOK, L"Save" }, { IDCANCEL, L"Cancel" } };
    f.onButton = [&](Form& f, int id) {
        if (id != IDOK) return true;
        for (auto& fl : f.fields) {
            std::wstring v = f.get(fl.key);
            if (v.empty()) continue;
            Msg q = syncipc::request("set-secret"), r;
            q.push_back({ "pair", p.id });
            q.push_back({ "key", u8(fl.key.substr(4)) });
            q.push_back({ "value", u8(v) });
            if (!agent_ok(f, q, r, f.title.c_str())) return false;
        }
        return true;
    };
    run_form(owner, f);
}

// Add a sync folder: where, which storage, its settings; then the keys (first time: made
// here, the recovery key shown; a storage made by another device: unlocked)
bool add_form(HWND owner) {
    sync::register_builtin_backends();
    Form f;
    f.title = L"Add a sync folder";
    f.heading = L"Add a sync folder";
    f.text = L"A folder on this PC kept the same as a storage location. Files and their names are encrypted on this PC "
             L"before they're sent: the storage holds only scrambled data.";
    f.width = 600;
    std::vector<std::wstring> names, ids;
    for (auto* t : sync::backend_types()) { names.push_back(t->name); ids.push_back(t->id); }
    auto base = [&](const std::wstring& root, const std::wstring& type, const std::wstring& autostart) {
        FField r;
        r.key = L"root";
        r.label = L"Folder on this PC";
        r.kind = F_FOLDER;
        r.value = root;
        r.help = L"Made if it isn't there. Files already in it are added to the storage.";
        FField t;
        t.key = L"type";
        t.label = L"Storage";
        t.kind = F_CHOICE;
        t.choices = names;
        t.values = ids;
        t.value = type;
        std::vector<FField> v = { r, t };
        for (auto& x : type_fields(type)) v.push_back(x);
        if (!settings_isolated()) {
            FField a;
            a.key = L"autostart";
            a.label = L"Start sync when I sign in to Windows";
            a.kind = F_BOOL;
            a.value = autostart;
            v.push_back(a);
        }
        return v;
    };
    std::wstring first = std::find(ids.begin(), ids.end(), L"azdo") != ids.end() ? L"azdo" : ids.empty() ? L"" : ids[0];
    f.fields = base(L"", first, L"1");
    f.buttons = { { ID_TEST, L"Test connection", true }, { IDOK, L"Add" }, { IDCANCEL, L"Cancel" } };
    f.onChange = [&](Form& f, size_t i) {
        if (f.fields[i].key != L"type") return;
        // (not inside the combo box's own notification: it's destroyed)
        f.later = [&](Form& f) {
            std::wstring root = f.get(L"root"), type = f.get(L"type"), as = f.get(L"autostart");
            set_fields(f, base(root, type, as.empty() ? L"1" : as));
            if (FField* t = f.field(L"type")) SetFocus(t->ctl);
        };
        PostMessageW(f.wnd, WM_FORM_LATER, 0, 0);
    };
    auto request = [&](Form& f) {
        Msg q = syncipc::request("test-connection");
        q.push_back({ "type", u8(f.get(L"type")) });
        for (auto& fl : f.fields) if (fl.key.compare(0, 4, L"cfg.") == 0) q.push_back({ u8(fl.key), u8(f.get(fl.key)) });
        return q;
    };
    // the storage's vault: "exists", "none" (empty: a new one), "plaintext"; "" failed (shown)
    auto test = [&](Form& f) -> std::string {
        Msg r;
        if (!agent_ok(f, request(f), r, L"Test connection", 300000)) return {};
        return syncipc::get(r, "vault");
    };
    f.onButton = [&](Form& f, int id) {
        if (id == IDCANCEL) return true;
        if (id == ID_TEST) {
            std::string v = test(f);
            if (v == "exists") MessageBoxW(f.wnd, L"Connected. The location holds Explorer Plus sync data: this PC joins it.", L"Test connection", MB_OK | MB_ICONINFORMATION);
            else if (v == "none") MessageBoxW(f.wnd, L"Connected. The location is empty: Add sets it up.", L"Test connection", MB_OK | MB_ICONINFORMATION);
            else if (v == "plaintext") MessageBoxW(f.wnd, L"Connected, but the location already holds other files. Sync folders need an empty location (or one another PC of yours set up).", L"Test connection", MB_OK | MB_ICONWARNING);
            return false;
        }
        std::wstring root = f.get(L"root");
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (root.size() < 3 || root[1] != L':' || root[2] != L'\\') {
            MessageBoxW(f.wnd, L"Choose the folder on this PC (a full path, like D:\\Sync\\Documents).", f.title.c_str(), MB_OK | MB_ICONWARNING);
            return false;
        }
        auto pairs = syncconf::load();
        for (auto& p : pairs) {
            std::wstring a = lower(root) + L"\\", b = lower(p.root) + L"\\";
            if (a.compare(0, b.size(), b) == 0 || b.compare(0, a.size(), a) == 0) {
                MessageBoxW(f.wnd, (L"That folder overlaps the sync folder " + p.name + L" (" + p.root + L").").c_str(), f.title.c_str(), MB_OK | MB_ICONWARNING);
                return false;
            }
        }
        std::string vault = test(f);
        if (vault.empty()) return false;
        if (vault == "plaintext") {
            MessageBoxW(f.wnd, L"The location already holds other files. Sync folders need an empty location (or one another PC of yours set up), "
                               L"so nothing is ever sent unencrypted next to them.", f.title.c_str(), MB_OK | MB_ICONWARNING);
            return false;
        }
        Msg ks = syncipc::request("keys-status"), kr;
        if (!agent_ok(f, ks, kr, f.title.c_str())) return false;
        bool haveRing = syncipc::get(kr, "ring") == "1";
        if (!haveRing && vault == "none") {
            // the first sync folder ever: the user's keys, and the recovery key
            if (MessageBoxW(f.wnd, L"This is your first sync folder, so Explorer Plus makes your encryption keys now and shows "
                                   L"your recovery key, to write down.\n\nNobody can open the synced files without it or one of your devices.",
                            f.title.c_str(), MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                return false;
            Msg sq = syncipc::request("setup-keys"), sr;
            if (!agent_ok(f, sq, sr, f.title.c_str())) return false;
            show_code(f.wnd, syncipc::get(sr, "code"), true);
        }
        syncconf::Pair p;
        p.id = syncconf::new_id();
        p.root = root;
        p.name = root.size() > 3 ? root.substr(root.find_last_of(L'\\') + 1) : root;
        p.type = f.get(L"type");
        p.create = vault == "none";
        sync::BackendConfig cfg, secrets;
        for (auto& fl : f.fields) if (fl.key.compare(0, 4, L"cfg.") == 0) cfg.push_back({ fl.key.substr(4), f.get(fl.key) });
        syncconf::split_secrets(p.type, cfg, secrets);
        p.cfg = cfg;
        for (auto& [k, v] : secrets) syncconf::set_secret(p.id, k, v);
        pairs.push_back(p);
        if (!syncconf::save(pairs)) {
            MessageBoxW(f.wnd, L"The sync settings couldn't be written.", f.title.c_str(), MB_OK | MB_ICONERROR);
            syncconf::forget_secrets(p.id);
            return false;
        }
        CreateDirectoryW(root.c_str(), nullptr);
        if (f.field(L"autostart")) autostart_set(f.get(L"autostart") == L"1");
        Msg rq = syncipc::request("reload"), rr;
        agent_call(f, rq, rr, 30000);
        if (!haveRing && vault == "exists") unlock_form(f.wnd, p.id, p.name);
        return true;
    };
    return run_form(owner, f) == IDOK;
}

struct DeviceRow {
    std::string id;
    std::wstring name;
    bool self = false;
};

void devices_form(HWND owner, const syncconf::Pair& pair) {
    Form f;
    f.title = L"Devices and keys";
    f.heading = L"Devices and keys";
    f.text = L"The devices that hold your sync keys. Add a device shows a one-time code for a new PC; removing one makes "
             L"new keys it never gets (and a new recovery key). Listed from " + pair.name + L"'s storage.";
    f.width = 620;
    f.listHeight = 200;
    f.columns = { L"Device", L"Added", L"" };
    f.widths = { 500, 300, 200 };
    f.buttons = { { ID_ADDDEV, L"Add a device...", true }, { ID_REMOVEDEV, L"Remove device", true }, { ID_NEWREC, L"New recovery key", true },
                  { IDCANCEL, L"Close" } };
    std::vector<DeviceRow> devs;
    auto reload = [&](Form& f) {
        Msg q = syncipc::request("devices"), r;
        q.push_back({ "pair", pair.id });
        if (!agent_ok(f, q, r, f.title.c_str())) return;
        devs.clear();
        std::vector<std::vector<std::wstring>> rows;
        for (auto& line : syncipc::all(r, "device")) {
            std::vector<std::string> part;
            size_t at = 0;
            for (size_t t; (t = line.find('\t', at)) != std::string::npos; at = t + 1) part.push_back(line.substr(at, t - at));
            part.push_back(line.substr(at));
            if (part.size() < 4) continue;
            DeviceRow d{ part[0], w16(part[1]), part[3] == "1" };
            uint64_t t = std::stoull(part[2]);
            FILETIME ft{ (DWORD)t, (DWORD)(t >> 32) }, lt;
            SYSTEMTIME st{};
            wchar_t date[64] = L"";
            if (t && FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st))
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, ARRAYSIZE(date), nullptr);
            rows.push_back({ d.name, date, d.self ? L"This PC" : L"" });
            devs.push_back(d);
        }
        f.set_rows(rows);
    };
    f.onShown = reload;
    f.onButton = [&](Form& f, int id) {
        if (id == IDCANCEL) return true;
        if (id == ID_ADDDEV) {
            Msg q = syncipc::request("pair-offer"), r;
            q.push_back({ "pair", pair.id });
            if (agent_ok(f, q, r, f.title.c_str())) show_code(f.wnd, syncipc::get(r, "code"), false);
            reload(f);
        } else if (id == ID_REMOVEDEV) {
            int i = f.selected();
            if (i < 0 || i >= (int)devs.size()) return false;
            if (devs[i].self) {
                MessageBoxW(f.wnd, L"That's this PC. Remove it from another of your devices (or remove the sync folders here).", f.title.c_str(), MB_OK | MB_ICONINFORMATION);
                return false;
            }
            std::wstring ask = L"Remove " + devs[i].name + L"?\n\nIt can't read anything synced from now on: new keys are made and passed to your "
                               L"other devices, and you get a new recovery key (the old one stops working for the new keys). "
                               L"Files it already has stay on it.\n\nRevoke its access to the storage first (its S3 key, its token): "
                               L"a device that can still write there can get around the removal.";
            if (MessageBoxW(f.wnd, ask.c_str(), f.title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return false;
            Msg q = syncipc::request("remove-device"), r;
            q.push_back({ "pair", pair.id });
            q.push_back({ "device", devs[i].id });
            if (agent_ok(f, q, r, f.title.c_str(), 600000)) show_code(f.wnd, syncipc::get(r, "code"), true);
            reload(f);
        } else if (id == ID_NEWREC) {
            if (MessageBoxW(f.wnd, L"Make a new recovery key? New keys are made too and passed to your other devices: the old "
                                   L"recovery key can still open what was synced before, nothing from now on.",
                            f.title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return false;
            Msg q = syncipc::request("new-recovery-key"), r;
            if (agent_ok(f, q, r, f.title.c_str(), 600000)) show_code(f.wnd, syncipc::get(r, "code"), true);
        }
        return false;
    };
    run_form(owner, f);
}

// ------------------------------------------------------------------
// Sync folders
// ------------------------------------------------------------------
struct PairRow {
    syncconf::Pair p;
    bool live = false;
    std::string state, message;
    size_t pending = 0, conflicts = 0, problems = 0, deletes = 0;
};

std::wstring storage_text(const syncconf::Pair& p) {
    const sync::BackendType* t = sync::find_backend(p.type);
    std::wstring s = t ? t->name : p.type;
    if (size_t paren = s.find(L" ("); paren != std::wstring::npos) s.erase(paren);
    for (const wchar_t* k : { L"url", L"bucket", L"path", L"folder" }) {
        std::wstring v = sync::config_get(p.cfg, k);
        if (!v.empty()) return s + L": " + v;
    }
    return s;
}

std::wstring status_text(const PairRow& r, bool agentUp) {
    if (!r.live) return agentUp ? L"Starting" : L"Not running";
    std::wstring s;
    if (r.state == "locked") s = L"Locked - double-click to unlock";
    else if (r.state == "sign-in needed") s = L"Sign-in needed - double-click";
    else if (r.state == "waiting to delete") s = std::to_wstring(r.deletes) + L" deletions wait - double-click";
    else if (r.state == "error") s = L"Error: " + w16(r.message);
    else if (r.state == "folder missing") s = L"Folder missing";
    else if (r.state == "paused") s = L"Paused";
    else if (r.state == "offline") s = L"Offline" + (r.message.empty() ? std::wstring() : L": " + w16(r.message));
    else if (r.pending) s = L"Syncing " + std::to_wstring(r.pending) + (r.pending == 1 ? L" file" : L" files");
    else if (r.state == "syncing") s = L"Syncing";
    else s = L"Up to date";
    if (r.conflicts) s += L", " + std::to_wstring(r.conflicts) + (r.conflicts == 1 ? L" conflict" : L" conflicts");
    if (r.problems) s += L", " + std::to_wstring(r.problems) + (r.problems == 1 ? L" problem" : L" problems");
    return s;
}

} // namespace

void sync_folders_dialog(HWND owner) {
    static bool open = false;
    if (open) return;
    open = true;
    sync::register_builtin_backends();
    std::thread([] { sync_agent_ensure_running(); }).detach();
    Form f;
    f.title = L"Sync folders";
    f.heading = L"Sync folders";
    f.text = L"Folders kept the same on this PC, your other PCs and a storage location: an S3 bucket, a Git repository "
             L"(Azure DevOps too) or a shared folder. Everything is encrypted on this PC before it leaves.";
    f.width = 820;
    f.listHeight = 220;
    f.timerMs = 1500;
    f.columns = { L"Folder", L"On this PC", L"Storage", L"Status" };
    f.widths = { 160, 290, 270, 280 };
    if (!settings_isolated()) {
        FField a;
        a.key = L"autostart";
        a.label = L"Start sync when I sign in to Windows";
        a.kind = F_BOOL;
        a.value = autostart_on() ? L"1" : L"0";
        f.fields.push_back(a);
    }
    f.buttons = { { ID_ADD, L"Add...", true }, { ID_REMOVE, L"Remove", true }, { ID_PAUSE, L"Pause", true }, { ID_SYNCNOW, L"Sync now", true },
                  { ID_DEVICES, L"Devices and keys...", false }, { IDCANCEL, L"Close", false } };
    std::vector<PairRow> rows;
    bool polling = false, agentUp = false;

    auto show = [&](Form& f) {
        std::vector<std::vector<std::wstring>> t;
        for (auto& r : rows) t.push_back({ r.p.name, r.p.root, storage_text(r.p), status_text(r, agentUp) });
        f.set_rows(t);
        int i = f.selected();
        bool sel = i >= 0 && i < (int)rows.size();
        EnableWindow(f.button(ID_REMOVE), sel);
        EnableWindow(f.button(ID_PAUSE), sel);
        EnableWindow(f.button(ID_SYNCNOW), sel);
        SetWindowTextW(f.button(ID_PAUSE), sel && rows[i].p.paused ? L"Resume" : L"Pause");
    };
    auto poll = [&](Form& f) {
        if (polling) return;
        polling = true;
        HWND w = f.wnd;
        std::thread([w] {
            auto* r = new Msg;
            bool ok = syncipc::call(syncipc::request("pairs"), *r, 5000);
            if (!PostMessageW(w, WM_FORM_REPLY, ok ? 1 : 0, (LPARAM)r)) delete r;
        }).detach();
    };
    f.onReply = [&](Form& f, Msg& r, int ok) {
        polling = false;
        agentUp = ok || syncipc::agent_running();
        std::vector<PairRow> now;
        for (auto& p : syncconf::load()) {
            PairRow row;
            row.p = p;
            int n = (int)num(syncipc::get(r, "count", "0"));
            for (int i = 0; ok && i < n; ++i) {
                std::string k = "p" + std::to_string(i) + ".";
                if (syncipc::get(r, k + "id") != p.id) continue;
                row.live = true;
                row.state = syncipc::get(r, k + "state");
                row.message = syncipc::get(r, k + "message");
                row.pending = num(syncipc::get(r, k + "pendingFiles"));
                row.conflicts = num(syncipc::get(r, k + "conflicts"));
                row.problems = num(syncipc::get(r, k + "problems"));
                row.deletes = num(syncipc::get(r, k + "pendingDeletes"));
            }
            now.push_back(row);
        }
        rows = std::move(now);
        show(f);
    };
    f.onShown = poll;
    f.onTimer = poll;
    f.onSelect = show;
    f.onChange = [&](Form& f, size_t i) {
        if (f.fields[i].key == L"autostart") autostart_set(f.get(L"autostart") == L"1");
    };
    auto simple = [&](Form& f, const char* cmd, const std::string& id) {
        Msg q = syncipc::request(cmd), r;
        q.push_back({ "pair", id });
        agent_ok(f, q, r, f.title.c_str());
        poll(f);
    };
    f.onActivate = [&](Form& f) {
        int i = f.selected();
        if (i < 0 || i >= (int)rows.size()) return;
        PairRow r = rows[i];
        if (r.state == "locked") unlock_form(f.wnd, r.p.id, r.p.name);
        else if (r.state == "sign-in needed") secrets_form(f.wnd, r.p);
        else if (r.state == "waiting to delete") {
            std::wstring ask = std::to_wstring(r.deletes) + L" files of " + r.p.name + L" were deleted - more at once than sync goes ahead with by itself.\n\n"
                               L"Yes: delete them everywhere (on the storage and your other devices).\nNo: bring them back.";
            int a = MessageBoxW(f.wnd, ask.c_str(), f.title.c_str(), MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
            if (a == IDCANCEL) return;
            Msg q = syncipc::request("confirm-deletes"), rr;
            q.push_back({ "pair", r.p.id });
            q.push_back({ "yes", a == IDYES ? "1" : "0" });
            agent_ok(f, q, rr, f.title.c_str());
        } else {
            ShellExecuteW(f.wnd, nullptr, exe_path().c_str(), (L"\"" + r.p.root + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }
        poll(f);
    };
    f.onButton = [&](Form& f, int id) {
        int i = f.selected();
        bool sel = i >= 0 && i < (int)rows.size();
        switch (id) {
            case IDCANCEL: return true;
            case ID_ADD:
                if (add_form(f.wnd)) { if (f.field(L"autostart")) SendMessageW(f.field(L"autostart")->ctl, BM_SETCHECK, autostart_on() ? BST_CHECKED : BST_UNCHECKED, 0); }
                poll(f);
                break;
            case ID_REMOVE: {
                if (!sel) break;
                syncconf::Pair p = rows[i].p;
                std::wstring ask = L"Stop syncing " + p.name + L"?\n\nIts files stay in " + p.root + L" and on the storage; nothing is deleted.";
                if (MessageBoxW(f.wnd, ask.c_str(), f.title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
                auto pairs = syncconf::load();
                std::erase_if(pairs, [&](const syncconf::Pair& x) { return x.id == p.id; });
                syncconf::save(pairs);
                syncconf::forget_secrets(p.id);
                Msg q = syncipc::request("reload"), r;
                if (syncipc::agent_running()) agent_call(f, q, r, 60000);
                syncconf::remove_state_dir(p.id);
                poll(f);
                break;
            }
            case ID_PAUSE:
                if (sel) simple(f, rows[i].p.paused ? "resume" : "pause", rows[i].p.id);
                break;
            case ID_SYNCNOW:
                if (sel) simple(f, "sync-now", rows[i].p.id);
                break;
            case ID_DEVICES:
                if (rows.empty()) {
                    MessageBoxW(f.wnd, L"Add a sync folder first: the devices and keys are kept with its storage.", f.title.c_str(), MB_OK | MB_ICONINFORMATION);
                    break;
                }
                devices_form(f.wnd, rows[sel ? i : 0].p);
                break;
        }
        return false;
    };
    run_form(owner, f);
    open = false;
}

// ------------------------------------------------------------------
// the lists' view: status bar text and badges
// ------------------------------------------------------------------
namespace {

struct PaneView {
    std::wstring path;
    std::wstring status;
    std::map<std::wstring, int> items;      // lowercased name -> ItemState
};
PaneView s_views[kPanes];

std::vector<syncconf::Pair> s_pairs;
FILETIME s_pairsTime{};
ULONGLONG s_pairsChecked = 0;

const std::vector<syncconf::Pair>& pairs_cached() {
    ULONGLONG now = GetTickCount64();
    if (s_pairsChecked && now - s_pairsChecked < 2000) return s_pairs;
    s_pairsChecked = now;
    std::wstring file = syncconf::pairs_file();
    WIN32_FILE_ATTRIBUTE_DATA a{};
    FILETIME t{};
    if (!file.empty() && GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a)) t = a.ftLastWriteTime;
    if (CompareFileTime(&t, &s_pairsTime) != 0 || (!t.dwLowDateTime && !t.dwHighDateTime)) {
        s_pairsTime = t;
        s_pairs = t.dwLowDateTime || t.dwHighDateTime ? syncconf::load() : std::vector<syncconf::Pair>{};
    }
    return s_pairs;
}

std::mutex s_pollMx;
std::condition_variable s_pollCv;
std::wstring s_pollPath[kPanes];
bool s_pollWake = false, s_pollStarted = false;

struct ViewResult {
    int pane;
    std::wstring path;
    bool ok;
    Msg reply;
};

int item_state_of(const std::string& name) {
    for (int s = 0; s <= (int)sync::ItemState::Excluded; ++s)
        if (u8(sync::item_state_name((sync::ItemState)s)) == name) return s;
    return 0;
}

std::wstring view_status(const Msg& r) {
    if (syncipc::get(r, "ok") != "1") return L"Sync: starting";
    PairRow row;
    row.live = true;
    row.state = syncipc::get(r, "state");
    row.message = syncipc::get(r, "message");
    row.pending = num(syncipc::get(r, "pendingFiles"));
    row.conflicts = num(syncipc::get(r, "conflicts"));
    row.problems = num(syncipc::get(r, "problems"));
    row.deletes = num(syncipc::get(r, "pendingDeletes"));
    std::wstring s = status_text(row, true);
    size_t dash = s.find(L" - double-click");
    if (dash != std::wstring::npos) s.erase(dash);
    return L"Sync: " + s;
}

void apply_view(void* p) {
    std::unique_ptr<ViewResult> res((ViewResult*)p);
    PaneView& v = s_views[res->pane];
    if (v.path != res->path) return;
    v.items.clear();
    v.status = res->ok ? view_status(res->reply) : L"Sync: not running";
    if (res->ok)
        for (auto& line : syncipc::all(res->reply, "item")) {
            size_t t = line.rfind('\t');
            if (t != std::string::npos) v.items[lower(w16(line.substr(0, t)))] = item_state_of(line.substr(t + 1));
        }
    Pane& pane = g_panes[res->pane];
    if (pane.list) InvalidateRect(pane.list, nullptr, FALSE);
    PaneScope ps(&pane);
    status_update();
}

// asks the agent about the shown sync folders every 2 s (and at once on navigation)
void poll_views() {
    std::wstring last[kPanes];
    std::string lastBody[kPanes];
    for (;;) {
        std::wstring paths[kPanes];
        {
            std::unique_lock<std::mutex> lk(s_pollMx);
            s_pollCv.wait_for(lk, std::chrono::seconds(2), [] { return s_pollWake; });
            s_pollWake = false;
            for (int i = 0; i < kPanes; ++i) paths[i] = s_pollPath[i];
        }
        for (int i = 0; i < kPanes; ++i) {
            if (paths[i].empty()) { last[i].clear(); continue; }
            Msg q = syncipc::request("status"), r;
            q.push_back({ "folder", u8(paths[i]) });
            bool ok = syncipc::call(q, r, 3000);
            std::string body = ok ? syncipc::encode(r) : "-";
            if (paths[i] == last[i] && body == lastBody[i]) continue;
            last[i] = paths[i];
            lastBody[i] = body;
            post_async(apply_view, new ViewResult{ i, paths[i], ok, std::move(r) });
        }
    }
}

} // namespace

void sync_view_folder(int pane, const std::wstring& fsPath) {
    if (pane < 0 || pane >= kPanes) return;
    std::wstring p = fsPath;
    if (!p.empty() && !syncconf::pair_of(pairs_cached(), p)) p.clear();
    if (p == s_views[pane].path) return;
    s_views[pane] = PaneView{ p, {}, {} };
    {
        std::lock_guard<std::mutex> lk(s_pollMx);
        s_pollPath[pane] = p;
        s_pollWake = true;
    }
    s_pollCv.notify_all();
    if (!p.empty() && !s_pollStarted) {
        s_pollStarted = true;
        std::thread(poll_views).detach();
    }
}

std::wstring sync_view_status(int pane) { return pane >= 0 && pane < kPanes ? s_views[pane].status : std::wstring(); }

int sync_view_item(int pane, const std::wstring& fileName) {
    if (pane < 0 || pane >= kPanes || s_views[pane].items.empty() || fileName.empty()) return 0;
    auto f = s_views[pane].items.find(lower(fileName));
    return f == s_views[pane].items.end() ? 0 : f->second;
}

void sync_draw_badge(HDC dc, const RECT& icon, int state, int dpi) {
    using S = sync::ItemState;
    S s = (S)state;
    if (s == S::None) return;
    COLORREF c = s == S::Synced ? (g_dark ? RGB(108, 203, 95) : RGB(16, 124, 16))
               : s == S::Syncing || s == S::Pending ? (g_dark ? RGB(96, 205, 255) : RGB(0, 103, 192))
               : s == S::Excluded ? RGB(138, 138, 138)
                                  : (g_dark ? RGB(255, 153, 164) : RGB(196, 43, 28));
    int d = std::max(7, MulDiv(9, dpi, 96)), o = MulDiv(2, dpi, 96);
    RECT b{ icon.left - o, icon.bottom - d + o, icon.left - o + d, icon.bottom + o };
    int saved = SaveDC(dc);
    HBRUSH fill = CreateSolidBrush(c);
    HPEN ring = CreatePen(PS_SOLID, std::max(1, dpi / 96), pal().content);
    SelectObject(dc, fill);
    SelectObject(dc, ring);
    Ellipse(dc, b.left, b.top, b.right, b.bottom);
    COLORREF mark = g_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, d / 6), mark);
    SelectObject(dc, pen);
    int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2, r = d / 4;
    if (s == S::Synced) {
        POINT tick[] = { { cx - r, cy }, { cx - r / 3, cy + r * 2 / 3 }, { cx + r, cy - r * 2 / 3 } };
        Polyline(dc, tick, 3);
    } else if (s == S::Syncing || s == S::Pending) {
        POINT arrow[] = { { cx - r, cy }, { cx + r, cy } };
        Polyline(dc, arrow, 2);
        POINT head[] = { { cx, cy - r * 2 / 3 }, { cx + r, cy }, { cx, cy + r * 2 / 3 } };
        Polyline(dc, head, 3);
    } else if (s == S::Excluded) {
        POINT bar[] = { { cx - r, cy }, { cx + r + 1, cy } };
        Polyline(dc, bar, 2);
    } else {
        POINT bang[] = { { cx, cy - r }, { cx, cy + r / 3 } };
        Polyline(dc, bang, 2);
        SetPixel(dc, cx, cy + r, mark);
    }
    RestoreDC(dc, saved);
    DeleteObject(fill);
    DeleteObject(ring);
    DeleteObject(pen);
}

bool sync_paths_synced(const std::vector<std::wstring>& paths) {
    const auto& pairs = pairs_cached();
    if (pairs.empty()) return false;
    for (auto& p : paths) if (syncconf::pair_of(pairs, p)) return true;
    return false;
}

void sync_now_for(const std::vector<std::wstring>& paths) {
    std::vector<std::string> ids;
    for (auto& p : paths)
        if (const syncconf::Pair* pr = syncconf::pair_of(pairs_cached(), p))
            if (std::find(ids.begin(), ids.end(), pr->id) == ids.end()) ids.push_back(pr->id);
    if (ids.empty()) return;
    std::thread([ids] {
        if (!sync_agent_start()) return;
        for (auto& id : ids) {
            Msg q = syncipc::request("sync-now"), r;
            q.push_back({ "pair", id });
            syncipc::call(q, r, 10000);
        }
    }).detach();
}

std::vector<std::pair<std::wstring, std::wstring>> sync_roots() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (auto& p : pairs_cached()) out.push_back({ p.name, p.root });
    return out;
}
