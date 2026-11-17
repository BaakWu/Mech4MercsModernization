# Runtime hook: windowed mode, borderless fullscreen, Windowed checkbox

Status: **development**. Tracks GitHub issue #1 (Windowed checkbox in the
Video Options menu). Open problems: `docs/KNOWN-ISSUES.md`.

Target: `MW4Mercs.exe` 3.1c, `51.03.01.0030`, SHA-256
`72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41`.
The executable on disk is never modified; every change below is made in
memory after the PECompact stub has unpacked the image.

## What works

- **Options > Video > Windowed** checkbox, next to Screen Resolution, using
  the same widget, bitmap (`checkbox_15x15_4state.tga`) and label font as
  MekTek's existing "Use Extended Visibility" checkbox. Nothing else on the
  screen moves or is restyled.
- **Windowed on:** a framed window whose client area is the selected
  resolution, centered on the primary monitor (a window the player moved
  stays put until its size changes).
- **Windowed off: borderless fullscreen.** A borderless window covering the
  whole primary monitor at its native resolution. The display mode is never
  changed. (Exclusive fullscreen is not used: on current Windows/drivers its
  800x600/640x480 mode switches fail and GameOS falls back to a window, which
  is what made "fullscreen" revert to windowed.)
- Switching takes effect immediately, from the checkbox or **Alt+Enter**
  (GameOS' own Alt+Enter toggle is replaced). Only the player changes the
  setting.
- The setting is stored as `windowed=true|false` under `[graphics options]`
  in `options.ini` (missing means `true`) and applied at the next launch.
- **Menus, MechLab and movies render at their native 800x600**, exactly as
  designed, and are scaled into the window (aspect kept, black bars where the
  window is not 4:3). Their layout, the MechLab mech view and the intro movies
  are therefore positioned as in the original game.
- **Missions render at the selected resolution.** In a framed window they fill
  the client 1:1; in borderless fullscreen DirectDraw scales them to the
  monitor (aspect kept).
- The cursor and hit areas match the scaled picture in menus and in scaled
  missions.
- Choosing a **Resolution** resizes the window immediately; the mission uses it
  from the next mission start. It is saved on **OK**, like every other option.
  The options screen has no Cancel (OK always saves); the Video tab's
  **Reset** button restores the resolution *and* the Windowed setting to their
  values from when the tab was opened.
- Sizes larger than the monitor (the 3.1c list goes up to 5040x1050): in a
  framed window the window is shrunk to fit the work area with the aspect
  kept; the game still renders at the chosen size and the picture is scaled.
- **Never 16-bit:** 16-bit requests are raised to 32-bit, and the 16-bit
  fallbacks in GameOS' (now unused) fullscreen setup are raised to 32-bit.
- DPI: the process opts out of DPI virtualization, so one game pixel is one
  physical pixel at 125%/150% display scaling.

## Launch

`dinput.dll` is imported statically by the PECompact-packed executable, so the
proxy loads before the game parses its command line. A local `version.dll` is
not loaded, and a local `ddraw.dll` loads too late for startup decisions.

The game always runs on GameOS' windowed path. If `-window` is missing, the
proxy relaunches the game once with it (an environment guard prevents loops);
the first process exits before the game initializes. The patched shortcuts
pass `-window` directly.

## How it works

### GameOS display entry point

Every display change in GameOS goes through `gos_SetScreenMode` (12 stdcall
arguments; retail `FUN_00709F20`, 3.1c `0x0070F8B0`). It only records the
request and sets a pending flag; the main loop applies it next frame through
the windowed setup (`0x0070FB90`) or the fullscreen setup (`0x0070FD90`).
Argument 8 requests fullscreen and argument 10 requests windowed. A request
identical to the current mode is ignored.

The shell always requests 800x600 (state 0xE, also used for the in-mission
options overlay); a mission requests the configured size (state 0xF).

`display.cpp` detours `gos_SetScreenMode` and:

- leaves the shell's 800x600 requests alone,
- replaces mission requests with the selected resolution,
- raises 16-bit to 32-bit,
- never requests exclusive fullscreen (and returns from it if GameOS is ever
  in it).

### Window

`window.cpp` owns the main window's style and placement. GameOS' windowed
setup asks `SetWindowPos` for a client of its render size; the hook replaces
that with the layout for the setting (framed + selected size, or borderless +
monitor rectangle). The checkbox, Alt+Enter and Resolution changes apply the
same layout directly, because a same-size `gos_SetScreenMode` request would be
ignored by GameOS. The window procedure is subclassed only to catch Alt+Enter.

### Present scaling and cursor

`ddraw.dll` proxy (`ddraw_proxy.cpp`): GameOS presents in windowed mode with a
`Blt` onto the primary surface at the client origin, sized to its render size.
When that does not match the client area:

- menus/MechLab/movies (shell mode, low frame rate): GDI `StretchBlt` with
  HALFTONE filtering into the fitted viewport, black bars filled,
- missions (full frame rate): DirectDraw's own stretch `Blt` into the viewport
  and color-fill blits for the bars.

The viewport and the picture size are exported (`Mw4FrontendViewport`).
GameOS reads the cursor with `GetCursorPos`, normalizes it by the render size,
and hit-tests in render pixels, so `cursor.cpp` maps the visible position from
the viewport into the picture's pixels (`SetCursorPos` gets the inverse).
Unscaled frames leave the cursor untouched.

A window that is not entirely on the main monitor is presented through the
window's own DC instead of the primary surface: the DirectDraw primary
surface only covers the main monitor, so a window dragged to another monitor
used to freeze (the game kept running behind a stale picture).

### Direct3D: per-frame display rebuild, depth buffer

Found 2026-11-16 by A/B with all display features disabled (the stock game
behaves the same):

- **Per-frame rebuild (frame rate, garbled audio).** GameOS clears through a
  wrapper (`0x00742EF0`, calls `IDirect3DDevice7::Clear`); its viewport setup
  (`0x00716230`) re-runs the **whole windowed DirectDraw/Direct3D setup**
  (`call 0x0070FB90` at `0x00716355`) whenever that Clear fails. GameOS
  renders some 3D elements into a 256x256 texture that has no depth buffer
  and asks to clear depth there; current Direct3D 7 returns
  `D3DERR_ZBUFFER_NOTPRESENT` (`0x88760816`), so the display was rebuilt
  every frame in missions and in the 3D campaign screens. Measured in the
  campaign mission Tharkad - Checkmate: 9-17 fps before, 110-150 fps after;
  the stalls also starved streaming audio. The ddraw proxy hooks
  `IDirect3DDevice7::Clear` (reached via `IDirectDraw7::QueryInterface(
  IID_IDirect3D7)` -> `IDirect3D7::CreateDevice`, vtable slot 10) and, when a
  Clear fails, retries without the stencil flag and then target-only; a
  depth-only Clear on a target without depth is a no-op. `[display]
  clear_fix=0` disables it.
- **Depth buffer not bound (see-through terrain, grey sheets).** GameOS
  attaches the depth buffer to the render target *after* creating the
  Direct3D device. A DX7 device binds the depth buffer attached to its render
  target only at creation or `SetRenderTarget`, so the shell and mission
  devices rendered with no depth test at all (geometry in draw order) and
  every depth Clear failed with `D3DERR_ZBUFFER_NOTPRESENT`. In the Clear
  hook, when that error occurs and the render target has a depth buffer
  attached, the proxy calls `SetRenderTarget` with the same target (binds it)
  and retries. Once per device; afterwards Clear succeeds. `[display]
  depth_fix=0` disables it.
- **Depth buffer precision.** GameOS first asks for a 32-bit depth buffer with
  the Z bits in the high 24 bits (`dwZBitMask 0xFFFFFF00`, 8 stencil bits).
  Current drivers only offer the standard layout (`0x00FFFFFF`), so the
  request fails with `E_NOTIMPL` and GameOS falls back to **16-bit** depth.
  The proxy retries the failed request as standard D24S8 (or D24X8 if no
  stencil was asked for) and reports the created format back. `[display]
  zbuffer_fix=0` disables it.
- The Direct3D 7 runtime gives each device its own vtable copy and can switch
  a device to another copy; the production hooks only rely on the `Clear`
  slot (verified to stay hooked on the mission device). The development trace
  (`[debug] d3d_trace=1`) hot-patches `SetTexture`/`SetTextureStageState`
  and re-patches the current vtable from there; it logs render states, draw
  categories, texture checksums, and supports a draw filter and a one-frame
  pixel probe through `mw4hook-d3dfilter.txt` (see `ddraw_proxy.cpp`).

### MW5-style controls (opt-in, `[controls] mw5=1`) and Throttle Decay

Issue #13. Both off by default. `mw5=1` gives the faster W/S and the MW5 key
layout; **Throttle Decay** is a separate On/Off switch on Options > Game
(below Splash Damage, included in that tab's Reset), saved as
`[controls] decay=` in `mw4hook.ini`. It is added to `OM_Game.script` in
memory the same way as the Windowed checkbox (option id 202). The drive hook
is installed whenever the hook layer is on, so the switch works at any time;
with `mw5=0` and decay off it changes nothing.

**Driving** (`controls.cpp`). The keyboard drive step `0x0054F2A0`
(thiscall, one `double` = time; called every frame from `0x00550120`) reads
the Accelerate/Decelerate "held" counters (`+0x3E4`/`+0x3E8`, set by the
command handlers `0x00559120`/`0x005591E0`) and steps the working throttle
`+0x2D8` (float, -1..1) by ±0.02 every 0.05 s; at its end the value is stored
in `+0x564` for the next frame. Per-mech minimums (`+0x64C`, `+0x650`) make
it skip the band around 0. A 5-byte detour runs the step, then:

- (`mw5=1`) repeats the game's own W/S step once (twice the rate: 0 → full
  in about 1.1 s instead of 2.5 s); only small changes, so presets are
  untouched,
- (Throttle Decay on) with W/S released, eases the throttle to 0 in both
  directions (`t·e^(-dt/0.35 s)`, snapping to 0 below the mech's minimum
  forward/reverse throttle: about 0.6 s from full forward, 0.45 s from full
  reverse). Only a throttle set with W/S decays; one set by a number-key
  preset, X, or a joystick holds until W/S is used again,
- S keeps running from forward through 0 into reverse, as in MW4.

**Key layout.** With `mw5=1`, the DEFAULT button in Options > Controls
(`ShellControlSetDefault`, 100) first restores MW4's factory layout, then the
hook assigns an MW5-style layout through the game's own
`ShellControlAssignValue` (101: row, column, key `<<8`, modifier `<<8`, all
passed as pointers; the game keeps only the virtual-key code and Shift = 2 /
Ctrl = 4 flags, and removes a key from any other command it was on). The
bindings therefore behave and save exactly like a manual remap (the game
handles chat, Shift/Ctrl). With `mw5=0` DEFAULT gives MW4's factory layout as
before. MW4's factory layout is not WASD (`=`/`-` throttle, arrows turn,
J jump jets); MekTek pilot profiles ship with their own WASD layout.

