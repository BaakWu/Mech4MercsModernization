# Scenario cards: what each regression check means

One card per scenario ID in `tools/Test-Mw4Regression.ps1`. Find the code
with Grep: `Test-Scenario '<ID>'` (the check) and the function named under
**Code**.

Each card has:
- **Intent**: the player-visible behaviour the check protects. This never changes.
- **Broken looks like**: what a player would notice if the feature regressed.
- **Proof**: the log lines / files / pixels the check reads.
- **Intent values (do not change)**: thresholds and required values.
  Owner decision 2026-11-26: the thresholds prove a feature **works at
  all**, not how precisely; each sits well clear of both a passing run and
  the broken behaviour (see the card). Do not tighten them again without
  the owner.
- **Mechanics (may change)**: what you may adjust if the test, not the game, is wrong.
- **Known test pitfalls**: failures seen before that were the test's fault.

Log lines are from the hook log `mw4hook-<pid>.log` (copied to
`logs\session-*.log` in the run folder). They are written only with
`[debug] automation=1`, which the test adds. `shell: N -> ...` lines are the
game's own shell calls (N = command number, see `docs/HOOK.md`, "Testing
without touching the desktop").

General mechanics shared by all menu steps (`Invoke-Ui`, `Click`, `Pick`):
- `Invoke-Ui <name> <commands> <regex>` sends the commands, waits for the
  regex in the log, retries up to `-Tries` times, then saves
  `stuck-<name>.png` and throws `UI step '<name>' did not reach /<regex>/`.
  The regex is a **checkpoint** (proof that the click landed), not the
  scenario's intent, unless a card says so.
- The game ignores the first click on a freshly loaded screen. `Invoke-Ui`
  first clicks the screen title (`smove 400 12`). In missions `-NoPrime`
  turns this off (a click would fire weapons).
- Droplists are driven by keys: open, Up to the top, Down n, Enter or click.
  Row numbers come from the logged lists (`shell: 4 row <n> '<name>'`).

---

## INST-1
**Intent:** the release installer installs on a clean 3.1c folder, sets
aside the game's `Assets\Binaries\imagehlp.dll` (issue #35, NVIDIA Hardware
Error) and writes the chosen options.
**Broken looks like:** NVIDIA players get "Hardware Error"; options ticked in the installer are not applied.
**Proof:** `mw4patch-install.ini` exists; `imagehlp.dll` gone and
`imagehlp.dll.mw4patch-disabled` present; `mw4hook.ini` has
`[controls] mw5=1`, `[aim] track_pointer=1`, `[campaign] full_company=1`,
`[controls] decay=0`. Installer output in `installer-install.log`.
**Intent values:** all four ini values above; imagehlp renamed, not deleted.
**Mechanics:** `Invoke-Installer` window-message details: dialog control
IDs (108 Install, 109 Uninstall, 105 MW5, 106 full company, 107 log box,
102 status), timeouts. If the GUI layout changed, read the IDs in
`src/patcher/` (Grep the resource/ID definitions).
**Pitfalls:** installer missing (build it first); `Reset-Mw4TestInstall.ps1`
failure (see `reset.log`) is not INST-1's fault.
If INST-1 fails, nothing else runs (by design).

## DISP-1
**Intent:** the game launched from the patch shortcut opens a framed window
with a 1024x768 client area, centred on the primary monitor's work area.
**Broken looks like:** 800x600 window, window off-screen or on another monitor, fullscreen at start.
**Proof:** `main window created: client=WxH window=WxH at X,Y style ...` and
`launch: -window present`. Centre = work area centre +-50 px.
**Intent values:** client 1024x768; `-window` from the shortcut; centred within 50 px.
**Mechanics:** regex if the log line gains fields.
**Pitfalls:** the user moved the taskbar / changed monitors between runs (the work area is read at test time, so this is usually fine).

