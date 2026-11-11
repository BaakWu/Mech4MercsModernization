# Runtime hook: windowed 1024x768 (development)

Status: **development only**. This documents the working windowed launch.

## What works

- The patched shortcut launches `MW4Mercs.exe -window` in a **1024x768
  client-area window**, centered on the primary monitor.
- The monitor stays at the desktop resolution; the game does not switch it to
  800x600 or another mode.
- The game uses its original renderer and assets.
- The on-disk `MW4Mercs.exe` is not modified.
- The game's built-in 800x600 display defaults are patched **in memory**.
- The in-game resolution menu is left in place and is not disabled.

**Important:** the raw `MW4Mercs.exe` still starts in the game's fullscreen
default. The supported launch is the patched shortcut (`-window`). The patcher
updates the existing `Shortcut to MW4Mercs.exe.lnk` and creates
`MechWarrior 4 Mercenaries (Windowed).lnk`.

## Why `dinput.dll`

The 3.1c executable is PECompact 2 packed. Two things matter:

- A local `version.dll` is **not** loaded by `MW4Mercs.exe`.
- A local `ddraw.dll` is loaded, but too late to affect startup decisions.

`dinput.dll` is imported statically by the game, so a local proxy is loaded
during import resolution - before the game parses its command line and creates
its main window. The module list confirms the game loads the local
`dinput.dll`.

Attempts to force the raw executable into the windowed path from inside the
process (PEB command-line rewrite, patching the `-window` check, blocking the
display-mode syscall, and forcing the window style) reached the windowed size
but triggered the game's own `GetLocationFromAddress` assert. Those experiments
are not part of the supported launch. The reliable path is the game's real
`-window` switch.

## Components

| File | Purpose |
|---|---|
| `src/hook/dinput_proxy.cpp` | Forwards the 7 `dinput.dll` exports to `System32\dinput.dll`. |
| `src/hook/dllmain.cpp` | Loads the hook and starts the worker thread. |
| `src/hook/window.cpp` | Hooks `user32` window calls, forces the client size, centers the window. |
| `src/hook/resolution.cpp` | Patches the game's 800x600 display defaults in memory. |
| `src/hook/iat.cpp` | Import-address-table patching by resolved function address. |
| `src/hook/log.cpp` | Config loading and `mw4hook-<pid>.log` output. |
| `src/hook/build.ps1` | Builds the x86 proxy DLL with Visual Studio 2022. |

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
MechWarrior Mercenaries  ...  1024x768  1030x797
```

## Current evidence

- Fresh 3.1c extraction verified against the 32-file core manifest.
- Local `dinput.dll` proxy loads in `MW4Mercs.exe` (PID matches the log).
- `MW4Mercs.exe -window -nosound` produces a centered 1024x768 client area.
- The desktop resolution remains 2048x1152 for the whole run.
- The game reaches the main menu and remains responsive.
- No game executable bytes are changed on disk.

## Known limitations

1. **The frontend still renders at 800x600.** The window is 1024x768, but the
   2002 shell draws its menu into an 800x600 buffer at the top-left, leaving
   black/garbage space on the right and bottom.
2. **In-mission rendering is not yet verified** at 1024x768.
3. **The in-game resolution menu is not yet retested** with the hook active.
4. **The raw executable remains fullscreen.** Use the patched shortcut. Forcing
   the raw launch triggered the game's `GetLocationFromAddress` assert.
5. **AntiHack and multiplayer have not been tested.**
6. **The intro-skip helper is test-only.** It modifies files under
   `Content\Movies` and must be restored before a baseline/hash check.
7. **Sound is unreliable in the current test environment.** With sound enabled,
   the game can show a DirectSound `CreateSoundBuffer` error during menu/movie
   playback. `-nosound` is used for display testing.

## Reversibility

- Run `mw4patch.exe uninstall` to remove the hook, restore `options.ini`, and
  restore the previous shortcut.
- Or delete `dinput.dll` and `mw4hook.ini` from the game directory.
- Restore the movies with `Set-Mw4IntroSkip.ps1 -Restore`.
- No executable bytes on disk are changed by the hook.
