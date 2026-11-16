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
Alt+Enter updates the box). They talk to the hook through the game's own
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
[frontend]
stretch=1     ; scale the picture into the window
[log]
enabled=1     ; mw4hook-<pid>.log and mw4hook-ddraw-<pid>.log
```

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
