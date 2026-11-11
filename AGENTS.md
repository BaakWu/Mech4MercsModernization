# AGENTS.md — working guide for this repository

This file is the entry point for any AI agent or contributor working on this
project. Read it before changing code, running the game, or touching a game
install.

## 1. Mission

Build a small, auditable Windows patch for **MechWarrior 4: Mercenaries** that
makes the existing 3.1c game behave correctly on modern systems.

This is **not** an engine rewrite, a source port, or a competitor to MekTek
Rebirth. The product is a patch executable that:

- verifies the supported game build
- fixes modern-Windows display and input defects
- keeps MekTek AntiHack intact
- is reversible and hash-checked
- does not redistribute game files

The primary release artifact will be a self-contained **`.exe` patcher**
(`mw4patch.exe`). It may embed and install a native runtime hook DLL, but the
user-facing release stays a single executable.

## 2. Supported baseline

The supported game is **MechWarrior 4: Mercenaries 3.1c**, version
`51.03.01.0030`.

| Field | Value |
|---|---|
| `MW4Mercs.exe` version | `51.03.01.0030` |
| `MW4Mercs.exe` SHA-256 | `72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41` |
| Reference manifest | `manifests/mw4-mercs-3.1c-core.sha256` |
| Full manifest | `manifests/mw4-mercs-3.1c-full.sha256` |

The local baseline copy and legacy patch media are intentionally excluded from
Git. They are local test inputs only:

```text
MekTek.rar              pristine distribution archive (gitignored, immutable)
MekTek/                 extracted local 3.1c game distribution (gitignored)
mekTekpatchPacks/       legacy MTX patches/hotfixes (gitignored)
```

Do not commit any of these. Do not use the legacy MTX client as the normal
install path. Do not apply the old incremental patches or `0030a/b/c` hotfixes
to the verified 3.1c baseline.

### 2.1 Pristine test source

`MekTek.rar` is the canonical, immutable test source. It contains the publicly
available MechWarrior 4: Mercenaries distribution at the supported 3.1c patch
version.

- SHA-256:
  `8c8a2c192834a597862cce19e4215d563f93de46872eff8fd1980d47bb417a34`
- Size: 1,820,457,699 bytes
- It is gitignored and must never be committed.
- **Do not modify, rename, repack, delete, or overwrite `MekTek.rar`.**
- The only permitted operation on it is **reading/extracting** it to create a
  fresh test install.
- Never extract over an existing install. Delete the extracted `MekTek/`
  directory first, then extract again.
- Verify the archive hash before and after extraction.
- Verify the extracted install with the core manifest before running tests.

Use the reset helper for every destructive or binary-patch test:

```powershell
pwsh ./tools/Reset-Mw4TestInstall.ps1 -Force
```

This deletes the extracted `MekTek/` folder, re-extracts it from `MekTek.rar`,
and verifies the core hash manifest. It never writes to the RAR. A clean
extraction was verified against the 32-file core manifest with 32/32 matches.

## 3. First release scope

### 3.1 Required in the first public patch

1. Launch the game in **windowed mode at 1024x768**.
2. Keep the **in-game options menu resolution controls working** so the player
   can change resolution from the game UI.
3. Fix the **mouse aiming dead zone**: the game subtracts a dead zone every
   frame, so higher frame rates make aiming worse. Aiming must be smooth at any
   frame rate.
4. Fix the mouse **sticking while the legs turn to follow the torso**.
5. Fix the **torso drifting down and right** when Windows display scaling is
   above 100%.
6. Make **menu buttons accept the first click**; a quick second click must not
   be lost.
7. Start on the **main monitor** and show the game picture immediately instead
   of a black screen until the window is clicked.
8. Fix **texture mipmap garbage** when `options.ini` names a second video card.
9. Fix crashes caused by the game's outdated **`imagehlp.dll`** interaction
   with current NVIDIA drivers.
10. Fix the **menu message-dispatch crash**.
11. A failed display-mode switch must not leave the game in **16-bit color**;
    every successful mode must be 32-bit.
12. **MekTek AntiHack must remain active and untouched.**

