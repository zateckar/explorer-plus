#include "shell_utils.h"
#include "pkeys_compat_def.h"
#include <cwctype>

bool pidl_is_desktop(LPCITEMIDLIST pidl) { return !pidl || ILIsEmpty(pidl); }

PITEMID_CHILD pidl_desktop() {
    // the namespace root is the empty list: just the 2-byte terminator
    auto p = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(sizeof(USHORT)));
    if (p) p->mkid.cb = 0;
    return p;
}

std::wstring pidl_abs_dispname(LPCITEMIDLIST abs) {
    std::wstring out;
    PIDLIST_ABSOLUTE root = nullptr;
    LPCITEMIDLIST p = abs;
    if (!p) { root = reinterpret_cast<PIDLIST_ABSOLUTE>(pidl_desktop()); p = root; }
    PWSTR dn = nullptr;
    if (p && SUCCEEDED(SHGetNameFromIDList(p, SIGDN_NORMALDISPLAY, &dn)) && dn) {
        out = dn; CoTaskMemFree(dn);
    }
    if (root) CoTaskMemFree(root);
    return out;
}

std::wstring pidl_parsingname(LPCITEMIDLIST abs) {
    if (!abs) return {};
    PWSTR dn = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetNameFromIDList(abs, SIGDN_DESKTOPABSOLUTEPARSING, &dn)) && dn) {
        out = dn; CoTaskMemFree(dn);
    }
    return out;
}

std::wstring pidl_editingname(LPCITEMIDLIST abs) {
    if (pidl_is_desktop(abs)) return pidl_abs_dispname(nullptr);
    PWSTR dn = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetNameFromIDList(abs, SIGDN_DESKTOPABSOLUTEEDITING, &dn)) && dn) {
        out = dn; CoTaskMemFree(dn);
    }
    if (out.empty()) out = pidl_abs_dispname(abs);
    return out;
}

PITEMID_CHILD pidl_clone(LPCITEMIDLIST pidl) {
    if (!pidl) return nullptr;
    return static_cast<PITEMID_CHILD>(ILClone(pidl));
}

bool pidl_equal(LPCITEMIDLIST a, LPCITEMIDLIST b) {
    bool da = pidl_is_desktop(a), db = pidl_is_desktop(b);
    if (da || db) return da == db;
    com_ptr<IShellFolder> dsf;
    if (FAILED(SHGetDesktopFolder(dsf.put()))) return false;
    HRESULT r = dsf->CompareIDs(SHCIDS_CANONICALONLY, a, b);
    return SUCCEEDED(r) && (SHORT)HRESULT_CODE(r) == 0;
}

ULONGLONG pidl_sig(LPCITEMIDLIST pidl) {
    // FNV-1a over the pidl bytes
    if (!pidl) return 0;
    ULONGLONG s = 1469598103934665603ULL;
    const UINT cb = ILGetSize(const_cast<LPITEMIDLIST>(pidl));
    const BYTE* p = reinterpret_cast<const BYTE*>(pidl);
    for (UINT i = 0; i < cb; ++i) { s ^= p[i]; s *= 1099511628211ULL; }
    return s;
}

std::wstring pidl_folder_path(LPCITEMIDLIST abs) {
    if (!abs) return {};
    WCHAR buf[MAX_PATH]{};
    if (!ILIsEmpty(abs) && SHGetPathFromIDListW(const_cast<LPITEMIDLIST>(abs), buf)) return buf;
    return pidl_parsingname(abs);
}

std::wstring child_name(IShellFolder* psf, PCUITEMID_CHILD child, SHGDNF flags) {
    std::wstring out;
    if (!psf || !child) return out;
    STRRET s{};
    if (SUCCEEDED(psf->GetDisplayNameOf(child, flags, &s))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(StrRetToStrW(&s, child, &p)) && p) { out = p; CoTaskMemFree(p); }
    }
    return out;
}

