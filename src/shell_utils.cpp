#include "shell_utils.h"
#include "cmdline.h"
#include "pkeys_compat_def.h"
#include <propvarutil.h>
#include <winioctl.h>
#include <cstddef>
#include <cwctype>
#include <atomic>

bool pidl_is_desktop(PCUIDLIST_RELATIVE pidl) { return !pidl || ILIsEmpty(pidl); }

PIDLIST_ABSOLUTE pidl_desktop() {
    // the namespace root is the empty list: just the 2-byte terminator
    auto p = static_cast<PIDLIST_ABSOLUTE>(CoTaskMemAlloc(sizeof(USHORT)));
    if (p) p->mkid.cb = 0;
    return p;
}

std::wstring pidl_abs_dispname(PCIDLIST_ABSOLUTE abs) {
    std::wstring out;
    PIDLIST_ABSOLUTE root = nullptr;
    PCIDLIST_ABSOLUTE p = abs;
    if (!p) { root = pidl_desktop(); p = root; }
    PWSTR dn = nullptr;
    if (p && SUCCEEDED(SHGetNameFromIDList(p, SIGDN_NORMALDISPLAY, &dn)) && dn) {
        out = dn; CoTaskMemFree(dn);
    }
    if (root) CoTaskMemFree(root);
    return out;
}

std::wstring pidl_parsingname(PCIDLIST_ABSOLUTE abs) {
    if (!abs) return {};
    PWSTR dn = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetNameFromIDList(abs, SIGDN_DESKTOPABSOLUTEPARSING, &dn)) && dn) {
        out = dn; CoTaskMemFree(dn);
    }
    return out;
}

std::wstring pidl_editingname(PCIDLIST_ABSOLUTE abs) {
    if (pidl_is_desktop(abs)) return pidl_abs_dispname(nullptr);
    PWSTR dn = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetNameFromIDList(abs, SIGDN_DESKTOPABSOLUTEEDITING, &dn)) && dn) {
        out = dn; CoTaskMemFree(dn);
    }
    if (out.empty()) out = pidl_abs_dispname(abs);
    return out;
}

PIDLIST_ABSOLUTE pidl_clone(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return nullptr;
    return static_cast<PIDLIST_ABSOLUTE>(ILClone(pidl));
}

PITEMID_CHILD pidl_clone_child(PCUITEMID_CHILD child) {
    if (!child) return nullptr;
    return static_cast<PITEMID_CHILD>(ILClone(child));
}

bool pidl_equal(PCIDLIST_ABSOLUTE a, PCIDLIST_ABSOLUTE b) {
    bool da = pidl_is_desktop(a), db = pidl_is_desktop(b);
    if (da || db) return da == db;
    com_ptr<IShellFolder> dsf;
    if (FAILED(SHGetDesktopFolder(dsf.put()))) return false;
    HRESULT r = dsf->CompareIDs(SHCIDS_CANONICALONLY, a, b);
    return SUCCEEDED(r) && (SHORT)HRESULT_CODE(r) == 0;
}

ULONGLONG pidl_sig(PCUIDLIST_RELATIVE pidl) {
    // FNV-1a over the pidl bytes
    if (!pidl) return 0;
    ULONGLONG s = 1469598103934665603ULL;
    const UINT cb = ILGetSize(pidl);
    const BYTE* p = reinterpret_cast<const BYTE*>(pidl);
    for (UINT i = 0; i < cb; ++i) { s ^= p[i]; s *= 1099511628211ULL; }
    return s;
}

std::wstring pidl_folder_path(PCIDLIST_ABSOLUTE abs) {
    if (!abs) return {};
    WCHAR buf[MAX_PATH]{};
    if (!ILIsEmpty(abs) && SHGetPathFromIDListW(abs, buf)) return buf;
    return pidl_parsingname(abs);
}

