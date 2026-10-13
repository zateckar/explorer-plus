"""Performance scenarios for Explorer Plus.

Usage: python -I tools/perf.py <exe> <workdir> [runs]

Creates test folders under <workdir> (50 files, 30,000 files), runs the app with
EXPLORERPLUS_PERF=<log> and a scratch settings file, drives scenarios through
window messages and prints a table from the logs:

  startup     process start -> window shown -> first paint -> listing complete (median of runs)
  big folder  30k files: first rows, complete, UI time spent applying batches
  scroll      page-down through the 30k listing: list paint times
  sort        sort the 30k listing by size and back by name
  views       Details -> Large icons -> List -> Details
  resize      20 window resizes: main + list paint times
  tree        expand C:\\Windows\\WinSxS (~17,000 subfolders) in the navigation pane:
              the listing (worker) and inserting the nodes (UI; 0 = no message over 16 ms)
"""
import os, sys, time, re, statistics, ctypes, ctypes.wintypes as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from winauto import App, u

exe = os.path.abspath(sys.argv[1])
work = os.path.abspath(sys.argv[2])
runs = int(sys.argv[3]) if len(sys.argv) > 3 else 5

IDM_PERF_MARK, IDM_VIEW_DETAILS, IDM_VIEW_LARGE, IDM_VIEW_LIST = 40158, 40050, 40052, 40055
IDM_SORT_COL_BASE = 40160
WM_COMMAND, WM_KEYDOWN, WM_VSCROLL = 0x0111, 0x0100, 0x0115

def make_folder(path, n, size=0):
    os.makedirs(path, exist_ok=True)
    have = len(os.listdir(path))
    if have >= n:
        return
    for i in range(have, n):
        with open(os.path.join(path, 'file%05d.txt' % i), 'wb') as f:
            if size:
                f.write(b'x' * (i % size))

small = os.path.join(work, 'small')
big = os.path.join(work, 'big')
make_folder(small, 50, 100)
make_folder(big, 30000, 1000)

def log_lines(path):
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []

