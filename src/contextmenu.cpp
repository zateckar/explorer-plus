#include "contextmenu.h"
#include "navigation.h"
#include "listview.h"
#include "fileops.h"
#include "tabs.h"
#include "enumerate.h"
#include "tree.h"
#include "cmdbar.h"
#include "flyout.h"
#include "settings.h"
#include "theme.h"
#include "perf.h"
#include "detailspane.h"
#include "searchbox.h"
#include "player.h"
#include "imageview.h"
#include "imageedit.h"
#include "notepad.h"
#include "sync_ui.h"
#include <algorithm>
#include <map>
#include <unordered_set>

// ------------------------------------------------------------------
// Shell context-menu hosting (classic IContextMenu, which is what Explorer's
// "Show more options" menu is). Shell items get ids [kIdFirst, kIdLast]; the app's
// own entries use IDM_* command ids, which all lie above kIdLast.
// ------------------------------------------------------------------

#ifndef CMF_EXTENDED
#define CMF_EXTENDED 0x00000100
#endif

static const UINT kIdFirst = 1;
static const UINT kIdLast  = 0x7FFF;

// handler of the menu currently being tracked (for message forwarding)
static IContextMenu2* s_cm2 = nullptr;
static IContextMenu3* s_cm3 = nullptr;

bool contextmenu_active() { return s_cm2 || s_cm3; }

bool contextmenu_handle_menu_msg(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (!contextmenu_active()) return false;
    // WM_DRAWITEM/WM_MEASUREITEM for controls (wParam != 0) are not menu messages
    if ((msg == WM_DRAWITEM || msg == WM_MEASUREITEM) && wp != 0) return false;
    LRESULT r = 0;
    if (s_cm3) {
        if (SUCCEEDED(s_cm3->HandleMenuMsg2(msg, wp, lp, &r))) { *result = r; return true; }
        return false;
    }
    if (msg == WM_MENUCHAR) return false;            // IContextMenu2 can't answer it
    if (SUCCEEDED(s_cm2->HandleMenuMsg(msg, wp, lp))) {
        *result = msg == WM_INITMENUPOPUP ? 0 : TRUE;
        return true;
    }
    return false;
}

// RAII: publish the handler for message forwarding while its menu is up
struct ActiveMenu {
    ActiveMenu(IContextMenu* cm) {
        if (!cm) return;
        cm->QueryInterface(IID_PPV_ARGS(&s_cm3));
        if (!s_cm3) cm->QueryInterface(IID_PPV_ARGS(&s_cm2));
    }
    ~ActiveMenu() {
        if (s_cm3) { s_cm3->Release(); s_cm3 = nullptr; }
        if (s_cm2) { s_cm2->Release(); s_cm2 = nullptr; }
    }
};

// Building a file's menu opens its property store several times (the pin handlers look
// for an app ID), and for audio / video every open runs a Media Foundation parser:
// ~50 ms each for MPEG-TS, 7 opens per menu. No menu verb uses media metadata, so while
// a menu is built those property handlers are shadowed by an in-process class object
// that declines; the shell then falls back to the plain file-system properties.
struct DeclineFactory : IClassFactory {
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }     // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown*, REFIID, void** ppv) override {
        *ppv = nullptr;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

// the property handler of an extension when it is a media one (Media Foundation parsers)
static std::optional<CLSID> media_property_handler(const std::wstring& ext) {
    static std::map<std::wstring, std::optional<CLSID>> cache;
    std::wstring key = ext;
    for (auto& c : key) c = (wchar_t)towlower(c);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::optional<CLSID> out;
    WCHAR clsid[64]{}, dll[MAX_PATH]{};
    DWORD cb = sizeof(clsid);
    std::wstring sub = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\" + key;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, clsid, &cb) == ERROR_SUCCESS) {
        cb = sizeof(dll);
        std::wstring srv = std::wstring(L"CLSID\\") + clsid + L"\\InprocServer32";
        CLSID id;
        if (RegGetValueW(HKEY_CLASSES_ROOT, srv.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, dll, &cb) == ERROR_SUCCESS &&
            SUCCEEDED(CLSIDFromString(clsid, &id))) {
            std::wstring name = PathFindFileNameW(dll);
            for (auto& c : name) c = (wchar_t)towlower(c);
            if (name.rfind(L"mf", 0) == 0 || name == L"msflacdecoder.dll" || name == L"sbe.dll") out = id;
        }
    }
    cache[key] = out;
    return out;
}

struct MediaPropsOff {
    std::vector<DWORD> cookies;
    MediaPropsOff(IShellFolder* parent, PCUITEMID_CHILD_ARRAY apidl, UINT cidl) {
        static DeclineFactory factory;
        std::vector<CLSID> done;
        for (UINT i = 0; i < cidl && i < 256; ++i) {
            std::wstring name = child_name(parent, apidl[i], SHGDN_INFOLDER | SHGDN_FORPARSING);
            const wchar_t* ext = PathFindExtensionW(name.c_str());
            if (!*ext) continue;
            auto id = media_property_handler(ext);
            if (!id || std::find(done.begin(), done.end(), *id) != done.end()) continue;
            done.push_back(*id);
            DWORD cookie = 0;
            if (SUCCEEDED(CoRegisterClassObject(*id, &factory, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &cookie)))
                cookies.push_back(cookie);
        }
    }
    ~MediaPropsOff() { for (DWORD c : cookies) CoRevokeClassObject(c); }
};