### 3.2 Backlog (not first release)

13. **WASD movement with throttle decay, similar to MechWarrior 5:**
    - `W` = throttle up
    - `S` = throttle down; holding past zero goes into reverse
    - `A` = turn left
    - `D` = turn right
    - throttle should decay naturally rather than behaving like a binary key
    - this is an input/gameplay change, not just a keybinding change

    The original request wrote "D is to throttle down" and also "D turns the
    mech right". This guide assumes the intended scheme is **W/S throttle,
    A/D turn**, which matches MW5-style controls. Confirm before implementing.

## 4. Known technical facts

### 4.1 The Hardware Error is a configuration bug

The "Your current hardware or driver setup is not compatible with MechWarrior 4"
dialog can be caused by an invalid `options.ini`, not by an unsupported GPU.

Observed broken values:

```ini
videodriverindex=-1
screenwidth=0
screenheight=0
bitdepth=65535
```

A valid 32-bit mode with a non-zero resolution launches the same install on
Windows 11 with an AMD Radeon AI PRO R9700. `tools/Repair-Mw4Config.ps1` writes
a known-good configuration and backs up the old file first.

### 4.2 `-window` exists, but does not honor the requested size

The game has a real `-window` command-line switch. It was confirmed in the
decompilation tools (`tools/run_debug.ps1`) and by launching the 3.1c build.

However, with the 3.1c binary, `-window` always produced an **806x629 window**
(800x600 client area plus borders), regardless of `options.ini`:

| Requested `screenwidth` | Actual window |
|---:|---:|
| 640 | 806x629 |
| 1024 | 806x629 |
| 1280 | 806x629 |
| 1600 | 806x629 |

Therefore **the first goal is not solved by config alone**. A runtime display
hook or a window/resolution patch is required to get a 1024x768 window.

### 4.3 The game already has an in-game resolution menu

The retail oracle shows five resolution modes:

| Index | Resolution |
|---:|---:|
| 0 | 640x480 |
| 1 | 800x600 |
| 2 | 1024x768 |
| 3 | 1280x1024 |
| 4 | 1600x1200 |

Relevant oracle functions:

- `FUN_0061A160` reads `options.ini` `[graphics options] screenwidth` and maps
  it to a resolution index.
- `FUN_0061AA30` writes `ScreenWidth`/`ScreenHeight` from that index.
- `FUN_0061A510` loads the `Resolution` option for the options UI.

The first release should make this existing menu work correctly on modern
Windows first. Extending the list beyond 1600x1200 is a later change.

### 4.4 The decompilation is a different binary

`C:\repos\Mech4MercDecomp` is a byte-identical oracle for the **retail PR1**
build:

| | Retail PR1 oracle | Our supported 3.1c build |
|---|---|---|
| Version | `50.07.01.2105` | `51.03.01.0030` |
| `MW4Mercs.exe` size | 3,915,776 bytes | 1,448,960 bytes |
| SHA-256 | `deb0cb7f...` | `72bbb1ba...` |

The oracle's VAs, function offsets, and hashes **do not transfer directly** to
the 3.1c binary. Use it as a behavior map and documentation source, not as a
byte-patch source.

Implications for the implementation:

- A static `MW4Mercs.exe` patch based on oracle addresses will be wrong.
- We need pattern scanning, binary diffing, or runtime hooks for 3.1c.
- A runtime hook is preferred for the first release because it avoids
  rewriting the executable and keeps AntiHack untouched.

### 4.5 Do not break online integrity

The retail join handshake hashes the code bytes of 15 resource-loader
functions plus one mission archive entry. Static patches inside those regions
can cause `Player_Reject` on multiplayer join. If we patch the executable, we
must know exactly what is hashed and verify multiplayer behavior.

This is another reason to prefer a runtime `version.dll`-style hook over
static executable edits.

### 4.6 AntiHack

The DX11 renderer mod explicitly leaves MekTek AntiHack active. We must do the
same. Do not disable, bypass, or patch out AntiHack.

## 5. Recommended technical direction

Start with a **patcher + runtime hook** architecture:

