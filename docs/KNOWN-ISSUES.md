# Known issues (windowed-mode / Windowed checkbox work)

Status as of 2026-11-16, branch `windowed-mode`. Issues 1-4 were reported
from a play session on 2026-11-15; their status after the follow-up work is
below.

Scope note: the initial public release is **only a windowed-mode patch**
(see `AGENTS.md` section 3.0).

## 1. Turning Windowed off did not stick — FIXED

**Was:** with Windowed unticked, entering MechLab or launching a mission put
the game back in a window.

**Causes found (two):**
- The hook inferred the setting from GameOS window calls (to follow Alt+Enter)
  and treated GameOS' own temporary windowed drops as "the player chose
  windowed".
- Exclusive fullscreen itself is unreliable on current Windows/drivers. On
  the test machine (2560x1440, AMD) GameOS' 800x600 and 640x480 mode switches
  fail; it then re-initializes DirectDraw over and over (76 times in one
  logged run) and falls back to its windowed path, or hangs until Windows
  closes it. The stock game behaves the same way.

**Fix:** "Windowed off" is now **borderless fullscreen**: a borderless window
covering the monitor at its native resolution, with the picture scaled. The
display mode is never changed and GameOS is never asked for exclusive
fullscreen. Only the checkbox and Alt+Enter (now routed to the same setting)
change it. Verified: MechLab and a mission stay borderless; Alt+Enter toggles
in a mission.

## 2. MechLab mech off screen, intro movies off-center — FIXED

**Cause:** the frontend was rendered at the selected resolution while the
shell is laid out for 800x600; engine-placed content (MechLab 3D view, movie
player) used the real render size.

**Fix:** menus, MechLab and movies now render at their native 800x600; only
the window is larger and the ddraw proxy scales the picture into it. The
`getresx()`/`getresy()` patch is no longer needed and was removed. Verified:
intro movie centered and filling the window; MechLab complete with the mech
centered.

## 3. Rendering artifacts in missions — PRE-EXISTING (not the patch)

**Observed:** flat grey/white translucent sheets over terrain near the
horizon, see-through ground in places.

**A/B result:** the same mission with every display feature of the hook
disabled (`[window]`, `[display]`, `[frontend]` off) shows the same artifacts,
also in an unscaled framed window. The patch does not cause them. Likely the
DX7 renderer on current drivers (fog/terrain LOD are candidates); outside the
windowed-mode scope. To confirm on another machine: uninstall the patch and
run `MW4Mercs.exe -window` into the same mission.

## 4. Audio garbled when the engine starts (MechLab, missions) — OPEN

Not reproducible by the automated tests (they cannot listen). Sound starts
without the old DirectSound error on the new test machine. Fixes 1 and 2
remove the mode-switch churn and the oversized frontend, both suspects.

**Next step (needs a listener):** retest with this build; if still garbled,
uninstall the patch and run `MW4Mercs.exe -window` into MechLab/a mission to
see whether the stock game does the same.

## 5. Crash at `0x006FABAA` during a manual session — OPEN

Windows Error Reporting logged an access violation at `MW4Mercs.exe+0x2FABAA`
(`0x006FABAA`, the game's heap allocator reading through a null heap pointer)
in a session started outside the test harness on 2026-11-15 21:55, shortly
after GameOS fell back from a failed fullscreen switch. The same location
crashes when `OM_Video.script` is malformed, but that script was not loaded
in this session. Unknown trigger; watch for it with the new build.

## 6. Smaller items

- Rapid double-clicks on the Windowed checkbox are unverified.
- The issue's 100 toggle cycles have not been run (borderless toggles no
  longer change the display mode, so this is now cheap to run).
- Resolution dropdown: picking an entry after scrolling the list selected
  index 0 in automated tests; this also happens with all display hooks
  disabled (pre-existing, or an artifact of instant cursor jumps).
- AntiHack and multiplayer join are untested with the detours.
- Mission frames in borderless fullscreen use DirectDraw's stretch filter
  (driver-dependent); menus use GDI HALFTONE.

## Test aids

- `[debug] automation=1` + `tools/Send-Mw4Automation.ps1` drive the game and
  capture frames from inside the process (`docs/HOOK.md`).
- `mw4hook-<pid>.log` records every `gos_SetScreenMode` request and every
  window layout change.
