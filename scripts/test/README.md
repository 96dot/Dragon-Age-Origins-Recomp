# Unattended test runs

Scripts to run the game without a person at the controller and check the result
without watching it. They run from Git Bash (or any POSIX shell) on Windows, with
Python 3 and Pillow.

```sh
# Load the latest save and turn the camera for 100 s at 4K with FSR 4, motion vectors off:
scripts/test/run_game.sh out/fsr 100 scripts/test/inputs/camera.txt \
    video.resolution_scale=3 'video.temporal_upscaler="fsr4"' xenia.dao_motion_vectors=false
python scripts/test/contact_sheet.py out/fsr        # one image of the whole run

# Frame rate and CPU per thread, from 75 s on:
EXTRA="DAO_THREAD_STATS=2" CAPTURE=20 scripts/test/run_game.sh out/perf 110 scripts/test/inputs/load_latest.txt
python scripts/test/perf_summary.py out/perf/dao.log 75
```

`run_game.sh` does the bookkeeping every run needs:

- refuses to start while the game is running (someone may be playing);
- opens the window in the background (`DAO_BACKGROUND=1`), so it doesn't take focus;
- gives the game a test copy of `userdata/config.toml` (launcher hidden, intro logos
  skipped, plus the `section.key=value` overrides) and puts the user's back afterwards,
  checking it byte for byte, even when interrupted;
- records the saves in `userdata/content` (and backups in `userdata/save_backups`) and
  afterwards removes saves the run made
  (`KEEP_SAVES=1` keeps them) and warns if an existing one changed;
- collects `capture_N.png` (every `CAPTURE` s, default 2), `dao.log` and `xenia.log`
  into the output folder, and prints any abort or device-loss lines.

If a run leaves only the logs and no captures, the game didn't start: check `EXE` and
the command first.

## Input scripts

`DAO_INPUT_SCRIPT` takes a file or inline text; statements are separated by newlines or
`;`, and `#` starts a comment:

```
<time s> <BUTTONS> [hold s]                            e.g. 27 A 0.2
every <period s> <BUTTONS> [hold s] [from <s>] [until <s>]
```

Buttons, joined with `+` (`A+LB`): `A B X Y START BACK LB RB LT RT LS RS GUIDE`, the d-pad
`UP DOWN LEFT RIGHT`, and full stick deflections `LUP LDOWN LLEFT LRIGHT`,
`RUP RDOWN RLEFT RRIGHT`. The input is merged with any real controller.

