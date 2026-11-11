param([Parameter(Mandatory = $true)][int]$ProcessId)

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class WinEnum {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern int GetWindowLongW(IntPtr hWnd, int index);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$target = [uint32]$ProcessId
$rows = [System.Collections.Generic.List[object]]::new()

$callback = [WinEnum+EnumWindowsProc] {
    param($hwnd, $lparam)

    $windowPid = [uint32]0
    [void][WinEnum]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
    if ($windowPid -eq $target) {
        $title = [Text.StringBuilder]::new(512)
        [void][WinEnum]::GetWindowTextW($hwnd, $title, 512)
        $class = [Text.StringBuilder]::new(256)
        [void][WinEnum]::GetClassNameW($hwnd, $class, 256)
        $client = [WinEnum+RECT]::new()
        [void][WinEnum]::GetClientRect($hwnd, [ref]$client)
        $outer = [WinEnum+RECT]::new()
        [void][WinEnum]::GetWindowRect($hwnd, [ref]$outer)
        $style = [WinEnum]::GetWindowLongW($hwnd, -16)

        $rows.Add([pscustomobject]@{
            Handle  = ('0x{0:X8}' -f $hwnd.ToInt64())
            Visible = [WinEnum]::IsWindowVisible($hwnd)
            Class   = $class.ToString()
            Title   = $title.ToString()
            Client  = ('{0}x{1}' -f ($client.Right - $client.Left), ($client.Bottom - $client.Top))
            Window  = ('{0}x{1} at {2},{3}' -f ($outer.Right - $outer.Left), ($outer.Bottom - $outer.Top), $outer.Left, $outer.Top)
            Style   = ('0x{0:X8}' -f $style)
        })
    }
    return $true
}

[void][WinEnum]::EnumWindows($callback, [IntPtr]::Zero)
if ($rows.Count -eq 0) {
    Write-Output "No top-level windows for pid $ProcessId"
} else {
    $rows | Format-Table -AutoSize
}
