# Baseline: MW4 Mercs 3.1c

## Identity

| Field | Value |
|---|---|
| Distribution | ModDB — MW4 Mercs MekTek |
| Game version | `51.03.01.0030` |
| Local path | `MekTek/Mechwarrior Mercenaries - Mektek Mekpak` |
| File count | 2,233 |
| Total size | 2,300,465,618 bytes (2.14 GiB) |
| Status | Known-good; launches on Windows 11 / AMD Radeon AI PRO R9700 with a valid `options.ini` |

This folder matches the independently working install on `D:\mektek.net`.
It is the baseline for all future manifests and patching work.

## Core hashes

These are SHA-256 hashes of the files that identify the build and its
subsystems. The same list is in `manifests/mw4-mercs-3.1c-core.sha256`.

| File | SHA-256 |
|---|---|
| `MW4Mercs.exe` | `72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41` |
| `MW4Ed2.exe` | `f9d177f0d1cb302ad89a4c7094b7647a7317a03387ec6a2eb36d550e3943ee29` |
| `autoconfig_MERCS.exe` | `ddb514a41b28a671907ddeedc95cd60f1c179a6833ac5ed1dc9f757f7e10f528` |
| `MapCreator.exe` | `4f1a37548b352fd36e99a71aded94db13253f9ec84c6d350237b525f5936ea7d` |
| `NFMEditor.exe` | `e5557e8f9df956945776ce96661f0cd7a163d599c0ddb236feb73be7caa5f4dc` |
| `MW4Stats.dll` | `61de52cb1606422181e03f87e1eeb944b705d69da1ab1e564a566d588ab814d5` |
| `MW4MekStats.dll` | `4099a3e572d0a259f9354dcfd73430a80ae3dfb3fcc30e73dbf1ca45737ddadc` |
| `blade.dll` | `d774c9fd2473e069c5c183333e07518392f2782b35582e2672a9479164652c6a` |
| `GunDll.dll` | `1e441bc27cf7779d893dc59d2cf52b39c28dee17172f0ae08edd183e481b2410` |
| `GunTicket.dll` | `569b84f4673e96c7746524aa91c5982420a27ad1dd8c3e28ccd304c875df9c50` |
| `Language.dll` | `8cacab8c2eb11ba1829ca2e57920bd8e191be8c8178dbb7e00352e65ef80c1d6` |
| `MissionLang_MERCS.dll` | `daa85bb4350cc884621725cc75df5f5d7f59f848fd2ead2902e5c98aea0db728` |
| `ScriptStrings_MERCS.dll` | `8bb9301fd70e52608d71434d7a945c50f3ea04d7a38ada3813437fed1819d16c` |
| `ZlogLib.dll` | `03af8be5d047ba02076253009523461ab25922376e2af6a3db1d523df259b295` |
| `artpclnt.dll` | `30ea1455f4ade24f4b6426781632b29c23c8006bc478da3a0d3c1fd6381925ee` |
| `Cdac14ba.dll` | `9d8379cd2cf899e8873d40932c66bc683676d9bdad6f0cdec7873859c95b13c1` |
| `dsetup.dll` | `4fed0b98a0fb6c40b0549fdf98c0374c223fdfac142081cae7939e5c9d8f8e06` |
| `EBUEula.dll` | `f06ec44024cc915f3d312a73da04d5023bd6eed325b73c0f4f1344bb6915b9b6` |
| `libapr-1.dll` | `4903392b4087a947e7de41b108b0bcf828ad6f27584c875a443d4182b84873c4` |
| `libapriconv-1.dll` | `9f2a6448a2ac82c6b7288ab6e587abd453fd00a229e69000de497a08e0ad9c39` |
| `libaprutil-1.dll` | `abe0ddb90b60aca25bb4d74cb58130f0a1d14f4072175143544cee1588bd5f7b` |
| `lmx-MD-vc6.dll` | `1b801cfee7abb7141b3bf1a7a817c856670cd2b7179ce903fa9a2f0230f21312` |
| `mfc42.dll` | `aa8c615c8bbcd8ad4a9b624addb512e0702223eb5f266ce8f6a9a005f718a483` |
| `msvcirt.dll` | `647f7534aaaedffa93534e4cb9b24bfcf91524828ff0364d88973be58139e255` |
| `msvcp80.dll` | `01455b24a2a82746ddbcb840faced2c46c870211eb9a2694b1d01047991dab15` |
| `msvcr80.dll` | `ed92413e6d719b61208c4e0e598d64d989d220d0902f3e2a4a54972fd2595057` |
| `msvcrt.dll` | `2d5670881a6d13a100e37da297f6eeb876f68b63073a28eb5d650625c60f9bf7` |
| `mtshellmq.dll` | `44107bf9e37184280c34d72c34dbc9f0af36408937841b632055c8ace3a19d6e` |
| `mtshl32.dll` | `1c2a452722751093a33123005ad082df87bbc64e35b5422c10e63a82b85e73aa` |
| `RESOURCE/core.mw4` | `cf6ab4813c23b9883fc4db5dd6f32119ed8f40022ba9a1103ef885e62f267758` |
| `RESOURCE/props.mw4` | `de67d25ab75aebab111265f0d9fb8abd149825d3898456de7e081b5af179a2aa` |
| `RESOURCE/textures.mw4` | `da6673c2fee854d544093f728ce9758fba4f8dbc08dc64b112a9e2a272183726` |

## Known-good configuration

The working `options.ini` in the baseline has:

```ini
[graphics options]
videodriverindex=0
antialias=true
screenwidth=1920
screenheight=1080
bitdepth=32
useextendedvisibility=1
```

The broken MTX-chain install had:

```ini
videodriverindex=-1
screenwidth=0
screenheight=0
bitdepth=65535
```

Those zero/unknown values are what trigger the **Hardware Error** dialog.
`tools/Repair-Mw4Config.ps1` writes a known-good configuration and backs up the
old file first.

## Important differences from the old MTX chain

The MTX-chain install in `C:\Program Files (x86)\mektek.net\...` reports the
same `51.03.01.0030` version string but is not the same build:

| File | ModDB 3.1c baseline | MTX-chain install |
|---|---|---|
| `MW4Mercs.exe` | `72bbb1ba...` | `48bda013...` |
| `MW4MekStats.dll` | 676,352 bytes | 394,240 bytes |
| `MissionLang_MERCS.dll` | `daa85bb4...` | `476b7fe6...` |
| `mtshellmq.dll` | 372,736 bytes | 371,712 bytes |

Do not use the MTX-chain install as the reference for manifests, patches, or
reverse engineering. It is useful only as an example of a failed/incomplete
patch result.

## Stale metadata in the baseline

- `md5check.txt` says `MP3.1 Release / 51.03.01.0029-final`. It is stale and
  does not describe the 3.1c file set.
- `playerLog.txt` contains a 2016 user profile (`Fooby`). It is user state and
  should be excluded from distribution manifests.
- `options.ini` is user state and should be excluded from distribution
  manifests.
- `RESOURCE/Pilots`, `RESOURCE/UserMissions`, `Stats`, and `screenshots` are
  mutable/user-owned and should be excluded when generating a clean manifest.
