# Release regression test

Issue #45. Run it before every release:

```powershell
pwsh ./tools/New-Mw4Release.ps1          # builds dist\MW4Mercs-ModernizationPatch-Setup-<version>.exe
pwsh ./tools/Test-Mw4Regression.ps1      # ~25-45 min, mostly the full-company mission
```

The release may be published only when the summary says **0 failed**.
Skipped is allowed only for the controller scenario when no controller is
plugged in (and say so in the release notes of a controller change).

## What it does

One command, no manual steps. It drives the real game from inside the
process (`[debug] automation=1`, `src/hook/automation.cpp`): the real
cursor, keyboard focus and desktop are never touched, so the machine stays
usable, but **do not start the game yourself while it runs** (it would be
driven too, and the test refuses to start while `MW4Mercs.exe` is running
unless `-Force` closes it).

1. Backs up `RESOURCE\Pilots` (the test install holds real pilot saves) to
   the output folder, resets the test install from `MekTek.rar`
   (`Reset-Mw4TestInstall.ps1`, core manifest verified) and installs the
   release candidate with the GUI installer (MW5 controls and full company
   ticked), clicking it through window messages.
2. Adds `[debug] automation=1` and the intro skip (test only), starts the game
   from the patch's shortcut and plays through every feature (below).
3. Removes the test settings, runs the installer again (update), uninstalls,
   compares the folder with `manifests/mw4-mercs-3.1c-full.sha256`, and puts
   the pilot saves back.