```text
mw4patch.exe                  user-facing installer/patcher
  - verifies game hashes
  - backs up options.ini
  - writes known-good config
  - installs version.dll proxy / mw4hook.dll
  - creates a windowed shortcut with -window
  - records installed files and hashes
  - supports uninstall/rollback

version.dll / mw4hook.dll      native runtime hook
  - loaded by MW4Mercs.exe
  - pattern-scans the 3.1c binary
  - hooks display/window, mouse input, menu dispatch, imagehlp
  - applies fixes only when enabled in an ini file
  - does not modify AntiHack
```

The DX11 Renderer Overhaul mod is a **behavior reference** for the desired fixes
and for the `version.dll` loading approach. It is not source code and must not
be copied or redistributed.

## 6. References

### In this repository

| Path | What to read |
|---|---|
| `docs/BASELINE.md` | 3.1c hashes, known-good config, MTX-chain differences |
| `docs/PATCHING.md` | MTX ZIP layout, why the old patcher is unreliable |
| `docs/REBIRTH.md` | Rebirth status and the unverified Phase 2 claim |
| `docs/ROADMAP.md` | Milestones and non-goals |
| `manifests/` | Core and full hash manifests |
| `templates/options.ini` | Known-good configuration template |
| `tools/Get-Mw4InstallInfo.ps1` | Identify a build and validate options.ini |
| `tools/Test-Mw4Manifest.ps1` | Verify files against a manifest |
| `tools/Repair-Mw4Config.ps1` | Write a safe options.ini with backup |
| `tools/New-Mw4Manifest.ps1` | Generate a full manifest |

### Local decompilation reference

`C:\repos\Mech4MercDecomp` — clone of
<https://github.com/mech4decomp/Mech4MercDecomp>. It is frozen; do not modify
it. Use it read-only.

High-value files:

| File | Why |
|---|---|
| `docs/subsystems/config.md` | `options.ini` parsing, option registry, LOD |
| `docs/subsystems/input.md` | keyboard/mouse bitmaps, DirectInput, mouse update |
| `docs/subsystems/ui.md` | frontend/menu paths, cursor, HUD |
| `docs/subsystems/render.md` | display normalization, backbuffer, present |
| `docs/subsystems/platform.md` | window setup, DirectDraw/D3D glue, SafeDisc tail |
| `docs/editor_tools/autoconfig.md` | display-mode probing and `options.ini` keys |
| `docs/GAME_VERSIONING.md` | join handshake and hashed loader regions |
| `tools/run_debug.ps1` | confirms the game's `-window` switch |
| `src/mw4mercs/text/func_0061a160.c` | `screenwidth` -> resolution index |
| `src/mw4mercs/text/func_0061aa30.c` | resolution index -> `ScreenWidth`/`ScreenHeight` |
| `src/mw4mercs/text/func_0061a510.c` | resolution/video options loading |
| `src/mw4mercs/text/func_0070a920.c` | resolution/window sizing |
| `src/mw4mercs/text/func_0070abe0.c` | full display-mode/window setup |
| `src/mw4mercs/text/func_00708030.c` | application/window initialization |

### External references

- **REA** — <https://rea.tools/> — use for binary inspection, function tracing,
  and mapping retail behavior onto the 3.1c binary.
- **DX11 Renderer Overhaul** —
  <https://www.moddb.com/mods/mw4-mercenaries-dx11-renderer-overhaul> —
  behavior reference for the fixes list, not source.
- **MekTek Rebirth** — <https://mektek.net/rebirth> — closed project. Do not
  depend on it. Public Phase 2 status is unverified; see `docs/REBIRTH.md`.

## 7. Engineering rules

1. **Never commit game files.** No executables, DLLs, art, audio, maps, MTX
   files, hotfixes, or extracted game data.
2. **Never modify the local `MekTek/` or `mekTekpatchPacks/` directories**
   unless the task explicitly says to test a change. Back up config files
   before writing. Prefer a disposable copy for destructive tests.
