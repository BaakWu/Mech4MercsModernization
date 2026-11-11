# mw4patch.exe (local build)

This folder contains the locally built development patcher:

```text
dist\mw4patch.exe
```

It is **not committed to Git** (binary build output). Rebuild it with:

```powershell
pwsh ./src/patcher/build.ps1
```

## Use

From the repository root:

```powershell
$install = "C:\repos\Mech4MercsModernization\MekTek\Mechwarrior Mercenaries - Mektek Mekpak"

# Verify the install first
.\dist\mw4patch.exe verify $install

# Apply the patch
.\dist\mw4patch.exe install $install --yes

# Inspect what was installed
.\dist\mw4patch.exe info $install

# Remove it again
.\dist\mw4patch.exe uninstall $install --yes
```

The patcher installs a windowed shortcut in the game directory:

```text
MechWarrior 4 Mercenaries (Windowed).lnk
```

## Notes

- The patcher verifies `MW4Mercs.exe` and all 32 core files before changing
  anything.
- It backs up `options.ini` and any conflicting `ddraw.dll` / `mw4hook.ini`.
- The game executable is not modified.
- This is a development build. The original frontend still renders at 800x600
  inside the 1024x768 window.
- See `docs/PATCHER.md` for details and limitations.
