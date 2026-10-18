#include "fileops.h"
#include "navigation.h"
#include "enumerate.h"
#include "detailspane.h"
#include "watcher.h"
#include "perf.h"
#include <algorithm>
#include <unordered_set>

// the watcher waits while an operation runs (they pump messages); deletes and renames
// also release its handle on the folder, which may be the one they act on
struct WatchHold {
    explicit WatchHold(bool releaseDir) { watcher_hold_begin(releaseDir); }
    ~WatchHold() { watcher_hold_end(); }
    WatchHold(const WatchHold&) = delete;
    WatchHold& operator=(const WatchHold&) = delete;
};

// ---------------------------------------------------------------
// clipboard helpers

// Whoever watches the clipboard (clipboard history, a clipboard manager, a remote desktop
// session) opens it for a moment after every change: a copy right after another one finds
// it held. Ask again for up to half a second rather than fail.
static void log_clipboard_busy(int tries) {
    if (!perf_on()) return;
    std::wstring holder;
    DWORD pid = 0;
    if (HWND w = GetOpenClipboardWindow()) GetWindowThreadProcessId(w, &pid);
    if (HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
        WCHAR path[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) holder = PathFindFileNameW(path);
        CloseHandle(p);
    }
    perf_event("clipboard.busy", -1, tries, holder.c_str());
}

template <class F> static bool with_clipboard_retry(F attempt) {
    for (int tries = 0; tries < 20; ++tries) {
        if (attempt()) {
            if (tries) log_clipboard_busy(tries);
            return true;
        }
        Sleep(25);
    }
    log_clipboard_busy(20);
    return false;
}

bool fileops_copy_paths(const std::vector<PCIDLIST_ABSOLUTE>& items) {
    std::wstring text;
    for (PCIDLIST_ABSOLUTE abs : items) {
        std::wstring s = pidl_folder_path(abs);
        if (s.empty()) continue;
        if (!text.empty()) text += L"\r\n";
        text += L"\"" + s + L"\"";
    }
    if (text.empty()) return false;
    if (!with_clipboard_retry([] { return OpenClipboard(g_main) != FALSE; })) {
        MessageBeep(MB_ICONASTERISK);
        return false;
    }
    EmptyClipboard();
    bool ok = false;
    size_t bytes = (text.size() + 1) * sizeof(WCHAR);
    if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(hg), text.c_str(), bytes);
        GlobalUnlock(hg);
        ok = SetClipboardData(CF_UNICODETEXT, hg) != nullptr;
        if (!ok) GlobalFree(hg);
    }
    CloseClipboard();
    return ok;
}

bool clipboard_has_files() {
    UINT cf = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    return IsClipboardFormatAvailable(cf) || IsClipboardFormatAvailable(CF_HDROP);
}

static bool clipboard_prefers_move(IDataObject* pdo) {
    FORMATETC fe{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT),
                  nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm{};
    bool move = false;
    if (SUCCEEDED(pdo->GetData(&fe, &sm))) {
        if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal) {
            if (void* p = GlobalLock(sm.hGlobal)) {
                move = (*(DWORD*)p & DROPEFFECT_MOVE) != 0;
                GlobalUnlock(sm.hGlobal);
            }
        }
        ReleaseStgMedium(&sm);
    }
    return move;
}

// "cut" ghosting: signatures of the cut children + the folder they live in, valid
// while the clipboard still holds what we put there
static std::unordered_set<ULONGLONG> s_cutSigs;
static ULONGLONG s_cutFolderSig = 0;
static DWORD s_cutClipSeq = 0;

void fileops_set_cut_items(const std::vector<PCITEMID_CHILD>& children) {
    s_cutSigs.clear();
    for (auto c : children) s_cutSigs.insert(pidl_sig(c));
    s_cutFolderSig = pidl_sig(g_pane->current);
    s_cutClipSeq = GetClipboardSequenceNumber();
}

bool fileops_is_cut(PCITEMID_CHILD child) {
    if (s_cutSigs.empty()) return false;
    if (GetClipboardSequenceNumber() != s_cutClipSeq) { s_cutSigs.clear(); return false; }
    return s_cutFolderSig == pidl_sig(g_pane->current) && s_cutSigs.count(pidl_sig(child));
}

