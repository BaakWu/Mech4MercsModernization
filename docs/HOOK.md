# Runtime hook: windowed mode, frontend resolution, Windowed checkbox

Status: **development**. Tracks GitHub issue #1 (Windowed checkbox in the
Video Options menu).

Target: `MW4Mercs.exe` 3.1c, `51.03.01.0030`, SHA-256
`72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41`.
The executable on disk is never modified; every change below is made in
memory after the PECompact stub has unpacked the image.

## What works

- **Options > Video > Windowed** checkbox, next to Screen Resolution, using
  the same widget, bitmap (`checkbox_15x15_4state.tga`) and label font as
  MekTek's existing "Use Extended Visibility" checkbox. Nothing else on the
  screen moves or is restyled.
- Unchecking switches to **fullscreen** at the selected resolution; checking
  returns to a **window** at that resolution. No restart. The box also
  follows mode changes made without it (GameOS' own Alt+Enter toggle, or a
  failed fullscreen switch that falls back to a window).
- The setting is stored as `windowed=true|false` under `[graphics options]`
  in `options.ini` (missing means `true`) and applied at the next launch.
- **The frontend runs at the selected resolution** in both modes. The shell is
  still laid out for 800x600; it is scaled into the largest centered 4:3
  viewport of the window/screen (black bars for 16:9, 16:10, 5:4, ...), so
  the original 4:3 look is kept. Cursor, hit areas and script layout
  (`getresx()`/`getresy()`) match the scaled picture.
- Choosing a **Resolution** resizes the window immediately (windowed) or
  switches the display mode (fullscreen). It is saved on **OK**, like every
  other option. Modes below 800x600 apply to missions only; the shell never
  renders smaller than its 800x600 layout (as in the original game).
- The in-mission **pause > Options** overlay (which the game shows in its
  800x600 "shell" mode) is scaled the same way; missions themselves are
  untouched.
- **Never 16-bit:** 16-bit requests are raised to 32-bit, and the two 16-bit
  fallbacks in the fullscreen setup (software-renderer path and the last-resort
  640x480x16 attempt) are raised to 32-bit. A fullscreen switch that fails
  falls back to GameOS' windowed path.
- The window is centered on the primary monitor's work area when its size or
  frame changes; a window the player moved stays put otherwise.
- DPI: the process opts out of DPI virtualization, so one game pixel is one
  physical pixel at 125%/150% display scaling.

## Launch

`dinput.dll` is imported statically by the PECompact-packed executable, so the
proxy loads before the game parses its command line. A local `version.dll` is
not loaded, and a local `ddraw.dll` loads too late for startup decisions.

At `DllMain` the proxy reads `windowed` from `options.ini`. If the `-window`
switch does not match the setting, it relaunches the game once with the switch
added or removed (an environment guard prevents loops) and the first process
exits before the game initializes. The patched shortcuts pass `-window`, which
matches the default.

## How it works

### GameOS display entry point

Every display change in GameOS goes through `gos_SetScreenMode` (12 stdcall
arguments; retail `FUN_00709F20`, 3.1c `0x0070F8B0`). It only records the
request and sets a pending flag; the main loop applies it next frame through
the windowed setup (`0x0070FB90`) or the fullscreen setup (`0x0070FD90`).
Argument 8 requests fullscreen and argument 10 requests windowed; the game's
own callers always pass 0 for both.

The shell always requests 800x600 (state 0xE, also used for the in-mission
options overlay); a mission requests the configured size (state 0xF).

`display.cpp` detours `gos_SetScreenMode` and:

- replaces the requested size with the selected resolution (shell requests
  below 800x600 stay 800x600),
- raises 16-bit to 32-bit,
- sets the fullscreen/windowed request flags from the Windowed setting.

Toggling the checkbox re-issues the game's last request through the same
detour, so the change happens on the next frame without a restart.

### Selected resolution