bool pidl_exists(PCIDLIST_ABSOLUTE abs) {
    if (pidl_is_desktop(abs)) return true;
    WCHAR path[MAX_PATH]{};
    if (SHGetPathFromIDListW(abs, path))
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    com_ptr<IShellItem> si;
    SFGAOF a = SFGAO_VALIDATE;
    return SUCCEEDED(SHCreateItemFromIDList(abs, IID_PPV_ARGS(si.put()))) && si &&
           SUCCEEDED(si->GetAttributes(SFGAO_VALIDATE, &a));
}

// "C:\..." or "\\?\C:\...": the drive letter, else 0
static wchar_t drive_letter(const std::wstring& path) {
    size_t at = path.rfind(L"\\\\?\\", 0) == 0 ? 4 : 0;
    if (path.size() >= at + 2 && iswalpha(path[at]) && path[at + 1] == L':') return (wchar_t)towupper(path[at]);
    return 0;
}

bool path_is_network(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return true;
    if (wchar_t d = drive_letter(path)) {
        WCHAR root[4] = { d, L':', L'\\', 0 };
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return path.rfind(L"\\\\", 0) == 0 && path.rfind(L"\\\\?\\", 0) != 0 && path.rfind(L"\\\\.\\", 0) != 0;
}

// the storage bus of a drive letter's disk; USB / SD / FireWire disks can report
// DRIVE_FIXED. Asked once per drive (the answer is kept for a minute: letters move).
static bool drive_on_external_bus(wchar_t d) {
    static std::mutex mx;
    static ULONGLONG checked[26]{};
    static bool external[26]{};
    int i = d - L'A';
    if (i < 0 || i >= 26) return false;
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(mx);
        if (checked[i] && now - checked[i] < 60000) return external[i];
    }
    bool ext = false;
    WCHAR dev[8] = { L'\\', L'\\', L'.', L'\\', d, L':', 0 };
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        BYTE buf[512]{};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, nullptr) &&
            got >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE)) {
            auto bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf)->BusType;
            ext = bus == BusTypeUsb || bus == BusType1394 || bus == BusTypeSd || bus == BusTypeMmc;
        }
        CloseHandle(h);
    }
    std::lock_guard<std::mutex> lk(mx);
    checked[i] = now;
    external[i] = ext;
    return ext;
}

bool path_on_internal_drive(const std::wstring& path) {
    wchar_t d = drive_letter(path);
    if (!d) return false;
    WCHAR root[4] = { d, L':', L'\\', 0 };
    return GetDriveTypeW(root) == DRIVE_FIXED && !drive_on_external_bus(d);
}

// a file-system folder known to be missing (not found, as opposed to unreadable / a
// drive without its disk); asks the file system only for local drives
bool fs_folder_missing(PCIDLIST_ABSOLUTE abs) {
    WCHAR path[MAX_PATH]{};
    if (pidl_is_desktop(abs) || !SHGetPathFromIDListW(abs, path)) return false;
    if (path[1] != L':' || path_is_network(path)) return false;     // a share: could block
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return false;
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
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
    std::wstring t = typed_text_clean(text);
    if (t.empty()) return E_INVALIDARG;
    PIDLIST_ABSOLUTE p = nullptr;
    HRESULT hr = SHParseDisplayName(t.c_str(), nullptr, &p, 0, nullptr);
    if (SUCCEEDED(hr) && p) { *out = p; return S_OK; }
    // display names of well-known roots ("Desktop", "This PC", ...)
    if (StrCmpIW(t.c_str(), pidl_abs_dispname(nullptr).c_str()) == 0) {
        *out = pidl_desktop();
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

void window_app_id(HWND hwnd, const wchar_t* appId) {
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&ps))) || !ps) return;
    PROPVARIANT v;
    if (appId) InitPropVariantFromBoolean(TRUE, &v); else PropVariantInit(&v);
    ps->SetValue(PKEY_AppUserModel_PreventPinning, v);
    PropVariantClear(&v);
    if (appId) InitPropVariantFromString(appId, &v); else PropVariantInit(&v);
    ps->SetValue(PKEY_AppUserModel_ID, v);
    PropVariantClear(&v);
    ps->Commit();
    ps->Release();
}