static std::wstring verb_of(IContextMenu* cm, UINT id) {
    WCHAR verb[128]{};
    if (id < kIdFirst || id > kIdLast) return {};
    if (FAILED(cm->GetCommandString(id - kIdFirst, GCS_VERBW, nullptr,
                                    reinterpret_cast<LPSTR>(verb), ARRAYSIZE(verb))))
        return {};
    return verb;
}

static bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

static void invoke_id(HWND hwnd, IContextMenu* cm, UINT id, POINT pt, PCIDLIST_ABSOLUTE folder,
                      bool async = true) {
    // a working directory only where the folder is one on disk (Home's "shell:::{...}" or
    // a search's "search-ms:..." would make a launched program fail to start)
    WCHAR dirBuf[MAX_PATH]{};
    bool haveDir = folder && SHGetPathFromIDListEx(folder, dirBuf, MAX_PATH, GPFIDL_DEFAULT) && dirBuf[0];
    char dirA[MAX_PATH]{};
    if (haveDir) WideCharToMultiByte(CP_ACP, 0, dirBuf, -1, dirA, MAX_PATH, nullptr, nullptr);

    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof(ici);
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | (async ? CMIC_MASK_ASYNCOK : 0);
    if (key_down(VK_CONTROL)) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (key_down(VK_SHIFT))   ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = hwnd;
    ici.lpVerb  = MAKEINTRESOURCEA(id - kIdFirst);
    ici.lpVerbW = MAKEINTRESOURCEW(id - kIdFirst);
    ici.lpDirectory  = haveDir ? dirA : nullptr;
    ici.lpDirectoryW = haveDir ? dirBuf : nullptr;
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = pt;
    cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&ici));
}

// ------------------------------------------------------------------
// Win11 compact menu: a flyout built from the classic menu's items. Its own
// entries use ids above kIdLast and below the IDM_* range.
// ------------------------------------------------------------------
static const UINT kFlyMore = 0x8001, kFlyNewTab = 0x8002, kFlyNewWindow = 0x8003;
static const UINT kFlyPlay = 0x8004, kFlyEnqueue = 0x8005, kFlyPlaylist = 0x8006, kFlyEditPicture = 0x8007;
static const UINT kFlyNotepad = 0x8008, kFlySyncNow = 0x8009, kFlySyncExclude = 0x800A, kFlySyncVersions = 0x800B;

struct ShellEntry {
    UINT id = 0;
    std::wstring verb, text;
    HMENU sub = nullptr;
    HBITMAP bmp = nullptr;          // the item's bitmap, else its check-mark one (7-Zip's logo)
    bool isDefault = false, disabled = false;
};

// a real bitmap, not 0 / HBMMENU_CALLBACK (-1) / the HBMMENU_* system glyphs (1..11);
// handles are 32-bit values that may come sign-extended, so compare as signed
static bool real_bitmap(HBITMAP b) {
    INT_PTR v = (INT_PTR)b;
    return v != 0 && v != -1 && !(v >= 1 && v <= 11);
}

static std::vector<ShellEntry> menu_entries(IContextMenu* cm, HMENU m, int from = 0) {
    std::vector<ShellEntry> out;
    for (int i = from; i < GetMenuItemCount(m); ++i) {
        WCHAR text[256]{};
        MENUITEMINFOW mi{ sizeof(mi) };
        mi.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP | MIIM_CHECKMARKS;
        mi.dwTypeData = text;
        mi.cch = ARRAYSIZE(text) - 1;
        if (!GetMenuItemInfoW(m, i, TRUE, &mi)) continue;
        if (mi.fType & (MFT_SEPARATOR | MFT_OWNERDRAW) || !text[0]) continue;
        ShellEntry e;
        e.id = mi.wID;
        e.text = text;
        e.sub = mi.hSubMenu;
        if (!e.sub) e.verb = verb_of(cm, e.id);
        if (real_bitmap(mi.hbmpItem)) e.bmp = mi.hbmpItem;
        else if (real_bitmap(mi.hbmpUnchecked)) e.bmp = mi.hbmpUnchecked;
        e.isDefault = (mi.fState & MFS_DEFAULT) != 0;
        e.disabled = (mi.fState & MFS_DISABLED) != 0;
        out.push_back(std::move(e));
    }
    return out;
}

static const ShellEntry* find_verb(const std::vector<ShellEntry>& items, const wchar_t* verb) {
    for (auto& e : items)
        if (!e.sub && lstrcmpiW(e.verb.c_str(), verb) == 0) return &e;
    return nullptr;
}

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring plain_text(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size() && s[i] != L'\t'; ++i)
        if (s[i] != L'&' || (i + 1 < s.size() && s[i + 1] == L'&' && ++i)) out += s[i];
    return out;
}

static FlyEntry fly_item(UINT id, const std::wstring& text, wchar_t glyph, const wchar_t* accel = L"") {
    FlyEntry f;
    f.id = id;
    f.text = text;
    f.glyph = glyph;
    f.accel = accel;
    return f;
}

static FlyEntry fly_from(const ShellEntry& e, wchar_t glyph, const wchar_t* accel = L"", bool appIcon = false) {
    FlyEntry f = fly_item(e.id, e.text, glyph, accel);
    if (e.sub) { f.kind = FlyEntry::Submenu; f.sub = e.sub; }
    if (appIcon && e.bmp) { f.bitmap = e.bmp; f.glyph = 0; }   // an app's own icon
    else if (!glyph) f.bitmap = e.bmp;
    f.disabled = e.disabled;
    return f;
}

