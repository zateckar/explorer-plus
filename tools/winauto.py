"""Tiny non-invasive automation helpers for Explorer Plus smoke tests.

Everything works through window messages and PrintWindow: the user's mouse and
keyboard focus are never touched.
"""
import ctypes, ctypes.wintypes as W, subprocess, time, os, struct

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

WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH = 0x000C, 0x000D, 0x000E
WM_KEYDOWN, WM_COMMAND, WM_CONTEXTMENU, WM_CANCELMODE, WM_CLOSE = 0x0100, 0x0111, 0x007B, 0x001F, 0x0010
MN_GETHMENU = 0x01E1
SB_GETTEXTW = 0x040D
LVM_GETITEMCOUNT, LVM_GETSELECTEDCOUNT = 0x1004, 0x1032


def text(h):
    n = u.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(n + 1)
    u.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.addressof(buf))
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


def children(h):
    out = []
    def cb(c, _):
        out.append(c)
        return True
    u.EnumChildWindows(h, EnumProc(cb), 0)
    return out


def child_by_id(h, cid):
    return u.GetDlgItem(h, cid)


class App:
    IDC_TREE, IDC_LIST, IDC_ADDRESS, IDC_SEARCH, IDC_STATUS = 1001, 1002, 1003, 1004, 1006

    def __init__(self, exe, *args, settings=None):
        env = dict(os.environ)
        if settings:
            env['EXPLORERPLUS_SETTINGS'] = settings   # never touch the user's settings.ini
        self.proc = subprocess.Popen([exe, *args], env=env)
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
        self.list = child_by_id(self.hwnd, self.IDC_LIST)
        self.tree = child_by_id(self.hwnd, self.IDC_TREE)
        self.address = child_by_id(self.hwnd, self.IDC_ADDRESS)
        self.search_box = child_by_id(self.hwnd, self.IDC_SEARCH)
        self.status = child_by_id(self.hwnd, self.IDC_STATUS)
        self.addr_edit = [c for c in children(self.address) if cls(c) == 'Edit'][0]

    def wait_idle(self, t=1.5):
        time.sleep(t)

    def title(self):
        return text(self.hwnd)

    def address_text(self):
        return text(self.addr_edit)

    def command(self, cid):
        u.SendMessageW(self.hwnd, WM_COMMAND, cid, 0)

    def go(self, path, t=1.5, post=False):
        buf = ctypes.create_unicode_buffer(path)
        u.SendMessageW(self.addr_edit, WM_SETTEXT, 0, ctypes.addressof(buf))
        # post when the app may answer with a modal dialog (SendMessage would block)
        (u.PostMessageW if post else u.SendMessageW)(self.addr_edit, WM_KEYDOWN, 0x0D, 0)
        self.wait_idle(t)

    def item_count(self):
        return u.SendMessageW(self.list, LVM_GETITEMCOUNT, 0, 0)

    def selected_count(self):
        return u.SendMessageW(self.list, LVM_GETSELECTEDCOUNT, 0, 0)

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
            idx = u.SendMessageW(self.list, 0x1053, -1, mem)                     # LVM_FINDITEMW
            # LVITEMW: only state / stateMask matter for LVM_SETITEMSTATE
            def set_state(i, state, mask):
                write(1024, struct.pack('<IiiII', 0, 0, 0, state, mask) + bytes(68))
                u.SendMessageW(self.list, 0x102B, i, mem + 1024)                 # LVM_SETITEMSTATE
            set_state(-1, 0, 0x0003)                                             # clear all
            if idx >= 0:
                set_state(idx, 0x0003, 0x0003)                                   # focused + selected
                u.SendMessageW(self.list, 0x1013, idx, 0)                        # LVM_ENSUREVISIBLE
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
            n = u.SendMessageW(self.list, 0x1073, i, mem)                        # LVM_GETITEMTEXTW
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
            u.SendMessageW(self.list, 0x100E, i, mem)                           # LVM_GETITEMRECT
            buf = ctypes.create_string_buffer(16)
            k.ReadProcessMemory(hp, mem, buf, 16, None)
            return struct.unpack('<iiii', buf.raw)
        finally:
            k.VirtualFreeEx(hp, mem, 0, 0x8000)
            k.CloseHandle(hp)

    def search(self, q):
        """Type into the search box and press Enter (a recursive search of the folder)."""
        buf = ctypes.create_unicode_buffer(q)
        u.SendMessageW(self.search_box, WM_SETTEXT, 0, ctypes.addressof(buf))
        u.SendMessageW(self.search_box, WM_KEYDOWN, 0x0D, 0)

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
                    return h, u.SendMessageW(h, MN_GETHMENU, 0, 0)
        return None, None

    def close_menus(self):
        fly = self.flyout()
        if fly:
            u.PostMessageW(fly, WM_KEYDOWN, 0x1B, 0)  # Esc
        u.SendMessageW(self.hwnd, WM_CANCELMODE, 0, 0)
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
        bmp_path = path + '.bmp'
        with open(bmp_path, 'wb') as f:
            f.write(b'BM' + struct.pack('<IHHI', 14 + 40 + len(buf.raw), 0, 0, 54))
            f.write(bi)
            f.write(buf.raw)
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', bmp_path, path], check=False)
        os.remove(bmp_path)

    def close(self):
        u.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def menu_items(hmenu):
    out = []
    for i in range(u.GetMenuItemCount(hmenu)):
        buf = ctypes.create_unicode_buffer(256)
        u.GetMenuStringW(hmenu, i, buf, 256, 0x400)
        out.append(buf.value.replace('&', ''))
    return out
