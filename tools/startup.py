"""Startup as the user sees it: Explorer Plus, its Notepad and its player.

Usage: python -I tools/startup.py <exe> <workdir> [runs] [scenario ...]

Each scenario starts the exe as the shell would (a folder; --notepad <file>; --play <file>),
with a scratch settings file, and measures from just before the process is created:

  shown   the app's window on the screen (visible and no longer cloaked: painted)
  ready   what was asked for there: the folder's files listed, the text in the Notepad's
          box, the player's window titled with the file (it plays)

Scenarios: main, notepad, notepad-tabs (a session of 12 tabs, two of them big files, restored
before the file asked for), play (a WAV), picture (a PNG in a folder of 3,000 pictures),
player-queue (a remembered queue of 20,000 files). Prints the median of the runs.
"""
import os, sys, time, struct, zlib, statistics, subprocess, ctypes, ctypes.wintypes as W, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from winauto import u, cls, top_windows, children, send, RES

exe = os.path.abspath(sys.argv[1])
work = os.path.abspath(sys.argv[2])
runs = int(sys.argv[3]) if len(sys.argv) > 3 else 5
only = sys.argv[4:]
os.makedirs(work, exist_ok=True)

dwm = ctypes.WinDLL('dwmapi')
DWMWA_CLOAKED = 14
LVM_GETITEMCOUNT = 0x1004
WM_CLOSE, WM_GETTEXTLENGTH = 0x0010, 0x000E


def cloaked(h):
    v = W.DWORD()
    return dwm.DwmGetWindowAttribute(W.HWND(h), DWMWA_CLOAKED, ctypes.byref(v), 4) == 0 and v.value != 0


def title(h):
    buf = ctypes.create_unicode_buffer(512)
    u.GetWindowTextW(h, buf, 512)
    return buf.value


def app_window(pid, prefix):
    for h in top_windows(pid):
        c = cls(h)
        if c.startswith(prefix) and not c.startswith(prefix + 'Gutter') and u.IsWindowVisible(h):
            return h
    return None


# --- test files ---------------------------------------------------------
def write(path, data):
    if not os.path.exists(path) or os.path.getsize(path) != len(data):
        with open(path, 'wb') as f:
            f.write(data)
    return path


def wav(path, secs=30):
    rate, n = 44100, 44100 * secs
    frames = b''.join(struct.pack('<h', int(8000 * math.sin(i * 440 * 2 * math.pi / rate))) for i in range(rate))
    data = frames * secs
    head = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16)
    return write(path, head + b'data' + struct.pack('<I', len(data)) + data)


def png(path, w=640, h=480, seed=0):
    raw = b''.join(b'\0' + bytes(v for x in range(w) for v in ((x + seed) & 255, y & 255, (x ^ y) & 255)) for y in range(h))
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    return write(path, b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                 chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b''))


files = os.path.join(work, 'files')
os.makedirs(files, exist_ok=True)
small = os.path.join(work, 'small')
os.makedirs(small, exist_ok=True)
for i in range(50):
    write(os.path.join(small, 'file%02d.txt' % i), b'x' * (i * 10))
text_file = write(os.path.join(files, 'notes.txt'), b''.join(b'A line of text, number %d\r\n' % i for i in range(2000)))
tabs = []
for i in range(10):
    tabs.append(write(os.path.join(files, 'tab%02d.cpp' % i),
                      b''.join(b'int f%d(int a) { return a * %d; }  // line %d\r\n' % (i, j, j) for j in range(3000))))
tabs.append(write(os.path.join(files, 'big1.log'), b''.join(b'2026-10-20 12:00:00.%03d INFO something happened here, item %d\n' % (j % 1000, j) for j in range(250000))))
tabs.append(write(os.path.join(files, 'big2.json'), b'[' + b','.join(b'{"id": %d, "name": "item %d", "tags": ["a", "b"]}' % (j, j) for j in range(150000)) + b']'))
song = wav(os.path.join(files, 'tone.wav'))
pics = os.path.join(work, 'pictures')
os.makedirs(pics, exist_ok=True)
first_pic = png(os.path.join(pics, 'pic0000.png'))
blob = open(first_pic, 'rb').read()
for i in range(1, 3000):
    write(os.path.join(pics, 'pic%04d.png' % i), blob)


