"""Smoke test for Explorer Plus. Usage: python -I tools/smoke_test.py <exe> <workdir>

Builds a scratch folder under <workdir>, drives the app through window messages and
checks results on disk / in the UI. Screenshots land in <workdir>/shots.
"""
import os, sys, time, shutil, ctypes, ctypes.wintypes as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import winauto
from winauto import App, menu_items, u, children, cls, text, clipboard_text, WM_KEYDOWN, WM_SETTEXT

IDM = dict(NEWFOLDER=40010, NEWTAB=40011, CLOSETAB=40012, COPY=40031, PASTE=40032, SELALL=40033, SELNONE=40034,
           DELETE=40036, RENAME=40037, DETAILS=40050, TILES=40051, LARGE=40052, MEDIUM=40053,
           LIST=40055, HIDDEN=40056, BACK=40070, FWD=40071, UP=40072, REFRESH=40073, INVERT=40035,
           NEW_MENU=40130, COPY_PATH=40135, NEW_WINDOW=40137, FOCUS_ADDRESS=40096,
           GROUP_NONE=40141, GROUP_TYPE=40144, XLARGE=40150, CHECKBOXES=40152, EXTENSIONS=40153,
           PREVIEW=40154, COMPACT=40155, TOOLS_MENU=40156, CMDBAR=40157, SORT_COL=40160)

exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
data = os.path.join(work, 'testdata')
shots = os.path.join(work, 'shots')
shutil.rmtree(data, ignore_errors=True)
os.makedirs(os.path.join(data, 'sub'))
os.makedirs(shots, exist_ok=True)
for n in ('a.txt', 'b.txt'):
    with open(os.path.join(data, n), 'w') as f:
        f.write('hello ' + n)
wall = r'C:\Windows\Web\Wallpaper\Windows\img0.jpg'
if os.path.exists(wall):
    shutil.copy(wall, os.path.join(data, 'photo.jpg'))

results = []
def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print(('PASS ' if ok else 'FAIL ') + name + (('  [' + str(detail) + ']') if detail else ''))

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
# promote a classic menu item every machine has into the compact menu (UTF-16 INI)
with open(ini, 'w', encoding='utf-16') as f:
    f.write('[contextmenu]\npromote1=Send to|%SystemRoot%\\System32\\shell32.dll,4\n')
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

app.command(IDM['MEDIUM'])
time.sleep(2)
app.screenshot(os.path.join(shots, '02_medium.png'))
app.command(IDM['TILES'])
time.sleep(1)
app.screenshot(os.path.join(shots, '03_tiles.png'))
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
    sub_items = menu_items(u.SendMessageW(menu, 0x01E1, 0, 0)) if menu else []
    pick = next((i for i, s in enumerate(sub_items) if s.replace('&', '').startswith('List')), -1)
    if pick >= 0:
        u.SendMessageW(menu, 0x01E5, pick, 0)      # MN_SELECTITEM
        u.PostMessageW(menu, WM_KEYDOWN, 0x0D, 0)
