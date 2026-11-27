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

## 4. See-through terrain, grey sheets in missions — FIXED

**Was:** flat grey/white sheets over terrain (very visible around the palace
in Tharkad - Checkmate and over the sea in Instant Action), see-through
ground, buildings and planters drawn over buildings in front of them. Also in
the stock game.

**Cause:** the mission's Direct3D device rendered with **no depth buffer**.
GameOS attaches the depth buffer to the render target after creating the
device; a Direct3D 7 device only binds the depth buffer attached to its render
target at creation or `SetRenderTarget`, so on current Windows depth testing
silently did nothing and geometry appeared in draw order. The grey sheets were
distant, correctly fogged palace walls painted over the nearer hill. Found by
probing one pixel through a whole frame: the hill was drawn at depth 0.995,
then the wall at 0.9999 (farther) still overwrote it with `ZENABLE=1`,
`ZFUNC=LESSEQUAL`; every frame's depth `Clear` on that device failed with
`D3DERR_ZBUFFER_NOTPRESENT` although the render target had a depth buffer
attached.

**Fix:** when a Clear reports `D3DERR_ZBUFFER_NOTPRESENT` and the render
target has a depth buffer attached, the proxy sets the same render target
again, which binds it (`[display] depth_fix`). Verified on the palace intro
and the city: correct occlusion, no Clear failures afterwards. This also
removes the cause of issue 3's rebuild loop on the main device.

Also: the game's 32-bit depth request is in a layout current drivers reject
(fell back to 16-bit); the proxy now creates the standard 24-bit + 8-bit
stencil format it asked for (`[display] zbuffer_fix`).

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

## 6. Crash at `0x006FABAA` ("Nested exception ... GetProcessorDetails") — FIXED

Windows Error Reporting logged access violations at `MW4Mercs.exe+0x2FABAA`
8 times this month (2026-11-15 fullscreen-switch work, 11-17, 11-21, 11-24).
It is never the first error (issue #47): GameOS hit some error, and its error
handler then crashed while collecting machine details. Its
`GetProcessorDetails` step calls `operator new` = `gos_Malloc(size, -1)`
("current heap"), and outside a `gos_PushCurrentHeap` scope the current heap
is null; `gos_Malloc` does not check it and reads `[0+94h]` at `0x006FABAA`.
The player saw "Nested exception! - Cause: 'GetProcessorDetails'" instead of
the real error. The hook now sends a null current heap to the default heap
(`[gameos] heap_fallback`), so the real error dialog appears, and writes every
GameOS error text to `mw4hook-<pid>.log` (`gameos: error ...`).
Reproduced and verified with `[debug] sound_lost=3` (`docs/HOOK.md`, "GameOS
error reports"). The 2026-11-24 occurrence was the sound failure of issue #46,
which the hook now recovers from.

## 7. Smaller items

- Menus are scaled with GDI HALFTONE, about 7-11 ms per frame at 1024x768
  (menus run at 70-110 fps). Fine for menus, but a cheaper path may be worth
  it.
- Rapid double-clicks on the Windowed checkbox are unverified.
- The issue's 100 toggle cycles have not been run.
- Resolution dropdown "selects index 0 after scrolling": not a game bug. The
  automated test clicked without resting the pointer on the entry first, which
  this UI requires; with a hover, scrolled entries select correctly (verified
  2026-11-16, e.g. 1280x1024 after scrolling).
- AntiHack and multiplayer join are untested with the detours.

## Test aids

- `[debug] automation=1` + `tools/Send-Mw4Automation.ps1` drive the game and
  capture frames from inside the process (`docs/HOOK.md`); `movewin x y`
  moves the window (multi-monitor tests).
- `mw4hook-ddraw-<pid>.log` logs the frame rate every 5 seconds and every
  failed Clear; `mw4hook-<pid>.log` counts GameOS' `SetWindowPos` calls (a
  count that keeps doubling means the display setup is being re-run).
