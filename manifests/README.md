# Manifests

Manifests are plain-text SHA-256 lists:

```text
<hash> *<relative/path>
```

They contain no game data. They are used to identify a build and detect mixed
or modified installs.

## Core manifest

`mw4-mercs-3.1c-core.sha256` covers the executable, the DLLs that identify the
build, and the three primary `RESOURCE/*.mw4` archives. It is small enough to
commit and precise enough to distinguish:

- the canonical ModDB 3.1c base
- the inconsistent MTX-chain install
- unknown or modified builds

## Full manifest

Generate a full manifest from a clean install with:

```powershell
pwsh ./tools/New-Mw4Manifest.ps1 `
  -Install "MekTek/Mechwarrior Mercenaries - Mektek Mekpak" `
  -Output "artifacts/mw4-mercs-3.1c-full.sha256"
```

The generator excludes user state by default:

- `options.ini`
- `playerLog.txt`
- `screenshots/`
- `RESOURCE/Pilots/`
- `RESOURCE/UserMissions/`
- `Stats/`
- `*.log`, `*.bak`

Do not commit full manifests generated from a modified install. Regenerate from
a verified clean base.
