"""Smoke test for Explorer Plus. Usage: python -I tools/smoke_test.py <exe> <workdir>

Builds a scratch folder under <workdir>, drives the app through window messages and
checks results on disk / in the UI. Screenshots land in <workdir>/shots.
"""
import os, sys, time, shutil, struct, subprocess, atexit, threading, ctypes, ctypes.wintypes as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winauto
from winauto import App, menu_items, u, children, cls, text, clipboard_text, send, WM_KEYDOWN, WM_SETTEXT

# command IDs from the app's own resource.h (a renumbered command can't quietly become
# another one here)
R = winauto.RES
GB_NONE, GB_TYPE = 1, 4                     # appstate.h: enum GroupBy
IDM = dict(NEWFOLDER=R['IDM_FILE_NEWFOLDER'], NEWTAB=R['IDM_FILE_NEWTAB'], CLOSETAB=R['IDM_FILE_CLOSETAB'],
           COPY=R['IDM_EDIT_COPY'], PASTE=R['IDM_EDIT_PASTE'], SELALL=R['IDM_EDIT_SELALL'],
           SELNONE=R['IDM_EDIT_SELNONE'], DELETE=R['IDM_EDIT_DELETE'], RENAME=R['IDM_EDIT_RENAME'],
           DETAILS=R['IDM_VIEW_DETAILS'], TILES=R['IDM_VIEW_TILES'], LARGE=R['IDM_VIEW_LARGE'],
           MEDIUM=R['IDM_VIEW_MEDIUM'], LIST=R['IDM_VIEW_LIST'], HIDDEN=R['IDM_VIEW_HIDDEN'],
           BACK=R['IDM_NAV_BACK'], FWD=R['IDM_NAV_FORWARD'], UP=R['IDM_NAV_UP'], REFRESH=R['IDM_NAV_REFRESH'],
           INVERT=R['IDM_EDIT_INVERT'], NEW_MENU=R['IDM_NEW_MENU'], COPY_PATH=R['IDM_COPY_PATH'],
           NEW_WINDOW=R['IDM_NEW_WINDOW'], FOCUS_ADDRESS=R['IDM_FOCUS_ADDRESS'],
           GROUP_NONE=R['IDM_GROUP_BASE'] + GB_NONE, GROUP_TYPE=R['IDM_GROUP_BASE'] + GB_TYPE,
           XLARGE=R['IDM_VIEW_XLARGE'], CHECKBOXES=R['IDM_VIEW_CHECKBOXES'], EXTENSIONS=R['IDM_VIEW_EXTENSIONS'],
           PREVIEW=R['IDM_VIEW_PREVIEWPANE'], COMPACT=R['IDM_VIEW_COMPACT'], TOOLS_MENU=R['IDM_TOOLS_MENU'],
           CMDBAR=R['IDM_VIEW_CMDBAR'], FOLDERSIZES=R['IDM_VIEW_FOLDERSIZES'], SORT_COL=R['IDM_SORT_COL_BASE'],
           VIEW_MENU=R['IDM_VIEW_MENU'], MORE_MENU=R['IDM_MORE_MENU'], APPLY_ALL=R['IDM_VIEW_APPLY_ALL'],
           RESET_ALL=R['IDM_VIEW_RESET_ALL'], TEXT_LARGER=R['IDM_TEXT_LARGER'], TEXT_SMALLER=R['IDM_TEXT_SMALLER'],
           TEXT_RESET=R['IDM_TEXT_RESET'], TREE_DELETE=R['IDM_TREE_DELETE'])
LV_VIEW_DETAILS, LV_VIEW_LIST, LV_VIEW_TILE = 1, 3, 4
LVM_GETVIEW, TVM_GETITEMHEIGHT = 0x108F, 0x111C

exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
# the test deletes <workdir>\testdata and recycles files in it: never a drive root, a
# profile or system folder, or a folder that already holds something else
home = os.path.abspath(os.path.expanduser('~'))
protected = {os.path.normcase(p) for p in (home, os.path.dirname(home), os.environ.get('SystemRoot', r'C:\Windows'),
                                           os.environ.get('ProgramFiles', r'C:\Program Files'))}
if (os.path.dirname(work) == work or os.path.normcase(work) in protected
        or (os.path.isdir(work) and [n for n in os.listdir(work)
                                     if n not in ('testdata', 'shots') and not n.startswith('settings.ini')])):
    sys.exit('refusing to use %s as the scratch folder: give a new or empty folder' % work)
data = os.path.join(work, 'testdata')
shots = os.path.join(work, 'shots')
shutil.rmtree(data, ignore_errors=True)
os.makedirs(os.path.join(data, 'sub'))
os.makedirs(os.path.join(data, '.dotdir'))        # like .git: once showed a file icon in the tree
os.makedirs(shots, exist_ok=True)
# the app's event log (navigation, watcher, tree), kept with the screenshots: what a
# failure on a CI runner went through
events = os.path.join(shots, 'events.log')
if os.path.exists(events):
    os.remove(events)
os.environ['EXPLORERPLUS_PERF'] = events
for n in ('a.txt', 'b.txt'):
    with open(os.path.join(data, n), 'w') as f:
        f.write('hello ' + n)
wall = r'C:\Windows\Web\Wallpaper\Windows\img0.jpg'
if os.path.exists(wall):
    shutil.copy(wall, os.path.join(data, 'photo.jpg'))
else:   # Server / trimmed images: the checks only need a big .jpg without a preview handler
    with open(os.path.join(data, 'photo.jpg'), 'wb') as f:
        f.write(b'\xff\xd8\xff\xe0' + bytes(256 * 1024))

# the test copies paths to the clipboard: put back the text that was there (other
# formats can't be restored) and never leave an app instance running, whatever happens
saved_clip = clipboard_text()
def cleanup():
    for a in winauto.started:
        if a.proc.poll() is None:
            a.proc.kill()
    winauto.set_clipboard_text(saved_clip)
atexit.register(cleanup)

results = []
def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print(('PASS ' if ok else 'FAIL ') + name + (('  [' + str(detail) + ']') if detail else ''))

# a check that couldn't run here (the desktop got in the way): a note locally, a failure
# on CI (GitHub sets CI=true), where a check that never runs would pass unnoticed
def skip(name, why):
    if os.environ.get('CI'):
        check(name, False, 'skipped: ' + why)
    else:
        print('SKIP ' + name + '  [' + why + ']')

# the whole run has a deadline: a hung app (or a wait that never ends) fails the run with
# a message, before the CI step's own timeout kills it without one
deadline = int(os.environ.get('SMOKE_DEADLINE', '480'))
def watchdog():
    print('FAIL watchdog: the smoke test still ran after %d s; stopping it' % deadline, flush=True)
    try:
        cleanup()
    finally:
        os._exit(3)
timer = threading.Timer(deadline, watchdog)
timer.daemon = True
timer.start()

def wait_for(pred, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.1)
    return pred()

ini = os.path.join(work, 'settings.ini')
if os.path.exists(ini):
    os.remove(ini)