HRESULT parse_typed(const std::wstring& text, PIDLIST_ABSOLUTE* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::wstring t = text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    size_t st = 0; while (st < t.size() && iswspace(t[st])) ++st;
    t.erase(0, st);
    // Strip quotes often pasted from command lines
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') t = t.substr(1, t.size() - 2);
    if (t.empty()) return E_INVALIDARG;
    // %USERPROFILE%\... and friends
    if (t.find(L'%') != std::wstring::npos) {
        WCHAR exp[4096]{};
        DWORD n = ExpandEnvironmentStringsW(t.c_str(), exp, ARRAYSIZE(exp));
        if (n && n <= ARRAYSIZE(exp)) t = exp;
    }
    PIDLIST_ABSOLUTE p = nullptr;
    HRESULT hr = SHParseDisplayName(t.c_str(), nullptr, &p, 0, nullptr);
    if (SUCCEEDED(hr) && p) { *out = p; return S_OK; }
    // display names of well-known roots ("Desktop", "This PC", ...)
    if (StrCmpIW(t.c_str(), pidl_abs_dispname(nullptr).c_str()) == 0) {
        *out = reinterpret_cast<PIDLIST_ABSOLUTE>(pidl_desktop());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

void shell_open_abs(HWND hwnd, PCIDLIST_ABSOLUTE abs) {
    if (!abs) return;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask  = SEE_MASK_INVOKEIDLIST | SEE_MASK_ASYNCOK | SEE_MASK_FLAG_LOG_USAGE;
    sei.hwnd   = hwnd;
    sei.lpIDList = const_cast<LPITEMIDLIST>(abs);
    sei.nShow  = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

HRESULT bind_parent(PCIDLIST_ABSOLUTE pidl, IShellFolder** ppsf, PCUITEMID_CHILD* child) {
    if (!ppsf) return E_POINTER;
    *ppsf = nullptr;
    if (child) *child = nullptr;
    if (pidl_is_desktop(pidl)) return E_INVALIDARG;   // the root has no parent
    return SHBindToParent(pidl, IID_PPV_ARGS(ppsf), child);
}

HRESULT bind_list(PCIDLIST_ABSOLUTE pidl, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    com_ptr<IShellFolder> desktop;
    HRESULT hr = SHGetDesktopFolder(desktop.put());
    if (FAILED(hr)) return hr;
    if (pidl_is_desktop(pidl)) return desktop->QueryInterface(riid, ppv);
    return desktop->BindToObject(pidl, nullptr, riid, ppv);
}

HRESULT create_item(IShellFolder* parent, PCITEMID_CHILD child, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    return SHCreateItemWithParent(nullptr, parent, child, riid, ppv);
}

HRESULT shell_item_for(PCIDLIST_ABSOLUTE abs, IShellItem** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!abs) {
        PITEMID_CHILD root = pidl_desktop();
        HRESULT hr = root ? SHCreateItemFromIDList(root, IID_PPV_ARGS(out)) : E_OUTOFMEMORY;
        if (root) CoTaskMemFree(root);
        return hr;
    }
    return SHCreateItemFromIDList(abs, IID_PPV_ARGS(out));
}

std::wstring fmt_size(ULONGLONG bytes) {
    WCHAR buf[64]{};
    if (SUCCEEDED(StrFormatByteSizeEx(bytes, SFBS_FLAGS_TRUNCATE_UNDISPLAYED_DECIMAL_DIGITS, buf, 64)))
        return buf;
    return std::to_wstring(bytes);
}

std::wstring fmt_filetime(const FILETIME& ft) {
    ULARGE_INTEGER ui{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    if (!ui.QuadPart) return {};
    SYSTEMTIME utc{}, st{};
    if (!FileTimeToSystemTime(&ft, &utc)) return {};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st)) return {};
    WCHAR db[64]{}, tb[64]{};
    int dl = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, db, 64, nullptr);
    int tl = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, tb, 64);
    if (!dl || !tl) return {};
    std::wstring out = db;
    out += L' ';
    out += tb;
    return out;
}

int sys_icon_index(LPCITEMIDLIST abs, bool open) {
    PITEMID_CHILD root = nullptr;
    if (!abs) { root = pidl_desktop(); abs = root; }
    SHFILEINFOW si{};
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_PIDL | SHGFI_SMALLICON | (open ? SHGFI_OPENICON : 0);
    DWORD_PTR r = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(abs), 0, &si, sizeof(si), flags);
    if (root) CoTaskMemFree(root);
    return r ? si.iIcon : 0;
}

HIMAGELIST sys_imagelist(int shil) {
    // (this MinGW's uuid library lacks IID_IImageList)
    static const IID kIID_IImageList =
        { 0x46EB5926, 0x582E, 0x4017, { 0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50 } };
    IImageList* il = nullptr;
    if (FAILED(SHGetImageList(shil, kIID_IImageList, (void**)&il)) || !il) return nullptr;
    // the system image list lives for the process; drop the extra reference
    HIMAGELIST h = reinterpret_cast<HIMAGELIST>(il);
    il->Release();
    return h;
}

HRESULT dataobject_from_children(HWND hwnd, IShellFolder* psf,
                                 const std::vector<PCITEMID_CHILD>& children, IDataObject** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!psf || children.empty()) return E_INVALIDARG;
    return psf->GetUIObjectOf(hwnd, (UINT)children.size(),
                              (PCUITEMID_CHILD_ARRAY)children.data(),
                              IID_IDataObject, nullptr, reinterpret_cast<void**>(out));
}