## CAMO-1
**Intent:** a new pilot who picks camouflage `1-Ops - Red` gets red 'Mechs
in the campaign, never the template's `7-Ops - Pink` (issue #19).
**Broken looks like:** every new pilot's campaign 'Mechs are pink.
**Proof:** `camo: dialog camouflage <n> '1-Ops - Red'`,
`controls: new pilot 'regtest' created`, `camo: regtest's default camouflage '1'`,
and after that line every `camo: skin read ... skin '<c>'` has `c = 1`, never `7`.
Frame `NNN-campaign-mechlab.png`.
**Intent values:** skin `1` after the switch; no `7` after the switch (the
first read *before* the switch is allowed to be `7`).
**Mechanics:** `$ui` Roster/Join coordinates (`RosterNew`, `RosterJoinKell`,
`JoinCompany`, `JoinCamo`, `JoinCamoRed`, `JoinOk`), waits, the typed name.
The camouflage list ignores Up, so its row is clicked by coordinate.
**Pitfalls:** a pilot `regtest` already exists (only with `-GameOnly`): the
name dialog refuses it. Remove that pilot from the backup copy, or run
with `-Skip Pilot`.

## LAB-3
**Intent:** in the campaign MechLab, Class = Light lists only Light (class 0)
'Mechs of the company (issue #28).
**Proof:** after the Class pick, `mechlab: filter 0, <n> rows` then
`mechlab: row '<name>' class <c>` lines; every `c` is 0 (negative = unknown, ignored).
**Intent values:** at least one row; no row of another class.
**Mechanics:** `CampaignLabClass` coordinate; Pick row (1 = Light; 0 = All); waits.