static void fly_separator(std::vector<FlyEntry>& list) {
    if (!list.empty() && list.back().kind != FlyEntry::Separator) {
        FlyEntry s;
        s.kind = FlyEntry::Separator;
        list.push_back(s);
    }
}

static void fly_finish(std::vector<FlyEntry>& list) {
    fly_separator(list);
    list.push_back(fly_item(kFlyMore, L"Show more options", L'\xE712', L"Shift+F10"));
}

// settings' [contextmenu] promotions: "<text or verb>[|<icon file>[,<index>]]"
struct Promotion { std::wstring match, iconFile; int iconIndex = 0; };

static std::vector<Promotion> promotions() {
    std::vector<Promotion> out;
    for (const std::wstring& line : settings_menu_promotions()) {
        Promotion p;
        size_t bar = line.find(L'|');
        p.match = lower(plain_text(line.substr(0, bar)));
        if (bar != std::wstring::npos) {
            std::wstring icon = line.substr(bar + 1);
            size_t comma = icon.rfind(L',');
            if (comma != std::wstring::npos && comma + 1 < icon.size() &&
                (iswdigit(icon[comma + 1]) || icon[comma + 1] == L'-')) {
                p.iconIndex = _wtoi(icon.c_str() + comma + 1);
                icon.resize(comma);
            }
            WCHAR exp[MAX_PATH]{};
            if (ExpandEnvironmentStringsW(icon.c_str(), exp, MAX_PATH)) icon = exp;
            p.iconFile = icon;
        }
        if (!p.match.empty()) out.push_back(std::move(p));
    }
    return out;
}

static bool promoted(const ShellEntry& e, const Promotion& p) {
    return lower(plain_text(e.text)).find(p.match) != std::wstring::npos ||
           (!e.verb.empty() && lower(e.verb) == p.match);
}

// The user's promoted items (settings' [contextmenu]), in menu order: they head the
// compact menu, right under the icon row. Icons loaded for them land in `icons` (caller
// frees); the entries taken go to `used` so the app verbs below don't list them again.
static void fly_promoted(std::vector<FlyEntry>& list, const std::vector<ShellEntry>& items,
                         std::vector<HICON>& icons, std::vector<const ShellEntry*>& used) {
    std::vector<Promotion> promo = promotions();
    for (auto& e : items) {
        auto p = std::find_if(promo.begin(), promo.end(), [&](const Promotion& p) { return promoted(e, p); });
        if (p == promo.end()) continue;
        FlyEntry f = fly_from(e, 0);
        if (!p->iconFile.empty()) {
            HICON ic = nullptr;
            UINT got = PrivateExtractIconsW(p->iconFile.c_str(), p->iconIndex, S(16), S(16), &ic, nullptr, 1, 0);
            if (got && got != 0xFFFFFFFF && ic) {
                icons.push_back(ic);
                f.icon = ic;
                f.bitmap = nullptr;
            }
        }
        list.push_back(f);
        used.push_back(&e);
    }
    // Windows Terminal's "Open in Terminal" (folders, backgrounds) joins them, with its icon
    for (auto& e : items)
        if (!e.sub && lower(e.verb).find(L"terminal") != std::wstring::npos &&
            std::find(used.begin(), used.end(), &e) == used.end()) {
            list.push_back(fly_from(e, L'\xE756', L"", true));
            used.push_back(&e);
        }
    fly_separator(list);
}

// verbs from installed apps that Win11 lists in the compact menu
static void fly_app_verbs(std::vector<FlyEntry>& list, const std::vector<ShellEntry>& items,
                          const std::vector<const ShellEntry*>& used) {
    fly_separator(list);
    auto taken = [&](const ShellEntry* e) { return std::find(used.begin(), used.end(), e) != used.end(); };
    static const struct { const wchar_t* verb; wchar_t glyph; } kApp[] = {
        { L"edit", L'\xE70F' }, { L"print", L'\xE749' }, { L"setdesktopwallpaper", L'\xE91B' },
        { L"rotate90", L'\xE7AD' }, { L"rotate270", L'\xE7AD' },
    };
    for (auto& a : kApp)
        if (auto e = find_verb(items, a.verb); e && !taken(e)) list.push_back(fly_from(*e, a.glyph));
    for (auto& e : items)
        if (!taken(&e) && lower(e.verb) == L"runas" && lower(e.text).find(L"administrator") != std::wstring::npos)
            list.push_back(fly_from(e, L'\xE7EF'));
}