check('compact menu View submenu chooses a view',
      wait_for(lambda: app.flyout() is None and u.SendMessageW(app.list, 0x108F, 0, 0) == 3, 4), sub_items)
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
    u.SendMessageW(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    u.SendMessageW(ed, WM_KEYDOWN, 0x0D, 0)
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
    u.SendMessageW(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    u.SendMessageW(ed, WM_KEYDOWN, 0x0D, 0)
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
new_items = menu_items(u.SendMessageW(menu, 0x01E1, 0, 0)) if menu else []
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
        u.SendMessageW(ed, WM_KEYDOWN, 0x1B, 0)                 # Esc: keep the name
    time.sleep(0.5)
    for n in created():
        pth = os.path.join(data, n)
        (os.rmdir if os.path.isdir(pth) else os.remove)(pth)
else:
    app.close_menus()
# address bar: Ctrl+L switches the breadcrumb to an edit box with the path
app.command(IDM['FOCUS_ADDRESS'])
time.sleep(0.3)
check('Ctrl+L shows the address edit with the path',
      u.IsWindowVisible(app.addr_edit) and app.address_text() == data, app.address_text())
u.SendMessageW(app.addr_edit, WM_KEYDOWN, 0x1B, 0)
time.sleep(0.3)
check('Esc returns to the breadcrumb', not u.IsWindowVisible(app.addr_edit))

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
    # 'sub' (the tree sync is deferred, the caret may still be on its parent)
    for _ in range(12):
        sel = u.SendMessageW(app.tree, TVM_GETNEXTITEM, TVGN_CARET, 0)
        if sel:
            u.SendMessageW(app.tree, TVM_EDITLABELW, 0, sel)
            ed = next((c for c in children(app.tree) if cls(c) == 'Edit' and u.IsWindowVisible(c)), None)
            if ed and text(ed) == 'sub':
                return ed
            print('  tree caret:', text(ed) if ed else None)
            if ed:
                u.SendMessageW(app.tree, TVM_ENDEDITLABELNOW, 1, 0)   # cancel
        time.sleep(0.5)
    return None
ed = tree_rename_box()
check('tree selects the current folder and F2 opens a rename box', ed is not None)
if ed:
    buf = ctypes.create_unicode_buffer('sub2')
    u.SendMessageW(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    u.SendMessageW(ed, WM_KEYDOWN, 0x0D, 0)
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
hdr = u.SendMessageW(app.list, 0x101F, 0, 0)                     # LVM_GETHEADER
ncols = u.SendMessageW(hdr, 0x1200, 0, 0)                         # HDM_GETITEMCOUNT
check('results show where each item is (Folder column)',
      ncols == 5 and app.item_text(0, 4) == os.path.join(data, 'sub'), (ncols, app.item_text(0, 4)))
txts = sum(1 for _, _, fs in os.walk(data) for n in fs if n.lower().endswith('.txt'))
app.search('*.txt')
check('wildcard search (refined in place)', wait_for(lambda: app.item_count() == txts, 15), (app.item_count(), txts))
u.SendMessageW(app.search_box, WM_KEYDOWN, 0x1B, 0)
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
app.command(IDM['UP']); time.sleep(1.5)
this_pc = app.title()
check('Up from C:\\ -> This PC', 'This PC' in this_pc or 'Tento' in this_pc, this_pc)
LVM_GETVIEW, LVM_GETGROUPCOUNT, LVM_ISGROUPVIEWENABLED = 0x108F, 0x1098, 0x10AF
check('This PC shows grouped drive tiles',
      u.SendMessageW(app.list, LVM_GETVIEW, 0, 0) == 4 and u.SendMessageW(app.list, LVM_ISGROUPVIEWENABLED, 0, 0)
      and u.SendMessageW(app.list, LVM_GETGROUPCOUNT, 0, 0) >= 1,
      (u.SendMessageW(app.list, LVM_GETVIEW, 0, 0), u.SendMessageW(app.list, LVM_GETGROUPCOUNT, 0, 0)))
app.screenshot(os.path.join(shots, '04_thispc.png'))
# a press on the first group's chevron collapses it without selecting its items
LVM_GETGROUPSTATE = 0x105C
dpi = u.GetDpiForWindow(app.hwnd)
pt = ((12 * dpi // 96) << 16) | (14 * dpi // 96)
u.PostMessageW(app.list, 0x0201, 1, pt); u.PostMessageW(app.list, 0x0202, 0, pt)   # posted: no drag loop
check('group chevron collapses the group',
      wait_for(lambda: u.SendMessageW(app.list, LVM_GETGROUPSTATE, 0, 1) == 1, 2) and app.selected_count() == 0,
      (u.SendMessageW(app.list, LVM_GETGROUPSTATE, 0, 1), app.selected_count()))
app.command(IDM['REFRESH']); time.sleep(1.5)
check('a collapsed group stays collapsed across a refresh', u.SendMessageW(app.list, LVM_GETGROUPSTATE, 0, 1) == 1)
app.command(IDM['UP']); time.sleep(2)
check('Up to Desktop lists items', wait_for(lambda: app.item_count() > 0, 5), (app.title(), app.item_count()))
app.screenshot(os.path.join(shots, '05_desktop.png'))
app.command(IDM['BACK']); time.sleep(1.5)
check('Back returns to This PC', app.title() == this_pc, app.title())
app.command(IDM['FWD']); time.sleep(1.5)
check('Forward', app.title() != this_pc, app.title())
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
      u.SendMessageW(app.list, LVM_ISGROUPVIEWENABLED, 0, 0) and
      u.SendMessageW(app.list, LVM_GETGROUPCOUNT, 0, 0) == len(types),
      (u.SendMessageW(app.list, LVM_GETGROUPCOUNT, 0, 0), sorted(types)))
app.go(os.path.join(data, 'sub2' if os.path.isdir(os.path.join(data, 'sub2')) else 'sub'))
check('grouping is per folder', not u.SendMessageW(app.list, LVM_ISGROUPVIEWENABLED, 0, 0))
app.go(data)

# column chooser: header right-click > Date created adds a column, remembered per folder
HDM_GETITEMCOUNT, LVM_GETHEADER = 0x1200, 0x101F
def header_count():
    return u.SendMessageW(u.SendMessageW(app.list, LVM_GETHEADER, 0, 0), HDM_GETITEMCOUNT, 0, 0)
def choose_column(title):
    hdr = u.SendMessageW(app.list, LVM_GETHEADER, 0, 0)
    u.PostMessageW(hdr, 0x0204, 2, (5 << 16) | 40); u.PostMessageW(hdr, 0x0205, 0, (5 << 16) | 40)
    menu = None
    for _ in range(20):
        time.sleep(0.25)
        menu = next((h for h in winauto.top_windows(app.proc.pid) if cls(h) == '#32768' and u.IsWindowVisible(h)), None)
        if menu: break
    items = menu_items(u.SendMessageW(menu, 0x01E1, 0, 0)) if menu else []
    if title not in items:
        if menu: app.close_menus()
        return items
    u.SendMessageW(menu, 0x01E5, items.index(title), 0)    # MN_SELECTITEM
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
check('per-folder view restored after restart (List)', u.SendMessageW(app.list, LVM_GETVIEW, 0, 0) == 3,
      u.SendMessageW(app.list, LVM_GETVIEW, 0, 0))
app.go(data)
check('Group by restored after restart', u.SendMessageW(app.list, LVM_ISGROUPVIEWENABLED, 0, 0))
check('chosen columns restored after restart', header_count() == cols_before_restart,
      (header_count(), cols_before_restart))
choose_column('Date created')
check('unchecking a column removes it', header_count() == cols_before_restart - 1, header_count())
app.go('shell:RecycleBinFolder')
rb = [u.SendMessageW(u.SendMessageW(app.list, LVM_GETHEADER, 0, 0), HDM_GETITEMCOUNT, 0, 0)]
check('Recycle Bin shows its own columns', rb[0] >= 5, rb)
app.go(data)
app.command(IDM['GROUP_NONE']); time.sleep(0.8)
check('Group by (None) ungroups', not u.SendMessageW(app.list, LVM_ISGROUPVIEWENABLED, 0, 0))

# Sort menu entries are the folder's columns: Size sorts largest first, Name back again
def row_of(name):
    return next((i for i in range(app.item_count()) if app.item_text(i) == name), -1)
app.command(IDM['SORT_COL'] + 3); time.sleep(0.8)                 # Name, Date modified, Type, Size
check('Sort by Size (largest first)', row_of('photo.jpg') < row_of('external.txt'),
      (row_of('photo.jpg'), row_of('external.txt')))
app.command(IDM['SORT_COL'] + 0); time.sleep(0.8)
check('Sort by Name', row_of('external.txt') < row_of('photo.jpg'), (row_of('external.txt'), row_of('photo.jpg')))

# view modes: Extra large / Medium icons are icon views
app.command(IDM['XLARGE']); time.sleep(1.5)
app.screenshot(os.path.join(shots, '08_xlarge.png'))
check('Extra large icons', u.SendMessageW(app.list, LVM_GETVIEW, 0, 0) == 0)
app.command(IDM['DETAILS']); time.sleep(0.5)

# item check boxes: a press on an item's box adds just that item to the selection
LVM_GETEXTENDEDLISTVIEWSTYLE, LVM_GETITEMRECT = 0x1037, 0x100E
app.command(IDM['CHECKBOXES']); time.sleep(0.5)
check('Item check boxes on', u.SendMessageW(app.list, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0) & 0x4)
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
# spacing, toggling back restores it
def row_h():
    r = app.item_rect(0, 0)
    return r[3] - r[1]
first = row_h()
app.command(IDM['COMPACT']); time.sleep(0.5)
second = row_h()
check('Compact view changes the Details row spacing', 0 < min(first, second) and first != second, (first, second))
app.command(IDM['COMPACT']); time.sleep(0.5)
check('Compact view toggled back restores the spacing', row_h() == first, row_h())

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
app.command(IDM['EXTENSIONS']); time.sleep(1.5)
check('extensions hidden', row_of('a') >= 0 and row_of('a.txt') < 0, [app.item_text(i) for i in range(app.item_count())])
app.select_name('a')
app.command(IDM['RENAME'])
wait_for(lambda: label_edit() is not None, 3)
ed = label_edit()
if ed:
    buf = ctypes.create_unicode_buffer('z')
    u.SendMessageW(ed, WM_SETTEXT, 0, ctypes.addressof(buf))
    u.SendMessageW(ed, WM_KEYDOWN, 0x0D, 0)
check('rename with the extension hidden keeps it', wait_for(lambda: os.path.isfile(os.path.join(data, 'z.txt')), 6),
      os.listdir(data))
app.command(IDM['EXTENSIONS']); time.sleep(1.5)
check('extensions shown again', row_of('z.txt') >= 0, [app.item_text(i) for i in range(app.item_count())])
app.close()

failed = [n for n, ok in results if not ok]
print('\n%d/%d passed' % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)
