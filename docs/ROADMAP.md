# Roadmap

## Milestone 0 — Establish the truth (done)

- Identify the canonical public base.
- Prove the Hardware Error is an `options.ini` problem, not a GPU problem.
- Compare the ModDB 3.1c base against the MTX-chain install.
- Decide to ignore the legacy patch packs for the normal install path.

## Milestone 1 — Repository and baseline (current)

- `.gitignore` for game data and patch media.
- Baseline documentation and core hash manifest.
- Known-good `options.ini` template.
- Inspection, verification, manifest, and config-repair tools.

## Milestone 2 — `mw4patch` CLI

A proper cross-platform CLI, likely .NET:

```text
mw4patch inspect <install>
mw4patch verify <install> --manifest <file>
mw4patch fix-config <install> --width 1920 --height 1080
mw4patch extract-mtx <file> --dest <dir> --dry-run
mw4patch manifest <install> --output <file>
mw4patch diff <install-a> <install-b>
```

Requirements:

- no network access
- path-traversal-safe extraction
- backup and rollback
- machine-readable JSON output
- deterministic manifests

## Milestone 3 — Offline MTX support

- Decode `IUUDFV2` update metadata.
- Implement or wrap the `D6 C3 C4 00` delta format.
- Verify pre/post hashes for every delta.
- Reproduce the old chain in a sandbox, without the MTX client.

This is research work. It is not required for the canonical 3.1c base.

## Milestone 4 — Hotfix normalization

- Determine exactly what `0030a/b/c` change.
- Decide whether they are fully superseded by the ModDB 3.1c base.
- Produce a single known-good 3.1c recipe with hashes.

## Milestone 5 — Optional modern layers

- DX11 renderer integration with hash checks and uninstall records.
- dgVoodoo2 as a fallback, clearly marked as a wrapper.
- Widescreen/HUD/mouse fixes where they are not already covered.
- Content mods only as opt-in layers.

## Milestone 6 — One-click installer

- Build from a verified local base.
- Apply config repair and optional layers.
- Write an install record.
- Preserve user saves and settings on upgrade/uninstall.

## Non-goals

- Reimplementing the engine.
- Replacing Rebirth.
- Shipping game files.
- Distributing modified executables without an auditable patch record.
- Reviving the dead MekTek online services.