// `picture`: one picture chosen (Edit picture); `text`: text files (Edit in Explorer Plus);
// `sync`: the sync entries (items inside a sync folder)
static UINT item_flyout(HWND hwnd, IContextMenu* cm, HMENU m, POINT pt, bool folder, PlayerOffer media, bool picture,
                        bool text, const SyncMenu& sync) {
    auto items = menu_entries(cm, m);
    std::vector<FlyButton> row;
    static const struct { const wchar_t* verb; wchar_t glyph; const wchar_t* tip; } kRow[] = {
        { L"cut", L'\xE8C6', L"Cut (Ctrl+X)" }, { L"copy", L'\xE8C8', L"Copy (Ctrl+C)" },
        { L"rename", L'\xE8AC', L"Rename (F2)" }, { L"Windows.ModernShare", L'\xE72D', L"Share" },
        { L"delete", L'\xE74D', L"Delete (Delete)" },
    };
    for (auto& b : kRow)
        if (auto e = find_verb(items, b.verb)) row.push_back({ e->id, b.glyph, b.tip, e->disabled });

    std::vector<FlyEntry> list;
    std::vector<HICON> icons;
    std::vector<const ShellEntry*> used;
    fly_promoted(list, items, icons, used);
    // (an item the user promoted is listed up there only)
    auto taken = [&](const ShellEntry* e) { return std::find(used.begin(), used.end(), e) != used.end(); };
    for (auto& e : items)
        if (e.isDefault && !e.sub) {
            if (taken(&e)) break;
            list.push_back(fly_from(e, L'\xE8E5'));
            list.back().bold = true;
            used.push_back(&e);
            break;
        }
    if (folder) {
        list.push_back(fly_item(kFlyNewTab, L"Open in new tab", L'\xE7C3'));
        list.push_back(fly_item(kFlyNewWindow, L"Open in new window", L'\xE8A7'));
    }
    if (media != PlayerOffer::None) {      // (named apart from a media app's own Play / Add to play queue verbs)
        if (media == PlayerOffer::View) list.push_back(fly_item(kFlyPlay, L"View in Explorer Plus", L'\xE8B9'));
        else list.push_back(fly_item(kFlyPlay, L"Play in Explorer Plus", L'\xE768'));
        list.push_back(fly_item(kFlyEnqueue, L"Queue in Explorer Plus", L'\xE710'));
        if (media == PlayerOffer::Play) list.push_back(fly_item(kFlyPlaylist, L"Create playlist...", L'\xEA37'));
    }
    if (picture) list.push_back(fly_item(kFlyEditPicture, L"Edit picture", L'\xE70F'));
    if (text) list.push_back(fly_item(kFlyNotepad, L"Edit in Explorer Plus", L'\xE70B'));
    if (sync.syncNow) list.push_back(fly_item(kFlySyncNow, L"Sync now", L'\xE895'));
    if (sync.versions) list.push_back(fly_item(kFlySyncVersions, L"Version history...", L'\xE81C'));
    if (sync.exclude) list.push_back(fly_item(kFlySyncExclude, L"Exclude from sync", L'\xE8D8'));
    if (sync.include) list.push_back(fly_item(kFlySyncExclude, L"Include in sync", L'\xE895'));
    for (auto& e : items)
        if (e.sub && plain_text(lower(e.text)).rfind(L"open with", 0) == 0) {
            if (!taken(&e)) { list.push_back(fly_from(e, L'\xE7AC')); used.push_back(&e); }
            break;
        }
    static const struct { const wchar_t* verb; wchar_t glyph; const wchar_t* accel; } kCore[] = {
        { L"pintohome", L'\xE718', L"" }, { L"unpinfromhome", L'\xE77A', L"" },
        { L"pintohomefile", L'\xE734', L"" }, { L"unpinfromhomefile", L'\xE8D9', L"" },
        { L"PinToStartScreen", L'\xE718', L"" }, { L"copyaspath", L'\xE71B', L"Ctrl+Shift+C" },
        { L"properties", L'\xE90F', L"Alt+Enter" },
    };
    for (auto& c : kCore)
        if (auto e = find_verb(items, c.verb); e && !taken(e)) {
            list.push_back(fly_from(*e, c.glyph, c.accel));
            used.push_back(e);
        }
    fly_app_verbs(list, items, used);
    fly_finish(list);
    UINT id = flyout_track(hwnd, pt, row, list);
    for (HICON ic : icons) DestroyIcon(ic);
    return id;
}

// The compact flyout first (`flyout` builds and tracks it); "Show more options",
// or Shift held (Shift+right-click, Shift+F10), shows the classic menu `m`.
template <class Flyout>
static UINT track(HWND hwnd, IContextMenu* cm, HMENU m, POINT pt, Flyout&& flyout) {
    ActiveMenu active(cm);
    UINT id = key_down(VK_SHIFT) ? kFlyMore : flyout();
    if (id == kFlyMore) id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, nullptr);
    return id;
}

static UINT track(HWND hwnd, IContextMenu* cm, HMENU m, POINT pt) {
    return track(hwnd, cm, m, pt, [] { return kFlyMore; });
}

static HRESULT item_menu(HWND hwnd, IShellFolder* parent, PCIDLIST_ABSOLUTE folderAbs,
                         PCUITEMID_CHILD_ARRAY apidl, UINT cidl, com_ptr<IContextMenu>& cm) {
    HRESULT hr = parent->GetUIObjectOf(hwnd, cidl, apidl, IID_IContextMenu, nullptr, (void**)cm.put());
    if (SUCCEEDED(hr) && cm) return hr;
    DEFCONTEXTMENU dcm{};
    dcm.hwnd = hwnd;
    dcm.psf = parent;
    dcm.pidlFolder = folderAbs;
    dcm.cidl = cidl;
    dcm.apidl = apidl;
    return SHCreateDefaultContextMenu(&dcm, IID_PPV_ARGS(cm.put()));
}

// is the single item a browsable folder (not an archive)?
static bool is_plain_folder(IShellFolder* parent, PCUITEMID_CHILD child) {
    SFGAOF a = SFGAO_FOLDER | SFGAO_STREAM;
    if (FAILED(parent->GetAttributesOf(1, &child, &a))) return false;
    return (a & SFGAO_FOLDER) && !(a & SFGAO_STREAM);
}

enum class MenuSource { List, Tree };

// verbs that never touch the files themselves (the preview can stay)
static bool verb_keeps_files(const std::wstring& verb) {
    static const wchar_t* const kKeep[] = {
        L"properties", L"copyaspath", L"pintohome", L"unpinfromhome", L"pintohomefile",
        L"unpinfromhomefile", L"PinToStartScreen", L"Windows.ModernShare", L"Windows.Share",
    };
    for (auto k : kKeep)
        if (lstrcmpiW(verb.c_str(), k) == 0) return true;
    return false;
}