# promote a classic menu item every machine has into the compact menu (UTF-16 INI);
# file name extensions on whatever Windows does (fresh images hide them): the checks
# find items by their full file name
with open(ini, 'w', encoding='utf-16') as f:
    f.write('[contextmenu]\npromote1=Send to|%SystemRoot%\\System32\\shell32.dll,4\n'
            '[prefs]\nshowExtensions=1\n')
uni = os.path.join(data, 'Stažené = [x]; 50%')     # non-ASCII + INI-hostile characters
os.makedirs(uni)

app = App(exe, data, settings=ini)          # opens the scratch folder as the active tab
app.wait_idle(2)
app.screenshot(os.path.join(shots, '00_start.png'))
check('command-line folder opens', 'testdata' in app.title(), app.title())
if 'testdata' not in app.title():
    app.close()
    sys.exit('aborting: not in the scratch folder, refusing to run file operations elsewhere')
app.go(os.path.join(data, 'sub'))
check('navigate via address bar', app.title().startswith('sub'), app.title())
app.go('..')
check('relative ".." in address bar', 'testdata' in app.title(), app.title())
expected = len(os.listdir(data))
check('lists folder', wait_for(lambda: app.item_count() == expected), (app.item_count(), expected))
app.screenshot(os.path.join(shots, '01_details.png'))

# the window (and so the taskbar button) carries the exe's own icon, not a folder
def class_icon():
    u.GetClassLongPtrW.restype = ctypes.c_void_p
    u.GetClassLongPtrW.argtypes = [W.HWND, ctypes.c_int]
    return u.GetClassLongPtrW(app.hwnd, -14)                         # GCLP_HICON
def icon_size(hicon):
    # the window's icon has the DPI's size (LoadIconMetric): compare at that size, so
    # neither image is scaled
    info = ctypes.create_string_buffer(32)                           # ICONINFO
    if not hicon or not u.GetIconInfo(ctypes.c_void_p(hicon), info):
        return 32
    mask, color = struct.unpack_from('<QQ', info, 16)
    bm = ctypes.create_string_buffer(32)                             # BITMAP
    ctypes.windll.gdi32.GetObjectW(ctypes.c_void_p(color or mask), 32, bm)
    for h in (color, mask):
        if h: ctypes.windll.gdi32.DeleteObject(ctypes.c_void_p(h))
    return struct.unpack_from('<i', bm, 4)[0] or 32
def exe_icon(size):
    k = ctypes.windll.kernel32
    k.LoadLibraryExW.restype = ctypes.c_void_p
    k.LoadLibraryExW.argtypes = [W.LPCWSTR, W.HANDLE, W.DWORD]
    u.LoadImageW.restype = ctypes.c_void_p
    u.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, W.UINT, ctypes.c_int, ctypes.c_int, W.UINT]
    mod = k.LoadLibraryExW(exe, None, 0x20 | 0x2)                    # AS_IMAGE_RESOURCE | AS_DATAFILE
    return u.LoadImageW(mod, 1, 1, size, size, 0) if mod else None   # IDI_APP
def folder_icon():
    info = ctypes.create_string_buffer(544)                          # SHSTOCKICONINFO
    struct.pack_into('<I', info, 0, 544)
    ctypes.windll.shell32.SHGetStockIconInfo(3, 0x100, info)         # SIID_FOLDER, SHGSI_ICON
    return struct.unpack_from('<Q', info, 8)[0]
mine = class_icon()
isz = icon_size(mine)
own, folder = exe_icon(isz), folder_icon()
if mine and own and folder:
    px = winauto.icon_pixels(mine, isz)
    d_own = winauto.pixel_distance(px, winauto.icon_pixels(own, isz))
    d_folder = winauto.pixel_distance(px, winauto.icon_pixels(folder, isz))
    check('window icon is the exe icon (taskbar)', d_own < 8 and d_own < d_folder, (round(d_own, 1), round(d_folder, 1)))
else:
    check('window icon is the exe icon (taskbar)', False, (mine, own, folder))

# navigation pane: every folder node has a folder icon (index 0 is the system list's
# generic file icon; startup races once left .git-like folders with it)
wait_for(lambda: any(app.tree_item(h)[0] == '.dotdir' for h in app.tree_nodes()), 5)
nodes = [app.tree_item(h) for h in app.tree_nodes()]
check('tree shows the scratch folder\'s subfolders', any(t == '.dotdir' for t, _ in nodes), [t for t, _ in nodes])
bad = [(t, i) for t, i in nodes if t and i <= 0]
check('no tree node has the generic file icon', not bad, bad)

# List view rows keep the Details row spacing (were packed edge to edge)
def row_h(i=0):
    r = app.item_rect(i, 0)
    return r[3] - r[1]
details_h = row_h()
app.command(IDM['LIST']); time.sleep(0.8)
list_h = row_h()
check('List view rows are spaced like Details', list_h >= details_h - 1 and list_h > 20, (list_h, details_h))
app.screenshot(os.path.join(shots, '01_list.png'))
app.command(IDM['DETAILS']); time.sleep(0.5)

# a double-click on empty space below the items goes up a level
rc = W.RECT()
u.GetClientRect(app.list, ctypes.byref(rc))
pt = ((rc.bottom - 8) << 16) | 60
last = app.item_rect(app.item_count() - 1, 0)
if last[3] < rc.bottom - 8:
    u.PostMessageW(app.list, 0x0203, 1, pt); u.PostMessageW(app.list, 0x0202, 0, pt)   # DBLCLK, UP
    check('double-click on empty list space goes up',
          wait_for(lambda: app.title().startswith(os.path.basename(work)), 3), app.title())
else:
    check('double-click on empty list space goes up', False, 'no empty space below the items')
