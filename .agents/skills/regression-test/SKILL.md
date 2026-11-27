---
name: regression-test
description: Run the MW4 Mercenaries release regression test (tools/Test-Mw4Regression.ps1, issue #45) end to end, read the summary, and triage any FAIL as a test problem (fix the test without changing what it checks) or a product bug (report it). Use when asked to run the regression / end-to-end / e2e / release test, check a release candidate, or fix a failing regression scenario.
---

# Regression test: run it, read it, fix the test only when the test is wrong

This skill is written as a checklist. Follow the steps in order. Do not skip
a step because it "looks fine". When a step says STOP, stop and tell the
user what you found.

Files you will use (all paths from the repo root `C:\repos\Mech4MercsModernization`):

| File | What it is |
|---|---|
| `tools/Test-Mw4Regression.ps1` | the test. One script, ~1600 lines |
| `docs/REGRESSION.md` | the human description of every scenario |
| `.agents/skills/regression-test/scenarios.md` | **the intent of every scenario**, what proves it, what may and may not be changed. Read the card of a scenario before touching it |
| `artifacts\regression\<run>\` | the output of one run (see step 4) |

---

## Step 1. Preconditions (check all, in this order)

1. **Is the game running?** Run `Get-Process MW4Mercs -ErrorAction SilentlyContinue`.
   - Nothing listed: go on.
   - Something listed: the owner may be playing on this install. Ask the
     user before you close it. Only pass `-Force` (which kills the game)
     after they say yes. Never send automation commands to a game you did
     not start.
2. **Which version is under test?** Read `kToolVersion` in `src/patcher/main.cpp`
   (e.g. `L"1.9.1"`).
3. **Does the installer exist?** `dist\MW4Mercs-ModernizationPatch-Setup-<version>.exe`.
   - If it is missing, or source in `src/` changed since it was built
     (compare `git log -1 --format=%ci -- src` with the file's LastWriteTime),
     build it: `pwsh -NoProfile -File tools/New-Mw4Release.ps1`. It must
     finish without error.
4. **`MekTek.rar` exists in the repo root.** The test re-extracts the game
   from it. Never modify, rename or delete `MekTek.rar`.

## Step 2. Start the run

Run from the repo root, **in the background** (a run takes 20-45 minutes):

```powershell
pwsh -NoProfile -File tools/Test-Mw4Regression.ps1
```

Use the PowerShell tool with `run_in_background: true`. You are notified
when it exits. Do not start the game, do not run a second copy, and do not
send automation commands while it runs: the test drives whatever
`MW4Mercs.exe` is running.

What the run does (so you know what you are watching):
1. backs up `RESOURCE\Pilots`, re-extracts the game from `MekTek.rar`,
   installs the release candidate with the GUI installer;
2. launches the game and plays every feature through in-process automation
   (it never moves the real mouse or takes focus);
3. updates, uninstalls, checks the full manifest, restores the pilot saves.

## Step 3. Watch it (optional, read-only)

The newest folder in `artifacts\regression\` is the running one:

```powershell
$run = Get-ChildItem artifacts\regression -Directory | Sort-Object LastWriteTime | Select-Object -Last 1
Get-Content (Join-Path $run.FullName 'run.log') -Tail 30
```

Each `run.log` line is `[HH:mm:ss] text`. Normal lines:
- `<ID>  PASS  <detail>` / `FAIL` / `SKIP`: a scenario result.
- `  <step> : no /<pattern>/ (try 2 of 5)`: a menu click was retried. A few
  of these are normal. The same step failing all tries ends in a FAIL.
- `Game session stopped: ...` or `ABORTED: ...`: the run stopped early; the
  reason follows.

Do not act on a run while it is still going. Wait for the exit.

## Step 4. Read the result

Open `artifacts\regression\<run>\summary.md`. The header says
`Result: **N passed, N failed, N skipped**`.

- **0 failed** (SKIP only for `CTRL-1` with no controller plugged in): the
  run passed. Go to step 7.
- **Any FAIL**: go to step 5 for each failed row, top to bottom. The first
  failure often causes later ones (e.g. `IA-1` failing means every mission
  scenario is `not run`), so fix the earliest one first.

Evidence in the run folder:

| File | Use it for |
|---|---|
| `summary.md` | which scenario failed and the one-line reason (Detail) |
| `run.log` | the step order and every retry, with times |
| `logs\session-mw4hook-<pid>.log` | the game's hook log: every check is a regex over these lines |
| `logs\session-mw4hook-ddraw-<pid>.log` | display/Direct3D log (DISP-4) |
| `logs\crash-*` | hook logs saved at a crash (CRASH-1) |
| `shots\NNN-<name>.png` | frames captured in the game. `stuck-<step>.png` = what the screen showed when a menu step gave up. **Look at it with the Read tool** |
| `installer-*.log` | the installer's own log for install / update / uninstall |
| `manifest-after-uninstall.txt` | which game files differ after uninstall (INST-3) |

## Step 5. Triage one FAIL

Do these sub-steps for one failed scenario at a time.

**5a. Read the intent.** Open `scenarios.md`, find the card for the ID
(e.g. `## AIM-2`). The card says what the player would see if the feature
is broken, what log lines or pixels prove it, and which parts of the check
are the intent (never change) and which are mechanics (may change).

**5b. Find the code.** In `tools/Test-Mw4Regression.ps1` search for
`Test-Scenario '<ID>'` (Grep). Read that block and the function it calls.
Line numbers change; search by text, not by line number.

**5c. Collect evidence.** For the failing assertion, find what it looked
for in the hook log:
```powershell
Select-String -Path "$($run.FullName)\logs\session-mw4hook-*.log" -Pattern '<the regex from the test>' | Select-Object -First 20
```
Also look at the `shots\` frames named in the Detail or `stuck-*.png`.

**5d. Decide: TEST problem or PRODUCT problem.** Use this table. Pick the
first row that matches.

| What you see | Verdict |
|---|---|
| The game crashed, a dialog appeared, `CRASH-1` failed | PRODUCT. STOP and report |
| The log line exists and shows the right behaviour, but the regex does not match it (the log text changed, extra field, different spacing) | TEST: update the regex |
| `stuck-*.png` shows the right screen, the click landed but the checkpoint line came later than the timeout | TEST: longer wait / timeout, or more tries |
| `stuck-*.png` shows the screen did not change: the click hit nothing (button moved, wrong coordinates) | TEST: fix the coordinates in the `$ui` table (see 5e) |
| A droplist picked the wrong row (log shows the wrong name), and the row list in the log (`shell: 4 row ...`) shows the row number moved | TEST: fix the row number / name lookup |
| The measured value is just outside the threshold, and the card says the threshold has slack for frame-rate noise | Not sure. Re-run first (step 6). If it fails again, report to the user with the numbers; do **not** loosen it yourself |
| The log line is there and shows the **wrong** behaviour (16-bit mode, torso did not reach the aim point, throttle did not decay, imagehlp.dll not restored, manifest mismatch, lancemates not 7) | PRODUCT. STOP and report |
| The log line is missing entirely and the screen looks right | Not sure. The hook may have stopped logging it (check `src/hook/` with Grep for the log text). If the hook code no longer writes it, report; do not invent a different proof |
| You cannot tell | Not sure: report, do not edit |

**PRODUCT** or **not sure**: do not edit the test. Tell the user: the
scenario ID, the expected behaviour (from the card), the evidence (log
lines, frame file names), and your best guess at the cause.

**5e. Fixing a TEST problem: the rules.**

Allowed (mechanics):
- Menu coordinates in the `$ui` table near the top of the script
  (`# Menu coordinates`). Coordinates are in the **800x600 menu picture**,
  not window pixels. Measure them on a `shots\*.png` menu frame: the frame
  is the window client area with the 800x600 picture scaled into it,
  aspect kept, centred. Convert: `x800 = (xPng - offsetX) / scale`, where
  `scale = min(W/800, H/600)` and `offsetX = (W - 800*scale)/2` (same for y).
- `wait <ms>` values, `-TimeoutSeconds`, `-Tries`, `Start-Sleep` lengths.
- Droplist row numbers and the fixed lists (`$resolutionRows`,
  skill/difficulty rows) when the game's list changed.
- A regex that must follow a renamed or reformatted log line, **as long as
  it still matches the same fact** (same numbers, same meaning).
- Order of UI steps or an extra neutral click (e.g. the "first click is
  ignored" priming click), retries around a lost click.

Forbidden (intent). Never do any of these to make a test pass:
- Loosen a threshold or tolerance (`-lt 0.01`, `-ge 200`, `-gt 0.75`,
  `-eq 32`, `-eq 0`, `-ge 0.99`, ...) or change a required value
  (`1024x768`, `'Vulture'`, 7 lancemates, `decay=0`, ...). The cards list
  the thresholds that are the intent.
- Delete or comment out an `Assert`, or wrap it in try/catch.
- Return `'SKIP: ...'` from a scenario other than `CTRL-1`'s
  "no controller" case.
- Mark a scenario PASS by hand, edit `summary.md` / `results.json`, or
  change the `$scenarios` descriptions to match what happened.
- Change the hook or the installer (`src/`) to satisfy the test. That is a
  product change: report it to the user instead.
- Touch `MekTek.rar`, the decompilation clone, or commit game files.

Write a short comment above each change saying why, in the style already
used in the script (e.g. `# Retried: run 22 lost the LAUNCH click.`).
Use the run folder name as the run number.

If the change alters what a scenario does (not only timing/coordinates),
update its row in `docs/REGRESSION.md` and its card in `scenarios.md` too.

## Step 6. Verify a test fix

Two options. Use the fast one while you iterate, the full one to finish.

**Fast (game session only, ~15-25 min).** Uses the current folder instead
of a fresh extraction. Needs the patch installed and the pilot saves
backed up first:

```powershell
$install = 'MekTek\Mechwarrior Mercenaries - Mektek Mekpak'
$bak = "$env:TEMP\mw4-pilots-backup"
Copy-Item "$install\RESOURCE\Pilots" $bak -Recurse -Force
& dist\mw4patch.exe install $install --yes --mw5 --full-company
pwsh -NoProfile -File tools/Test-Mw4Regression.ps1 -GameOnly            # add -Skip Pilot,Lab to skip the campaign / MechLab parts
& dist\mw4patch.exe uninstall $install --yes
Remove-Item "$install\RESOURCE\Pilots" -Recurse -Force; Copy-Item $bak "$install\RESOURCE\Pilots" -Recurse
```

`-GameOnly` needs `[controls] decay=0` and `[aim] gyro=0` in `mw4hook.ini`
(a fresh install has them); if it aborts with "mission A needs Throttle
Decay and Gyro Aim off", install again. INST-* rows read SKIP with
`-GameOnly`; that is expected.

**Full.** Repeat step 2 with no switches. A fix is done only when a full
run reports **0 failed**. If a fixed scenario passes but a new one fails,
go back to step 5 for that one.

Flaky check: if a scenario failed once and passes on the re-run with no
change, say so to the user (scenario, both run folders). Do not hide it.

## Step 7. Finish

1. Check nothing is left running: `Get-Process MW4Mercs, WerFault -ErrorAction SilentlyContinue`.
2. Check the pilot saves were restored (`run.log` ends with
   `Pilot saves restored` on a full run).
3. Report to the user:
   - version and installer path (from `summary.md`),
   - the result line (`N passed, N failed, N skipped`) and the run folder,
   - for every FAIL: verdict (TEST fixed / PRODUCT / not sure) and evidence,
   - every test edit: scenario, what changed, why, and that its intent is
     unchanged (quote the card's intent line),
   - that the run started from a fresh `MekTek.rar` extraction (or say
     `-GameOnly`).
4. Do **not** commit unless the user asks. Do **not** tag, publish or upload
   a release: that needs the owner's explicit go-ahead (AGENTS.md rule 12).