static void show_cm(HWND hwnd, POINT pt, IShellFolder* parent, PCIDLIST_ABSOLUTE folderAbs,
                    PCUITEMID_CHILD_ARRAY apidl, UINT cidl, MenuSource src) {
    com_ptr<IContextMenu> cm;
    if (FAILED(item_menu(hwnd, parent, folderAbs, apidl, cidl, cm)) || !cm) return;
    HMENU m = CreatePopupMenu();
    if (!m) return;
    UINT flags = CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME;
    if (key_down(VK_SHIFT)) flags |= CMF_EXTENDED;     // Shift+right-click verbs
    double t0 = perf_now();
    HRESULT hr;
    {
        MediaPropsOff fast(parent, apidl, cidl);
        hr = cm->QueryContextMenu(m, 0, kIdFirst, kIdLast, flags);
    }
    if (FAILED(hr)) { DestroyMenu(m); return; }
    perf_event("menu.query", perf_now() - t0, GetMenuItemCount(m));

    bool single = cidl == 1;
    bool folder = single && is_plain_folder(parent, apidl[0]);
    // the folder itself; one found by a search at its real location (as double-click does)
    auto full = [&]() {
        PIDLIST_ABSOLUTE f = ILCombine(folderAbs, apidl[0]);
        if (f && search_is_results(folderAbs))
            if (PIDLIST_ABSOLUTE real = search_real_location(f)) { CoTaskMemFree(f); f = real; }
        return f;
    };
    // files (and folders) the media player plays: Play / Add to play queue
    std::vector<MediaItem> media;
    PlayerOffer offer = PlayerOffer::None;
    if (src == MenuSource::List && cidl <= 1000) {
        for (UINT i = 0; i < cidl; ++i) {
            SFGAOF a = SFGAO_FOLDER | SFGAO_STREAM | SFGAO_FILESYSTEM;
            if (FAILED(parent->GetAttributesOf(1, &apidl[i], &a)) || !(a & SFGAO_FILESYSTEM)) continue;
            bool dir = (a & SFGAO_FOLDER) && !(a & SFGAO_STREAM);
            PIDLIST_ABSOLUTE f = ILCombine(folderAbs, apidl[i]);
            if (f && search_is_results(folderAbs))
                if (PIDLIST_ABSOLUTE real = search_real_location(f)) { CoTaskMemFree(f); f = real; }
            PWSTR path = nullptr;
            if (f && SUCCEEDED(SHGetNameFromIDList(f, SIGDN_FILESYSPATH, &path)) && path) {
                media.push_back({ path, dir });
                CoTaskMemFree(path);
            }
            CoTaskMemFree(f);
        }
        offer = player_offer(media);
    }
    // (a picture: drawn on and written, by the picture editor)
    bool picture = media.size() == 1 && cidl == 1 && !media[0].folder && image_file(media[0].path);
    // (text files, all of them: each in a tab of the Notepad)
    bool text = !media.empty() && media.size() == cidl && media.size() <= 100 &&
                std::all_of(media.begin(), media.end(), [](const MediaItem& i) { return !i.folder && notepad_text_file(i.path); });
    std::vector<std::wstring> paths;
    std::vector<bool> dirs;
    for (auto& i : media) { paths.push_back(i.path); dirs.push_back(i.folder); }
    SyncMenu sync = sync_menu_for(paths, dirs);
    UINT id = track(hwnd, cm.get(), m, pt, [&] { return item_flyout(hwnd, cm.get(), m, pt, folder, offer, picture, text, sync); });
    if (id == kFlySyncNow) {
        sync_now_for(paths);
    } else if (id == kFlySyncExclude) {
        sync_exclude(paths[0], sync.exclude);
    } else if (id == kFlySyncVersions) {
        sync_version_history(hwnd, paths[0]);
    } else if (id == kFlyNotepad) {
        std::vector<std::wstring> files;
        for (auto& i : media) files.push_back(i.path);
        notepad_open(files);
    } else if (id == kFlyPlay || id == kFlyEnqueue) {
        player_open(media, id == kFlyEnqueue);
    } else if (id == kFlyPlaylist) {
        player_save_playlist(hwnd, media);
    } else if (id == kFlyEditPicture) {
        imageedit_open_file(media[0].path);
    } else if (id == kFlyNewTab) {
        PIDLIST_ABSOLUTE f = full();
        tabmodel_new_tab();
        navigate_owned(f);
    } else if (id == kFlyNewWindow) {
        PIDLIST_ABSOLUTE f = full();
        navigation_open_new_window(f);
        CoTaskMemFree(f);
    } else if (id >= kIdFirst && id <= kIdLast) {
        std::wstring verb = verb_of(cm.get(), id);
        // verbs the app handles itself so they act inside this window
        if ((verb == L"open" || verb == L"explore") && folder) {
            PIDLIST_ABSOLUTE f = full();
            navigate_owned(f);
        } else if (verb == L"opennewtab" && folder) {
            PIDLIST_ABSOLUTE f = full();
            tabmodel_new_tab();
            navigate_owned(f);
        } else if (verb == L"paste" && folder) {
            PIDLIST_ABSOLUTE f = full();                   // (the shell's paste copies on this thread)
            fileops_paste_into(f);
            CoTaskMemFree(f);
        } else if (verb == L"rename") {
            if (src == MenuSource::List) list_rename_selected();
            else tree_rename_menu_node();
        } else if (verb == L"delete" && src == MenuSource::Tree) {
            tree_delete_menu_node(key_down(VK_SHIFT));     // moves on to the next folder
        } else if (verb == L"delete") {
            list_delete_selection(key_down(VK_SHIFT));     // as the Delete key (preview, watcher)
        } else if ((verb == L"cut" || verb == L"copy") && src == MenuSource::List) {
            list_copy_selection(verb == L"cut");
        } else if (verb == L"copyaspath") {
            // (Windows' own gives up when the clipboard is busy for a moment: this asks again)
            std::vector<PIDLIST_ABSOLUTE> own;
            for (UINT i = 0; i < cidl; ++i) {
                PIDLIST_ABSOLUTE f = ILCombine(folderAbs, apidl[i]);
                if (f && search_is_results(folderAbs))
                    if (PIDLIST_ABSOLUTE real = search_real_location(f)) { CoTaskMemFree(f); f = real; }
                if (f) own.push_back(f);
            }
            fileops_copy_paths(std::vector<PCIDLIST_ABSOLUTE>(own.begin(), own.end()));
            for (auto p : own) CoTaskMemFree(p);
        } else {
            // a verb may move, compress or open the file for writing: the preview handler
            // lets go of it first (it comes back with the next selection change)
            if (src == MenuSource::List && !verb_keeps_files(verb)) detailspane_release_files();
            invoke_id(hwnd, cm.get(), id, pt, folderAbs);
            // Pin to / Unpin from Quick access changes the navigation pane
            if (verb == L"pintohome" || verb == L"unpinfromhome") tree_reread_pins();
        }
    }
    DestroyMenu(m);
}