app.go(data)
r = app.item_rect(app.select_name('sub'), 0)
pt = (((r[1] + r[3]) // 2) << 16) | (r[0] + 30)
u.PostMessageW(app.list, 0x0203, 1, pt); u.PostMessageW(app.list, 0x0202, 0, pt)
check('double-click on an item still opens it', wait_for(lambda: app.title().startswith('sub'), 3), app.title())
app.go(data)

app.command(IDM['MEDIUM'])
time.sleep(2)
app.screenshot(os.path.join(shots, '02_medium.png'))
app.command(IDM['TILES'])
time.sleep(1)
app.screenshot(os.path.join(shots, '03_tiles.png'))
# a tile's lines under the name are its Type / Size sub-items (sub-item 0 is the name
# again: it showed the title twice until the tile was hovered)
tile_row = next((i for i in range(app.item_count()) if app.item_text(i) == 'a.txt'), 0)
tcols = app.tile_columns(tile_row)
check('tiles show type / size under the name, not the name again', tcols and 0 not in tcols, tcols)
app.command(IDM['DETAILS'])

# select all / invert: must not deadlock
app.command(IDM['SELALL'])
check('select all', wait_for(lambda: app.selected_count() == expected), app.selected_count())
app.command(IDM['INVERT'])
check('invert selection (no deadlock)', wait_for(lambda: app.selected_count() == 0), app.selected_count())

# copy + paste in the same folder -> "a - Copy.txt"
app.select_name('a.txt')
check('select by name', app.selected_count() == 1, app.selected_count())
app.command(IDM['COPY'])
app.command(IDM['PASTE'])
check('copy/paste creates "a - Copy.txt"',
      wait_for(lambda: os.path.exists(os.path.join(data, 'a - Copy.txt')), 8), os.listdir(data))

# Win11 compact context menu (flyout) on the selection
app.select_name('b.txt')
fly = app.open_context_flyout()
entries = app.flyout_entries(fly) if fly else []
check('compact context menu opens', bool(fly) and entries[-1:] == ['Show more options'], entries)
check('compact menu lists Open / Properties', 'Open' in entries and 'Properties' in entries, entries)
check('a promoted classic item is listed', 'Send to' in entries, entries)
if fly:
    app.screenshot(os.path.join(shots, '04_flyout.png'))
    u.PostMessageW(fly, WM_KEYDOWN, 0x1B, 0)      # Esc
check('Esc closes the compact menu', wait_for(lambda: app.flyout() is None, 3))
# access key: "Copy &as path" puts the quoted path on the clipboard
app.select_name('a.txt')
fly = app.open_context_flyout()
if fly:
    u.PostMessageW(fly, 0x0102, ord('a'), 0)       # WM_CHAR
check('compact menu access key runs the verb',
      wait_for(lambda: clipboard_text() == '"%s"' % os.path.join(data, 'a.txt'), 5), clipboard_text())
app.close_menus()
# a real click on another file while the compact menu is open closes the menu and
# selects that file: one click, not one to close the menu and another to select
u.WindowFromPoint.argtypes = [W.POINT]
u.WindowFromPoint.restype = W.HWND
ia = app.select_name('a.txt')
app.select_name('b.txt')
l, t, r, b = app.item_rect(ia, 2)                                    # LVIR_LABEL
pt = W.POINT((l + r) // 2, (t + b) // 2)
u.ClientToScreen(app.list, ctypes.byref(pt))
if (u.WindowFromPoint(pt) or 0) == app.list:
    cursor = W.POINT()
    u.GetCursorPos(ctypes.byref(cursor))                            # (put back: menus opened by
    fly = app.open_context_flyout()                                 # keyboard later must not be hovered)
    if fly:
        u.SetCursorPos(pt.x, pt.y)
        time.sleep(0.2)
        u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.05)       # MOUSEEVENTF_LEFTDOWN
        u.mouse_event(0x0004, 0, 0, 0, 0)                          # MOUSEEVENTF_LEFTUP
    def picked():
        return send(app.list, 0x100C, -1, 0x0002)         # LVM_GETNEXTITEM, LVNI_SELECTED
    check('one click beside the compact menu closes it and selects the file there',
          fly and wait_for(lambda: app.flyout() is None and app.selected_count() == 1 and picked() == ia, 3),
          (bool(fly), app.flyout(), app.selected_count(), picked(), ia))
    app.close_menus()
    # the pointer glides off the selected row and back: the row keeps its text (a hover
    # once had the control custom-draw the row off-screen, and our highlight then
    # covered the row's text on screen until the next repaint)
    def glide(x0, y0, x1, y1):
        for k in range(1, 26):
            u.SetCursorPos(x0 + (x1 - x0) * k // 25, y0 + (y1 - y0) * k // 25); time.sleep(0.012)
    if picked() == ia:
        kept = []
        for dx, dy in ((150, 90), (300, 0), (40, 160)):
            glide(pt.x, pt.y, pt.x + dx, pt.y + dy); time.sleep(0.3)
            glide(pt.x + dx, pt.y + dy, pt.x, pt.y); time.sleep(0.8)
            kept.append(app.row_has_text(ia))
        check('a selected row keeps its text when the pointer comes back over it', all(kept), kept)
    u.SetCursorPos(cursor.x, cursor.y)
else:
    skip('one click beside the compact menu closes it and selects the file there',
         'the list is covered at %s' % ((pt.x, pt.y),))
# background flyout: View / Sort by / Group by / New, with our own submenus
app.command(IDM['SELNONE'])
fly = app.open_context_flyout()
entries = app.flyout_entries(fly) if fly else []
check('background compact menu', all(s in entries for s in ('View', 'Sort by', 'Group by', 'New', 'Show more options')),
      entries)
app.close_menus()
# its View submenu: Down to "View", Right opens it as a classic popup; choosing List
# applies and closes the flyout
fly = app.open_context_flyout()
sub_items = []
if fly:
    # promoted items (Open in Terminal, ...) may come first; separators aren't stops
    stops = [s for s in app.flyout_entries(fly) if s != '-']
    for _ in range(stops.index('View') + 1 if 'View' in stops else 1):
        u.PostMessageW(fly, WM_KEYDOWN, 0x28, 0)  # Down
    u.PostMessageW(fly, WM_KEYDOWN, 0x27, 0)      # Right
    menu = None
    for _ in range(12):
        time.sleep(0.25)
        menu = next((h for h in winauto.top_windows(app.proc.pid) if cls(h) == '#32768' and u.IsWindowVisible(h)), None)
        if menu: break
    sub_items = menu_items(send(menu, 0x01E1, 0, 0)) if menu else []
    pick = next((i for i, s in enumerate(sub_items) if s.replace('&', '').startswith('List')), -1)
    if pick >= 0:
        send(menu, 0x01E5, pick, 0)      # MN_SELECTITEM
        u.PostMessageW(menu, WM_KEYDOWN, 0x0D, 0)
check('compact menu View submenu chooses a view',
      wait_for(lambda: app.flyout() is None and send(app.list, 0x108F, 0, 0) == 3, 4), sub_items)
app.close_menus()
app.command(IDM['DETAILS'])
check('no menu left open', app.flyout() is None)

# the command bar row is folded into the nav row's "..." by default
def cmdbar_visible():
    return any(cls(c) == 'ExplorerPlusCmdBar' and u.IsWindowVisible(c) for c in children(app.hwnd))
check('command bar hidden by default', not cmdbar_visible())
u.PostMessageW(app.hwnd, 0x0111, IDM['TOOLS_MENU'], 0)
fly = None
for _ in range(20):
    time.sleep(0.25)
    fly = app.flyout()
    if fly: break
entries = app.flyout_entries(fly) if fly else []
check('"..." lists the command bar commands', all(s in entries for s in ('New', 'Sort', 'View', 'Show command bar')), entries)
if fly:
    u.PostMessageW(fly, 0x0102, ord('b'), 0)       # "Show command &bar"
check('"Show command bar" brings the row back', wait_for(cmdbar_visible, 3))
def open_popup(cmd):
    """Post a command that shows a classic popup menu: (menu window, item texts)."""
    u.PostMessageW(app.hwnd, 0x0111, cmd, 0)
    for _ in range(20):
        time.sleep(0.25)
        menu = next((h for h in winauto.top_windows(app.proc.pid) if cls(h) == '#32768' and u.IsWindowVisible(h)), None)
        if menu:
            return menu, menu_items(send(menu, 0x01E1, 0, 0))
    return None, []
def choose(menu, items, label):
    idx = next((i for i, s in enumerate(items) if s.replace('&', '').split('\t')[0] == label), -1)
    if menu and idx >= 0:
        send(menu, 0x01E5, idx, 0)                       # MN_SELECTITEM
        u.PostMessageW(menu, WM_KEYDOWN, 0x0D, 0)
    else:
        app.close_menus()
    return idx >= 0
menu, items = open_popup(IDM['VIEW_MENU'])
clean = [s.replace('&', '').split('\t')[0] for s in items]
check('View menu offers Text size and Apply / Reset all folders',
      all(s in clean for s in ('Text size', 'Apply this view to all folders', 'Reset all folders')), clean)
app.close_menus()
menu, items = open_popup(IDM['MORE_MENU'])
check('the bar\'s "..." menu has "Hide command bar"', choose(menu, items, 'Hide command bar'), items)
check('"Hide command bar" hides the row', wait_for(lambda: not cmdbar_visible(), 3))
app.command(IDM['CMDBAR'])
check('View > Command bar toggles it back', wait_for(cmdbar_visible, 3))
app.command(IDM['CMDBAR'])
check('command bar hidden again', wait_for(lambda: not cmdbar_visible(), 3))

# classic context menu through "Show more options"
app.select_name('b.txt')
mw, hm = app.open_keyboard_context_menu()
items = menu_items(hm) if hm else []
check('item context menu opens', bool(items), items)
if mw:
    app.screenshot(os.path.join(shots, '04_ctxmenu.png'))
send_to = [i for i, s in enumerate(items) if s.startswith('Send to')]
check('menu has Send to / Properties', send_to and any('Properties' in s for s in items))
app.close_menus()

# new folder: created, selected and put into rename mode
app.command(IDM['NEWFOLDER'])
check('new folder created', wait_for(lambda: os.path.isdir(os.path.join(data, 'New folder')), 5))
def label_edit():
    for c in children(app.list):
        if cls(c) == 'Edit' and u.IsWindowVisible(c):
            return c
    return None
ed = None
wait_for(lambda: label_edit() is not None, 4)
ed = label_edit()
check('new folder enters rename mode', ed is not None)
if ed:
    buf = ctypes.create_unicode_buffer('renamed dir')
    send(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    send(ed, WM_KEYDOWN, 0x0D, 0)
    check('rename via label edit', wait_for(lambda: os.path.isdir(os.path.join(data, 'renamed dir')), 6),
          os.listdir(data))

# F2 rename of a file
app.select_name('b.txt')
app.command(IDM['RENAME'])
wait_for(lambda: label_edit() is not None, 3)
ed = label_edit()
check('F2 opens rename box', ed is not None)
if ed:
    buf = ctypes.create_unicode_buffer('c.txt')
    send(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    send(ed, WM_KEYDOWN, 0x0D, 0)
    check('file renamed b.txt -> c.txt',
          wait_for(lambda: os.path.isfile(os.path.join(data, 'c.txt')) and
                   not os.path.exists(os.path.join(data, 'b.txt')), 6), os.listdir(data))

# Copy path: quoted full path of the selection as clipboard text
time.sleep(1)
app.select_name('a.txt')
app.command(IDM['COPY_PATH'])
check('copy path puts the quoted path on the clipboard',
      clipboard_text() == '"%s"' % os.path.join(data, 'a.txt'), clipboard_text())

# command bar "New" menu: Windows' ShellNew entries; the new item enters rename mode
u.PostMessageW(app.hwnd, 0x0111, IDM['NEW_MENU'], 0)
menu = None
for _ in range(20):
    time.sleep(0.25)
    menu = next((h for h in winauto.top_windows(app.proc.pid) if cls(h) == '#32768' and u.IsWindowVisible(h)), None)
    if menu:
        break
new_items = menu_items(send(menu, 0x01E1, 0, 0)) if menu else []
# the ShellNew types depend on the machine's registrations; Folder is always there
check('New menu lists the shell New items', 'Folder' in new_items, new_items)
pick = next((s for s in new_items if 'Text Document' in s), 'Folder')
idx = new_items.index(pick) if pick in new_items else -1
if menu and idx >= 0:
    before = set(os.listdir(data))
    real = [i for i, s in enumerate(new_items) if s]           # arrow keys skip separators
    for _ in range(real.index(idx) + 1):
        u.PostMessageW(menu, WM_KEYDOWN, 0x28, 0)                # VK_DOWN
        time.sleep(0.05)
    u.PostMessageW(menu, WM_KEYDOWN, 0x0D, 0)
    created = lambda: sorted(set(os.listdir(data)) - before)
    check('New > %s creates the item' % pick, wait_for(lambda: bool(created()), 6), created())
    check('new item enters rename mode', wait_for(lambda: label_edit() is not None, 4))
    ed = label_edit()
    if ed:
        send(ed, WM_KEYDOWN, 0x1B, 0)                 # Esc: keep the name
    time.sleep(0.5)
    for n in created():
        pth = os.path.join(data, n)
        (os.rmdir if os.path.isdir(pth) else os.remove)(pth)
else:
    app.close_menus()
# address bar: Ctrl+L switches the breadcrumb to an edit box with the path
app.command(IDM['FOCUS_ADDRESS'])
check('Ctrl+L shows the address edit with the path',
      wait_for(lambda: u.IsWindowVisible(app.addr_edit) and app.address_text() == data, 2), app.address_text())
send(app.addr_edit, WM_KEYDOWN, 0x1B, 0)
check('Esc returns to the breadcrumb', wait_for(lambda: not u.IsWindowVisible(app.addr_edit), 2))

# F6 / Tab move the keyboard focus around the panes in screen order: tab strip, navigation
# buttons, address bar (editing), search box, command bar (when shown), navigation pane,
# file list
class GUITHREADINFO(ctypes.Structure):
    _fields_ = [('cbSize', W.DWORD), ('flags', W.DWORD), ('hwndActive', W.HWND), ('hwndFocus', W.HWND),
                ('hwndCapture', W.HWND), ('hwndMenuOwner', W.HWND), ('hwndMoveSize', W.HWND),
                ('hwndCaret', W.HWND), ('rcCaret', W.RECT)]
def focused():
    gi = GUITHREADINFO(cbSize=ctypes.sizeof(GUITHREADINFO))
    tid = u.GetWindowThreadProcessId(app.hwnd, None)
    return gi.hwndFocus if u.GetGUIThreadInfo(tid, ctypes.byref(gi)) else None
search = winauto.child_by_id(app.hwnd, App.IDC_SEARCH)
if focused() and focused() == app.list:
    seen = []
    u.FindWindowExW.restype = W.HWND
    strip = u.FindWindowExW(app.hwnd, None, 'ExplorerPlusTabStrip', None)
    navbtns = u.FindWindowExW(app.hwnd, None, 'ExplorerPlusNavBar', None)
    cmdbar = u.FindWindowExW(app.hwnd, None, 'ExplorerPlusCmdBar', None)
    order = [(0x75, strip), (0x75, navbtns), (0x75, app.addr_edit), (0x75, search)]   # F6 x4
    if cmdbar and u.IsWindowVisible(cmdbar):
        order.append((0x09, cmdbar))
    order += [(0x09, app.tree), (0x09, app.list)]                                      # Tab
    for key, want in order:
        u.PostMessageW(focused(), WM_KEYDOWN, key, 0)
        wait_for(lambda: focused() == want, 2)
        seen.append(cls(focused()) if focused() else None)
        if focused() != want:
            break
    check('F6 / Tab cycle the focus: tabs, buttons, address, search, commands, navigation pane, list',
          len(seen) == len(order) and focused() == app.list, seen)
    check('... and leaving the address bar ends its edit', not u.IsWindowVisible(app.addr_edit))
else:
    skip('F6 / Tab cycle the focus: tabs, buttons, address, search, commands, navigation pane, list',
         'the list does not have the focus (%s)' % focused())

# Ctrl+N opens a second window on the same folder (separate process)
before = set(winauto.windows_of_class('ExplorerPlusMainWnd'))
app.command(IDM['NEW_WINDOW'])
other = []
wait_for(lambda: other.extend(set(winauto.windows_of_class('ExplorerPlusMainWnd')) - before) or bool(other), 8)
check('Ctrl+N opens a new window on the folder', other and 'testdata' in text(other[0]),
      [text(h) for h in other])
for h in other:
    u.PostMessageW(h, 0x0010, 0, 0)

# navigation pane: the current folder is selected in the tree; F2 there renames it
app.go(os.path.join(data, 'sub'), 2.0)
TVM_GETNEXTITEM, TVM_EDITLABELW, TVM_ENDEDITLABELNOW, TVGN_CARET = 0x110A, 0x1141, 0x1116, 0x0009
def tree_rename_box():
    # the label edit shows the caret item's name: only rename when it is the scratch
    # 'sub' (the tree sync is deferred, the caret may still be on its parent; slow on CI)
    for _ in range(30):
        sel = send(app.tree, TVM_GETNEXTITEM, TVGN_CARET, 0)
        if sel:
            send(app.tree, TVM_EDITLABELW, 0, sel)
            ed = next((c for c in children(app.tree) if cls(c) == 'Edit' and u.IsWindowVisible(c)), None)
            if ed and text(ed) == 'sub':
                return ed
            print('  tree caret:', text(ed) if ed else None)
            if ed:
                send(app.tree, TVM_ENDEDITLABELNOW, 1, 0)   # cancel
        time.sleep(0.5)
    return None
ed = tree_rename_box()
check('tree selects the current folder and F2 opens a rename box', ed is not None)
if ed:
    buf = ctypes.create_unicode_buffer('sub2')
    send(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    send(ed, WM_KEYDOWN, 0x0D, 0)
    check('tree rename renames the folder on disk',
          wait_for(lambda: os.path.isdir(os.path.join(data, 'sub2')), 6), os.listdir(data))
    check('renaming the current folder follows it',
          wait_for(lambda: app.title().startswith('sub2'), 4) and
          app.address_text().endswith('sub2'), (app.title(), app.address_text()))
app.go(data, 1.5)
if os.path.isdir(os.path.join(data, 'sub2')):
    os.rename(os.path.join(data, 'sub2'), os.path.join(data, 'sub'))
time.sleep(1)

# watcher: a file created behind the app's back appears (debounced refresh)
with open(os.path.join(data, 'external.txt'), 'w') as f:
    f.write('x')
check('external change auto-refreshes',
      wait_for(lambda: app.item_count() == len(os.listdir(data)), 6), (app.item_count(), len(os.listdir(data))))

# delete to recycle bin
app.select_name('c.txt')
u.PostMessageW(app.hwnd, 0x0111, IDM['DELETE'], 0)   # posted: may raise a confirmation
def confirm_and_gone():
    for h in winauto.top_windows(app.proc.pid):
        if cls(h) == '#32770' and u.IsWindowVisible(h) and 'Delete' in text(h):
            u.PostMessageW(h, 0x0111, 6, 0)      # IDYES
    return not os.path.exists(os.path.join(data, 'c.txt'))
check('delete moves file to recycle bin', wait_for(confirm_and_gone, 8))

# search: a recursive name search of the folder, shown as its own location
needle = os.path.join(data, 'sub', 'needle.txt')
with open(needle, 'w') as f:
    f.write('x')
app.search('needle')
check('search finds a file in a subfolder',
      wait_for(lambda: app.title().startswith('Search Results in testdata') and app.item_count() == 1, 15),
      (app.title(), app.item_count()))
app.screenshot(os.path.join(shots, '07_search.png'))
hdr = send(app.list, 0x101F, 0, 0)                     # LVM_GETHEADER
ncols = send(hdr, 0x1200, 0, 0)                         # HDM_GETITEMCOUNT
check('results show where each item is (Folder column)',
      ncols == 5 and app.item_text(0, 4) == os.path.join(data, 'sub'), (ncols, app.item_text(0, 4)))
txts = sum(1 for _, _, fs in os.walk(data) for n in fs if n.lower().endswith('.txt'))
app.search('*.txt')
check('wildcard search (refined in place)', wait_for(lambda: app.item_count() == txts, 15), (app.item_count(), txts))
send(app.search_box, WM_KEYDOWN, 0x1B, 0)
check('Esc returns to the searched folder and clears the box',
      wait_for(lambda: app.title().startswith('testdata') and not text(app.search_box), 5),
      (app.title(), text(app.search_box)))
app.command(IDM['FWD'])
check('Forward reopens the results with their query',
      wait_for(lambda: app.title().startswith('Search Results') and text(app.search_box) == '*.txt', 3),
      (app.title(), text(app.search_box)))
app.command(IDM['BACK']); time.sleep(1)
os.remove(needle)

# navigation: up to the drive, This PC, Desktop and back
app.go('C:\\')
check('navigate C:\\', '(C:)' in app.title(), app.title())
app.command(IDM['UP'])
wait_for(lambda: 'This PC' in app.title() or 'Tento' in app.title(), 5)
this_pc = app.title()
check('Up from C:\\ -> This PC', 'This PC' in this_pc or 'Tento' in this_pc, this_pc)
wait_for(lambda: send(app.list, 0x10AF, 0, 0), 3)                  # (LVM_ISGROUPVIEWENABLED: the tiles are in)
LVM_GETVIEW, LVM_GETGROUPCOUNT, LVM_ISGROUPVIEWENABLED = 0x108F, 0x1098, 0x10AF
check('This PC shows grouped drive tiles',
      send(app.list, LVM_GETVIEW, 0, 0) == 4 and send(app.list, LVM_ISGROUPVIEWENABLED, 0, 0)
      and send(app.list, LVM_GETGROUPCOUNT, 0, 0) >= 1,
      (send(app.list, LVM_GETVIEW, 0, 0), send(app.list, LVM_GETGROUPCOUNT, 0, 0)))
app.screenshot(os.path.join(shots, '04_thispc.png'))
# a press on the first group's chevron collapses it without selecting its items
LVM_GETGROUPSTATE = 0x105C
dpi = u.GetDpiForWindow(app.hwnd)
pt = ((12 * dpi // 96) << 16) | (14 * dpi // 96)
u.PostMessageW(app.list, 0x0201, 1, pt); u.PostMessageW(app.list, 0x0202, 0, pt)   # posted: no drag loop
check('group chevron collapses the group',
      wait_for(lambda: send(app.list, LVM_GETGROUPSTATE, 0, 1) == 1, 2) and app.selected_count() == 0,
      (send(app.list, LVM_GETGROUPSTATE, 0, 1), app.selected_count()))
app.command(IDM['REFRESH']); time.sleep(1.5)
check('a collapsed group stays collapsed across a refresh', send(app.list, LVM_GETGROUPSTATE, 0, 1) == 1)
app.command(IDM['UP']); time.sleep(2)
check('Up to Desktop lists items', wait_for(lambda: app.item_count() > 0, 5), (app.title(), app.item_count()))
app.screenshot(os.path.join(shots, '05_desktop.png'))
app.command(IDM['BACK'])
check('Back returns to This PC', wait_for(lambda: app.title() == this_pc, 5), app.title())
app.command(IDM['FWD'])
check('Forward', wait_for(lambda: app.title() != this_pc, 5), app.title())
app.go('C:\\does\\not\\exist', 1.5, post=True)
errs = [h for h in __import__('winauto').top_windows(app.proc.pid) if cls(h) == '#32770' and u.IsWindowVisible(h)]
check('bad path shows an error instead of an empty folder', bool(errs))
for h in errs:
    u.PostMessageW(h, 0x0010, 0, 0)
time.sleep(0.5)

# an unreachable UNC path resolves off the UI thread; navigating away supersedes it
app.go(r'\\no-such-host-explorerplus\share', 0.1, post=True)
res = W.DWORD_PTR() if hasattr(W, 'DWORD_PTR') else ctypes.c_size_t()
t0 = time.time()
alive = u.SendMessageTimeoutW(app.hwnd, 0, 0, 0, 0x0002, 1000, ctypes.byref(res))  # WM_NULL, SMTO_ABORTIFHUNG
check('UI responsive while a UNC path resolves', alive and time.time() - t0 < 1.0)
app.go(data, 1.0)
time.sleep(6)
stale = [h for h in __import__('winauto').top_windows(app.proc.pid) if cls(h) == '#32770' and u.IsWindowVisible(h)]
check('superseded typed path does not navigate or pop up', not stale and 'testdata' in app.title(), app.title())
for h in stale:
    u.PostMessageW(h, 0x0010, 0, 0)

app.command(IDM['NEWTAB']); time.sleep(1.5)
app.screenshot(os.path.join(shots, '06_tabs.png'))
app.command(IDM['CLOSETAB']); time.sleep(0.5)

# another instance holding the settings lock: the tab save waits (never writes over it),
# then a retry saves the tab set once the lock is free
k32 = ctypes.windll.kernel32
k32.CreateMutexW.restype = W.HANDLE
def ini_bytes():
    try:
        with open(ini, 'rb') as f:
            return f.read()
    except OSError:                                                 # (being replaced this moment)
        return None
time.sleep(2.5)                                                     # (the tab save above done)
ini_before = ini_bytes()
lock = k32.CreateMutexW(None, False, 'Local\\ExplorerPlus.Settings')
if ini_before and lock and k32.WaitForSingleObject(lock, 5000) == 0:
    app.command(IDM['NEWTAB'])
    time.sleep(6.5)                                                 # saved at +2 s, gives up at +5 s
    check('settings locked by another instance: file untouched', ini_bytes() == ini_before)
    k32.ReleaseMutex(lock)
    check('lock released: the tab save is retried',
          wait_for(lambda: ini_bytes() not in (None, ini_before), 8))
    app.command(IDM['CLOSETAB']); time.sleep(0.5)
else:
    skip('settings locked by another instance: file untouched', 'mutex not acquired')
if lock:
    k32.CloseHandle(lock)

# per-folder view memory survives a restart, even for a non-ASCII, INI-hostile name
app.go(uni)
check('non-ASCII folder opens', app.title().startswith('Stažené'), app.title())
app.command(IDM['LIST']); time.sleep(0.5)
app.go(data)

# Group by Type: one group per type, and it is remembered for this folder only
app.command(IDM['GROUP_TYPE']); time.sleep(0.8)
types = {'File folder'} | {os.path.splitext(n)[1].lower() for n in os.listdir(data)
                           if os.path.isfile(os.path.join(data, n))}
check('Group by Type groups the listing',
      send(app.list, LVM_ISGROUPVIEWENABLED, 0, 0) and
      send(app.list, LVM_GETGROUPCOUNT, 0, 0) == len(types),
      (send(app.list, LVM_GETGROUPCOUNT, 0, 0), sorted(types)))
app.go(os.path.join(data, 'sub2' if os.path.isdir(os.path.join(data, 'sub2')) else 'sub'))
check('grouping is per folder', not send(app.list, LVM_ISGROUPVIEWENABLED, 0, 0))
app.go(data)

# column chooser: header right-click > Date created adds a column, remembered per folder
HDM_GETITEMCOUNT, LVM_GETHEADER = 0x1200, 0x101F
def header_count():
    return send(send(app.list, LVM_GETHEADER, 0, 0), HDM_GETITEMCOUNT, 0, 0)
def choose_column(title):
    hdr = send(app.list, LVM_GETHEADER, 0, 0)
    u.PostMessageW(hdr, 0x0204, 2, (5 << 16) | 40); u.PostMessageW(hdr, 0x0205, 0, (5 << 16) | 40)
    menu = None
    for _ in range(20):
        time.sleep(0.25)
        menu = next((h for h in winauto.top_windows(app.proc.pid) if cls(h) == '#32768' and u.IsWindowVisible(h)), None)
        if menu: break
    items = menu_items(send(menu, 0x01E1, 0, 0)) if menu else []
    if title not in items:
        if menu: app.close_menus()
        return items
    send(menu, 0x01E5, items.index(title), 0)    # MN_SELECTITEM
    u.PostMessageW(menu, WM_KEYDOWN, 0x0D, 0)
    time.sleep(1.5)
    return items
before = header_count()
items = choose_column('Date created')
check('header menu offers columns', 'Date created' in items and 'Name' in items, items)
check('choosing a column adds it', header_count() == before + 1, (before, header_count()))
cols_before_restart = header_count()

app.close()
check('exits cleanly', app.proc.returncode == 0, app.proc.returncode)

app = App(exe, uni, settings=ini)
app.wait_idle(1.5)
check('per-folder view restored after restart (List)', send(app.list, LVM_GETVIEW, 0, 0) == 3,
      send(app.list, LVM_GETVIEW, 0, 0))
app.go(data)
check('Group by restored after restart', send(app.list, LVM_ISGROUPVIEWENABLED, 0, 0))
check('chosen columns restored after restart', header_count() == cols_before_restart,
      (header_count(), cols_before_restart))
choose_column('Date created')
check('unchecking a column removes it', header_count() == cols_before_restart - 1, header_count())
app.go('shell:RecycleBinFolder')
rb = [send(send(app.list, LVM_GETHEADER, 0, 0), HDM_GETITEMCOUNT, 0, 0)]
check('Recycle Bin shows its own columns', rb[0] >= 5, rb)
app.go(data)
app.command(IDM['GROUP_NONE']); time.sleep(0.8)
check('Group by (None) ungroups', not send(app.list, LVM_ISGROUPVIEWENABLED, 0, 0))

# Sort menu entries are the folder's columns: Size sorts largest first, Name back again
def row_of(name):
    return next((i for i in range(app.item_count()) if app.item_text(i) == name), -1)
app.command(IDM['SORT_COL'] + 3)                                   # Name, Date modified, Type, Size
check('Sort by Size (largest first)', wait_for(lambda: 0 <= row_of('photo.jpg') < row_of('external.txt'), 3),
      (row_of('photo.jpg'), row_of('external.txt')))
app.command(IDM['SORT_COL'] + 0)
check('Sort by Name', wait_for(lambda: 0 <= row_of('external.txt') < row_of('photo.jpg'), 3),
      (row_of('external.txt'), row_of('photo.jpg')))

# folder sizes (on by default): a subfolder's Size is everything under it, every level
sized = os.path.join(data, 'sized')
os.makedirs(os.path.join(sized, 'deep', 'deeper'))
with open(os.path.join(sized, 'top.bin'), 'wb') as f:
    f.write(bytes(1000))
with open(os.path.join(sized, 'deep', 'deeper', 'low.bin'), 'wb') as f:
    f.write(bytes(300000))
def byte_size(n):                                                   # as the app formats it: KB, like Explorer
    buf = ctypes.create_unicode_buffer(64)
    ctypes.windll.shlwapi.StrFormatKBSizeW(ctypes.c_longlong(n), buf, 64)
    return buf.value
def size_of(name):
    i = row_of(name)
    return app.item_text(i, 3) if i >= 0 else None                  # Name, Date modified, Type, Size
app.command(IDM['REFRESH'])
check('folder size in the Size column', wait_for(lambda: size_of('sized') == byte_size(301000), 5),
      (size_of('sized'), byte_size(301000)))
app.command(IDM['FOLDERSIZES']); time.sleep(0.5)
check('Folder sizes off: no folder size', size_of('sized') == '', size_of('sized'))
app.command(IDM['FOLDERSIZES'])
check('Folder sizes on again', wait_for(lambda: size_of('sized') == byte_size(301000), 5), size_of('sized'))
# a junction is not followed, not even as the folder whose size is asked for
sized_link = os.path.join(data, 'sized-link')
if subprocess.run(['cmd', '/c', 'mklink', '/J', sized_link, sized], capture_output=True).returncode == 0:
    wait_for(lambda: row_of('sized-link') >= 0, 5)
    time.sleep(1.5)
    check('junction: no folder size', size_of('sized-link') == '', size_of('sized-link'))
    os.rmdir(sized_link)                                            # (the junction, not its target)
else:
    skip('junction: no folder size', 'mklink /J failed')
shutil.rmtree(sized)
check('removed folder leaves the list', wait_for(lambda: row_of('sized') < 0, 5))

# view modes: Extra large / Medium icons are icon views
app.command(IDM['XLARGE']); time.sleep(1.5)
app.screenshot(os.path.join(shots, '08_xlarge.png'))
check('Extra large icons', send(app.list, LVM_GETVIEW, 0, 0) == 0)
app.command(IDM['DETAILS']); time.sleep(0.5)

# item check boxes: a press on an item's box adds just that item to the selection
LVM_GETEXTENDEDLISTVIEWSTYLE, LVM_GETITEMRECT = 0x1037, 0x100E
app.command(IDM['CHECKBOXES']); time.sleep(0.5)
check('Item check boxes on', send(app.list, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0) & 0x4)
app.select_name('photo.jpg')
r = app.item_rect(row_of('external.txt'), 1)                       # LVIR_ICON
box = (((r[1] + r[3]) // 2) << 16) | max(1, r[0] - 8)
u.PostMessageW(app.list, 0x0201, 1, box); u.PostMessageW(app.list, 0x0202, 0, box)
check('check box adds to the selection', wait_for(lambda: app.selected_count() == 2, 2), app.selected_count())
app.screenshot(os.path.join(shots, '09_checkboxes.png'))
u.PostMessageW(app.list, 0x0201, 1, box); u.PostMessageW(app.list, 0x0202, 0, box)
check('check box again removes it', wait_for(lambda: app.selected_count() == 1, 2), app.selected_count())
app.command(IDM['CHECKBOXES']); time.sleep(0.3)

# Compact view (starts as Windows' own setting): toggling changes the Details row
# spacing and the navigation pane's along with it, toggling back restores them
def row_h():
    r = app.item_rect(0, 0)
    return r[3] - r[1]
def tree_h():
    return send(app.tree, TVM_GETITEMHEIGHT, 0, 0)
first, tfirst = row_h(), tree_h()
app.command(IDM['COMPACT']); time.sleep(0.5)
second, tsecond = row_h(), tree_h()
check('Compact view changes the Details row spacing', 0 < min(first, second) and first != second, (first, second))
check('... and the navigation pane\'s, the same way',
      0 < min(tfirst, tsecond) and (tfirst < tsecond) == (first < second) and tfirst != tsecond, (tfirst, tsecond))
app.command(IDM['COMPACT']); time.sleep(0.5)
check('Compact view toggled back restores the spacing', row_h() == first and tree_h() == tfirst, (row_h(), tree_h()))

# Preview pane: a text file shows through its shell preview handler (a window in the host)
app.command(IDM['PREVIEW']); time.sleep(0.5)
app.select_name('external.txt')
def preview_window():
    hosts = [c for c in children(app.hwnd) if cls(c) == 'ExplorerPlusPreviewHost']
    return any(u.IsWindowVisible(h) and children(h) for h in hosts)
check('Preview pane shows a text file through its preview handler', wait_for(preview_window, 8))
app.screenshot(os.path.join(shots, '10_preview.png'))
app.select_name('photo.jpg')
check('a file without a preview handler drops the handler', wait_for(lambda: not preview_window(), 5))
app.command(IDM['PREVIEW']); time.sleep(0.5)

# File name extensions off: "a.txt" lists as "a"; renaming keeps the hidden extension
app.command(IDM['EXTENSIONS'])
check('extensions hidden', wait_for(lambda: row_of('a') >= 0 and row_of('a.txt') < 0, 5),
      [app.item_text(i) for i in range(app.item_count())])
app.select_name('a')
app.command(IDM['RENAME'])
wait_for(lambda: label_edit() is not None, 3)
ed = label_edit()
if ed:
    buf = ctypes.create_unicode_buffer('z')
    send(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    send(ed, WM_KEYDOWN, 0x0D, 0)
check('rename with the extension hidden keeps it', wait_for(lambda: os.path.isfile(os.path.join(data, 'z.txt')), 6),
      os.listdir(data))
app.command(IDM['EXTENSIONS'])
check('extensions shown again', wait_for(lambda: row_of('z.txt') >= 0, 5), [app.item_text(i) for i in range(app.item_count())])

# Windows hides a shortcut's extension whatever the setting: a rename keeps it
lnk = os.path.join(data, 'shortcut.lnk')
subprocess.run(['powershell', '-NoProfile', '-Command',
                '$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:LNK); '
                '$s.TargetPath = $env:TARGET; $s.Save()'],
               env=dict(os.environ, LNK=lnk, TARGET=os.path.join(data, 'z.txt')), check=False)
app.command(IDM['REFRESH'])
wait_for(lambda: row_of('shortcut') >= 0, 5)
app.select_name('shortcut')
app.command(IDM['RENAME'])
wait_for(lambda: label_edit() is not None, 3)
ed = label_edit()
if ed:
    buf = ctypes.create_unicode_buffer('link')
    send(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    send(ed, WM_KEYDOWN, 0x0D, 0)
check('renaming a shortcut keeps its hidden .lnk', wait_for(lambda: os.path.isfile(os.path.join(data, 'link.lnk')), 6),
      os.listdir(data))

def dialogs():
    return [h for h in winauto.top_windows(app.proc.pid) if cls(h) == '#32770' and u.IsWindowVisible(h)]
def view():
    return send(app.list, LVM_GETVIEW, 0, 0)

# View > Apply this view to all folders: a folder never opened before gets this
# folder's view and columns; Reset all folders brings every folder back to Details
app.command(IDM['DETAILS']); time.sleep(0.5)
base_cols = header_count()
choose_column('Date created')
applied_cols = header_count()
app.command(IDM['LIST']); time.sleep(0.5)
app.command(IDM['APPLY_ALL']); time.sleep(0.5)
fresh = os.path.join(data, 'fresh')
os.makedirs(fresh)
app.go(fresh)
check('Apply to all folders: a new folder opens in that view', view() == LV_VIEW_LIST, view())
check('Apply to all folders: a new folder gets those columns', header_count() == applied_cols, (header_count(), applied_cols))
app.go(os.path.join(data, 'sub'))
check('Apply to all folders: folders with their own view follow too', view() == LV_VIEW_LIST, view())
app.command(IDM['RESET_ALL'])
check('Reset all folders: back to Details', wait_for(lambda: view() == LV_VIEW_DETAILS, 3), view())
app.go(fresh)
check('Reset all folders: default columns again', view() == LV_VIEW_DETAILS and header_count() == base_cols,
      (view(), header_count(), base_cols))
app.go(data)

# text size: list and tree rows grow, the menu limits hold, the size survives a restart
rows0, tree0 = row_h(), send(app.tree, TVM_GETITEMHEIGHT, 0, 0)
app.command(IDM['TEXT_LARGER']); app.command(IDM['TEXT_LARGER']); time.sleep(0.5)
rows1, tree1 = row_h(), send(app.tree, TVM_GETITEMHEIGHT, 0, 0)
check('Larger text grows list and tree rows', rows1 > rows0 and tree1 > tree0, (rows0, rows1, tree0, tree1))
app.screenshot(os.path.join(shots, '11_text_larger.png'))
app.command(IDM['TEXT_SMALLER']); time.sleep(0.3)
check('Smaller text shrinks them again', rows0 < row_h() < rows1, (rows0, row_h(), rows1))
app.command(IDM['TEXT_LARGER']); time.sleep(0.3)

# a folder in the profile, inside the hidden AppData: the tree walks there under This PC
# (the path's own pidl goes by way of the user's folder) and shows AppData on the way
temp = os.environ.get('TEMP', '')
if temp.lower().startswith(os.path.expanduser('~').lower() + '\\appdata\\'):
    app.go(temp, 2.0)
    check('the tree follows into a folder inside the hidden AppData',
          wait_for(lambda: app.tree_item(app.tree_caret())[0].lower() == os.path.basename(temp).lower(), 8),
          app.tree_item(app.tree_caret()))
    app.go(data)

# deleting the open folder in the navigation pane opens the next folder, no error
for n in ('del1', 'del2'):
    os.makedirs(os.path.join(data, n))
app.go(os.path.join(data, 'del1'), 2.0)
caret_ok = wait_for(lambda: app.tree_item(app.tree_caret())[0] == 'del1', 8)
check('the tree selects a folder created while the app runs', caret_ok, app.tree_item(app.tree_caret()))
if not caret_ok:                                                     # never delete the wrong node
    app.close(); sys.exit('aborting: the tree is not on the scratch folder')
u.PostMessageW(app.tree, WM_KEYDOWN, 0x2E, 0)                        # Del in the tree (TVN_KEYDOWN)
errors, seen = [], set()
def tree_delete_done():
    for h in dialogs():
        if 'Delete' in text(h):                                      # recycle confirmation: Yes
            seen.add(text(h))
            for c in children(h):
                if cls(c) == 'Button' and u.GetDlgCtrlID(c) == 6:
                    u.PostMessageW(c, 0x00F5, 0, 0)                  # BM_CLICK
        else:
            errors.append(text(h)); u.PostMessageW(h, 0x0010, 0, 0)
    return not os.path.exists(os.path.join(data, 'del1')) and app.title().startswith('del2')
check('tree Delete removes the open folder and opens the next one', wait_for(tree_delete_done, 10),
      (os.path.exists(os.path.join(data, 'del1')), app.title(), seen))
time.sleep(1.5)
errors += [text(h) for h in dialogs()]
check('tree Delete shows no "Location is not available"', not errors, errors)

# the open folder removed behind the app's back: it goes to the parent, no error
os.rmdir(os.path.join(data, 'del2'))
check('a folder deleted externally leaves for its parent', wait_for(lambda: app.title().startswith('testdata'), 6), app.title())
time.sleep(1.5)
errors = [text(h) for h in dialogs()]
check('... without an error dialog', not errors, errors)
for h in dialogs():
    u.PostMessageW(h, 0x0010, 0, 0)

# a big folder deleted in the list leaves the navigation pane too (the shell reports it
# as one "folder updated", not "folder removed")
big = os.path.join(data, 'bigdir')
for i in range(2000):
    d = os.path.join(big, 'd%d' % (i // 50))
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, '%d.txt' % i), 'w').close()
def big_in_tree():
    return any(app.tree_item(h)[0] == 'bigdir' for h in app.tree_nodes())
send(app.tree, 0x1102, 0x0002, app.tree_caret())                    # TVM_EXPAND (TVE_EXPAND)
check('the tree shows a big folder made while the app runs', wait_for(big_in_tree, 8))
wait_for(lambda: row_of('bigdir') >= 0, 5)
app.select_name('bigdir')
u.PostMessageW(app.hwnd, 0x0111, IDM['DELETE'], 0)
def big_gone():
    for h in dialogs():
        if 'Delete' in text(h):
            u.PostMessageW(h, 0x0111, 6, 0)                          # IDYES
    return not os.path.exists(big)
check('a big folder deleted in the list goes from the tree',
      wait_for(big_gone, 15) and wait_for(lambda: not big_in_tree(), 5),
      ('on disk' if os.path.exists(big) else 'gone from disk', 'in tree' if big_in_tree() else 'gone from tree'))

app.close()
check('exits cleanly after the second run', app.proc.returncode == 0, app.proc.returncode)
app = App(exe, data, settings=ini)
app.wait_idle(2)
check('text size remembered after restart', row_h() == rows1, (row_h(), rows1))
app.command(IDM['TEXT_RESET']); time.sleep(0.5)
check('Reset text size', row_h() == rows0, (row_h(), rows0))
app.close()

failed = [n for n, ok in results if not ok]
if failed and os.path.exists(events):
    keys = ('process.start', 'nav.', 'tree.delete', 'tree.rename', 'tree.select', 'watch.full', 'watch.dir', 'enum.start')
    print('\nevents (%s):' % events)
    with open(events, encoding='utf-8-sig', errors='replace') as f:
        for line in f:
            if any(k in line for k in keys):
                print('  ' + line.rstrip())
print('\n%d/%d passed' % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)
