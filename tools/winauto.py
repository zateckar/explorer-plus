"""Tiny non-invasive automation helpers for Explorer Plus smoke tests.

Everything works through window messages and PrintWindow: the user's mouse and
keyboard focus are never touched.
"""
import ctypes, ctypes.wintypes as W, subprocess, time, os, struct, zlib

u = ctypes.windll.user32
g = ctypes.windll.gdi32
# physical pixels everywhere (window rects, screenshots), whatever the display scaling
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # PER_MONITOR_AWARE_V2
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.GetMenuItemCount.argtypes = [W.HMENU]
u.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, ctypes.c_int]
u.GetMenuStringW.argtypes = [W.HMENU, W.UINT, W.LPWSTR, ctypes.c_int, W.UINT]
u.GetSubMenu.argtypes = [W.HMENU, ctypes.c_int]
u.GetSubMenu.restype = W.HMENU
u.GetMenuItemID.argtypes = [W.HMENU, ctypes.c_int]
u.GetMenuItemID.restype = W.UINT
EnumProc = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

# own handle: its prototypes and last-error stay out of the shared `u`
_send_u = ctypes.WinDLL('user32', use_last_error=True)
_send_u.SendMessageTimeoutW.restype = W.LPARAM
_send_u.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT,
                                        ctypes.POINTER(ctypes.c_size_t)]


class NoAnswer(RuntimeError):
    """The app didn't answer a message in time (hung): the test fails instead of hanging."""


def send(h, msg, wp=0, lp=0, timeout_ms=30000):
    """SendMessage with a deadline (and none at all to a hung window)."""
    res = ctypes.c_size_t()
    if not _send_u.SendMessageTimeoutW(h, msg, wp, lp, 0x0002, timeout_ms, ctypes.byref(res)):   # SMTO_ABORTIFHUNG
        if ctypes.get_last_error() == 1460:                                  # ERROR_TIMEOUT
            raise NoAnswer('window %#x did not answer message %#x within %d ms' % (h or 0, msg, timeout_ms))
        return 0                                                             # (a gone window: as SendMessage)
    v = res.value
    return v - (1 << 64) if v >= 1 << 63 else v                             # LRESULT is signed


def resource_ids(path=None):
    """The app's #define NAME number lines (src/resource.h): command and control IDs."""
    import re
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'resource.h')
    ids = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\s*#define\s+(\w+)\s+(\d+)\b', line)
            if m:
                ids[m.group(1)] = int(m.group(2))
    return ids


RES = resource_ids()


def screen_pixels(x, y, w, h):
    """BGRX bytes (top-down) of a screen rectangle, as shown right now."""
    # own handles on the DLLs: the prototypes set here stay out of the shared u / g
    g, u = ctypes.WinDLL('gdi32'), ctypes.WinDLL('user32')
    g.CreateCompatibleDC.restype = W.HDC
    g.CreateCompatibleBitmap.restype = W.HBITMAP
    g.CreateCompatibleBitmap.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int]
    g.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
    g.SelectObject.restype = W.HGDIOBJ
    g.BitBlt.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.HDC, ctypes.c_int, ctypes.c_int, W.DWORD]
    g.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, ctypes.c_void_p, ctypes.c_void_p, W.UINT]
    g.DeleteObject.argtypes = [W.HGDIOBJ]
    g.DeleteDC.argtypes = [W.HDC]
    u.GetDC.restype = W.HDC
    u.ReleaseDC.argtypes = [W.HWND, W.HDC]
    sdc = u.GetDC(None)
    mem = g.CreateCompatibleDC(sdc)
    bmp = g.CreateCompatibleBitmap(sdc, w, h)
    old = g.SelectObject(mem, bmp)
    g.BitBlt(mem, 0, 0, w, h, sdc, x, y, 0x00CC0020)             # SRCCOPY
    bi = ctypes.create_string_buffer(struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0), 44)
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, bi, 0)
    g.SelectObject(mem, old)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(None, sdc)
    return buf.raw

WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH = 0x000C, 0x000D, 0x000E
WM_KEYDOWN, WM_COMMAND, WM_CONTEXTMENU, WM_CANCELMODE, WM_CLOSE = 0x0100, 0x0111, 0x007B, 0x001F, 0x0010
MN_GETHMENU = 0x01E1
SB_GETTEXTW = 0x040D
LVM_GETITEMCOUNT, LVM_GETSELECTEDCOUNT = 0x1004, 0x1032