// ------------------------------------------------------------------
// entry points
// ------------------------------------------------------------------
void show_items_context_menu(HWND hwnd, POINT pt) {
    auto sel = list_selected_pidls();
    if (sel.empty()) { show_bg_context_menu(hwnd, pt); return; }
    com_ptr<IShellFolder> parent;
    if (FAILED(bind_list(g_pane->current, IID_PPV_ARGS(parent.put()))) || !parent) return;
    // clone: a refresh while the menu is up would free the rows' pidls
    std::vector<PITEMID_CHILD> own;
    for (auto c : sel) own.push_back(pidl_clone_child(c));
    PIDLIST_ABSOLUTE folder = current_clone();
    show_cm(hwnd, pt, parent.get(), folder,
            (PCUITEMID_CHILD_ARRAY)own.data(), (UINT)own.size(), MenuSource::List);
    for (auto c : own) CoTaskMemFree(c);
    CoTaskMemFree(folder);
}

void show_tree_context_menu(HWND hwnd, POINT pt, PCIDLIST_ABSOLUTE abs) {
    if (pidl_is_desktop(abs)) {
        // the namespace root has no parent: use the Desktop's background menu
        com_ptr<IShellFolder> desk;
        com_ptr<IContextMenu> cm;
        if (FAILED(SHGetDesktopFolder(desk.put())) ||
            FAILED(desk->CreateViewObject(hwnd, IID_PPV_ARGS(cm.put()))) || !cm) return;
        HMENU m = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(m, 0, kIdFirst, kIdLast, CMF_NORMAL))) {
            UINT id = track(hwnd, cm.get(), m, pt);
            if (id >= kIdFirst && id <= kIdLast) invoke_id(hwnd, cm.get(), id, pt, abs);
        }
        DestroyMenu(m);
        return;
    }
    com_ptr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(bind_parent(abs, parent.put(), &child)) || !parent || !child) return;
    PIDLIST_ABSOLUTE parentAbs = pidl_clone(abs);
    ILRemoveLastID(parentAbs);
    show_cm(hwnd, pt, parent.get(), parentAbs, &child, 1, MenuSource::Tree);
    CoTaskMemFree(parentAbs);
}

static std::vector<std::wstring> folder_names(PCIDLIST_ABSOLUTE folder);

// The background menu's "New" submenu: the one whose (lazily filled) items include
// the "NewFolder" verb. Call while the handler is active (ActiveMenu).
static HMENU find_new_submenu(HWND hwnd, IContextMenu* cm, HMENU m, int from) {
    for (int i = from; i < GetMenuItemCount(m); ++i) {
        HMENU s = GetSubMenu(m, i);
        if (!s) continue;
        SendMessageW(hwnd, WM_INITMENUPOPUP, (WPARAM)s, i);
        for (int j = 0; j < GetMenuItemCount(s); ++j)
            if (verb_of(cm, GetMenuItemID(s, j)) == L"NewFolder") return s;
    }
    return nullptr;
}

static bool menu_has_id(HMENU m, UINT id) {
    if (!m) return false;
    for (int i = 0; i < GetMenuItemCount(m); ++i)
        if (GetMenuItemID(m, i) == id) return true;
    return false;
}

// runs a New item and selects the created item in rename mode, like Explorer
static void invoke_new_item(HWND hwnd, IContextMenu* cm, UINT id, POINT pt) {
    // (the folder's own copy: the command can pump messages, and a navigation meanwhile
    // frees g_pane->current)
    PIDLIST_ABSOLUTE folder = pidl_clone(g_pane->current);
    if (!folder) return;
    std::vector<std::wstring> was = folder_names(folder);
    std::unordered_set<std::wstring> before;
    for (auto& n : was) { CharLowerBuffW(n.data(), (DWORD)n.size()); before.insert(std::move(n)); }
    invoke_id(hwnd, cm, id, pt, folder, false);
    for (auto& n : folder_names(folder)) {
        std::wstring k = n;
        CharLowerBuffW(k.data(), (DWORD)k.size());
        if (!before.count(k)) {
            if (pidl_equal(folder, g_pane->current)) enum_set_pending_select(n, true);
            break;
        }
    }
    CoTaskMemFree(folder);
    navigation_refresh();
}

