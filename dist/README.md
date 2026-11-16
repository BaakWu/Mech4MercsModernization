# dist (local build output)

Build output lands here. Nothing in this folder except this README is
committed to Git.

## Release installer

```text
dist\MW4Mercs-WindowedPatch-Setup-<version>.exe
```

The public release, a GUI installer with the hook embedded. Build it with:

```powershell
pwsh ./tools/New-Mw4Release.ps1           # rebuilds hook + installer
pwsh ./tools/New-Mw4Release.ps1 -NoBuild  # package what is in artifacts\
```

The version is `kToolVersion` in `src/patcher/main.cpp`. The build refuses a
shipped `mw4hook.ini` with a `[debug]` section or an opt-in feature enabled.
See `docs/PATCHER.md`.

## Command-line installer

```text
dist\mw4patch.exe
```

The same install code without the window, for testing. Rebuild it with
`pwsh ./src/patcher/build.ps1`.

```powershell
$install = "C:\repos\Mech4MercsModernization\MekTek\Mechwarrior Mercenaries - Mektek Mekpak"
.\dist\mw4patch.exe verify $install
.\dist\mw4patch.exe install $install --yes [--mw5 | --no-mw5]
.\dist\mw4patch.exe uninstall $install --yes
```