def text(h):
    n = send(h, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(n + 1)
    send(h, WM_GETTEXT, n + 1, ctypes.addressof(buf))
    return buf.value


def cls(h):
    buf = ctypes.create_unicode_buffer(64)
    u.GetClassNameW(h, buf, 64)
    return buf.value


def top_windows(pid=None):
    out = []
    def cb(h, _):
        if pid is not None:
            p = W.DWORD()
            u.GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value != pid:
                return True
        out.append(h)
        return True
    u.EnumWindows(EnumProc(cb), 0)
    return out


def windows_of_class(name):
    return [h for h in top_windows() if cls(h) == name and u.IsWindowVisible(h)]


def clipboard_text():
    k = ctypes.windll.kernel32
    k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalLock.argtypes = [ctypes.c_void_p]
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]
    u.GetClipboardData.restype = ctypes.c_void_p
    for _ in range(10):                       # the clipboard may be briefly held by others
        if u.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        return None
    try:
        h = u.GetClipboardData(13)            # CF_UNICODETEXT
        if not h:
            return None
        p = k.GlobalLock(h)
        try:
            return ctypes.wstring_at(p)
        finally:
            k.GlobalUnlock(h)
    finally:
        u.CloseClipboard()


def set_clipboard_text(s):
    """Put s on the clipboard as text; None empties the clipboard."""
    k = ctypes.windll.kernel32
    k.GlobalAlloc.restype = ctypes.c_void_p
    k.GlobalAlloc.argtypes = [W.UINT, ctypes.c_size_t]
    k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalLock.argtypes = [ctypes.c_void_p]
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]
    k.GlobalFree.argtypes = [ctypes.c_void_p]
    u.SetClipboardData.restype = ctypes.c_void_p
    u.SetClipboardData.argtypes = [W.UINT, ctypes.c_void_p]
    for _ in range(10):
        if u.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        return False
    try:
        u.EmptyClipboard()
        if s is None:
            return True
        data = (s + '\0').encode('utf-16-le')
        h = k.GlobalAlloc(0x0002, len(data))          # GMEM_MOVEABLE
        if not h:
            return False
        p = k.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        k.GlobalUnlock(h)
        if not u.SetClipboardData(13, h):             # CF_UNICODETEXT; the system owns h on success
            k.GlobalFree(h)
            return False
        return True
    finally:
        u.CloseClipboard()


def children(h):
    out = []
    def cb(c, _):
        out.append(c)
        return True
    u.EnumChildWindows(h, EnumProc(cb), 0)
    return out


def child_by_id(h, cid):
    return u.GetDlgItem(h, cid)


started = []          # every App launched, for the test's cleanup