def wait_log(path, pred, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        lines = log_lines(path)
        if any(pred(l) for l in lines):
            return True
        time.sleep(0.05)
    return False

def parse(lines):
    """[(t, event, {dur, n, ...}, detail)]"""
    out = []
    for l in lines:
        m = re.match(r'\s*([\d.]+) (\S+)(.*)', l)
        if not m:
            continue
        kv = dict(re.findall(r'(\w+)=([\d.]+)', m.group(3)))
        out.append((float(m.group(1)), m.group(2), {k: float(v) for k, v in kv.items()}, m.group(3)))
    return out

def first(ev, name):
    return next((e for e in ev if e[1] == name), None)

def launch(folder, tag):
    log = os.path.join(work, 'perf_%s.log' % tag)
    ini = os.path.join(work, 'perf_%s.ini' % tag)
    for p in (log, ini):
        if os.path.exists(p):
            os.remove(p)
    os.environ['EXPLORERPLUS_PERF'] = log
    app = App(exe, folder, settings=ini)
    del os.environ['EXPLORERPLUS_PERF']
    return app, log

def mark(app, log):
    n = sum(1 for l in log_lines(log) if ' mark' in l)
    u.SendMessageW(app.hwnd, WM_COMMAND, IDM_PERF_MARK, 0)
    wait_log(log, lambda l: False, 0)
    return n

def segment_paints(log, k):
    """paint summaries flushed by the k-th mark (0-based)"""
    ev = parse(log_lines(log))
    seg, marks = [], 0
    for e in ev:
        if e[1] == 'mark':
            if marks == k:
                return seg
            marks += 1
            seg = []
        elif e[1].startswith('paint.'):
            seg.append(e)
    return seg

rows = []
def report(name, value, note=''):
    rows.append((name, value, note))
    print('%-34s %12s  %s' % (name, value, note), flush=True)

# --- startup ------------------------------------------------------------
shown, painted, complete = [], [], []
for i in range(runs):
    app, log = launch(small, 'start%d' % i)
    try:
        wait_log(log, lambda l: 'enum.complete' in l, 20)
        time.sleep(0.3)
        ev = parse(log_lines(log))
        s, p, c = first(ev, 'window.shown'), first(ev, 'main.first_paint'), first(ev, 'enum.complete')
        if s: shown.append(s[0])
        if p: painted.append(p[0])
        if c: complete.append(c[0])
    finally:
        app.close()
med = lambda v: '%.0f ms' % statistics.median(v) if v else 'n/a'
report('startup: window shown', med(shown), 'median of %d' % runs)
report('startup: first paint', med(painted))
report('startup: 50-file listing complete', med(complete))

# --- 30k folder ---------------------------------------------------------
app, log = launch(big, 'big')
try:
    ok = wait_log(log, lambda l: 'enum.complete' in l, 120)
    time.sleep(0.5)
    ev = parse(log_lines(log))
    starts = [e for e in ev if e[1] == 'enum.start' and big.lower() in e[3].lower()]
    t0 = starts[-1][0] if starts else 0
    fi = [e for e in ev if e[1] == 'enum.first_item']
    af = [e for e in ev if e[1] == 'enum.apply_first']
    ap = [e for e in ev if e[1] == 'enum.append']
    aa = [e for e in ev if e[1] == 'enum.apply_all']
    wd = [e for e in ev if e[1] == 'enum.worker_done']
    cp = [e for e in ev if e[1] == 'enum.complete']
    if af: report('30k: first rows on screen', '%.0f ms' % (af[-1][0] - t0), 'after navigation start')
    if wd: report('30k: worker enumeration', '%.0f ms' % wd[-1][2].get('dur', 0), '%d items' % wd[-1][2].get('n', 0))
    if cp: report('30k: listing complete', '%.0f ms' % (cp[-1][0] - t0), '' if ok else 'TIMEOUT')
    ui = sum(e[2].get('dur', 0) for e in af + ap + aa)
    mx = max([e[2].get('dur', 0) for e in af + ap + aa] or [0])
    report('30k: UI time applying batches', '%.0f ms' % ui, 'longest %.0f ms, %d batches' % (mx, len(af + ap + aa)))
    k = mark(app, log)
    for e in segment_paints(log, k):
        report('30k load ' + e[1], '%.2f ms' % e[2].get('mean', 0), e[3].strip())

    # scroll
    u.SetForegroundWindow(app.hwnd)
    t = time.time()
    for _ in range(60):
        u.SendMessageW(app.list, WM_VSCROLL, 3, 0)        # SB_PAGEDOWN
        u.UpdateWindow(app.list)
    dt = (time.time() - t) * 1000
    k = mark(app, log)
    report('scroll: 60 page-downs', '%.0f ms' % dt, '%.1f ms per page' % (dt / 60))
    for e in segment_paints(log, k):
        report('scroll ' + e[1], '%.2f ms' % e[2].get('mean', 0), e[3].strip())

    # sort by Size (display column 3), then by Name
    t = time.time()
    u.SendMessageW(app.hwnd, WM_COMMAND, IDM_SORT_COL_BASE + 3, 0)
    u.SendMessageW(app.hwnd, WM_COMMAND, IDM_SORT_COL_BASE + 0, 0)
    dt = (time.time() - t) * 1000
    ev = parse(log_lines(log))
    sorts = [e[2].get('dur', 0) for e in ev if e[1] == 'sort']
    report('sort: 30k by size + by name', '%.0f ms' % dt, 'sort() %s ms' % ', '.join('%.0f' % s for s in sorts[-2:]))
    mark(app, log)

    # views
    t = time.time()
    for v in (IDM_VIEW_LARGE, IDM_VIEW_LIST, IDM_VIEW_DETAILS):
        u.SendMessageW(app.hwnd, WM_COMMAND, v, 0)
        u.UpdateWindow(app.hwnd)
        u.UpdateWindow(app.list)
    dt = (time.time() - t) * 1000
    ev = parse(log_lines(log))
    va = [e[2].get('dur', 0) for e in ev if e[1] == 'view.apply'][-3:]
    k = mark(app, log)
    report('views: Large -> List -> Details', '%.0f ms' % dt, 'apply %s ms' % ', '.join('%.0f' % s for s in va))
    for e in segment_paints(log, k):
        report('views ' + e[1], '%.2f ms' % e[2].get('mean', 0), e[3].strip())

    # resize
    rc = W.RECT()
    u.GetWindowRect(app.hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    t = time.time()
    for i in range(20):
        d = 80 if i % 2 == 0 else 0
        u.SetWindowPos(app.hwnd, None, rc.left, rc.top, w - d, h - d, 0x0004 | 0x0010)
        u.UpdateWindow(app.hwnd)
    dt = (time.time() - t) * 1000
    k = mark(app, log)
    report('resize: 20 resizes', '%.0f ms' % dt, '%.1f ms each' % (dt / 20))
    for e in segment_paints(log, k):
        report('resize ' + e[1], '%.2f ms' % e[2].get('mean', 0), e[3].strip())
finally:
    app.close()

# --- navigation pane: a node with ~17,000 subfolders (C:\Windows\WinSxS) -----
sxs = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'WinSxS')
if os.path.isdir(sxs):
    app, log = launch(sxs, 'tree')
    try:
        wait_log(log, lambda l: 'tree.synced n=1' in l, 30)
        time.sleep(0.5)
        n0 = len(log_lines(log))
        TVM_EXPAND, TVM_GETNEXTITEM, TVGN_CARET, TVE_EXPAND = 0x1102, 0x110A, 0x0009, 0x0002
        u.SendMessageW(app.tree, TVM_EXPAND, TVE_EXPAND, u.SendMessageW(app.tree, TVM_GETNEXTITEM, TVGN_CARET, 0))
        wait_log(log, lambda l: 'tree.list' in l and l.rstrip().lower().endswith('winsxs'), 60)
        time.sleep(1.0)
        ev = parse(log_lines(log)[n0:])
        tl = [e for e in ev if e[1] == 'tree.list' and e[3].rstrip().lower().endswith('winsxs')]
        if tl:
            report('tree: list WinSxS subfolders', '%.0f ms' % tl[-1][2].get('dur', 0),
                   '%d folders (worker)' % tl[-1][2].get('n', 0))
            # the UI side: the longest message after the listing came in (inserting the nodes)
            after = [e[2].get('dur', 0) for e in ev if e[1] == 'slow.msg' and e[0] >= tl[-1][0]]
            report('tree: insert them (UI)', '%.0f ms' % max(after or [0]), 'longest message after the listing')
    finally:
        app.close()