# --- a launch -------------------------------------------------------------
def launch(args, ini, prefix, ready, timeout=30):
    env = dict(os.environ)
    env['EXPLORERPLUS_SETTINGS'] = ini
    t0 = time.perf_counter()
    p = subprocess.Popen([exe, *args], env=env)
    shown = done = None
    h = None
    end = t0 + timeout
    while time.perf_counter() < end and done is None:
        if h is None:
            h = app_window(p.pid, prefix)
        if h is not None:
            if shown is None and not cloaked(h):
                shown = (time.perf_counter() - t0) * 1000
            if shown is not None and ready(h):
                done = (time.perf_counter() - t0) * 1000
        time.sleep(0.001)
    return p, h, shown, done


def close(p, h):
    if h:
        u.PostMessageW(h, WM_CLOSE, 0, 0)
    try:
        p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def list_ready(n):
    def f(h):
        lst = u.GetDlgItem(h, RES['IDC_LIST'])
        return lst and send(lst, LVM_GETITEMCOUNT) >= n
    return f


def notepad_ready(h):
    for c in children(h):
        if 'RICHEDIT' in cls(c).upper() and u.IsWindowVisible(c) and send(c, WM_GETTEXTLENGTH) > 0:
            return True
    return False


def titled(name):
    return lambda h: name.lower() in title(h).lower()


def scratch(tag):
    d = os.path.join(work, 'ini_' + tag)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, 'settings.ini')


def reset(ini):
    d = os.path.dirname(ini)
    for root, dirs, fs in os.walk(d, topdown=False):
        for f in fs:
            os.remove(os.path.join(root, f))
        for x in dirs:
            os.rmdir(os.path.join(root, x))


def measure(name, prepare, args, prefix, ready):
    shown, done = [], []
    for i in range(runs):
        ini = scratch(name)
        reset(ini)
        prepare(ini)
        p, h, s, d = launch(args, ini, prefix, ready)
        if s is not None: shown.append(s)
        if d is not None: done.append(d)
        time.sleep(0.5)
        close(p, h)
    med = lambda v: '%6.0f ms' % statistics.median(v) if v else '     n/a'
    print('%-14s shown %s   ready %s   (%d runs)' % (name, med(shown), med(done), runs), flush=True)


def nothing(ini):
    pass


def notepad_session(ini):
    # the tabs opened (one process, the others handed to it), then closed: kept as a session
    env = dict(os.environ)
    env['EXPLORERPLUS_SETTINGS'] = ini
    p = subprocess.Popen([exe, '--notepad', tabs[0]], env=env)
    h = None
    for _ in range(400):
        h = app_window(p.pid, 'ExplorerPlusNotepad')
        if h:
            break
        time.sleep(0.025)
    for t in tabs[1:]:
        subprocess.run([exe, '--notepad', t], env=env, timeout=30)
    time.sleep(1.5)
    close(p, h)


def player_queue(ini):
    # a queue of 20,000 entries remembered (an M3U played once, then the player closed)
    m3u = os.path.join(work, 'files', 'many.m3u')
    with open(m3u, 'w', encoding='utf-8') as f:
        f.write('#EXTM3U\n')
        for i in range(20000):
            f.write('#EXTINF:30,Artist %d - Title %d\n%s\n' % (i, i, song))
    env = dict(os.environ)
    env['EXPLORERPLUS_SETTINGS'] = ini
    p = subprocess.Popen([exe, '--play', m3u], env=env)
    h = None
    for _ in range(400):
        h = app_window(p.pid, 'ExplorerPlusPlayer')
        if h:
            break
        time.sleep(0.025)
    time.sleep(3)
    close(p, h)


scenarios = [
    ('main', nothing, [small], 'ExplorerPlusMainWnd', list_ready(50)),
    ('notepad', nothing, ['--notepad', text_file], 'ExplorerPlusNotepad', titled('notes.txt')),
    ('notepad-tabs', notepad_session, ['--notepad', text_file], 'ExplorerPlusNotepad', titled('notes.txt')),
    ('play', nothing, ['--play', song], 'ExplorerPlusPlayer', titled('tone')),
    ('picture', nothing, ['--play', first_pic], 'ExplorerPlusPlayer', titled('pic0000')),
    ('player-queue', player_queue, ['--play', song], 'ExplorerPlusPlayer', titled('tone')),
]
for s in scenarios:
    if not only or s[0] in only:
        measure(*s)