class App:
    IDC_TREE, IDC_LIST, IDC_ADDRESS, IDC_SEARCH, IDC_STATUS = (RES[n] for n in (
        'IDC_TREE', 'IDC_LIST', 'IDC_ADDRESS', 'IDC_SEARCH', 'IDC_STATUS'))

    def __init__(self, exe, *args, settings=None):
        env = dict(os.environ)
        if settings:
            env['EXPLORERPLUS_SETTINGS'] = settings   # never touch the user's settings.ini
        self.proc = subprocess.Popen([exe, *args], env=env)
        started.append(self)
        self.hwnd = None
        for _ in range(60):
            for h in top_windows(self.proc.pid):
                if cls(h) == 'ExplorerPlusMainWnd':
                    self.hwnd = h
            if self.hwnd:
                break
            time.sleep(0.25)
        if not self.hwnd:
            raise RuntimeError('main window not found')
        # the main window is findable while it is still creating its controls (a
        # slow runner can look in between): wait until they are all there
        for _ in range(60):
            self.list = child_by_id(self.hwnd, self.IDC_LIST)
            self.tree = child_by_id(self.hwnd, self.IDC_TREE)
            self.address = child_by_id(self.hwnd, self.IDC_ADDRESS)
            self.search_box = child_by_id(self.hwnd, self.IDC_SEARCH)
            self.status = child_by_id(self.hwnd, self.IDC_STATUS)
            edits = [c for c in children(self.address) if cls(c) == 'Edit'] if self.address else []
            if all((self.list, self.tree, self.search_box, self.status)) and edits:
                self.addr_edit = edits[0]
                break
            time.sleep(0.25)
        else:
            raise RuntimeError('main window controls not found')

    def wait_idle(self, t=1.5):
        time.sleep(t)

    def title(self):
        return text(self.hwnd)

    def address_text(self):
        return text(self.addr_edit)

    def command(self, cid):
        send(self.hwnd, WM_COMMAND, cid, 0)

    def go(self, path, t=1.5, post=False):
        buf = ctypes.create_unicode_buffer(path)
        send(self.addr_edit, WM_SETTEXT, 0, ctypes.addressof(buf))
        # post when the app may answer with a modal dialog (SendMessage would block)
        (u.PostMessageW if post else send)(self.addr_edit, WM_KEYDOWN, 0x0D, 0)
        self.wait_idle(t)

    def item_count(self):
        return send(self.list, LVM_GETITEMCOUNT, 0, 0)

    def selected_count(self):
        return send(self.list, LVM_GETSELECTEDCOUNT, 0, 0)

    def status_texts(self):
        # SB_GETTEXTW needs a buffer in the target process: read the window text of
        # part 0 via WM_GETTEXT instead, and parts via screenshot when needed
        return text(self.status)

    def _remote(self, size):
        """(process handle, address) of a zeroed block in the app (list messages need it)."""
        k = ctypes.windll.kernel32
        k.OpenProcess.restype = W.HANDLE
        k.VirtualAllocEx.restype = ctypes.c_void_p
        k.VirtualAllocEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, W.DWORD, W.DWORD]
        k.WriteProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        k.VirtualFreeEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, W.DWORD]
        hp = k.OpenProcess(0x0008 | 0x0010 | 0x0020, False, self.proc.pid)   # VM_OPERATION|READ|WRITE
        mem = k.VirtualAllocEx(hp, None, size, 0x3000, 0x04)                  # COMMIT|RESERVE, RW
        return hp, mem

    def select_name(self, name):
        """Select exactly the item called `name` (case-insensitive), like a click on it."""
        k = ctypes.windll.kernel32
        hp, mem = self._remote(4096)
        try:
            def write(off, data):
                k.WriteProcessMemory(hp, mem + off, data, len(data), None)
            # LVFINDINFOW (x64): flags, psz, lParam, pt, vkDirection -> 40 bytes; the string at +512
            write(512, name.encode('utf-16-le') + b'\0\0')
            write(0, struct.pack('<I4xQQqI4x', 0x0002, mem + 512, 0, 0, 0))     # LVFI_STRING
            idx = send(self.list, 0x1053, -1, mem)                     # LVM_FINDITEMW
            # LVITEMW: only state / stateMask matter for LVM_SETITEMSTATE
            def set_state(i, state, mask):
                write(1024, struct.pack('<IiiII', 0, 0, 0, state, mask) + bytes(68))
                send(self.list, 0x102B, i, mem + 1024)                 # LVM_SETITEMSTATE
            set_state(-1, 0, 0x0003)                                             # clear all
            if idx >= 0:
                set_state(idx, 0x0003, 0x0003)                                   # focused + selected
                send(self.list, 0x1013, idx, 0)                        # LVM_ENSUREVISIBLE
        finally:
            k.VirtualFreeEx(hp, mem, 0, 0x8000)
            k.CloseHandle(hp)
        time.sleep(0.3)
        return idx

    def item_text(self, i, sub=0):
        """Text of row i, column sub (LVM_GETITEMTEXTW through the app's memory)."""
        k = ctypes.windll.kernel32
        k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        hp, mem = self._remote(4096)
        try:
            # LVITEMW: iSubItem at +8, pszText at +24, cchTextMax at +32; the text buffer at +1024
            item = struct.pack('<IiiII4xQi', 0, i, sub, 0, 0, mem + 1024, 1024) + bytes(52)
            k.WriteProcessMemory(hp, mem, item, len(item), None)
            n = send(self.list, 0x1073, i, mem)                        # LVM_GETITEMTEXTW
            buf = ctypes.create_unicode_buffer(1024)
            k.ReadProcessMemory(hp, mem + 1024, buf, 2048, None)
            return buf.value[:max(n, 0)]
        finally:
            k.VirtualFreeEx(hp, mem, 0, 0x8000)
            k.CloseHandle(hp)

    def item_rect(self, i, code=0):
        """(left, top, right, bottom) of row i (LVM_GETITEMRECT; code: LVIR_BOUNDS/ICON/LABEL)."""
        k = ctypes.windll.kernel32
        k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        hp, mem = self._remote(64)
        try:
            k.WriteProcessMemory(hp, mem, struct.pack('<iiii', code, 0, 0, 0), 16, None)
            send(self.list, 0x100E, i, mem)                           # LVM_GETITEMRECT
            buf = ctypes.create_string_buffer(16)
            k.ReadProcessMemory(hp, mem, buf, 16, None)
            return struct.unpack('<iiii', buf.raw)
        finally:
            k.VirtualFreeEx(hp, mem, 0, 0x8000)
            k.CloseHandle(hp)

    def tree_item(self, hitem):
        """(text, image index) of a navigation pane node (TVM_GETITEMW through the app's memory)."""
        k = ctypes.windll.kernel32
        k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        hp, mem = self._remote(2048)
        try:
            # TVITEMW (x64): mask, hItem +8, pszText +24, cchTextMax +32, iImage +36; the text at +512
            item = struct.pack('<I4xQII', 0x0001 | 0x0002 | 0x0010, hitem, 0, 0) + struct.pack('<QiiiiQ', mem + 512, 256, 0, 0, 0, 0)
            k.WriteProcessMemory(hp, mem, item, len(item), None)
            send(self.tree, 0x113E, 0, mem)                          # TVM_GETITEMW
            head = ctypes.create_string_buffer(56)
            k.ReadProcessMemory(hp, mem, head, 56, None)
            image = struct.unpack_from('<i', head.raw, 36)[0]
            buf = ctypes.create_unicode_buffer(256)
            k.ReadProcessMemory(hp, mem + 512, buf, 512, None)
            return buf.value, image
        finally:
            k.VirtualFreeEx(hp, mem, 0, 0x8000)
            k.CloseHandle(hp)

    def tree_nodes(self):
        """Every node the navigation pane shows (expanded branches), in display order."""
        TVM_GETNEXTITEM, TVGN_NEXTVISIBLE, TVGN_FIRSTVISIBLE, TVGN_ROOT = 0x110A, 0x0006, 0x0005, 0x0000
        out = []
        h = send(self.tree, TVM_GETNEXTITEM, TVGN_ROOT, 0)
        while h:
            out.append(h)
            h = send(self.tree, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE, h)
        return out

    def tree_caret(self):
        return send(self.tree, 0x110A, 0x0009, 0)                   # TVGN_CARET

    def tile_columns(self, i):
        """The sub-items a tile shows under item i's name (LVM_GETITEMW with LVIF_COLUMNS)."""
        k = ctypes.windll.kernel32
        k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        hp, mem = self._remote(1024)
        try:
            # LVITEMW (x64): mask, iItem +4, cColumns +56, puColumns +64, piColFmt +72; arrays at +512 / +576
            item = bytearray(88)
            struct.pack_into('<Ii', item, 0, 0x0200, i)                       # LVIF_COLUMNS
            struct.pack_into('<I', item, 56, 4)
            struct.pack_into('<QQ', item, 64, mem + 512, mem + 576)
            k.WriteProcessMemory(hp, mem, bytes(item), len(item), None)
            send(self.list, 0x104B, 0, mem)                          # LVM_GETITEMW
            head = ctypes.create_string_buffer(88)
            k.ReadProcessMemory(hp, mem, head, 88, None)
            n = struct.unpack_from('<I', head.raw, 56)[0]
            cols = ctypes.create_string_buffer(16)
            k.ReadProcessMemory(hp, mem + 512, cols, 16, None)
            return list(struct.unpack('<4I', cols.raw))[:min(n, 4)]
        finally:
            k.VirtualFreeEx(hp, mem, 0, 0x8000)
            k.CloseHandle(hp)

    def search(self, q):
        """Type into the search box and press Enter (a recursive search of the folder)."""
        buf = ctypes.create_unicode_buffer(q)
        send(self.search_box, WM_SETTEXT, 0, ctypes.addressof(buf))
        send(self.search_box, WM_KEYDOWN, 0x0D, 0)

    def flyout(self):
        """The open Win11 compact context menu, or None."""
        return next((h for h in top_windows(self.proc.pid)
                     if cls(h) == 'ExplorerPlusFlyout' and u.IsWindowVisible(h)), None)

    def open_context_flyout(self):
        """Keyboard context menu (Apps key): returns the flyout hwnd or None."""
        u.PostMessageW(self.hwnd, WM_CONTEXTMENU, self.list, 0xFFFFFFFF)
        for _ in range(20):
            time.sleep(0.25)
            h = self.flyout()
            if h:
                return h
        return None

    @staticmethod
    def flyout_entries(h):
        """The flyout's entry texts ('-' for separators), from its window text."""
        buf = ctypes.create_unicode_buffer(4096)
        u.GetWindowTextW(h, buf, 4096)
        return [s for s in buf.value.split('\n') if s]

    def open_keyboard_context_menu(self):
        """The classic menu through the flyout's "Show more options" (last entry);
        returns (menu_hwnd, hmenu) or (None, None)."""
        fly = self.open_context_flyout()
        if not fly:
            return None, None
        u.PostMessageW(fly, WM_KEYDOWN, 0x23, 0)     # End
        u.PostMessageW(fly, WM_KEYDOWN, 0x0D, 0)     # Enter
        for _ in range(20):
            time.sleep(0.25)
            for h in top_windows(self.proc.pid):
                if cls(h) == '#32768' and u.IsWindowVisible(h):
                    return h, send(h, MN_GETHMENU, 0, 0)
        return None, None

    def close_menus(self):
        fly = self.flyout()
        if fly:
            u.PostMessageW(fly, WM_KEYDOWN, 0x1B, 0)  # Esc
        send(self.hwnd, WM_CANCELMODE, 0, 0)
        time.sleep(0.3)

    def screenshot(self, path):
        rc = W.RECT()
        u.GetWindowRect(self.hwnd, ctypes.byref(rc))
        w, h = rc.right - rc.left, rc.bottom - rc.top
        hdc = u.GetDC(self.hwnd)
        mem = g.CreateCompatibleDC(hdc)
        bmp = g.CreateCompatibleBitmap(hdc, w, h)
        g.SelectObject(mem, bmp)
        u.PrintWindow(self.hwnd, mem, 2)  # PW_RENDERFULLCONTENT
        bi = struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        g.GetDIBits(mem, bmp, 0, h, buf, bi, 0)
        g.DeleteObject(bmp); g.DeleteDC(mem); u.ReleaseDC(self.hwnd, hdc)
        # BGRX top-down -> 8-bit RGB PNG (no external tools: CI runners have no ffmpeg)
        bgrx = buf.raw
        rgb = bytearray(w * h * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgrx[2::4], bgrx[1::4], bgrx[0::4]
        stride = w * 3
        raw = b''.join(b'\0' + rgb[y * stride:(y + 1) * stride] for y in range(h))

        def chunk(tag, data):
            return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n'
                    + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                    + chunk(b'IDAT', zlib.compress(raw, 6))
                    + chunk(b'IEND', b''))

    def row_has_text(self, i, code=2):
        """Does row i show text on the screen right now? (the screen's pixels, not
        PrintWindow, which makes the control paint afresh: a stale or overpainted row
        only shows on screen). Text: pixels far from the row's dominant colour."""
        l, t, r, b = self.item_rect(i, code)
        pt = W.POINT(l, t)
        u.ClientToScreen(self.list, ctypes.byref(pt))
        px = screen_pixels(pt.x, pt.y, r - l, b - t)
        colours = {}
        for o in range(0, len(px), 4):
            c = px[o:o + 3]
            colours[c] = colours.get(c, 0) + 1
        bg = max(colours, key=colours.get)
        far = sum(n for c, n in colours.items() if sum(abs(c[k] - bg[k]) for k in range(3)) > 150)
        return far >= 12

    def close(self):
        u.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def icon_pixels(hicon, size=32):
    """BGRA pixels of an icon drawn at size x size (icons are shared across processes)."""
    hdc = u.GetDC(None)
    mem = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, size, size)
    old = g.SelectObject(mem, bmp)
    u.DrawIconEx.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int, W.HICON, ctypes.c_int, ctypes.c_int, W.UINT, W.HBRUSH, W.UINT]
    u.DrawIconEx(mem, 0, 0, hicon, size, size, 0, None, 0x0003)            # DI_NORMAL
    bi = struct.pack('<IiiHHIIiiII', 40, size, -size, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(size * size * 4)
    g.SelectObject(mem, old)
    g.GetDIBits(mem, bmp, 0, size, buf, bi, 0)
    g.DeleteObject(bmp); g.DeleteDC(mem); u.ReleaseDC(None, hdc)
    return buf.raw


def pixel_distance(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / max(1, len(a))


def menu_items(hmenu):
    out = []
    for i in range(u.GetMenuItemCount(hmenu)):
        buf = ctypes.create_unicode_buffer(256)
        u.GetMenuStringW(hmenu, i, buf, 256, 0x400)
        out.append(buf.value.replace('&', ''))
    return out
