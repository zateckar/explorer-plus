#include "navigation.h"
#include "cmdline.h"
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
#include "perf.h"
#include "foldersizes.h"
#include "layout.h"
#include "statusbar.h"
#include "cmdbar.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include "workers.h"
#include "sync_ui.h"
#include "assoc.h"
#include "notepad.h"
#include "player.h"

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
    enumerate_start(g_pane->current, keepView);
    watcher_register_for(g_pane->current);
    // a folder inside a sync folder: its sync state (status bar, badges)
    sync_view_folder(g_pane->index, pidl_fs_path(g_pane->current).c_str());
}

// the active list's folder on everything around the lists: address bar, navigation
// pane, buttons, details pane, search box, title, status bar
static void show_active_folder(TabState* t) {
    SetWindowTextW(g_address, current_display_path().c_str());
    tree_sync_to_current_deferred();
    navbar_refresh();
    detailspane_update();
    search_on_navigated();
    std::wstring name = t ? t->nav().name : std::wstring();
    std::wstring title = name.empty() ? L"Explorer Plus" : (name + L" \x2013 Explorer Plus");
    SetWindowTextW(g_main, title.c_str());
    status_update();
    statusbar_refresh();
}

// Apply g_pane's side of a tab to its list: folder, view, listing (and the bars when it
// is the active list).
void navigate_replace(TabState* t) {
    if (!t) return;
    ++s_typedGen;
    PaneNav& n = t->nav();
    current_replace(pidl_clone(n.current));
    set_view_mode(n.view, false);
    start_enum_current(false);
    layout_pane_caption_changed();
    if (pane_is_active()) show_active_folder(t);
    tabstrip_refresh();
}

// a list the tab doesn't show: nothing listed, watched or walked there
static void pane_stop() {
    ++g_pane->enumId;
    enum_clear_items();
    deregister_current_watch();
    foldersizes_release_watch();
}

void navigation_show_tab(TabState* t, bool anotherTab) {
    if (!t) return;
    if (t->dual && !t->side[1].current) t->dual = false;    // (nothing to show there)
    g_dual = t->dual;
    if (g_dual) list_ensure(&g_panes[1]);
    if (anotherTab) tree_restore_state(t->tree);             // (before the tree syncs to the folder)
    HWND focus = GetFocus();
    bool listFocused = !focus || is_pane_list(focus) || is_pane_list(GetParent(focus));
    Pane* active = &g_panes[g_dual ? t->activeSide : 0];
    pane_set_active(active);
    for (Pane& p : g_panes) {
        if (!p.list) continue;
        PaneScope ps(&p);
        if (pane_shown(&p)) navigate_replace(t);
        else pane_stop();
    }
    layout_relayout();                                       // (shows / hides the second list)
    if (listFocused && GetFocus() != active->list && IsWindowVisible(g_main)) SetFocus(active->list);
    cmdbar_refresh();
}

// the focus went to the other list: commands, the bars and the navigation pane follow it
void navigation_activate_pane(Pane* p) {
    if (!p || p == active_pane() || !pane_shown(p)) return;
    pane_set_active(p);
    TabState* t = tabmodel_active();
    if (t) t->activeSide = p->index;
    PaneScope ps(p);
    show_active_folder(t);
    layout_pane_caption_changed();
    tabstrip_refresh();
    cmdbar_refresh();
    tabs_persist_soon();
}

// Ctrl+Shift+D: a second list beside the first (on the folder it showed last in this tab,
// else the first list's), or back to one
void navigation_toggle_dual() {
    TabState* t = tabmodel_active();
    if (!t) return;
    if (!t->dual) list_ensure(&g_panes[1]);
    if (!g_panes[1].list) return;
    if (!t->dual) {
        PaneNav& n = t->side[1];
        if (!n.current) {
            n.current = pidl_clone(t->side[0].current);
            n.name = t->side[0].name;
            n.view = t->side[0].view;
        }
        t->dual = g_dual = true;
        {
            PaneScope ps(&g_panes[1]);
            navigate_replace(t);
        }
        layout_relayout();
        SetFocus(g_panes[1].list);
        navigation_activate_pane(&g_panes[1]);
    } else {
        navigation_activate_pane(&g_panes[0]);
        t->dual = g_dual = false;
        t->activeSide = 0;
        {
            PaneScope ps(&g_panes[1]);
            pane_stop();
        }
        layout_relayout();
        SetFocus(g_panes[0].list);
    }
    tabstrip_refresh();
    cmdbar_refresh();
    tabs_persist_soon();
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
    settings_set_default_view(g_pane->view);
    settings_forget_fs_folders();
}

