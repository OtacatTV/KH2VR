# Status

The agents' scratch pad for this mod. Every session reads this first and adds a dated entry to the
Log (newest first) before it stops: what changed, the evidence (commit, run folder, test result),
and what's next. Keep the sections above the Log current. Keep it factual; it's how the next
session (or the other agent) picks up the work.

## Current state

- Runtime: OpenXR via SteamVR (Bigscreen Beyond 2). Agreed 2026-11-09. RESEARCH.md §4
- Stereo: agreed 2026-11-09 as same-frame at headset rate with the game ticking at 90. **Invalidated by step 3 (2026-11-09): the game's simulation tick is fixed at 60/s at every frame-rate setting**; Unlocked only adds extra Presents from a second thread. Needs re-agreeing with the human: see RESEARCH.md §4 note and the open issue on the second Present. Cutscenes (30 fps) on a cinema panel for M1. RESEARCH.md §4
- Camera mode(s): M1 third-person camera swap (agreed 2026-11-09); M2 diorama option for combat proposed
- Input mode(s): existing gamepad controls for M1 (agreed); tracked input deferred
- Capabilities and action mappings: RESEARCH.md §5 (memory R/W and per-frame callback confirmed via Panacea/LuaBackend; camera records found for build 15194255, see RESEARCH.md §3 and Log 2026-11-09 step 4; input API and observation addresses unknown). No CONTROLS.md yet
- Publisher guidance and chosen scope: RESEARCH.md §2 (SE Material Usage Policy forbids modifying without permission; app EULA not yet read; no anti-cheat found). Scope (2026-11-09): personal experiment; public release maybe later
- Launch for testing: direct exe launch works (game folder has `steam_appid.txt` = 2552430); no launcher script yet. Game at `F:\SteamLibrary\steamapps\common\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-`, exe hash matches the pin (checked 2026-11-09)
- Who owns the game install right now: nobody (2026-11-09 step-1 smoke runs finished, game closed)
- RenderDoc on the PC (2026-11-10, human approved): v1.47 portable zip from renderdoc.org, signature verified
  (key 1B03 9DB9 A471 8A2D 699D E031 AC61 2C31 20C3 4695, matches renderdoc.org/builds), unpacked to
  `<downloads>\renderdoc\RenderDoc_1.47_64` (nothing installed system-wide; Vulkan layer not
  registered on purpose). Launch: `renderdoccmd capture -d <game dir> -c <run>\kh2 --opt-capture-callstacks
  --opt-capture-callstacks-only-actions <exe>`, F12 in game. Analysis scripts `scripts/rdc/*.py` run via
  `qrenderdoc.exe --python <script>` with KH2VR_* env vars. Captures (*.rdc) are git-ignored.
- Mod loader on the PC (2026-11-09): OpenKH Build 1806 (release2-1806) at `<downloads>\openkh\openkh`; Panacea 1806 installed as the game folder's `DBGHELP.dll`; LuaBackend v1.9.1-hook as `LuaBackend.dll` (loaded by Panacea). `panacea_settings.txt`: mod_path `<downloads>\openkh\openkh\mod`, show_console=true. Plugin DLLs go in `<mod_path>\kh2\dll\`, Lua scripts in `<mod_path>\kh2\scripts\` (both empty = clean profile). Pre-existing Dec 2025 files backed up to `<downloads>\kh2-gamefolder-backup-2026-11-09`

## Rendering compatibility

- Renderer: **Direct3D 12, confirmed 2026-11-09** by the swapchain probe (the game's only swapchain's device
  is an `ID3D12Device`; device max supported FL 12_2 on the 4090, creation FL unknown). Swapchain: flip-sequential,
  3 buffers, B8G8R8A8_UNORM, 2560x1440 windowed, flags 0x42 (allow mode switch + frame-latency waitable), sync
  interval 0. Store's "DirectX 11" is a minimum spec, not the API. Formats of scene/depth targets and AA/temporal
  effects still unknown until a capture. Baseline: vendor-neutral D3D12 + OpenXR (`XR_KHR_D3D12_enable`), no vendor SDK
- Evidence matrix: empty (no hardware tested). Target rig: RTX 4090 (NVIDIA driver 617.42), Bigscreen Beyond 2 via SteamVR, Windows 11 Pro 10.0.26200. Planned scenes in RESEARCH.md §7
- Claims: all combinations untested. Note AMD RX 7000/ROG Ally launch crashes were fixed by the 2024-08-26 game update
- Unsupported settings: planned visible flat cinema-panel fallback with reason; never silent mono

## Rendering workarounds

One entry per broad workaround; link longer evidence rather than copying logs here.

- [ ] **W1 XR staging copies** (2026-11-10, with `xr_own_queue=1`): every eye-image frame the game's list also
  copies both eye targets (2 x 2560x1440 BGRA) into one of 3 staging slots; the XR thread copies the newest slot
  into the swapchain on its own queue. Reason: the session's queue gets the runtime's per-frame waits (game fell
  to 36-43 ticks/s when that was the game's queue), and our queue must not read the eye targets while the game's
  next frame renders into them. Scope: XR output only. A/B: `xr_own_queue=0` (session on the game's queue, no
  staging; game slows). Cost: 2 GPU copies of ~15 MB per game frame (not yet timed on the GPU; game-thread CPU
  unchanged, Present 0.23 ms, replay 0.20 ms). Replace with: rendering the eyes straight into swapchain or
  per-frame eye targets. Revisit at render-resolution changes and before release.
- [ ] **W2 Kept MSAA HUD target sets** (2026-11-11, `hud_ensure_ms`): the HUD's MSAA targets (panel and
  screen-effects, 2 colour + 2 depth each, at the scene's 2560x1440) are created once per sample count (2x/4x/8x)
  and never released while running. Reason: freeing the 8x set when the game switched back to 2x (menu exit) while
  the GPU still drew into it hung the GPU (driver reset, game crash; runs camcrash2/3, stack2). Scope: frames where
  the game renders MSAA (menu exits, loads). No A/B switch (the freeing path crashes). Cost: ~1 GB of VRAM once
  the 8x set exists (peak 5.4 GB of 23.4 GB in run camfix2). Replace with: release after a fence passes, or HUD
  targets at 1x with the HUD resolved. Revisit before release and on GPUs with less VRAM.
- [ ] [ID / introduced commit: optimisation disabled or work forced; correctness reproduction;
  exact scope and activation log; A/B switch; costs by representative scene (unknown stays
  unknown); narrower replacement or removal criterion; revisit on new scene types and release]

## Performance and release evidence

- Baselines and budgets: [fixed checkpoints/routes, matched settings and builds, repeated A/B
  runs, timing/throughput/tail/pose-age budgets and configurable regression review triggers]
- Results: [small scene plus applicable dense interior, terrain and busiest gameplay; CPU/GPU
  times, fresh delivered stereo, gaps/drops and pose age; missing metrics and profiler overhead]
- Package audit: [package hash, effective defaults/settings, active workaround IDs, diagnostics
  off, normal-launch and settings/load recovery checks; evidence and untested gaps]

## Waiting on the human

- [ ] Gamepad model (headset/GPU/seated answered 2026-11-09; GPU driver 617.42 read from the PC 2026-11-09).
- [ ] Read the game's Steam EULA and note its date (before any public release).
- [ ] Voice-recorded playtests: deferred by the human (2026-11-09); set up before the first headset playtest.
- [ ] On-PC discovery pass (needs a Claude Code session on the Windows PC, or the human running steps):
  1. ~~Clone this repo there; install OpenKH Mods Manager + Panacea + LuaBackend (Steam, clean profile).~~ Done 2026-11-09, see Log.
  2. ~~Confirm D3D11 vs D3D12 for the renderer (RenderDoc/PIX capture or a swapchain probe).~~ D3D12, 2026-11-09, see Log.
  3. ~~Frame-rate check: Unlocked + 90 fps cap vs 60; time a fixed run and a combo at both.~~ Done 2026-11-09: tick fixed at 60 in both; the 90 cap is moot. See Log.
  4. ~~The flat-screen camera experiment (RESEARCH.md §6).~~ **Passed 2026-11-10** (yaw and sideways
     translation, both within 5% of prediction; see Log). On-PC discovery pass complete.
- [ ] `AGENTS.md` Commands section brackets (build/tests/launch/test driver) — filled once tooling exists.

## Open issues

- [x] ~~**Twin bundle pool overflows**~~ (runs head2-5: one burst of `pool=86..92` failures during the
  `load_spot` walk; overflow bundles drew with the game's camera that frame). Fixed 2026-11-10: pool 512 per
  tick per eye (lazy allocators), eye-camera ring 1024 blocks per region; STATS shows `twin peak/tick`.
  head6: pool=0, peak 206 in the flow, **355 during the human's free play**. Revisit if the peak nears 512
  (busier scenes, combat): size by renders instead of ticks.
- [ ] **Post stage missing from the eye images** (glow, haze, sun flare, HUD): the replay segment ends before
  the post stage; the monitor shows the game's post stage on top of the eye copy with the game's camera.

- [ ] **Update 2026-11-10 (latest):** Sora's model-view lives only in the GPU upload ring (memcpy from fn
  exe+0x111bd8), built from the setter's final view; exe+0x1a94c0 is culling, not the builder. Injection plan:
  setter eye/target. See Log "Sora's model-view in upload memory". Earlier:
- [ ] **Update 2026-11-10 (later):** the copy is the setter matrix exe+0xac1028 itself; M1 and exe+0xac1b58
  are ruled out. Reader fn exe+0x1a94c0 (~90 reads/tick) is suspected, not confirmed (see Log, unattended
  live tests). Original note:
- [ ] **Sora's model-view is built on the CPU from a camera copy we haven't located** (narrowed 2026-11-10,
  RenderDoc): Sora's draws carry world x view in their per-object block with an identity view in their camera
  block, while world and companions get world-only matrices plus the shared camera block (= M2). So M2 edits
  leave Sora fixed (`runs/2026-11-10-m2-viewyaw/AC.preview.png`). The per-tick setter edit moves Sora correctly,
  so the copy is derived from it. Next: find the CPU code that multiplies Sora's world by the view (watch the
  model-view's source, or candidates M1 exe+0x8aed40 / setter exe+0xac1028 edited alone with region checks).
  Sky/sun camera not traced yet (bundles 2-3).

- [x] ~~Probe camera adjust ran twice per tick on the eye~~ (setter writes the eye's w twice). Fixed 2026-11-10:
  all edits happen on the target write (once per tick); rerun `runs/2026-11-10-step4-sideways` shows
  eye_hits=2090 vs 584 adjusts, sideways test symmetric (+46.2/-46.2 px).
- [x] ~~Horizontal FOV only derived from image shifts.~~ Confirmed 2026-11-10 from the GPU side: the applied
  projection is in root parameter 0 (xs 0.805, ys 1.430 = 102.3 x 69.9 deg), checked by projecting the three
  characters' feet onto the capture thumbnail (see Log). Still to find: where the CPU keeps it (for per-eye
  projections) and what the 2.56x-larger matrix in the camera block is for.

- [ ] Extra Present per game tick at Frame Rate = Unlocked: tick (Panacea `OnFrame`) 60/s, Presents ~120/s,
  one on the main thread and one on a second thread; at Frame Rate = 60, one Present per tick, main thread only.
  ~120 matches the desktop monitor's 119 Hz refresh (not yet proven to follow refresh). Under a 45 Hz tick cap the
  second-thread Presents nearly stop (~5/s). Repro: `kh2vr_probe` run, STATS `on_main_thread` vs `presents`
  (`runs/2026-11-09-step3-framerate`). **Answered 2026-11-09: fresh renders** (see Log, step 3b). Still open:
  what drives the second thread's rate (it ran ~120/s normally but ~400-480/s while Desktop Duplication was
  capturing), whether only the camera or also animation is interpolated, and the unexplained 60/s and 30/s
  stretches at the start of the Unlocked capture.
- [ ] Probe setting `dummy_d3d12=1` hangs the GPU (LiveKernelEvent 1a8/1b8 then app crash in KERNELBASE)
  right after the game's first Present (run `runs/2026-11-09-step2-probe`). Off by default; don't create
  objects on the game's D3D12 device from outside its own frame flow without understanding why.

## Log

### 2026-11-11 Claude + human: source published publicly (early experimental build)

- Human's decision: publish the source publicly at github.com/OtacatTV/KH2VR as an unofficial, non-commercial
  early experimental build, open to contributors; licence MIT, "KH2VR contributors". Shown first: Square
  Enix's Material Usage Policy (RESEARCH.md section 2); recorded in RESEARCH.md and AGENTS.md.
