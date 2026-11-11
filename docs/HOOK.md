# Runtime hook: windowed 1024x768 (development)

Status: **development only**. This documents the working windowed launch.

## What works

- The patched shortcut launches `MW4Mercs.exe -window` in a **1024x768
  client-area window**, centered on the primary monitor.
- The process opts out of DPI virtualization before the game creates its
  window, so one game pixel is one physical pixel at 125%/150% display
  scaling. This removes the extra scaled border and the mouse drift caused by
  virtualized cursor coordinates.
- Starting the raw `MW4Mercs.exe` also works: the proxy relaunches it with
  `-window` before the game initializes. The first process exits immediately
  and the visible game process runs windowed.
- The monitor stays at the desktop resolution; the game does not switch it to
  800x600 or another mode.
- The 2002 frontend shell is still laid out for 800x600. The `ddraw.dll` proxy
  scales that content to fill the 1024x768 window, so there are no black bands
  on the right and bottom.
- In-mission rendering is 1024x768 and fills the window. The frontend scaler
  detects the shell's black border and does not touch mission frames.
- The game uses its original renderer and assets.
- The on-disk `MW4Mercs.exe` is not modified.
- The game's built-in 800x600 display defaults are patched **in memory**.
- The in-game resolution menu is left in place and is not disabled.

## Why `dinput.dll`

The 3.1c executable is PECompact 2 packed. Two things matter:

- A local `version.dll` is **not** loaded by `MW4Mercs.exe`.
- A local `ddraw.dll` is loaded, but too late to affect startup decisions.

`dinput.dll` is imported statically by the game, so a local proxy is loaded
during import resolution - before the game parses its command line and creates
its main window. The module list confirms the game loads the local
`dinput.dll`.

Attempts to force the raw executable into the windowed path in-process (PEB
command-line rewrite, patching the `-window` check, blocking the display-mode
syscall, and forcing the window style) reached the windowed size but triggered
the game's own `GetLocationFromAddress` assert. The working approach is a
**startup relaunch**: the proxy checks the command line at DllMain time, and if
`-window` is missing it creates a new `MW4Mercs.exe ... -window` process and
terminates the original. The relaunched process takes the game's normal
windowed path.

## Components

| File | Purpose |
|---|---|
| `src/hook/dinput_proxy.cpp` | Forwards the 7 `dinput.dll` exports to `System32\dinput.dll`. |
| `src/hook/dllmain.cpp` | Loads the hook, opts out of DPI virtualization, starts the worker thread. |
| `src/hook/window.cpp` | Hooks `user32` window calls, forces the client size, centers the window. |
| `src/hook/ddraw_proxy.cpp` | Forwards `ddraw.dll`, patches the surface vtable, scales the 800x600 frontend into the window. |
| `src/hook/resolution.cpp` | Patches the game's 800x600 display defaults in memory. |
| `src/hook/iat.cpp` | Import-address-table patching by resolved function address. |
| `src/hook/log.cpp` | Config loading and `mw4hook-<pid>.log` output. |
| `src/hook/build.ps1` | Builds the x86 proxy DLLs with Visual Studio 2022. |

### DPI awareness

The game has no DPI-awareness manifest. On a 125% display Windows gives it a
1024x768 logical client that is 1280x960 physical pixels. DirectDraw draws the
1024x768 surface into the top-left of that window, which is exactly the black
right/bottom band in the launch screenshots. The virtualized cursor also no
longer matches the render surface, which contributes to the in-mission aim
drift at non-100% scaling.

`dllmain.cpp` calls `SetProcessDpiAwarenessContext(SYSTEM_AWARE)` before the
game creates its window (with `SetProcessDPIAware` as the fallback). The client
area becomes a true 1024x768 physical area, the game and cursor share the same
coordinate space, and the extra scaling disappears.

### Frontend scaling (`ddraw.dll`)

`MW4Mercs.exe` imports `ddraw.dll`; the local proxy loads the system DLL and
patches the real `IDirectDraw7` and surface vtables. The frontend presents its
800x600 shell to a 1024x768 surface with `IDirectDrawSurface7::Blt`.

`Hook_Blt` samples the area outside the shell's 800x600 rectangle. When that
border is mostly black, the frame is the frontend and the hook replaces the
1:1 `Blt` with a `StretchBlt` of the 800x600 content to the destination. A
mission fills the surface, fails the border check, and is copied unchanged.
The behaviour is controlled by `[frontend] stretch=1` in `mw4hook.ini`.