3. **Never modify `C:\repos\Mech4MercDecomp`.** It is a frozen oracle.
4. **Verify before and after.** Use `Test-Mw4Manifest.ps1` and record hashes.
5. **Prefer config and runtime hooks over static exe patches.** Static patches
   risk AntiHack and multiplayer integrity.
6. **Do not touch AntiHack.**
7. **Do not patch the 15 hashed loader regions** without explicit analysis and
   a multiplayer test plan.
8. **Patch data must be reversible.** Back up every changed file and write an
   install record.
9. **Keep the release a single `.exe`.** Native payloads may be embedded as
   resources and extracted into the game directory.
10. **Document every binary change** with:
    - target file name and SHA-256
    - version
    - original bytes / pattern
    - patched bytes / hook behavior
    - evidence and test result
11. **Treat `MekTek.rar` as immutable.** The only allowed operation is
    extraction to create a fresh test install. Never modify, repack, rename,
    delete, or commit it.

## 8. Agent workflow

1. Read this file and the relevant `docs/` page.
2. Verify the current baseline with:

   ```powershell
   pwsh ./tools/Get-Mw4InstallInfo.ps1 -Install "<game dir>"
   pwsh ./tools/Test-Mw4Manifest.ps1 -Install "<game dir>" `
     -Manifest "manifests/mw4-mercs-3.1c-core.sha256"
   ```

3. For any test that may modify the install or exercise a binary patch, reset
   to a fresh extraction first:

   ```powershell
   pwsh ./tools/Reset-Mw4TestInstall.ps1 -Force
   ```

   Do not test against a hand-modified or partially patched install. The reset
   tool deletes only the extracted `MekTek/` folder, re-extracts from the
   immutable `MekTek.rar`, and verifies the core manifest.

4. Reproduce the issue before changing anything.
5. For binary work, record the exact target hash and use REA/x64dbg.
6. Make the smallest possible change. Keep a toggle where practical.
7. Test:
   - game launches
   - first-release behavior works
   - in-game resolution menu still works
   - AntiHack still loads
   - no new crash in the menu or in a mission
8. Update docs and commit only source, docs, manifests, and small patch data.
9. Never leave the game or a test process running.
10. Before handing off, record whether the test started from a fresh
    `MekTek.rar` extraction or from an already-patched install.

## 9. Current status and open questions

### Done

- Git repository established.
- 3.1c baseline identified and hashed.
- Core and full manifests verified.
- Pristine `MekTek.rar` test source recorded (hash and size).
- Reset helper tested against a clean extraction; 32/32 core files matched.
- Config-repair tooling written and tested.
- Hardware Error root cause identified.
- `-window` switch confirmed.
- Windowed mode tested: it ignores requested resolution and uses 800x600.
- Rebirth Phase 2 claim reviewed and marked unverified.

### Open

- Choose the native hook implementation (C++ `version.dll` proxy vs. injected
  DLL vs. launcher-side wrapper).
- Determine whether the window can be resized after creation or whether the
  display-mode creation path must be hooked.
- Map the retail oracle functions to the 3.1c binary with pattern scanning.
- Find the exact 3.1c mouse aim/dead-zone code and the leg-turn mouse lock.
- Determine what causes the Windows-scaling torso drift.
- Identify the menu first-click delay and second-click loss.
- Identify the main-monitor/black-screen behavior.
- Identify the second-video-card mipmap bug.
- Identify the `imagehlp.dll` crash path.
- Identify the menu message-dispatch crash.
- Confirm how to force 32-bit color after a failed mode switch.
- Decide whether the in-game resolution list should be extended beyond
  1600x1200.
- Confirm the WASD throttle-decay design and whether it belongs in the same
  hook as the input fixes.

## 10. Definition of done for the first release

- A user with a verified 3.1c install can run one `.exe`.
- The patcher verifies the game, backs up files, installs the fix layer, and
  reports exactly what changed.
- The game launches in a 1024x768 window.
- The in-game options menu can change resolution.
- The listed first-release fixes are present or explicitly scoped out.
- AntiHack remains active.
- Uninstall restores the original config and removes the hook files.
- No game files are redistributed.