// "Reset Folders": every file-system folder back to Details with the standard columns
void views_reset_all() {
    settings_forget_fs_folders();
    columns_forget_default();
    settings_set_default_view(View{ LV_VIEW_DETAILS, 0, GB_AUTO });
    if (TabState* t = tabmodel_active()) {
        for (auto& n : t->side)
            if (n.current) n.view = view_for_folder(n.current);
        navigation_show_tab(t, false);  // lists again with the folders' (default) columns
    }
}

// ordinary navigation within the active tab (pushes history)
void navigate_owned(PIDLIST_ABSOLUTE pidl) {
    if (!pidl) pidl = pidl_desktop();
    TabState* t = tabmodel_active();
    if (t && pidl_equal(t->nav().current, pidl)) {  // same folder: just refresh
        CoTaskMemFree(pidl);
        navigation_refresh();
        return;
    }
    if (t && t->pinned && g_pane->index == 0) {     // a pinned tab keeps its (first list's) folder
        tabmodel_open_in_new_tab(pidl);             // adopts pidl
        address_history_add(current_display_path());
        return;
    }
    tabmodel_on_navigate(pidl, true);   // adopts pidl
    if (t) t->nav().view = view_for_folder(t->nav().current);
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
                    navigate_owned(target);
                    return;
                }
                CoTaskMemFree(target);
            }
        }
    }
    navigation_open_files({ abs });
}

void navigation_open_files(const std::vector<PCIDLIST_ABSOLUTE>& files) {
    std::vector<std::wstring> notes;
    std::vector<MediaItem> media;
    for (PCIDLIST_ABSOLUTE f : files) {
        PWSTR path = nullptr;
        OwnApp app = OwnApp::None;
        if (SUCCEEDED(SHGetNameFromIDList(f, SIGDN_FILESYSPATH, &path)) && path) {
            app = assoc_own_app(path);
            if (app == OwnApp::Notepad) notes.push_back(path);
            else if (app == OwnApp::Player) media.push_back({ path, false });
            CoTaskMemFree(path);
        }
        if (app == OwnApp::None) shell_open_abs(g_main, f);
    }
    if (perf_on() && (!notes.empty() || !media.empty())) perf_event("open.own", -1, (long long)(notes.size() + media.size()));
    if (!notes.empty()) notepad_open(notes);
    if (!media.empty()) player_open(media, false);
}

// Typed-path resolution runs on a worker: SHParseDisplayName / PathFileExists on
// an unreachable "\\server\share" can block for tens of seconds. Any navigation
// that happens meanwhile supersedes the pending one (generation counter); the focus
// gone to the other list meanwhile, the one it was typed for goes there.

struct TypedNav {
    unsigned gen;
    std::wstring text;
    std::wstring fsDir;              // current folder as a path, empty when virtual
    Pane* pane = nullptr;            // the list it was typed for (in the active tab: another tab shown supersedes it)
    PIDLIST_ABSOLUTE result = nullptr;
};