| Action | Key | | Action | Key |
|---|---|---|---|---|
| Turn left / right | A / D | | Target under reticule / next enemy / previous enemy | R / T / Shift+T |
| Throttle up / down | W / S | | Target next friendly | Q |
| **Full stop** (Set Throttle To 0%) | **X** | | Fire groups 1–6 | 1–6 |
| **Power** (Shutdown/Startup) | **P** | | Throttle 20–90% / 100% | Numpad 2–9 / Numpad 0 |
| Startup 'Mech | Shift+P | | Pitch torso up / down, twist left / right | ↑ / ↓ / ← / → |
| Jump jets | Space | | Look left / right | Shift+← / Shift+→ |
| Centre torso to legs | C | | Chain fire (Toggle Group Firing Mode) | Backspace |
| Crouch | Shift+C | | Toggle Forward/Reverse | `\` |
| Flush coolant | F | | Select next weapon | Ctrl+P |
| Override auto shutdown (heat) | O | | Screen snapshot | F12 |
| Show mission objectives | Shift+O | | Night vision (light amplification) | N |
| Battle grid (radar/map) | B | | Next nav point | Shift+N |
| Toggle zoom window | Z | | Toggle radar range | Shift+R |

The same step turns the mouse on and puts the Mouse X Axis on the torso, as
in MW5 (`SetMouse` 213 = 1, `SetMouseXAxis` 225 = 2; that list is 0 None,
1 Turning, 2 Torso). MW4's factory default leaves the mouse X axis on None.

**New pilots.** A pilot created in the Roster starts from MekTek's template
profile. Its Controls list shows a WASD layout, but a mission plays with
MW4's factory keys (W/S do nothing, `=` accelerates). That reproduces in the
stock game, and Options > OK alone does not fix it. The hook remembers a
pilot created with `ShellCreateNewPilot` (18). When that pilot is next
selected (`ShellSelectPilot` 19, called by START and the roster list), it
applies the layout and saves (`ShellSaveOptions` 51), as DEFAULT + OK would:
MW4's layout, or with `mw5=1` the MW5 layout plus mouse on / X axis = Torso.

Two traps found along the way:

- Calling the layout during creation itself hangs the game.
- `ShellControlAssignValue` mails the Options > Controls screen
  (`OM_Controls`) to redraw the row; with that screen closed, the game tries
  to load it in the middle of the callback and hangs. Outside the screen the
  hook calls the handler's core directly instead (`0x004FCA50`, thiscall on
  the controls object; pilot index, row, virtual key, modifier flag 1 / 2 /
  4 / 0x10). All of it is read from the handler's own instructions (pattern
  `C1 FA 08 83 FA 1B 0F 84` at `0x0051AFC8`); nothing is hard-coded and no
  code is changed.

Verified: a new pilot in Instant Action accelerates with W and stops with X.
The mouse setters (213, 225) do not touch the screen and are called normally.

Everything else keeps MW4's factory key (Center Legs To Torso Shift+Space,
chat Y/U, lance orders F1–F10, group toggles Shift+1–6, `[`/`]`, V view).
The Controls list names keys by scan code, so the arrow keys are shown as
"Num 8/2/4/6" (the numpad presets show the same names); both work as bound.

Verified in the campaign mission Tharkad - Checkmate: X stops, Numpad 5 = 50%,
Numpad 0 = 100%, P shuts down and starts up, W/S rates as above; with Throttle
Decay on, full forward and full reverse ease to 0 after release, a Numpad 5
preset holds, and a W tap from it decays again.

### Mouse aim point (`[aim] track_pointer=1`, part of the MW5 controls; issue #15)

It is installed together with the MW5-style controls: the installer sets
`track_pointer` to the same choice as `mw5`. There is no in-game switch.

**Stock behavior.** The mouse sets a torso turn *speed*. The per-frame mouse
step (`0x0054EDA0`) adds each frame's motion × sensitivity (`+0x17C`) to an
accumulator (`+0x554` X, `+0x558` Y, clamped to ±1). That accumulator is
the torso rate command, and it drains by `clamp(acc, ±maxchange) × 4 × dt`,
but by at least `minchange` per frame (`options.ini [mouse] minchange=0.015 /
maxchange=0.75`). So:

- the torso keeps turning after the mouse stops (the accumulator empties
  over time),
- fast motion saturates at ±1, so a flick lands short,
- the minimum drain is per frame, so at 150 fps small movements disappear
  about 5× faster than at 30 fps (issue #3).

**With `track_pointer`.** The mouse moves an **aim point**: an angle from
the legs, in radians, resolution independent. In aim mode the mouse step's
motion is taken and the stock accumulators are kept at 0. After the drive
step (which computes the torso keys and auto-centering), the hook:

- moves the aim point by motion × sensitivity × 0.116 rad per slider unit
  (`[aim] sensitivity` multiplies it), in the same directions as the stock
  mouse, including the game's Y invert;
- clamps it to the mech's own twist and pitch limits (`mech+0x76C`,
  `mech+0x7C8`; ±80° / ±30° on the test mech), so it **stops at the limit**
  and nothing piles up beyond it;
- steers the torso (`+0x2E4` yaw, `+0x2E0` pitch) with
  `clamp((target - angle) / (rate × τ), ±1)`. That is full rate while far
  away, then proportional, so there is no overshoot. τ = max(2 frames, 60 ms).
  `rate` is learned per axis from the torso's response (the measured twist
  rate was 0.78 rad/s and pitch 0.64 rad/s on the test mech), which also
  absorbs the game's zoom speed modifiers.

It works with the game's other torso controls:

- **Auto-centering** (`options.ini [special commands] autotorsocenter`; the
  default 2 means "after a delay") turns the *legs* toward the torso and, in
  the same step, counter-rotates the torso (`+0x2DC` and `+0x2E4`,
  `0x0054FD10`). The hook keeps that counter-rotation and moves the aim point
  with it, so the aim holds still in the world while the legs catch up. This
  was verified on the HUD compass: a 4° flick changed the heading 1° → 5°,
  and it stayed at 5° through 6 s of leg centering. An earlier build
  overrode the counter-rotation and the legs chased the torso in a slow spin.
- **Torso keys** (held counters `+0x464`..`+0x470`) and **Center Torso**
  (`+0x6D0`) drive the torso as usual. The aim point follows the torso while
  they are in use.
- Turning the legs (A/D) carries the aim point along, as in the stock game,
  unless Gyro Aim is on (below).

**Gyro Aim** (issue #17). An On/Off switch on Options > Game, below Throttle
Decay. It is only added when `track_pointer` is installed, uses option id 203
and is saved as `[aim] gyro=` (off by default; the installer keeps it on
Update). While the legs turn, from A/D or auto-centering, the aim point keeps
its direction in the world:

- The legs' heading is read from the mech's rotation matrix:
  `atan2(mech+0xFC, mech+0xF4)`, mirrored at `+0x114` / `+0x11C`, with
  + = left like the torso yaw. It was found by diffing the mech before and
  after a held turn.
- Each frame the legs' turn is taken off the torso's twist angle directly
  (`torso+0x19C`, clamped to the twist limit), and the aim point moves with
  it. So the torso holds its world direction like a gyro-stabilized turret,
  however fast the legs turn.
- At the twist limit the aim point stops and the torso is carried with the
  legs. With Gyro Aim on, this also replaces the auto-centering compensation
  above.
- Mouse aiming still twists at the mech's own rate.

A first version used a counter-rotation *command* instead. That is capped at
the mech's twist rate, so on a held turn (legs turning faster than the torso
can twist) the aim slid. Only light taps on A/D held.

**Fairness.** The torso's world direction is held at the legs' full turn
rate, which can exceed the mech's own twist rate. Turning and aiming add up:
the aim can sweep faster than without Gyro Aim. That is why it is a visible
gameplay option, not an install-time choice. Multiplayer behavior (the
twist angle is set on the player's own mech) is untested.

Verified (Instant Action, holding the turn keys, a mech with an 80° limit):

- A held for 1.5 s turned the legs about 80° in 1.2 s (faster than this
  mech's twist rate). The world aim stayed at −2.2757 rad, unchanged to the
  last digit, until the torso reached the limit; from there it was carried.
- D held for 4 s: the aim held until the opposite limit, then the torso was
  carried.
- Auto-centering afterwards brought the legs back while the aim held
  exactly.
- Switched Off in the menu: saved as `gyro=0`, and the aim turned with the
  legs as in the stock game.

Applies only with the mouse on and Mouse X Axis = Torso (the MW5 layout sets
both). Otherwise, or with `track_pointer=0`, the stock code runs unchanged.

**Marker** (`[aim] marker=1`, default with `track_pointer`). In aim mode the
`ddraw.dll` proxy always draws a small green circle with a dark outline at
the aim point, also when the reticle is on it. It is drawn on the
finished mission frame just before present, using `IDirectDrawSurface7::Blt`
colour fills, and scaled with the frame height. The aim offset comes from
the dinput proxy's export `Mw4AimMarker`. The angle-to-pixel scale was
measured from frames a known twist apart, because GameOS transforms vertices
itself and Direct3D's projection is unused: `x = w/2 × 2.02 × tan(yaw)`,
`y = h/2 × 2.32 × tan(pitch)` (about 53° horizontal field of view; the zoom
view is a separate inset).

Verified in Tharkad - Checkmate (fresh extraction, `track_pointer=1`):

| Test | Result |
|---|---|
| 300 counts instant / over 300 ms | both end at −0.3480 rad, no overshoot |
| −300 counts over 1 s / over 100 ms | both end at 0.0000 |
| 3 s idle | no drift |
| 3000 counts left | aim point stops at +1.3963 (the limit); 1200 back lands at 0.0043 |
| pitch, 2000 counts | stops at −0.5236 (the limit) |
| 20° flick | compass 4° → 23°; marker shown while turning |

### Selected resolution

The Resolution dropdown is filled by `ShellGetSupportedResolutions` from a
29-entry `(width, height)` table (`0x0081DCC0`). The game's own index-to-size
map used at mission start (`0x0052C9D6`) disagrees with that table for some
entries (index 2 shows 960x600 but applies 800x600; index 20 applies nothing),
so the hook keeps the selected size itself, applies it to mission requests,
keeps the game's copy (`0x0081B9EC/F0`) in sync, and writes `screenwidth` /
`screenheight` after the game's own save.

3.1c's `ShellSetOptionValue(MWO_SCREENRESOLUTION)` falls through into the
`MWO_USEEXTENDEDVISIBILITY` case and overwrites that option with the
resolution index. The hook preserves Use Extended Visibility across the call.

### Windowed checkbox

The Video tab is `mercsshellscripts\OM_Video.script` in `RESOURCE\props.mw4`
(gosScript source compiled at runtime; it is **not** in `core.mw4`).
`shellscript.cpp` wraps the `#VBD` archive member fetch (`0x00419150`). After
the game decompresses that script into its own buffer, the text is rewritten
**in place at the same length**: full-line comments are dropped and alignment
whitespace collapsed (CRLF line endings kept), which frees room for the added
lines; a leading marker comment absorbs the remaining length. Buffer pointers
and size fields are never touched, and `props.mw4` is not modified. If an
anchor is missing or the text would not fit, the script is left as shipped.