// Drop `pdo` onto a folder through the folder's own drop target — the same path
// Explorer uses for paste, so collisions, "- Copy" naming, archives, undo and
// cross-volume rules behave identically. False when the folder refused it.
static bool drop_into_folder(PCIDLIST_ABSOLUTE folder, IDataObject* pdo, DWORD effect) {
    detailspane_release_files();        // a moved file may be the one previewed
    com_ptr<IShellFolder> psf;
    if (FAILED(bind_list(folder, IID_PPV_ARGS(psf.put()))) || !psf) return false;
    com_ptr<IDropTarget> dt;
    if (FAILED(psf->CreateViewObject(g_main, IID_PPV_ARGS(dt.put()))) || !dt) return false;
    POINTL pt{ 0, 0 };
    DWORD eff = effect;
    if (FAILED(dt->DragEnter(pdo, MK_LBUTTON, pt, &eff)) || !(eff & effect)) {
        dt->DragLeave();
        return false;
    }
    eff = effect;
    // (the effect written back isn't checked: shell targets report DROPEFFECT_NONE for
    // optimized moves and asynchronous copies that did happen)
    return SUCCEEDED(dt->Drop(pdo, 0, pt, &eff));
}

// after a cut was pasted: are its files all still where they were? (the user cancelled
// the conflict dialog or skipped everything: the cut stays on the clipboard)
static bool cut_files_still_there(IDataObject* pdo) {
    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm{};
    if (FAILED(pdo->GetData(&fe, &sm))) return false;     // (not files: can't tell)
    bool all = false;
    if (sm.tymed == TYMED_HGLOBAL && sm.hGlobal) {
        HDROP drop = (HDROP)sm.hGlobal;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        all = n > 0;
        for (UINT i = 0; i < n && i < 64 && all; ++i) {     // (a sample is enough)
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(drop, i, p.data(), len + 1);
            all = GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
    }
    ReleaseStgMedium(&sm);
    return all;
}

void fileops_paste_into_current() { fileops_paste_into(g_pane->current); }

void fileops_paste_into(PCIDLIST_ABSOLUTE target) {
    com_ptr<IDataObject> pdo;
    if (FAILED(OleGetClipboard(pdo.put())) || !pdo || !target) return;
    // its own copy: the drop can pump messages (a conflict dialog), and a navigation meanwhile
    // frees the pane's folder that `target` may be
    PIDLIST_ABSOLUTE folder = pidl_clone(target);
    if (!folder) return;
    bool move = clipboard_prefers_move(pdo.get());
    if (!drop_into_folder(folder, pdo.get(), move ? DROPEFFECT_MOVE : DROPEFFECT_COPY)) {
        MessageBeep(MB_ICONASTERISK);
        CoTaskMemFree(folder);
        return;
    }
    if (move && !cut_files_still_there(pdo.get())) {
        OleSetClipboard(nullptr);    // a cut is consumed by the paste
        s_cutSigs.clear();
    }
    for_each_pane([folder] { if (pidl_equal(folder, g_pane->current)) navigation_refresh(); }, true);
    CoTaskMemFree(folder);
}

bool fileops_set_clipboard(IShellFolder* parent, const std::vector<PCITEMID_CHILD>& children, bool cut) {
    if (!parent || children.empty()) return false;
    com_ptr<IDataObject> pdo;
    if (FAILED(dataobject_from_children(g_main, parent, children, pdo.put())) || !pdo) return false;
    FORMATETC fe{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT),
                  nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm{};
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (sm.hGlobal) {
        if (void* p = GlobalLock(sm.hGlobal)) {
            *(DWORD*)p = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(sm.hGlobal);
        }
        if (FAILED(pdo->SetData(&fe, &sm, TRUE))) GlobalFree(sm.hGlobal);
    }
    // (another program may hold the clipboard past the retries: then nothing is cut)
    HRESULT hr = E_FAIL;
    with_clipboard_retry([&] { hr = OleSetClipboard(pdo.get()); return hr != CLIPBRD_E_CANT_OPEN; });
    bool onClipboard = SUCCEEDED(hr);
    if (onClipboard) OleFlushClipboard();
    else MessageBeep(MB_ICONASTERISK);
    // the cut file is about to be moved by whoever pastes it: the preview lets go of it
    if (onClipboard && cut) detailspane_release_files();
    return onClipboard;
}

void fileops_delete_items(HWND hwnd, IShellFolder* parent,
                          const std::vector<PCITEMID_CHILD>& children, bool permanent, bool refresh) {
    if (!parent || children.empty()) return;
    detailspane_release_files();        // the preview handler may hold the file open
    com_ptr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(fo.put()))) || !fo) return;
    // permanent delete always asks "Are you sure...?"; recycling asks only when the
    // Recycle Bin's "Display delete confirmation dialog" setting is on (as Explorer) —
    // but an item that can't be recycled (network, too big) still asks before it is
    // deleted for good
    DWORD flags = permanent ? FOFX_EARLYFAILURE
                            : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD);
    if (!permanent) {
        SHELLSTATEW ss{};
        SHGetSetSettings(&ss, SSF_NOCONFIRMRECYCLE, FALSE);
        if (ss.fNoConfirmRecycle) flags |= FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING;
    }
    fo->SetOperationFlags(flags);
    fo->SetOwnerWindow(hwnd);
    com_ptr<IShellItemArray> arr;
    if (FAILED(SHCreateShellItemArray(nullptr, parent, (UINT)children.size(),
                                      (PCUITEMID_CHILD_ARRAY)children.data(), arr.put())) || !arr)
        return;
    if (SUCCEEDED(fo->DeleteItems(arr.get()))) {
        WatchHold hold(true);
        fo->PerformOperations();
    }
    if (refresh) navigation_refresh();
}

