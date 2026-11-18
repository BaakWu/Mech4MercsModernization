# ModDB upload kit — Modernization Patch 1.7.1

ModDB has no upload API, so the upload itself must be done in a browser
signed in to a ModDB account. Everything below is paste-ready.

- **File to upload:** `dist\MW4Mercs-ModernizationPatch-Setup-1.7.1.exe`
  (679 KB, SHA-256 `48cb3344d63509ff134193acba0be996cec0ed289b52aed17548fb41b42b5e35`
  — verified identical to the GitHub release asset)
- **Icon:** `docs/images/logo-1024x768.png` (or the transparent variant)
- **License:** MIT
- **Homepage:** https://github.com/BaakWu/Mech4MercsModernization
- There is no ModDB page for this mod yet (checked 2026-11-17), so create a
  new mod page under the existing game page
  [Mechwarrior 4: Mercenaries](https://www.moddb.com/games/mechwarrior-4-mercenaries).

## Steps

1. Sign in at moddb.com (register first if needed).
2. Control Panel → **Mods** → **Add New Mod** (or the game page → Mods → Add mod).
3. Fill the mod page fields from "Mod page fields" below; upload the logo as
   the mod icon/box art (ModDB requires at least one image before publishing).
4. Save/publish the page (new pages may sit in a short staff review).
5. Control Panel → **Files** → **Add New File**; fill from "File entry fields"
   and upload the setup exe from `dist\`.
6. Optional but recommended: Control Panel → **Articles** → post the
   announcement draft so followers get notified.
7. Optional: add 1–2 in-game screenshots to the mod page (the automation
   capture, `[debug] automation=1` + `tools/Send-Mw4Automation.ps1`, can grab
   clean frames).

## Mod page fields

| Field | Value |
|---|---|
| Mod name | MechWarrior 4 Mercenaries Modernization Patch |
| Parent game | Mechwarrior 4: Mercenaries |
| Release status | Released |
| Release date | 17 Nov 2026 |
| Genre | First Person Shooter |
| Language | English |
| Homepage | https://github.com/BaakWu/Mech4MercsModernization |
| Icon | `docs/images/logo-1024x768.png` |

### Short description

Runtime patch for MechWarrior 4: Mercenaries 3.1c (MekTek): windowed and borderless modes, a working resolution menu, and modern-Windows fixes (frame rate, audio, terrain, DPI scaling). No game files changed. Not yet tested with AntiHack or multiplayer.

### Full description

```markdown
An independent patch for **MechWarrior 4: Mercenaries 3.1c**
(MekTek, `51.03.01.0030`). It makes the original game launch and play
correctly on modern Windows and adds a few optional quality-of-life
features — without replacing the engine or shipping game files. The game keeps its original 2002 look; this is a compatibility
patch, not a graphics overhaul.

## What the patch does

### Always installed

- a **Windowed** checkbox in Options > Video: a window at the chosen
  resolution, or borderless fullscreen on the main monitor (Alt+Enter toggles)
- the in-game **Options > Resolution controls keep working**
- always **32-bit color**, never a 16-bit fallback
- correct mouse and display behavior with **Windows display scaling above 100%**
- **sharper menus**: the 800x600 menus, MechLab and text are scaled up with a
  Lanczos filter, readable on 1080p and larger screens
- a **weight-class filter in the MechLab** (All / Light / Medium / Heavy /
  Assault / Elemental)
- a **MODERNIZED / PATCH vX.Y.Z badge** in the Main Menu, so you can see the
  patch is active and which version
- **sharper HUD text**: the 'Mech names in the lance roster are redrawn from
  the game's own MicroStyle font at your resolution
- **audio keeps playing** when you click outside the game window; input still pauses
- fixes for four bugs in the stock game on current Windows: low mission frame
  rate, garbled audio, see-through terrain, and a window that froze on a
  second monitor

### Options on Options > Game (off by default)

- **Throttle Decay**: the throttle eases back to zero when W/S is released.
- **Gyro Aim** (with the MW5 controls): while the legs turn, the aim keeps its
  direction until the torso reaches its twist limit.

### Optional components (chosen in the installer)

- **MechWarrior 5-style controls**: W/S throttle, MW5 key layout, the mouse
  aim point and Gyro Aim. Unticked keeps MW4's controls.
- **Full company in campaign missions**: up to 7 lancemates where a mission
  allows any (experimental).

## Requirements

- MechWarrior 4: Mercenaries **3.1c** (MekTek), `MW4Mercs.exe` `51.03.01.0030`.
  The installer refuses anything else.
- Windows 10 or 11.

## Install

1. Run `MW4Mercs-ModernizationPatch-Setup-1.7.1.exe`.
2. Check the game folder; tick the optional components you want.
3. Click **Install** — or **Update** over an earlier version (your settings and
   resolution are kept) — and start the game as usual.

Run the installer again to change the options, or to **Uninstall**. No game
file is changed. Uninstalling does not undo what the game saved while the
patch was installed: pilots, campaign progress and key bindings stay as they
are.

## Notes

- **Not yet tested:** MekTek AntiHack and multiplayer with the patch
  installed. We don't know yet whether they work.
- Windows SmartScreen may warn about an unsigned download, and antivirus
  programs sometimes flag `dinput.dll` / `ddraw.dll` in a game folder. The
  full source is open and the patch is small and documented.
- Source and issue tracker: <https://github.com/BaakWu/Mech4MercsModernization>
- License: MIT. This patch ships no game files.
```

## File entry fields

| Field | Value |
|---|---|
| File name | Modernization Patch 1.7.1 |
| Mod | MechWarrior 4 Mercenaries Modernization Patch |
| Category | Patch |
| Version | 1.7.1 |
| Platform | Windows |
| License | MIT License |
| File | `dist\MW4Mercs-ModernizationPatch-Setup-1.7.1.exe` |

### File description

```markdown
GUI installer for version 1.7.1 of the Modernization Patch.
Run it, point it at your MechWarrior 4: Mercenaries 3.1c folder, tick the
optional components you want, and click Install.

SHA-256:
48cb3344d63509ff134193acba0be996cec0ed289b52aed17548fb41b42b5e35  MW4Mercs-ModernizationPatch-Setup-1.7.1.exe
```

### File changelog

```markdown
1.7.1

- Aiming no longer turns your 'Mech (issue #31): with the mouse aim point
  (MW5-style controls), the game's automatic torso centering turned the legs
  toward the torso a moment after you aimed, so the whole 'Mech rotated on
  its own. While the aim point is in use, automatic centering is now off:
  aiming twists only the torso, and the legs turn only when you turn (A/D)
  or ask them to. Center Legs To Torso (Shift+Space) and Center Torso To
  Legs (C) work as before. If you preferred the old behavior, set
  `auto_center=1` under `[aim]` in `mw4hook.ini`.

1.7.0

- MechLab weight-class filter, sharp 'Mech names in the lance roster, Main
  Menu version badge, salvage camouflage, full company in campaign missions
  (optional), background audio, the aim line, sharper menus, camouflage for
  new pilots, Gyro Aim, the mouse aim point and MW5-style controls
  (optional), the Windowed checkbox (Alt+Enter), Throttle Decay, and the
  frame rate, audio, terrain, second-monitor and display-scaling fixes.
```

## Announcement article draft

Title: **Modernization Patch 1.7.1 — aiming no longer turns your 'Mech**

```markdown
Version 1.7.1 of the MechWarrior 4 Mercenaries Modernization Patch is out.

The one fix in this release: with the mouse aim point (MW5-style controls),
the game's automatic torso centering turned the legs toward the torso a
moment after you aimed, so the whole 'Mech rotated on its own. While the aim
point is in use, automatic centering is now off — aiming twists only the
torso, and the legs turn only when you turn (A/D) or ask them to
(Shift+Space / C). Old behavior is one line in `mw4hook.ini` (`auto_center=1`).

Everything from 1.7.0 is included: the MechLab weight-class filter, sharp
'Mech names in the lance roster, the Main Menu badge, salvage camouflage,
full company in campaign missions (optional), background audio, the aim
line, sharper menus, Gyro Aim, the mouse aim point and MW5-style controls
(optional), the Windowed checkbox (Alt+Enter), Throttle Decay, and the
frame rate, audio, terrain, second-monitor and display-scaling fixes.

Grab the installer from the Files tab. It only supports 3.1c and changes no
game file. Source:
https://github.com/BaakWu/Mech4MercsModernization
```

## Notes for future releases

Update this file's version strings and the SHA-256 after each
`tools/New-Mw4Release.ps1` run, then add a new **File** entry (and optionally
an article) on the same mod page.
