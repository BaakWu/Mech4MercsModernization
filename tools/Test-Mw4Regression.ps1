#Requires -Version 7
<#
.SYNOPSIS
Release regression test (issue #45): installs the release candidate on a fresh
MekTek.rar extraction, plays the real game through the hook's in-process
automation, exercises every patch feature, uninstalls, and prints a
pass/fail/skipped summary.

.DESCRIPTION
One command, no manual steps. It never touches the real cursor, keyboard focus
or desktop: clicks and keys go to the game through [debug] automation=1
(src/hook/automation.cpp). Every step is checked from the hook log (shell
calls, screen script loads, display, throttle and aim traces) or from a frame
capture, so a run is either right or reports exactly where it went wrong.

Order (each phase depends on the previous one):
  1. back up RESOURCE\Pilots (real pilot saves), reset the test install from
     MekTek.rar (Reset-Mw4TestInstall.ps1), install the release candidate with
     MW5 controls and full company on
  2. game session 1: launch from the patch's shortcut; new pilot; Instant Action
     lance (player in a Vulture, 7 Expert lancemates in Archers, Eaton - Merc
     Alley, Regular, Day); mission A (decay and Gyro Aim off): throttle, mouse
     aim point, Gyro Aim off, Vulture twist, Alt+Enter in a mission,
     controllers; campaign: the new pilot's camouflage, campaign MechLab;
     Options (resolution, Windowed, Alt+Enter; Throttle Decay and Gyro Aim on);
     mission B (fresh launch, so the 'Mech starts on its original heading):
     full speed for exactly 40 s, stop, the lancemates finish the mission
     (15 min, up to 3 attempts), then the Gyro Aim and Throttle Decay checks;
     the Instant Action MechLab class filter last
  3. re-run the installer (update keeps the player's choices), uninstall,
     compare with the full 3.1c manifest, put the pilot saves back

Results, logs, frame captures and the installer logs are written to
-OutDir (default artifacts\regression\<time>). The script exits 0 only if
nothing failed.

.EXAMPLE
pwsh ./tools/Test-Mw4Regression.ps1
pwsh ./tools/Test-Mw4Regression.ps1 -Installer dist\MW4Mercs-ModernizationPatch-Setup-1.9.2.exe
#>
[CmdletBinding()]
param(
    # Release candidate (GUI installer). Default: dist\MW4Mercs-ModernizationPatch-Setup-<kToolVersion>.exe
    [string]$Installer = '',
    # Use the command-line build (dist\mw4patch.exe) instead of the GUI installer.
    [switch]$Cli,
    [string]$OutDir = '',
    # Resolution checked in Options > Video and in the missions (above 2048 wide).
    [string]$Resolution = '2560x1080',
    [int]$DriveSeconds = 40,
    [int]$MissionMinutes = 15,
    [int]$MissionAttempts = 3,
    [string]$PilotName = 'regtest',
    # Close a running MW4Mercs.exe instead of refusing to start.
    [switch]$Force,
    # Development: skip reset/install/update/uninstall and drive the game in
    # the current (already patched) install.
    [switch]$GameOnly,
    # Development: skip phases of the game session (Pilot = new pilot,
    # campaign MechLab; Lab = Instant Action MechLab filter).
    [ValidateSet('Pilot', 'Lab')][string[]]$Skip = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 1   # undefined variables only; .Count on one or no result is fine
$repo = Split-Path -Parent $PSScriptRoot
$install = Join-Path $repo 'MekTek\Mechwarrior Mercenaries - Mektek Mekpak'
$started = Get-Date
if (-not $OutDir) { $OutDir = Join-Path $repo ('artifacts\regression\' + $started.ToString('yyyyMMdd-HHmmss')) }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$shotDir = Join-Path $OutDir 'shots'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
$transcript = Join-Path $OutDir 'run.log'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if (-not ('Mw4Test.Win' -as [type])) {
    # No System.Drawing types in here: PowerShell 7 cannot compile against
    # them. Images are read by PowerShell (Read-Pixels) and passed as bytes.
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace Mw4Test {
public static class Win {
    delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    // Visible top-level windows of a process, optionally of one class.
    public static List<IntPtr> Windows(int pid, string cls) {
        var found = new List<IntPtr>();
        EnumWindows((h, p) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == (uint)pid && IsWindowVisible(h)) {
                var sb = new StringBuilder(256);
                GetClassNameW(h, sb, 256);
                if (cls == null || sb.ToString() == cls) found.Add(h);
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Title(IntPtr h) {
        var sb = new StringBuilder(512);
        GetWindowTextW(h, sb, 512);
        return sb.ToString();
    }
    public static string Text(IntPtr h) {
        var sb = new StringBuilder(1 << 20);
        SendMessageW(h, 0x000D, (IntPtr)sb.Capacity, sb);  // WM_GETTEXT
        return sb.ToString();
    }
    public static void Click(IntPtr button) { PostMessageW(button, 0x00F5, IntPtr.Zero, IntPtr.Zero); }  // BM_CLICK
    public static void Close(IntPtr h) { PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }  // WM_CLOSE
    public static bool Checked(IntPtr button) { return SendMessageW(button, 0x00F0, IntPtr.Zero, IntPtr.Zero) == (IntPtr)1; }
}

// 32-bit BGRA pixels, stride = width * 4.
public static class Pix {
    public static int CountColor(byte[] px, int r, int g, int b) {
        int n = 0;
        for (int i = 0; i + 3 < px.Length; i += 4) {
            if (px[i + 2] == r && px[i + 1] == g && px[i] == b) n++;
        }
        return n;
    }
    // Share of non-black pixels in a band along each edge (a picture that
    // fills the window has content up to its edges).
    public static double EdgeContent(byte[] px, int w, int h, int band) {
        int lit = 0, all = 0;
        for (int y = 0; y < h; y += 2) {
            for (int x = 0; x < w; x += 2) {
                if (x >= band && x < w - band && y >= band && y < h - band) continue;
                int i = (y * w + x) * 4;
                all++;
                if (px[i] + px[i + 1] + px[i + 2] > 24) lit++;
            }
        }
        return all == 0 ? 0 : (double)lit / all;
    }
    // Green pixels (the aim line, 40 FF 40, also after scaling) in a
    // rectangle.
    public static int CountGreen(byte[] px, int w, int h, int x0, int y0, int x1, int y1) {
        int n = 0;
        for (int y = Math.Max(0, y0); y < y1 && y < h; y++) {
            for (int x = Math.Max(0, x0); x < x1 && x < w; x++) {
                int i = (y * w + x) * 4;
                int g = px[i + 1]; if (g > 100 && g * 2 > px[i + 2] * 5 && g * 2 > px[i] * 5) n++;
            }
        }
        return n;
    }
    // Pixels that differ inside a rectangle.
    public static int Diff(byte[] a, byte[] b, int w, int h, int x0, int y0, int x1, int y1) {
        if (a.Length != b.Length) return -1;
        int n = 0;
        for (int y = Math.Max(0, y0); y < y1 && y < h; y++) {
            for (int x = Math.Max(0, x0); x < x1 && x < w; x++) {
                int i = (y * w + x) * 4;
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) n++;
            }
        }
        return n;
    }
}
}
'@
}

# An image file as 32-bit BGRA bytes: @{ W; H; Px }.
function Read-Pixels([string]$Path) {
    $bmp = [System.Drawing.Bitmap]::new($Path)
    try {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $bmp.Width, $bmp.Height)
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $px = [byte[]]::new($bmp.Width * $bmp.Height * 4)
        for ($y = 0; $y -lt $bmp.Height; $y++) {
            [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]::Add($data.Scan0, $y * $data.Stride), $px, $y * $bmp.Width * 4, $bmp.Width * 4)
        }
        $bmp.UnlockBits($data)
        return @{ W = $bmp.Width; H = $bmp.Height; Px = $px }
    } finally {
        $bmp.Dispose()
    }
}
function Get-ColorCount([string]$Path, [int]$R, [int]$G, [int]$B) { [Mw4Test.Pix]::CountColor((Read-Pixels $Path).Px, $R, $G, $B) }
# Bright green pixels on the horizontal band through the picture's centre,
# left of the reticle (the aim line runs there while the torso turns left).
function Get-AimLinePixels([string]$Path) {
    $i = Read-Pixels $Path
    $cx = [int]($i.W / 2); $cy = [int]($i.H / 2)
    [Mw4Test.Pix]::CountGreen($i.Px, $i.W, $i.H, 0, $cy - 6, $cx - [int]($i.W * 0.1), $cy + 7)
}
function Get-EdgeContent([string]$Path, [int]$Band) { $i = Read-Pixels $Path; [Mw4Test.Pix]::EdgeContent($i.Px, $i.W, $i.H, $Band) }
function Get-PixelDiff([string]$A, [string]$B, [int[]]$Rect) {
    $p = Read-Pixels $A
    $q = Read-Pixels $B
    [Mw4Test.Pix]::Diff($p.Px, $q.Px, $p.W, $p.H, $Rect[0], $Rect[1], $Rect[2], $Rect[3])
}

# ---------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------
# Every scenario of issue #45, in report order. A scenario that is never
# reached is reported as "not run" (a failure).
$scenarios = [ordered]@{
    'INST-1'  = 'Install on a fresh extraction; imagehlp.dll set aside (#35)'
    'DISP-1'  = 'Launches windowed: framed window, 1024x768 client, centered on the main monitor'
    'CAMO-1'  = "New pilot with a camouflage other than 7-Ops - Pink; campaign MechLab shows it (#19)"
    'LAB-3'   = 'Campaign MechLab: Class filter lists only the classes of the company (#28)'
    'DISP-2'  = "Options > Video resolution $Resolution; missions render at that size and fill the window (#38)"
    'DISP-3'  = 'Windowed checkbox and Alt+Enter switch windowed <-> borderless both ways, menus and mission; saved (#1)'
    'LAB-1'   = 'Instant Action MechLab: each class lists only its band; All restores the full list (#28)'
    'LAB-2'   = 'A chassis picked from a filtered list loads exactly as from the full list (#28)'
    'IA-1'    = 'Instant Action: Mission Play, Eaton - Merc Alley, Regular, Day; player in a Vulture, 7 Expert lancemates in Archers'
    'THR-1'   = 'Throttle Decay off: a W/S throttle holds its value'
    'AIM-1'   = 'Mouse aim point: the torso follows the pointer (#15)'
    'AIM-2'   = 'Green line from the reticle to the aim-point marker while turning (#23)'
    'AIM-3'   = 'Moving the aim point does not turn the legs (#31)'
    'AIM-4'   = 'Torso twist sound not left on: yaw command exactly 0 when settled, Gyro Aim off and on (#32)'
    'AIM-5'   = 'Gyro Aim off: the aim turns with the legs; on: it holds its world direction (#17)'
    'AIM-6'   = 'Gyro Aim on: turning with A/D does not shake the view (#33)'
    'AIM-7'   = "Vulture (360-degree torso): the aim point twists past 90 degrees (#37)"
    'CTRL-1'  = 'Joystick / gamepad listed and usable (skipped if none is plugged in) (#40, #42)'
    'OPT-1'   = 'Options > Game: Throttle Decay and Gyro Aim switch on and are saved'
    'COMP-1'  = "Full company: drive $DriveSeconds s on the start heading, stop; the 7 lancemates finish within $MissionMinutes min (#21)"
    'THR-2'   = 'Throttle Decay on: a W/S throttle eases back to 0 (both directions); presets and X hold (#13)'
    'DISP-4'  = 'Only 32-bit display modes; no 16-bit depth buffer'
    'CRASH-1' = 'No crash dialog, access violation or unexpected exit (#44 watched)'
    'INST-2'  = "Re-running the installer updates in place and keeps the player's Throttle Decay / Gyro Aim choices"
    'INST-3'  = 'Uninstall restores imagehlp.dll and leaves a folder matching the full 3.1c manifest'
}
$results = [ordered]@{}

function Write-Step([string]$Text) {
    $line = '[{0}] {1}' -f (Get-Date).ToString('HH:mm:ss'), $Text
    Write-Host $line
    Add-Content -LiteralPath $transcript -Value $line
}

function Set-Result([string]$Id, [ValidateSet('PASS', 'FAIL', 'SKIP')][string]$Status, [string]$Detail = '') {
    $results[$Id] = [pscustomobject]@{ Id = $Id; Status = $Status; Detail = $Detail }
    Write-Step ("{0,-7} {1}  {2}" -f $Id, $Status, $Detail)
}

# Runs one check; an exception is a FAIL with its message. Returns $true on PASS.
function Test-Scenario([string]$Id, [scriptblock]$Body) {
    try {
        $detail = & $Body
        if ($detail -is [array]) { $detail = $detail -join '; ' }
        if ($detail -is [string] -and $detail.StartsWith('SKIP:')) {
            Set-Result $Id SKIP $detail.Substring(5).Trim()
            return $false
        }
        Set-Result $Id PASS "$detail"
        return $true
    } catch {
        if ($_.Exception.Message -like 'GAME:*') { throw }  # crash: stop the session
        Set-Result $Id FAIL $_.Exception.Message
        return $false
    }
}

function Assert($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# ---------------------------------------------------------------------------
# Game process, log and automation
# ---------------------------------------------------------------------------
$script:gamePid = 0
$script:logPath = ''
$script:crash = ''
foreach ($v in 'company', 'missionB', 'missionBChecked', 'decay', 'gyroOn', 'gyroOff', 'gyroLines', 'zeroOn', 'zeroOff',
    'menuToggle', 'missionToggle', 'missionShot', 'missionRender', 'controlsShot', 'videoSaved', 'lanceShot',
    'missionAResult', 'fullRows', 'driveNote') {
    Set-Variable -Scope Script -Name $v -Value $null
}

function Get-GameProcesses { @(Get-Process MW4Mercs -ErrorAction SilentlyContinue) }

function Stop-Game {
    # Windows Error Reporting restarts a crashed game a few seconds later.
    foreach ($round in 1..3) {
        $p = Get-GameProcesses
        if (-not $p) {
            if ($round -gt 1) { break }
            Start-Sleep -Seconds 1
            continue
        }
        $p | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 4
    }
    Get-Process WerFault -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*MechWarrior*' -or $_.MainWindowTitle -like '*MW4*' } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Read-Log {
    if (-not $script:logPath -or -not (Test-Path -LiteralPath $script:logPath)) { return @() }
    $stream = [IO.File]::Open($script:logPath, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = [IO.StreamReader]::new($stream)
        return @($reader.ReadToEnd() -split "`r?`n")
    } finally {
        $stream.Dispose()
    }
}

function Get-LogMark { @(Read-Log).Count }

function Get-LogSince([int]$Mark) {
    $all = Read-Log
    if ($Mark -ge $all.Count) { return @() }
    return @($all[$Mark..($all.Count - 1)])
}

# Hard failures: a dialog in the game process (crash/assert/error box), the
# game gone, or a crash signature in the log. Throws 'GAME: ...'.
function Assert-GameHealthy {
    if ($script:crash) { throw "GAME: $script:crash" }
    $p = Get-Process -Id $script:gamePid -ErrorAction SilentlyContinue
    if (-not $p) {
        $script:crash = "the game exited unexpectedly (pid $script:gamePid)"
        throw "GAME: $script:crash"
    }
    foreach ($h in [Mw4Test.Win]::Windows($script:gamePid, '#32770')) {
        $title = [Mw4Test.Win]::Title($h)
        $text = [Mw4Test.Win]::Text([Mw4Test.Win]::GetDlgItem($h, 0xFFFF))
        $script:crash = "dialog in the game: [$title] $text"
        Save-Evidence 'crash'
        throw "GAME: $script:crash"
    }
    foreach ($w in @(Get-Process WerFault -ErrorAction SilentlyContinue)) {
        if ($w.MainWindowTitle -match 'MechWarrior|MW4') {
            $script:crash = "Windows Error Reporting: $($w.MainWindowTitle)"
            throw "GAME: $script:crash"
        }
    }
}

function Save-Evidence([string]$Tag) {
    $dir = Join-Path $OutDir 'logs'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Get-ChildItem -LiteralPath $install -Filter 'mw4hook*.log' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dir "$Tag-$($_.Name)") -Force }
}

# Waits for a log line matching $Pattern after $Mark. Returns the line.
function Wait-Log([string]$Pattern, [int]$Mark, [double]$TimeoutSeconds = 20, [switch]$NoThrow) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        Assert-GameHealthy
        $hit = Get-LogSince $Mark | Where-Object { $_ -match $Pattern } | Select-Object -First 1
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    if ($NoThrow) { return $null }
    throw "no log line /$Pattern/ within $TimeoutSeconds s"
}

# Sends one automation batch and waits until the hook has run it.
function Send-Game([string[]]$Commands, [int]$TimeoutSeconds = 300) {
    Assert-GameHealthy
    $out = & (Join-Path $PSScriptRoot 'Send-Mw4Automation.ps1') -Install $install -Command $Commands `
        -ShotDir $shotDir -TimeoutSeconds $TimeoutSeconds 2>&1
    Assert-GameHealthy
    return $out
}

# A frame capture (PNG path); the name is made unique.
$script:shotCount = 0
function Save-Shot([string]$Name) {
    $script:shotCount++
    $file = '{0:D3}-{1}.bmp' -f $script:shotCount, $Name
    Send-Game @("shot $file") | Out-Null
    $png = Join-Path $shotDir ([IO.Path]::ChangeExtension($file, '.png'))
    # The ddraw proxy takes the frame at its next present; it can land just
    # after the automation batch ends.
    $bmp = Join-Path $install $file
    for ($i = 0; $i -lt 20 -and -not (Test-Path -LiteralPath $png); $i++) {
        Start-Sleep -Milliseconds 250
        if (Test-Path -LiteralPath $bmp) {
            Start-Sleep -Milliseconds 300
            $image = [System.Drawing.Image]::FromFile($bmp)
            $image.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
            $image.Dispose()
            Remove-Item -LiteralPath $bmp -Force
        }
    }
    Assert (Test-Path -LiteralPath $png) "frame capture $file missing"
    return $png
}

# Clicks a point (x, y) of the 800x600 menu picture.
function Click([int[]]$P) { @("smove $($P[0]) $($P[1])", 'wait 600', 'click', 'wait 400') }

# Opens a droplist and picks row $Row with the keyboard (Up to the top first,
# then Down; the cursor is not moved, so the highlight follows the keys only).
# The keys go to the droplist that has the focus: until the clicked list has
# dropped, that is the one used before, so the list gets $OpenMs to drop
# (longer for long lists).
#
# -Visible n (the list shows n rows): the row is then clicked instead of
# taking it with Enter. Enter can leave the list dropped, and the next click
# then lands on one of its rows. Rows are 17 px apart, the first 17 px below
# the box. After Up to the top and Down $Row the row is shown at
# min($Row, n - 1).
# -AtTop: scroll the row to the top instead (Down past it, Up back to it).
# The lancemate 'Mech lists need it: their lower rows hang over the next
# lancemate's panel, which can take the click. (Not for the player's list:
# its first row is over the player's variant box.)
function Pick([int[]]$P, [int]$Row, [int]$Rows = 10, [int]$OpenMs = 0, [int]$Visible = 0, [switch]$AtTop) {
    if ($OpenMs -le 0) { $OpenMs = if ($Rows -gt 20) { 1500 } else { 700 } }
    if ($Visible -le 0) {
        return @("smove $($P[0]) $($P[1])", 'wait 400', 'click', "wait $OpenMs", "key 38 10 $($Rows + 2)") +
            $(if ($Row -gt 0) { @("key 40 10 $Row") } else { @() }) + @('key 13 10', 'wait 700')
    }
    if (-not $AtTop) {
        $y = $P[1] + 17 * ([math]::Min($Row, $Visible - 1) + 1)
        return @("smove $($P[0]) $($P[1])", 'wait 400', 'click', "wait $OpenMs", "key 38 10 $($Rows + 2)") +
            $(if ($Row -gt 0) { @("key 40 10 $Row") } else { @() }) + @("smove $($P[0]) $y", 'wait 500', 'click', 'wait 700')
    }
    $down = [math]::Min($Row + $Visible - 1, $Rows - 1)     # highlight below the row
    $top = [math]::Max(0, $down - ($Visible - 1))           # first row shown then
    $up = $down - $Row                                      # back up, no scrolling
    $y = $P[1] + 17 * ($Row - $top + 1)
    @("smove $($P[0]) $($P[1])", 'wait 400', 'click', "wait $OpenMs", "key 38 10 $($Rows + 2)") +
    $(if ($down -gt 0) { @("key 40 10 $down") } else { @() }) +
    $(if ($up -gt 0) { @("key 38 10 $up") } else { @() }) +
    @("smove $($P[0]) $y", 'wait 500', 'click', 'wait 700')
}

# Runs a UI step and waits for its checkpoint; retried (a click can be lost
# while a screen is still loading).
#
# The shell ignores the first click on a screen it has just loaded (the
# menus' first-click problem, AGENTS.md item 6), so each menu step starts
# with a click on the screen's title, which does nothing else. In a mission
# (-NoPrime) a click would fire the weapons.
function Invoke-Ui([string]$Name, [string[]]$Commands, [string]$Expect, [double]$TimeoutSeconds = 12, [int]$Tries = 5, [switch]$NoPrime) {
    $prime = if ($NoPrime) { @() } else { @('smove 400 12', 'wait 400', 'click', 'wait 400') }
    for ($try = 1; $try -le $Tries; $try++) {
        $mark = Get-LogMark
        Send-Game ($prime + $Commands) | Out-Null
        $hit = Wait-Log $Expect $mark $TimeoutSeconds -NoThrow
        if ($hit) { return $hit }
        Write-Step "  $Name : no /$Expect/ (try $try of $Tries)"
        Start-Sleep -Seconds 2
    }
    try { Save-Shot ("stuck-" + ($Name -replace '\W', '_')) | Out-Null } catch { if ($_.Exception.Message -like 'GAME:*') { throw } }
    throw "UI step '$Name' did not reach /$Expect/"
}

function Start-Game {
    Stop-Game
    Get-ChildItem -LiteralPath $install -Filter 'mw4hook-*.log' -ErrorAction SilentlyContinue | Remove-Item -Force
    $shortcut = Join-Path $install 'MechWarrior 4 Mercenaries (Windowed).lnk'
    Assert (Test-Path -LiteralPath $shortcut) "patch shortcut missing: $shortcut"
    Start-Process -FilePath $shortcut | Out-Null
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        foreach ($p in Get-GameProcesses) {
            $log = Join-Path $install "mw4hook-$($p.Id).log"
            if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'auto: automation enabled' -Quiet)) {
                $script:gamePid = $p.Id
                $script:logPath = $log
                $script:crash = ''
                return
            }
        }
    }
    throw 'GAME: the game did not start (no hook log with automation enabled)'
}

# Waits for the first menu after start: the Main Menu, or the Roster when
# there is more than one pilot (then START with the selected pilot).
function Wait-MainMenu([int]$Mark) {
    $hit = Wait-Log 'script: load (MainMenu|PilotEntry)\.script' $Mark 120
    if ($hit -match 'PilotEntry') {
        Start-Sleep -Seconds 3
        Invoke-Ui 'Roster START' (@('activate') + (Click @(678, 561))) 'script: load MainMenu\.script' 15 | Out-Null
    }
    Start-Sleep -Seconds 2
}

# Parsed traces --------------------------------------------------------------
$aimPattern = '^aim: t=(?<t>[-\d.]+) d=(?<dx>[-+\d]+),(?<dy>[-+\d]+) target=(?<ty>[-+\d.]+),(?<tp>[-+\d.]+) ' +
    'torso=(?<y>[-+\d.]+),(?<p>[-+\d.]+) cmd=(?<cy>[-+\d.]+),(?<cp>[-+\d.]+) .*limit=(?<ly>[\d.]+),(?<lp>[\d.]+)' +
    '(?<full> full_turn)?.* heading=(?<h>[-+\d.]+) world=(?<w>[-+\d.]+) gyro=(?<g>\d)(?: cmd0=(?<z>\d))?'
function Get-Aim([string[]]$Lines) {
    foreach ($l in $Lines) {
        if ($l -match $aimPattern) {
            [pscustomobject]@{
                T = [double]$Matches.t; Target = [double]$Matches.ty; Yaw = [double]$Matches.y; Cmd = [double]$Matches.cy
                Limit = [double]$Matches.ly; Full = [bool]$Matches.full; Heading = [double]$Matches.h
                World = [double]$Matches.w; Gyro = [int]$Matches.g; Zero = if ($Matches.z) { [int]$Matches.z } else { -1 }
            }
        }
    }
}
function Get-Throttle([string[]]$Lines) {
    foreach ($l in $Lines) {
        if ($l -match '^controls: t=(?<t>[\d.]+) throttle (?<v>[-+\d.]+) keys (?<k>\S\S)') {
            [pscustomobject]@{ T = [double]$Matches.t; Value = [double]$Matches.v; Keys = $Matches.k }
        }
    }
}
# Throttle traces after the key was let go: the game's "held" flag can read
# off on single frames while the key is held, so the release is the last
# frame that shows the key held.
function Get-AfterRelease([object[]]$Trace, [string]$Keys) {
    $lastHeld = -1
    for ($k = 0; $k -lt $Trace.Count; $k++) { if ($Trace[$k].Keys -eq $Keys) { $lastHeld = $k } }
    if ($lastHeld -lt 0 -or $lastHeld -ge $Trace.Count - 1) { return @() }
    return @($Trace[($lastHeld + 1)..($Trace.Count - 1)])
}
function Get-AngleDiff([double]$A, [double]$B) {
    $d = $A - $B
    while ($d -gt [math]::PI) { $d -= 2 * [math]::PI }
    while ($d -lt -[math]::PI) { $d += 2 * [math]::PI }
    return $d
}

function Get-ClientSize {
    $line = Read-Log | Where-Object { $_ -match 'main window(?: created| \(\w+\))?: client=(\d+)x(\d+)' } | Select-Object -Last 1
    if ($line -match 'client=(\d+)x(\d+)') { return @([int]$Matches[1], [int]$Matches[2]) }
    return @(1024, 768)
}
function Get-RenderSize {
    $line = Read-Log | Where-Object { $_ -match '^gos_SetScreenMode \d+x\d+ -> (\d+)x(\d+)x' } | Select-Object -Last 1
    if ($line -match '-> (\d+)x(\d+)x') { return @([int]$Matches[1], [int]$Matches[2]) }
    return @(1024, 768)
}
# A point of the mission picture, given as an offset from its centre in
# render pixels, as client coordinates (the picture is scaled into the window
# with its aspect kept).
function MissionPoint([int]$Dx, [int]$Dy) {
    $c = Get-ClientSize
    $r = Get-RenderSize
    $scale = [math]::Min($c[0] / $r[0], $c[1] / $r[1])
    $x = [int]($c[0] / 2 + $Dx * $scale)
    $y = [int]($c[1] / 2 + $Dy * $scale)
    return "move $x $y"
}

# A rectangle of the 800x600 menu picture (x0, y0, x1, y1) in a frame
# capture: the picture is scaled into the window with its aspect kept.
function ShotRect([string]$Png, [int[]]$R) {
    $img = [System.Drawing.Image]::FromFile($Png)
    try {
        $scale = [math]::Min($img.Width / 800, $img.Height / 600)
        $ox = ($img.Width - 800 * $scale) / 2
        $oy = ($img.Height - 600 * $scale) / 2
        return @([int]($ox + $R[0] * $scale), [int]($oy + $R[1] * $scale), [int]($ox + $R[2] * $scale), [int]($oy + $R[3] * $scale))
    } finally {
        $img.Dispose()
    }
}

function Get-IniValue([string]$Path, [string]$Section, [string]$Key) {
    $in = $Section -eq ''
    foreach ($l in Get-Content -LiteralPath $Path) {
        if ($l -match '^\s*\[(.+)\]') { $in = $Matches[1] -eq $Section; continue }
        if ($in -and $l -match "^\s*$([regex]::Escape($Key))\s*=\s*(.*)$") { return $Matches[1].Trim() }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Installer (GUI release, or mw4patch.exe with -Cli)
# ---------------------------------------------------------------------------
function Invoke-Installer([ValidateSet('install', 'uninstall')][string]$Action, [string]$Tag) {
    $logFile = Join-Path $OutDir "installer-$Tag.log"
    if ($Cli) {
        $exe = Join-Path $repo 'dist\mw4patch.exe'
        $cliArgs = if ($Action -eq 'install') { @('install', $install, '--yes', '--mw5', '--full-company') } else { @('uninstall', $install, '--yes') }
        $out = & $exe @cliArgs 2>&1
        $code = $LASTEXITCODE
        $out | Set-Content -LiteralPath $logFile
        Assert ($code -eq 0) "mw4patch.exe $Action exited $code (see $logFile)"
        return
    }
    $proc = Start-Process -FilePath $Installer -ArgumentList "--dir `"$install`" --mw5 --full-company" -PassThru
    try {
        $main = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(30)
        while ($main -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 300
            $main = [Mw4Test.Win]::Windows($proc.Id, 'MW4ModernizationPatchSetup') | Select-Object -First 1
            if (-not $main) { $main = [IntPtr]::Zero }
        }
        Assert ($main -ne [IntPtr]::Zero) 'installer window did not open'
        $buttonId = if ($Action -eq 'install') { 108 } else { 109 }
        $button = [Mw4Test.Win]::GetDlgItem($main, $buttonId)
        $deadline = (Get-Date).AddSeconds(30)
        while (-not [Mw4Test.Win]::IsWindowEnabled($button) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        Assert ([Mw4Test.Win]::IsWindowEnabled($button)) "installer: $Action button stays disabled: $([Mw4Test.Win]::Text([Mw4Test.Win]::GetDlgItem($main, 102)))"
        if ($Action -eq 'install') {
            Assert ([Mw4Test.Win]::Checked([Mw4Test.Win]::GetDlgItem($main, 105))) 'installer: MW5 controls not ticked'
            Assert ([Mw4Test.Win]::Checked([Mw4Test.Win]::GetDlgItem($main, 106))) 'installer: full company not ticked'
        }
        [Mw4Test.Win]::Click($button)
        # Uninstall asks first; then a result box ("Installed", "Installation
        # failed" or "Uninstall").
        $message = ''
        $deadline = (Get-Date).AddSeconds(180)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 400
            $dialog = [Mw4Test.Win]::Windows($proc.Id, '#32770') | Select-Object -First 1
            if (-not $dialog) { continue }
            $title = [Mw4Test.Win]::Title($dialog)
            $text = [Mw4Test.Win]::Text([Mw4Test.Win]::GetDlgItem($dialog, 0xFFFF))
            $yes = [Mw4Test.Win]::GetDlgItem($dialog, 6)
            if ($yes -ne [IntPtr]::Zero -and $text -like 'Remove the patch*') {
                [Mw4Test.Win]::Click($yes)
                continue
            }
            $message = "[$title] $text"
            [Mw4Test.Win]::Click([Mw4Test.Win]::GetDlgItem($dialog, 2))  # IDCANCEL also closes an MB_OK box
            [Mw4Test.Win]::Click([Mw4Test.Win]::GetDlgItem($dialog, 1))
            break
        }
        Start-Sleep -Milliseconds 500
        [Mw4Test.Win]::Text([Mw4Test.Win]::GetDlgItem($main, 107)) | Set-Content -LiteralPath $logFile
        Add-Content -LiteralPath $logFile -Value "`r`nResult: $message"
        Assert ($message) "installer: no result after $Action"
        $ok = if ($Action -eq 'install') { $message -like '`[Installed`]*' } else { $message -like '*was removed*' }
        Assert $ok "installer: $message"
    } finally {
        if (-not $proc.HasExited) {
            foreach ($h in [Mw4Test.Win]::Windows($proc.Id, 'MW4ModernizationPatchSetup')) { [Mw4Test.Win]::Close($h) }
            if (-not $proc.WaitForExit(10000)) { $proc | Stop-Process -Force }
        }
    }
}

# Test-only settings on top of what the installer wrote: the in-process
# automation and the intro skip. Both are removed before the update/uninstall.
function Enable-TestMode {
    $ini = Join-Path $install 'mw4hook.ini'
    $text = Get-Content -LiteralPath $ini -Raw
    if ($text -notmatch '(?m)^\[debug\]') {
        Set-Content -LiteralPath $ini -Value ($text.TrimEnd() + "`r`n`r`n[debug]`r`nautomation=1`r`n") -NoNewline
    }
    & (Join-Path $PSScriptRoot 'Set-Mw4IntroSkip.ps1') -Install $install | Out-Null
    if ($GameOnly) {
        # Start from the installer's display settings, as on a fresh install.
        $opts = Join-Path $install 'options.ini'
        $o = Get-Content -LiteralPath $opts
        $o = $o -replace '^screenwidth=.*', 'screenwidth=1024' -replace '^screenheight=.*', 'screenheight=768' -replace '^windowed=.*', 'windowed=true'
        Set-Content -LiteralPath $opts -Value $o -Encoding ascii
    }
}
function Disable-TestMode {
    $ini = Join-Path $install 'mw4hook.ini'
    if (Test-Path -LiteralPath $ini) {
        $text = Get-Content -LiteralPath $ini -Raw
        $text = [regex]::Replace($text, '(?ms)\r?\n*^\[debug\]\r?\n(?:(?!^\[).)*', "`r`n")
        Set-Content -LiteralPath $ini -Value ($text.TrimEnd() + "`r`n") -NoNewline
    }
    & (Join-Path $PSScriptRoot 'Set-Mw4IntroSkip.ps1') -Install $install -Restore | Out-Null
    foreach ($leftover in 'mw4hook-scripts', 'mw4hook-cmd.txt', 'mw4hook-cmd.busy', 'mw4hook-shot.req') {
        $p = Join-Path $install $leftover
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }
}

# ---------------------------------------------------------------------------
# Menu coordinates (800x600 menu picture; see docs/REGRESSION.md)
# ---------------------------------------------------------------------------
$ui = @{
    MainInstantAction = 387, 243; MainOptions = 387, 303; MainRoster = 387, 333; MainCampaign = 387, 364
    RosterNew = 157, 360; RosterJoinKell = 428, 391; RosterStart = 678, 561
    JoinCompany = 273, 310; JoinCamo = 330, 405; JoinCamoRed = 223, 439; JoinOk = 249, 432
    CommandMechLab = 674, 388; CommandMainMenu = 683, 480
    CampaignLabClass = 262, 137; CampaignLabClose = 712, 588
    OptGameTab = 107, 79; OptVideoTab = 252, 79; OptOk = 677, 559
    OptDecayOn = 105, 405; OptGyroOn = 105, 455
    OptResolution = 180, 156; OptWindowed = 279, 155
    IaCommanderMech = 100, 108; IaLaunch = 385, 341; IaMechLab = 680, 548; IaMainMenu = 680, 570
    IaDifficulty = 297, 491; IaTime = 477, 491
    IaLabType = 109, 145; IaLabClass = 262, 145; IaLabClose = 712, 578; IaLabNeutral = 400, 12
    WrapupClose = 659, 567
    ControlsClose = 672, 546
}
# Lancemate slots 1-7: pilot skill droplist, 'Mech droplist.
$lanceSlots = @(
    @(125, 159, 100, 216), @(125, 269, 100, 326), @(125, 379, 100, 435),
    @(695, 49, 670, 105), @(695, 159, 670, 216), @(695, 269, 670, 326), @(695, 379, 670, 435)
)
$resolutionRows = @('640x480', '800x600', '960x600', '1024x768', '1152x768', '1152x864', '1280x720', '1280x768',
    '1280x800', '1280x854', '1280x960', '1280x1024', '1360x768', '1366x768', '1400x1050', '1440x900', '1440x960',
    '1600x900', '1600x1024', '1600x1200', '1680x1050', '1920x1080', '1920x1200', '2560x1080', '2560x1200',
    '2560x1440', '2560x1600', '3440x1440', '3840x2160')

# Pause menu buttons (offsets from the mission picture's centre, render px).
$pause = @{ Resume = -137; Options = -77; Exit = -17; MainMenu = 43; YesDx = -79; YesDy = 84 }

function Get-MechRow([int]$List, [string]$Name) {
    $line = Read-Log | Where-Object { $_ -match "^shell: $List row (\d+) '$([regex]::Escape($Name))'$" } | Select-Object -First 1
    Assert ($line) "'$Name' is not in the 'Mech list ($List)"
    return [int]([regex]::Match($line, 'row (\d+)').Groups[1].Value)
}

# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------
function Invoke-NewPilot {
    Invoke-Ui 'Main Menu > Roster' (@('activate') + (Click $ui.MainRoster)) 'script: load PilotEntry\.script' | Out-Null
    Start-Sleep -Seconds 2
    Send-Game ((Click $ui.RosterNew) + @('wait 800') + (Click $ui.RosterJoinKell) + @('wait 1500')) | Out-Null
    # The pilot name field has the focus.
    $keys = foreach ($ch in $PilotName.ToUpperInvariant().ToCharArray()) { 'key 0x{0:X2} 40' -f [int]$ch }
    Send-Game (@($keys) + (Click $ui.JoinCompany) + @('key 0x51 40', 'key 0x41 40')) | Out-Null
    # Camouflage: the list opens at the top (None, 0-Ops - Blue, 1-Ops - Red,
    # ...) and does not take the Up key, so the Red row is clicked.
    Send-Game ((Click $ui.JoinCamo) + @('wait 800') + (Click $ui.JoinCamoRed) + @('wait 800')) | Out-Null
    $mark = Get-LogMark
    Send-Game (Click $ui.JoinOk) | Out-Null
    Wait-Log "camo: dialog camouflage \d+ '1-Ops - Red'" $mark 10 | Out-Null
    Wait-Log "controls: new pilot '$PilotName' created" $mark 10 | Out-Null
    $mark = Get-LogMark
    Invoke-Ui 'Roster START' (Click $ui.RosterStart) 'script: load MainMenu\.script' 15 | Out-Null
    Wait-Log "controls: new pilot '$PilotName': MW5 layout applied and saved" 0 5 | Out-Null
}

function Invoke-CampaignMechLab {
    Start-Sleep -Seconds 2
    $campaignMark = Get-LogMark
    Invoke-Ui 'Main Menu > Campaign' (Click $ui.MainCampaign) 'script: load CommandCenter\.script' 30 | Out-Null
    $camo = Wait-Log "camo: $PilotName's default camouflage '1'" $campaignMark 20
    Start-Sleep -Seconds 3
    Invoke-Ui 'Command Center > MechLab' (Click $ui.CommandMechLab) 'script: load MechBay\.script' 30 | Out-Null
    Start-Sleep -Seconds 5
    Send-Game @('smove 790 595') | Out-Null
    $shot = Save-Shot 'campaign-mechlab'
    # The first read is the template's '7' (Pink); the hook switches the
    # pilot's default right there, and every later read must be '1'.
    $lines = @(Get-LogSince $campaignMark)
    $switched = [array]::IndexOf($lines, $camo)
    $skins = @($lines[($switched + 1)..($lines.Count - 1)] | Where-Object { $_ -match "camo: skin read .*skin '(.)'" } | ForEach-Object { $Matches[1] } | Select-Object -Unique)
    Test-Scenario 'CAMO-1' {
        Assert ($skins -and -not ($skins -contains '7')) "campaign 'Mechs read skin(s) '$($skins -join ',')' (expected '1', never '7')"
        "new pilot's 'Mechs get '1' (1-Ops - Red): $camo; frame $([IO.Path]::GetFileName($shot))"
    } | Out-Null
    Test-Scenario 'LAB-3' {
        $m = Get-LogMark
        Invoke-Ui 'Class > Light' ((Pick $ui.CampaignLabClass 1 8) + @('wait 1500')) '^mechlab: filter 0, \d+ rows' 12 | Out-Null
        $rows = @(Get-LogSince $m | Where-Object { $_ -match "^mechlab: row '(.+)' class (-?\d+)" })
        $classes = @($rows | ForEach-Object { [int]($_ -replace ".* class ", '') } | Select-Object -Unique)
        Assert ($rows.Count -gt 0) 'no rows reported after choosing a class'
        Assert (@($classes | Where-Object { $_ -ne 0 -and $_ -ge 0 }).Count -eq 0) "Light shows other classes: $($classes -join ',')"
        "Light: $($rows.Count) row(s): $((($rows | ForEach-Object { ($_ -split "'")[1] }) | Select-Object -Unique) -join ', ')"
    } | Out-Null
    try { Invoke-Ui 'Class > All' ((Pick $ui.CampaignLabClass 0 8) + @('wait 1000')) '^mechlab: filter -1, ' 12 | Out-Null } catch { if ($_.Exception.Message -like 'GAME:*') { throw } }
    Invoke-Ui 'MechLab > Close' ((Click $ui.IaLabNeutral) + (Click $ui.CampaignLabClose)) 'script: load CommandCenter\.script' 30 | Out-Null
    Start-Sleep -Seconds 2
    Invoke-Ui 'Command Center > Main Menu' (Click $ui.CommandMainMenu) 'script: load MainMenu\.script' 30 | Out-Null
    Start-Sleep -Seconds 2
}

function Invoke-VideoOptions {
    $row = [array]::IndexOf($resolutionRows, $Resolution)
    Assert ($row -ge 0) "$Resolution is not in the test's resolution table"
    $w, $h = $Resolution -split 'x'
    Invoke-Ui 'Main Menu > Options' (Click $ui.MainOptions) 'script: load OM_Multi\.script' 20 | Out-Null
    Start-Sleep -Seconds 2
    # All tabs load with the Options screen; the tab click has no checkpoint
    # of its own, so it is repeated with the resolution pick.
    Invoke-Ui 'Video > Resolution' ((Click $ui.OptVideoTab) + @('wait 1500') + (Pick $ui.OptResolution $row 32)) "display: script sets Resolution = index $row`$" 10 | Out-Null
    # Windowed off from the checkbox: borderless.
    $mark = Get-LogMark
    $off = Invoke-Ui 'Video > Windowed off' ((Click $ui.OptWindowed) + @('wait 2500')) 'display: windowed -> 0 \(borderless' 10
    $borderless = Wait-Log 'main window \(borderless\): client=' $mark 5 -NoThrow
    $shotOff = Save-Shot 'menu-borderless'
    $mark = Get-LogMark
    Invoke-Ui 'Options OK' (Click $ui.OptOk) 'script: load MainMenu\.script' 20 | Out-Null
    $opts = Join-Path $install 'options.ini'
    $savedW = Get-IniValue $opts 'graphics options' 'screenwidth'
    $savedH = Get-IniValue $opts 'graphics options' 'screenheight'
    $savedWin = Get-IniValue $opts 'graphics options' 'windowed'
    # Alt+Enter in the menus: framed again.
    $mark = Get-LogMark
    Send-Game @('altenter', 'wait 2500', 'activate') | Out-Null
    $on = Wait-Log 'display: windowed -> 1 \(framed' $mark 10 -NoThrow
    $savedWin2 = Get-IniValue $opts 'graphics options' 'windowed'
    $script:menuToggle = [pscustomobject]@{
        Off = $off; Borderless = $borderless; On = $on; SavedOff = $savedWin; SavedOn = $savedWin2; Shot = $shotOff
    }
    $script:videoSaved = "$savedW x $savedH"
    Assert ($savedW -eq $w -and $savedH -eq $h) "options.ini has ${savedW}x${savedH} after OK (expected $Resolution)"
}

function Invoke-GameOptions([bool]$Decay, [bool]$Gyro) {
    Invoke-Ui 'Main Menu > Options' (Click $ui.MainOptions) 'script: load OM_Multi\.script' 20 | Out-Null
    Start-Sleep -Seconds 2
    $mark = Get-LogMark
    # Each switch reports itself as it is clicked.
    $decayAt = if ($Decay) { $ui.OptDecayOn } else { @(($ui.OptDecayOn[0] + 100), $ui.OptDecayOn[1]) }
    $gyroAt = if ($Gyro) { $ui.OptGyroOn } else { @(($ui.OptGyroOn[0] + 100), $ui.OptGyroOn[1]) }
    Invoke-Ui 'Game > Throttle Decay' ((Click $decayAt) + @('wait 500')) "^controls: throttle decay $(if ($Decay) { 'on' } else { 'off' })" 8 | Out-Null
    Invoke-Ui 'Game > Gyro Aim' ((Click $gyroAt) + @('wait 500')) "^aim: gyro aim $(if ($Gyro) { 'on' } else { 'off' })" 8 | Out-Null
    Invoke-Ui 'Options OK' (Click $ui.OptOk) 'script: load MainMenu\.script' 20 | Out-Null
    $lines = Get-LogSince $mark
    return [pscustomobject]@{
        Decay = ($lines | Where-Object { $_ -match '^controls: throttle decay (on|off)' } | Select-Object -Last 1)
        Gyro  = ($lines | Where-Object { $_ -match '^aim: gyro aim (on|off)' } | Select-Object -Last 1)
        IniDecay = Get-IniValue (Join-Path $install 'mw4hook.ini') 'controls' 'decay'
        IniGyro  = Get-IniValue (Join-Path $install 'mw4hook.ini') 'aim' 'gyro'
    }
}

function Open-InstantAction {
    $mark = Get-LogMark
    Invoke-Ui 'Main Menu > Instant Action' (Click $ui.MainInstantAction) '^shell: 1 ->' 20 | Out-Null
    Wait-Log '^shell: 114 rows \d+' 0 15 | Out-Null
    Start-Sleep -Seconds 2
    return $mark
}

function Invoke-LanceSetup([switch]$Fresh) {
    $openMark = Open-InstantAction
    $mission = Get-LogSince $openMark | Where-Object { $_ -match "^shell: 71 ->" } | Select-Object -Last 1
    Assert ($mission -match "'Eaton - Merc Alley'") "Instant Action map is not Eaton - Merc Alley: $mission"
    $commanderRows = [int]([regex]::Match((Read-Log | Where-Object { $_ -match '^shell: 4 rows \d+' } | Select-Object -Last 1), 'rows (\d+)').Groups[1].Value)
    $mateRows = [int]([regex]::Match((Read-Log | Where-Object { $_ -match '^shell: 114 rows \d+' } | Select-Object -Last 1), 'rows (\d+)').Groups[1].Value)
    # The player in a Vulture (the Vulture twist check, issue #37), the
    # lancemates in Archers (near the top of their list).
    $vulture = Get-MechRow 4 'Vulture'
    $archer = Get-MechRow 114 'Archer'
    $mark = $openMark
    # Each setting is one step: open the droplist, scroll with the keys, click
    # the row (-Visible), then check the shell call it makes. If the row click
    # did not register, the list is still open with the keys' highlight on
    # the row, so Enter takes it. Otherwise the step is repeated.
    # Difficulty and time are only set (shell 74 / 7) when they change; the
    # screen reads them when it opens (73 / 70).
    $steps = [System.Collections.Generic.List[object]]::new()
    $steps.Add(@{ Name = 'player Vulture'; Pattern = '^shell: 5 ->'; Good = "'Vulture'"; Pick = { Pick $ui.IaCommanderMech $vulture $commanderRows -Visible 8 } })
    $steps.Add(@{ Name = 'difficulty Regular'; Since = 0; Pattern = '^shell: 7[34] ->'; Good = ' 1$'; Pick = { Pick $ui.IaDifficulty 1 6 -Visible 4 } })
    $steps.Add(@{ Name = 'time Day'; Since = 0; Pattern = '^shell: (7|70) ->'; Good = ' 0$'; Pick = { Pick $ui.IaTime 0 4 -Visible 2 } })
    for ($slot = 0; $slot -lt 7; $slot++) {
        $s = $lanceSlots[$slot]
        # Skill: (empty), Normal, Veteran, Expert. The 'Mech list is filled
        # once a skill is chosen.
        # -Fresh: the lancemates are cleared once the session has left
        # Instant Action for the campaign (runs 21/22: mission B opened with
        # 7 empty slots), so only what was set on this visit counts.
        $since = if ($Fresh) { $openMark } else { 0 }
        $steps.Add(@{ Name = "lancemate $($slot + 1) Expert"; Since = $since; Pattern = "^shell: 55 ->\S+ $slot "; Good = " $slot 3$"; Pick = [scriptblock]::Create("Pick @($($s[0]), $($s[1])) 3 6 -Visible 4") })
        $steps.Add(@{ Name = "lancemate $($slot + 1) Archer"; Since = $since; Pattern = "^shell: 56 ->\S+ $slot .*'"; Good = "'Archer'$"; Pick = [scriptblock]::Create("Pick @($($s[2]), $($s[3])) $archer $mateRows -Visible 8 -AtTop") })
    }
    # A setting is only reported when it changes (difficulty and time also
    # when the screen first opens), so the whole log is read unless a step
    # says otherwise (Since).
    $isSet = {
        param($step)
        $from = if ($step.Since) { $step.Since } else { 0 }
        # [string]: no match must read as "", not as an empty collection.
        ([string](Get-LogSince $from | Where-Object { $_ -match $step.Pattern } | Select-Object -Last 1)) -match $step.Good
    }
    # A click on the title first: the screen ignores its first click.
    Send-Game @('smove 400 12', 'wait 400', 'click', 'wait 600') | Out-Null
    for ($pass = 1; $pass -le 2; $pass++) {
        foreach ($step in $steps) {
            for ($try = 1; -not (& $isSet $step) -and $try -le 3; $try++) {
                $m = Get-LogMark
                Send-Game ((& $step.Pick) + @('wait 800')) 300 | Out-Null
                if (Wait-Log $step.Pattern $m 4 -NoThrow) { continue }
                # The row click was not taken: the list is open on the row.
                Send-Game @('key 13 10', 'wait 1000') | Out-Null
                if (Wait-Log $step.Pattern $m 4 -NoThrow) { continue }
                Write-Step "  lance: $($step.Name) not set (try $try of 3)"
            }
        }
        # A later click can land in a list left open and change an earlier
        # setting: go round once more for anything that is no longer right.
        if (-not ($steps | Where-Object { -not (& $isSet $_) })) { break }
        Write-Step "  lance: $(($steps | Where-Object { -not (& $isSet $_) } | ForEach-Object Name) -join ', ') changed; setting again"
    }
    $lines = Read-Log
    $last = { param($pattern) $lines | Where-Object { $_ -match $pattern } | Select-Object -Last 1 }
    $session = $lines
    $diff = $session | Where-Object { $_ -match '^shell: 7[34] ->' } | Select-Object -Last 1
    $time = $session | Where-Object { $_ -match '^shell: (7|70) ->' } | Select-Object -Last 1
    $player = & $last '^shell: 5 ->'
    Assert ($diff -match ' 1$') "difficulty not Regular: $diff"
    Assert ($time -match ' 0$') "time of day not Day: $time"
    Assert ($player -match "'Vulture'") "player 'Mech: $player"
    for ($slot = 0; $slot -lt 7; $slot++) {
        $skill = & $last "^shell: 55 ->\S+ $slot "
        $mech = & $last "^shell: 56 ->\S+ $slot .*'"
        Assert ($skill -match " $slot 3$") "lancemate $($slot + 1) skill: $skill"
        Assert ($mech -match "'Archer'$") "lancemate $($slot + 1) 'Mech: $mech"
    }
    Send-Game @('smove 790 595') | Out-Null
    $script:lanceShot = Save-Shot 'instant-action-lance'
}

function Start-Mission {
    $mark = Get-LogMark
    # Retried: run 22 lost the LAUNCH click (no shell read-back, no load).
    # A click that was taken loads Loading.script at once, so a retry after
    # 20 s cannot launch twice.
    Invoke-Ui 'Instant Action > LAUNCH' (Click $ui.IaLaunch) 'script: load Loading\.script' 20 3 | Out-Null
    $company = Wait-Log 'campaign: mission \d+ lancemates \d+ -> 7' $mark 30 -NoThrow
    Wait-Log '^gos_SetScreenMode \d+x\d+ -> (?!800x600)\d+x\d+x\d+' $mark 120 | Out-Null
    # Running: aim traces with the mech's twist limits.
    Wait-Log '^aim: t=.* limit=' $mark 120 | Out-Null
    Start-Sleep -Seconds 4
    Send-Game @('activate') | Out-Null
    return [pscustomobject]@{ Mark = $mark; Company = $company }
}

# Exit Mission from the pause menu, then close the results screen. Returns
# the mission's result line (shell 88).
function Exit-Mission {
    $mark = Get-LogMark
    Invoke-Ui 'Esc' @('key 27 100', 'wait 1500') 'script: load PauseModal' 10 -NoPrime | Out-Null
    $result = Invoke-Ui 'Pause > Exit Mission > Yes' @((MissionPoint 0 $pause.Exit), 'wait 600', 'click', 'wait 1500', (MissionPoint $pause.YesDx $pause.YesDy), 'wait 600', 'click') '^shell: 88 ->' 30 -NoPrime
    Start-Sleep -Seconds 3
    Invoke-Ui 'Results > Close' (Click $ui.WrapupClose) '^shell: 1 ->' 30 | Out-Null
    Start-Sleep -Seconds 2
    return $result
}

function Get-StartHeading([int]$Mark) {
    $first = Get-Aim (Get-LogSince $Mark) | Where-Object { $_.Heading -ne 0 } | Select-Object -First 1
    Assert ($first) 'no aim trace with a heading after the mission start'
    return $first.Heading
}

# Mission A: decay off, Gyro Aim off. -----------------------------------------
function Invoke-MissionA {
    $m = Start-Mission
    $missionMark = $m.Mark
    $script:company = $m.Company

    Test-Scenario 'THR-1' {
        $mark = Get-LogMark
        Send-Game @('key 87 1500', 'wait 2500') | Out-Null
        $t = @(Get-Throttle (Get-LogSince $mark))
        Assert ($t.Count -gt 0) 'W moved no throttle (no controls: lines)'
        $peak = ($t | Measure-Object Value -Maximum).Maximum
        $after = Get-AfterRelease $t 'W-'
        # Owner 2026-11-26: the checks prove the throttle moves and then holds
        # (decay off), not how far a timed key press ramps it. A decaying
        # throttle drops far more than 0.02 within the 2.5 s wait.
        Assert ($peak -gt 0.5) "W reached only $peak"
        Assert (-not ($after | Where-Object { [math]::Abs($_.Value - $peak) -gt 0.02 })) "forward throttle changed after release: $(($after | ForEach-Object Value) -join ', ')"
        Send-Game @('key 88 100', 'wait 800') | Out-Null
        $mark = Get-LogMark
        Send-Game @('key 83 600', 'wait 2500') | Out-Null
        $t = @(Get-Throttle (Get-LogSince $mark))
        $low = ($t | Measure-Object Value -Minimum).Minimum
        $after = Get-AfterRelease $t '-S'
        Assert ($low -lt -0.1) "S reached only $low"
        Assert (-not ($after | Where-Object { [math]::Abs($_.Value - $low) -gt 0.02 })) "reverse throttle changed after release: $(($after | ForEach-Object Value) -join ', ')"
        $mark = Get-LogMark
        Send-Game @('key 88 100', 'wait 800') | Out-Null
        $stop = @(Get-Throttle (Get-LogSince $mark)) | Select-Object -Last 1
        Assert ($stop -and [math]::Abs($stop.Value) -lt 0.01) 'X did not stop the throttle'
        # The 100 % preset (MW5 layout: Numpad 0) that mission B drives with.
        $mark = Get-LogMark
        Send-Game @('key 0x60 100', 'wait 1500') | Out-Null
        $preset = @(Get-Throttle (Get-LogSince $mark)) | Select-Object -Last 1
        Send-Game @('key 88 100', 'wait 800') | Out-Null
        Assert ($preset -and $preset.Value -ge 0.95) "Numpad 0 (100 % preset) set the throttle to $(if ($preset) { $preset.Value } else { 'nothing' })"
        "W -> $peak held; S -> $low held; X -> 0; Numpad 0 -> $($preset.Value)"
    } | Out-Null

    $heading0 = Get-StartHeading $missionMark
    $mark = Get-LogMark
    Send-Game @('mouse -1500 0 300') | Out-Null
    $turning = Save-Shot 'aim-turning'
    Send-Game @('wait 6000') | Out-Null
    $settled = Save-Shot 'aim-settled'
    Send-Game @('wait 1000') | Out-Null
    $aim = @(Get-Aim (Get-LogSince $mark))
    Test-Scenario 'AIM-1' {
        Assert ($aim.Count -gt 10) 'no aim traces'
        $last = $aim[-1]
        # Owner 2026-11-26: proves the torso follows the pointer, not the
        # exact settle point (runs see 1.74 rad, torso within 0.0005).
        Assert ([math]::Abs($last.Target) -gt 0.3) "aim point moved only to $($last.Target) rad"
        Assert ([math]::Abs((Get-AngleDiff $last.Yaw $last.Target)) -lt 0.05) "torso $($last.Yaw) did not reach the aim point $($last.Target)"
        "aim point $($last.Target) rad, torso $($last.Yaw) rad"
    } | Out-Null
    Test-Scenario 'AIM-2' {
        $line = Get-AimLinePixels $turning
        $rest = Get-AimLinePixels $settled
        # Runs see 410 while turning and 0 settled; the line is there or not.
        Assert ($line -ge 50) "while turning only $line line-green pixels ($([IO.Path]::GetFileName($turning)))"
        Assert ($rest -lt $line / 2) "line still drawn after the torso reached the aim point ($rest pixels)"
        "line pixels while turning $line, settled $rest"
    } | Out-Null
    Test-Scenario 'AIM-3' {
        # A legs turn from aiming (#31) is tenths of a radian; 0.02 leaves room
        # for a nudge from a lancemate.
        $moved = $aim | Where-Object { [math]::Abs((Get-AngleDiff $_.Heading $heading0)) -gt 0.02 }
        Assert (-not $moved) "the legs turned while aiming: heading $heading0 -> $(($moved | Select-Object -First 1).Heading)"
        "heading stayed $heading0"
    } | Out-Null
    # AIM-4 samples: the last 5 frames of each settle (15 could reach back
    # into the last moments of the turn).
    $script:zeroOff = @($aim | Select-Object -Last 5)

    # Gyro Aim off: holding D turns the legs and the aim with them.
    $mark = Get-LogMark
    Send-Game @('key 68 3000', 'wait 1500') | Out-Null
    $turn = @(Get-Aim (Get-LogSince $mark))
    $script:gyroOff = $turn

    # Vulture: 30000 counts left; the torso twists far past 90 degrees.
    $mark = Get-LogMark
    Send-Game @('mouse -30000 0 2000', 'wait 9000') | Out-Null
    $spin = @(Get-Aim (Get-LogSince $mark))
    Test-Scenario 'AIM-7' {
        Assert ($spin.Count -gt 10) 'no aim traces'
        Assert ($spin[0].Full -and $spin[0].Limit -gt 6.2) "twist limit $($spin[0].Limit) (expected 2 pi, full_turn) - is the player in a Vulture?"
        $total = 0.0
        for ($k = 1; $k -lt $spin.Count; $k++) { $total += Get-AngleDiff $spin[$k].Yaw $spin[$k - 1].Yaw }
        Assert ([math]::Abs($total) -gt [math]::PI / 2 + 0.1)("the torso twisted only {0:N2} rad" -f $total)
        "limit $($spin[0].Limit) full_turn; torso twisted {0:N2} rad ({1:N0} degrees)" -f $total, ($total * 180 / [math]::PI)
    } | Out-Null
    $script:zeroOff += @($spin | Select-Object -Last 5)

    # Alt+Enter in the mission: borderless and back.
    $mark = Get-LogMark
    Send-Game @('altenter', 'wait 3000', 'activate') | Out-Null
    $missionBorderless = Save-Shot 'mission-borderless'
    Send-Game @('altenter', 'wait 3000', 'activate') | Out-Null
    $missionFramed = Save-Shot 'mission-framed'
    $lines = Get-LogSince $mark
    $script:missionToggle = [pscustomobject]@{
        Off = $lines | Where-Object { $_ -match 'Alt\+Enter: Windowed -> 0' } | Select-Object -First 1
        On  = $lines | Where-Object { $_ -match 'Alt\+Enter: Windowed -> 1' } | Select-Object -First 1
        Framed = $missionFramed; Borderless = $missionBorderless
    }
    $script:missionShot = $missionFramed
    $script:missionRender = Get-RenderSize

    # Controllers: Pause > Options shows the Controls page.
    $script:controlsShot = $null
    $mark = Get-LogMark
    Invoke-Ui 'Esc' @('key 27 100', 'wait 1500') 'script: load PauseModal' 10 -NoPrime | Out-Null
    Invoke-Ui 'Pause > Options' @((MissionPoint 0 $pause.Options), 'wait 600', 'click', 'wait 1500') 'script: load OM_Controls\.script' 10 -NoPrime | Out-Null
    Start-Sleep -Seconds 2
    $script:controlsShot = Save-Shot 'mission-controls'
    Invoke-Ui 'Controls > Close' (Click $ui.ControlsClose) 'script: load PauseModal' 10 | Out-Null
    Send-Game @((MissionPoint 0 $pause.Resume), 'wait 300', 'click', 'wait 1500') | Out-Null

    $script:missionAResult = Exit-Mission
}

# Mission B: decay and Gyro Aim on; the full-company run. ---------------------
function Invoke-MissionB {
    $attempts = @()
    $script:missionBChecked = $false
    for ($attempt = 1; $attempt -le $MissionAttempts; $attempt++) {
        if ($attempt -gt 1) {
            Invoke-Ui 'Instant Action > Main Menu' (Click $ui.IaMainMenu) 'script: load MainMenu\.script' 20 | Out-Null
            Start-Sleep -Seconds 2
        }
        Invoke-LanceSetup -Fresh
        $m = Start-Mission
        $heading0 = Get-StartHeading $m.Mark
        $started = Get-Date
        # Full speed for $DriveSeconds s: the 100 % preset (MW5 layout:
        # Numpad 0), which holds whatever Throttle Decay does, then X.
        # The first key press can be lost while the mission is still starting
        # (run 20261126-134847: neither Numpad 0 nor X logged a controls:
        # line), so Numpad 0 is pressed again until the throttle shows full.
        $mark = Get-LogMark
        $full = $null
        for ($press = 1; $press -le 5 -and -not $full; $press++) {
            $pm = Get-LogMark
            Send-Game @('key 0x60 100', 'wait 1500') | Out-Null
            $full = @(Get-Throttle (Get-LogSince $pm)) | Where-Object { $_.Value -ge 0.99 } | Select-Object -First 1
            if (-not $full) { Write-Step "  drive: Numpad 0 not taken (press $press of 5)" }
        }
        Send-Game @("wait $($DriveSeconds * 1000)") ($DriveSeconds + 60) | Out-Null
        $drive = Get-LogSince $mark
        Send-Game @('key 88 100', 'wait 1000') | Out-Null
        # The drive only takes the player toward the fight; COMP-1's intent is
        # that the lancemates win. Owner 2026-11-26: a drive that was not
        # perfectly straight or full is noted, never fatal, so the Gyro Aim
        # and decay checks below still run. (AIM-3 / AIM-5 check the legs.)
        $t = @(Get-Throttle $drive)
        $a = @(Get-Aim $drive)
        $drop = $t | Where-Object { $full -and $_.T -gt $full.T -and $_.Value -lt 0.9 }
        $off = $a | Where-Object { [math]::Abs((Get-AngleDiff $_.Heading $heading0)) -gt 0.05 }
        $script:driveNote = if (-not $full) { 'drive: throttle never reached full (Numpad 0)' }
                     elseif ($drop) { "drive: throttle dropped to $(($drop | Select-Object -First 1).Value)" }
                     elseif ($off) { "drive: heading left the start heading ($heading0 -> $(($off | Select-Object -First 1).Heading))" }
                     else { '' }
        if ($script:driveNote) { Write-Step "  $script:driveNote (noted, not a failure)" }

        if (-not $script:missionBChecked) {
            $script:missionBChecked = $true
            Invoke-MissionBChecks
        }

        # Wait for the lancemates (results screen = shell 88).
        $deadline = $started.AddMinutes($MissionMinutes)
        $result = $null
        while (-not $result -and (Get-Date) -lt $deadline) {
            $left = ($deadline - (Get-Date)).TotalSeconds
            $result = Wait-Log '^shell: 88 ->' $m.Mark ([math]::Min(30, [math]::Max(1, $left))) -NoThrow
        }
        $elapsed = (Get-Date) - $started
        if ($result) {
            $won = $result -match "^shell: 88 ->\S+ '[^']*' '[^']*' -?\d+ -?\d+ (\d+)" -and [int]$Matches[1] -gt 0
            $attempts += [pscustomobject]@{ Attempt = $attempt; Result = $(if ($won) { 'complete' } else { 'failed' }); Minutes = [math]::Round($elapsed.TotalMinutes, 1); Line = $result }
            Start-Sleep -Seconds 3
            Invoke-Ui 'Results > Close' (Click $ui.WrapupClose) '^shell: 1 ->' 30 | Out-Null
            Start-Sleep -Seconds 2
            if ($won) { break }
        } else {
            $attempts += [pscustomobject]@{ Attempt = $attempt; Result = 'timeout'; Minutes = [math]::Round($elapsed.TotalMinutes, 1); Line = '' }
            Save-Shot "mission-timeout-$attempt" | Out-Null
            Exit-Mission | Out-Null
        }
    }
    $script:missionB = $attempts
}

# Gyro Aim on / Throttle Decay on checks, after the drive (the start heading
# no longer matters).
function Invoke-MissionBChecks {
    # Gyro Aim on: A held turns the legs; the aim holds its world direction.
    Send-Game @('mouse 400 0 200', 'wait 3000') | Out-Null
    $mark = Get-LogMark
    Send-Game @('key 65 3000', 'wait 2000') | Out-Null
    $lines = Get-LogSince $mark
    $script:gyroOn = @(Get-Aim $lines)
    $script:gyroLines = @($lines | Where-Object { $_ -match '^gyro: .* drawn_world=([-+\d.]+)' })
    $script:zeroOn = @($script:gyroOn | Select-Object -Last 5)

    # The mission at the Options > Video resolution (DISP-2).
    Send-Game @('wait 500') | Out-Null
    $script:missionShot = Save-Shot 'mission-b'

    # Throttle Decay on.
    $decay = [ordered]@{}
    $mark = Get-LogMark
    Send-Game @('key 83 600', 'wait 2500') | Out-Null
    $decay.Reverse = @(Get-Throttle (Get-LogSince $mark))
    $mark = Get-LogMark
    Send-Game @('key 87 600', 'wait 2500') | Out-Null
    $decay.Forward = @(Get-Throttle (Get-LogSince $mark))
    $mark = Get-LogMark
    Send-Game @('key 0x65 100', 'wait 3000') | Out-Null   # Numpad 5: 50 %
    $decay.Preset = @(Get-Throttle (Get-LogSince $mark))
    $mark = Get-LogMark
    Send-Game @('key 88 100', 'wait 2500') | Out-Null    # X: full stop
    $decay.Stop = @(Get-Throttle (Get-LogSince $mark))
    $script:decay = $decay
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
$userPilots = Join-Path $OutDir 'user-pilots'
$exitCode = 1
try {
    Write-Step "Regression test, output in $OutDir"
    if (Get-GameProcesses) {
        if (-not $Force) { throw 'MW4Mercs.exe is running (someone may be playing). Close it, or pass -Force.' }
        Stop-Game
    }
    $version = ([regex]::Match((Get-Content (Join-Path $repo 'src\patcher\main.cpp') -Raw), 'kToolVersion\s*=\s*L"([^"]+)"')).Groups[1].Value
    if (-not $Installer) { $Installer = Join-Path $repo "dist\MW4Mercs-ModernizationPatch-Setup-$version.exe" }
    if (-not $Cli -and -not $GameOnly) {
        Assert (Test-Path -LiteralPath $Installer) "installer not found: $Installer (build it with tools\New-Mw4Release.ps1)"
    }
    $installerFile = if ($Cli) { Join-Path $repo 'dist\mw4patch.exe' } else { $Installer }
    $installerHash = if (Test-Path -LiteralPath $installerFile) { (Get-FileHash -LiteralPath $installerFile -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
    Write-Step "Release $version, installer $installerFile ($installerHash)"

    if (-not $GameOnly) {
        # 1. Pilot saves, fresh extraction, install.
        $pilots = Join-Path $install 'RESOURCE\Pilots'
        if (Test-Path -LiteralPath $pilots) {
            Copy-Item -LiteralPath $pilots -Destination $userPilots -Recurse -Force
            Write-Step "Pilot saves backed up to $userPilots"
        }
        Write-Step 'Resetting the test install from MekTek.rar'
        $reset = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'Reset-Mw4TestInstall.ps1') -Force 2>&1
        $reset | Set-Content -LiteralPath (Join-Path $OutDir 'reset.log')
        Assert ($LASTEXITCODE -eq 0 -and ($reset -match 'reset complete')) 'Reset-Mw4TestInstall.ps1 failed (see reset.log)'

        $imagehlp = Join-Path $install 'Assets\Binaries\imagehlp.dll'
        $ok = Test-Scenario 'INST-1' {
            Invoke-Installer install 'install'
            $record = Join-Path $install 'mw4patch-install.ini'
            Assert (Test-Path -LiteralPath $record) 'no install record'
            Assert (-not (Test-Path -LiteralPath $imagehlp)) 'Assets\Binaries\imagehlp.dll is still in place'
            Assert (Test-Path -LiteralPath "$imagehlp.mw4patch-disabled") 'imagehlp.dll.mw4patch-disabled missing'
            $ini = Join-Path $install 'mw4hook.ini'
            foreach ($k in @(@('controls', 'mw5', '1'), @('aim', 'track_pointer', '1'), @('campaign', 'full_company', '1'), @('controls', 'decay', '0'))) {
                $v = Get-IniValue $ini $k[0] $k[1]
                Assert ($v -eq $k[2]) "mw4hook.ini [$($k[0])] $($k[1])=$v (expected $($k[2]))"
            }
            "installed $(Get-IniValue $record '' 'tool_version'); imagehlp.dll set aside; mw5=1 track_pointer=1 full_company=1 decay=0"
        }
        if (-not $ok) { throw 'install failed; nothing else can run' }
    }

    # 2. Game session.
    Enable-TestMode
    try {
        Start-Game
        $mark = 0
        Wait-MainMenu $mark
        Test-Scenario 'DISP-1' {
            $log = Read-Log
            $created = $log | Where-Object { $_ -match '^main window created: client=(\d+)x(\d+) window=(\d+)x(\d+) at (-?\d+),(-?\d+) style' } | Select-Object -First 1
            Assert ($created) 'no "main window created" line'
            $null = $created -match 'client=(\d+)x(\d+) window=(\d+)x(\d+) at (-?\d+),(-?\d+)'
            $cw, $ch, $ww, $wh, $x, $y = 1..6 | ForEach-Object { [int]$Matches[$_] }
            Assert ($log -match '^launch: -window present') 'the shortcut did not pass -window'
            Assert ($cw -eq 1024 -and $ch -eq 768) "client ${cw}x${ch} (expected 1024x768)"
            $work = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
            $ex = $work.X + [int](($work.Width - $ww) / 2)
            $ey = $work.Y + [int](($work.Height - $wh) / 2)
            # Owner 2026-11-26: centred on the primary monitor, not to the pixel.
            Assert ([math]::Abs($x - $ex) -le 50 -and [math]::Abs($y - $ey) -le 50)"window at $x,$y (centred would be $ex,$ey on $($work))"
            "framed ${ww}x${wh} window, client ${cw}x${ch}, at $x,$y on the primary monitor"
        } | Out-Null

        $camoOk = 'Pilot' -notin $Skip
        if ($camoOk) { try { Invoke-NewPilot } catch {
            if ($_.Exception.Message -like 'GAME:*') { throw }
            Set-Result 'CAMO-1' FAIL "new pilot: $($_.Exception.Message)"
            $camoOk = $false
            # Back to the Main Menu (Escape closes a dialog; START leaves the Roster).
            Send-Game @('key 27 100', 'wait 1000') | Out-Null
            Invoke-Ui 'Roster START' (Click $ui.RosterStart) 'script: load MainMenu\.script' 15 | Out-Null
        } }
        # Instant Action first (the new pilot has the MW5 controls the aim and
        # throttle checks need). Throttle Decay and Gyro Aim are off on a
        # fresh install.
        $hookIni = Join-Path $install 'mw4hook.ini'
        Assert ((Get-IniValue $hookIni 'controls' 'decay') -eq '0' -and (Get-IniValue $hookIni 'aim' 'gyro') -eq '0') 'mission A needs Throttle Decay and Gyro Aim off'

        $iaOk = Test-Scenario 'IA-1' {
            Invoke-LanceSetup
            "lance set (shell traces 5/55/56/71/74/7); frame $([IO.Path]::GetFileName($script:lanceShot))"
        }
        if (-not $iaOk) { throw 'Instant Action setup failed; the mission scenarios cannot run' }

        Invoke-MissionA
        # Exit Mission returns to the Instant Action screen.
        Invoke-Ui 'Instant Action > Main Menu' (Click $ui.IaMainMenu) 'script: load MainMenu\.script' 20 | Out-Null
        Start-Sleep -Seconds 2

        # Campaign: the new pilot's camouflage and the campaign MechLab.
        if ($camoOk) {
            try { Invoke-CampaignMechLab } catch { if ($_.Exception.Message -like 'GAME:*') { throw }; if (-not $results.Contains('CAMO-1')) { Set-Result 'CAMO-1' FAIL $_.Exception.Message } }
        }

        # Options > Video: the resolution for mission B, Windowed, Alt+Enter.
        Invoke-VideoOptions

        Test-Scenario 'DISP-3' {
            $t = $script:menuToggle
            Assert ($t.Off) 'menus: Windowed checkbox did not switch to borderless'
            Assert ($t.Borderless) 'menus: no borderless window'
            Assert ($t.SavedOff -eq 'false') "menus: options.ini windowed=$($t.SavedOff) after OK (expected false)"
            Assert ($t.On) 'menus: Alt+Enter did not switch back to framed'
            Assert ($t.SavedOn -eq 'true') "menus: options.ini windowed=$($t.SavedOn) after Alt+Enter (expected true)"
            $m = $script:missionToggle
            Assert ($m.Off -and $m.On) 'mission: Alt+Enter did not switch both ways'
            $size = [System.Drawing.Image]::FromFile($m.Borderless)
            $bw = $size.Width; $size.Dispose()
            $screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
            Assert ($bw -eq $screen.Width) "mission borderless frame is $bw wide (monitor $($screen.Width))"
            'checkbox off + saved, Alt+Enter on + saved (menus); Alt+Enter off/on in the mission'
        } | Out-Null
        Test-Scenario 'CTRL-1' {
            $log = Read-Log
            $pads = @($log | Where-Object { $_ -match '^dinput: device "(.+)" \(controller' })
            if (-not $pads) { return 'SKIP: no joystick or gamepad detected (not plugged in)' }
            $name = [regex]::Match($pads[0], 'device "(.+?)"').Groups[1].Value
            $opened = $log | Where-Object { $_ -match "^dinput: opened `"$([regex]::Escape($name))`" -> 0x00000000" }
            Assert ($opened) "$name was listed but not opened"
            $gamepad = $log | Where-Object { $_ -match 'gamepad axis layout' }
            $layout = if ($gamepad) { 'MW5 gamepad layout applied' } else { 'not a gamepad: MW5 gamepad layout not applicable' }
            "$name opened; $layout; Controls page $([IO.Path]::GetFileName($script:controlsShot)); stick input needs a person (not automated)"
        } | Out-Null
        Test-Scenario 'AIM-5' {
            $off = $script:gyroOff
            Assert ($off.Count -gt 5) 'no aim traces while D was held'
            $dh = Get-AngleDiff $off[-1].Heading $off[0].Heading
            $dw = Get-AngleDiff $off[-1].World $off[0].World
            # How far the legs turn depends on the frame rate (the key's held
            # flag); what matters is that the aim turns with them.
            Assert ([math]::Abs($dh) -gt 0.02) ("Gyro off: D did not turn the legs ({0:N3} rad)" -f $dh)
            # Owner 2026-11-26: the aim goes with the legs (within a quarter of
            # their turn); a broken Gyro-off path leaves it still (dw = 0).
            Assert ([math]::Abs($dw - $dh) -lt [math]::Max(0.01, 0.25 * [math]::Abs($dh)))("Gyro off: the aim did not turn with the legs (legs {0:N3}, aim {1:N3})" -f $dh, $dw)
            'off: aim turned with the legs ({0:N2} rad); on: checked in mission B' -f $dh
        } | Out-Null

        Test-Scenario 'OPT-1' {
            $on = Invoke-GameOptions $true $true
            Assert ($on.Decay -match 'on$') "no 'controls: throttle decay on' ($($on.Decay))"
            Assert ($on.Gyro -match 'on$') "no 'aim: gyro aim on' ($($on.Gyro))"
            Assert ($on.IniDecay -eq '1' -and $on.IniGyro -eq '1') "mw4hook.ini decay=$($on.IniDecay) gyro=$($on.IniGyro)"
            'Throttle Decay on, Gyro Aim on, both saved to mw4hook.ini'
        } | Out-Null

        try {
            Invoke-MissionB
        } catch {
            if ($_.Exception.Message -like 'GAME:*') { throw }
            Set-Result 'COMP-1' FAIL $_.Exception.Message
            # Leave the mission, if it is still running, for the last section.
            if (Read-Log | Select-Object -Last 200 | Where-Object { $_ -match '^aim: t=' }) {
                try { Exit-Mission | Out-Null } catch { if ($_.Exception.Message -like 'GAME:*') { throw } }
            }
        }

        Test-Scenario 'DISP-2' {
            $w, $h = $Resolution -split 'x'
            $log = Read-Log
            $mode = $log | Where-Object { $_ -match "^gos_SetScreenMode \d+x\d+ -> ${w}x${h}x32" } | Select-Object -First 1
            Assert ($mode) "no mission mode ${w}x${h}x32"
            $edge = Get-EdgeContent $script:missionShot 8
            # Owner 2026-11-26: > 0.5 (runs see 92-100 %); black bars leave the
            # side bands dark, and a dark or fading frame no longer fails.
            Assert ($edge -gt 0.5)("the mission picture does not reach the window edges ({0:P0} of the edge band lit)" -f $edge)
            "mission ${w}x${h}x32, picture to the edges ({0:P0}); saved $script:videoSaved" -f $edge
        } | Out-Null
        # Mission B stopped early: keep the reason Invoke-MissionB's catch
        # recorded instead of overwriting it ("Cannot index into a null array").
        if ($script:missionB) { Test-Scenario 'COMP-1' {
            $last = $script:missionB[-1]
            $summary = ($script:missionB | ForEach-Object { "attempt $($_.Attempt): $($_.Result) after $($_.Minutes) min" }) -join '; '
            # Logged when Instant Action builds the mission's lance (screen open).
            $company = Read-Log | Where-Object { $_ -match '^campaign: mission \d+ lancemates \d+ -> 7' } | Select-Object -Last 1
            Assert ($company) 'full company hook did not raise the mission to 7 lancemates'
            Assert ($last.Result -eq 'complete') "no mission complete: $summary"
            "$summary; $company$(if ($script:driveNote) { "; $script:driveNote" })"
        } | Out-Null }
        Test-Scenario 'AIM-6' {
            $g = @($script:gyroLines | ForEach-Object { [double]([regex]::Match($_, 'drawn_world=([-+\d.]+)').Groups[1].Value) })
            Assert ($g.Count -gt 5) 'no gyro traces while A was held'
            $spread = (($g | Select-Object -Skip 1 | ForEach-Object { [math]::Abs((Get-AngleDiff $_ $g[1])) }) | Measure-Object -Maximum).Maximum
            # Owner 2026-11-26: 0.02 rad. Runs see 0.00001; the #33 shake slid
            # the aim 0.032 rad, so it still fails.
            Assert ($spread -le 0.02)("the drawn aim moved {0:N4} rad while A was held" -f $spread)
            'drawn aim spread {0:N5} rad over {1} frames' -f $spread, $g.Count
        } | Out-Null
        Test-Scenario 'AIM-5' {
            $on = $script:gyroOn | Where-Object { $_.Gyro -eq 1 }
            Assert ($on.Count -gt 5) 'no aim traces with Gyro Aim on'
            $turning = @($on)
            $dh = Get-AngleDiff $turning[-1].Heading $turning[0].Heading
            $dw = Get-AngleDiff $turning[-1].World $turning[0].World
            Assert ([math]::Abs($dh) -gt 0.02) ("Gyro on: A did not turn the legs ({0:N3} rad)" -f $dh)
            # Owner 2026-11-26: the aim holds (moves < a quarter of the legs'
            # turn); with Gyro Aim not working it turns as far as the legs.
            Assert ([math]::Abs($dw) -lt [math]::Max(0.02, 0.25 * [math]::Abs($dh)))("Gyro on: the aim moved {0:N4} rad in the world while the legs turned {1:N3}" -f $dw, $dh)
            $offTurn = Get-AngleDiff $script:gyroOff[-1].Heading $script:gyroOff[0].Heading
            'off: aim turned with the legs ({0:N2} rad); on: legs {1:N2} rad, aim held ({2:N4})' -f $offTurn, $dh, $dw
        } | Out-Null
        Test-Scenario 'AIM-4' {
            $off = @($script:zeroOff | Where-Object { $_.Zero -ge 0 })
            $on = @($script:zeroOn | Where-Object { $_.Zero -ge 0 })
            Assert ($off.Count -gt 0 -and $on.Count -gt 0) 'no cmd0 field in the aim traces (old hook?)'
            Assert (-not ($off | Where-Object { $_.Zero -ne 1 })) 'Gyro off: settled yaw command is not exactly 0'
            Assert (-not ($on | Where-Object { $_.Zero -ne 1 })) 'Gyro on: settled yaw command is not exactly 0'
            "settled yaw command exactly 0 ($($off.Count) frames off, $($on.Count) on)"
        } | Out-Null
        Test-Scenario 'THR-2' {
            $d = $script:decay
            Assert ($d) 'decay checks did not run'
            foreach ($dir in 'Reverse', 'Forward') {
                $t = @($d[$dir])
                Assert ($t.Count -gt 0) "$dir : no throttle traces"
                $peak = if ($dir -eq 'Reverse') { ($t | Measure-Object Value -Minimum).Minimum } else { ($t | Measure-Object Value -Maximum).Maximum }
                $end = $t[-1]
                # Owner 2026-11-26 (run 20261126-123100): the peak depends on how
                # many frames the game registers the 600 ms hold (that run saw
                # 0.18 from a 2-frame press). The check proves W/S moves the
                # throttle and decay brings it back to 0, not the ramp height.
                Assert ([math]::Abs($peak) -gt 0.1) "$dir : W/S moved the throttle only to $peak"
                Assert ([math]::Abs($end.Value) -le 0.02 -and $end.Keys -eq '--') "$dir : did not ease back to 0 after release (last $($end.Value))"
            }
            # Presets and X hold: a decaying throttle moves far more than 0.02
            # in the 2.5-3 s wait.
            $p = @($d.Preset)
            Assert ($p.Count -gt 0) 'Numpad 5: no throttle traces'
            Assert ([math]::Abs($p[0].Value - 0.5) -lt 0.1) "Numpad 5 set $($p[0].Value)"
            Assert (-not ($p | Where-Object { [math]::Abs($_.Value - $p[0].Value) -gt 0.02 })) "Numpad 5 preset did not hold: $(($p | ForEach-Object Value) -join ', ')"
            $s = @($d.Stop)
            Assert ($s.Count -gt 0 -and -not ($s | Where-Object { [math]::Abs($_.Value) -ge 0.01 })) 'X did not stop and hold 0'
            'reverse and forward W/S ease to 0; Numpad 5 holds 0.5; X holds 0'
        } | Out-Null

        # Instant Action MechLab class filter: last, after the missions (a
        # visit to this MechLab left the lance droplists not opening).
        if ('Lab' -notin $Skip) {
            try { Invoke-Ui 'Instant Action > Main Menu' (Click $ui.IaMainMenu) 'script: load MainMenu\.script' 20 | Out-Null } catch { if ($_.Exception.Message -like 'GAME:*') { throw } }
            Start-Sleep -Seconds 2
        }
        # Instant Action MechLab class filter.
        if ('Lab' -notin $Skip) {
        Open-InstantAction | Out-Null
        Invoke-Ui 'Instant Action > MechLab' (Click $ui.IaMechLab) 'script: load MechBay\.script' 30 | Out-Null
        Start-Sleep -Seconds 5
        $classNames = 'Light', 'Medium', 'Heavy', 'Assault', 'Elemental'
        $byClass = @{}
        $fullRows = 0
        $mark0 = Get-LogMark
        Test-Scenario 'LAB-1' {
            $notes = @()
            for ($c = 1; $c -le 5; $c++) {
                $mark = Get-LogMark
                Invoke-Ui "Class > $($classNames[$c - 1])" ((Pick $ui.IaLabClass $c 8) + @('wait 1500')) "^mechlab: filter $($c - 1), \d+ rows" 12 | Out-Null
                Start-Sleep -Seconds 3   # the class's first 'Mech loads
                $lines = Get-LogSince $mark
                $header = $lines | Where-Object { $_ -match "^mechlab: filter $($c - 1), (\d+) rows" } | Select-Object -Last 1
                Assert ($header) "$($classNames[$c - 1]): no filtered list reported"
                $at = [array]::LastIndexOf($lines, $header)
                $n = [int]([regex]::Match($header, ', (\d+) rows').Groups[1].Value)
                $rows = @($lines[($at + 1)..($at + $n)] | ForEach-Object { if ($_ -match "^mechlab: row '(.+)' class (-?\d+)") { [pscustomobject]@{ Name = $Matches[1]; Class = [int]$Matches[2] } } })
                $wrong = @($rows | Where-Object { $_.Class -ne $c - 1 -and $_.Class -ge 0 })
                Assert ($rows.Count -eq $n -and $n -gt 0) "$($classNames[$c - 1]): $n rows reported, $($rows.Count) read"
                Assert ($wrong.Count -eq 0) "$($classNames[$c - 1]) lists $($wrong[0].Name) (class $($wrong[0].Class))"
                $byClass[$c - 1] = $rows
                $notes += "$($classNames[$c - 1]) $n"
            }
            $mark = Get-LogMark
            Invoke-Ui 'Class > All' ((Pick $ui.IaLabClass 0 8) + @('wait 1500')) '^mechlab: filter -1, \d\d+ rows' 12 | Out-Null
            $all = Get-LogSince $mark | Where-Object { $_ -match '^mechlab: filter -1, (\d+) rows' } | Select-Object -Last 1
            $script:fullRows = [int]([regex]::Match("$all", ', (\d+) rows').Groups[1].Value)
            $sum = ($byClass.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
            Assert ($script:fullRows -ge $sum -and $script:fullRows -gt 50) "All lists $script:fullRows rows (classes add up to $sum)"
            ($notes -join ', ') + "; All $script:fullRows"
        } | Out-Null
        Test-Scenario 'LAB-2' {
            Assert ($byClass.ContainsKey(2)) 'no Heavy list from LAB-1'
            $pickName = 'Avatar'
            $filteredRow = [array]::IndexOf(@($byClass[2] | ForEach-Object Name), $pickName)
            Assert ($filteredRow -ge 0) "$pickName is not in the Heavy list"
            $fullList = @(Read-Log | Where-Object { $_ -match "^shell: 4 row (\d+) '(.+)'$" } | ForEach-Object { ($_ -split "'")[1] })
            $fullRow = [array]::IndexOf($fullList, $pickName)
            Assert ($fullRow -ge 0) "$pickName is not in the full 'Mech list"
            # A chassis pick reloads the 'Mech, whose variant (Name) list then
            # reports itself (mechlab: filter -1). A click made while the
            # MechLab is still loading is lost, and the keys then go to the
            # Class list, so each pick waits for that line and checks the
            # class was not changed.
            # A neutral click first (the MechLab title) takes the focus off
            # the Class list; if the keys still reached it, the class is set
            # again and the pick repeated.
            $pickChassis = {
                param([int]$Row, [int]$Rows, [int]$Class)
                # Run 20261126-142519: the Type click was lost 3/3 times (the
                # keys reached the Class list, #206 5). The class change that
                # precedes every attempt reloads the 'Mech, and a Type click
                # made during that load is lost; the retry's own class reset
                # re-triggered it, so every try hit the same window. Let the
                # MechLab settle before each try and give the pick more tries.
                for ($try = 1; $try -le 5; $try++) {
                    $m = Get-LogMark
                    Start-Sleep -Seconds 10   # the 'Mech the class change loads
                    # The MechLab's Type list takes a few seconds to drop (the
                    # class filter sorts its rows in script).
                    # Like a new screen, the MechLab ignores the first click
                    # after it has loaded a 'Mech: two neutral clicks first.
                    Send-Game ((Click $ui.IaLabNeutral) + @('wait 1500') + (Click $ui.IaLabNeutral) + @('wait 1000') + (Pick $ui.IaLabType $Row $Rows 3500) + @('wait 1500')) | Out-Null
                    # The variant (Name) list; the Type list itself reports 128 rows.
                    $hit = Wait-Log '^mechlab: filter -1, \d{1,2} rows' $m 12 -NoThrow
                    $changed = Get-LogSince $m | Where-Object { $_ -match '^shell: 49 ->\S+ #206 ' }
                    if ($hit -and -not $changed) {
                        Start-Sleep -Seconds 4
                        Send-Game @('smove 790 595', 'wait 800') | Out-Null
                        return [int]([regex]::Match($hit, ', (\d+) rows').Groups[1].Value)
                    }
                    Write-Step "  Type > $pickName : $(if ($changed) { "keys reached the Class list ($changed)" } else { 'no chassis loaded' }) (try $try of 5)"
                    if ($changed) {
                        Invoke-Ui 'Class (again)' ((Click $ui.IaLabNeutral) + (Pick $ui.IaLabClass $Class 8) + @('wait 1500')) "^shell: 49 ->\S+ #206 $Class$" 15 | Out-Null
                        Start-Sleep -Seconds 8
                    }
                }
                throw "could not pick $pickName from the Type list"
            }
            # From the full list (Class All).
            $variantsAll = & $pickChassis $fullRow $fullList.Count 0
            $a = Save-Shot 'lab-avatar-all'
            # From the Heavy list.
            Invoke-Ui 'Class > Heavy' ((Pick $ui.IaLabClass 3 8) + @('wait 1500')) '^shell: 49 ->\S+ #206 3$' 15 | Out-Null
            Start-Sleep -Seconds 8   # the first Heavy 'Mech loads
            $variantsHeavy = & $pickChassis $filteredRow 40 3
            $b = Save-Shot 'lab-avatar-heavy'
            Assert ($variantsAll -eq $variantsHeavy) "$pickName has $variantsAll variants from the full list, $variantsHeavy from the Heavy list"
            # The left panel below the Type/Class row (variant, camouflage,
            # components, heat sinks, speed) and the tonnage/stat bars. The
            # Type/Class row differs anyway (Class reads All / Heavy, and the
            # Type text can sit a pixel lower); the chassis is checked by the
            # variant count above. The stats panel stops
            # above its last row: that row is translucent over the animated
            # hangar.
            $diff = 0
            $area = 0
            foreach ($r in @(@(16, 160, 316, 505), @(336, 30, 680, 60), @(340, 62, 476, 180))) {
                $px = ShotRect $a $r
                $diff += Get-PixelDiff $a $b $px
                $area += ($px[2] - $px[0]) * ($px[3] - $px[1])
            }
            # Owner 2026-11-26: up to 2 % of the panel pixels may differ (runs
            # failed on 593 and 1597 px from animation/blending, out of a few
            # hundred thousand at full-window frame sizes); a different 'Mech
            # or loadout changes whole panels.
            Assert ($diff -le 0.02 * $area) "$pickName from the Heavy list differs from the full list in $diff of $area pixels ($([IO.Path]::GetFileName($a)) vs $([IO.Path]::GetFileName($b)))"
            Send-Game ((Pick $ui.IaLabClass 0 8) + @('wait 1500')) | Out-Null
            "$pickName via Heavy row $filteredRow = full list row $fullRow (panels identical)"
        } | Out-Null
        Invoke-Ui 'MechLab > Close' ((Click $ui.IaLabNeutral) + (Click $ui.IaLabClose)) '^shell: 1 ->' 30 | Out-Null
        Start-Sleep -Seconds 2
        Invoke-Ui 'Instant Action > Main Menu' (Click $ui.IaMainMenu) 'script: load MainMenu\.script' 20 | Out-Null
        Start-Sleep -Seconds 2
        }
    } catch {
        if ($_.Exception.Message -like 'GAME:*') {
            Save-Evidence 'crash'
            Set-Result 'CRASH-1' FAIL $_.Exception.Message.Substring(5).Trim()
        } else {
            Write-Step "Game session stopped: $($_.Exception.Message)"
            Add-Content -LiteralPath $transcript -Value $_.ScriptStackTrace
        }
    } finally {
        # Whole-session checks from the logs, then close the game.
        Save-Evidence 'session'
        $hookLog = Read-Log
        $ddraw = if ($script:gamePid) { Join-Path $install "mw4hook-ddraw-$script:gamePid.log" } else { '' }
        $ddrawLog = if ($ddraw -and (Test-Path -LiteralPath $ddraw)) { Get-Content -LiteralPath $ddraw } else { @() }
        if ($hookLog.Count) {
            Test-Scenario 'DISP-4' {
                $modes = @($hookLog | Where-Object { $_ -match '^gos_SetScreenMode \d+x\d+ -> \d+x\d+x(\d+)' } | ForEach-Object { [int]([regex]::Match($_, '-> \d+x\d+x(\d+)').Groups[1].Value) })
                Assert ($modes.Count -gt 0) 'no display modes logged'
                $bad = $modes | Where-Object { $_ -ne 32 }
                Assert (-not $bad) "non-32-bit modes: $($bad -join ', ')"
                $z16 = $ddrawLog | Where-Object { $_ -match 'z-buffer .*: 16-bit Z' }
                Assert (-not $z16) "16-bit depth buffer: $($z16 | Select-Object -First 1)"
                "$($modes.Count) mode set(s), all 32-bit; no 16-bit depth buffer"
            } | Out-Null
        }
        if (-not $results.Contains('CRASH-1')) {
            Test-Scenario 'CRASH-1' {
                Assert (-not $script:crash) $script:crash
                # 'sound:' lines are the hook's own recovery from a lost DirectSound
                # device (#46): the game went on, so they are reported, not failed.
                $sig = $hookLog + $ddrawLog | Where-Object { $_ -match '0x00667FA4|00667FA4|DSERR_BUFFERLOST|SoundPause|access violation|exception' -and $_ -notmatch '^(auto|sound):' }
                Assert (-not $sig) "crash signature in the log: $($sig | Select-Object -First 1)"
                Assert ($script:gamePid -and (Get-Process -Id $script:gamePid -ErrorAction SilentlyContinue)) 'the game was not running at the end of the session'
                $recovered = @($hookLog | Where-Object { $_ -match '^sound: .*(still DSERR_BUFFERLOST|silent buffer)' }).Count
                if ($recovered) { "no dialog, no unexpected exit, no crash signature (#44); sound recovered from $recovered lost-buffer event(s) (#46)" }
                else { 'no dialog, no unexpected exit, no crash signature (#44)' }
            } | Out-Null
        }
        Stop-Game
        Disable-TestMode
    }

    if (-not $GameOnly) {
        # 3. Update in place, uninstall, manifest.
        Test-Scenario 'INST-2' {
            $ini = Join-Path $install 'mw4hook.ini'
            $before = @{ decay = Get-IniValue $ini 'controls' 'decay'; gyro = Get-IniValue $ini 'aim' 'gyro' }
            Invoke-Installer install 'update'
            $after = @{ decay = Get-IniValue $ini 'controls' 'decay'; gyro = Get-IniValue $ini 'aim' 'gyro'; mw5 = Get-IniValue $ini 'controls' 'mw5'; tp = Get-IniValue $ini 'aim' 'track_pointer' }
            Assert ($before.decay -eq '1') "Throttle Decay was not on before the update ($($before.decay)); OPT-1 must pass first"
            Assert ($after.decay -eq $before.decay) "update changed decay $($before.decay) -> $($after.decay)"
            Assert ($after.gyro -eq $before.gyro) "update changed gyro $($before.gyro) -> $($after.gyro)"
            Assert ($after.mw5 -eq '1' -and $after.tp -eq '1') "update: mw5=$($after.mw5) track_pointer=$($after.tp)"
            "updated in place; decay=$($after.decay) gyro=$($after.gyro) kept"
        } | Out-Null
        Test-Scenario 'INST-3' {
            Invoke-Installer uninstall 'uninstall'
            $imagehlp = Join-Path $install 'Assets\Binaries\imagehlp.dll'
            Assert (Test-Path -LiteralPath $imagehlp) 'imagehlp.dll not restored'
            Assert (-not (Test-Path -LiteralPath "$imagehlp.mw4patch-disabled")) 'imagehlp.dll.mw4patch-disabled left behind'
            $left = @('dinput.dll', 'ddraw.dll', 'mw4hook.ini', 'mw4patch-install.ini', 'mw4hook-pilots.ini', 'MechWarrior 4 Mercenaries (Windowed).lnk') |
                Where-Object { Test-Path -LiteralPath (Join-Path $install $_) }
            $logs = @(Get-ChildItem -LiteralPath $install -Filter 'mw4hook*' -ErrorAction SilentlyContinue)
            Assert (-not $left -and -not $logs) "left behind: $(@($left) + @($logs | ForEach-Object Name) -join ', ')"
            $manifest = & (Join-Path $PSScriptRoot 'Test-Mw4Manifest.ps1') -Install $install -Manifest (Join-Path $repo 'manifests\mw4-mercs-3.1c-full.sha256') 2>&1
            $manifest | Set-Content -LiteralPath (Join-Path $OutDir 'manifest-after-uninstall.txt')
            $verified = ($manifest | Select-String 'Verified : (\d+)').Matches.Groups[1].Value
            Assert ($LASTEXITCODE -eq 0) "full manifest differs: $(($manifest | Select-String 'Missing  :|Mismatch :') -join ' ')"
            "imagehlp.dll restored; full manifest $verified files match; no patch files left"
        } | Out-Null
    }
    $exitCode = 0
} catch {
    Write-Step "ABORTED: $($_.Exception.Message)"
    Add-Content -LiteralPath $transcript -Value $_.ScriptStackTrace
} finally {
    Stop-Game
    if (-not $GameOnly -and (Test-Path -LiteralPath $userPilots)) {
        $pilots = Join-Path $install 'RESOURCE\Pilots'
        if (Test-Path -LiteralPath $pilots) { Remove-Item -LiteralPath $pilots -Recurse -Force }
        Copy-Item -LiteralPath $userPilots -Destination $pilots -Recurse -Force
        Write-Step 'Pilot saves restored'
    }
    if (Get-GameProcesses) { Write-Step 'WARNING: MW4Mercs.exe is still running' }

    # Summary.
    foreach ($id in $scenarios.Keys) {
        if (-not $results.Contains($id)) {
            if ($GameOnly -and $id -like 'INST-*') { $results[$id] = [pscustomobject]@{ Id = $id; Status = 'SKIP'; Detail = '-GameOnly' } }
            else { $results[$id] = [pscustomobject]@{ Id = $id; Status = 'FAIL'; Detail = 'not run' } }
        }
    }
    $rows = foreach ($id in $scenarios.Keys) { $r = $results[$id]; [pscustomobject]@{ Id = $id; Status = $r.Status; Scenario = $scenarios[$id]; Detail = $r.Detail } }
    $nPass = @($rows | Where-Object Status -eq 'PASS').Count
    $nFail = @($rows | Where-Object Status -eq 'FAIL').Count
    $nSkip = @($rows | Where-Object Status -eq 'SKIP').Count
    $md = @(
        "# MW4 Mercenaries Modernization Patch regression test",
        '',
        "- Release: $version",
        "- Installer: ``$installerFile``",
        "- Installer SHA-256: ``$installerHash``",
        "- Start: $($started.ToString('yyyy-MM-dd HH:mm:ss')), duration $([math]::Round(((Get-Date) - $started).TotalMinutes, 1)) min",
        "- Source: $(if ($GameOnly) { 'existing install (-GameOnly)' } else { 'fresh MekTek.rar extraction' })",
        "- Result: **$nPass passed, $nFail failed, $nSkip skipped**",
        '',
        '| Id | Status | Scenario | Detail |',
        '|---|---|---|---|'
    ) + ($rows | ForEach-Object { "| $($_.Id) | $($_.Status) | $($_.Scenario) | $(($_.Detail -replace '\|', '/') -replace "`r?`n", ' ') |" })
    $md | Set-Content -LiteralPath (Join-Path $OutDir 'summary.md')
    $rows | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutDir 'results.json')
    Write-Host ''
    $rows | Format-Table Id, Status, Detail -AutoSize -Wrap | Out-String -Width 220 | Write-Host
    Write-Host "Release $version  installer $installerHash"
    Write-Host "PASS $nPass  FAIL $nFail  SKIP $nSkip   ($OutDir\summary.md)"
    if ($nFail -gt 0) { $exitCode = 1 }
}
exit $exitCode
