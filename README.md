# MechWarrior 4: Mercenaries — Modern Patch

An independent, reversible patch project for **MechWarrior 4: Mercenaries
3.1c** (`51.03.01.0030`).

The goal is to make the original game launch and play correctly on modern
Windows without replacing the engine, breaking AntiHack, or shipping game
files.

> **Status: pre-release.** There is no public patch download yet. The current
> repository contains the verified baseline, hash manifests, configuration
> repair tools, and the technical plan for the first patch.

## Why this exists

MechWarrior 4: Mercenaries was built for DirectX 7, 32-bit Windows, and display
hardware from 2002. On modern systems it can fail in ways that look like
hardware incompatibility but are often configuration or compatibility bugs.

The project’s first release focuses on the problems that matter most for
getting the game running properly:

- launch in **windowed mode at 1024x768**
- keep the **in-game Options > Resolution controls working**
- smooth **mouse aiming** at any frame rate
- fix the mouse **sticking while the legs follow the torso**
- fix **torso drift** when Windows display scaling is above 100%
- make menu buttons accept the **first click**
- start on the **main monitor** without a black screen until clicked
- fix **texture mipmap corruption** when a second video card is configured
- fix crashes involving the outdated **`imagehlp.dll`** path
- fix the **menu message-dispatch crash**
- prevent a failed mode switch from leaving the game in **16-bit color**
- keep the game’s **AntiHack active and untouched**

The first release does **not** replace the renderer, reimplement the engine, or
change the single-player campaign.

### Fix reference, not a renderer replacement

The compatibility fixes in this project are based on the behavior documented by
the **DX11 Renderer Overhaul** mod:

<https://www.moddb.com/mods/mw4-mercenaries-dx11-renderer-overhaul>

That project is a **behavior reference for its fixes only**. We are not
adopting its renderer or any of its visual changes.

The game must continue to look like the original 2002 release:

- original DirectX 7 rendering path
- original textures, models, lighting, water, terrain, skies, effects, and HUD
- original 4:3 display modes
- no HDR, TAA, baked GI, new shadows, new water, terrain or texture replacement,
  grass/clutter, model replacement, widescreen/FOV changes, or HUD restyling
- no 64-channel audio change

The only display change in the first release is compatibility-oriented:
windowed 1024x768 plus a working in-game resolution menu. That is not a visual
overhaul.

| Fix demonstrated by the reference mod | Adopted? |
|---|---|
| Mouse aiming dead-zone fix | **Yes** |
| Mouse no longer sticks while the legs follow the torso | **Yes** |
| Torso drift with Windows display scaling above 100% | **Yes** |
| Menu buttons accept the first click; quick second click is not lost | **Yes** |
| Start on the main monitor without a black screen until clicked | **Yes** |
| Texture mipmap garbage when a second video card is configured | **Yes** |
| Crashes from the outdated `imagehlp.dll` path | **Yes** |
| Menu message-dispatch crash | **Yes** |
| Failed mode switch leaving the game in 16-bit color | **Yes** |
| AntiHack left active and untouched | **Yes** |
| DX11 renderer, HDR, TAA, shadows, water, terrain, textures, sky, models | **No** |
| Widescreen/ultrawide/FOV/HUD changes | **No** |
| 64-channel audio change | **No** |

## Planned user experience

The release will be a single **`mw4patch.exe`**:

1. Verify the supported game build by hash.
2. Back up configuration and any files it changes.
3. Write a known-good display configuration.
4. Install an optional runtime fix layer next to the game executable.
5. Create a windowed launch shortcut.
6. Record exactly what was installed.
7. Provide an uninstall/rollback path.

The fix layer is loaded at runtime rather than rewriting `MW4Mercs.exe`. That
keeps the patch reversible and avoids touching the game’s AntiHack or the code
regions used by the multiplayer integrity check.

## Supported game

| Field | Value |
|---|---|
| Game | MechWarrior 4: Mercenaries |
| Version | `3.1c` |
| Executable version | `51.03.01.0030` |
| Executable SHA-256 | `72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41` |

You must supply your own legally obtained copy of the game. This repository
does not contain the game, its assets, or a modified executable.

## What works today

The repository currently provides PowerShell tooling for the baseline:

