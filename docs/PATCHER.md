# mw4patch.exe (development patcher)

Status: **development**. This is the first single-executable patcher for
testing the windowed 1024x768 fix. It is not a signed public release.

## What it does

`mw4patch.exe` embeds:

- the `dinput.dll` runtime hook,
- the `ddraw.dll` frontend scaler,
- `mw4hook.ini`,
- the 32-file 3.1c core manifest,
- the known-good `options.ini` template.

On install it:

1. verifies `MW4Mercs.exe` against the supported 3.1c SHA-256,
2. verifies all 32 core files against the embedded manifest,
3. refuses to run while `MW4Mercs.exe` is running,
4. backs up `options.ini`, any conflicting `dinput.dll` / `ddraw.dll` /
   `mw4hook.ini`, and the existing game shortcut,
5. patches the `[graphics options]` display keys to 1024x768, 32-bit,
6. extracts `dinput.dll`, `ddraw.dll`, and `mw4hook.ini`,
7. creates `MechWarrior 4 Mercenaries (Windowed).lnk` and updates the existing
   `Shortcut to MW4Mercs.exe.lnk` to use `-window`,
8. writes `mw4patch-install.ini` with installed hashes and backup names.

The game executable is never modified. The proxy relaunches a raw
`MW4Mercs.exe` launch with `-window`; the patched shortcuts pass the switch
directly.

## Commands

```text
mw4patch.exe install   [install-dir] [--yes]
mw4patch.exe uninstall [install-dir] [--yes]
mw4patch.exe verify    [install-dir]
mw4patch.exe info      [install-dir]
```

If `install-dir` is omitted, the current directory is used.

Examples:

```powershell
.\dist\mw4patch.exe verify "MekTek\Mechwarrior Mercenaries - Mektek Mekpak"

.\dist\mw4patch.exe install "MekTek\Mechwarrior Mercenaries - Mektek Mekpak" --yes

.\dist\mw4patch.exe uninstall "MekTek\Mechwarrior Mercenaries - Mektek Mekpak" --yes
```

## Output and exit codes

| Code | Meaning |
|---:|---|
| 0 | success |
| 1 | usage or file I/O error |
| 2 | unsupported install / manifest mismatch |
| 3 | user abort or the game is running |

`verify` returns 2 if the executable or any core file does not match.

## Backups and install record

Default backup names:

```text
options.ini.mw4patch-backup
dinput.dll.mw4patch-backup
ddraw.dll.mw4patch-backup
mw4hook.ini.mw4patch-backup
Shortcut to MW4Mercs.exe.lnk.mw4patch-backup
```

`mw4patch-install.ini` records the tool version, UTC timestamp, target hash,
installed file hashes, shortcut names, and backup names. Uninstall uses it to
remove only files that match the recorded hooks and to restore the backups.

If a recorded hook hash does not match the file on disk, uninstall leaves that
file alone and keeps the backup rather than overwriting user changes.

## Build

```powershell
pwsh ./src/hook/build.ps1      # builds dinput.dll, ddraw.dll, and the config
pwsh ./src/patcher/build.ps1   # builds artifacts/mw4patch.exe and publishes dist/mw4patch.exe
```

The patcher build generates an RC file, compiles the resources with `rc.exe`,
and links a single console executable (`/MT`, no VC++ redistributable). Use
`dist\mw4patch.exe` for testing; it is the stable copy of the build output.

## Current limitations

- The frontend is still generated at 800x600 and upscaled by the `ddraw.dll`
  proxy; the layout is preserved but the menu art is slightly softer.
- The patcher installs the development hook, not a final release payload.
- The install record is a simple key/value file, not a signed manifest.
- The patcher does not install or remove the test-only intro skip. That helper
  is separate (`tools/Set-Mw4IntroSkip.ps1`).
- Sound can trigger a DirectSound `CreateSoundBuffer` error in this test
  environment; `-nosound` is used for display testing only.
- AntiHack and multiplayer have not been tested with the patcher installed.
- Launching the raw `MW4Mercs.exe` relaunches it with `-window`; the original
  process exits before the game initializes. The patched shortcuts pass the
  switch directly and skip the relaunch.

## Safety notes

- Always test against a fresh extraction when changing game files.
- The patcher refuses unknown builds. Do not bypass the manifest check for
  release testing.
- Keep the install record and backups until the patch is accepted.