- Pre-publish checks on the tracked tree: no personal data left (the Windows username in 3 local paths
  replaced with placeholders; commit authors are all the agent's noreply address), no secrets, no runs of
  game bytes, no binaries, dumps, captures or disassembly (130 text files). Game addresses (RVAs) for the one
  build remain; the launcher refuses other exe hashes.
- Published as a fresh repo with one squashed commit of this tree (the private history stays local).
- Added README.md (write-up of what works, tested setup, known issues, roadmap, developer run steps,
  contributing) and LICENSE.
- Not done (player-package items, out of scope for a source-only build): installer, diagnostics-off player
  profile, rendering compatibility matrix beyond the one rig, tag and release notes.

### 2026-11-11 Claude + human: summon black screen fixed (flat fallback); 3D summons half work

- **Black screen on summons** (human: Chicken Little): the summon intro draws without the camera block on the
  list, so `skip_nonworld` (meant for menu models) skipped all ~300 frames (run `2026-11-11-mm2`: non-world
  skips +300, new eye images 0/s) while the gameplay flag still said gameplay. The headset kept the last eye
  image, the game's fade to black. Fix: `flat_noreplay` (default 6 ticks): in gameplay with no eye image for
  that long, the flat screen shows the game's own frame; back to VR when eye images resume. Walking: 0 flat
  frames (run `summon1`). Human: "looks good".
- **3D summon experiment** (`skip_nonworld=0` live, headset recording in `runs/2026-11-11-summon1/rec`): Sora
  and the summon render in stereo with the right camera ("the new camera worked"), but the background is
  black in both eyes. It is drawn outside the replayed segment (before it, or in the post stage), like the
  sky and light effects in backlog item 4. Parked with that item; default stays `skip_nonworld=1`.

### 2026-11-11 Claude: minimap backdrop, quick fix tried and dropped

Run `runs/2026-11-11-mm2` (Back Alley spot; sky and a building edge behind the minimap on the monitor). The
minimap's semi-transparent corridor fill shows the monitor's view (building windows) faintly in the headset;
the dark backdrop itself is fine (black with alpha).
- Colour-to-alpha against the pre-HUD image (most transparent layer per pixel): **worse**, the monitor's
  windows show more clearly (low alpha amplifies the background). Reverted.
- With the minimap special case off, the HUD matte gives a solid yellow/white square there, with
  `hud_clear_alpha` 0, 0.5 and 1 alike: the minimap's draw isn't plain alpha and isn't masked through the
  target's alpha. A proper fix needs the minimap's passes mapped (bundles, blend states, textures it samples)
  and drawn correctly into the HUD images. Estimate: a couple of hours. Parked; the bleed is faint and the
  human found camera positions where it's barely visible.

### 2026-11-11 Claude + human: backlog and order (human's list, organised)

Agreed next: minimap backdrop -> lock-on/Triangle in 3D -> anti-aliasing. The human's full list, grouped by
shared discovery work, with Claude's first feasibility estimates (not measured yet):

1. **Minimap backdrop** (small): subtract the pre-HUD image inside the minimap rect, so the monitor's world
   doesn't show through. Proper fix (replay the backdrop draw with a working blend) only if other HUD parts
   need it.
2. **Summons: screen goes black (e.g. Chicken Little)** [bug, major]: not looked at yet. Likely a different
   render path or a full-screen layer the replay or flat-screen logic doesn't expect. Medium; reproduce with a
   summon save first.
3. **World sprites drawn from the HUD stage** [major], one discovery effort for all of them:
   lock-on marker and Triangle prompt in 3D; magic (flat, wrong perspective); 2D enemy drops in 3D space. They
   are drawn in the post stage, so they land on the panel. Needs telling world sprites from HUD sprites and
   drawing them per eye at a depth (game's world position if findable, else scene depth under the sprite).
   Medium-hard; lock-on/Triangle first.
4. **Rendering correctness** [major]: sky and light effects (Twilight Town lights, skybox effects), probably
   drawn outside the replayed segment or from the monitor camera; character culling (enemies and NPCs must be
   visible anywhere in the VR view: the game culls with its own narrower view, so its cull test has to be
   widened). Medium each, culling maybe harder.
5. **Anti-aliasing** (jagged characters) [major]: after the HUD settles. Options: the game's own MSAA path in
   the eye replay, or a post-process AA (FXAA/SMAA) per eye. Medium.
6. **Camera** [polish]: right stick vertical moves the camera straight up/down and stays level (no orbit or
   pitch down); the built-in first-person camera stays on between rooms; first-person height closer to Sora's
   eyes. The camera block and its write point are known (step 4), so low-medium.
7. **Menus and HUD polish** [polish]: clean loading transition (black background, keyhole animation shown
   well); movable HUD items (the split-panel variant, also wanted); VR settings menu (HUD elements, world scale
   etc.; settings file first, in-game overlay later); layered menus with depth (Sora and party walking between
   foreground and background menu elements). Layered menus are the hardest (needs the menus understood in
   detail); the rest medium.
8. **Comfort and smoothness** [polish]: frame generation or reprojection to guarantee smoothness. SteamVR
   already reprojects at 75 Hz; own depth-based reprojection is possible (the eye depth exists) but hard.
   Measure first.

Reminder from the human: the final stage is mostly polish, and it needs the menus understood well.

### 2026-11-11 Claude + human: HUD freeze fixed; camera-setting crash was a GPU hang, fixed; crash catcher

- **HUD freeze (human: "all the animations on the hud freeze when walking")**: in one-list frames (scene, glow
  and HUD in the same command list) the HUD stage was never seen after the list's replay fired, because
  `inj_track` stops returning a list once it's done, so the render-target binds after the replay went untracked.
  Fix: `hk_om` keeps tracking binds of done lists (`last_rt`, glow marker). On the way: the HUD stage now needs
  a glow (640x360) target bound before the full-size HUD target (`rt_is_glow`; the scene list before its own
  replay had put world shadows on the panel), one HUD pass per replay (`g_hud_list_fire` vs `g_fire_seq`; two
  per tick made the panel flash), render-target identity by resource not handle, MSAA HUD targets when the
  game's scene is MSAA, and `hud_target` (full size, scene format). Diagnostic `cmd=tick_log`: one line per
  tick with each list's segment/fired/bundles/HUD-stage state. Human confirmed: "the hud seems to be
  functional now". Scripted check run hudms3: HUD frames 300 per 5 s while walking.
- **Minimap**: its see-through backdrop doesn't survive the HUD redraw, so the panel takes the minimap rectangle
  (`hud_minimap_rect`, game pixels) by comparing the finished image with a copy made right before the HUD:
  unchanged = transparent, darker = black with matching alpha, brighter = opaque. Fixed the blinking and the
  monitor image showing through. `hud_fx_offscreen` (readable geometry outside the screen rect -> effects
  layer) is off by default: the command-menu frames and the minimap are outside that rect too (they appeared
  enlarged in the effects layer).
- **Crash when changing Field Camera in Config (human, 4 times in the headset; never in scripted runs with
  the headset off)**: Windows reported a stack overflow in clr.dll. Actual chain, from the new crash catcher:
  leaving the menu the game draws one 8x MSAA frame and then 2x; the probe freed its 8x HUD targets ~50 ms
  later, while the GPU could still be drawing into them -> GPU hang -> NVIDIA driver reset (System log
  nvlddmkm 153 at each crash: 17:59:49, 18:13:48, 18:19:25, 18:25:56) -> the game's next D3D12 call threw
  `_com_error` inside D3D12Core -> the .NET runtime's handler reported it as a stack overflow (the game's
  fiber stack is fully committed). Fix: W2 (keep a set per sample count). Verified run camfix2: human toggled
  the setting 3-4 times in the headset, no crash; 32 set switches, no driver reset, no D3D12 exceptions.
- **Crash catcher** (`crash_catch=1`, default on): on a stack overflow anywhere or an access violation in
  the probe, logs the fault (module+offset), an unwound backtrace with the most repeated return addresses,
  stack headroom and VRAM, and writes `crash.dmp` to the run folder (git-ignored; holds game memory). Also
  logs C++ exceptions thrown inside D3D12Core (type name + stack; none in normal play). `cmd=crash_test`
  overflows a spare thread on purpose (kills the game; proved the catcher in runs crashtest/crashtest2).
  `stack_watch=1` logs the main thread's stacks (it switches between fibers, 1 MB each). Build now writes
  `build/probe/kh2vr_probe.map` to map probe offsets to functions. STATS logs VRAM usage vs budget.
- One start (run camfix1) lost the D3D12 device while the XR session was created, two minutes after a GPU
  reset; the retry was fine. Watch for a repeat after a clean reboot.
- Next: the human's call. Open items: battle-start shards on the panel, lock-on/Triangle icons anchored in
  3D, split HUD panels, AA, slightly tall sparks, dead `g_segment_rts` code, 2 old CopyResource debug errors.

### 2026-11-11 Claude + human: gameplay flag found; menus, pause, loads on a flat screen

- **State-flag discovery tool** (`cmd=bscan_snap` with `bscan_label` 0 field / 1 inventory menu / 2 battle pause,
  `cmd=bscan_find`, `bscan_small`, `bscan_clear`): snapshots the game image's writable data (~39 MB, 90 regions)
  and lists bytes constant per label. Runs `flag1`..`flag3`: 15 snapshots (field incl. walking, orbit, attack,
  fights; menu incl. the Items submenu; battle pause), each checked by a monitor screenshot.
- **`exe+0x717418` (build 15194255) = 1 during gameplay, 0 otherwise**: watched live (`mon=... mon_bytes=1
  mon_change=1`, logs on change every tick): 0 on title / load menu / loading, 1 from gameplay start, stays 1
  through walking, orbit and attacks, 0 the tick the menu opens, 1 the tick it closes; snapshots: 1 in fights, 0
  on the battle pause screen. Other candidates lagged or flickered during loads (e.g. exe+0x8a8271).
  Not in a per-build table yet: it's the `flat_flag` setting's default (override per build); the launcher's exe
  SHA-256 pin keeps the probe off other builds.
- **Flat screen** (`flat=1`): while the flag reads "not gameplay" for `flat_after` (2) ticks, the finished
  backbuffer is copied right before each main-thread Present (own list on the game's queue, 3-slot staging,
  fenced) and shown as an opaque world-locked quad in front of the recentred view (`flat_dist` 2 m,
  `flat_width` 2.6 m); the HUD panel and effects layer hide meanwhile; the world stays behind it. First attempt
  (copying the scene image at list close) failed: menu UI lists aren't tracked lists (56 of ~30k).
  Unattended (`flat9dbg`): inventory menu on the screen, back to normal on close, 0 new debug errors.
  **Human (`flat10`): battle pause, loading keyhole, cutscene: "all look good".**
- Next: minimap (white box on the panel), battle-start shards, ledge recheck after `skip_nonworld`.

### 2026-11-11 Claude + human: menus and pause: findings, flat fallback (experimental, off)

Human: two pause menus exist: the inventory menu (Start in the field) and the battle pause screen.
Runs `pause1`, `flat1dbg`..`flat6` (inventory menu via `tests/pad/pause_toggle.pad`, unattended).
- **Replay signatures** (new `INJ sig` log line, on change): field scene replays bind the camera block on the
  list 7x (27 bundles); the inventory menu's replay has 3 bundles, **0 list-level r2**, r2 only inside the menu
  models' bundle. Replaying it put black + menu models into the eyes. New `skip_nonworld=1`: segments with no
  list-level r2 (in the ops or the start state) aren't replayed -> the world stays frozen behind the menu.
  **To verify with the human: the ledge/plaza frames** (they lacked list-level r2 in the ops; if their start
  state lacks it too, the ledge hitch returns -> set `skip_nonworld=0` and refine).
- **The menu keeps rendering the world** behind its panels (~25 eye images/s while open, `flat6`), so "no world
  replay for N ticks" doesn't detect menus reliably. Flat fallback built but **off** (`flat=0`): game side
  stages the game's finished scene image at the end of lists that drew into it once `flat_after` ticks pass
  without a world replay; XR side shows it as an opaque world-locked screen (`flat_dist/width/down`) while flat
  frames keep coming, and hides the HUD/effects layers. Needs a reliable menu/pause/load signal: next, find the
  game's menu/pause state in memory (probe memory-diff tools) and drive the flat screen from it.
- Minimap (white box): not started this round. Debug layer: 2 old CopyResource-state errors seen in HUD runs
  (destination in RENDER_TARGET), source not yet traced.

### 2026-11-11 Claude + human: screen-effects layer; battle-start impact not yet separable

- **Screen-effects layer** (`hud_fx=1`, `fx_dist` 2 m): a second matted image (same pipeline as the HUD panel)
  shown as a head-locked quad (VIEW space) sized to cover the headset FOV; order projection < effects < HUD.
  Calm scenes: invisible, 0 new debug errors (`fx6dbg`).
- **Battle-start impact** (human: "still in the box", `fx7`): `cmd=big_log` (large HUD-stage bundles, de-duped
  by shape) in `fx9` during battle starts shows: besides the glow composite, **3D meshes** (vertex stride 40,
  GPU-only vertex buffers, 190-1730 vertices, VB 13-45 KB) and a 1408-draw bundle in another coordinate range
  (~13000; particles). The **command-menu frames are also stride-40 GPU-only meshes** (VB <= 4.4 KB, `fx10`:
  routing all unreadable meshes to effects removed the menu frames from the panel). Current rule: readable
  geometry outside the screen rectangle -> effects; unreadable meshes stay on the panel. The shards still land
  on the panel. Next: read mesh positions via GPU readback (or another per-bundle signature) to separate shards
  from menu frames.
- Still open: minimap white box; HUD/effects capture while no scene is drawn (pause, load keyhole).

### 2026-11-11 Claude + human: HUD as a floating panel (v1), transparency, screen-wide effects

Runs `hud4dbg`..`hud11` (Twilight Town; human in the headset for `hud5`, `hud8`, `hud11`).
- **HUD panel** (`hud=1`): in the post-stage list after this tick's replay, bundles that bind the scene image
  after the glow chain are the HUD; each is executed again with our HUD targets bound (state restored after),
  and at the list's end the result is staged and shown as an OpenXR quad layer (own swapchain, our queue,
  fenced) placed in front of/below the recentred view (yaw only). Live: `hud_dist` 1.4 m, `hud_down` 0.35 m,
  `hud_width` 1.5 m, `hud_tilt_deg` 15. Human: "looks decent as long as it's adjustable". Monitor keeps its HUD.
- **Transparency** (human: "a number of transparency issues"): difference matting: the HUD is drawn twice, over
  opaque black and opaque white; a compute pass gives alpha = 1 - mean(white - black), colour = black / alpha
  (straight alpha, `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`; `hud_premul`, `hud_debug` 1/2 show the raw
  black/white images). Menus, prompts, portraits and gauges now come out right.
- **Gray layer in battle** (human): the glow composite (first scene-bound bundle after the glow chain, a
  full-screen quad) landed in the HUD. Now left out by size: bundle vertex bounds in the game's screen units
  (full screen = 1792..4352 x 1840..3280; `post_log` prints draws/verts/bbox; ListRec table 512 -> 8192, it
  overflowed for bundles); `hud_skip_fullscreen=1` drops quads covering >=90% of the screen.