The added lines create the checkbox and label, read the state in `GUI_INIT`,
toggle it in `GUI_MAILBOX`, and keep it in step in `GUI_EXECUTE` (so
Alt+Enter updates the box). Like the script's own `dRes`, a `dWin` variable
records the setting when the tab opens, and the Reset button's handler sets it
back next to `dRes`. The rewritten script uses all but 14 bytes of the buffer.

The Resolution dropdown lists the 29 sizes returned by the game's
`ShellGetSupportedResolutions` (MekTek's list), not the retail game's five.
The list opens where it was last scrolled, each arrow click scrolls two rows,
and like every shell button an entry only takes a click after the pointer has
rested on it briefly (normal for this UI; automated tests must hover first). They talk to the hook through the game's own
`ShellGetOptionValue`/`ShellSetOptionValue` with option id 200
(`kMwoWindowed`; the game's ids are 0-34). The hook answers that id before the
game's handler sees it.

### Patch sites (all located by unique byte patterns)

| Site | 3.1c VA | Change |
|---|---|---|
| `gos_SetScreenMode` | `0x0070F8B0` | 9-byte detour |
| `Shell_CallbackHandler` | `0x00506770` | 6-byte detour |
| `#VBD` member fetch | `0x00419150` | 7-byte detour |
| fullscreen 16-bit (renderer path) | `0x0070FECC` | `0x10` -> `0x20` |
| fullscreen 16-bit (last resort) | `0x007100F0`, `0x007100FA` | `0x10` -> `0x20` |
| `IDirect3DDevice7::Clear` | ddraw.dll vtable slot 10 | retry failed Clear without stencil / depth |
| `IDirectDraw7::CreateSurface` | ddraw.dll vtable slot 6 | retry failed 32-bit depth request as D24S8/D24X8 |
| `IDirectDrawSurface7::Blt` | ddraw.dll vtable slot 5 | present scaling; window DC off the main monitor; aim marker |
| keyboard drive step | `0x0054F2A0` | 5-byte detour (MW5 throttle, Throttle Decay, aim point) |
| mouse step | `0x0054EDA0` | 5-byte detour (aim point; pass-through when off) |

The last three are COM vtable hooks inside the `ddraw.dll` proxy; nothing in
`MW4Mercs.exe` is patched for them.

The globals the hook reads (fullscreen flag `0x0124067C`, bpp `0x01240670`,
device `0x01240674`, resolution table, selected size) are taken from operands
inside those patterns, not hard-coded. None of these sites are in the
resource-loader regions hashed by the multiplayer join handshake.

## Configuration (`mw4hook.ini`)

```ini
[window]
enabled=1     ; window layout (framed / borderless), Alt+Enter
relaunch=1    ; relaunch once if -window is missing
[display]
enabled=1     ; mission size, 32-bit, never exclusive fullscreen
checkbox=1    ; add the Windowed checkbox to Options > Video
clear_fix=1   ; (default, not in the shipped ini) stop the per-frame rebuild
zbuffer_fix=1 ; (default, not in the shipped ini) 24-bit depth instead of 16
depth_fix=1   ; (default, not in the shipped ini) bind the attached depth buffer
[controls]
mw5=0         ; faster W/S + DEFAULT applies the MW5 key layout
decay=0       ; Throttle Decay (also Options > Game; written there)
[aim]
track_pointer=0 ; mouse moves an aim point; the torso turns to it and stops
sensitivity=1.0 ; aim point speed (x the game's mouse sensitivity slider)
marker=1        ; small circle at the aim point (always shown in aim mode)
gyro=0          ; Gyro Aim (Options > Game; written there)
[frontend]
stretch=1     ; scale the picture into the window
[log]
enabled=1     ; mw4hook-<pid>.log and mw4hook-ddraw-<pid>.log
```

`mw4hook-ddraw-<pid>.log` records the present rate every 5 seconds
(`present: N fps, blit avg/max, longest gap`), failed Clears and depth-buffer
creation; useful in player bug reports.

Development only (not shipped):

```ini
[debug]
automation=1  ; in-process test driver + frame capture (see below)
dump_image=1  ; write the unpacked image to mw4hook-image-<pid>.bin
```

## Build and install

```powershell
pwsh ./src/hook/build.ps1        # artifacts/hook/dinput.dll, ddraw.dll, mw4hook.ini
pwsh ./src/patcher/build.ps1     # artifacts/mw4patch.exe (embeds the above)
pwsh ./tools/Reset-Mw4TestInstall.ps1 -Force   # 7-Zip, or Windows' tar.exe
.\artifacts\mw4patch.exe install "MekTek\Mechwarrior Mercenaries - Mektek Mekpak" --yes
```

## Testing without touching the desktop

AntiHack denies external `PROCESS_VM_READ`, and driving the real cursor or
capturing the screen interferes with whoever is using the machine. With
`[debug] automation=1` the hook drives the game from inside the process:

- a virtual cursor replaces `GetCursorPos`/`SetCursorPos`,
- clicks/keys are posted as window messages (the game keeps mouse buttons in
  its key bitmap, filled by the window procedure) and injected into the
  DirectInput state; `altenter` posts Alt+Enter,
- the app-active flags are held on so input is processed while unfocused,
- `shot` saves the presented frame (after scaling) from the ddraw proxy.

```powershell
pwsh ./tools/Send-Mw4Automation.ps1 -Install $install -ShotDir out `
    -Command 'activate','smove 387 303','click','wait 3000','shot options.bmp'
```

`smove` takes coordinates in the scaled picture (800x600 in menus).
`click <ms>` holds the button that long (default 150; list arrows auto-repeat
while held, `click 40` scrolls one step). `movewin x y` moves the window.
With automation on, every shell script the game loads is also saved to
`mw4hook-scripts\` in the game folder (as shipped, before any rewrite).
`setwindowed 0|1` calls the same backend as the checkbox.
`tools/Invoke-Mw4Ui.ps1` reports the window size and style in physical pixels.

## Evidence (fresh `MekTek.rar` extraction via tar.exe, patched with `mw4patch.exe`)

- Intro movies fill the 1024x768 window, centered (were off-center/cropped).
- Main menu and MechLab: original layout, MechLab mech centered in the bay
  (was cut off), cursor on the hovered item.
- `windowed=false` at launch: borderless 2560x1440 window, no display mode
  change, menu pillarboxed 4:3, cursor on target.
- Borderless: MechLab and an Instant Action mission stay borderless (was:
  reverted to windowed). The 1024x768 mission is scaled to 1920x1440.
- Alt+Enter in the mission: framed, centered 1024x768 window, mission 1:1,
  `windowed=true` saved.
- Exclusive fullscreen on this machine (stock and hooked): the 800x600 and
  640x480 mode switches fail; GameOS re-initializes DirectDraw repeatedly and
  falls back to a window or hangs. This is why exclusive fullscreen is no
  longer used.

## Known limitations / not yet verified

See `docs/KNOWN-ISSUES.md` for the full list. In short: mission rendering
artifacts and garbled audio are pre-existing or unconfirmed; AntiHack and
multiplayer are untested; the 100-cycle toggle test is still to do.

## Reversibility

- `mw4patch.exe uninstall` removes `dinput.dll`, `ddraw.dll`, `mw4hook.ini`
  and the windowed shortcut and restores `options.ini` and the old shortcut.
- No game file on disk is changed; all patches are in memory.