static UINT bg_flyout(HWND hwnd, IContextMenu* cm, HMENU m, POINT pt, HMENU view, HMENU sort,
                      HMENU group, int shellFrom, HMENU newSub) {
    std::vector<FlyButton> row;
    if (clipboard_has_files()) row.push_back({ IDM_EDIT_PASTE, L'\xE77F', L"Paste (Ctrl+V)" });
    std::vector<FlyEntry> list;
    auto sub = [&](HMENU h, const wchar_t* text, wchar_t glyph) {
        FlyEntry e = fly_item(0, text, glyph);
        e.kind = FlyEntry::Submenu;
        e.sub = h;
        list.push_back(e);
    };
    std::vector<ShellEntry> items;
    if (cm) items = menu_entries(cm, m, shellFrom);
    std::vector<HICON> icons;
    std::vector<const ShellEntry*> used;
    fly_promoted(list, items, icons, used);
    sub(view, L"&View", L'\xE8A9');
    sub(sort, L"&Sort by", L'\xE8CB');
    sub(group, L"Group &by", L'\xE8FD');
    list.push_back(fly_item(IDM_NAV_REFRESH, L"R&efresh", L'\xE72C'));
    if (newSub) {
        fly_separator(list);
        sub(newSub, L"Ne&w", L'\xE710');
    }
    fly_separator(list);
    if (auto e = find_verb(items, L"properties"); e && std::find(used.begin(), used.end(), e) == used.end())
        list.push_back(fly_from(*e, L'\xE90F', L"Alt+Enter"));
    fly_app_verbs(list, items, used);
    fly_finish(list);
    UINT id = flyout_track(hwnd, pt, row, list);
    for (HICON ic : icons) DestroyIcon(ic);
    return id;
}

// background: app's View / Sort by / Group by / Refresh, then the folder's own
// background menu (Paste, New, Open in Terminal, Properties, ...)
void show_bg_context_menu(HWND hwnd, POINT pt) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    HMENU view = menu_view_modes(), sort = menu_sort_by(), group = menu_group_by();
    AppendMenuW(m, MF_POPUP, (UINT_PTR)view, L"View");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sort, L"Sort by");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)group, L"Group by");
    AppendMenuW(m, MF_STRING, IDM_NAV_REFRESH, L"Refresh");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (clipboard_has_files() ? 0 : MF_GRAYED), IDM_EDIT_PASTE, L"Paste");
    UINT fixed = (UINT)GetMenuItemCount(m);

    com_ptr<IShellFolder> psf;
    com_ptr<IContextMenu> cm;
    if (SUCCEEDED(bind_list(g_pane->current, IID_PPV_ARGS(psf.put()))) && psf)
        psf->CreateViewObject(hwnd, IID_PPV_ARGS(cm.put()));
    if (cm && FAILED(cm->QueryContextMenu(m, fixed, kIdFirst, kIdLast, CMF_NORMAL)))
        cm.reset();
    // the folder menu brings its own Paste on most folders: drop ours then
    if (cm) {
        for (int i = (int)fixed; i < GetMenuItemCount(m); ++i) {
            if (verb_of(cm.get(), GetMenuItemID(m, i)) == L"paste") {
                // (ours sits right before the shell's items: they now start one earlier)
                if (DeleteMenu(m, IDM_EDIT_PASTE, MF_BYCOMMAND)) --fixed;
                break;
            }
        }
    }

    HMENU newSub = nullptr;
    UINT id = track(hwnd, cm.get(), m, pt, [&] {
        if (cm) newSub = find_new_submenu(hwnd, cm.get(), m, (int)fixed);
        return bg_flyout(hwnd, cm.get(), m, pt, view, sort, group, (int)fixed, newSub);
    });
    if (id >= kIdFirst && id <= kIdLast && cm) {
        std::wstring verb = verb_of(cm.get(), id);
        if (verb == L"paste") fileops_paste_into_current();
        else if (menu_has_id(newSub, id)) invoke_new_item(hwnd, cm.get(), id, pt);
        else invoke_id(hwnd, cm.get(), id, pt, g_pane->current);
    } else if (id) {
        SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(id, 0), 0);
    }
    DestroyMenu(m);    // destroys the View / Sort by submenus too
}

// in-folder names of the current folder, straight from the shell (not the list,
// which may be stale right after a create)
static std::vector<std::wstring> folder_names(PCIDLIST_ABSOLUTE folder) {
    std::vector<std::wstring> out;
    // a folder on disk: its names straight from the file system (the shell reads each item;
    // a folder of 50,000 files took seconds, twice per New)
    WCHAR path[MAX_PATH * 4]{};
    if (SHGetPathFromIDListEx(folder, path, ARRAYSIZE(path), GPFIDL_DEFAULT) && path[0]) {
        std::wstring pattern = std::wstring(path) + (path[wcslen(path) - 1] == L'\\' ? L"*" : L"\\*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) out.push_back(fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            return out;
        }
    }
    com_ptr<IShellFolder> psf;
    if (FAILED(bind_list(folder, IID_PPV_ARGS(psf.put()))) || !psf) return out;
    com_ptr<IEnumIDList> en;
    SHCONTF f = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN;
    if (psf->EnumObjects(g_main, f, en.put()) != S_OK || !en) return out;
    PITEMID_CHILD c = nullptr;
    while (en->Next(1, &c, nullptr) == S_OK) {
        out.push_back(child_name(psf.get(), c, SHGDN_INFOLDER | SHGDN_FORPARSING));   // file names
        CoTaskMemFree(c);
    }
    return out;
}

