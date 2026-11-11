param([Parameter(Mandatory = $true)][int]$ProcessId)

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class WinText {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int max);
}
"@

function Get-Text([IntPtr]$h) {
    $sb = [Text.StringBuilder]::new(2048)
    [void][WinText]::GetWindowTextW($h, $sb, 2048)
    $sb.ToString()
}
function Get-Class([IntPtr]$h) {
    $sb = [Text.StringBuilder]::new(256)
    [void][WinText]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}

$target = [uint32]$ProcessId
$tops = [System.Collections.Generic.List[IntPtr]]::new()
$cb = [WinText+EnumProc] {
    param($hwnd, $p)
    $windowPid = [uint32]0
    [void][WinText]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
    if ($windowPid -eq $target) { $tops.Add($hwnd) }
    return $true
}
[void][WinText]::EnumWindows($cb, [IntPtr]::Zero)

foreach ($top in $tops) {
    Write-Output ("TOP {0} class={1} text={2}" -f ('0x{0:X8}' -f $top.ToInt64()), (Get-Class $top), (Get-Text $top))
    $childCb = [WinText+EnumProc] {
        param($hwnd, $p)
        $t = Get-Text $hwnd
        if ($t) {
            Write-Output ("  CHILD {0} class={1} text={2}" -f ('0x{0:X8}' -f $hwnd.ToInt64()), (Get-Class $hwnd), $t)
        }
        return $true
    }
    [void][WinText]::EnumChildWindows($top, $childCb, [IntPtr]::Zero)
}
