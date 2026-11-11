# MechWarrior 4: Mercenaries — Modern Patching

An independent, offline-first patching and preservation project for
**MechWarrior 4: Mercenaries**.

The goal is not to rewrite the engine. The goal is to make the existing game
installable, verifiable, and runnable on modern Windows without depending on
dead MekTek services or undocumented patch state.

## Canonical baseline

We use the publicly available **MW4 Mercs MekTek 3.1c** distribution from
ModDB as the baseline:

- Game version: `51.03.01.0030`
- `MW4Mercs.exe` SHA-256:
  `72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41`
- Local extracted path:
  `MekTek/Mechwarrior Mercenaries - Mektek Mekpak`
- Status: launches on Windows 11 with an AMD Radeon AI PRO R9700 once
  `options.ini` contains valid display settings.

The game data is **not** part of this repository. `MekTek/` and
`mekTekpatchPacks/` are ignored by Git.

## What we established

The original instructions in this folder used the old MTX chain:

```text
all -> 0028 -> 0029 -> 0030
```

That chain depends on `mtxservice.mektek.net`, which no longer resolves. The
MTX client falls back to offline mode and can still apply local `.mtx` files,
but it is not reliable:

- A patch attempt failed on `MW4MekStats.dll`, then appeared to succeed on a
  retry.
- The resulting install reported `51.03.01.0030`, but its executable and
  several DLLs did **not** match the known-good ModDB 3.1c build.
- The install wrote `options.ini` with `screenwidth=0`, `screenheight=0`,
  `bitdepth=65535`, and `videodriverindex=-1`.
- Launching that install produced the **Hardware Error** dialog.

Copying a valid `options.ini` into the same install made it launch. The GPU is
not the problem; the invalid display configuration is.

The ModDB 3.1c folder matches the independently working install and is the
baseline this repository will verify against.

## What is in the repo

| Path | Purpose |
|---|---|
| `docs/BASELINE.md` | Identity and hashes of the canonical 3.1c base |
| `docs/PATCHING.md` | Legacy MTX format, why it is unreliable, and the modern replacement plan |
| `docs/REBIRTH.md` | Public status of MekTek Rebirth, including the Phase 2 claim |
| `docs/ROADMAP.md` | Milestones and non-goals |
| `manifests/` | Known-good core and full hash manifests |
| `templates/options.ini` | Known-good display/configuration template |
| `tools/` | PowerShell inspection, verification, and config-repair tools |

## Quick start

Inspect an install:

```powershell
pwsh ./tools/Get-Mw4InstallInfo.ps1 `
  -Install "MekTek/Mechwarrior Mercenaries - Mektek Mekpak"
```

Repair a broken `options.ini` (backs up the old one first):

```powershell
pwsh ./tools/Repair-Mw4Config.ps1 `
  -Install "MekTek/Mechwarrior Mercenaries - Mektek Mekpak" `
  -Width 1920 -Height 1080
```

Verify the core files:

```powershell
pwsh ./tools/Test-Mw4Manifest.ps1 `
  -Install "MekTek/Mechwarrior Mercenaries - Mektek Mekpak" `
  -Manifest "manifests/mw4-mercs-3.1c-core.sha256"
```

Verify the full baseline (1,716 distribution files):

```powershell
pwsh ./tools/Test-Mw4Manifest.ps1 `
  -Install "MekTek/Mechwarrior Mercenaries - Mektek Mekpak" `
  -Manifest "manifests/mw4-mercs-3.1c-full.sha256"
```

Generate a full manifest for a clean install:

```powershell
pwsh ./tools/New-Mw4Manifest.ps1 `
  -Install "MekTek/Mechwarrior Mercenaries - Mektek Mekpak" `
  -Output "artifacts/mw4-mercs-3.1c-full.sha256"
```

## Legacy patch packs

`mekTekpatchPacks/` contains the old MTX base, incremental patches, hotfixes,
and installers. It is kept locally as research material only.

For the normal install path:

- **Do not** run `SetupMTX*.exe`.
- **Do not** apply the incremental MTX patches to the ModDB 3.1c base.
- **Do not** apply the `0030a/b/c` hotfixes unless we are deliberately
  reconstructing an older base in a disposable copy.

The ModDB 3.1c base is already the known-good final state. The old patch chain
is useful for understanding the delta format and for reconstructing earlier
versions, not for producing a clean 3.1c install.

## Non-goals

- Redistributing the game or its assets.
- Reimplementing the engine.
- Replacing MekTek Rebirth or competing with it.
- Shipping a modified executable without a clear, auditable patch record.

## Legal

MechWarrior, BattleTech, MechWarrior 4, and related marks belong to their
respective owners. This repository contains no game executables, DLLs, art,
audio, maps, or other proprietary game content. You must supply your own
legally obtained copy of the game. See `NOTICE.md`.
