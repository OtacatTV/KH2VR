# KH2VR: an experimental VR mod for Kingdom Hearts II Final Mix (PC)

> **Unofficial, non-commercial fan project. Not affiliated with, endorsed or supported by Square Enix or
> Disney.** Kingdom Hearts is a trademark of its owners; official site: <https://www.kingdomhearts.com/>.
> This repository holds only the mod's own code and notes: no game files, game data or game code.
> Square Enix's published policies restrict modifying and reverse-engineering its games; see
> [RESEARCH.md §2](RESEARCH.md). Check them yourself and use this at your own risk.

**Status: early experimental developer build (November 2026).** There is no installer yet. It runs as a
"discovery probe" DLL with diagnostics on, started by a Python launcher. It has been played on exactly one
setup. Contributors are very welcome: see [Contributing](#contributing).

## What it does today

Kingdom Hearts II Final Mix from *KINGDOM HEARTS -HD 1.5+2.5 ReMIX-* (Steam) rendered in stereo 3D in a
PC VR headset, played seated with a gamepad.

- **True stereo 3D.** Each frame the game's own world-drawing commands are recorded at the Direct3D 12 level
  and replayed once per eye with a per-eye camera and the headset's projection, so the world has real depth.
  No game code is changed for this.
- **Head tracking** at the headset's refresh rate (tested at 75 Hz). A separate XR thread submits frames
  on its own GPU queue, so head movement stays smooth while the game runs its usual 60 ticks per second.
- **Full headset field of view** per eye (2560x2336 per eye on the test headset), not a window.
- **HUD on a floating panel.** The game's HUD (command menu, HP/MP, party, minimap) is drawn again into
  separate images, with transparency recovered, and shown as an adjustable panel in the world
  (distance, height, width, tilt are live settings).
- **Screen-wide effects** (battle-start impact, load-screen keyhole) on a full-view layer.
- **Flat screen for everything else.** Menus, pause, loading, cutscenes and summon intros are shown on a
  large flat screen in front of you, switched by the game's own gameplay flag (or when no 3D image is
  available).
- **Stability work:** fixes for hitches (NPC talk, ledges), white flashes, frozen HUD, and a GPU hang when
  the game changes anti-aliasing (Config menu camera setting). A built-in crash catcher logs the cause of
  any crash for debugging.
- **Test harness:** an automated launcher (build-checked install, launch, cleanup), a virtual gamepad
  driver for scripted routes (`tests/pad`), save-load flows, VR View and eye snapshots, and screen-state
  checks, so most problems can be reproduced without wearing the headset.

## Tested setup (the only one so far)

| Part | Tested |
|---|---|
| Game | Steam app 2552430, `KINGDOM HEARTS II FINAL MIX.exe` build 15194255 (SHA-256 `9002b2de…849ed`, see RESEARCH.md). **Other builds are refused.** |
| Loader | OpenKH Panacea, Build 1806 (LuaBackend not required) |
| GPU | NVIDIA RTX 4090 |
| Headset | Bigscreen Beyond 2, SteamVR (OpenXR), 75 Hz |
| OS | Windows 11 |

Single-player only; the game has no online mode. Other GPUs, headsets and runtimes are untested.

## Known issues

- Some effects are missing or wrong: sky and light effects (e.g. Twilight Town), magic and enemy drops are
  drawn flat on the HUD panel, battle-start shards land on the panel.
- The lock-on marker and Triangle prompt sit on the HUD panel instead of over their targets.
- Characters can look jagged (no anti-aliasing in the eye images yet).
- The game culls with its own narrower view, so enemies or NPCs can vanish near the edges of the headset view.
- The minimap's see-through corridors faintly show the monitor's view.
- Summon intros play on the flat screen (3D works for the characters, but their background is missing).
- After a GPU driver reset, the next launch can fail once; restart it.

## Roadmap

Roughly in order; see [STATUS.md](STATUS.md) for the detailed backlog and findings.

1. **World sprites in 3D:** lock-on marker and Triangle prompt over their targets, magic and enemy drops
   in 3D space (they are drawn in the same late stage as the HUD today).
2. **Rendering correctness:** sky and light effects, summon backgrounds, character culling widened to
   the headset view.
3. **Anti-aliasing** for the eye images.
4. **Camera:** level camera that moves straight up/down; keep the game's first-person view between rooms,
   at Sora's eye height.
5. **Menus and HUD polish:** clean loading transitions, movable HUD elements, a VR settings menu (HUD,
   world scale), menus with real depth.
6. **Comfort and smoothness:** measure, then consider depth-based reprojection or frame generation.
7. **Player release:** an installer that checks the game build, installs and removes the mod cleanly,
   backs up saves, and turns diagnostics off.

## Running it (developers)

Requirements: the tested game build, OpenKH Panacea installed in the game folder, SteamVR, Visual Studio
2022 with C++ x64 tools, and Python 3 with Pillow and NumPy.

```bat
python -m venv .venv
.venv\Scripts\pip install pillow numpy
scripts\build_probe.cmd
```

On first use `scripts\kh2run.py` writes `scripts\kh2_local.json` (git-ignored): set `game_dir` and
`mod_dll_dir` (`<OpenKH folder>\mod\kh2\dll`) there. Then, with SteamVR running:

```bat
.venv\Scripts\python.exe -I scripts\kh2run.py start play1 d3d_capture=1 d3d_observe=1 d3d_inject=3 d3d_eye_show=0 xr=1 xr_head=1
.venv\Scripts\python.exe -I scripts\kh2run.py set play1 xr_recenter_seq=1
.venv\Scripts\python.exe -I scripts\kh2run.py stop play1
```

`start` verifies the exe, installs the probe and launches the game; load your save as normal. `set`
changes live settings (recentre: change the number each time; HUD: `hud_dist`, `hud_down`, `hud_width`,
`hud_tilt_deg`). **Always run `stop`**: it closes the game and removes the probe, so a normal launch stays
unmodded. Logs go to `runs\<name>\` (git-ignored). Every setting, with a comment, is in the
`Settings` struct near the top of `src/probe/kh2vr_probe.cpp`.

## How it works (short)

`src/probe/kh2vr_probe.cpp` is a Panacea plugin. It hooks D3D12 command lists to find the scene segment of
each frame, records it, and replays it into per-eye targets with patched camera constants. The post-
processing stage (HUD and screen effects) is captured separately and matted for transparency. An OpenXR
session on the game's device presents the eyes, the HUD and effects layers, and a flat screen. Game
addresses are fixed for the one supported build, and the launcher checks the exe's SHA-256 first.
[RESEARCH.md](RESEARCH.md) has the research and decisions, [STATUS.md](STATUS.md) the dated log of every
step, measurement and workaround.

## Contributing

Issues and pull requests are welcome, especially from other GPUs and headsets: report your hardware,
runtime and what you saw in each eye. Keep the repository free of game material: no game files, extracted
assets, decompiler output, disassembly or crash dumps (see `checklists/before-publishing.md`). The project
was developed with an AI coding agent; `AGENTS.md` and the skills in `.agents/skills` describe the working
rules and test harness, and are useful for human contributors too.

## Licences

The mod's code is MIT ([LICENSE](LICENSE)). The development kit it started from is MIT
([KIT-LICENSE](KIT-LICENSE)). OpenXR headers (Khronos OpenXR SDK 1.1.65): Apache-2.0, The Khronos Group
([third_party/openxr/LICENSE](third_party/openxr/LICENSE)).
