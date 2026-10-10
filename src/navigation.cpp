#include "navigation.h"
#include "tabs.h"
#include "settings.h"
#include "watcher.h"
#include "enumerate.h"
#include "tree.h"
#include "navbar.h"
#include "listview.h"
#include "detailspane.h"
#include "groups.h"
#include "searchbox.h"
#include "columns.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include "workers.h"

static std::vector<std::wstring> g_addrHistory;
// bumped by every navigation; a typed path resolved after that is dropped
static std::atomic<unsigned> s_typedGen{ 0 };

std::vector<std::wstring> address_history_list() { return g_addrHistory; }
void address_history_add(const std::wstring& s) {
    if (s.empty()) return;
    auto it = std::find(g_addrHistory.begin(), g_addrHistory.end(), s);
    if (it != g_addrHistory.end()) g_addrHistory.erase(it);
    g_addrHistory.insert(g_addrHistory.begin(), s);
    if (g_addrHistory.size() > 32) g_addrHistory.resize(32);
}

// (re)list the current folder; keepView = refresh in place
static void start_enum_current(bool keepView) {
    enumerate_start(g_current, keepView);
    watcher_register_for(g_current);
}

// Apply a tab's state to the UI: current folder, view, address, tree, strip.
void navigate_replace(TabState* t) {
    if (!t) return;
    ++s_typedGen;
    current_replace(pidl_clone(t->current));
    set_view_mode(t->view, false);
    SetWindowTextW(g_address, current_display_path().c_str());
    start_enum_current(false);
    tree_sync_to_current_deferred();
    navbar_refresh();
    tabstrip_refresh();
    detailspane_update();
    search_on_navigated();
    std::wstring title = t->name.empty() ? L"Explorer Plus" : (t->name + L" \x2013 Explorer Plus");
    SetWindowTextW(g_main, title.c_str());
}

// settings key of a folder's view and columns; all search results share one
std::wstring folder_settings_key(PCIDLIST_ABSOLUTE abs) {
    if (search_is_results(abs)) return L"::search-results";
    std::wstring key = pidl_parsingname(abs);
    if (key.empty()) key = pidl_abs_dispname(abs);
    return key;
}

// per-folder view memory; falls back to the last view chosen anywhere
View view_for_folder(PCIDLIST_ABSOLUTE abs) {
    const View* v = settings_view_for(folder_settings_key(abs));
    if (v) return *v;
    if (is_this_pc_folder(abs)) return View{ LV_VIEW_TILE, 0, GB_AUTO };   // drive tiles
    if (search_is_results(abs)) return View{ LV_VIEW_DETAILS, 0, GB_AUTO };
    return settings_default_view();
}

// Folder Options' "Apply to Folders": this folder's view and columns become the
// default of every file-system folder, and the ones folders kept for themselves are
// dropped (virtual folders, This PC, search results keep theirs)
void views_apply_to_all() {
    if (!columns_can_apply_to_all()) { MessageBeep(MB_ICONASTERISK); return; }
    columns_set_default_from_current();
    settings_set_default_view(g_view);
    settings_forget_fs_folders();
}

// "Reset Folders": every file-system folder back to Details with the standard columns
void views_reset_all() {
    settings_forget_fs_folders();
    columns_forget_default();
    settings_set_default_view(View{ LV_VIEW_DETAILS, 0, GB_AUTO });
    if (TabState* t = tabmodel_active()) {
        t->view = view_for_folder(t->current);
        navigate_replace(t);            // lists again with the folder's (default) columns
    }
}

// ordinary navigation within the active tab (pushes history)
void navigate_owned(PITEMID_CHILD pidl) {
    if (!pidl) pidl = pidl_desktop();
    TabState* t = tabmodel_active();
    if (t && pidl_equal(t->current, pidl)) {        // same folder: just refresh
        CoTaskMemFree(pidl);
        navigation_refresh();
        return;
    }
    tabmodel_on_navigate(pidl, true);   // adopts pidl
    if (t) t->view = view_for_folder(t->current);
    navigate_replace(tabmodel_active());
    address_history_add(current_display_path());
}

void navigate_to(PCIDLIST_ABSOLUTE pidl) {
    navigate_owned(pidl ? pidl_clone(pidl) : pidl_desktop());
}

// Folders (including resolved folder shortcuts) open in place; everything else is
// handed to the shell's default verb.
void navigation_open_abs(PCIDLIST_ABSOLUTE abs) {
    if (!abs) return;
    com_ptr<IShellItem> si;
    if (FAILED(shell_item_for(abs, si.put())) || !si) return;
    SFGAOF attrs = 0;
    si->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM | SFGAO_LINK, &attrs);
    if ((attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM)) { navigate_to(abs); return; }
    if (attrs & SFGAO_LINK) {
        com_ptr<IShellLinkW> link;
        if (SUCCEEDED(si->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(link.put()))) && link) {
            PIDLIST_ABSOLUTE target = nullptr;
            if (SUCCEEDED(link->GetIDList(&target)) && target) {
                com_ptr<IShellItem> ti;
                SFGAOF ta = 0;
                if (SUCCEEDED(SHCreateItemFromIDList(target, IID_PPV_ARGS(ti.put()))) && ti)
                    ti->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &ta);
                if ((ta & SFGAO_FOLDER) && !(ta & SFGAO_STREAM)) {
                    navigate_owned(reinterpret_cast<PITEMID_CHILD>(target));
                    return;
                }
                CoTaskMemFree(target);
            }
        }
    }
    shell_open_abs(g_main, abs);
}

