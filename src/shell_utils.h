#pragma once

#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <objbase.h>
#include <ShlGuid.h>
#include <propsys.h>
#include <propkey.h>
#include <objectarray.h>
#include "pkeys_compat.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "resource.h"

#define APP_CLASS L"ExplorerPlusMainWnd"

// com_ptr: minimal COM smart pointer
template <class T>
struct com_ptr {
    T* p = nullptr;
    com_ptr() = default;
    explicit com_ptr(T* raw) : p(raw) {}
    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;
    com_ptr(com_ptr&& o) noexcept : p(o.p) { o.p = nullptr; }
    com_ptr& operator=(com_ptr&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    ~com_ptr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    void attach(T* raw) { reset(); p = raw; }        // adopt WITHOUT AddRef
    T* get() const { return p; }
    T* operator->() const { return p; }
    T** put() { reset(); return &p; }
    T* detach() { T* t = p; p = nullptr; return t; }
    explicit operator bool() const { return p != nullptr; }
};

struct ComInit {
    HRESULT hr;
    ComInit() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); }
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr); }
};
struct OleInit {
    HRESULT hr;
    OleInit() { hr = OleInitialize(nullptr); }
    ~OleInit() { if (SUCCEEDED(hr)) OleUninitialize(); }
    bool ok() const { return SUCCEEDED(hr); }
};

// pidl helpers
// The Desktop (namespace root) is represented by an EMPTY absolute pidl; nullptr is
// accepted everywhere as a synonym, but g_current / tab state always hold a real pidl.
bool pidl_is_desktop(LPCITEMIDLIST pidl);
PITEMID_CHILD pidl_desktop();                               // newly allocated empty pidl
std::wstring pidl_abs_dispname(LPCITEMIDLIST abs);          // display name of an absolute pidl ("Downloads")
std::wstring pidl_parsingname(LPCITEMIDLIST abs);
std::wstring pidl_editingname(LPCITEMIDLIST abs);           // what Explorer shows in the address edit
PITEMID_CHILD pidl_clone(LPCITEMIDLIST pidl);
bool pidl_equal(LPCITEMIDLIST a, LPCITEMIDLIST b);          // canonical shell comparison
ULONGLONG pidl_sig(LPCITEMIDLIST pidl);    // stable content hash, works for relative pidls
std::wstring pidl_folder_path(LPCITEMIDLIST abs);           // fs path if possible else parsing name
std::wstring child_name(IShellFolder* psf, PCUITEMID_CHILD child, SHGDNF flags);

// typed path parsing (address bar)
HRESULT parse_typed(const std::wstring& text, PIDLIST_ABSOLUTE* out);

// open an item the way Explorer does (default verb on the ID list)
void shell_open_abs(HWND hwnd, PCIDLIST_ABSOLUTE abs);

// binding
HRESULT bind_list(PCIDLIST_ABSOLUTE pidl, REFIID riid, void** ppv);                    // bind folder
HRESULT bind_parent(PCIDLIST_ABSOLUTE pidl, IShellFolder** ppsf, PCUITEMID_CHILD* child);
HRESULT create_item(IShellFolder* parent, PCITEMID_CHILD child, REFIID riid, void** ppv);
HRESULT shell_item_for(PCIDLIST_ABSOLUTE abs, IShellItem** out);                       // nullptr = Desktop

// formatting
std::wstring fmt_size(ULONGLONG bytes);
std::wstring fmt_filetime(const FILETIME& ft);

bool pidl_exists(LPCITEMIDLIST abs);          // the item is (still) there
bool fs_folder_missing(LPCITEMIDLIST abs);    // a local file-system folder that is gone (never blocks on the network)

// system image list index for an absolute pidl (thread-safe); `fallback` if the shell has none
int sys_icon_index(LPCITEMIDLIST abs, bool open = false, int fallback = 0);
int sys_folder_icon_index(bool open);     // the stock folder icon
HIMAGELIST sys_imagelist(int shil);       // SHIL_SMALL / SHIL_LARGE / SHIL_EXTRALARGE (shared, never destroy)
void sys_imagelist_init();                // once, on the UI thread, before workers ask for icons

// create IDataObject from child PIDLs (same parent)
HRESULT dataobject_from_children(HWND hwnd, IShellFolder* psf,
                                 const std::vector<PCITEMID_CHILD>& children, IDataObject** out);