// Explorer's "New" menu (Folder, Shortcut, Text Document, ... from ShellNew), taken
// from the folder's background menu, which hosts the NewMenu handler the way
// Explorer does (a bare CLSID_NewMenu only offers Folder and Shortcut).
// The created item is selected and put into rename mode, like Explorer does.
void show_new_menu(HWND hwnd, POINT pt, UINT tpmAlign) {
    com_ptr<IShellFolder> psf;
    com_ptr<IContextMenu> cm;
    if (FAILED(bind_list(g_pane->current, IID_PPV_ARGS(psf.put()))) || !psf ||
        FAILED(psf->CreateViewObject(hwnd, IID_PPV_ARGS(cm.put()))) || !cm)
        return;
    HMENU host = CreatePopupMenu();
    if (FAILED(cm->QueryContextMenu(host, 0, kIdFirst, kIdLast, CMF_NORMAL))) { DestroyMenu(host); return; }
    UINT id = 0;
    {
        ActiveMenu active(cm.get());
        HMENU sub = find_new_submenu(hwnd, cm.get(), host, 0);
        if (sub) id = TrackPopupMenuEx(sub, TPM_RETURNCMD | TPM_LEFTBUTTON | tpmAlign, pt.x, pt.y, hwnd, nullptr);
        else MessageBeep(MB_ICONASTERISK);      // nothing can be created here (This PC, ...)
    }
    if (id >= kIdFirst && id <= kIdLast) invoke_new_item(hwnd, cm.get(), id, pt);
    DestroyMenu(host);
}
static bool invoke_verb_with(HWND hwnd, IShellFolder* parent, PIDLIST_ABSOLUTE folderAbs /*adopted*/,
                             std::vector<PITEMID_CHILD>& own /*adopted*/, const char* verb);

// invoke a canonical verb ("properties", "Windows.ModernShare", ...) on the
// selection, or on the current folder itself when nothing is selected
bool contextmenu_invoke_verb(HWND hwnd, const char* verb) {
    auto sel = list_selected_pidls();
    if (sel.empty()) return contextmenu_invoke_verb_on(hwnd, g_pane->current, verb);
    com_ptr<IShellFolder> parent;
    if (FAILED(bind_list(g_pane->current, IID_PPV_ARGS(parent.put())))) return false;
    std::vector<PITEMID_CHILD> own;
    for (auto c : sel) own.push_back(pidl_clone_child(c));
    return invoke_verb_with(hwnd, parent.get(), current_clone(), own, verb);
}

bool contextmenu_invoke_verb_on(HWND hwnd, PCIDLIST_ABSOLUTE item, const char* verb) {
    com_ptr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(bind_parent(item, parent.put(), &child)) || !child) return false;
    std::vector<PITEMID_CHILD> own{ pidl_clone_child(child) };
    PIDLIST_ABSOLUTE folderAbs = pidl_clone(item);
    ILRemoveLastID(folderAbs);
    return invoke_verb_with(hwnd, parent.get(), folderAbs, own, verb);
}

static bool invoke_verb_with(HWND hwnd, IShellFolder* parent, PIDLIST_ABSOLUTE folderAbs,
                             std::vector<PITEMID_CHILD>& own, const char* verb) {
    bool ok = false;
    com_ptr<IContextMenu> cm;
    if (SUCCEEDED(item_menu(hwnd, parent, folderAbs,
                            (PCUITEMID_CHILD_ARRAY)own.data(), (UINT)own.size(), cm)) && cm) {
        HMENU m = CreatePopupMenu();
        HRESULT hr;
        {
            MediaPropsOff fast(parent, (PCUITEMID_CHILD_ARRAY)own.data(), (UINT)own.size());
            hr = cm->QueryContextMenu(m, 0, kIdFirst, kIdLast, CMF_NORMAL);
        }
        if (SUCCEEDED(hr)) {
            WCHAR verbW[64]{};
            MultiByteToWideChar(CP_ACP, 0, verb, -1, verbW, 64);
            CMINVOKECOMMANDINFOEX ici{};
            ici.cbSize = sizeof(ici);
            ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
            ici.hwnd = hwnd;
            ici.lpVerb = verb;
            ici.lpVerbW = verbW;
            ici.nShow = SW_SHOWNORMAL;
            ok = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&ici)));
        }
        DestroyMenu(m);
    }
    for (auto c : own) CoTaskMemFree(c);
    CoTaskMemFree(folderAbs);
    return ok;
}

void contextmenu_properties(HWND hwnd) {
    if (contextmenu_invoke_verb(hwnd, "properties")) return;
    // virtual roots without a parent item
    if (pidl_is_desktop(g_pane->current) || list_selected_pidls().empty())
        SHObjectProperties(hwnd, SHOP_FILEPATH, pidl_folder_path(g_pane->current).c_str(), nullptr);
}

void contextmenu_properties_of(HWND hwnd, PCIDLIST_ABSOLUTE item) {
    if (!contextmenu_invoke_verb_on(hwnd, item, "properties"))
        SHObjectProperties(hwnd, SHOP_FILEPATH, pidl_folder_path(item).c_str(), nullptr);
}