`inputs/` holds DAO's: `load_latest.txt`, `camera.txt`, `walk.txt` (walks back and
forth while turning the camera), `new_game.txt` (a new game's opening, into its first
conversation), `options_menu.txt`, `save_game.txt` (its load steps
follow the developer's saves; adjust them), `sandbox_overwrite_save.txt` (overwrites
a save: only in a sandbox), `body_mesh.txt` (108 s with `CAPTURE=5`, in a sandbox whose
latest save is unarmored in an open area: close view, camera distance and walking), and
`photo_mode.txt` (goes through photo mode's controls; with
`EXTRA="DAO_PHOTO_DIR=<folder>"` for the photo, and `capture_window.ps1` for the overlay,
which the game's captures leave out). To make a
new one, run with captures every 2 s, read the times off the contact sheet, and keep
the result here.

## Sandbox runs

To test with someone else's saves and settings (a bug only their save shows), or with
input that changes saves, use a sandbox instead of the repo's `userdata`: a folder with a
`game` junction to the real game folder and a copy of their `userdata` (with
`skip_intro_logos = true` under `[patches]`, so the input scripts' times fit).

```sh
cmd //c mklink /J "C:\Sandbox\game" "C:\path\to\DAO\game"   # then copy userdata in
DAO_SANDBOX=C:/Sandbox scripts/test/run_sandbox.sh out/walk 100 scripts/test/inputs/walk.txt
```

`run_sandbox.sh` refuses to start while the game is running and opens the window in the
background, like `run_game.sh`, but doesn't restore settings or saves: the copy is there
to be changed. It starts the game in the exe's folder, as a shortcut does: with an `EXE`
copied into the sandbox and mods such as OptiScaler beside it, the logs they write there
during the run are copied into the output folder too. Remove a
sandbox's `game` junction with `rmdir`, never `rm -r`, which deletes the real game files
through it.

## Flicker and motion

For temporal-upscaling artifacts, `xenia.dao_upscale_ui=false` shows the
upscaled scene without the game's UI, distinguishing scene artifacts from the
UI composite. `xenia.dao_temporal_jitter=false` keeps temporal upscaling on
without shifting the scene between frames. These are diagnostic settings:
restore both to `true` after testing so menus are visible and temporal
anti-aliasing receives its jittered samples.

For border flicker, test with jitter enabled at resolution scales 1 and 2,
including opening a game menu. Screen-space color and depth clears must stay
aligned to their rectangles; jitter applies to the clipped scene geometry.
The one-frame GPU trace (`DAO_GPU_FRAME_TRACE=<swap number>`) includes the depth
test, vertex XY format and clipping state to distinguish those draws.

`flicker.py <dir>...` scores flicker in consecutive frames: a frame history
(`DAO_FRAME_HISTORY=1`, F12 saves the last 180 frames) or captures taken every frame.
Compare the ratio against the same walk with the effect off (`walk.txt` gives the same
walk each time); it also lists the frames and blocks most like a one-frame flash.

## Depth of field

The recomp's own depth of field (with temporal upscaling; for the game's, see "Bloom and
the game's depth of field" below): `xenia.dao_show_depth_of_field=true` shows its blur
instead of the scene (red behind the focus, blue in front of it), and
`xenia.dao_depth_of_field_always=true` turns it on in every 3D scene, not only
conversations, for testing it anywhere (both with `video.depth_of_field` on).
`xenia.dao_depth_of_field_scale` (0.25 to 4, default 1) scales the blur's size, for
tuning. In game, F8 turns it off and on (not saved), to compare. Its GPU time is the
"depth of field" mark of `DAO_GPU_TIMING=1`.

## Bloom and the game's depth of field

With `resolution_scale` 2 and up, the game's bloom is drawn at the console's resolution
(its passes' pixel shaders are in `xenia/src/xenia/gpu/d3d12/dao_render_target_cache.cc`).
`xenia.dao_native_bloom=false` draws it at the resolution scale instead, for comparing:
there its blur leaves gaps, and bright edges get offset copies (issue #11).

The game's own depth of field (in conversations; not the recomp's, above) blurs at half
the console's resolution too, and those passes are drawn at the console's resolution the
same way, then blended over the scene with bilinear filtering (the console's point
sampling shows sharp blocks at a higher resolution). The blend's state sets only the
blur's min and mag filters, so its mip filter is whatever an earlier draw left: where
that's point or base map, Xenia's snap to texel centers for point sampling would bring
the blocks back (it can change from scene to scene), so it's off for the blur. Compare
with `video.temporal_upscaler="off"`: temporal upscaling's jitter hides the blocks.
`xenia.dao_native_depth_of_field=false` turns both off: the passes are drawn at the
scale, where the blurred background gets thin streaks (issue #13), and the blend
point-samples again. `video.console_depth_of_field=false` (the setting, or its cvar
`xenia.dao_console_depth_of_field=false`) turns the game's depth of field off: its passes
are skipped, and its blend draws the scene alone. `inputs/new_game.txt` reaches a
conversation: it starts a new game from the title screen (with a save, so New Game is one
below Continue) and plays into its first conversation, from about 300 s.

## Texture packs

Put a pack (or a folder junction to one) in `texture packs\` next to the repo's `game\`.
`xenia.dao_texture_log=true` logs to `xenia.log` each pack texture swapped in (with the
graphics memory in use), refused over the budget, or sent back to the game's, and the
hash of the first 64 KB of each game texture of 64 KB or more as it loads. For the budget, a small
`video.texture_pack_memory=64` shows refusals and swaps back on the walk:

```
EXE=<build>/DAORecomp/DAORecomp.exe CAPTURE=40 scripts/test/run_game.sh <out> 110 scripts/test/inputs/walk.txt video.texture_pack_memory=64 xenia.dao_texture_log=true
```

Count swaps with `grep -c "swapped in ("` (plain "swapped in" also matches the "not
swapped in" refusals), and compare the frame pacing with the pack off
(`video.texture_packs=false`): the `VdSwap #` lines of `dao.log`, 600 frames apart, should
stay 10.00 s apart.

`dao.log`'s "Texture packs:" line says "found" when the match was made again and "saved
results" when it came from `userdata/texture_pack_index.bin` (delete it to make it
again), how many of the game's textures the files replace and how many of those are
copies of their pictures under other names, and how many files were skipped for each
reason.
`EXTRA="DAO_TEXTURE_PACKS_TOGGLE_AT=60,80"` presses F9 at those seconds (test windows
aren't focused, so the key can't be pressed): the game's own textures from 60 s, the
pack's again from 80 s.

## Launcher and installer

- `fake_steam.py <folder>`: a fake Steam for *Add to Steam* (with `DAO_STEAM_ROOT`, below);
  `--dump` shows what the launcher wrote.
- `fake_install_engine.py`: stands in for the installer
  (`DAO_INSTALL_ENGINE=<its absolute path>`), to test the install screen in seconds.
- `installer_cancel.py`: Cancel must end a silent build step and what it started at once,
  also when the step has exited and a child of it still holds its output.
- `installer_play.py <install folder>`: the installer's Play button after an install.
- `release_list_test.cpp`: the update the launcher offers (version tags, the release and
  dev channels, unusable releases), without the network. From the repo root:

  ```
  mkdir build\release_list_test
  scripts\env.cmd clang-cl /nologo /std:c++20 /EHsc /DDAO_LAUNCHER_ONLY /DFMT_HEADER_ONLY /IDAORecomp /Itools\XenonRecomp\XenonUtils /Itools\xenia-canary\third_party\fmt\include /Itools\xenia-canary\third_party\rapidjson\include scripts\test\release_list_test.cpp DAORecomp\launcher\release_list.cpp DAORecomp\version.cpp /Fobuild\release_list_test\ /Febuild\release_list_test\test.exe && build\release_list_test\test.exe
  ```
- `capture_window.ps1 <png>`: the launcher window's contents, even behind other windows;
  `watch_windows.ps1`: logs the game window's size, position and DPI on every change.

## Content

- `skeleton_test.cpp`: PC-to-console bone numbering, preserved palette order,
  omitted finger joints, reduced distant rigs, invalid palettes and failure without
  partial changes. No game files needed; the conversion's own check of models against
  meshes (`CheckSkeletons` in `convert.cpp`) isn't covered. With two arguments (a PC
  `.mmh`, the game's) it converts those instead. From the repo root:

  ```
  mkdir build\skeleton_test
  scripts\env.cmd clang-cl /nologo /std:c++20 /EHsc /DDAO_LAUNCHER_ONLY /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DFMT_HEADER_ONLY /IDAORecomp /Itools\XenonRecomp\XenonUtils /Itools\xenia-canary\third_party\fmt\include scripts\test\skeleton_test.cpp DAORecomp\mods\convert\skeleton.cpp DAORecomp\mods\convert\gff.cpp /Fobuild\skeleton_test\ /Febuild\skeleton_test\test.exe && build\skeleton_test\test.exe
  ```

- `convert_regress.py <work dir> <PC mod>=<earlier conversion>...`: converts copies of PC
  mods with the current build and lists the files that changed since an earlier
  conversion (the work folder must be away from the inputs).
- `dlc_install.py <packages> <userdata/content>`: installs DLC packages as the launcher
  does.

## Other tools

- `untile.py`: raw Xbox 360 tiled 32bpp texture data (a save's `scrnshot.bin`, a memory
  dump) to PNG, to see what the CPU got from the GPU.
- `set_config.py`, `saves.py`: the config and save guards `run_game.sh` uses.
- `DAO_STEAM_ROOT=<folder>`: the launcher's *Add to Steam* uses that folder as Steam's
  (with `userdata/<account>/config/` inside), to test it without touching the real Steam.