## DISP-2
**Intent:** Options > Video can choose a resolution wider than 2048
(`-Resolution`, default `2560x1080`) and a mission then renders at that
size and fills the window (issue #38).
**Broken looks like:** the mission stays at 1024x768, has black bars inside the window, or does not load.
**Proof:** `display: script sets Resolution = index <row>`; options.ini
`screenwidth/screenheight` after OK; `gos_SetScreenMode ... -> 2560x1080x32`;
frame `NNN-mission-b.png` has lit pixels in > 50% of an 8 px edge band.
**Intent values:** exact mode `WxHx32`; edge content > 0.5 (runs: 0.92-1.0); saved size = chosen size.
**Mechanics:** `$resolutionRows` (must equal the game's list, in order:
see `src/hook/display.cpp` resolution table); `OptVideoTab`, `OptResolution`; waits.
**Pitfalls:** if the hook's resolution list changes (a size added), every
row after it shifts: update `$resolutionRows`, not the expected size.
A dark mission frame (night map, or the frame grabbed during a fade) can
read low edge content: check the PNG before deciding.

## DISP-3
**Intent:** the Windowed checkbox and Alt+Enter switch between a framed
window and borderless fullscreen in both directions, in menus and in a
mission, and the choice is saved (issue #1).
**Proof:** menus: `display: windowed -> 0 (borderless`, `main window (borderless): client=`,
options.ini `windowed=false` after OK; Alt+Enter: `display: windowed -> 1 (framed`,
`windowed=true`. Mission: `Alt+Enter: Windowed -> 0` and `-> 1`; frame
`mission-borderless.png` is as wide as the primary monitor.
**Intent values:** both directions in both places; saved values `false` / `true`; borderless width = monitor width.
**Mechanics:** `OptWindowed` coordinate, waits after `altenter` (the window rebuild takes a moment).

## IA-1
**Intent:** the Instant Action screen can be set up fully by the test:
Mission Play on Eaton - Merc Alley, Regular, Day, player in a Vulture,
7 Expert lancemates in Archers. Everything in the missions depends on it.
**Proof:** `shell: 71 -> ... 'Eaton - Merc Alley'`, `shell: 73/74 -> ... 1`
(Regular), `shell: 7/70 -> ... 0` (Day), `shell: 5 -> ... 'Vulture'`, per
slot `shell: 55 ->... <slot> 3` (Expert) and `shell: 56 ->... <slot> ... 'Archer'`.
Frame `NNN-instant-action-lance.png`.
**Intent values:** the map, difficulty, time, Vulture (AIM-7 needs its
360-degree torso), 7 lancemates (COMP-1 needs them), Expert, Archer.
**Mechanics:** `$lanceSlots` coordinates, `IaCommanderMech`, `IaDifficulty`,
`IaTime`, the `-Visible` / `-AtTop` options of `Pick`, retry counts.
**Pitfalls:** this is the most timing-sensitive step. A click on an open
droplist can change an earlier slot; the code already goes round twice.
Look at the lance frame and the last `shell: 55/56` lines per slot to see
which slot is wrong. Map is not Eaton: the game's default map changed, not
something the test picks.

## THR-1
**Intent:** with Throttle Decay off (default), a W/S throttle holds its value after the key is released; X stops; Numpad 0 is 100%.
**Broken looks like:** the 'Mech slows down by itself with decay off.
**Proof:** `controls: t=<t> throttle <v> keys <WS>` lines. `keys` is two
chars: `W-` W held, `-S` S held, `--` none.
**Intent values:** W peak > 0.5 and unchanged (+-0.02) after release; S
reaches < -0.1 and holds (+-0.02); X -> |v| < 0.01; Numpad 0 -> >= 0.95.
A decaying throttle moves far more than 0.02 in the 2.5 s wait.
**Mechanics:** key hold times (`key 87 1500` = W for 1.5 s), waits.
**Pitfalls:** the "held" flag can flicker off for one frame while held;
`Get-AfterRelease` already uses the *last* held frame. Keep that.

## AIM-1
**Intent:** with the aim point (`[aim] track_pointer`, issue #15) the torso turns to where the mouse points.
**Proof:** `aim: t=... target=<yaw>,<pitch> torso=<yaw>,<pitch> ...` after 1500 counts of mouse left.
**Intent values:** target moved > 0.3 rad; final torso within 0.05 rad of the target (runs: 1.74 rad, within 0.0005).
**Mechanics:** the mouse amount/duration (`mouse -1500 0 300`) and the 6 s settle wait.

## AIM-2
**Intent:** while the torso is still turning toward the aim point, a green
line is drawn from the reticle to the marker; it disappears once settled (issue #23).
**Proof:** pixels in `aim-turning.png` and `aim-settled.png`, horizontal
band through the centre, left of the reticle, that are bright green
(`Get-AimLinePixels`; the line colour is `40 FF 40`).
**Intent values:** >= 50 green pixels while turning; settled < half of that (runs: 410 and 0).
**Mechanics:** when the `turning` shot is taken (right after the mouse
move); the band geometry if the render size changed.
**Pitfalls:** a green-heavy map would add false pixels; the map is fixed (Eaton), so this should not happen.

## AIM-3
**Intent:** moving the aim point does not turn the legs (issue #31).
**Proof:** every `aim:` line's `heading=` during the aim move equals the
mission's start heading.
**Intent values:** heading change <= 0.02 rad (a legs turn from aiming is tenths of a radian).
**Mechanics:** none really; if it fails, it is very likely PRODUCT.

## AIM-4
**Intent:** when the torso has settled the yaw command is exactly 0, so the torso-twist sound does not keep playing (issue #32), with Gyro Aim off and on.
**Proof:** the `cmd0=` field of the last 5 `aim:` lines of each settle
(`cmd0=1` = exactly 0).
**Intent values:** every sampled frame `cmd0=1` (5 per settle: 15 could
reach back into the end of the turn).
**Pitfalls:** "no cmd0 field (old hook?)" means the installed hook is older than the test: rebuild the installer.

## AIM-5
**Intent:** Gyro Aim (issue #17). Off: turning the legs (D) turns the aim
with them. On (mission B): turning the legs (A) leaves the aim pointing
at the same place in the world.
**Proof:** `aim:` `heading=` (legs) and `world=` (aim direction in the world).
**Intent values:** legs turn > 0.02 rad in both cases. Off: world change
equals legs change within max(0.01, 25%). On: world change < max(0.02, 25%
of the legs' turn); runs: 0.0000. Broken Gyro Aim turns it as far as the legs.
**Mechanics:** key hold time (3 s), the mouse nudge before the Gyro-on test.
**Pitfalls:** how far the legs turn depends on frame rate; that is why only > 0.02 is required.
AIM-5 is reported twice (mission A partial, then mission B): the second result is the final one.

## AIM-6
**Intent:** with Gyro Aim on, turning with A/D does not make the view shake (issue #33).
**Proof:** `gyro: ... drawn_world=<rad>` per drawn frame while A is held.
**Intent values:** > 5 frames; max deviation from the second frame <= 0.02 rad
(runs: 0.00001; the #33 shake slid the aim 0.032 rad).

## AIM-7
**Intent:** a 'Mech with a 360-degree torso (Vulture) can twist past 90 degrees with the aim point (issue #37).
**Proof:** `aim:` `limit=<yaw>,<pitch> full_turn`; accumulated torso yaw while 30000 counts of mouse left are applied.
**Intent values:** limit > 6.2 (2 pi) with `full_turn`; total twist > pi/2 + 0.1 rad (runs: 4.65).
**Pitfalls:** "twist limit ... is the player in a Vulture?" = IA-1 picked a different 'Mech; fix IA-1, not this.

## CTRL-1
**Intent:** a plugged-in joystick or gamepad is listed and opened by the game (issues #40, #42); gamepads get the MW5 gamepad layout.
**Proof:** `dinput: device "<name>" (controller`, `dinput: opened "<name>" -> 0x00000000`, `gamepad axis layout`; frame `mission-controls.png`.
**SKIP rule:** SKIP only when no controller line exists (nothing plugged in). This is the only allowed SKIP.
**Mechanics:** `ControlsClose` coordinate; pause-menu offsets in `$pause` (render pixels from the mission picture's centre).

## OPT-1
**Intent:** Options > Game has Throttle Decay and Gyro Aim switches; switching them on takes effect and is saved.
**Proof:** `controls: throttle decay on`, `aim: gyro aim on`; `mw4hook.ini` `[controls] decay=1`, `[aim] gyro=1`.
**Intent values:** both on in the log and both `1` in the ini.
**Mechanics:** `OptDecayOn`, `OptGyroOn` coordinates (the "off" button is 100 px to the right), `OptGameTab`.

## COMP-1
**Intent:** with `[campaign] full_company=1`, a mission that allows
lancemates gets 7 of them (issue #21), and they can win the mission on
their own after the player drives 40 s forward and stops.
**Proof:** `campaign: mission <id> lancemates <n> -> 7`; the result screen `shell: 88 -> ... <result>` with result > 0
within `-MissionMinutes` (15). Up to `-MissionAttempts` (3) fresh attempts.
**Intent values:** `-> 7`; a win (result > 0).
**Drive (mechanics, owner 2026-11-26):** Numpad 0 is pressed again (up to
5 times) until the throttle shows full: the first key of a fresh mission
can be lost (run 20261126-134847). A drive that never reached full,
dropped below 0.9 or left the start heading by > 0.05 rad is written to
the run log and the COMP-1 detail, never fatal, so the Gyro Aim and decay
checks still run.
**Mechanics:** `-DriveSeconds`, `-MissionMinutes`, `-MissionAttempts` defaults
are part of the published test (docs/REGRESSION.md): change them only with
the user's agreement. Exit-mission and results-close coordinates.
**Pitfalls:** a lost mission or timeout is retried automatically; three in
a row is a real finding (report it: the lancemates may not be fighting).
"drive: heading left the start heading" in the detail: look at AIM-3 /
AIM-5; if they pass, a collision is the likely cause.

## THR-2
**Intent:** with Throttle Decay on (issue #13), a W or S throttle eases back
to 0 after release (both directions); presets (Numpad 5 = 50%) and X (stop)
hold their value.
**Proof:** `controls:` lines after S, W, Numpad 5, X.
**Intent values:** W/S moved the throttle > 0.1 away from 0 (owner
2026-11-26: the check proves W/S moves the throttle and decay returns it to
0; the peak height depends on how many frames the game registers the hold —
run 20261126-123100 saw 0.18 from a 2-frame press); last value
within 0.02 of 0 with `keys --`; Numpad 5 within 0.1 of 0.5 and unchanged
(+-0.02); X |v| < 0.01 and held.
**Mechanics:** key hold times, the 2.5-3 s waits (decay must finish inside them; lengthening a wait is allowed).

## LAB-1
**Intent:** the Instant Action MechLab Class filter shows only 'Mechs of
the chosen class; All shows every 'Mech again (issue #28).
**Proof:** per class c = 0..4: `mechlab: filter <c>, <n> rows` followed by
exactly n `mechlab: row '<name>' class <c>` lines. All:
`mechlab: filter -1, <n> rows`.
**Intent values:** every row's class = chosen class (negative = unknown, ignored); n > 0; All >= sum of classes and > 50.
**Mechanics:** `IaLabClass` coordinate, Pick rows (1..5 = Light..Elemental, 0 = All), the 3 s load wait.

## LAB-2
**Intent:** picking a chassis from a filtered list loads exactly the same 'Mech as picking it from the full list (issue #28).
**Proof:** the variant count (`mechlab: filter -1, <n> rows` with 1-2 digits)
is the same both ways, and three panel rectangles of `lab-avatar-all.png`
and `lab-avatar-heavy.png` are pixel-identical.
**Intent values:** same variant count; at most 2% of the panel pixels differ
(runs failed on 593 and 1597 px of animation/blending).
**Mechanics:** the chassis name (`Avatar`), the rectangles (in 800x600
menu coordinates; they skip the Type/Class row, which differs on purpose,
and the translucent last stats row), the neutral clicks, waits.
**Pitfalls:** the keys reaching the Class list instead of the Type list
(`shell: 49 -> ... #206 <class>` appears): the code re-sets the class and
retries. A non-zero diff: open both PNGs and compare; an animated
element inside a rectangle is a TEST problem (shrink the rectangle and
explain why), different stats are PRODUCT.

## DISP-4
**Intent:** every display mode the game sets is 32-bit, and no 16-bit depth buffer is used (AGENTS.md item 11).
**Proof:** all `gos_SetScreenMode ... -> WxHx<bpp>` have bpp 32; no
`z-buffer ...: 16-bit Z` in the ddraw log.
**Intent values:** 32 only; no 16-bit Z. Any failure is PRODUCT.

## CRASH-1
**Intent:** the game never crashes, shows an error dialog, or exits by itself during the session (issue #44 watched).
**Proof:** no `#32770` dialog in the game process, no WerFault window, the
process still alive at the end, no `00667FA4` / `DSERR_BUFFERLOST` /
`SoundPause` / `access violation` / `exception` in the logs (lines
starting `auto:` are the automation's own and ignored).
**Intent values:** all of them. Any failure is PRODUCT. Evidence in `logs\crash-*`.
**Note:** a crash ends the game session; later game scenarios are then `not run`. Fix nothing else until the crash is understood.

## INST-2
**Intent:** running the installer again updates in place and keeps the player's own choices (Throttle Decay, Gyro Aim on from OPT-1), with MW5 controls and the aim point still on.
**Proof:** `mw4hook.ini` before/after: decay and gyro unchanged (both 1), `mw5=1`, `track_pointer=1`. `installer-update.log`.
**Intent values:** all of those. "Throttle Decay was not on before the update" = OPT-1 failed earlier; fix that first.

## INST-3
**Intent:** uninstall restores the stock game exactly: `imagehlp.dll`
back, no patch file or hook log left, and the folder matches the full 3.1c
manifest.
**Proof:** files listed in the check; `manifest-after-uninstall.txt`
(`Verified : N`, and `Missing` / `Mismatch` lines on failure).
**Intent values:** all. A mismatch is PRODUCT unless the mismatching file
is one the **test** wrote and forgot to remove (e.g. something from
`Enable-TestMode` / `Set-Mw4IntroSkip.ps1`); then fix `Disable-TestMode`.
