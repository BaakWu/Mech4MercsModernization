# Known issues (windowed-mode / Windowed checkbox work)

Status as of 2026-11-16, branch `windowed-mode`. Issues 1-4 were reported
from a play session on 2026-11-15, issues 5-7 from a recorded session on
2026-11-16 (campaign, Tharkad - Checkmate).

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
  fail; it then re-initializes DirectDraw over and over and falls back to its
  windowed path, or hangs until Windows closes it. The stock game behaves the
  same way.

**Fix:** "Windowed off" is **borderless fullscreen**: a borderless window
covering the monitor at its native resolution, with the picture scaled. The
display mode is never changed. Only the checkbox and Alt+Enter change it.

## 2. MechLab mech off screen, intro movies off-center — FIXED

Menus, MechLab and movies render at their native 800x600 and the picture is
scaled into the window. Verified: intro movie centered, MechLab mech centered.

## 3. Low frame rate and garbled audio in missions and campaign screens — FIXED

**Was:** missions ran at 9-20 fps in a 1024x768 window; audio stuttered and
scrambled in the campaign screens, mission briefings and missions.

**Cause (also in the stock game):** GameOS rebuilt its whole DirectDraw /
Direct3D setup every frame. Its viewport setup re-runs the windowed setup
whenever `IDirect3DDevice7::Clear` fails, and a depth Clear on a render
target without a depth buffer fails on current Direct3D 7
(`D3DERR_ZBUFFER_NOTPRESENT`). The rebuild stalls starved the streaming
audio. Details in `docs/HOOK.md` (Direct3D section).

**Fix:** the ddraw proxy retries a failed Clear without the parts the target
cannot clear. Measured on the campaign mission Tharkad - Checkmate: 110-150
fps (was 9-17), no rebuilds; menus no longer rebuild either. Confirmed by the
player: audio and frame rate fixed.

## 4. Rendering artifacts in missions — OPEN (pre-existing, not the patch)

**Observed:** flat grey/white translucent sheets over terrain (very visible
around the palace in Tharkad - Checkmate), see-through ground in places.

**Findings:**
- Same artifacts with every display feature of the hook disabled.
- The game was running with a 16-bit depth buffer (its 24-bit request is in a
  layout current drivers reject). The hook now gets it 24-bit depth + 8-bit
  stencil; the sheets look the same, so depth precision is not the cause.
- The per-frame rebuild (issue 3) is not the cause either; the sheets look
  the same without it.

Next candidates: how the terrain is drawn by the DX7 renderer on current
drivers (terrain LOD/vertex buffers, fog, texture-stage blending). This is a
renderer compatibility problem; any fix must keep the original look (no
renderer replacement, `AGENTS.md` 3.1).

## 5. Window dragged to another monitor froze — FIXED

**Was:** dragging the framed window to another monitor froze the picture
while the game kept running (sound and clicks still worked).

**Cause:** every frame is blitted onto DirectDraw's primary surface, which
only covers the main monitor.

**Fix:** when the window is not entirely on the main monitor the proxy
presents through the window's own DC. Verified by screen grabs on the left
monitor: the main menu (frozen before the fix) and live mission gameplay
update. Not yet tested: a monitor with a different display scaling (DISPLAY1
on the test machine runs at 125%).

## 6. Crash at `0x006FABAA` during a manual session — OPEN

Windows Error Reporting logged an access violation at `MW4Mercs.exe+0x2FABAA`
(`0x006FABAA`, the game's heap allocator reading through a null heap pointer)
in a session on 2026-11-15 21:55, shortly after GameOS fell back from a failed
fullscreen switch. Unknown trigger; not seen since.

## 7. Smaller items

- Menus are scaled with GDI HALFTONE, about 7-11 ms per frame at 1024x768
  (menus run at 70-110 fps). Fine for menus, but a cheaper path may be worth
  it.
- Rapid double-clicks on the Windowed checkbox are unverified.
- The issue's 100 toggle cycles have not been run.
- Resolution dropdown: picking an entry after scrolling the list selected
  index 0 in automated tests; also with all display hooks disabled.
- AntiHack and multiplayer join are untested with the detours.

## Test aids

- `[debug] automation=1` + `tools/Send-Mw4Automation.ps1` drive the game and
  capture frames from inside the process (`docs/HOOK.md`); `movewin x y`
  moves the window (multi-monitor tests).
- `mw4hook-ddraw-<pid>.log` logs the frame rate every 5 seconds and every
  failed Clear; `mw4hook-<pid>.log` counts GameOS' `SetWindowPos` calls (a
  count that keeps doubling means the display setup is being re-run).
