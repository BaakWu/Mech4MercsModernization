[CmdletBinding()]
param(
    [ValidateSet('info')]
    [string]$Action = 'info',
    [int]$ProcessId = 0
)

# Test helper: report the MW4 Mercenaries main window in physical pixels
# (the helper is per-monitor DPI aware, so a 125% desktop does not skew the
# numbers). It never moves the cursor, changes focus, or captures the screen;
# use tools\Send-Mw4Automation.ps1 to drive the game and capture frames from
# inside the process.
$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class Mw4Ui {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int index);
}
"@

[void][Mw4Ui]::SetThreadDpiAwarenessContext([IntPtr](-4))

if ($ProcessId -eq 0) {
    $proc = Get-Process MW4Mercs -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
} else {
    $proc = Get-Process -Id $ProcessId -ErrorAction Stop
}
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw 'MW4Mercs.exe has no main window.' }

$client = New-Object Mw4Ui+RECT
[void][Mw4Ui]::GetClientRect($hwnd, [ref]$client)
$origin = New-Object Mw4Ui+POINT
[void][Mw4Ui]::ClientToScreen($hwnd, [ref]$origin)
$window = New-Object Mw4Ui+RECT
[void][Mw4Ui]::GetWindowRect($hwnd, [ref]$window)
$style = [Mw4Ui]::GetWindowLong($hwnd, -16)

[pscustomobject]@{
    Pid    = $proc.Id
    Client = "$($client.Right)x$($client.Bottom)"
    Origin = "$($origin.X),$($origin.Y)"
    Window = "$($window.Right - $window.Left)x$($window.Bottom - $window.Top)"
    Style  = ('0x{0:X8}' -f $style)
    Popup  = (($style -band 0x80000000) -ne 0)
}
