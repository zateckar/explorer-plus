<#
.SYNOPSIS
    Prints what screen readers see of Explorer Plus's custom-drawn controls (MSAA:
    name, role, state, shortcut, default action of each part).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/acc_dump.ps1 build-release\explorer-plus.exe C:\Windows

.NOTES
    Windows PowerShell 5.1 (uses the .NET Framework Accessibility assembly). The app runs
    with a scratch settings file (third argument, default: %TEMP%\acc_dump.ini).
#>
param([string]$exe, [string]$folder = $env:SystemRoot, [string]$ini = (Join-Path $env:TEMP 'acc_dump.ini'))
$exe = (Resolve-Path $exe).Path
Add-Type -ReferencedAssemblies Accessibility @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using Accessibility;
public static class Msaa {
    [DllImport("oleacc.dll")]
    static extern int AccessibleObjectFromWindow(IntPtr hwnd, int id, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IAccessible obj);
    [DllImport("oleacc.dll", CharSet = CharSet.Unicode)]
    static extern uint GetRoleTextW(uint role, StringBuilder sb, uint cch);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    static string Role(object r) {
        var sb = new StringBuilder(64);
        GetRoleTextW(Convert.ToUInt32(r), sb, 64);
        return sb.ToString();
    }
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    public static IntPtr Child(IntPtr main, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(main, (h, l) => {
            var sb = new StringBuilder(64); GetClassNameW(h, sb, 64);
            if (sb.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr MainOf(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && FindWindowExW(h, IntPtr.Zero, "ExplorerPlusNavBar", null) != IntPtr.Zero) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Dump(IntPtr hwnd) {
        Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");
        IAccessible acc;
        int hr = AccessibleObjectFromWindow(hwnd, -4, ref iid, out acc);
        if (hr != 0 || acc == null) return "  (no object, hr=" + hr + ")";
        var sb = new StringBuilder();
        sb.AppendFormat("  '{0}' role={1} value='{2}' children={3}\n", acc.get_accName(0), Role(acc.get_accRole(0)),
                        acc.get_accValue(0), acc.accChildCount);
        for (int i = 1; i <= acc.accChildCount; i++) {
            sb.AppendFormat("    {0} '{1}' {2} state=0x{3:X} key='{4}' action='{5}'\n", i, acc.get_accName(i),
                            Role(acc.get_accRole(i)), Convert.ToInt32(acc.get_accState(i)),
                            acc.get_accKeyboardShortcut(i), acc.get_accDefaultAction(i));
        }
        return sb.ToString();
    }
}
"@
$env:EXPLORERPLUS_SETTINGS = $ini
$p = Start-Process -FilePath $exe -ArgumentList "`"$folder`"" -PassThru
Start-Sleep 3
try {
    $main = [Msaa]::MainOf([uint32]$p.Id)
    if ($main -eq [IntPtr]::Zero) { "no main window"; return }
    foreach ($cls in "ExplorerPlusTabStrip", "ExplorerPlusNavBar", "ExplorerPlusAddress", "ExplorerPlusCmdBar", "ExplorerPlusStatus") {
        $cls
        [Msaa]::Dump([Msaa]::Child($main, $cls))
    }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}


