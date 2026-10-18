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
// accepted everywhere as a synonym, but g_pane->current / tab state always hold a real pidl.
bool pidl_is_desktop(PCUIDLIST_RELATIVE pidl);
PIDLIST_ABSOLUTE pidl_desktop();                            // newly allocated empty pidl
std::wstring pidl_abs_dispname(PCIDLIST_ABSOLUTE abs);      // display name of an absolute pidl ("Downloads")
std::wstring pidl_parsingname(PCIDLIST_ABSOLUTE abs);
std::wstring pidl_editingname(PCIDLIST_ABSOLUTE abs);       // what Explorer shows in the address edit
PIDLIST_ABSOLUTE pidl_clone(PCIDLIST_ABSOLUTE pidl);
PITEMID_CHILD pidl_clone_child(PCUITEMID_CHILD child);
bool pidl_equal(PCIDLIST_ABSOLUTE a, PCIDLIST_ABSOLUTE b);  // canonical shell comparison
ULONGLONG pidl_sig(PCUIDLIST_RELATIVE pidl);    // stable content hash, works for relative pidls
std::wstring pidl_folder_path(PCIDLIST_ABSOLUTE abs);       // fs path if possible else parsing name
std::wstring pidl_fs_path(PCIDLIST_ABSOLUTE abs);           // fs path (long ones too), else empty
std::wstring module_file();                                 // this exe's path (long ones too)
std::wstring child_name(IShellFolder* psf, PCUITEMID_CHILD child, SHGDNF flags);

// typed path parsing (address bar)
HRESULT parse_typed(const std::wstring& text, PIDLIST_ABSOLUTE* out);

// A window of its own on the taskbar, with its own icon, not grouped with the file windows
// (the player, the Notepad, sync, screenshots), and not pinnable (a pin would start the
// file manager). Set before the window is shown; nullptr when it closes.
void window_app_id(HWND hwnd, const wchar_t* appId);

// open an item the way Explorer does (default verb on the ID list)
void shell_open_abs(HWND hwnd, PCIDLIST_ABSOLUTE abs);

// binding
HRESULT bind_list(PCIDLIST_ABSOLUTE pidl, REFIID riid, void** ppv);                    // bind folder
HRESULT bind_parent(PCIDLIST_ABSOLUTE pidl, IShellFolder** ppsf, PCUITEMID_CHILD* child);
HRESULT create_item(IShellFolder* parent, PCITEMID_CHILD child, REFIID riid, void** ppv);
HRESULT shell_item_for(PCIDLIST_ABSOLUTE abs, IShellItem** out);                       // nullptr = Desktop

// formatting
std::wstring fmt_size(ULONGLONG bytes);         // "1.5 MB": drives, totals, the status bar
std::wstring fmt_size_kb(ULONGLONG bytes);      // "1 536 KB": a file's or folder's Size column
std::wstring fmt_filetime(const FILETIME& ft);

// Where a path lives, asked without touching the path itself (never blocks):
// a share or a mapped network drive (asking it anything waits for the server)
bool path_is_network(const std::wstring& path);
// a fixed disk inside the machine (not a share, not removable, not a USB / SD disk
// that Windows reports as fixed): safe to walk and to hold open
bool path_on_internal_drive(const std::wstring& path);

bool pidl_exists(PCIDLIST_ABSOLUTE abs);          // the item is (still) there
bool fs_folder_missing(PCIDLIST_ABSOLUTE abs);    // a local file-system folder that is gone (never blocks on the network)

// system image list index for an absolute pidl (thread-safe); `fallback` if the shell has none
int sys_icon_index(PCIDLIST_ABSOLUTE abs, bool open = false, int fallback = 0);
int sys_folder_icon_index(bool open);     // the stock folder icon
HIMAGELIST sys_imagelist(int shil);       // SHIL_SMALL / SHIL_LARGE / SHIL_EXTRALARGE (shared, never destroy)
void sys_imagelist_init();                // once at startup, before workers ask for icons

// create IDataObject from child PIDLs (same parent)
HRESULT dataobject_from_children(HWND hwnd, IShellFolder* psf,
                                 const std::vector<PCITEMID_CHILD>& children, IDataObject** out);
