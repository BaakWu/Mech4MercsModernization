MechWarrior 4: Mercenaries - Windowed Mode Patch  {{VERSION}}
==============================================================

Adds a Windowed option to MechWarrior 4: Mercenaries 3.1c (MekTek,
51.03.01.0030) and fixes problems the game has on current Windows.

What it does
------------
- Options > Video gets a "Windowed" checkbox:
    ticked   = a normal window at the resolution you pick in the same menu
    unticked = borderless fullscreen on your main monitor
  Alt+Enter switches between the two at any time.
- Menus, MechLab and movies keep their original 4:3 look, scaled to fit.
- Fixes, also present in the unpatched game on current Windows:
    - low frame rate in missions and stuttering, garbled audio
    - see-through terrain and grey "sheets" over the ground
    - the picture freezing when the window is on a second monitor
    - "fullscreen" falling back to a window
- Always 32-bit color.

No game file is changed. Everything happens in memory while the game runs.

Install
-------
1. Find your game folder: the one that contains MW4Mercs.exe.
2. Copy these three files into it:
       dinput.dll
       ddraw.dll
       mw4hook.ini
   If Windows asks whether to replace a file, check what it is first: the
   game does not ship its own dinput.dll or ddraw.dll, so an existing one
   belongs to another mod.
3. Start the game as usual. It opens in a window; set the size and the
   Windowed option in Options > Video and press OK.

Uninstall
---------
Delete dinput.dll, ddraw.dll and mw4hook.ini from the game folder, plus any
mw4hook-*.log files (and this WindowedPatch-README.txt if you copied it).
Your settings stay in options.ini as the game saved them.

Notes
-----
- Only version 3.1c (51.03.01.0030) is supported. On another version the
  patch finds nothing to change and the game runs as it normally would.
- MekTek AntiHack is not touched. Multiplayer with the patch installed has
  not been tested yet.
- If the game shows "Your current hardware or driver setup is not compatible
  with MechWarrior 4", your options.ini is usually broken (for example
  screenwidth=0 or bitdepth=65535), not your graphics card. See the project
  page for a fix.
- Each fix can be switched off in mw4hook.ini. The log files
  (mw4hook-*.log) next to MW4Mercs.exe help when reporting a problem;
  set [log] enabled=0 to turn them off.
- Antivirus programs sometimes flag files named dinput.dll or ddraw.dll in a
  game folder, because replacing these libraries is a common technique. The
  source code for these files is public on the project page.

Project page, source code and issue tracker:
https://github.com/BaakWu/Mech4MercsModernization

Unofficial community project, not affiliated with Microsoft, FASA
Interactive, Cyberlore Studios, MekTek or any rights holder.

File checksums (SHA-256)
------------------------
{{HASHES}}