void shell_open_abs(HWND hwnd, PCIDLIST_ABSOLUTE abs) {
    if (!abs) return;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask  = SEE_MASK_INVOKEIDLIST | SEE_MASK_ASYNCOK | SEE_MASK_FLAG_LOG_USAGE;
    sei.hwnd   = hwnd;
    sei.lpIDList = const_cast<PIDLIST_ABSOLUTE>(abs);
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
        PIDLIST_ABSOLUTE root = pidl_desktop();
        HRESULT hr = root ? SHCreateItemFromIDList(root, IID_PPV_ARGS(out)) : E_OUTOFMEMORY;
        if (root) CoTaskMemFree(root);
        return hr;
    }
    return SHCreateItemFromIDList(abs, IID_PPV_ARGS(out));
}

// the Size column of files and folders, as in Explorer: whole kilobytes, rounded up
// ("1 KB" for a 1-byte file), with the locale's digit grouping and unit text
std::wstring fmt_size_kb(ULONGLONG bytes) {
    WCHAR buf[64]{};
    if (StrFormatKBSizeW((LONGLONG)bytes, buf, 64)) return buf;
    return std::to_wstring((bytes + 1023) / 1024) + L" KB";
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

int sys_icon_index(PCIDLIST_ABSOLUTE abs, bool open, int fallback) {
    PIDLIST_ABSOLUTE root = nullptr;
    if (!abs) { root = pidl_desktop(); abs = root; }
    SHFILEINFOW si{};
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_PIDL | SHGFI_SMALLICON | (open ? SHGFI_OPENICON : 0);
    // (a lookup can fail while another thread is adding to the image list: ask again)
    DWORD_PTR r = 0;
    for (int attempt = 0; attempt < 3 && !r; ++attempt) {
        if (attempt) Sleep(10);
        r = SHGetFileInfoW(reinterpret_cast<LPCWSTR>(abs), 0, &si, sizeof(si), flags);
    }
    if (root) CoTaskMemFree(root);
    return r ? si.iIcon : fallback;
}

int sys_folder_icon_index(bool open) {
    SHSTOCKICONINFO sii{ sizeof(sii) };
    if (SUCCEEDED(SHGetStockIconInfo(open ? SIID_FOLDEROPEN : SIID_FOLDER, SHGSI_SYSICONINDEX, &sii)))
        return sii.iSysImageIndex;
    return 0;
}

void sys_imagelist_init() {
    // FileIconInit: the full system image list, as Explorer has it (shell32 #660; not
    // in every SDK's import library)
    using FileIconInitFn = BOOL (WINAPI*)(BOOL);
    if (HMODULE sh = GetModuleHandleW(L"shell32.dll")) {
        auto init = (FileIconInitFn)(void*)GetProcAddress(sh, "FileIconInit");
        if (!init) init = (FileIconInitFn)(void*)GetProcAddress(sh, MAKEINTRESOURCEA(660));
        if (init) init(TRUE);
    }
    sys_imagelist(SHIL_SMALL);
    sys_imagelist(SHIL_LARGE);
    sys_imagelist(SHIL_EXTRALARGE);
    sys_folder_icon_index(false);            // the stock folder icons are in it from now on
    sys_folder_icon_index(true);
}

HIMAGELIST sys_imagelist(int shil) {
    // (this MinGW's uuid library lacks IID_IImageList)
    static const IID kIID_IImageList =
        { 0x46EB5926, 0x582E, 0x4017, { 0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50 } };
    // the system image lists live for the process: asked for once (asking takes the lock
    // that workers adding icons hold, and rows ask as they are drawn)
    static std::atomic<HIMAGELIST> known[SHIL_LAST + 1]{};
    if (shil < 0 || shil > SHIL_LAST) return nullptr;
    if (HIMAGELIST h = known[shil].load()) return h;
    IImageList* il = nullptr;
    if (FAILED(SHGetImageList(shil, kIID_IImageList, (void**)&il)) || !il) return nullptr;
    // (drop the extra reference)
    HIMAGELIST h = reinterpret_cast<HIMAGELIST>(il);
    il->Release();
    known[shil] = h;
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
