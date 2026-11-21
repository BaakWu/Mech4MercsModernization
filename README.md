# MechWarrior 4 Mercenaries Modernization Patch

> I DON'T GIVE A F\*\*\* ABOUT THE F\*\*\*ING CODE! I just want to download this
> stupid patch and stomp around in my Atlas. WHY IS THERE CODE??? MAKE A
> F\*\*\*ING .EXE FILE AND GIVE IT TO ME. These techs think every pilot is a
> developer and understands code. Well I am not, I'm a MechWarrior. I only know
> how to download, install and alpha strike. SO WHY IS THERE CODE? Make an EXE
> file and give it to me. STUPID SMELLY TECHS.
>
> — a MechWarrior, probably

**Here's your .exe:** 

1. Open the [latest release](https://github.com/BaakWu/Mech4MercsModernization/releases/latest).
2. Download `MW4Mercs-ModernizationPatch-Setup-<version>.exe` under **Assets**.
3. Run it, check that it found your game folder, and click **Install**. Then
   start the game as usual.

---

An independent patch for **MechWarrior 4: Mercenaries 3.1c**
(`51.03.01.0030`).

It makes the original game launch and play correctly on modern Windows and
adds a few optional quality-of-life features, without replacing the engine or
shipping game files. It has not yet been tested with MekTek AntiHack or in
multiplayer.

> **Status:** releases are on the GitHub Releases page as a GUI installer,
> `MW4Mercs-ModernizationPatch-Setup-<version>.exe`.

## Why this exists

MechWarrior 4: Mercenaries was built for DirectX 7, 32-bit Windows, and display
hardware from 2002. On modern systems it can fail in ways that look like
hardware incompatibility but are often configuration or compatibility bugs.

## What the patch does

### Always installed

- a **Windowed** checkbox in Options > Video: a window at the chosen
  resolution, or borderless fullscreen on the main monitor (Alt+Enter toggles)
- the **in-game Options > Resolution controls** keep working
- always **32-bit color**, never a 16-bit fallback
- correct mouse and display behavior with Windows display scaling above 100%
- **joysticks, gamepads and wheels keep working**: controllers are listed on
  their own, so a headset or receiver that crashes Windows' DirectInput while
  it walks every HID device can no longer hide the joystick (issues #34, #40)
- **sharper menus**: the 800x600 menus, MechLab and text are scaled up with a
  Lanczos filter, readable on 1080p and larger screens (issue #20)
- a **weight-class filter in the MechLab**: a Class droplist (All / Light /
  Medium / Heavy / Assault / Elemental) next to the chassis list, so you no
  longer scroll through every chassis; choosing a class loads its first
  'Mech, and in the campaign it only offers the classes you own (issue #28)
- a **MODERNIZED / PATCH vX.Y.Z badge** in the empty plate at the bottom
  left of the Main Menu, so you can see the patch is active and which
  version (issue #24)
- **sharper HUD text**: the 'Mech names in the lance roster are redrawn from
  the game's own MicroStyle font at your resolution, as sharp as the pilot
  names beside them, and the other small white HUD labels are smoothed
  (issue #27); the rest of the HUD is unchanged
- **audio keeps playing** when you click outside the game window; input still
  pauses (issue #26)
- fixes for four bugs in the stock game on current Windows: low mission frame
  rate, garbled audio, see-through terrain, and a window that froze on a
  second monitor

Gameplay settings, each an On/Off switch on Options > Game (off by default):

- **Throttle Decay**: the throttle eases back to zero when W/S is released.
- **Gyro Aim** (with the MW5 controls, issue #17): while the legs turn, the
  aim keeps its direction until the torso reaches its twist limit. It lets
  you aim faster while turning, so it is a per-player choice.

**Camouflage for new pilots** (issue #19): Roster > New > Join Regiment has
a Camouflage list (default None). MekTek's template profile gave every new
unit "7-Ops - Pink". Roster > Update changes a pilot's camouflage for the
'Mechs it gets from then on (as does Options > Multiplayer > Skin).

### Optional: MechWarrior 5-style controls

Ticked in the installer:

- faster W/S throttle and an MW5-style key layout (X full stop, P power,
  numpad presets, mouse aims the torso, …). New pilots get it automatically;
  for an existing pilot press DEFAULT in Options > Controls.
- **mouse aim point** (issue #15): the mouse moves an aim point, marked by a
  small circle, and the torso turns to it at its own speed and stops on it.
  A thin green line joins the reticle to the aim point while the torso is
  still turning (issue #23). There is no drift after the mouse stops, and a
  fast flick lands where a slow one does.

Leave it unticked to keep MW4's controls. Details in `docs/HOOK.md`.

### Optional: full company in campaign missions (experimental)

Ticked in the installer (issue #21): campaign missions that allow lancemates
allow **7**, so you can field 8 'Mechs. The pre-mission screen opens the
**Secondary Lance**; it needs a lance commander, and the pilots and 'Mechs
come from your company as usual. Missions are not rebalanced for it.

The patch does **not** replace the renderer, reimplement the engine, or
change the single-player campaign.

### Not a renderer replacement

The game must continue to look like the original 2002 release:

- original DirectX 7 rendering path
- original textures, models, lighting, water, terrain, skies, effects, and HUD
- original 4:3 display modes
- no HDR, TAA, baked GI, new shadows, new water, terrain or texture replacement,
  grass/clutter, model replacement, widescreen/FOV changes, or HUD restyling
- no 64-channel audio change

The display changes are compatibility-oriented: windowed or borderless play at
the chosen resolution, with the original 4:3 picture scaled to fit. That is
not a visual overhaul.

## Installing

Run `MW4Mercs-ModernizationPatch-Setup-<version>.exe`:

1. It finds the game folder (or use Browse) and checks that it is
   MechWarrior 4: Mercenaries 3.1c.
2. Windowed mode and the display fixes are always installed, and so is the
   Throttle Decay setting (off until you switch it on in the game). Tick
   **MechWarrior 5-style controls** if you want them.
3. Click **Install**, then start the game as usual. It opens in a window; set
   the size and the Windowed option in Options > Video.

Run the installer again to install a newer version or switch the MW5 controls
on or off (**Update**; your resolution and settings are kept), or to
**Uninstall**. If the game is under Program Files, the installer offers to
restart itself as administrator.

The fix layer is loaded at runtime rather than rewriting `MW4Mercs.exe`. That
avoids editing the code regions used by the multiplayer integrity check;
whether the patch works with AntiHack and multiplayer has not been tested
yet. No game file is changed, except that the game's old
`Assets\Binaries\imagehlp.dll` is set aside (renamed), because it stops NVIDIA
graphics cards from working with the game; uninstalling puts it back.
Uninstalling does not undo what the game saved
while the patch was installed: pilots, campaign progress, key bindings and
camouflage stay as they are, and `options.ini` goes back to
the copy taken at install, so settings changed since then are lost.

Build the installer with:

```powershell
pwsh ./tools/New-Mw4Release.ps1     # dist\MW4Mercs-ModernizationPatch-Setup-<version>.exe
```

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

### Runtime hook

The native hook in `src/hook/` is a `dinput.dll` proxy plus a `ddraw.dll`
proxy. Together they:

- add the **Windowed** checkbox to Options > Video and apply it instantly
  (framed window or borderless fullscreen; Alt+Enter toggles)
- run missions at the chosen resolution and scale the 800x600 menus, MechLab
  and movies into the window with the 4:3 shape kept
- relaunch a raw `MW4Mercs.exe` launch with `-window` before the game starts
- fix the per-frame display rebuild (frame rate, audio), the unbound depth
  buffer (see-through terrain) and the frozen window on a second monitor
- add the gameplay options, the mouse aim point, the camouflage choice and
  background audio described above
- do not modify `MW4Mercs.exe` or any game file on disk

Build, test and technical details are in `docs/HOOK.md`; open problems are in
`docs/KNOWN-ISSUES.md`.

### Command-line installer (testing)

`src/patcher/` also builds `mw4patch.exe`, the command-line version of the
installer (same code), used for testing; `--mw5` / `--no-mw5` choose the
controls. It embeds the hook files, verifies the supported 3.1c build against
the core manifest, backs up `options.ini`, installs the hook, creates a
windowed shortcut, and writes an install record with uninstall support.

```powershell
pwsh ./src/patcher/build.ps1

.\dist\mw4patch.exe verify  "MekTek\Mechwarrior Mercenaries - Mektek Mekpak"
.\dist\mw4patch.exe install "MekTek\Mechwarrior Mercenaries - Mektek Mekpak" --yes
.\dist\mw4patch.exe uninstall "MekTek\Mechwarrior Mercenaries - Mektek Mekpak" --yes
```

See `docs/PATCHER.md` for the install record, backups, and current limitations.

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

Note: the base game’s `-window` switch opens an 800x600 window regardless of
the configured resolution; configuration alone cannot change that. The runtime
hook does.

## Repository layout

| Path | Purpose |
|---|---|
| `AGENTS.md` | Agent/contributor guide, release scope, technical references |
| `docs/BASELINE.md` | Supported build identity, hashes, known-good configuration |
| `docs/HOOK.md` | Runtime hook: display, controls, aim point, camouflage, audio |
| `docs/KNOWN-ISSUES.md` | Problems found in play testing and their status |
| `docs/PATCHER.md` | Development `mw4patch.exe`: verification, install, backups, uninstall |
| `docs/PATCHING.md` | Legacy patch format and modern patching plan |
| `docs/ROADMAP.md` | Milestones and non-goals |
| `manifests/` | Core and full SHA-256 baselines |
| `src/hook/` | Native `dinput.dll` and `ddraw.dll` proxies (the runtime hook) |
| `src/patcher/` | Installer: GUI release build and command-line `mw4patch.exe` |
| `templates/` | Known-good configuration template |
| `tools/` | Inspect, verify, repair, window-test, and manifest scripts |

## Development

Tools require **PowerShell 7**; the hook and installer build with the Visual
Studio 2022 C++ tools (x86). `pwsh ./tools/New-Mw4Release.ps1` builds both and
writes the release installer to `dist\`.

Before working on binary behavior, read `AGENTS.md`. In particular:

- do not commit game files
- do not modify the verified baseline or legacy patch media
- do not touch AntiHack
- prefer runtime hooks over static executable patches
- verify hashes before and after any change

## Contributing

Good contributions at this stage:

- testing windowed and borderless play on different GPUs and monitor setups
- testing the existing configuration repair on different GPUs and display
  layouts
- documenting exact game versions and hashes
- mapping the retail decompilation behavior onto the 3.1c binary
- implementing small fixes, each with an off switch and tests

Please keep changes focused and document any binary-level work with the target
file hash, original bytes or pattern, and observed behavior.

## Roadmap

See `docs/ROADMAP.md` for the milestone plan. The short version:

1. Verify the baseline and fix configuration. **Done.**
2. Implement windowed mode and the Windowed checkbox. **Done.**
3. Release build: GUI installer (`tools/New-Mw4Release.ps1`). **Done.**
4. Publish releases. **Done** (1.0.0 onward).
5. Optional MW5-style controls, Throttle Decay, mouse aim point and Gyro Aim.
   **Done.**
6. 8-'Mech campaign lances (#21), optional. **Done** (experimental).
7. Open: campaign co-op (#16), AntiHack and multiplayer verification (#12).

## Legal

This is an unofficial community project. It is not affiliated with or endorsed
by Microsoft, FASA Interactive, Cyberlore Studios, Catalyst Game Labs, The
Topps Company, or any current rights holder.

This repository contains no game executables, DLLs, art, audio, maps, or other
proprietary game content. You must supply your own legally obtained copy of the
game. The original code and documentation in this repository are licensed under
the MIT License; that license does not grant rights to the game or its assets.
See `NOTICE.md` for details.