- **Open:** the **minimap** comes out as a white box (its dark backdrop doesn't survive the redraw; plan: take it
  from the game's finished frame). The **battle-start impact effect** (screen shatters into shards) still lands
  on the panel as a frozen picture: no single shard is full-screen. Human wants screen-wide effects (impact,
  load-screen keyhole) **across the whole VR view**: plan = classify by a bundle's total vertex bounds, route
  screen effects to a second, head-locked full-view layer; keep the glow composite out. While paused the game
  draws no scene, so the HUD stage logic (tied to the tick's replay) doesn't run: the panel freezes.

### 2026-11-11 Claude: HUD mapped: the post stage's last bundles, separable from the scene

Runs `hud2`, `hud3` (Twilight Town, Back Alley spot). Tools: `cmd=post_log` (logs the next 80 bundles executed
after this tick's replay, with list and render target) and `hud_skip_lo/hi` (skips that range of them on the
game's own list; live). Note: the test driver needs the physical controller plugged in (`hud1` stalled on the
load menu without it).
- In Twilight Town the post stage is its own list: bundles 0-1 into the scene image, 2-15 the glow chain
  (ping-pong between two other targets), **16+ into the scene image = the HUD only** (one frame; indices vary).
- Hiding (monitor, `runs/2026-11-11-hud3/skip_grid.png`): 16+ removes the whole HUD and leaves the scene and the
  sun glow intact; 16-30 = command menu, minimap, the Triangle icon over the lamp; 31+ = the "Examine" prompt,
  party portraits, HP/MP gauge; 0-15: no visible change at the spot.
- Design proposal: replay the HUD bundles (post-stage bundles after the glow chain that target the scene image)
  into our own transparent 2560x1440 HUD texture each frame; show sub-rectangles of it as OpenXR quad layers
  (command menu, HP/party, minimap, prompts), each with live placement settings; the lock-on and Triangle icons
  get placed per eye at their 3D target (screen position from the HUD texture + the game's depth at that point,
  unprojected with the game camera). To check first: the alpha the HUD draws leave in a cleared target.

### 2026-11-11 Claude: stopping point and plan for the next session (agreed with the human)

State at commit e6b3bb0: VR output at 75 Hz from its own thread, game at 60 ticks/s, eyes at the headset FOV
(2560x2336), head tracking, MSAA paths, per-eye history, effect remaps; Twilight Town and Land of Dragons
verified with the human. Game closed, probe removed. Next, in order:
1. **HUD + post stage** (human's priority): lock-on and Triangle icons per eye at their 3D targets; other HUD
   elements on adjustable floating panels (in sight, out of the way; live settings for distance/size/angle);
   glow/haze per eye. Start by mapping which post-stage draws belong to which HUD element and where the lock-on
   and reaction icons get their screen positions. Also removes the flat HUD baked into Land of Dragons eyes.
2. Small items: AA (compute FXAA on the eye images, the blit infrastructure exists); pre-create MSAA eye sets
   (~130 ms first-use stall); slightly tall sparks; the two ~44/s dips near the ledge; new screen refs for
   `load_spot` with the wide eye crop.
3. Later: pose prediction for the render time, per-eye asymmetric projections, cutscenes (cinema panel or
   stereo), menus/game-over screens (frozen image today).

### 2026-11-11 Claude + human: ledge hitch: open areas draw the world in 2 bundles

- Replaying the human's recorded route (`tests/pad/ledge_jump_route.pad`) **drifts** (Sora ran into a wall,
  human); recorded input isn't a reliable repro over 100 s. Keep it only as a reference; use the human or short
  scripted moves from a known spot.
- Cause found (`ledge2`): in open areas (the plaza below the ledge) only ~210 of 300 ticks per 5 s got an eye
  replay. Those scene lists hold only **2 bundles** (most of the world culled; facing the bright open area), so
  the trigger's `d3d_inject_min_bundles=10` never let them fire. Fix: **default 2**, and the post-stage trigger
  now also fires at most once per tick (`g_last_fire_tick`), so a later list can't overwrite the eye images.
- Human (`ledge3`, threshold 2): **"the hitching didn't happen this time"**; ~59-60 new images/s for most of the
  session, two 5 s windows at ~44/s (00:57:01, 00:57:11; cause not captured: the new rejected-list log spent its
  6 lines on the loading screen). Land of Dragons on this build (`lod2`): 60 new images/s, stereo correct.
- Open: those two dips; the `INJ rejected at close` log (op shape + how many bundles bind r2 inside) should skip
  load screens next time.
- **VR-start stall** (human: "the camera hitched at some point in Land of Dragons"; `lod2` flow stopped at step 3):
  a **2.3 s game-thread stall** when VR switched on (first replay + OpenXR session creation inside the game's
  frame); in LoD that happens mid-walk, so the flow drifted. Fix: `xr_init` runs on its own thread (device from
  the captured queue). `lod3`: flow passes, worst stall at VR start **204 ms**; first use of each MSAA eye set
  still costs up to ~130 ms (8x) -> create them up front (open).

### 2026-11-11 Claude + human: white flash fixed; ledge hitch traced to a one-list frame type

Runs `hist1`, `hist2dbg`, `flash3`..`flash5` (Twilight Town; human in the headset for `flash3`/`flash5`).
- **Per-eye history** (`eye_history=1`): copies in the replay from a past-frame image (same size/format as the
  game's scene image, not a scene image, not written earlier in the segment; found with `cmd=copy_log`: the
  game copies its previous finished frame `...BFA0` into an effect texture each frame) get the eye's own
  previous image (per-eye history textures, stored after the eye images are final) via the compute blit. Blit
  descriptor slots now separate per source (eye 0-1, history 2-3, UAVs 4-7). Debug layer: 0 errors of ours.
- **White flash** (human, `flash3`): caught by recording VR View during play (`vrrec.py`, scratchpad): 9 bursts
  in 75 s of ~90% white frames with the HUD on top. Cause: the end-of-list fallback replayed whole lists that
  carry glow chain + HUD into the eyes (`INJ at close` op-shape log). Fixes: the fallback fires at most once per
  tick, and only for lists that bind the camera block (r2) themselves. `flash5`: 160 recorded frames during the
  human's play, **0 white**, human: "no flash".
- **Ledge hitch** (human, `flash5`: "hitched when I jumped off that ledge multiple times, reliably"): 50-70 ticks
  per 5 s without a new eye image there. Those frames use a different list shape: sky/background (2 bundles),
  then the glow chain (ping-pong targets with copies), then ~40 single-bundle binds of the scene target with
  depth (world, then HUD), no camera block on the list itself. The post-stage trigger misses it (glow chain
  starts after 2 bundles), and the fallback now rejects it (no r2) -> stale image (before: replayed whole ->
  white). Next: a RenderDoc capture at the ledge, then split that list (world into the eyes; glow/HUD not).
- **Repro of the human's route:** `tests/pad/ledge_jump_route.pad` (107 s, recorded with `pad_record=1`,
  `scripts/pad_from_log.py`), from the `load_spot` start point.
- Not rechecked after the r2 condition: Land of Dragons (its scene list binds r2 on the list, `fx4` dump).

### 2026-11-11 Claude + human: NPC hitch fixed; bright ghost "flash" = game image blended into the eyes

Human (run `npc1`, Twilight Town): NPC hitching fixed. New: "a really bright flash" when jumping/attacking,
more often jumping off a ledge toward the bright light and looking up; sometimes without an attack.
- **Reproduced** with `tests/pad/look_up_jump.pad` (camera up, jump B, air attacks A): the monitor (game's own
  image, `d3d_eye_show=0`) has no flash; the eye images get a large, blurred, bright, translucent copy of the
  game's view blended over them, including the game's HUD (`runs/2026-11-11-npc1/mon_vs_vr.png`,
  `runs/2026-11-11-flash2dbg/grid.png`). Live A/B: not the effect remaps (`eye_clip_fx=0`, counters frozen,
  flash stayed), not the eye FOV.
- **Mechanism:** screen-space effects (afterimage/blur/glare) sample an image of the scene. Part 1, fixed
  (`eye_copy_fix=1`): copies/resolves the segment makes *from the scene image* now read the eye's image; a
  compute shader (compiled at runtime with d3dcompiler_47, own root signature/PSO/heap; game heaps and PSO
  re-bound after) scales the 2560x2336 eye image into an intermediate of the destination's size, then copies.
  ~1 copy per frame per eye in Twilight Town (806-1222 per run); MSAA path: eye MSAA image resolved into the eye
  1-sample image, then scaled; the game's own MSAA -> main-scene resolve is left alone. Debug layer
  (`flash2dbg`): 2 errors of ours (that resolve case, now excluded), the rest the game's/SteamVR's.
  **Part 2, open:** the ghost still shows, with the HUD in it, so the effect also samples a previous finished
  frame (post stage + HUD), produced outside the replayed segment. Needs per-eye history (each eye's effects
  read that eye's previous image), which belongs with the post-stage/HUD work.
- Flows: `load_spot`'s final screen check fails while `d3d_eye_show=2` shows the wide eye crop (reference
  mismatch); passes with `d3d_eye_show=0`. Refs to update.

### 2026-11-11 Claude + human: combat in VR: MSAA regression, effects, Land of Dragons replay

Human in the headset (run `fov4`, Back Alley): full view "looks good"; hitching again when talking to NPCs and
fighting the bees; keyblade strike effects "stretched" and "not tied to the VR camera".
- **Hitch = my regression:** the MSAA eye sets were only created when the game's MSAA target matched the eye
  image size; with eyes at 2560x2336 (vs 1440) none were created and every MSAA frame (dialogue close-ups and
  fights use 2x/4x/8x, up to 70% of frames in a window) fell back to "other target". Fixed: compare with the
  game's 1-sample target. `fx4`: 240+ MSAA-path replays, 0 "other target".
- **No VR image at all in Land of Dragons** (`fx3`, combat save): 0 replays, every segment "no post stage": the
  scene list there ends after its last bundle and the post stage runs in a later list, so the trigger (first
  clear of a new target) never came; VR View showed SteamVR's empty environment. Fixed
  (`d3d_inject_at_close=1`): a segment that reaches its list's Close unfired is replayed there. `fx4`/`fx5`: eyes
  every frame, 60 new images/s in the fight. Side effect: there the game draws the HUD inside that list, so the
  eye images carry a flat, screen-fixed HUD (temporary, until the HUD work).
- **Effects:** two kinds of per-draw projections bypass the eye camera (no stereo, no head tracking, game FOV):
  (1) identity r0+80 (vertices pre-projected; ~600 draws/s, nothing visible changes when hidden in a calm scene,
  `fx2`) -> remapped game clip -> view -> eye pose -> eye clip (`eye_clip_fx=1`; `=2` hides them); (2) the
  game's projection in PS2 screen units (x/y ~275/298, centre 2048 in the z row; LoD effects, ~50% of r0 blocks
  there) -> eye pose applied and x/y scaled to the eye FOV. Human (`fx5`): "still perhaps a little tall ... much
  improved"; the lock-on icon is "a tiny bit off" (it's in the flat HUD copy). Remaining tallness: likely sprites
  sized in screen units in the shader (they keep the game's NDC size, ~1.7x too tall in a 107 deg view). Open.

### 2026-11-10 Claude: eyes rendered at the headset's FOV ("black box" fixed)

Human: "the game still appears as a black box ... consider increasing the FOV so I can best judge the menu
placement". HUD direction from the human: most comfort/customisation; lock-on and the Triangle (reaction) icon
must keep matching their targets in VR; other elements placed for immersion, in sight but out of the way.
- **Headset FOV** (logged by the XR thread, run `fov2`): left eye L-55.0 R42.3 U44.1 D-53.2 deg, right eye
  L-42.6 R54.6 U43.8 D-53.3; symmetric union **110.1 x 106.6 deg** (the game renders 102.3 x 69.9).
- **Eye projection** (`eye_fov=1`, default; `eye_fov_scale` widens it): our r0 copies (main list while the eye
  target is bound, and twins) get the projection scales of that union instead of the game's; the layer FOV uses
  the same values. The game's own picture is untouched. VR View: fills the whole view, characters keep their size
  (`runs/2026-11-10-fov2/pair.png`, top `eye_fov=0`, bottom `=1`).
- **Eye image size** (`eye_w`/`eye_h`, default **2560x2336** = this headset's FOV shape; read at eye-target
  creation): eye targets, MSAA sets, staging and swapchains use it; while an eye target is bound the replay
  scales the game's viewports and scissors to it. Monitor preview (`d3d_eye_show`) shows the centre crop.
  Debug layer (`fov3dbg`): 0 new errors. `fov4`: 60.0 ticks/s, 60 new images/s at 75 Hz, GPU 44% busy.
  Pixel density now ~23 px/deg both ways (was 13.5 vertically at 2560x1440 over 107 deg).
- Not checked yet: culling at the new top/bottom edges during motion (the game still culls with its own 70 deg
  vertical frustum; nothing missing at the spot), per-eye asymmetric projections (the union wastes ~12 deg per
  eye on the nasal side), the human's look in the headset.

### 2026-11-10 Claude: jagged edges: gameplay has no anti-aliasing; the post stage mapped

Human (after the hitch retest): "the characters look a little jagged"; agreed to add the remaining elements
first. Findings:
- **The game renders gameplay without anti-aliasing**, with or without our eye replays: run `aa1` with the
  replays held off (`d3d_eye_lock_segments=100000000`) counts every scene segment's sample count (new `SCENE
  samples` line): **1x in 300 of 300 per 5 s** during orbit, walk and idle. MSAA (2x/4x/8x) appears only in
  cutscenes and area loads (`hitch6`). So the jaggies are the game's own 1x image, magnified by the headset;
  not something our extra renders caused. Whether a game option changes this: unknown.
- **Post stage** (RenderDoc capture `runs/2026-11-10-renderdoc2/kh2_frame16438.rdc`, `.actions.txt`): after the
  scene (into the scene colour target, shadow target in between), a blur/glow chain ping-pongs between two other
  targets with copies (bundles), then ~30 bundles draw the composite and the HUD **into the scene colour target
  itself**, then a copy and one full-screen draw scale it to the 4K backbuffer. No separate AA pass.
- Options for AA later: (a) our own post-AA (FXAA-like compute shader compiled at runtime) on each eye image
  before staging, vendor-neutral; (b) find the game's MSAA switch and use the MSAA path in gameplay (its
  cutscene pipelines exist; cost x3 renders); (c) render the eyes above 2560x1440 (game viewports/passes).
- Next (proposed): replay the glow/haze part of the post stage per eye; take the HUD out of the eye images and
  show it as its own flat panel (OpenXR quad layer) — the HUD placement is a design choice for the human.

### 2026-11-10 Claude + human: camera hitch every 10-30 s: MSAA scene paths, and a stray barrier

Human in the headset (run `pace7`): "smoother, normal speed, some intermittent camera hitching roughly every
10-30 seconds". Runs `hitch1` .. `hitch6` (unattended, `load_spot` + orbit/walk pads + idle).
- **Reproduced from the logs:** game ticks steady at 60, but new eye images dropped (35-57/s in some 5 s
  windows, 0 for 40 s twice while the human stood still). Per-reason skip counters (new, in the `D3D inject`
  line) showed every lost frame was "other target": **the game renders some frames into an MSAA scene target**
  (2560x1440 fmt 87, 2x seen at area loads, 8x seen while idle in `hitch2`, 67-830 frames in a row), and the
  eye replay only knew the 1-sample target, so the headset re-showed the last image (hitch). What decides the
  path is unknown (possibly GPU headroom; not reproduced on demand in `hitch5`/`hitch6`).
- **Fix** (`d3d_eye_msaa=1`, default): up to 3 MSAA eye sets, one per MSAA scene target (same size and format),
  created on first use; after the eye replays the MSAA eye images are resolved (ResolveSubresource) into the
  1-sample eye images, so staging/XR/stereo checks are unchanged. `d3d_eye_show` copies the MSAA eye image over
  the game's MSAA target. `hitch6`: 92 MSAA-path replays at the area load, then ~2 min of motion and idle with
  **0 "other target" skips, 59.7-60.1 new images/s**. Image check after it: blank/stereo PASS, ref 0.55 (Sora
  had walked; preview looked at: correct alley scene). Not yet seen: an 8x frame through the new path.
- **Stray barrier found by the debug layer** (`hitch3dbg`: 13k errors, ids 527/538/1047): my staging patch left
  an extra barrier in the XR thread's list that moved the eye images to COPY_SOURCE on our queue and never back,
  so the game's next replay rendered into them in the wrong state across queues (also in the build the human
  tested in `pace7`). Removed; `hitch4dbg`: 9 errors, all the game's own startup ones (1422) and SteamVR's (916).
- New diagnostics: skip reasons, `xr images lost`, `MSAA-path replays`, and a TIMING line (Present, eye replay,
  ExecuteCommandLists CPU per call).
- **Human retest (same run `hitch6`, recentred 23:24:55): "No hitching"**, gameplay OK; the human also entered a
  cutscene ("we will probably want to fix them at some point"). Log: the game created **2x, 4x and 8x** MSAA
  targets over the session (all three eye sets used; all frames replayed, 0 "other target"). The 4x/8x stretches
  ran at **30 ticks/s with 30 new images/s** (23:25:26-23:25:36, 23:26:06-23:26:21): in-engine cutscenes run
  at 30 fps and use the MSAA path. Afterwards the game-over screen (no 3D scene pass, so no eye replay; the
  headset holds the last image).
- Open: cutscenes in VR (agreed earlier as a cinema panel for M1, RESEARCH §4; today they play in stereo at
  30 fps), menus/game-over screens (no scene pass -> frozen image), a 4th MSAA target would get no eye set
  (3 sets, all used in this session: 2x, 4x, 8x).

### 2026-11-10 Claude: headset pacing: game back at 60 ticks/s, headset fed at 75 Hz

Runs `runs/2026-11-10-pace1` .. `-pace7` (Beyond 2 at 75 Hz, chosen by the human; on the desk).
- **Bug: VR slowed the game.** Since head tracking (v0: xrWaitFrame/EndFrame on the game thread), the game ran
  at **36-43 ticks/s** whenever XR frames flowed (head1, head6 incl. the human's test; 60 without XR). The
  simulation is per tick, so the game ran at ~60-70% speed. Ruled out one by one, live: the copies (`xr_copy=0`,
  pace4: still 42), a lock in our code (pace3), GPU saturation (39% busy), game-thread CPU (new TIMING line:
  Present 0.23 ms, eye replay 0.20 ms per call). XR without eye replays: 55-57; eye replays without XR: 60.
  Cause: the runtime's per-frame synchronisation on the queue the session is bound to, i.e. the game's queue.
- **Fix** (`xr_thread=1`, `xr_own_queue=1`, both default): an XR thread runs wait/begin/locate/copy/end at the
  headset rate and publishes the latest pose; the game thread takes it each tick (no waiting). The session and
  the swapchain copies use **our own direct queue**; the game's list copies the eye images into a 3-slot staging
  ring and signals a fence; our queue waits on that fence and copies the slot into the swapchain. Each layer
  carries the pose its image was rendered with, so the compositor reprojects to the display pose.
- **Result (pace7):** game **60.0 ticks/s** (frame interval 16.7 ms), headset **75.0 frames/s**, 60 new images/s
  + 15 repeats/s, 0 layerless frames after the first. VR View: blank/ref/stereo checks PASS (ref = head5's VR
  View, similarity 0.74; correlation 0.93, dy 0.006); depth check FAIL as in head5 before this change: true-scale
  disparity is only an 11 px spread in VR View's mirror (near ground 180, characters 177, buildings 175, sky 169 px
  on top of VR View's ~170 px panel offset), below frame_check's tile threshold. Use region disparities here.
- Also fixed: the XR thread first stopped after a settings write (pace1): a reload resets `g_cfg` to defaults
  for a moment, so the loop no longer checks `g_cfg.xr`. Same caveat for any thread reading settings.
- Headset rate: 75 Hz fits better than 90 while the game renders 60 images/s (4:5 vs 2:3 repeats). Rendering
  the in-between frames per eye (Unlocked) is the route to a new image every headset frame.
- Not done: comfort/feel at the new pacing (human), GPU time of the staging copies, pose prediction for the
  game's render time (it uses the pose of the latest XR frame; the layer pose keeps reprojection correct).

### 2026-11-10 Claude + human: first headset test with head tracking: "World stays put"

Run `runs/2026-11-10-head6` (Back Alley spot via `load_spot`, `xr=1 xr_head=1`, Unlocked). Human seated with
the Beyond 2 on; recentred with `xr_recenter_seq=1` (reference at (-0.004 -0.001 -0.000) m). Human looked
around, leaned and walked Sora, then reported: **"World stays put."** ~4.5 min: XR frames ended 16800, forced
0, head_frames 16800; eye-camera ring missed 0; twins pool failures 0, peak 355/tick. Scale and comfort not
yet asked in detail. Before this run the twin pool was raised (see Open issues, fixed).
Next: the post stage (glow/haze/HUD) in the eye images, pacing at the headset rate, a known-bad frame_check
reference, then camera/input modes (RESEARCH §5) with the human.

### 2026-11-10 Claude: head tracking in the eye cameras; synthetic yaw passes in the headset images

Runs `runs/2026-11-10-head1` .. `-head5` (SteamVR + Beyond 2 on the desk, VR View open; nobody wearing it).
- **Probe** (`xr=1 xr_head=1`, default on): per tick in OnFrame `xr_tick_begin` (poll, wait/begin frame,
  locate views); the eye copies of every camera block (main-list r2 and twin-bundle r2) get
  `view x inverse(eye pose)` instead of the plain eye offset. Eye poses are relative to a recentre reference
  (eye 0's orientation + the eye midpoint; `xr_recenter_seq=<n>` recentres), converted to game view axes with
  C = diag(1,-1,-1), translation x `units_per_m x world_scale`. IPD comes from the runtime's eye positions.
  Test knob `xr_test_yaw_deg` adds a synthetic yaw. Frame flow: record copies in the replay, submit after the
  game's list (`hk_ecl`). XR log: frames = ended = head_frames, forced 0.
- **Result (head5, VR View, both eyes):** +10 deg yaw moves sky, mid-ground and characters by the same
  -95..-97 px in both eyes, dy 0..1 (sky -96/-109 at low correlation): a pure rotation, sky included. On the
  monitor (`d3d_eye_show=2`, head1) the scene moves +-181 px (predicted 182 for 10 deg at xs 0.805), Sora,
  Donald, Goofy and the lamp alike (`runs/2026-11-10-head1/sora_crop.png`); the first per-region numbers that
  disagreed were a bad measurement. **The monitor's sun glow, haze and HUD stay put**: the game's post stage
  draws them after our eye copy with its own camera. The headset images have no post stage at all (known gap).
- **Bug found and fixed: eyes locked onto the wrong target.** head3 ran with 3 replays and 25k skips: the first
  replay segment (load/title) bound an **8x MSAA** target, the eye targets were cloned from it, and every
  gameplay segment then mismatched (flat output, no error). Fix: lock only after `d3d_eye_lock_segments`
  (default 30) consecutive segments on the same target, and only a 1-sample one (`d3d_eye_single_sample=1`).
  head4/head5: "multisampled: not locking" logged once, then 2560x1440 1-sample targets, replays every frame.
- Diagnostics added: `cmd=ecam_log` logs 40 camera blocks before/after the eye edit, 40 distinct non-plain r0
  matrices and up to 40 eye-pass bundles without twins (STATS: `eye bundles untwinned`). Findings: the main-list
  camera block is a normal view; twins also carry per-character top-down blocks (z translation 1e7, likely
  shadow projections; only used while the shadow target is bound, where twins aren't used); the only
  non-plain r0 matrix is identity; untwinned eye-pass bundles: 26 at load, 0 in gameplay.
- Not done: real head motion (the headset lay still), comfort, the post stage in the eye images, pacing at 90 Hz.
  Twin pool overflow seen (`pool=86..92` failures in some runs at Unlocked): those bundles fall back to the
  game's camera for that frame (see Open issues).

### 2026-11-10 Claude + human: OpenXR output v0: both eyes reach the headset (head-locked)

Runs `runs/2026-11-10-xr1` (debug layer), `-xr2` (none). Human started SteamVR with the Beyond 2 and opened VR View.
- **OpenXR headers** vendored: `third_party/openxr` (Khronos OpenXR SDK 1.1.65: openxr.h, openxr_platform.h,
  openxr_platform_defines.h, LICENSE Apache-2.0), copied from a local OpenXR-SDK checkout. The loader isn't
  vendored: the probe loads the active runtime's (`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` = SteamVR;
  `SteamVR\bin\win64\openxr_loader.dll` 1.1.62; `xr_loader` overrides). Build: `/I third_party\openxr\include`,
  advapi32.lib.
- **Probe XR output** (`xr=1`, needs `d3d_inject>=2 d3d_capture=1`): queue ExecuteCommandLists hooked (slot 10,
  vtable from a throwaway queue) to capture the game's direct queue and to end frames; session with
  XR_KHR_D3D12_enable on the game's device + queue; 2 swapchains like the eye targets (2560x1440, **format 91 =
  B8G8R8A8_UNORM_SRGB**, same bits copied: the game's pixels are gamma-encoded), 3 images each; per tick after
  the eye replays: xrWaitFrame/BeginFrame/LocateViews, acquire, record copies into the game's list; after the
  game submits that list: release + xrEndFrame with a projection layer (view poses as located = **head-locked v0**,
  symmetric FOV from `xr_xs`/`xr_ys`, default the game's 0.805/1.431). A stale-frame valve ends a frame after 2 ticks
  if its list is never seen submitted (0 times so far).
- Results: adapter LUID matches the runtime's; session IDLE -> READY -> SYNCHRONIZED -> VISIBLE -> **FOCUSED**;
  frames ended every tick (0 forced), **0 debug-layer errors**. **60 frames/s to the runtime with the game at
  60 ticks/s without the debug layer** (~21/s with it). SteamVR reprojects to 90 Hz.
- **Image check (VR View, both eyes):** `kh2run.py snapwin <run> <name> "VR View"` (refuses when covered; a first
  unguarded capture caught the desktop and was deleted). frame_check on the split VR View: blank/stereo/depth
  checks PASS (correlation 0.94, dy 0.004), verdict INCONCLUSIVE without a reference. A "reject" image taken with
  `xr=0` is **not** valid: SteamVR keeps showing the last submitted frame (similarity 0.99). **Liveness proven
  instead:** the disparity between VR View's eyes follows the eye separation live: -266 px (3.15 units/eye) ->
  -119 px (60) -> -266 px (back to 3.15).
- Next: head tracking (put the located head pose into the per-eye camera copies, view space, incl. Sora's twins)
  so the world stays put; then a proper known-bad reference (session ended) for frame_check; pacing at 90 Hz.

### 2026-11-10 Claude: real-world scale: ~1 game unit = 1 cm; eye separation from IPD

- Character heights from the capture (frame 16438): feet in camera space from the draws' model-view (Sora
  (0, 167, 461), Donald (-98.9, 189.8, 338.5)) and the applied projection (ys 1.431) land on the feet in the
  capture thumbnail; reading the head tops there gives **Sora ~161 units, Donald ~94 units** (+-10%, head tops
  read by eye; Goofy ~156 is unreliable: stooped, hat). Bone blocks were too noisy to use (50 slots per block,
  unused ones hold other characters' bones). Matches **~1 unit = 1 cm** (Sora ~1.55-1.6 m, Donald ~0.9 m).
- New live settings: `ipd_m` (0.063), `units_per_m` (100), `world_scale` (1.0); the per-eye half offset is
  ipd/2 x units/m x world scale (3.15 units by default); `d3d_eye_offset` still overrides when non-zero.
  The earlier stereo tests used 30 units per eye, ~10x true scale.
- **True-scale check** (`runs/2026-11-10-scale1`, no debug layer): half offset 3.15; shifts vs centre:
  Goofy -10.0 / +9.3 px, Sora -7.9 / +6.7, far -2.1 / +2.2 (4K frame): a tenth of the offset-30 run, as
  expected (linear). **The game held 60 ticks/s with both eye replays and no debug layer** (frame interval
  p99 18.4 ms); GPU cost not measured separately yet.
- Head motion: 1 m of head movement = ~100 units of camera movement at world scale 1.

### 2026-11-10 Claude: field of view widened through the per-pass projection; no culling gaps seen

Run `runs/2026-11-10-fov1` (unattended, Back Alley spot, no debug layer).
- Plan for headset projection: OpenXR takes each eye's image with the FOV it was rendered with, so both eyes can
  share **one symmetric projection** wide enough for the headset's per-eye view (each eye cropped/warped by the
  runtime with its own pose), instead of per-eye asymmetric projections. The scene target is 16:9, so the
  vertical FOV is the constraint: 90 deg vertical = ys 1.0 -> xs 0.5625 (~121 deg horizontal).
- Probe: `poke2_offset` / `poke2_add`: a second target in the same execute-breakpoint poke.
- Test: at fn exe+0x125d30 entry, projection (rdx) x scale 0.805 -> 0.5625 and y scale 1.431 -> 1.0
  (`poke_offset=0 poke_add=0.5625 poke2_offset=20 poke2_add=1.0 poke_set=1`): the whole 3D scene zooms out about
  the centre (HUD unaffected); preview `runs/2026-11-10-fov1/fov.preview.png`. Shift measurements are only
  indicative (phase correlation isn't scale-aware: left buildings +235 px, centre -1 px, right +84 px).
- **No culling gaps at this spot**: the alley's near left wall and upper buildings, outside the original view,
  appear complete in the wide view. The game's CPU culling projection (exe+0xac18c4) wasn't changed; gaps could
  still show as popping during motion: check during sweeps and combat. Both eyes replay the same r0 blocks, so
  they get the same FOV.
- Pixel density: 2560x1440 over ~121 x 90 deg is ~21 px/deg, below the headset's; the game's render
  resolution setting would need raising later.
- Next: real-world scale (game units per metre, for IPD and head motion), post-processing in the eye images,
  performance without the debug layer, then an OpenXR output path.

### 2026-11-10 Claude: both eyes rendered per tick; stereo pair VERDICT PASS

Run `runs/2026-11-10-eyes1` (unattended, Back Alley spot, debug layer on, Frame Rate Unlocked setting).
- Two eye targets (left/right clones of the scene target), two replays per tick (each with its own
  pre-barriers and start state), per-eye camera copies (4 ring regions: main/twin x left/right), two twins per
  game bundle. `d3d_eye_offset` is now the half separation (left -, right +); `d3d_eye_show` 0 none / 1 left /
  2 right copies that eye over the game's scene target for checking. The game's own pass stays the centre view.
- Offset 30 (separation 60 game units), shift against the centre view (4K frame):
  Goofy -91.1 / +91.1 px, Sora -72.6 / +69.9, left buildings -35.0 / +34.9, far -21.1 / +21.2: **symmetric
  and depth-ordered**, 0 new debug errors, 0 camera-copy misses, 0 twin failures.
- **`frame_check.py left.png right.png --reject center.png`: VERDICT PASS (8 checks)**: both eyes non-blank and
  unlike the centre view (similarity 0.70 < 0.85), different images, correlated (0.49), no vertical offset
  (dy 0.000), multiple depths (7 of 12 tiles at another disparity). Snapshots in the run folder (git-ignored).
- Cost: ~36 fps with the debug layer and two eye replays (48 with the layer alone); not yet measured without
  the layer.
- Not done: per-eye projection (headset FOV, asymmetric/cropped), offset units vs real IPD, post-processing and
  effects on the eye images (the replay covers the scene segment only: bloom/post and HUD are not in the eye
  images), performance without the debug layer, the in-between frames at Unlocked, OpenXR output.

### 2026-11-10 Claude + human: stage C: a right-eye camera for world, companions, Sora and shadows

Runs `runs/2026-11-10-eyeC2..eyeC6` (unattended except the human watching the monitor; debug layer on).
- **Flow retry:** a missed title press (~48 fps with the debug layer) left the game on the title while my
  snapshots were refused because another window covered the game; the human noticed. `kh2_flow.py` now replays
  the previous step's presses (`"retry": {"after": 8, "times": 2}`) on the load-menu steps, only while the
  previous screen is still showing.
- **C1 (`d3d_inject=2`, `d3d_eye_offset`):** `d3d_capture` also hooks CreateCommittedResource /
  CreatePlacedResource (device slots 27/29) to map GPU addresses to buffers. In the eye replay each main-list
  r2 camera block is read through Map, copied into our upload ring (3 frames x 256 blocks) with view-space
  x += offset, and bound instead. Offset 0: identical (0.00 px). Offset 60 (4K frame): Donald +111, buildings
  +69.5, far +43, **Sora -0.4 (unmoved: his per-frame bundles bind their own r2)**; 386 copies, 0 missed,
  0 new debug errors.
- **C2 (`d3d_inject=3`): twin bundles.** Every bundle the game records on the main thread is mirrored op for op
  into a bundle of ours (pool 4 tick slots x 160, own allocators) whose r2 binding points at an eye copy (its
  own ring half, cycled per tick); the eye replay executes the twin. With offset 60: **Sora +138.8 px**,
  buildings +69, far +43, offset 0 identical, 0 debug errors. First pool (32/tick) overflowed (with the debug
  layer the game records ~30-70 bundles per tick): 160/tick, 0 failures.
- **Shadows came apart from the characters (seen by the human on the monitor during the C2 run).** Reproduced
  with aligned zoom crops (`runs/2026-11-10-eyeC5`: shadow blob left of Sora's feet). A first theory (combined
  world-view-projection matrices in r0 of the shadow receivers) was wrong: editing them moved the shadows by
  -14/+7 px, not linearly; that edit is now off (`d3d_eye_wvp_scale=0`). **Cause: my twins gave the eye offset
  to every r2, including the light camera of the shadow-map pass.** Fix: the replay uses a twin only while the
  eye's scene target is bound; shadow-target bundles keep their originals. **Verified** (`-eyeC6`, aligned
  crops: shadow back under Sora's feet as in the baseline; the earlier build's crop shows it detached), 0 new
  debug errors, 0 twin failures.
- State: one eye (the "right" one, shown on the monitor via `d3d_eye_show`) renders the full 3D scene with a
  camera offset, at the game's tick. Not done: the second eye/left offset, per-eye projections (asymmetric /
  wider FOV), the offset's units vs metres (an offset of 60 game units moved the world as the per-pass test's
  ~40), effects and post on the eye image, performance without the debug layer, Unlocked, VR output.

### 2026-11-10 Claude: per-eye replay at the D3D12 level: stages A and B pass

Runs `runs/2026-11-10-inj1..inj4`, `-eyeB1` (unattended, Back Alley spot, debug layer on).
- **D3D12 debug layer** (`d3d_debug=1`, start only): the game's `D3D12CreateDevice` import is redirected to
  enable the layer before its device exists. Errors (not warnings: the game alone emits ~7,000/s clear-value
  warnings, ids 820/821) go to the main log and, synchronously via `ID3D12InfoQueue1::RegisterMessageCallback`,
  to `<log_dir>\d3d_debug.log` (own lock; written before any hang). Game baseline: 7 errors, all id 1422
  (render targets used before their first clear). Frame rate ~48/s with the layer.
- **Replay engine** (`d3d_inject`): main-thread direct lists are tracked from Reset; the scene segment starts at
  the first render-target bind with depth; all ops are recorded by value; at the first clear of a render target
  not used in the segment (the post stage, after >= `d3d_inject_min_bundles` bundles) the start state is
  re-applied and the segment replayed through the original methods, then the game continues.
- **Three GPU timeouts on the way** (19:17 breakpoints in the draw path, 19:20 game-code replay, 20:53 first
  stage A): the last one was my bug: the replay ran while my hook held its lock; the debug layer calls back
  through the same vtable, so a nested hook deadlocked (stack overflow in clr.dll, then the GPU timed out).
  Fixed: a recursive lock and a per-thread guard covering each whole hook (nested calls only forward).
- **Stage A (`d3d_inject=1`, replay into the same targets), PASS:** 265 replays (~1/tick), ~114 ops each,
  **0 new debug errors**, picture identical (0.00 px, mean difference 0.86 grey levels = idle animation),
  game fine after. Needed **pre-barriers**: the segment isn't balanced (shadow target starts RENDER_TARGET,
  ends PIXEL_SHADER_RESOURCE; a mesh vertex buffer is updated with COPY_DEST barriers): before replaying, every
  resource the segment transitions is put back into its starting state (276-283 pre-barriers per ~260 replays).
- **Stage B (`d3d_inject=2 d3d_capture=1 d3d_eye_show=1`), PASS:** `d3d_capture` hooks the device's
  CreateRenderTargetView/CreateDepthStencilView (slots 20/21) from device creation, mapping view handles to
  resources. The eye target is cloned from the real scene target: **2560x1440 B8G8R8A8 (87), 1 sample; depth
  D32_FLOAT_S8X24 (20)** (the scene renders at 2560x1440 and is scaled to the 4K window later). Replay with the
  scene colour/depth substituted (binds, clears; their barriers dropped), then copied over the game's scene
  colour: 258 replays, **0 new debug errors**, picture identical (0.00 px, 0.89).
- Next (stage C): right-eye camera. Hook CreateCommittedResource/CreatePlacedResource (device slots 27/29) to
  map GPU addresses to buffers; at replay, read each main-list r2 camera block via Map, write an eye-offset
  copy into our own upload ring and bind it; then twin bundles for the per-frame bundles that set their own r2
  (Sora). Judge with the parallax regions (expect world/Donald/Sora shifts like the per-pass eye test).

### 2026-11-10 Claude: D3D12 command-list observer: the scene list's live structure

Runs `runs/2026-11-10-obs1..obs3` (unattended; observer only, nothing modified).
- New probe feature **`d3d_observe=1`** (read-only): at the first Present it creates a throwaway allocator,
  direct list and bundle list on the game's device to find the vtables (direct and bundle tables are
  **separate**; both patched), then hooks Close, Reset, DrawInstanced, DrawIndexedInstanced, ExecuteBundle,
  SetGraphicsRootConstantBufferView, OMSetRenderTargets and ClearRenderTargetView (counting and
  forwarding only). `D3D` lines per STATS window by thread and list type; `cmd=d3d_dump` logs the op
  sequence of 3 bundle-executing direct lists per thread. No crashes or GPU problems with it on.
- **Display change:** the game window is now **3840x2160** borderless on a 4K primary monitor (Windows
  reports 2560x1440 at 150%); earlier runs captured 2560x1440. Screen references (fractions) still match;
  the load-list pointer finder now scales frames to a 1440-pixel height and searches +-40 px sideways (the
  pointer bobs): offline row 3 score 0.98 (4K), row 4 0.99 (old frame). Flows pass again (53-55 s).
- Pitfall: `back_to_title.pad` pressed B twice; the second B selected the title's BACK, which **quits the game**
  when it's started directly. Fixed to one B.
- **Scene list (main thread, one per tick, three dumps identical in shape):** RT 8980 (MSAA scene target +
  depth) -> full-screen draw, clear -> bundles with **r2 = the per-frame camera block** (ring offsets 0x300 ->
  0x600300 -> 0x400300, triple-buffered) -> four **shadow segments** (RT 8a60 + depth: 2 bundles; RT 8a60 no
  depth: 1 bundle (blur); back to RT 8980: receiver bundle) -> clears -> RT 8b00: 1 bundle (post/HUD stage).
  27 bundles per list.
- **Bundles are mostly reused across frames:** ~8 bundles recorded per tick (~6 draws each) vs 74 executed
  per frame; most executes are of bundles recorded earlier (not in the observer's table). The bundles
  re-recorded each tick are the ones that **set their own r2** (character bundles, Sora's included).
- Another thread also records direct lists with bundles (~12 lists/tick, ~6 bundles each, 5.8 RT switches);
  not dumped yet (its lists seem to be reset before the dump was armed).
- Design (RESEARCH.md §4, draft) made concrete: right-eye pass = replay this list's scene segment after it
  ends, with RT 8980 swapped for a right-eye target, the main r2 swapped for a right-eye copy of the camera
  block, twin bundles (r2 -> identity view + eye offset) for the per-frame character bundles, and the
  shadow segments re-run as is. Next: dump the other thread's lists; then build the replay with our own
  targets in a guarded A/B mode (watchdog: turn off on any device-removed / long fence wait).

### 2026-11-10 Claude: scene draw mapped statically; a second scene draw per tick hangs the GPU

- Static reading (local disassembly in the scratchpad, nothing kept):
  - fn exe+0x19ce80 returns a global (exe+0xac1838); fn exe+0x126b10 stores it into a render-context field
    (context from fn exe+0x11c150, +0xE530). Setup only.
  - **The scene draw consumes a double-buffered display list** (PS2 style): game code writes draw commands
    during the tick through a write pointer (exe+0xac79a0) and size (exe+0xac7998); fn exe+0x1ae4a0 picks the
    current list with the flag at exe+0xac7970 (pointers exe+0xac7980 / exe+0xac7988) and draws it via fn
    exe+0x1aded0 (D3D build: per-pass setup, uploads, command lists). Afterwards **fn exe+0x1aeee0 resets the
    list header, zeroes the size, flips the flag and points the writer at the other list**.
- **Contained attempt (run `runs/2026-11-10-sd1`): replaying fn exe+0x1ae4a0 once per tick** (RIP from its
  return exe+0x1a047e back to the call at exe+0x1a0479, inside the draw task, before the flip), right-eye
  offset only in the replay: **GPU timeout (LiveKernelEvent 1a8/1b8) within ~4 s, then APPCRASH (clr.dll)**.
  Windows recovered; `stop` cleaned up. So the D3D side of the draw (command lists, bundle allocators, upload
  ring, fences) doesn't tolerate a second build per tick as is.
- Two GPU timeouts today (19:17 logging breakpoints in the draw path, 19:20 this replay). **No further
  live replays of draw-path functions** without a design for the D3D side: a per-eye render has to be built
  at the D3D12 level (e.g. record/execute the frame's scene command lists twice with a per-eye camera and
  projection constant and a per-eye render target), not by re-running the game's draw code.
- Both stereo plans now meet the same core requirement: a D3D12-level per-eye scene pass. Rank 2 would do it
  once per tick; rank 1 per in-between frame plus per-tick for CPU-transformed objects.

### 2026-11-10 Claude: scene-draw structure mapped; one GPU timeout from breakpoints in the draw path

Run `runs/2026-11-10-bp1` (unattended, Back Alley spot). Probe stack capture now keeps up to 20 code-only
return addresses (unwind entry required), scanning 2048 stack slots.
- **Main loop = task dispatcher** fn exe+0x14fdaa (call returns to exe+0x14fdfe for every task). Per tick:
  - **draw task** fn exe+0x1634c0 -> fn exe+0x155c41 -> fn exe+0x1a0460 -> **fn exe+0x1ae4a0 (scene draw,
    no arguments)** -> fn exe+0x1aded0/0x1adee0, which contains both the build (per-pass setup chain to fn
    exe+0x125d30, return exe+0x1adfb0) and the per-draw uploads (chain to uploader fn exe+0x111bd8, return
    exe+0x1adfe4);
  - **render task** fn exe+0x1548e9 -> fn exe+0x105e10 -> fn exe+0x128450 -> fn exe+0x107d00 -> fn exe+0x128470
    -> submit fn exe+0x128260.
- From the call sites (local disassembly, nothing kept): fn exe+0x1ae4a0 reads a byte flag at exe+0xac7970 that
  picks one of two pointers (exe+0xac7980 / exe+0xac7988: double-buffered draw lists?) and calls fn exe+0x1aded0.
  Its caller fn exe+0x1a0460(cl) runs fn exe+0x19ce80 and fn exe+0x126b10 first and fn exe+0x1aeee0 after; cl
  chooses fn exe+0x1ae4a0 or fn exe+0x1ade10.
- **Incident:** arming execute breakpoints with argument logging on fn exe+0x1634c0, exe+0x1ae4a0 and exe+0x1adee0
  at once caused a **GPU timeout (LiveKernelEvent 1a8/1b8) and an APPCRASH in ntdll** seconds later (19:17).
  Likely a CPU stall in the draw path while the GPU waited on it. Windows recovered; `stop` removed the probe.
  Rule from now on: no logging execute breakpoints on hot draw-path functions; one slot at a time,
  `exec_log=0`, and prefer static reading of call sites.
- Not attempted: replaying the scene draw. It builds into the frame's command lists and upload ring; the
  wrapper calls before/after it likely reset/close them, so a replay after the submit would risk a GPU hang.
- Next: map fn exe+0x19ce80, exe+0x126b10 and exe+0x1aeee0 statically (what they reset/close: command
  allocators, ring heads, the buffer flag), then decide where a second, per-eye scene draw + submit can run
  safely (likely inside the draw task, between the build and the flag/close step, with its own render target).

### 2026-11-10 Claude: double render: re-submitting is cheap but identical; per-eye needs the build phase

Runs `runs/2026-11-10-dr1`, `-dr2` (unattended, Back Alley spot, where the game renders on the main thread).
- New probe feature: **replay a call once per tick** (`replay_start_slot`, `replay_end_slot`, `replay_rip`,
  `replay_n`): an execute breakpoint at the call's return address sets RIP back to its argument setup, so the
  game repeats the call in its own context; every call is timed (QPC), logged as `REPLAY` with `watch_report`.
  `poke_when` (0 always / 1 only inside a replay / 2 only outside) limits execute-breakpoint pokes.
- Tick render call: fn exe+0x128470 calls fn exe+0x128260(render object, 0) at exe+0x128608 (arguments set
  at exe+0x128603 from rbx; return exe+0x12860d).
- **Replaying it once per tick:** first call 0.36-0.37 ms CPU (max 0.65), replay 0.35-0.37 ms (max 0.72);
  presents 60 -> 120/s (all main thread), ticks stay 60/s; desktop capture still 60-63 changed updates/s
  (59-60 baseline): the second submit shows the same picture and advances nothing visible. Baseline restored
  after (60/s, 0.37 ms).
- **Right-eye offset only inside the replay: no effect** (POKES done=0; 80 captured frames all at shift 0 in
  every region). fn exe+0x125d30 (per-pass camera setup) is **not called inside fn exe+0x128260**: the tick
  render only re-submits command lists and constants built earlier in the tick. That's why it's cheap and
  identical.
- Verdict: a real per-eye render needs the **build phase** (the per-pass setup chain: fn exe+0x125d30 <- fn
  exe+0x1a0d10 <- fn exe+0x4724e0 <- fn exe+0x126540 <- fn exe+0x474cc0 <- fn exe+0x1ae7bc, from earlier
  stacks) re-run per eye with that eye's edits, then the submit. Next: locate the build phase's entry per tick,
  replay it once with the eye edit, and check that it doesn't advance game state (animation, particles, timers):
  compare captures and watch a few state counters.

### 2026-11-10 Claude: in-between frames move only the world camera, not Sora (prototype input)

Human decision: choose rank 1 vs rank 2 after a two-eye prototype (RESEARCH.md §4). Run `runs/2026-11-10-pe1`,
unattended, Frame Rate Unlocked, interpolation forced on (switch field poke) for all tests below.
- Write watches during a sweep with forced interpolation (render loop hit 8,509 / 18,680 times): Sora's
  model-view in the GPU upload ring, the r0 projection block and the CPU staging copy of the shared camera
  block are written **only by the main thread** (128-143 times, once per tick per ring slot). In-between
  frames rewrite none of these.
- At rest the render thread barely touches the camera (2 render-thread M2 writes in ~2 s): in-between frames
  at rest redraw the same constants.
- **Decisive test:** camera yaw 3 deg applied only on the render thread's M2 writes (`mat_threads=1`, 1,141
  edits) during a sweep: world region **FLICKER** (90 back-and-forth jumps >= 30 px: in-between frames use
  the edited world camera), **Sora's region CONSISTENT (0 jumps)**; control sweep without the edit: both
  consistent. So **in-between frames update only the world camera; Sora (CPU-built model-view) keeps the
  tick's camera** (other CPU-transformed objects probably too).
- Meaning: rank 1 can't simply put per-eye/head-pose edits into the game's in-between frames: Sora would
  swim against the world with head motion and get no per-eye offset. Rank 1 needs the mod to re-run the
  per-pass setup (fn exe+0x125d30 path, which does reach Sora) per eye and render, or to re-derive Sora's
  model-view per in-between frame. Rank 2 (both eyes per tick through fn exe+0x125d30, re-presented at 90)
  is unaffected. Next: measure the cost of drawing the main-thread render twice per tick (the rank 2 core,
  and the part rank 1 would need anyway).

### 2026-11-10 Claude + human: combat save, closed-loop file selection, forced in-between frames in combat

- **Human made save file 3** (Bamboo Grove save point, Land of Dragons, LV 1) and demoed loading it and
  walking to the enemies once (recorded with `pad_record=1`, run `runs/2026-11-10-combatsave2`).
- **The load list's cursor starts on the most recently used file**, so fixed press counts break whenever
  another file was used last (the demo started on 03; the Back Alley flow had assumed 02). `kh2_flow.py` now
  selects files closed-loop (`"slot": N`): it finds the pointer hand with a template (runs/refs/screens/
  load_hand.png, git-ignored; rows 208 px apart from y 368), presses UP/DOWN, re-checks, then A. Offline: row
  4 found with score 0.99, a frame without the hand scores 0.49 (refused). Live: slot 3 (score 0.76) and
  slot 2 (`load_spot` passed in 51 s with 3 used last).
- **`kh2_flow.py load_combat <run>`**: title -> file 3 -> Bamboo Grove -> recorded walk
  (`tests/pad/combat_walk1_grove_to_exit.pad`) -> encampment (`combat_spot`, minimap reference, min 0.75):
  **48 s from a fresh launch** (`runs/2026-11-10-cmb4`). The Heartless close in by themselves, so no second
  walk (`combat_walk2_camp_to_enemies.pad` kept, unused). The area change doesn't fade fully to black, so the
  `__transition` wait missed it. Screen-check minimums are now per step (`"min"`; `kh2run.py state ... --min`).
- **Combat, forced vs natural** (`tests/live/forced_interp_actions.py --only combat_mix`, attacks + camera
  sweeps + a jump, twice each; `runs/2026-11-10-cmb3`): **in combat the game interpolates by itself**
  (natural 408-420 extra/s); forced 389-418/s. 0 HUD blips in all four bursts; world CONSISTENT in 3, one
  natural burst INCONCLUSIVE (low-confidence shifts: effects / fast camera). Frames show a real fight
  (Heartless, hit flashes, Sora attacking; Goofy's portrait red).
- Verdict: forcing in-between frames on showed no visible fault in combat, HUD menus, the reaction command,
  a Drive and attacks. The game turns interpolation off at the Back Alley spot (active "Examine"
  prompt) but not in this fight. Still not covered: cutscenes, loading transitions, and a frame-level check
  of Sora's/enemies' animation in in-between frames (blips can't be counted in moving regions).

### 2026-11-10 Claude: forced in-between frames during HUD, reaction command and attacks

Frame Rate = Unlocked, unattended, run `runs/2026-11-10-act1`, scenario `tests/live/forced_interp_actions.py`.
- New checker `scripts/blip_count.py`: one-frame A-B-A dropouts per region. Proven on synthetic bursts
  (a HUD panel missing in one frame: FAIL, blip at frame 15; clean: PASS). Limit, also proven: it can't
  see blips in regions that move every frame (neighbours differ too), so moving content is judged with
  `burst_shifts.py` instead.
- Each action played natural (the game kept interpolation off: 0 extra/s) then forced (switch field set to 1
  at the render loop's read: 298-370 extra/s; one 1 s window at 1/s during the Examine message). 300-frame
  bursts at desktop rate:
  - command menu (`tests/pad/command_menu.pad`), reaction command (`reaction_examine.pad`), attack combo
    (`attack_combo.pad`): **forced: 0 HUD blips (command area, right-hand HUD), world CONSISTENT (no
    back-and-forth jumps)** in all three; natural the same.
  - Outcomes checked on frames: the Examine message ("Station Heights / Sandlot" sign) appeared; attacks
    swung; the command-menu script did **not** do what it meant: the menu cursor wasn't on Attack, and the
    presses ended in a **Drive into Valor Form** (Goofy merged; nothing saved). So the forced frames also
    covered a form change, its HUD changes and Valor attacks: no blips, no jumps.
- Verdict: no visible fault from forcing the game's in-between frames in HUD animation, reaction
  commands, menus, a Drive and attacks (no enemies at the spot). **Not covered:** fights with enemies (hit
  effects, enemy animation, damage numbers), cutscenes, loading transitions; Sora's own animation is only
  checked by eye on a few frames (blips can't be counted in moving regions).
- Pitfall: menu scripts are open loop; the command menu remembers its cursor, so a script must check the
  selection (screen state) before pressing A. `command_menu.pad` should not be used as is.
- Next for combat: a save near enemies (or the human names a nearby spot with Heartless), then the same
  scenario with attacks on enemies.

### 2026-11-10 Claude: what switches the in-between frames on and off (found, and controllable)

Frame Rate = Unlocked; unattended runs `runs/2026-11-10-xf1..xf4`. Human hint: the mouse camera moves faster
(not tested: the trigger turned out not to be input-related).
- Condition matrix (`tests/live/extra_frames_matrix.py`): with extra frames running they ran in **every**
  phase (idle, sweep, walk, Desktop Duplication, breakpoints armed, game foreground or background, 360-455/s).
  So motion, input, capture and focus are not the trigger; the on/off switch is stateful.
- The render thread's loop (fn exe+0x1287d0, read locally from a disassembly in the scratchpad, nothing kept)
  is paced by a multimedia timer (timeSetEvent + WaitForSingleObject + QPC). It renders an interpolated frame
  (fn exe+0x120ED0, alpha in +0x354) only when **field +0x59C8 of the render object exe+0x8a0970
  (= exe+0x8a6338) == 1** (and +0x59CC == 1, a mode check on +0x2C, a timing check); otherwise the main
  thread renders the tick frame (fn exe+0x128260 via fn exe+0x128470) and nothing in between.
- Probe field monitor (`mon`, `mon_base`, `mon_ms`) on that object: **+0x59C8 = 4096 (0x1000) exactly
  while there are no extra frames, 1 while there are**; +0x354 drops from 1 to 0.03-0.06 at the same time.
- Writers of +0x59C8: the per-pass setup fn exe+0x125d30 stores each pass's flags argument (rcx) there
  (~50x/tick, always 0x1000 in the samples); once per tick fn exe+0x119f78 (via fn exe+0x128100) and
  fn exe+0x116100 write it too. Off state: fn exe+0x4aeb30 issues ~32 screen-space passes (identity
  projection and view, exe+0x7134f0: HUD) per tick from call site exe+0x4aeb92 at the end of the tick, so the
  field ends at 0x1000. On state: that site is silent; 4 passes come from exe+0x4aec1f mid-tick, the field
  ends at 1. The visible difference at the test spot: the "Examine" reaction command is **active (bright)
  when off, greyed when on**. Hypothesis (strong, not proven): an active reaction-command / HUD element drawn
  as many passes disables the game's interpolation.
- **Control, VERDICT PASS:** an execute breakpoint at the render loop's read (exe+0x128af6) that sets the field
  to 1 (`poke_abs=exe+0x8a6338 poke_int=1 poke_add=1`) turned extra frames on in the "off" scene: 0 -> 318-363/s,
  back to 0 when removed. Forced frames at rest: world identical, HUD step differences no larger than natural
  (mean 0.04-0.06 vs 0.10-0.19). Forced during a camera sweep: 330-355 extra/s, no back-and-forth jumps, HUD
  steady (no step > 3 grey levels), motion in fine steps (0.2 px/captured frame vs 4.1 px at 60 Hz; similar
  total motion): real in-between camera positions.
- New probe features: `find_floats`; field monitor; execute-breakpoint pokes at an absolute address
  (`poke_abs`) and as integers (`poke_int`). Flow fix: `tests/pad/title_to_load.pad` now uses the D-pad after
  an 800 ms settle (a 120 ms stick flick was missed once and A opened NEW GAME; nothing saved);
  `tests/pad/back_to_title.pad` backs out of NEW GAME.
- Meaning for VR: the game's in-between renders can be forced on, so the 90 Hz plan (60 Hz simulation,
  in-between renders paced to the headset) doesn't depend on what's on screen. Before relying on it: check
  forced frames during a HUD animation and a reaction command use (why the game turns it off), and in combat.

### 2026-11-10 Claude + human: stereo edits at Frame Rate = Unlocked (render-thread frames)

Human switched Frame Rate to Unlocked (persists across launches); run `runs/2026-11-10-unl1`, unattended
otherwise. Scripts: `tests/live/unlocked_flicker.py`, `tests/pad/orbit_right_long.pad` (now a left/right sweep).
- Present rate confirms Unlocked: 430-480/s during the walk, main thread 60/s. The scripted load + walk works
  unchanged at Unlocked (45 s).
- **fn exe+0x125d30 runs only on the main thread**: execute-breakpoint hit records during an orbit with
  158 presents/s showed 11,736 main-thread calls and none from any other thread.
- **At rest with ~390 extra render-thread frames/s, every frame keeps both edits**: absolute shift vs an
  unedited reference burst, 60 frames each: reference +0 (60/60), projection row2.x=0.25 +320 px (60/60),
  eye offset +60 units +110 px (60/60). So extra frames reuse the main thread's per-pass constants.
- **While the camera moves: not verified.** The extra frames couldn't be produced on demand: during scripted
  sweeps the game ran at 60/s main-thread only (all three bursts CONSISTENT but with no extra frames, so they
  prove nothing), while at rest right after a burst it ran at ~450/s. What switches the render thread's
  extra frames on is still unknown (see the earlier open issue; Desktop Duplication raises the rate).
  Earlier evidence (2026-11-10, M2 edits) says the render thread rewrites M2 with an interpolated view per
  extra frame (fn exe+0x120f35) while moving, and render-thread-only M2 edits flickered. So the **eye offset
  will probably also need applying on that render-thread path**; the projection is probably reused (not tested
  while moving).
- Measurement pitfalls: a one-way 6 s orbit pinned the camera against the alley wall, so later orbits didn't
  move it (median 0.1 px/frame); a 150 px jump threshold can't see a ~110 px eye-offset flicker (use 60).
- Next: find what triggers the extra frames (try ptrack-free runs, the game's own fps counter, or the
  render-thread loop fn exe+0x1287d0's wait condition), then rerun the sweep with extra frames present; or,
  for the stereo prototype, run at Frame Rate 60 first (no render-thread frames) and add Unlocked later.

### 2026-11-10 Claude: per-pass stereo injection point found (projection + eye offset, all draw paths)

Unattended, runs `runs/2026-11-10-proj3`, `-proj4`.
- New probe command `find_floats` (floats=a,b,*,..., float_tol, mv_scan_wc): the r0 block's CPU scratch is at
  exe+0x8a6350 / exe+0x8a63e0 (1/1280, 1/720, 1280, 720, 1792, 1856, ..., (1.992,1.992,1,1) at +64); its
  projection slot (+80) is filled per pass by **fn exe+0x125d30** (~50 writes/tick), the same function that
  writes M2.
- Execute breakpoint at fn exe+0x125d30 entry: **rdx = the GPU projection** (0.805, 1.431, row 3 z -4: a stack
  local of the caller fn exe+0x1a0d10), **r8 = the view** (the setter view for world passes; an identity
  view for the passes whose draws carry a CPU-built model-view, e.g. Sora's). Once per render pass.
- New probe edit: register-relative poke at execute breakpoints (`poke_reg` 0..3 = rcx/rdx/r8/r9,
  `poke_offset`, `poke_add`, `poke_set=1` to write instead of add).
- **Projection off-axis term, VERDICT PASS** (`tests/live/projection_shift.py`, default mode exec): setting
  rdx->row2.x = 0.1 shifted Sora, Donald, buildings and far background by **128.0 / 127.9 / 128.0 / 128.0 px**
  (predicted 128), all back to rest after (7632 pokes).
- **Eye offset (view translation x += 20 at r8+48):** Sora +46.4 px (setter sideways test: 46.2), Donald +62.9
  (nearer), buildings +23.6, far +14.1: depth-ordered parallax on every draw path including Sora's (his
  identity-view passes get the same camera-space offset after his model-view), all back to 0 after.
- **Verdict: fn exe+0x125d30 is a single per-pass point where both stereo controls (eye offset in camera space,
  asymmetric per-eye projection) apply to the world, companions and Sora consistently.** The setter
  eye/target route stays valid for head rotation/position. Still to do before stereo: render each frame twice
  (left/right eye) with these edits, check shadows/sky/effects, and check the Unlocked render-thread frames
  use the same path (only tested at Frame Rate 60).
- The earlier CPU projection tests (entry below) stay valid: exe+0xac18c4 / exe+0xac1840 aren't the GPU's.

### 2026-11-10 Claude: projection edit tests: the CPU matrices don't feed the picture

Unattended, runs `runs/2026-11-10-proj1`, `-proj2`; scenario `tests/live/projection_shift.py`.
- New probe edit: **poke on write** (`poke_slot`, `poke_offset`, `poke_add`): adds a value to a float right
  after the game writes a watched address (here the matrix's last float), i.e. after the per-tick rebuild
  and before its readers. `POKES done/refused` is logged with `watch_report`.
- The projection at exe+0xac18c4 is rebuilt every tick (setter path fn exe+0x19c820 / exe+0x142ec0), so a
  once-per-tick `freeze` is overwritten: no effect. With the poke, the dump shows the edit held (r2.x 0 ->
  0.2: the last float is written twice per tick), yet **no region moved** (Sora, Donald, buildings, far all
  0.0 +- 0.3 px, prediction 128 px). Same for the PS2-style screen matrix exe+0xac1840 (offset term), and a
  **+10% scale on either matrix also changed nothing** (a used scale would zoom the left buildings ~77 px).
  Verdict: **neither CPU matrix is the projection the GPU uses**; they feed culling / VP helpers (fn exe+
  0x13f850, 0x1a94c0) and fn exe+0x17070b. The GPU's r0 projection (row 3 z = -4, unlike exe+0xac18c4's -2)
  comes from another source.
- Uploader: execute breakpoint on the memcpy call (exe+0x111de9): 60/60 copies came from one per-draw
  scratch block exe+0x8a6518 (0x1c0 bytes): the r1 per-object blocks. r0 and the camera block (r2) are
  uploaded from other call sites, not yet found.
- Next: a RenderDoc capture with a poke active (does r0+80 change?) or execute breakpoints on the other
  uploader call sites to find r0's source; search for a FOV scalar (1/xs = 1.2422, 102.3 / 69.9 deg in
  degrees or radians) near the camera record, as the ultrawide/FOV mods suggest one exists.

### 2026-11-10 Claude: Sora's model-view in upload memory; setter internals; CPU projection found

All unattended (`kh2_flow.py load_spot`), runs `runs/2026-11-10-mv1..3`.
- New probe command `find_mv` (cmd=find_mv, addr=<view>, mv_scan_wc=1): finds matrices M = W x V with W a
  yaw + y-flip world matrix. With write-combined memory included it finds **Sora's model-view only in the GPU
  upload ring** (heap, WC/mapped): mv_t (0, 166.7, 461.0), same as the RenderDoc capture; implied world
  position (-259, 0, -63), yaw 82.6. Private-memory hits are bone-level (world y -126 / -206), not Sora's
  placement. That's why every earlier CPU scan (WC excluded) missed it.
- Write watch on three ring copies: written every few ticks by a memcpy called from **fn exe+0x111bd8** (the
  per-draw constant uploader; via fn exe+0x113590, fn exe+0x112fee). The model-view is built in a temporary
  buffer and copied straight to the GPU, so no persistent CPU copy exists to edit.
- New probe feature: execute breakpoints (`watch_rw=0`), logging argument registers and the 4x4 they point at;
  `exec_arg=<addr>` filters to calls passing that address, `exec_log=<n>` caps lines (reset on each arm).
  - fn exe+0x1a94c0 (read the setter matrix ~90x/tick): plane-like inputs, called per object: visibility
    culling, **not** Sora's builder.
  - fn exe+0x13f900 is a 4x4 multiply (rcx = A, r8 = B, rdx = out), called from fn exe+0x13f850 with
    B = the setter view and A = **the applied projection at exe+0xac18c4** (xs 0.805, ys 1.431, 102.3 deg:
    the CPU source of r0's projection, open issue answered) or **a PS2-style screen matrix at exe+0xac1840**
    (scale 274.8 x 297.7, offset 2048/2048, z -4): the likely source of the 2.56x-larger camera-block matrix.
  - **Inside the setter fn exe+0x19cba0, exe+0xac1028 first holds the camera-to-world matrix** (translation =
    eye (-674.7, -251.2, -15.4)), built by fn exe+0x1404c0 from eye/target/up, and is then **inverted in place**
    by fn exe+0x13f3a0 into the view matrix. A watch on its translation w fires for both stages, which
    explains the mixed shifts of the setter-matrix edit test (one edit per stage, different meanings).
- Verdict: Sora's model-view is built from the setter's final view matrix by a per-draw path ending in the
  uploader fn exe+0x111bd8; the exact multiply is still unidentified. For VR this doesn't change the plan:
  the head pose goes in upstream, at the setter's eye/target (proven to move Sora, companions and world
  together). Per-eye projections: the CPU matrices at exe+0xac18c4 / exe+0xac1840 do NOT feed the GPU
  (tested later the same day, see the projection log entry above).
- Next: edit the projection at exe+0xac18c4 live (e.g. an off-axis shift) and check every region follows;
  then prototype a per-eye render (eye offset via the setter + projection per eye) on the test spot.

### 2026-11-10 Claude + human: unattended live tests; Sora's camera source narrowed to the setter matrix

- **Automation (human asked for it; human demoed the load and the walk once).** No human needed from launch
  to test spot any more:
  - Probe test driver (`drive=1`): redirects the game's own `XInputGetState` import (XINPUT1_4, by ordinal)
    and, with `drive_focus=1`, `GetForegroundWindow` (the game calls it ~60/s and ignores the pad when not in
    front). Plays `*.pad` scripts (`tests/pad/`), holds a neutral pad for `drive_release_ms` afterwards, then
    passes the real pad through. **Proven by outcome:** injected presses drove the title menu, LOAD, and a
    walk, with the game window unfocused. `pad_record=1` logs the physical pad at full rate (PADREC);
    `scripts/pad_from_log.py` turns that into a pad script.
  - `scripts/kh2run.py` (start/set/pad/wait/snap/state/stop/status): checks the exe SHA-256 pin, installs the
    probe, launches directly, waits for the first Present, refuses snapshots when the game window is covered,
    and `stop` closes the game and removes the probe (verified empty after every run today).
  - `scripts/screen_state.py`: names the screen from reference thumbnails in `runs/refs/screens/`
    (git-ignored): title_menu, load_menu, save_room (minimap crop), back_alley, alley_spot.
  - `scripts/kh2_flow.py load_spot <run>`: title -> LOAD -> default file (the cursor starts on the most
    recently used file: slot 2, "The Usual Spot", Twilight Town) -> save room -> recorded walk, split at
    the area transition (waits for fade + settle) -> Back Alley test spot. **Repeatable:** a second fresh
    launch reached the spot in 46 s, 3 px from the reference (`runs/2026-11-10-auto5`).
  - Pitfalls found: the title cursor starts on NEW GAME (the first unscripted taps began a new game; nothing
    was saved); a reference grabbed mid-transition (load menu) doesn't match the settled screen; the save
    point's glow keeps the room from "settling"; open-loop walks must be split at area loads.
- **Camera-copy edit test** (`tests/live/camera_copy_edit.py`, yaw 6 deg, regions measured separately):
  - M2 (control): world +190 px at the left buildings, Donald +115 px, Sora +0.8 px. These match a true 6 deg
    yaw at those screen positions (predicted ~183 / ~120 px with the 102.3 deg projection).
  - M1 (exe+0x8aed40) and the setter's copy (exe+0xac1b58), rows+t or rows only: written every tick
    (watch counts), **no effect anywhere**.
  - Setter matrix (exe+0xac1028, same layout and values as M2): the only candidate that moves Sora, but the
    shifts are mixed (rows+t: Sora +17, Donald -48, world +139; rows only: Sora -62, Donald +10,
    world -23). Not a clean yaw, so it has more than one consumer (likely the camera logic reads it back).
- **Read watch on the setter matrix** (244 ticks): fn exe+0x125d30 reads it ~70x per tick (it writes M2:
  **M2 is copied from the setter matrix**); **fn exe+0x1a94c0 reads it ~90x per tick** from per-object
  call chains (via fn exe+0x14fdaa, exe+0x3bf714): best candidate for the CPU world x view that Sora's draws
  carry. Also read once per tick by fns exe+0x13f3a0, 0x13f900, 0x1404c0, 0x142080, 0x17070b, 0x19cba0.
- Verdict so far: **Sora's camera source is the setter matrix exe+0xac1028 itself** (M1 and the copy are
  ruled out). Which reader builds his model-view (fn exe+0x1a94c0 suspected) is still **unconfirmed**.
- Next: confirm fn exe+0x1a94c0 by editing only what it reads (e.g. a copy of the matrix it is pointed at)
  or by watching where its results go; then decide the head-pose injection point (the setter's eye/target,
  proven to move everything consistently, stays the leading candidate).

### 2026-11-10 Claude: Sora's draws traced in the capture; projection confirmed

- Bundle command lists are recorded inline in the capture (Reset type=1 ... Close, then ExecuteBundle), so
  draws inside bundles are in the structured chunks: 348 draws in 74 bundles (frame 16438).
  `scripts/rdc/bundle_draws.py` dumps every draw with its root CBVs, vertex buffer statistics and classified
  matrices (`KH2VR_BUNDLES`, `KH2VR_CB_FLOATS`); `scripts/rdc/chunk_fields.py` prints chunk field names.
  Report files: `runs/2026-11-10-renderdoc2/bundle_draws.txt`, `bundle4.txt`, `bundle6.txt` (git-ignored).
- Root layout: r0 scene/per-draw constants, r1 per-object block (4x4 at +320, then model-space bones: GPU
  skinning, vertices hold bone-local positions), r2 camera block, r3 material/light block, r4/r5 tables.
- Bundle 6 = Donald and Goofy (world translations (-448,0,0.1) and (-517,0,-185); r1+320 = world matrix,
  shared r2 camera = M2 form). Bundle 4 = world geometry (124 draws) **plus Sora's 13 meshes (draws 111-123)**,
  identified by sharing bone matrices with the centred character's shadow pass (bundle 9, world (-365,0,-136)).
- **Sora's r1+320 is his model-view matrix** (rows (-1,0,0), (0,-0.982,-0.19), (0,-0.19,0.982), t (0,167,461)):
  world (from the shadow pass) x camera view reproduces it to 0.5 units. His draws set their own r2 with an
  identity view. Verdict: Sora's model-view is computed on the CPU from a camera copy; M2 isn't that copy.
  The earlier "camera-space CPU skinning" guess was wrong (skinning is on the GPU); the camera-space part was right.
- **Projection, VERDICT PASS:** r0+80 (xs 0.805, ys 1.430, 102.3 x 69.9 deg) projects Sora's, Donald's and
  Goofy's feet onto all three in the capture thumbnail (`thumb16438.png`, 2048x1152: predicted (1024,874),
  (783,1038), (1265,1040)); the r2+80 matrix (xs 2.061, 51.8 deg, 2.56x r0's, divided by the 1.992 factor)
  misses by 70-130 px and puts the feet off-screen. Matches the image-shift hfov (103-104.3 deg).
- Dead ends: replaying to each ExecuteBundle and diffing the colour target gave 0 changed pixels, and saved
  targets were black (the replay doesn't render these bundles, or the MSAA target isn't read this way).
  `scripts/rdc/bundle_pixels.py` is kept for its chunk histogram; don't rely on its pixel diff.
- Next: live test to find Sora's camera copy (edit candidate copies alone, measure Sora's region separately),
  then trace the sky/sun (bundles 2-3) and find the CPU projection source.

### 2026-11-10 Claude + human: RenderDoc captures: the scene's camera constant block

- RenderDoc 1.47 set up (see Current state). Capture 1 (`runs/2026-11-10-renderdoc/`, Frame Rate Unlocked) and
  three more at Frame Rate 60 (`runs/2026-11-10-renderdoc2/`) all have the same structure: ~115 actions, 2 plain
  draws (full-screen passes), 74-75 `ExecuteBundle` calls, 13 CopyResource, 10 ExecuteCommandLists. **The whole
  3D scene is drawn through D3D12 bundles**, so pixel history only reached a clear (bundle contents aren't listed
  as separate actions), and the CPU scans never saw the GPU-side constants.
- `scripts/rdc/bundle_roots.py`: before every bundle the main command list sets root parameter 2 to the same CBV:
  buffer named by the game **"ConstantBuffers" + 0x600300**. Contents (frame 16438): +0 a view matrix in the M2
  form (rows + translation (-252, 110, 752)); +64 (1.992, 1.992, 1, 1); +80 a row-vector perspective with
  xs 2.061, ys 3.663 (16:9, **51.8 x 30.5 deg**). This conflicts with the ~104 deg hfov derived from image shifts
  (step 4): the +64 factor or a PS2-style clip mapping must be involved; to reconcile before per-eye projections.
- Interpretation (hypothesis): Sora isn't drawn with a separate camera block (all bundles share this one); his
  vertices probably arrive already in camera space (CPU skinning with a camera copy). Supports: Sora's bone
  matrices (exe+0x8a67xx..0x8a7xxx) changed on every orbit tick in the first Sora scan. Doesn't explain yet why
  Donald and Goofy followed the M2 edit.
- Scripts: `list_actions.py`, `pixel_draws.py`, `find_cameras.py`, `bundle_roots.py`, `id_probe.py`, shared
  `camera_math.py` (VIEW, VIEWPROJ, scale-tolerant WVP classifiers; checked on synthetic matrices). Lesson:
  qrenderdoc's embedded Python may lack `__file__`; scripts open their report first and exit via `os._exit`.
- Next: identify Sora's bundle and its per-object data (descriptor tables / other root parameters inside the
  bundle), compare ConstantBuffers+0x600300 +0 with M2, and reconcile the projection with the measured shifts.

### 2026-11-10 Claude + human: hunting Sora's camera (not found yet)

- `runs/2026-11-10-sora-camera/`: per-frame ranking gave two heap matrices (a z-mirrored copy of M2 and its
  inverse), written once per tick by fn exe+0x72070 (via exe+0x77fc0, exe+0xfb040) on a worker thread. Edit
  test with the pad idle: no change anywhere (1.4-1.9% pixels differ in every pair, control included).
- `runs/2026-11-10-sora-camera2/`: new probe filter `report_min_rest`/`report_max_rest`; toggled a 15 deg yaw at
  the per-tick setter 10x at rest, so camera-derived matrices change ~20x. Found heap copies of the camera
  (one overlaps a second render-camera record that fn exe+0x19d3d0 also fills; others written by fn exe+0x72070).
  Edit tests: no visible change.
- Probe quirk fixed: keys missing from the ini kept their previous values across reloads (camera-adjust slots
  from one run stayed active in the next; harmless, since yaw was 0). Reloads now start from defaults.
- `runs/2026-11-10-m2-viewyaw/` (pad idle): M2 edit with rows only: world orbits around Sora (reproduces what
  the human saw); rows + translation (`mat_include_t=1`, watch on translation w, exe+0x8aedbc, written by
  fn exe+0x125d30 8470x): ground +272 px (true 15 deg yaw ~280 px), world and companions turn, **Sora stays put**
  and the sun glare changes. Confirms the human's observation: Sora (and the sky) use another camera.
- Game closed, probe removed, no crash events.

### 2026-11-10 Claude + human: per-frame camera pipeline mapped; render-thread edit drives extra frames

- Read/write watch on the block while orbiting (`runs/2026-11-10-interp-readblock/`, watch_rw=3). Call it
  M1 = 4x4 at exe+0x8aed40, M2 = 4x4 at exe+0x8aed80 (rows +0/+0x10/+0x20, translation +0x30).
  - Main thread per tick: fn exe+0x125d30 reads/writes M1 once and writes M2 ~16x (per pass?);
    fn exe+0x10fe21 reads M2 (~2/tick).
  - Render thread (tid 19160) per extra frame, equal counts (818 each in ~5 s): runtime copy of M1 (called
    from the render loop at exe+0x128d49), fn exe+0x13f3a0 reads M1, fn exe+0x120f35 writes M2, then
    fn exe+0x10fe21 reads M2 twice. **M1 = per-tick input, M2 = the camera the draw uses, fn exe+0x10fe21 =
    the shared consumer.**
- Re-analysed the orbit bursts from `runs/2026-11-10-interp-edit/` on the background band (the first analysis
  used a centre crop = Sora again): no edit 0 jumps; **render-thread-only 15 deg: 21 jumps >=150 px, 8
  back-and-forth flips (flicker)**; both threads: 0 jumps. So the render thread's M2 write drives the world in
  extra frames, the main thread's writes drive tick frames, and editing both is steady. At rest the render
  thread doesn't redraw the world (why the static render-only check showed nothing).
- Still open: Sora's pass uses another camera (edit M2 alone -> Sora fixed while the world turns; sinking into
  the ground when tilting, seen by the human). Candidates: the heap matrices that changed within ticks in the
  ptrack2 run (#2/#3, 167 intra-tick changes); heap addresses change per launch.
- Next: find Sora's camera path (scan + edit test with region checks), or hook upstream of both (M1 / fn
  exe+0x120f35 / the per-tick setter) so every pass derives from one edited source.
- Game closed, probe removed, no crash events.

### 2026-11-10 Claude + human: editing the per-frame camera block has no visible effect

- Probe gained `mat_slot`/`mat_rows_offset`/`mat_yaw_deg`/`mat_threads`: on a write watch, rotate 3 rotation rows
  about world y, filtered by thread (1 render, 2 main, 3 both). Watched exe+0x8aedb8 (translation z of the
  matrix at exe+0x8aed80, written after its rows). Run `runs/2026-11-10-interp-edit/`.
- Orbiting bursts (60 frames each): no edit CONSISTENT; render-thread-only 15 deg (244 edits) mostly small steps
  plus one +220/+131/-241 excursion, 10 low-confidence steps; both threads (1,518 main + 419 render edits)
  CONSISTENT. Bursts can't tell "all turned" from "nothing turned".
- **Static check (pad idle), CORRECTED:** first measured on a centre crop (mostly Sora): 0.02 px, wrongly read
  as "no effect". The human reported seeing Sora stay centred while the scene moved, and Sora sinking in and out
  of the ground when tilting the camera. Re-measured by region (same snapshots): main-thread edit moves the top
  background (~260 px), left side (~186 px) and ground (~-400 px), all low-correlation (not a clean shift);
  **Sora 0.7 px (unchanged)**; HUD side 0. Preview `AB_main15.preview.png` (git-ignored): buildings, ground,
  Donald and Goofy swung ~15 deg, Sora fixed at the centre. **So exe+0x8aed80 written on the main thread (~16x
  per tick, likely once per render pass) drives the world and companions, but Sora's pass uses a different
  camera**: editing it alone makes passes disagree (the sinking the human saw). Render-thread writes edited at
  rest: no change in any region, although that thread was presenting: its per-frame write probably comes
  after drawing (bookkeeping for the next blend).
- Side observation: Presents stayed 230-300/s with the pad idle during this run (earlier idle runs: 60/s);
  not explained.
- Lesson: measure camera edits in several regions (background, ground, player, HUD), never one centre crop.
- Next: read-watch exe+0x8aed40 / exe+0x8aed80 by thread while orbiting, to find what the render thread reads
  before each in-between frame. Option kept open: a code hook at fn exe+0x120f35 (needs a hooking library such
  as MinHook, the human's call).
- Game closed, probe removed, no crash events.

### 2026-11-10 Claude + human: in-between (per-drawn-frame) camera found

All runs at Frame Rate Unlocked with the human orbiting the camera (in-between frames only appear in motion).
- Read watchpoints (probe `watch_rw=3`, hits split main/other thread; `runs/2026-11-10-interp-readwatch/`): the
  per-tick render camera (exe+0xac1068, its copy exe+0xac1b98, controller exe+0x718c78) is read only on the
  main thread, once per tick, while Presents ran 160-340/s. Readers: fn exe+0x19cba0 (copies render camera to
  exe+0xac1b58, reads eye 4x/tick) and fn exe+0x2bd190. So in-between frames don't read the tick camera.
- Second-thread Present call chain (probe `present_probe_hook`, `runs/2026-11-10-interp-ptrack/`): extra frames
  are presented by one render thread via fn exe+0x129110 (thread entry) -> exe+0x1287d0 -> exe+0x128260 ->
  exe+0x1231e0 (sometimes via exe+0x1210c1). Tracking all 36,907 candidates per Present slowed that thread to
  ~28/s (observer effect, no data); fixed by tracking only candidates that changed while orbiting
  (`ptrack_min_orbit`, `ptrack_max`).
- `runs/2026-11-10-interp-ptrack2/`: 1,890 of 48,015 candidates tracked at ~255 second-thread Presents/s.
  **exe+0x8aed80 changed at every second-thread Present within a tick (1699/1699)**, 699 orbit ticks, 0 rest
  ticks. Block exe+0x8aed30..: position xyz1, 3 rotation rows (up row ~(0,0.999,0.044): y-up, not the tick
  camera's y-down frame), position xyz1, repeated: two camera transforms (current/previous?), not decoded.
- Write watch on it: render thread (tid 17848) fn exe+0x120f35, called from the render loop fn exe+0x1287d0,
  writes it once per in-between frame (669 in ~5 s, matching ~130 extra Presents/s under the watch); the main
  thread fn exe+0x125d30 (via exe+0x1264a0) writes it ~16x per tick while building the tick's frame.
  **fn exe+0x120f35 on the render thread is the candidate per-drawn-frame write point for the head pose.**
- Next: edit that block on the render-thread write (yaw about its up row) and burst-capture while orbiting:
  every drawn frame should turn with no flicker. Then decode the block (which transform is current vs
  previous, y-up scale vs the tick camera) for the stereo design.
- Game closed, probe removed after each run, no crash events.

### 2026-11-10 Claude + human: Unlocked retest: in-between frames carry the camera edit

- Human noted step 4 ran with Frame Rate = 60 and switched to Unlocked. Run `runs/2026-11-10-step4-unlocked/`.
- **Unlocked only adds frames while things move**: title/menus 480 Presents/s; field with the pad idle 60/s
  (one per tick, main thread); right stick held 200-440/s while ticks stay 60. Explains the 60/30 images/s
  stretches at the start of the step-3b capture (camera not yet moving). So standing-still tests at Unlocked
  behave like 60.
- Hands-off repeat (effectively 60): +5 deg 89.7 px, +10 deg 179.9 px, back 0.9 px; sideways +-20 at Sora
  +59.5/-60.3 px (different spot from the previous run's 46 px), back 0.1 px. Consistent with the earlier run.
- **Flicker test while orbiting** (new: capture burst mode `out_%03d.bmp`, `scripts/burst_shifts.py`, checked
  on synthetic steady vs every-other-frame bursts: CONSISTENT vs FLICKER): render-camera setter still runs once
  per tick (eye_hits 2/tick, adjusts ~53/s) while Presents ran 200-440/s. Burst with no edit: CONSISTENT; burst
  with a 15 deg yaw held: CONSISTENT, steps 0 to -48 px, no back-and-forth jumps (a missed edit would flip by
  ~290 px). **The in-between renders are built from the per-tick render camera we edit**: one write point per
  tick covers every drawn frame. (Bursts were 30 frames, not 60: snapshot/burst mode clash in the capture
  tool, fixed after the run.)
- The view-matrix copy exe+0x24fe8d4 was never written in this area (watch slot2: 0 hits): matrix copy
  locations vary by scene; don't rely on them.
- Byte 30 of `config1525.dat` read 3 both after step 3 (Unlocked) and today, while the human reports the game
  ran at 60 during step 4: mapping still unconfirmed.
- Game closed, probe removed, no crash events since the fix.

### 2026-11-10 Claude + human: step 4 done: sideways translation and yaw both match prediction

- Probe fix: camera edits now happen once per tick on the target write (eye hits only counted).
  `scripts/measure_shift.py` gained `--region` (fractions) to measure near and far parts separately.
- Run `runs/2026-11-10-step4-sideways/` (human hands-off, 584 adjusts, 0 refused, no crash; camera pitch ~11 deg
  down, eye-target 428 units, from a dump of exe+0xac1068):
  - Sideways +-20 units: Sora region +46.2 / -46.2 px, far top band +23.1 / -21.0 px (parallax, near ~2x far),
    back to 0: 0.9 / 0.0 px. Predicted for Sora at the look-at depth: f*d/z = 1037.5*20/428 = 48.5 px;
    measured 46.2 px: **-4.7%, PASS** (5% bound; Sora's exact depth not measured).
  - Yaw: +5 deg 89.0 px gives hfov 103.0 deg (104.3 deg in the previous run at another spot); +10 deg
    predicted 179.4 px (pitch-corrected), measured 178.0 px: **-0.8%, PASS**; back to 0: 0.00 px.
  - **Sign conventions** (probe math, up=(0,-1,0)): `+offset_right` along up x forward moves the image right,
    i.e. it is camera-LEFT; `+yaw_deg` turns the view left. The mod must use forward x up for camera-right.
- Discovery pass (steps 1-4) is complete. Next decisions for the human: agree the stereo plan (RESEARCH.md §4:
  60 Hz sim, in-between renders paced to 90 Hz, per-eye render), then design the per-eye render path
  (hook the render-camera setter via the per-build table; confirm hfov from the GPU side).
- Game closed, probe removed from the mod folder.

### 2026-11-10 Claude + human: step 4 part 2: in-frame camera write point found, yaw test passes

- Watchpoint run (`runs/2026-11-10-step4-watch/kh2vr_probe.log`, human in the field, 72 threads armed, no tick
  loss): render eye exe+0xac1068 and controller eye exe+0x718c78 are both written by a vector-copy helper at
  exe+0x1a8e69 once per tick on the main thread (different callers); the view-matrix copy exe+0x24fe8d4 is
  written by exe+0x1a94e7 4x per tick, also main thread, after the render camera.
- Second run with function lookup (exe's own .pdata via RtlLookupFunctionEntry; no disassembly):
  render-camera setter = fn exe+0x19d3d0 (writes eye then target), called from fn exe+0x165700.
- **In-frame write point**: a hardware write watchpoint on the render eye's / target's w component; the
  vectored handler edits xyz right after the game writes them and before the view matrix is built. No
  game code patched. Probe settings `cam_eye_slot`, `cam_target_slot`, `cam_watch_offset`, `yaw_deg`,
  `offset_right`, `cam_up`.
- First attempt crashed the game (exception 0x80000004 in kh2vr_probe.dll): the handler's own write overlapped
  the watched z/w bytes and re-trapped. Fixed: watch only w (+12), and refuse any adjust whose write would
  overlap an armed slot (`refused` counter). Crash run log kept as `kh2vr_probe.crash1.log`. That crash also
  left GPU LiveKernelEvents 1a8/1b8/193 and one 117 (TDR) at 08:17:09-14; none since the fix (checked after
  the sideways run). A snapshot taken
  after that crash captured the desktop; deleted. The test now checks the game is alive around each snapshot.
- **Yaw test (RESEARCH.md §6 point 3), run `runs/2026-11-10-step4-offset/`**, human hands-off, 1662 adjusts,
  0 refused, no crash. Shifts by `scripts/measure_shift.py` (phase correlation, checked on a synthetic 37 px
  shift: 37.00): back to 0 deg 0.20 px; +5 deg +87.9 px; -5 deg -86.0 px; implied hfov 104.3 deg; +10 deg
  predicted 175.2 px, measured 175.95 px (crop 0.3, peak 3.2): **+0.4%, PASS** (stable 171.7-176.2 across
  crops). Sideways 20 units: +46 px, size/sign not valid yet (open issue: eye adjusted twice).
- Game closed, probe removed from the mod folder, no crash events after the fix.
- Next: fix the double adjust, rerun the sideways test (sign of right vector, parallax), confirm hfov from the
  GPU side; then agree the stereo plan (RESEARCH.md §4) and design the per-eye render.

### 2026-11-09 Claude: step 4 part 2 (watchpoint) built, not yet run

- Probe gained hardware write watchpoints (`cmd=watch|unwatch|watch_report`, `watch0..2=<addr>`): DR0-DR2
  4-byte write breakpoints set on all threads by a helper thread; a vectored handler logs the writing
  instruction as exe+RVA, thread (main or not), hit count and exe-range stack values. Builds clean; **never run**.
- Planned run: human in the field; ini `watch0=exe+0xac1068` (render eye), `watch1=exe+0x718c78`
  (controller eye), `watch2=exe+0x24fe8d4` (view-matrix copy), `cmd=watch`; after ~10 s `cmd=watch_report`,
  then `cmd=unwatch`. Then choose the in-frame write point and redo the RESEARCH.md §6 offset test.
- Stopped here (usage limit). Game closed, probe removed from the mod folder. Nothing committed yet.

### 2026-11-09 Claude + human: step 4 (camera) part 1: camera found, tick-start writes are fought

- Probe gained camera-discovery commands via the ini (`cmd=scan|report|find_eye|freeze|unfreeze|dump`,
  `cmd_seq`, `addr`, `count`, `add`, `tol`). Scan = every 3x3 rotation (padded 16-byte rows or packed) in the
  game image's and private writable memory, 4-byte steps; then per-tick change counts while only the right
  stick is held vs idle. Field scan: 683 MB in 1.9 s, 36,729 candidates, no tick-rate loss while tracking.
- Report (`runs/2026-11-09-step4-camera/kh2vr_probe.scan1.log`): top ~14 candidates are copies of one view
  matrix (e.g. exe+0x24fe8d4, exe+0x24ff9d4, exe+0xac1028), changing in 99.7% of orbit ticks.
- Eye search from that matrix (`kh2vr_probe.log`) found two camera records, build 15194255 only:
  - **render camera** exe+0xac1028: view matrix (64 B), then eye xyz1 at +0x40 (exe+0xac1068), target xyz1 at
    +0x50, up (0,-1,0,1) at +0x60. Copy at exe+0xac1b58. **World is y-down.**
  - **controller** exe+0x718c68: target xyz1, eye xyz1, then fields incl. 420.0 (about the eye-target distance,
    measured 424) and -1.432 twice (an angle in radians?). Not yet decoded.
- Freeze test (human orbiting continuously, Frame Rate 60, `freeze_capture.csv`): freezing the render camera
  had no effect (rebuilt each frame). Freezing the controller at tick start locked eye height at 275.4 and cut
  the orbit swing from ~550 to ~85 units, with the eye flipping between two positions per tick: it drives the
  camera, but the game's camera update runs after `OnFrame` and fights the write. Capture can't separate
  camera from character motion (60 big changes/s throughout). Human's visual report pending.
- Consequence: the RESEARCH.md §6 offset/pixel-shift check needs a write point inside the frame (after the
  camera update, before the view matrix is built). Proposed next: a hardware data watchpoint (debug registers,
  set from the probe) on the render camera's eye to log which game code writes it (RVA only, no code bytes),
  then hook there through the per-build table.
- Game closed, probe removed from the mod folder, no crash events.

### 2026-11-09 Claude + human: step 3b: Unlocked's extra frames are fresh renders

- Built `frame_diff_capture` (`src/tools/frame_diff_capture.cpp`, `scripts/build_capture.cmd`): DXGI Desktop
  Duplication of the game window's monitor, from outside the game process; compares each desktop update with the
  previous one on a 1-in-4 pixel grid; CSV per update. `scripts/analyze_capture.py` buckets per second into
  identical / small change / big change (>= 20% of samples).
- Human turned the camera continuously in a field area. Run folder `runs/2026-11-09-step3b-play/`:
  - **Unlocked** (`capture_unlocked.csv`, 30 s): with sustained motion (24 s), median **182 big-change images/s**
    (seconds 17-29: 200-240/s, zero identical) while the probe showed **60 ticks/s** and ~400 Presents/s (60 on
    the main thread). Seconds 3-7 ran at exactly 60/s and 10-14 at exactly 30/s, cause not identified.
  - **60 control** (`capture_60.csv`, 30 s): 59.4 updates/s, 0.4% identical, median 60 big changes/s; probe 60
    Presents = 60 ticks, all on the main thread. Confirms the tool doesn't invent differences.
- Observer effect: with Desktop Duplication active the second thread presented ~420/s (title screen: 480/s)
  versus ~120/s without capture. Simulation stayed at 60 ticks/s either way. Keep in mind for any timing work.
- Conclusion: at Unlocked the engine renders the 3D scene several times per simulation tick, each with new
  content (at least a new camera; animation interpolation not yet separated). That's a render path
  decoupled from the sim, the best case for stereo: drive that path at 90 Hz from OpenXR and render
  per eye. Stereo plan needs re-agreeing with the human (RESEARCH.md §4).
- Probe removed from the mod folder; game closed; no crash events.

### 2026-11-09 Claude + human: discovery step 3 (frame rate) done

- Probe extended (`src/probe/kh2vr_probe.cpp`): live ini reload, `tick_cap_hz` (paces `OnFrame` like
  xrWaitFrame would), XInput edge logging (`pad_log`, `stick_threshold`). Self-test
  (`runs/2026-11-09-step3-selftest`): cap 45 held ticks at 45/s with ~19 ms wait per tick (game needs ~3 ms
  per tick at the title screen), reload applied within 1 s, pad found as XInput 0, no crash events.
- Human played in a field area (run, combo and jump x3 each) at Frame Rate Unlocked, then 60, then Unlocked;
  "all seemed normal". Log `runs/2026-11-09-step3-framerate/kh2vr_probe.log` (23:26:37-23:28:17, 59 pad events,
  8 stick runs): **game tick 60/s throughout**; Presents ~120/s at Unlocked (half on a second thread), ~60/s
  at 60. Simulation speed is therefore identical by construction at both settings, consistent with the
  human's report. The 90 Hz cap (set 23:28:08) had no effect, since the tick never exceeds 60; phase 3 skipped.
- Monitor: RTX 4090 desktop at 2560x1440, 119 Hz. `config1525.dat` (game settings, in My Games) changed one
  byte over the session, offset 30: 2 -> 3; which value means which frame-rate setting is unconfirmed
  (snapshots in the run folder, git-ignored).
- Consequence: the agreed stereo plan (game ticking at 90) can't work as stated; options in RESEARCH.md §4.
- Probe removed from the mod folder; game closed; no crash events.
- Next: settle what the second-thread frame is (open issue), then re-agree stereo with the human, then step 4.

### 2026-11-09 Claude: discovery step 2 done: the game renders with D3D12

- Built `kh2vr_probe` (`src/probe/kh2vr_probe.cpp`, `scripts/build_probe.cmd`, VS 2022 / MSVC, /MT): a
  Panacea plugin that patches DXGI's swapchain vtable Present/Present1 slots (found via a throwaway
  D3D11 swapchain; no game addresses, so no build table entry needed) on the first `OnFrame`, never
  in `OnInit` (Panacea calls `OnInit` inside its `DllMain`). Logs swapchain/device facts and 1 s timing
  summaries. Settings in `kh2vr_probe.ini` next to the DLL.
- Run a (`runs/2026-11-09-step2-probe`): API check said UNKNOWN (swapchain `GetDevice` with
  `ID3D12CommandQueue` returns E_NOINTERFACE; `ID3D12Device` works) and the game crashed ~4 s in after a
  GPU hang; cause attributed to the D3D12 dummy swapchain made on the game's own device (now
  `dummy_d3d12=0` by default). Runs b and c (`...-probe-b`, `...-probe-c`, 45 s and 30 s at the title
  screen): no crash events, **API=D3D12**, swapchain facts in Rendering compatibility above.
- Timing (title screen, run c): 120 Presents/s, half on the main thread, half on a second thread;
  `OnFrame` 60/s; interval p50 ~8 ms, p99 ~16 ms. Logged as an open issue for step 3.
- Probe DLL and ini removed from the mod folder after each run; mod `kh2\dll\` is empty.
- Next: step 3 (frame rate). First read the game's frame-rate setting, then explain the second Present.

### 2026-11-09 Claude: Windows kit setup and discovery step 1 (mod loader) done

- `.venv` recreated on the PC with Python 3.13.15 (the only Python 3 here; `python` on PATH is 2.7);
  test-harness requirements installed (numpy 2.5.4, pillow 13.0.0); `frame_check.py --help` runs.
- Game found at `F:\SteamLibrary\...\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-`, Steam build 15194255,
  `KINGDOM HEARTS II FINAL MIX.exe` SHA-256 matches the pin.
- The game folder already had a Dec 2025 Panacea (`DBGHELP.dll`) + LuaBackend pointing at a deleted
  mod folder. With the human's approval: backed those up outside the repo, downloaded OpenKH
  release2-1806 (`openkh.zip`, SHA-256 matched GitHub's digest) and LuaBackend v1.9.1-hook (digest
  matched; its DLL is byte-identical to the one already installed), installed Panacea 1806 as
  `DBGHELP.dll`, repointed `panacea_settings.txt` and `LuaBackend.toml` to a new empty mod folder.
  The Mods Manager's first-run wizard has not been run (not needed for dll/Lua work).
- Proof: `lua/discovery/smoke.lua` copied into the mod scripts folder, game launched directly:
  process loaded the game-folder `dbghelp.dll` (Panacea) and `LuaBackend.dll`; heartbeat file
  reached frames=2580 at os.clock=49.0 s (`runs/2026-11-09-step1-smoke/`, git-ignored). Script
  removed from the mod folder afterwards. First attempt failed: LuaBackend opens no `debug` library
  (use the `SCRIPT_PATH` global).
- Findings: LuaBackend's `_OnFrame` runs on a wall-clock throttle (60 Hz default; F3 cycles
  120/240), not once per game frame, so per-frame VR work belongs in the Panacea plugin's `OnFrame`.
  The game process loads `d3d12.dll` **and** `D3D12Core.dll` (created only when a D3D12 device is
  made) plus `d3d11.dll`: points to D3D12 rendering, not yet proof of which device owns the swapchain.
- Next: discovery step 2 (confirm the swapchain's API).

### 2026-11-09 Claude: pinned the exe, corrected API and frame-rate findings

Hashed the human's uploaded exe (digest and metadata only; exe not in the repo):
SHA-256 9002b2de6a1f91a790bd0673de125d1cf833f7942bfec827cdcf6ba64d5849ed, Steam build 15194255,
PE timestamp 2024-07-22, version 1.0.0.2. Import table and RTTI show D3D12 rendering code
(D3D11 likely only for movie decoding) — the "DirectX 11" in RESEARCH.md was wrong, now a
to-confirm item. The PC port supports 30/60/120/Unlocked frame rates, so the "locked to 60"
claim was wrong too; stereo plan is now same-frame at 90 Hz paced by OpenXR, pending a game-timing
check at 90. Human agreed the plan; not using mods yet; voice playtests later.

### 2026-11-09 Claude: recorded the human's setup and scope

Human answered: seated with a gamepad; Bigscreen Beyond 2; RTX 4090; scope personal experiment
(maybe public later); Steam app 2552430 build 15194255 (the build SteamDB lists as Steam Frame
Verified on 2026-10-19). Exe SHA-256 still needed. Implications added to RESEARCH.md "Decisions
and target rig": SteamVR runtime (OpenXR via SteamVR still recommended), 75/90 Hz display vs the
game's 60 Hz loop makes re-presenting at headset rate important, 4090 has ample headroom for
drawing the scene twice at high resolution. Voice playtests explained, answer pending.

### 2026-11-09 Claude: kit set up, research pass done

Set up the vr-modding-with-agents kit at commit 35c03ea (templates, checklist, skills in
`.agents/skills` and `.claude/skills`, `.venv` with system numpy/pillow satisfying the harness
requirements, kit LICENSE kept as `KIT-LICENSE`). Ran the `vr-mod-research` pass from a cloud
workspace with no game install: wrote `RESEARCH.md` (frameworks: OpenKH Panacea `dll\` plugin
loader + LuaBackend Hook; D3D11; no existing KH2 VR mod; publisher guidance; stereo ranking; first
flat experiment). No mod code. Kit unit tests (tests.test_kit from the kit clone, repo .venv): 48 passed. Next: human answers the "Waiting on
the human" list and agrees the plan; then the flat-screen camera experiment on the Windows machine.