The Resolution dropdown is filled by `ShellGetSupportedResolutions` from a
29-entry `(width, height)` table (`0x0081DCC0`). The game's own index-to-size
map used at mission start (`0x0052C9D6`) disagrees with that table for some
entries (index 2 shows 960x600 but applies 800x600; index 20 applies nothing),
so the hook keeps the selected size itself, applies it to every request, keeps
the game's copy (`0x0081B9EC/F0`) in sync, and writes `screenwidth` /
`screenheight` after the game's own save.

3.1c's `ShellSetOptionValue(MWO_SCREENRESOLUTION)` falls through into the
`MWO_USEEXTENDEDVISIBILITY` case and overwrites that option with the
resolution index. The hook preserves Use Extended Visibility across the call.

### Script layout size

The shell scripts lay themselves out with `getresx()`/`getresy()` (about 80
scripts: centered dialogs, help text, loading screens, the main-menu column).
These builtins are evaluated inline by the expression interpreter
(`0x006D1433`) and read the render size directly. The three operands are
pointed at hook variables that report 800x600 while the game is in shell mode
and the real size in missions.

### Frontend scaling and cursor

`ddraw.dll` proxy (`ddraw_proxy.cpp`):

- windowed: the present (`Blt` onto the primary) is replaced by a GDI
  `StretchBlt` of the 800x600 shell area into the 4:3 viewport,
- fullscreen: `Flip` scales the back buffer in place and restores the next
  back buffer to the unscaled shell image, so the next frame starts from what
  the shell expects.

Shell vs mission comes from the dinput proxy (`Mw4ShellMode`); the old
black-border test is only a fallback.