- identify a game install and its version
- verify core and full file manifests
- detect an invalid `options.ini`
- write a known-good `options.ini` with backup
- generate a full SHA-256 manifest

### Inspect an install

```powershell
pwsh ./tools/Get-Mw4InstallInfo.ps1 -Install "C:\Games\MechWarrior4Mercs"
```

### Verify the core files

```powershell
pwsh ./tools/Test-Mw4Manifest.ps1 `
  -Install "C:\Games\MechWarrior4Mercs" `
  -Manifest "manifests/mw4-mercs-3.1c-core.sha256"
```

### Verify the full baseline

```powershell
pwsh ./tools/Test-Mw4Manifest.ps1 `
  -Install "C:\Games\MechWarrior4Mercs" `
  -Manifest "manifests/mw4-mercs-3.1c-full.sha256"
```

### Repair display configuration

```powershell
pwsh ./tools/Repair-Mw4Config.ps1 `
  -Install "C:\Games\MechWarrior4Mercs" `
  -Width 1024 -Height 768
```

The script backs up the existing `options.ini` before writing a new one.

## Hardware Error troubleshooting

A common cause of this dialog:

> Your current hardware or driver setup is not compatible with MechWarrior 4.

is an invalid `options.ini`, especially:

```ini
videodriverindex=-1
screenwidth=0
screenheight=0
bitdepth=65535
```

Try `tools/Repair-Mw4Config.ps1` before assuming the GPU is unsupported. The
script writes a non-zero resolution, 32-bit color, and a valid video driver
index.

Note: the base game’s `-window` switch currently opens an 800x600 window
regardless of the configured resolution. The first patch release is intended
to fix that; configuration alone is not enough.

## Backlog

- **WASD movement with throttle decay**, similar to MechWarrior 5:
  - `W` throttle up
  - `S` throttle down; holding past zero goes into reverse
  - `A` turn left
  - `D` turn right
  - throttle decays naturally instead of behaving like a binary key

This is a gameplay/input change and is not part of the first release.

## Repository layout

| Path | Purpose |
|---|---|
| `AGENTS.md` | Agent/contributor guide, release scope, technical references |
| `docs/BASELINE.md` | Supported build identity, hashes, known-good configuration |
| `docs/PATCHING.md` | Legacy patch format and modern patching plan |
| `docs/ROADMAP.md` | Milestones and non-goals |
| `manifests/` | Core and full SHA-256 baselines |
| `templates/` | Known-good configuration template |
| `tools/` | Inspect, verify, repair, and manifest scripts |

## Development

Current tools require **PowerShell 7**.

The planned patcher will be a self-contained Windows executable. The planned
runtime fix layer will be a native DLL loaded beside the game executable. Build
and packaging instructions will be added with the first patcher scaffold.

Before working on binary behavior, read `AGENTS.md`. In particular:

- do not commit game files
- do not modify the verified baseline or legacy patch media
- do not touch AntiHack
- prefer runtime hooks over static executable patches
- verify hashes before and after any change

## Contributing

Good contributions at this stage:

- reproducing modern-Windows launch and input bugs
- testing the existing configuration repair on different GPUs and display
  layouts
- documenting exact game versions and hashes
- mapping the retail decompilation behavior onto the 3.1c binary
- implementing small, reversible fixes with tests

Please keep changes focused and document any binary-level work with the target
file hash, original bytes or pattern, and observed behavior.

## Roadmap

See `docs/ROADMAP.md` for the milestone plan. The short version:

1. Verify the baseline and fix configuration. **Done.**
2. Build the `mw4patch.exe` scaffold and release pipeline.
3. Implement windowed 1024x768 and display fixes.
4. Implement mouse, menu, and crash fixes.
5. Add optional WASD throttle-decay controls.
6. Ship a single reversible patch executable.

## Legal

This is an unofficial community project. It is not affiliated with or endorsed
by Microsoft, FASA Interactive, Cyberlore Studios, Catalyst Game Labs, The
Topps Company, or any current rights holder.

This repository contains no game executables, DLLs, art, audio, maps, or other
proprietary game content. You must supply your own legally obtained copy of the
game. The original code and documentation in this repository are licensed under
the MIT License; that license does not grant rights to the game or its assets.
See `NOTICE.md` for details.