It never leaves `MW4Mercs.exe` running. Everything is written to
`artifacts\regression\<time>\`:

| File | Content |
|---|---|
| `summary.md` | release version, installer SHA-256, a PASS/FAIL/SKIP row per scenario |
| `results.json` | the same, for tools |
| `run.log` | every step with its time |
| `logs\` | the hook logs (`mw4hook-<pid>.log`, `mw4hook-ddraw-<pid>.log`) |
| `shots\` | frame captures (menus, Instant Action lance, aim line, missions, MechLab) |
| `installer-*.log` | the installer's log box and result for install, update, uninstall |
| `manifest-after-uninstall.txt` | the full-manifest comparison |

Exit code 0 means nothing failed.

Options: `-Installer <exe>` (default: `dist\` setup of the current
`kToolVersion`), `-Cli` (use `dist\mw4patch.exe` instead of the GUI),
`-Force` (close a running game), `-GameOnly` (development: skip
reset/install/update/uninstall and drive the game in the current patched
install), `-MissionMinutes`, `-MissionAttempts`, `-DriveSeconds`,
`-Resolution`.

## Scenarios

Every check reads the hook log or a frame capture; nothing is judged by eye.
The thresholds prove that each feature works, not how precisely (owner,
2026-11-26): each sits well clear of both a passing run and the broken
behaviour.
The hook writes the test traces only with `[debug] automation=1`
(`docs/HOOK.md`, "Testing without touching the desktop").

| Id | Checks | How |
|---|---|---|
| INST-1 | install (#35) | install record, `imagehlp.dll` renamed to `imagehlp.dll.mw4patch-disabled`, `mw5=1 track_pointer=1 full_company=1 decay=0` |
| DISP-1 | launch | `main window created`: client 1024x768, framed, centred in the primary monitor's work area; `-window` from the shortcut |
| CAMO-1 | new pilot camouflage (#19) | Roster > New > Kell Hounds, Camouflage `1-Ops - Red`; the campaign's 'Mechs read skin `1`, never `7` (Pink); MechLab frame |
| LAB-3 | campaign MechLab filter (#28) | Class Light lists only class-0 rows (`mechlab:` trace) |
| DISP-2 | resolution above 2048 (#38) | Options > Video `2560 x 1080` (row 23); mission mode `2560x1080x32`; the mission frame has content to its edges |
| DISP-3 | Windowed (#1) | checkbox off -> borderless, saved `windowed=false`; Alt+Enter in the menus -> framed, saved `true`; Alt+Enter off and on in a mission (borderless frame as wide as the monitor) |
| LAB-1 | Instant Action MechLab filter (#28) | each class lists only rows of that class; All lists them all |
| LAB-2 | filtered pick (#28) | Avatar picked from the Heavy list and from the full list: same variant count; the left panel and stats match (at most 2% of pixels differ) |
| IA-1 | lance | Mission Play, Eaton - Merc Alley, Regular, Day; player in a Vulture, 7 Expert lancemates in Archers (shell traces 71, 73/74, 70/7, 5, 55, 56) |
| THR-1 | decay off | W to full, S to reverse: each holds after release; X stops |
| AIM-1..3 | aim point (#15, #23, #31) | 1500 counts left: the torso reaches the aim point; the green line (exactly `40 FF 40`) is drawn while turning (>= 50 px) and gone when settled; the legs' heading does not change |
| AIM-7 | Vulture (#37) | twist limit 2 pi (`full_turn`); 30000 counts left twist the torso more than 90 degrees |
| AIM-5 | Gyro Aim (#17) | off: D turns the legs and the aim with them; on (mission B): A turns the legs, the world aim holds (< 0.02 rad or a quarter of the legs' turn) |
| AIM-6 | no shake (#33) | Gyro on, A held: the drawn world aim (`gyro:` trace) moves <= 0.02 rad (the #33 shake was 0.032) |
| AIM-4 | torso sound (#32) | settled yaw command exactly 0 (`cmd0=1`), Gyro off and on |
| CTRL-1 | controllers (#40, #42) | a controller listed and opened (`dinput:` lines), Controls page frame; gamepad layout if it is a gamepad; **skipped** when none is plugged in. Stick input itself cannot be scripted |
| OPT-1 | Options > Game | Throttle Decay and Gyro Aim on; `controls: throttle decay on`, `aim: gyro aim on`, saved in `mw4hook.ini` |
| COMP-1 | full company (#21) | fresh mission: the start heading is recorded, Numpad 0 (pressed again until the throttle shows full) for 40 s, X (a drive that was not full or straight is noted in the detail, not failed); the 7 lancemates must win (result screen `shell: 88` with result > 0) within 15 min; a timeout or a lost mission is retried (3 attempts); the full-company hook must log `lancemates 3 -> 7` |
| THR-2 | decay on (#13) | after the drive: S and W tapped -> each eases back to 0; Numpad 5 = 0.5 holds; X holds 0 |
| DISP-4 | 32-bit | every `gos_SetScreenMode` is 32-bit; no 16-bit depth buffer in the ddraw log |
| CRASH-1 | crashes (#44) | no dialog in the game process, no Windows Error Reporting window, no unexpected exit, no `00667FA4` / `DSERR_BUFFERLOST` / `SoundPause` / exception in the logs; any of these stops the session |
| INST-2 | update | installer again: `decay` and `gyro` keep the player's values, `mw5`/`track_pointer` stay 1 |
| INST-3 | uninstall | `imagehlp.dll` back, no patch file or log left, full manifest matches |

Mission A (decay and Gyro Aim off) holds the throttle, aim, Vulture,
Alt+Enter and controller checks, then exits. Mission B is launched fresh,
so the 40-second drive starts on the mission's own start heading; the
Gyro Aim and decay checks run after the drive.

Out of scope: front-end resolution upgrades (#20, #27, #43), AntiHack and
multiplayer join (#12, manual).

## When it fails

- `summary.md` names the scenario and the reason; `run.log` has the step;
  a `stuck-*.png` frame is saved when a menu step does not reach its
  checkpoint (each menu step is retried 3 times first).
- A crash stops the game session (`CRASH-1`), the hook logs are kept under
  `logs\crash-*`.
- The menu coordinates are in the `$ui` table at the top of the script
  (800x600 menu picture). The droplists are driven with the keyboard (open,
  Up to the top, Down n, Enter), so a row is picked by number; the row
  numbers come from the logged lists (`shell: 4 row ...`) or from fixed
  lists in the script (resolution, skills, difficulty).

## Fallback: by hand

If the script cannot drive a step on some machine, the same scenarios can be
run by hand from the table above, using the same install and settings. Record
the result in the same `summary.md` format.
