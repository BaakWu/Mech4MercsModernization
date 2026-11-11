# Patching: legacy MTX vs. the modern path

## What the legacy MTX files actually are

The `.mtx` files are ZIP archives with a convention, not an opaque proprietary
container:

```text
base/...             complete files to install
patches/0,1,2,...    binary deltas for selected files
updtdetails.udt      update metadata (registry entries, shortcuts, patch map)
```

The small `0028 -> 0029` file has 13 entries; the `0029 -> 0030` file has 9.
The full `all -> 0028` file has 1,569 entries and is roughly 1.98 GB.

`updtdetails.udt` begins with the ASCII magic `IUUDFV2` and contains
install/update instructions, including registry paths and a mapping from target
files to `patches/N` deltas. The delta files begin with the bytes:

```text
D6 C3 C4 00 00 01 ...
```

This is a custom binary delta format, not a standard `.patch`, `bsdiff`, or
`xdelta` file. The old `logic.dll` (a managed .NET assembly) contains the
apply/verify code and is the best reference if we need to implement it.

## Why the old client is not trustworthy

The original instructions use `SetupMTX` and the MekTek X client:

```text
all -> 0028 -> 0029 -> 0030
```

Observed behavior on this machine:

- `mtxservice.mektek.net` does not resolve. The client logs
  `Servers unreachable entering offline mode.`
- It can still consume a local `.mtx` file and patch offline.
- One `0029 -> 0030` attempt failed while patching `MW4MekStats.dll`.
- A later attempt reported success, but the resulting install did not match the
  known-good 3.1c build.
- The final `options.ini` contained zero/unknown display values and caused the
  Hardware Error dialog.

So the old client is a useful historical reference, but it is not a reliable
installer. It also has no modern verification, rollback, or dry-run support.

## Modern pipeline

The intended replacement is small, auditable, and manifest-driven:

1. **Acquire the canonical base**
   Use the public ModDB 3.1c distribution as the baseline. It is already the
   final 3.1c state and matches the known-good install.

2. **Verify the base**
   Hash the core files (and optionally every file) against a manifest.
   Refuse to patch an unknown or mixed-version install.

3. **Repair configuration**
   Write a known-good `options.ini` with a non-zero resolution, 32-bit color,
   and a valid video driver index. Back up the old file first.

4. **Apply optional layers**
   The DX11 renderer or other content mods are separate, opt-in layers. They
   must be installed with their own hash checks and an uninstall record.

5. **Record the result**
   Write an install record containing:
   - baseline manifest hash
   - applied layers and versions
   - pre/post hashes of changed files
   - timestamps and tool version

### Extracting MTX without MTX

The `base/` entries can be read with any ZIP64-capable library. A modern
extractor should:

- reject absolute paths, `..`, and symlink/reparse-point escapes
- extract to a staging directory first
- verify every extracted file
- only then merge into the install, with backups

The `patches/` deltas need a separate implementation. They are not required for
the canonical ModDB 3.1c base. They are required only if we want to rebuild
older versions from `all -> 0028` or audit the old chain.

### Hotfixes

`mekpak31.0030a/b/c` are `WinPatch` self-extracting patches. They are not
needed for the ModDB 3.1c base, which already contains newer 2011 DLLs than the
MTX-chain result. Treat them as:

- **ignore** for normal installation
- **research material** for reconstructing an older `0030` state
- **only run in a disposable copy**, never against the canonical base

## Decision for this repo

`mekTekpatchPacks/` stays on disk as ignored reference material. We will not
copy the binaries into Git and we will not make the old MTX client part of the
supported install path.

The first supported path is:

```text
ModDB 3.1c base
  -> verify
  -> repair options.ini
  -> optional renderer/mod layers
```

No network services, no torrent client, no dead MekTek endpoints.
