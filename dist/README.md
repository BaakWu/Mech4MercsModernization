# dist (local build output)

Build output lands here. Nothing in this folder except this README is
committed to Git.

## Release zip

```text
dist\MW4Mercs-WindowedPatch-<date>-<commit>.zip
```

The public release. Build it with:

```powershell
pwsh ./tools/New-Mw4Release.ps1                 # rebuilds the hook, then zips
pwsh ./tools/New-Mw4Release.ps1 -Version 1.0.0  # explicit version name
```

It contains `dinput.dll`, `ddraw.dll`, `mw4hook.ini` and
`WindowedPatch-README.txt` (player instructions and file checksums). Players
copy the three files next to `MW4Mercs.exe`; uninstall is deleting them. The
build refuses a file name that the 3.1c game folder already uses.

## Development installer

```text
dist\mw4patch.exe
```

A test tool, not the release (`docs/PATCHER.md`). Rebuild it with
`pwsh ./src/patcher/build.ps1`.

```powershell
$install = "C:\repos\Mech4MercsModernization\MekTek\Mechwarrior Mercenaries - Mektek Mekpak"
.\dist\mw4patch.exe verify $install
.\dist\mw4patch.exe install $install --yes
.\dist\mw4patch.exe uninstall $install --yes
```
