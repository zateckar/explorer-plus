#include "fileops.h"
#include "navigation.h"
#include "enumerate.h"
#include <unordered_set>

// ---------------------------------------------------------------
// clipboard helpers

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
    s_cutFolderSig = pidl_sig(g_current);
    s_cutClipSeq = GetClipboardSequenceNumber();
}

bool fileops_is_cut(PCITEMID_CHILD child) {
    if (s_cutSigs.empty()) return false;
    if (GetClipboardSequenceNumber() != s_cutClipSeq) { s_cutSigs.clear(); return false; }
    return s_cutFolderSig == pidl_sig(g_current) && s_cutSigs.count(pidl_sig(child));
}

// Drop `pdo` onto a folder through the folder's own drop target — the same path
// Explorer uses for paste, so collisions, "- Copy" naming, archives, undo and
// cross-volume rules behave identically.
static bool drop_into_folder(PCIDLIST_ABSOLUTE folder, IDataObject* pdo, DWORD effect) {
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
    return SUCCEEDED(dt->Drop(pdo, 0, pt, &eff));
}

void fileops_paste_into_current() {
    com_ptr<IDataObject> pdo;
    if (FAILED(OleGetClipboard(pdo.put())) || !pdo) return;
    bool move = clipboard_prefers_move(pdo.get());
    if (!drop_into_folder(g_current, pdo.get(), move ? DROPEFFECT_MOVE : DROPEFFECT_COPY)) {
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    if (move) {
        OleSetClipboard(nullptr);    // a cut is consumed by the paste
        s_cutSigs.clear();
    }
    navigation_refresh();
}

void fileops_delete_items(HWND hwnd, IShellFolder* parent,
                          const std::vector<PCITEMID_CHILD>& children, bool permanent) {
    if (!parent || children.empty()) return;
    com_ptr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(fo.put()))) || !fo) return;
    // permanent delete always asks "Are you sure...?"; recycling asks only when the
    // Recycle Bin's "Display delete confirmation dialog" setting is on (as Explorer)
    DWORD flags = permanent ? FOFX_EARLYFAILURE
                            : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD);
    if (!permanent) {
        SHELLSTATEW ss{};
        SHGetSetSettings(&ss, SSF_NOCONFIRMRECYCLE, FALSE);
        if (ss.fNoConfirmRecycle) flags |= FOF_NOCONFIRMATION;
    }
    fo->SetOperationFlags(flags);
    fo->SetOwnerWindow(hwnd);
    com_ptr<IShellItemArray> arr;
    if (FAILED(SHCreateShellItemArray(nullptr, parent, (UINT)children.size(),
                                      (PCUITEMID_CHILD_ARRAY)children.data(), arr.put())) || !arr)
        return;
    if (SUCCEEDED(fo->DeleteItems(arr.get())))
        fo->PerformOperations();
    navigation_refresh();
}

void fileops_rename_item(HWND hwnd, IShellFolder* parent, PCITEMID_CHILD child,
                         const std::wstring& newName, bool exactFileName) {
    if (!parent || !child || newName.empty()) return;
    if (exactFileName) {
        // the name carries an extension Windows hides: the shell's rename would add the
        // old one again ("b.txt.txt"), so set the file name as it is
        PITEMID_CHILD out = nullptr;
        if (SUCCEEDED(parent->SetNameOf(hwnd, child, newName.c_str(), SHGDN_INFOLDER | SHGDN_FORPARSING, &out)))
            enum_set_pending_select(newName, false);
        if (out) CoTaskMemFree(out);
        navigation_refresh();
        return;
    }
    com_ptr<IShellItem> si;
    if (FAILED(create_item(parent, child, IID_PPV_ARGS(si.put()))) || !si) return;
    com_ptr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(fo.put()))) || !fo) return;
    fo->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
    fo->SetOwnerWindow(hwnd);
    BOOL aborted = FALSE;
    if (SUCCEEDED(fo->RenameItem(si.get(), newName.c_str(), nullptr)) &&
        SUCCEEDED(fo->PerformOperations()) &&
        SUCCEEDED(fo->GetAnyOperationsAborted(&aborted)) && !aborted)
        enum_set_pending_select(newName, false);     // keep the renamed item selected
    navigation_refresh();
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
    if (FAILED(shell_item_for(g_current, dest.put())) || !dest) return;
    if (FAILED(bind_list(g_current, IID_PPV_ARGS(psf.put()))) || !psf) return;
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
    if (SUCCEEDED(fo->NewItem(dest.get(), FILE_ATTRIBUTE_DIRECTORY, nm.c_str(), nullptr, nullptr)) &&
        SUCCEEDED(fo->PerformOperations())) {
        enum_set_pending_select(nm, true);   // select + start renaming once listed
    }
    navigation_refresh();
}
