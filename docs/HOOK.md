# Runtime hook: windowed 1024x768 (development)

Status: **development only**. This documents the first working implementation
of the windowed 1024x768 launch. It is not the final patcher.

## What works

- `MW4Mercs.exe -window` starts in a **1024x768 client-area window**.
- The hook is a `ddraw.dll` proxy placed next to `MW4Mercs.exe`.
- The game still uses its original renderer and assets.
- The on-disk `MW4Mercs.exe` is not modified.
- The game's built-in 800x600 display defaults are patched **in memory** so
  the display path agrees with the 1024x768 window.
- The game's in-game resolution menu is left in place and is not disabled.

## Why `ddraw.dll`, not `version.dll`

AGENTS.md originally suggested a `version.dll` proxy. Testing against the
supported 3.1c build showed:

- The 3.1c executable is **PECompact 2 packed**. Its on-disk import table only
  lists `kernel32.dll`.
- A `version.dll` placed in the game directory is not loaded by
  `MW4Mercs.exe`. The `version.dll` log entries seen during the investigation
  came from `dw.exe`, a helper in the same directory.
- The game does load **`ddraw.dll` from its application directory**. Copying
  the system `ddraw.dll` into the game directory and inspecting the process
  module list confirmed the local file wins.

So the hook is a `ddraw.dll` proxy. This is the same general loading technique
used by dgVoodoo2 and other DirectDraw-era compatibility layers.

## Components

| File | Purpose |
|---|---|
| `src/hook/ddraw_proxy.cpp` | Forwards all 22 `ddraw.dll` exports to `System32\ddraw.dll`. |
| `src/hook/dllmain.cpp` | Loads the hook, starts the worker thread. |
| `src/hook/window.cpp` | Hooks `user32` imports in the game EXE and forces the main window size. |
| `src/hook/resolution.cpp` | Patches the game's 800x600 display defaults in memory. |
| `src/hook/iat.cpp` | Import-address-table patching by resolved function address. |
| `src/hook/log.cpp` | Config loading and `mw4hook-<pid>.log` output. |
| `src/hook/build.ps1` | Builds the x86 DLL with Visual Studio 2022. |

### Window sizing

The hook watches `CreateWindowExA` for the top-level window whose class is
`MechWarrior Mercenaries`. It computes the outer size needed for a 1024x768
client area and passes that to the real `CreateWindowExA`. The game later
tries to apply its stored 800x600 size with `SetWindowPos`; the first such
resize is redirected to 1024x768. Later resizes are left alone so the in-game
resolution menu can still change the window.

### Resolution defaults

The retail decompilation (`FUN_00407BE0`) shows the game writing:

```asm
mov dword ptr [g_width],  0x320   ; 800
mov dword ptr [g_height], 0x258   ; 600
```

The 3.1c build is packed, but the same instruction pair exists in the unpacked
image. The hook scans the executable's `.text` for that pair, rewrites the two
immediates to 1024/768, and updates the live globals. It also rewrites the
`push 600; push 800` pair in the windowed display path. This is an in-memory
patch only; the file on disk is untouched.

## Build

Requires Visual Studio 2022 C++ build tools (x86).

```powershell
pwsh ./src/hook/build.ps1
```

Output:

```text
artifacts/hook/ddraw.dll
```

## Test from a pristine install

Always start from a fresh extraction for destructive tests:

```powershell
pwsh ./tools/Reset-Mw4TestInstall.ps1 -Force
```

Install the hook into the test install:

```powershell
$install = "MekTek\Mechwarrior Mercenaries - Mektek Mekpak"
Copy-Item artifacts/hook/ddraw.dll $install -Force
Copy-Item artifacts/hook/mw4hook.ini $install -Force
```

The intro FMV sequence makes each test take a long time, so a test-only helper
replaces the startup movies with the game's own `CEmpty.mpg` placeholder:

```powershell
pwsh ./tools/Set-Mw4IntroSkip.ps1 -Install $install
```

Launch windowed:

```powershell
Start-Process "$install\MW4Mercs.exe" -ArgumentList "-window" -WorkingDirectory $install
```

While testing the display path, add the game's own `-nosound` switch. On this
machine the full sound path can fail with a DirectSound
`CreateSoundBuffer` error during menu/movie audio, which is a pre-existing
game issue and not caused by the window hook:

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

Inspect the hook log:

```powershell
Get-ChildItem "$install\mw4hook-*.log" | Get-Content
```

Restore the intro movies and remove the hook when finished:

```powershell
pwsh ./tools/Set-Mw4IntroSkip.ps1 -Install $install -Restore
Remove-Item "$install\ddraw.dll","$install\mw4hook.ini"
```

## Current evidence

- Fresh 3.1c extraction verified against the 32-file core manifest.
- `ddraw.dll` proxy loads in `MW4Mercs.exe` (PID matches the log).
- All 22 `ddraw.dll` exports resolve.
- Main window is created at a 1024x768 client area.
- The game reaches the main menu and remains responsive.
- Intro-skip test path reaches the menu with no fatal dialog.

## Known limitations

1. **The frontend still renders at 800x600.** The window is 1024x768, but the
   2002 shell draws its menu into an 800x600 buffer at the top-left, leaving
   black/garbage space on the right and bottom. Fixing this requires either
   reversing the shell's DIB/blit path or a different display architecture.
2. **In-mission rendering is not yet verified** at 1024x768.
3. **The in-game resolution menu is not yet retested** with the hook active.
4. **Borderless fullscreen was attempted and rejected.** Running the normal
   fullscreen path with `DDSCL_NORMAL` and a forced window reached the menu,
   but the game resized the window to about 803x575 and produced a DirectSound
   buffer error. That path is not part of this implementation.
5. **AntiHack and multiplayer have not been tested.** The hook does not modify
   the executable on disk, but this must be verified before release.
6. **The intro-skip helper is test-only.** It modifies files under
   `Content\Movies` and must be restored before any baseline/hash check.
7. **Sound is unreliable in the current test environment.** With sound enabled,
   the game can show a DirectSound `CreateSoundBuffer` error during menu/movie
   playback. `-nosound` is used for display testing. This is a separate game
   compatibility problem, not a side effect of the window hook.

## Reversibility

- Delete `ddraw.dll` and `mw4hook.ini` from the game directory.
- Restore `options.ini` from its backup if it was changed.
- Restore the movies with `Set-Mw4IntroSkip.ps1 -Restore`.
- No executable bytes on disk are changed by the hook.