// not a location: like Explorer, try running it ("cmd", "notepad", URLs). From a message
// of its own (post_later): the error box's loop would run the rest of the async batch.
static void typed_run(void* payload) {
    std::unique_ptr<TypedNav> job(static_cast<TypedNav*>(payload));
    if (job->gen != s_typedGen.load()) return;
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

static void typed_apply(void* payload) {
    std::unique_ptr<TypedNav> job(static_cast<TypedNav*>(payload));
    if (job->gen != s_typedGen.load() || !pane_shown(job->pane)) {     // superseded by a newer navigation; its list closed
        if (job->result) CoTaskMemFree(job->result);
        return;
    }
    if (job->result) {
        PaneScope ps(job->pane);
        navigation_open_abs(job->result);
        CoTaskMemFree(job->result);
        return;
    }
    post_later(typed_run, job.release());
}

static void typed_worker(TypedNav* job) {
    ComInit ci;
    const std::wstring& text = job->text;
    // relative to the current folder first ("..", "subfolder\x"), then as typed
    if (!job->fsDir.empty() && PathIsRelativeW(text.c_str()) && text.find(L"::") != 0 &&
        text.find(L':') == std::wstring::npos) {
        // (PathCombine stops at MAX_PATH: a long folder's relative path is joined by hand)
        std::wstring combined = job->fsDir;
        if (!combined.empty() && combined.back() != L'\\') combined += L'\\';
        combined += text;
        std::wstring full(32768, L'\0');
        DWORD n = GetFullPathNameW(combined.c_str(), (DWORD)full.size(), full.data(), nullptr);
        if (n && n < full.size()) {
            full.resize(n);
            if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) parse_typed(full, &job->result);
        }
    }
    if (!job->result && FAILED(parse_typed(text, &job->result))) job->result = nullptr;
    post_async(typed_apply, job);
}

void navigate_path_text(const std::wstring& text) {
    auto* job = new TypedNav{ ++s_typedGen, text, {}, g_pane };
    if (!pidl_is_desktop(g_pane->current)) job->fsDir = pidl_fs_path(g_pane->current);
    worker_submit([job] { typed_worker(job); });
}

// another window on `folder` (a separate process, as each window keeps its own
// navigation state)
void navigation_open_new_window(PCIDLIST_ABSOLUTE folder) {
    std::wstring exe = module_file();
    if (exe.empty()) return;
    std::wstring path = pidl_is_desktop(folder) ? L"::desktop" : pidl_folder_path(folder);
    std::wstring cmd = quote_command_arg(exe) + L" --new-window " + quote_command_arg(path);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// history moves show each folder in its own remembered view (not the one just left)
void navigation_go_back() {
    if (!tabmodel_go_back()) return;
    TabState* t = tabmodel_active();
    t->nav().view = view_for_folder(t->nav().current);
    navigate_replace(t);
}
void navigation_go_forward() {
    if (!tabmodel_go_forward()) return;
    TabState* t = tabmodel_active();
    t->nav().view = view_for_folder(t->nav().current);
    navigate_replace(t);
}

// replace the current folder without a history entry (it was deleted, renamed, ...)
void navigate_in_place(PIDLIST_ABSOLUTE pidl) {
    tabmodel_on_navigate(pidl, false);  // adopts pidl
    TabState* t = tabmodel_active();
    if (!t) return;
    t->nav().view = view_for_folder(t->nav().current);
    navigate_replace(t);
}
bool navigation_can_up() { return !pidl_is_desktop(g_pane->current); }
void navigation_go_up() {
    if (!navigation_can_up()) return;
    if (PCIDLIST_ABSOLUTE scope = search_scope_of(g_pane->current)) { navigate_to(scope); return; }   // results -> searched folder
    PIDLIST_ABSOLUTE parent = pidl_clone(g_pane->current);
    if (!parent) return;
    ILRemoveLastID(parent);       // a top-level item leaves the empty (Desktop) pidl
    navigate_owned(parent);
}
void navigation_refresh() { start_enum_current(true); }

// The current folder was deleted or moved away behind our back: go to the nearest
// folder above it that still exists, in place (Back must not lead into the void).
bool navigation_leave_if_gone() {
    if (search_is_results(g_pane->current) || !fs_folder_missing(g_pane->current)) return false;
    PIDLIST_ABSOLUTE up = pidl_clone(g_pane->current);
    while (up && !pidl_is_desktop(up) && fs_folder_missing(up)) ILRemoveLastID(up);
    if (!up) return false;
    if (perf_on()) perf_event("nav.leave_gone", -1, -1, pidl_folder_path(up).c_str());
    navigate_in_place(up);
    return true;
}