// Typed-path resolution runs on a worker: SHParseDisplayName / PathFileExists on
// an unreachable "\\server\share" can block for tens of seconds. Any navigation
// that happens meanwhile supersedes the pending one (generation counter).

struct TypedNav {
    unsigned gen;
    std::wstring text;
    std::wstring fsDir;              // current folder as a path, empty when virtual
    PIDLIST_ABSOLUTE result = nullptr;
};

static void typed_apply(void* payload) {
    std::unique_ptr<TypedNav> job(static_cast<TypedNav*>(payload));
    if (job->gen != s_typedGen.load()) {        // superseded by a newer navigation
        if (job->result) CoTaskMemFree(job->result);
        return;
    }
    if (job->result) {
        navigation_open_abs(job->result);
        CoTaskMemFree(job->result);
        return;
    }
    // not a location: like Explorer, try running it ("cmd", "notepad", URLs)
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_main;
    sei.lpFile = job->text.c_str();
    sei.lpDirectory = job->fsDir.empty() ? nullptr : job->fsDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!job->text.empty() && ShellExecuteExW(&sei)) {
        SetWindowTextW(g_address, current_display_path().c_str());
        return;
    }
    std::wstring msg = L"Windows can't find '" + job->text + L"'. Check the spelling and try again.";
    MessageBoxW(g_main, msg.c_str(), L"Explorer Plus", MB_ICONERROR | MB_OK);
}

static void typed_worker(TypedNav* job) {
    ComInit ci;
    const std::wstring& text = job->text;
    // relative to the current folder first ("..", "subfolder\x"), then as typed
    if (!job->fsDir.empty() && PathIsRelativeW(text.c_str()) && text.find(L"::") != 0 &&
        text.find(L':') == std::wstring::npos) {
        WCHAR combined[MAX_PATH]{};
        if (PathCombineW(combined, job->fsDir.c_str(), text.c_str()) && PathFileExistsW(combined))
            parse_typed(combined, &job->result);
    }
    if (!job->result && FAILED(parse_typed(text, &job->result))) job->result = nullptr;
    post_async(typed_apply, job);
}

void navigate_path_text(const std::wstring& text) {
    auto* job = new TypedNav{ ++s_typedGen, text, {} };
    WCHAR fsDir[MAX_PATH]{};
    if (!pidl_is_desktop(g_current) &&
        SHGetPathFromIDListW(const_cast<LPITEMIDLIST>((LPCITEMIDLIST)g_current), fsDir))
        job->fsDir = fsDir;
    worker_submit([job] { typed_worker(job); });
}

// an argument for CommandLineToArgvW: quoted, with the backslashes before the closing
// quote doubled ("C:\" would otherwise arrive as C:")
static std::wstring quote_arg(const std::wstring& s) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; q += c; continue; }
        if (c == L'"') q.append(slashes + 1, L'\\');      // (not in paths; kept correct anyway)
        slashes = 0;
        q += c;
    }
    q.append(slashes, L'\\');
    return q + L"\"";
}

// another window on `folder` (a separate process, as each window keeps its own
// navigation state)
void navigation_open_new_window(PCIDLIST_ABSOLUTE folder) {
    WCHAR exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = pidl_is_desktop(folder) ? L"::desktop" : pidl_folder_path(folder);
    std::wstring cmd = quote_arg(exe) + L" --new-window " + quote_arg(path);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// history moves show each folder in its own remembered view (not the one just left)
void navigation_go_back() {
    if (!tabmodel_go_back()) return;
    TabState* t = tabmodel_active();
    t->view = view_for_folder(t->current);
    navigate_replace(t);
}
void navigation_go_forward() {
    if (!tabmodel_go_forward()) return;
    TabState* t = tabmodel_active();
    t->view = view_for_folder(t->current);
    navigate_replace(t);
}

// replace the current folder without a history entry (it was deleted, renamed, ...)
void navigate_in_place(PITEMID_CHILD pidl) {
    tabmodel_on_navigate(pidl, false);  // adopts pidl
    TabState* t = tabmodel_active();
    if (!t) return;
    t->view = view_for_folder(t->current);
    navigate_replace(t);
}
bool navigation_can_up() { return !pidl_is_desktop(g_current); }
void navigation_go_up() {
    if (!navigation_can_up()) return;
    if (PCIDLIST_ABSOLUTE scope = search_scope_of(g_current)) { navigate_to(scope); return; }   // results -> searched folder
    PITEMID_CHILD parent = pidl_clone(g_current);
    if (!parent) return;
    ILRemoveLastID(parent);       // a top-level item leaves the empty (Desktop) pidl
    navigate_owned(parent);
}
void navigation_refresh() { start_enum_current(true); }

// The current folder was deleted or moved away behind our back: go to the nearest
// folder above it that still exists, in place (Back must not lead into the void).
bool navigation_leave_if_gone() {
    if (search_is_results(g_current) || !fs_folder_missing(g_current)) return false;
    PITEMID_CHILD up = pidl_clone(g_current);
    while (up && !pidl_is_desktop(up) && fs_folder_missing(up)) ILRemoveLastID(up);
    if (!up) return false;
    navigate_in_place(up);
    return true;
}