bool fileops_rename_item(HWND hwnd, IShellFolder* parent, PCITEMID_CHILD child,
                         const std::wstring& newName, bool exactFileName) {
    if (!parent || !child || newName.empty()) return false;
    detailspane_release_files();        // the preview handler may hold the file open
    bool renamed = false;
    if (exactFileName) {
        // the name carries an extension Windows hides: the shell's rename would add the
        // old one again ("b.txt.txt"), so set the file name as it is. That path doesn't check
        // the name the way the shell's rename does: "..\x.txt" or "sub\x.txt" would move it.
        if (newName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
            std::any_of(newName.begin(), newName.end(), [](wchar_t c) { return c < 32; }) ||
            newName == L"." || newName == L"..") {
            MessageBoxW(hwnd, L"A file name can't contain any of the following characters:\n\\ / : * ? \" < > |",
                        L"Rename", MB_OK | MB_ICONWARNING);
            return false;
        }
        PITEMID_CHILD out = nullptr;
        {
            WatchHold hold(true);
            renamed = SUCCEEDED(parent->SetNameOf(hwnd, child, newName.c_str(), SHGDN_INFOLDER | SHGDN_FORPARSING, &out));
        }
        if (renamed) enum_set_pending_select(newName, false);
        if (out) CoTaskMemFree(out);
        navigation_refresh();
        return renamed;
    }
    com_ptr<IShellItem> si;
    if (FAILED(create_item(parent, child, IID_PPV_ARGS(si.put()))) || !si) return false;
    com_ptr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(fo.put()))) || !fo) return false;
    fo->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
    fo->SetOwnerWindow(hwnd);
    BOOL aborted = FALSE;
    {
        WatchHold hold(true);
        renamed = SUCCEEDED(fo->RenameItem(si.get(), newName.c_str(), nullptr)) &&
                  SUCCEEDED(fo->PerformOperations()) &&
                  SUCCEEDED(fo->GetAnyOperationsAborted(&aborted)) && !aborted;
    }
    if (renamed) enum_set_pending_select(newName, false);     // keep the renamed item selected
    navigation_refresh();
    return renamed;
}

static bool name_exists(IShellFolder* psf, const std::wstring& name) {
    PIDLIST_RELATIVE p = nullptr;
    ULONG eaten = 0;
    HRESULT hr = psf->ParseDisplayName(nullptr, nullptr, const_cast<LPWSTR>(name.c_str()), &eaten, &p, nullptr);
    if (p) CoTaskMemFree(p);
    return SUCCEEDED(hr);
}

void fileops_new_folder() {
    com_ptr<IShellItem> dest;
    com_ptr<IShellFolder> psf;
    if (FAILED(shell_item_for(g_pane->current, dest.put())) || !dest) return;
    if (FAILED(bind_list(g_pane->current, IID_PPV_ARGS(psf.put()))) || !psf) return;
    com_ptr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(fo.put()))) || !fo) return;
    fo->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOF_NOCONFIRMMKDIR);
    fo->SetOwnerWindow(g_main);
    // unique name the way Explorer numbers it: "New folder", "New folder (2)", ...
    // (asks the folder itself, so hidden items count too)
    std::wstring nm = L"New folder";
    for (int i = 2; name_exists(psf.get(), nm) && i < 1000; ++i)
        nm = L"New folder (" + std::to_wstring(i) + L")";
    WatchHold hold(false);
    if (SUCCEEDED(fo->NewItem(dest.get(), FILE_ATTRIBUTE_DIRECTORY, nm.c_str(), nullptr, nullptr)) &&
        SUCCEEDED(fo->PerformOperations())) {
        enum_set_pending_select(nm, true);   // select + start renaming once listed
    }
    navigation_refresh();
}
