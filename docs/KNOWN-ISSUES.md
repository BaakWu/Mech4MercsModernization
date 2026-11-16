# Known issues (windowed-mode / Windowed checkbox work)

Status as of 2026-11-15, branch `windowed-mode`. Reported from a real play
session on the patched 3.1c install (hook + `mw4patch.exe` from this branch).
Causes below are **hypotheses** unless marked otherwise; none of these has been
fixed yet.

Scope note: the initial public release is now **only a windowed-mode patch**
(see `AGENTS.md` section 3). Several items below come from the frontend
resolution / fullscreen work that goes beyond that scope; the release may ship
with those parts disabled.

## 1. Turning Windowed off does not stick (fullscreen -> windowed)

**Observed:** with Windowed unticked (fullscreen), entering MechLab or
launching an Instant Action mission puts the game back in a window. Audio is
garbled at the same time (see 4).

**Likely cause (our regression):** `NoteAppliedMode()` in
`src/hook/display.cpp` was added so the checkbox follows GameOS' own Alt+Enter
toggle. It treats any windowed `SetWindowPos` from GameOS as "the player chose
windowed" and persists `windowed=true`. GameOS, however, temporarily drops to
its windowed path while fullscreen during some transitions (the retail oracle
shows `FUN_0070DFE0` and `FUN_007472A0` calling the windowed setup and later
the fullscreen setup again). The sync flips the setting during that temporary
drop, and every later `gos_SetScreenMode` then requests windowed.

**Next step:** confirm from `mw4hook-<pid>.log` (look for "switched to
windowed on its own"), then sync only on the Alt+Enter path (the window
procedure at `0x0072CDA1` sets the go-windowed flag directly) instead of
inferring intent from window calls.

## 2. Frontend content positioned by the real render size (MechLab mech, FMV)

**Observed:**
- MechLab: the 3D mech is drawn partly off screen / cut off on the right; the
  screen's left edge also looks cropped.
- Startup FMV (MekTek/MP3.1 logo movies) is off-center and cropped in the
  1024x768 window.

**Likely cause (our change):** the frontend now renders at the selected
resolution (e.g. 1024x768) while the shell is laid out for 800x600, and the
ddraw proxy scales only the top-left 800x600 area. Script layout was fixed by
making `getresx()`/`getresy()` report 800x600, but content placed by the
engine itself (the MechLab 3D viewport/camera and the movie player) still uses
the real render size (`0x01240668/6C`). It is centered or sized for 1024x768,
then cut and scaled as if it were 800x600.

**Options:** run the frontend at 800x600 internally and scale only the
window/present (the window grows, the game keeps its native shell size; this
also fits the windowed-only release), or find and adjust the MechLab viewport
and movie placement code.

## 3. Rendering artifacts in missions (see-through / missing terrain)

**Observed:** in missions, terrain and buildings show see-through gaps and
flat white/grey smears (sky showing through the ground, bright bands across
the terrain near the horizon). Also visible in our own automated mission
capture at 1280x720.

**Cause: unknown.** Not yet established whether the hook causes it. Candidates:
- pre-existing driver behavior on modern GPUs (first-release item 8 in
  `AGENTS.md`, "texture mipmap garbage", and the DX7 path in general),
- the 32-bit enforcement (`bpp` 16 -> 32 and the fullscreen fallback patches),
- the selected-resolution substitution (mission size / aspect),
- texture/detail settings in `options.ini` written by the patcher.

**Next step:** A/B on a fresh extraction: stock game (no hook), hook with
`[display] enabled=0`, and the full hook, same mission, same `options.ini`.

## 4. Audio garbled when the engine starts (MechLab, missions)

**Observed:** audio becomes garbled as soon as the game moves into the 3D
engine (MechLab or a mission), in windowed and fullscreen mode.

**Cause: unknown.** Our automated tests ran with `-nosound` (this machine
shows the pre-existing DirectSound `CreateSoundBuffer` / DSERR_BUFFERLOST
error with sound on), so audio was never exercised. Candidates: the
mode-switch churn from issue 1, frame pacing changes from the GDI frontend
scaler, the process relaunch, or a pre-existing modern-Windows DirectSound
problem.

**Next step:** same A/B as issue 3 with sound enabled, and check whether it
also happens with the original (pre-branch) hook.

## 5. Smaller items found during testing

- Rapid double-clicks on the Windowed checkbox are unverified.
- Only 5 windowed <-> fullscreen cycles were run (issue #1 asks for 100).
- Resolution dropdown: picking an entry after scrolling the list selected
  index 0 in automated tests; this also happens with all display hooks
  disabled (pre-existing, or an artifact of instant cursor jumps).
- AntiHack and multiplayer join are untested with the detours.
- The software renderer (renderer 3) is now asked for 32-bit; untested.

## Test aids

- `[debug] automation=1` + `tools/Send-Mw4Automation.ps1` drive the game and
  capture frames from inside the process (`docs/HOOK.md`).
- `mw4hook-<pid>.log` records every `gos_SetScreenMode` request with the
  substituted size and the windowed/fullscreen flags.