GameOS reads the cursor with `GetCursorPos`, normalizes it by the render
size, and the shell hit-tests in render pixels. While shell frames are shown,
`cursor.cpp` maps the visible position from the viewport into the 800x600
shell space (`SetCursorPos` gets the inverse), so the drawn cursor and the hit
areas match the scaled picture. Missions are not affected.

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
toggle it in `GUI_MAILBOX`, and keep it in step in `GUI_EXECUTE`. They talk to
the hook through the game's own `ShellGetOptionValue`/`ShellSetOptionValue`
with option id 200 (`kMwoWindowed`; the game's ids are 0-34). The hook answers
that id before the game's handler sees it.

### Patch sites (all located by unique byte patterns)

| Site | 3.1c VA | Change |
|---|---|---|
| `gos_SetScreenMode` | `0x0070F8B0` | 9-byte detour |
| `Shell_CallbackHandler` | `0x00506770` | 6-byte detour |
| `#VBD` member fetch | `0x00419150` | 7-byte detour |
| `getresx`/`getresy` | `0x006D1448`, `0x006D145B`, `0x006D1482` | operand -> hook variable |
| fullscreen 16-bit (renderer path) | `0x0070FECC` | `0x10` -> `0x20` |
| fullscreen 16-bit (last resort) | `0x007100F0`, `0x007100FA` | `0x10` -> `0x20` |

The globals the hook reads (fullscreen flag `0x0124067C`, bpp `0x01240670`,
device `0x01240674`, resolution table, selected size) are taken from operands
inside those patterns, not hard-coded. None of these sites are in the
resource-loader regions hashed by the multiplayer join handshake.

## Configuration (`mw4hook.ini`)

```ini
[window]
enabled=1     ; size, frame and center the main window in windowed mode
relaunch=1    ; relaunch once if -window does not match the setting
[display]
enabled=1     ; frontend at selected resolution, live mode switch, 32-bit
checkbox=1    ; add the Windowed checkbox to Options > Video
[frontend]
stretch=1     ; scale the 800x600 shell (4:3 kept)
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
pwsh ./tools/Reset-Mw4TestInstall.ps1 -Force
.\artifacts\mw4patch.exe install "MekTek\Mechwarrior Mercenaries - Mektek Mekpak" --yes
```

## Testing without touching the desktop

AntiHack denies external `PROCESS_VM_READ`, and driving the real cursor or
capturing the screen interferes with whoever is using the machine. With
`[debug] automation=1` the hook drives the game from inside the process:

- a virtual cursor replaces `GetCursorPos`/`SetCursorPos`,
- clicks/keys are posted as window messages (the game keeps mouse buttons in
  its key bitmap, filled by the window procedure) and injected into the
  DirectInput state,
- the app-active flags are held on so input is processed while unfocused,
- `shot` saves the presented frame (after scaling) from the ddraw proxy.

```powershell
pwsh ./tools/Send-Mw4Automation.ps1 -Install $install -ShotDir out `
    -Command 'activate','smove 387 303','click','wait 3000','shot options.bmp'
```

`smove` takes 800x600 shell coordinates. `setwindowed 0|1` calls the same
backend as the checkbox. `tools/Invoke-Mw4Ui.ps1` reports the window size and
style in physical pixels.

Limitation: in exclusive fullscreen without real focus, posted clicks are
ignored by the game, so automated fullscreen -> windowed uses `setwindowed 1`.
A real player in fullscreen has focus and uses the checkbox normally.

## Evidence (fresh `MekTek.rar` extraction, patched with `mw4patch.exe`)

- Raw `MW4Mercs.exe` launch relaunches with `-window`; 1024x768 client,
  centered; all eight patterns found once.
- Main menu, Options, Video tab: original layout at 1024x768, 1152x864 and
  1280x720 (pillarboxed); drawn cursor on the hovered button.
- Video tab shows Windowed checked; clicking it switches to fullscreen
  (popup window, display mode 1280x720, box unchecked, `windowed=false`);
  switching back restores the framed, centered window and checks the box.
- `windowed=false` at launch: `-window` is stripped by the relaunch and the
  game starts fullscreen with the scaled frontend.
- Resolution dropdown 1024x768 -> 1152x864 resizes and re-centers the window
  immediately; OK writes `screenwidth`/`screenheight`.
- 5 windowed <-> fullscreen cycles from the Video tab: no crash, setting and
  checkbox consistent.
- Instant Action mission at 1280x720 windowed renders full-window; pause menu
  centered; pause > Options overlay scaled; Close returns to the mission.
- Uninstall removes the hook files and restores `options.ini`; the 32-file
  core manifest verifies afterwards.

## Known limitations / not yet verified

Play testing found further problems (fullscreen setting reverting on
MechLab/mission, MechLab mech and FMV mispositioned, mission rendering
artifacts, garbled audio). They are tracked in `docs/KNOWN-ISSUES.md`.

1. **AntiHack and multiplayer** have not been tested with the detours.
2. Only 5 toggle cycles were run (each one switches the monitor's mode); the
   issue's 100-cycle acceptance test is still to do.
3. Rapid double-clicks on the checkbox were not verified (the second click of
   the automated attempt arrived after the switch to fullscreen, where the
   harness cannot click).
4. Selecting a Resolution entry after scrolling the dropdown picked index 0 in
   the automated test. The same happens with all display hooks disabled, so it
   is pre-existing listbox behavior (or an artifact of instant cursor jumps),
   not caused by this hook.
5. Sound: with sound enabled the test machine shows the pre-existing
   DirectSound `CreateSoundBuffer` (DSERR_BUFFERLOST) error, so tests use
   `-nosound`. (A malformed `OM_Video.script` shows up as a "Nested
   exception ... 0x00000094" fatal error when Options opens; the shipped
   rewrite does not.)
6. The frontend scaler is a GDI stretch of the 800x600 shell; text is softer
   than a native high-resolution UI would be.
7. Renderer 3 (software) is now asked for 32-bit; it was not tested.

## Reversibility

- `mw4patch.exe uninstall` removes `dinput.dll`, `ddraw.dll`, `mw4hook.ini`
  and the windowed shortcut and restores `options.ini` and the old shortcut.
- No game file on disk is changed; all patches are in memory.