### Window sizing and centering

The hook watches `CreateWindowExA` for the top-level window whose class is
`MechWarrior Mercenaries`. It computes the outer size needed for a 1024x768
client area and passes that to the real `CreateWindowExA`. It then centers the
window on the primary work area. The game's first `SetWindowPos` applying its
stored 800x600 size is redirected to 1024x768.

### Resolution defaults

The retail decompilation (`FUN_00407BE0`) shows the game writing:

```asm
mov dword ptr [g_width],  0x320   ; 800
mov dword ptr [g_height], 0x258   ; 600
```

The 3.1c build is packed, but the same instruction pair exists in the unpacked
image. The hook scans `.text` for that pair, rewrites the two immediates to
1024/768, and updates the live globals. It also rewrites the
`push 600; push 800` pair in the windowed display path. This is an in-memory
patch only; the file on disk is untouched.

## Build

Requires Visual Studio 2022 C++ build tools (x86).

```powershell
pwsh ./src/hook/build.ps1
```

Output:

```text
artifacts/hook/dinput.dll
artifacts/hook/ddraw.dll
artifacts/hook/mw4hook.ini
```

## Test from a pristine install

Always start from a fresh extraction for destructive tests:

```powershell
pwsh ./tools/Reset-Mw4TestInstall.ps1 -Force
```

Build and install via the patcher:

```powershell
pwsh ./src/patcher/build.ps1

$install = "MekTek\Mechwarrior Mercenaries - Mektek Mekpak"
.\dist\mw4patch.exe install $install --yes
```

Launch the windowed shortcut, or pass the switch explicitly:

```powershell
Start-Process "$install\MW4Mercs.exe" -ArgumentList "-window" -WorkingDirectory $install
```

For display testing, add `-nosound` to avoid the game's pre-existing
DirectSound `CreateSoundBuffer` error:

```powershell
pwsh ./tools/Start-Mw4Windowed.ps1 -Install $install -NoSound
```

Verify the client area:

```powershell
pwsh ./tools/Get-Mw4WindowInfo.ps1 -ProcessId <pid>
```

Expected:

```text
MechWarrior Mercenaries  ...  1024x768  1030x803
```

## Current evidence

- Fresh 3.1c extraction verified against the 32-file core manifest.
- Local `dinput.dll` and `ddraw.dll` proxies load in `MW4Mercs.exe`.
- `MW4Mercs.exe -window -nosound` produces a centered 1024x768 physical client
  area on a 125% scaled desktop.
- The 125% display scaling no longer produces the extra right/bottom band.
- The main menu and Options screens fill the 1024x768 window without black
  bands; the menu cursor and hit areas remain aligned with the scaled artwork.
- In-mission rendering fills the 1024x768 window and is not stretched by the
  frontend scaler.
- The Video tab still shows the in-game Screen Resolution control (1024x768).
- Starting `MW4Mercs.exe -nosound` without `-window` relaunches the process
  with `-window`; the visible process is a centered 1024x768 window.
- The desktop resolution remains 2048x1152 for the whole run.
- No game executable bytes are changed on disk.

## Known limitations

1. **The frontend scaler is a GDI present.** The shell is still generated at
   800x600 and upscaled to 1024x768, so the menu art is slightly softer than a
   native 1024x768 shell. The original 4:3 layout is preserved. The scaler is
   enabled by `[frontend] stretch=1` and can be turned off for comparison.
2. **The in-game resolution menu is displayed but not fully retested for a
   mode change.** It still shows the five original modes and the current
   1024x768 mode; the hook controls the window size, so changing the menu mode
   is a later test.
3. **The relaunch creates a brief second process.** The original process exits
   before the game initializes; the relaunched process is the visible game. The
   patched shortcuts pass `-window` directly and skip this step.
4. **AntiHack and multiplayer have not been tested.**
5. **The intro-skip helper is test-only.** It modifies files under
   `Content\Movies` and must be restored before a baseline/hash check.
6. **Sound is unreliable in the current test environment.** With sound enabled,
   the game can show a DirectSound `CreateSoundBuffer` error during menu/movie
   playback. `-nosound` is used for display testing.

## Reversibility

- Run `mw4patch.exe uninstall` to remove the hook, restore `options.ini`, and
  restore the previous shortcut.
- Or delete `dinput.dll` and `mw4hook.ini` from the game directory.
- Restore the movies with `Set-Mw4IntroSkip.ps1 -Restore`.
- No executable bytes on disk are changed by the hook.
