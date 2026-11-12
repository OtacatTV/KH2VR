# Research: Kingdom Hearts II Final Mix VR (Steam)

Research pass, 2026-11-09 (Claude, `vr-mod-research` skill). No mod code written yet.
Target: KINGDOM HEARTS II FINAL MIX inside *KINGDOM HEARTS -HD 1.5+2.5 ReMIX-* on Steam
(app 2552430), executable `KINGDOM HEARTS II FINAL MIX.exe`.

Confidence tags used below: **[confirmed]** = a primary source or code says so; **[reported]** =
community/secondary source; **[hypothesis]** = my inference, needs an experiment; **[unknown]**.

All sources accessed 2026-11-09 unless a date is given. Code references are to shallow clones
read on that date (commits noted); nothing from them is copied into this repo.

---

## 1. Existing mods and frameworks

**No VR mod for KH2 (PC) was found.** Searched Nexus, Reddit/Flat2VR coverage, helixmod
(geo-11 / 3D Vision fixes), and general web. **[reported: absence]**

- The only "Kingdom Hearts VR" is Square Enix's own *Kingdom Hearts VR Experience*, a free,
  PSVR-exclusive diorama/cutscene piece (2019). Not a mod, not on PC.
  ([khwiki](https://www.khwiki.com/Kingdom_Hearts_VR_Experience))
- No geo-11/3D Vision fix for the collection turned up. ReShade users report since 2021-2022 that
  ReShade **cannot find a usable depth buffer** in this port, so depth-based fake-3D tools
  (ReShade SuperDepth3D, VRto3D-style reprojection) are unlikely to work as-is. **[reported]**
  ([ReShade forum #7769](https://reshade.me/forum/troubleshooting/7769-5-0-still-no-depth-buffers-on-kingdom-hearts-1-5-2-5-hd-collection),
  [#7261](https://www.reshade.me/forum/general-discussion/7261-no-depth-buffer-available))
- Not Unreal / RE Engine, so UEVR and REFramework don't apply. The engine is Square Enix's own
  (PS2-derived game code in the HD remaster). **[confirmed by elimination; engine internals unknown]**

**Frameworks to build on (strong recommendation): OpenKH Panacea + LuaBackend Hook.** This is the
established, actively maintained KH PC modding stack, used by the KH2 Randomizer, Archipelago and
Crowd Control, and it supports the Steam version.
([KH2Rando setup](https://tommadness.github.io/KH2Randomizer/setup/Panacea-ModLoader),
[Archipelago KH2](https://archipelago.gg/tutorial/Kingdom%20Hearts%202/setup_en),
[Crowd Control](https://crowdcontrol.live/guides/kh1fm))

- **Panacea** (OpenKH, commit `b67a743`, 2026-10-22) installs as a `VERSION.dll` proxy (also
  exporting DbgHelp's `MiniDumpWriteDump`), hooks the game's app vtable `onFrame`, and
  **loads every `*.dll` in the mod build folder's `dll\` subfolder, calling exported `OnInit(path)`
  once and `OnFrame()` every game frame.** That is a ready-made native plugin route with a
  frame-boundary callback, so the VR mod can be a Panacea plugin DLL rather than another proxy.
  **[confirmed: `OpenKh.Research.Panacea/dllmain.cpp`, `Panacea.cpp` `LoadDLLs`]**
- **LuaBackend Hook** (Sirius902 fork, commit `11258d3`, 2025-08-21) loads as `DBGHELP.dll`
  (or `DINPUT8.dll` under Wine) or via Panacea, runs Lua scripts **synchronised with the game's
  main loop**, and exposes relative/absolute memory read/write. Scripts hot-reload (F1 in the F2
  console). Ideal for the first flat-screen camera experiments before any C++ exists. Supports KH2FM
  PC Global and JP. **[confirmed: README, DOCUMENT.md, config/LuaBackend.toml]**
  Steam saves/scripts live under `Documents\My Games\KINGDOM HEARTS HD 1.5+2.5 ReMIX\`.
  **On-PC finding (2026-11-09, v1.9.1-hook source + run):** scripts' `_OnFrame` runs on a
  wall-clock throttle (16 ms by default, F3 cycles 8/4 ms), not once per game frame; scripts get
  base/package/coroutine/string/os/math/table/io/bit32/utf8 but **no `debug` library**, and a
  `SCRIPT_PATH` global. Fine for discovery; per-frame VR work must use Panacea's `OnFrame`. **[confirmed]**
- **ReShade** has been loaded into this game as `dxgi.dll` **[reported, ReShade log in forum #7769]**,
  so a DXGI proxy is a viable fallback loading route, at the cost of conflicting with ReShade.
- Prior art that touches the camera/viewport (proves the values are reachable from Lua):
  *KH2: Dynamic Ultrawide Support* sets viewport width/height from the window aspect via
  LuaBackend ([Nexus kh2fm/77](https://www.nexusmods.com/kingdomhearts2finalmix/mods/77));
  a KH1 camera-sensitivity Lua mod located camera physics constants and refuses to write on
  unknown builds ([Nexus kh1fm/222](https://www.nexusmods.com/kingdomheartsfinalmix/mods/222));
  Re:Fixed/Re:Fined add a KH1 FOV option ([rpgsite](https://rpgsite.net/feature/12932-kingdom-hearts-1-2-and-birth-by-sleep-on-pc-get-a-wealth-of-new-nonintrusive-features-and-enhancements-with-the-refixed-mod)).
  Re:Fined has moved to Codeberg, which this environment can't reach; worth reading on your machine.

**Decision proposed:** build our own OpenXR VR plugin (D3D12 or D3D11, whichever the game renders with), loaded by Panacea's `dll\` folder,
using LuaBackend for discovery experiments. Don't fork a VR framework (there isn't one to fork).

## 2. Loading, graphics API, builds, publisher guidance

- **Graphics API: Direct3D 12 [confirmed on the PC 2026-11-09]**: the probe found one swapchain whose
  device is an `ID3D12Device` (STATUS.md Log). The analysis below is kept for history.
  Earlier: probably Direct3D 12, not 11 (corrected 2026-11-09 after inspecting your exe).
  The Steam store lists "DirectX: Version 11" with a GTX 750 / RX 460 minimum
  ([Steam store](https://store.steampowered.com/app/2552430/KINGDOM_HEARTS_HD_1525_ReMIX/)), but
  the exe's import table pulls `D3D12SerializeRootSignature` and `d3d12.dll` ordinal 101 from
  `d3d12.dll`, and its RTTI names `std::function`s taking an `ID3D12GraphicsCommandList*`.
  **[confirmed: D3D12 code is present]** It also imports `D3D11CreateDevice`, which fits the
  Media Foundation video path (`MFCreateDXGIDeviceManager` needs a D3D11 device) for the
  pre-rendered movies. **[hypothesis: D3D12 renders the game, D3D11 decodes movies]** "DirectX 11"
  on the store most likely means D3D12 at feature level 11_x (a GTX 750 can run that).
  **Confirm on your PC** with one RenderDoc/PIX capture, or from our first probe logging which
  device owns the swapchain. If it's D3D12: OpenXR uses `XR_KHR_D3D12_enable` (SteamVR supports
  it), and per-eye re-rendering means recording the scene pass twice into command lists, not
  replaying immediate-context draws.
  Swapchain format seen in a ReShade log: `B8G8R8A8_UNORM`. **[reported]**
- **Other exe facts [confirmed]:** 64-bit, mixed native + .NET (C++/CLI: CLR header, imports
  `mscoree.dll`), links `steam_api64.dll`. Input arrives via XInput (`XINPUT1_4` ordinals 2/3,
  i.e. `XInputGetState`/`XInputSetState`), DirectInput 8 and raw HID (PlayStation-style pads), with
  Steam Input in front on Steam. Imports `dbghelp.dll`, the name LuaBackend proxies.
- **Launch:** the collection launcher can be bypassed by putting the game's exe name before
  `%command%` in Steam launch options, or with a `steam_appid.txt` (2552430) for direct launch.
  **[reported, PCGamingWiki]** Useful for the kit's `-DirectLaunch` test runs.
- **Builds (for `vr-mod-game-patches`):** Steam updates have shipped without patch notes and have
  broken memory-address mods before. **[reported]**
  - Steam build **14716933** (June 2024): launcher/settings font only, exes unchanged.
    ([Steam discussion](https://steamcommunity.com/app/2552430/discussions/0/4347743556801015863/))
  - **26 Aug 2024** update: all `.exe` files changed (AMD RX 7000 / ROG Ally crash fix).
    ([Steam news](https://steamcommunity.com/app/2552430/allnews/),
    [discussion](https://steamcommunity.com/app/2552430/discussions/0/4426562616043342284))
  - Steam Frame "Verified" test on build **15194255**, 19 Oct 2026; SteamDB record updated
    26 Oct 2026. Whether the exe changed in Oct 2026 is **[unknown]**.
    ([SteamDB](https://steamdb.info/app/2552430/patchnotes/))
  - Archipelago's KH2 guide still pins "Steam Build Version 14716933" (older page revisions) — treat
    community address tables as possibly stale. The Xbox PC (Microsoft Store) release of
    8 Oct 2026 is a different distribution and **out of scope**.
  - **Action:** pin *your* exe by SHA-256 + Steam build ID before any address goes in the table.
- **Anti-cheat / online:** none found; the game is single-player and its Steam spec lists no
  network requirement. **[reported]**
- **Publisher guidance (summary, not legal advice — your call):**
  - Square Enix's West *Material Usage Policy* says players must still follow the software licence
    and may not modify its games without permission or reverse engineer them.
    **[confirmed, official]** ([square-enix-games.com](https://square-enix-games.com/documents/materialusagepolicy), accessed 2026-11-09)
  - The game-specific Steam EULA for app 2552430 **couldn't be retrieved** from here. **[unknown]**
    Read it in the Steam client (store page → EULA) and note its date.
  - Steam's Subscriber Agreement also restricts modifying Steam content without Valve's consent
    (per a third-party summary captured 2026-05-10; read the original). **[reported]**
  - No official KH PC modding programme, mod tools or modding mode was found. In practice a large,
    public KH PC modding scene (OpenKH, Randomizer, Archipelago, Re:Fined, Crowd Control) has
    operated openly since 2021 with no takedowns found in this pass — **that is tolerance observed,
    not permission.** Square Enix has been comparatively strict on FFXIV mods (an online game).
  - **Your decision needed:** scope (personal-use experiment vs. public release), recorded here and
    in `STATUS.md`. The release question can wait for `vr-mod-release` + `checklists/before-publishing.md`.

## 3. Camera, projection and frame boundary

None of these are publicly documented for the PC remaster. **[unknown]** What we know and how to find them:

- **Frame boundary:** Panacea's `onFrame` hook (app vtable slot) gives one call per game frame,
  before the game's own frame update. **[confirmed in code]** The DXGI `IDXGISwapChain::Present`
  is the GPU-side boundary; pair the two so each Present maps to a game frame ID for the trace.
- **Frame rate: the PC port is NOT locked to 60** (corrected 2026-11-09; the earlier draft wrongly
  carried over the PS4 limit). The PC settings offer **30 / 60 / 120 / Unlocked** frame rate
  ([Wccftech, 2021-03-30](https://wccftech.com/kingdom-hearts-pc-unlocked-frame-rate/)), and the exe
  has `SettingMenu::CFrameRate` and `CVSyncMode` classes. **[confirmed]** Caveats **[reported]**:
  stutters above 60 fps, fixed in 2021 by a community exe patch for KH2
  ([DSOGaming](https://www.dsogaming.com/news/mod-fix-stutters-cutscene-fps-issues-kingdom-hearts-2-pc/);
  whether today's build still has it is unknown); in-engine cutscenes reportedly stay at 30 fps;
  some players saw no change when unlocking. The PS4 needed patches for gauge, damage and movement
  bugs at 60 fps ([OpenKH docs](https://openkh.dev/remasters/15plus25/updates.html)), so
  **gameplay correctness at 90 must be tested** (timed runs and combos at 60 vs 90), not assumed.
  This drives the stereo choice in §4.
- **Camera:** KH2 has a right-stick orbit camera (with vertical tilt in FM) plus scripted event
  cameras. The ultrawide and FOV mods show viewport/aspect/FOV live in writable memory.
  **Find it by:** (1) LuaBackend + Cheat Engine float scans while orbiting the camera (expect an
  eye position and a target/look-at, possibly yaw/pitch/distance); (2) a RenderDoc capture of one
  frame to locate the view and projection constants in the per-draw constant buffers;
  (3) Ghidra headless on your machine to find what writes them. **[hypothesis: PS2-era camera =
  eye + target + FOV, matrices built each frame]**
- **Second camera / multi-view path:** none known (no split-screen, PiP or mirrors in KH2).
  **[hypothesis]** Same-frame stereo therefore means re-issuing the frame's draws with a second
  view, either by calling the game's scene-render routine twice or by recording/replaying the D3D12 (or D3D11)
  draw stream per eye. RenderDoc will show whether the 3D pass is cleanly separable from the 2D HUD
  pass (needed either way, to put the HUD on a panel).
- **Found on the PC 2026-11-09 (build 15194255; per-build table material, not general facts):** render camera
  at exe+0xac1028 = view matrix, eye, target, up (0,-1,0): a classic look-at, y-down world. Controller state at
  exe+0x718c68 (target, eye, distance-like 420.0, angle-like -1.432). Writing the controller at Panacea
  `OnFrame` only partly sticks; the camera update runs later in the tick. **[confirmed by freeze test]**
- **In-frame write point (2026-11-10):** the render-camera setter (fn exe+0x19d3d0, main thread, once per tick)
  writes eye then target; a hardware write watchpoint on each vector's w lets the mod edit eye/target right
  after, before the view matrix is built. A 5 deg / 10 deg yaw applied there moves the image by the amounts
  a pinhole camera predicts (+0.4% at 10 deg, implied hfov 104.3 deg at 2560x1440). **[confirmed, flat screen]**
  Debug-register hooks are a discovery tool; the mod proper should hook the setter through the per-build table.
  Sideways translation through the same point gives correct parallax (near ~2x far) and -4.7% vs the pinhole
  prediction at the look-at depth (2026-11-10). In the game's y-down frame, camera-right = forward x up with
  up = (0,-1,0). **[confirmed, flat screen]** This is the RESEARCH.md §6 experiment: passed.
- **Unlocked (2026-11-10):** extra in-between frames appear only while something moves (idle field = 60/s,
  orbiting = 200-440/s). The setter still runs once per tick, and a yaw edit held there shows on every drawn
  frame (no flicker), so the in-between renders are interpolated from the per-tick render camera.
  **[confirmed]** For stereo: per-eye views can be applied at the setter, but the in-between render path (where
  the head pose must be applied per drawn frame) is still to be located.
- **In-between render path (2026-11-10):** a separate render thread (entry fn exe+0x129110, loop fn exe+0x1287d0)
  presents the extra frames; per extra frame, fn exe+0x120f35 writes an interpolated camera block at
  exe+0x8aed30.. (y-up rotation rows + positions). The tick camera is read only on the main thread.
  **[confirmed: writer and rate]** Editing that block on the *main thread's* writes (~16/tick, per pass?) turns
  the world and companions but not Sora (Sora is drawn with another camera; mixed edits make Sora sink into
  the ground, seen by the human). Editing the *render thread's* write changed nothing at rest. Per-pass views
  must stay consistent, so the head pose belongs at a single upstream source: the per-tick setter (proven
  consistent incl. Sora) plus whatever the render thread reads for in-between frames (still being traced).
  Correction (same day): render-thread M2 edits *do* show in the extra frames while moving (flicker when only
  that thread is edited, steady when both are); the earlier "no effect" came from a Sora-only crop.
  Per extra frame: copy M1 (exe+0x8aed40) -> fn exe+0x13f3a0 -> fn exe+0x120f35 writes M2 (exe+0x8aed80) ->
  fn exe+0x10fe21 reads M2. Sora's pass uses a different camera, still to be found. **[confirmed]**
  M2 is a row-vector view matrix: rotating rows + translation about camera y is a true yaw for the world and
  companions (+272 px for 15 deg). Sora and the sky/sun are drawn with other camera data that no orthonormal
  CPU-memory candidate controls; next tool is a GPU capture or a viewproj-aware scan. **[confirmed 2026-11-10]**
- **GPU-side draw data (RenderDoc, frame 16438, 2026-11-10):** the 3D scene is drawn through D3D12 bundles that
  are recorded inline each frame (`scripts/rdc/bundle_draws.py`). One root signature: r0 per-draw/scene constants,
  r1 per-object block (header + a 4x4 at +320 + model-space bone matrices; GPU skinning, vertices hold
  bone-local positions), r2 camera block, r3 material/light block, r4/r5 descriptor tables. All in the
  upload buffer the game names "ConstantBuffers". **[confirmed]**
  - World geometry and Donald/Goofy: r1+320 is the **world** matrix (yaw, y flip, world translation); r2 is the
    shared camera block at +0x600300 (view in the M2 form). So an M2 edit moves them. **[confirmed]**
  - **Sora** (13 meshes inside the world bundle, same skinned pipeline): r1+320 is his **model-view** matrix,
    computed on the CPU: his world matrix (from his shadow pass) times the view gives exactly the stored
    matrix (to 0.5 units). His draws set their own r2 with an **identity** view. So the CPU builds Sora's
    model-view from a camera copy other than the edited M2: this is why M2 edits leave him fixed on screen
    and mixed edits make him sink. **[confirmed]** Which CPU camera copy feeds it: **[unknown]**, next live test.
  - **Projection:** r0 (+64 (1.992, 1.992, 1, 1), +80 xs 0.805, ys 1.430 = **102.3 x 69.9 deg**) is the one
    applied on screen: projecting Sora's, Donald's and Goofy's feet with it lands on all three in the capture's
    own thumbnail; the r2+80 matrix (xs 2.061, 51.8 deg, exactly 2.56x r0's) misses. Agrees with the
    image-shift hfov (103-104.3 deg). **[confirmed]** r0+0 also holds 1/1280, 1/720, 1280, 720 (a 1280x720
    virtual screen).
  - CPU side (live, 2026-11-10): a projection with the same lens values sits at exe+0xac18c4 and a PS2-style
    screen matrix (offset 2048/2048) at exe+0xac1840; fn exe+0x13f850 multiplies each by the setter's view.
    Editing either (offset or +10% scale, applied right after the per-tick rebuild) changes nothing on
    screen: they feed culling/VP helpers, **not** the GPU projection.
  - **Per-pass camera setup fn exe+0x125d30** (rdx = GPU projection, a stack local of fn exe+0x1a0d10;
    r8 = view, identity for CPU-model-view passes such as Sora's) runs once per render pass. Editing its
    inputs at entry: projection row2.x += 0.1 shifts every region by exactly 128 px; view translation x += 20
    gives depth-correct parallax on every region including Sora. **Stereo injection point [confirmed
    2026-11-10, Frame Rate 60]**. At Unlocked the function runs on the main thread only; the render thread's
    extra frames keep both edits at rest (60/60 frames); while moving **[unverified]**, and the render thread's
    interpolated view (fn exe+0x120f35) probably needs the eye offset too.
  - **In-between frame switch:** the render thread interpolates only when field +0x59C8 of the render object
    exe+0x8a0970 is 1; the last render pass of the tick leaves it there, and HUD passes (likely an active
    reaction command) leave 0x1000, turning interpolation off. Setting it to 1 at the render loop's read
    forces in-between frames on, with a steady HUD and real in-between camera positions.
    **[confirmed 2026-11-10; HUD cause is a hypothesis]**
  - **In-between frames update only the world camera** (render thread writes M2 while the camera moves):
    a render-thread-only camera edit flickers the world but leaves Sora unmoved; per-draw blocks (Sora's
    model-view, r0) are main-thread only. So the in-between path alone can't carry per-eye/head-pose views
    for Sora. **[confirmed 2026-11-10]** The
    setter (fn exe+0x19cba0) builds the camera-to-world matrix at exe+0xac1028 from eye/target/up and
    inverts it in place into the view. Sora's model-view is copied into the GPU upload ring by the per-draw
    uploader fn exe+0x111bd8 and has no persistent CPU copy. **[confirmed]**
  - Shadows: each character is drawn low-poly into a 256x256 target with a light camera (r2 replaced), blurred
    and projected onto a ground patch. Sky/sun: bundles 2-3 (coordinates up to ~7e6), not traced yet.
- **Depth:** ReShade's failure to find depth suggests depth is in an unusual DSV/format or cleared
  before Present. **[hypothesis]** Check in RenderDoc; needed for HUD depth and any reprojection.

## 4. Runtime and stereo recommendation

**Runtime: OpenXR**, behind a small backend interface. Nothing found that needs OpenVR; the
kit's both-eye mirror recorder is OpenVR-based, so keep SteamVR as the test runtime either way.
Revisit only if your headset/input needs something OpenXR can't do.

**Update 2026-11-09 after discovery step 3 (on the PC):** the simulation tick is fixed at **60/s** at both
Frame Rate = 60 and Unlocked; Unlocked adds a second Present per tick from another thread (~120/s on a
119 Hz monitor). So rank 1 as written (game ticking at 90) is not available. If the extra frame is a fresh
render (interpolated, decoupled from the sim), the engine already has a render-faster-than-sim path, and rank 1
becomes "60 Hz sim, renders driven at 90 by OpenXR through that path". If it's a repeat, rank 2 (60 Hz sim +
re-present with a fresh head pose at 90) becomes the plan. **Checked 2026-11-09 (STATUS.md Log, step 3b): fresh
renders**: ~180-240 distinct, large-change images/s with the camera moving at Unlocked versus 60/s at Frame
Rate 60, while the sim stayed at 60 ticks/s. **Proposed new rank 1:** 60 Hz sim; Frame Rate Unlocked; the
engine's render-between-ticks path paced to the headset (90 Hz) by OpenXR, with the 3D pass rendered once per
eye per render. Next unknowns: which thread/routine does the in-between render (the second Present thread),
what it interpolates (camera only or animation too), and how its rate is set. **[confirmed: tick fixed at 60; extra frames are fresh renders]**

**Scale (2026-11-10):** ~1 game unit = 1 cm (Sora ~161 units, Donald ~94, measured on a capture); per-eye half
separation = ipd/2 x 100 units/m x world scale (live settings). **Projection:** both eyes can share one wide
symmetric FOV set through fn exe+0x125d30's projection input (90 deg vertical tested, no culling gaps at the
test spot); OpenXR gets that FOV per eye and warps.

**Decision 2026-11-10 (human):** choose between rank 1 (90 Hz via the game's in-between renders, forced on)
and rank 2 (60 Hz + re-present) **after a two-eye prototype** has measured cost and side effects.

**Per-eye scene pass, design draft (2026-11-10, after the game's own draw code proved unreplayable):**
work at the D3D12 command level, not by re-running game code.
1. Observe (read-only, first): hook ID3D12GraphicsCommandList recording and ID3D12CommandQueue::
   ExecuteCommandLists through their vtables (instances created on the game's device only to find the
   vtables). Per frame and thread: lists and bundles recorded, render-target switches, root-CBV r2 bindings
   (shared camera block on the main list; Sora's identity-view block inside his bundles), bundle executes.
2. Twin bundles: while the game records a bundle, record a mirror bundle whose r2 bindings point at
   eye-adjusted copies of the same blocks (view translation += eye offset; Sora's identity view gets the
   same offset, applied after his model-view). Everything else mirrored as is.
3. Right-eye replay: after the game's scene segment on the main list (render target = MSAA scene target),
   bind a right-eye target and depth, clear, and for each ExecuteBundle bind the right-eye r2 copy and
   execute the twin. Shadow passes, post and HUD are not replayed at first.
4. Projection: one symmetric FOV wide enough for both eyes, set through fn exe+0x125d30's projection input
   (proven to reach all draws), each eye cropped from it.
Unknowns the observer must settle: in-between (render-thread) frames' command structure; whether bundles
are re-recorded every frame; ring/allocator sizes for the extra work; the post chain per eye.

**Stereo, ranked for this game (as of the research pass):**

| Rank | Approach | Why |
|---|---|---|
| 1 | **Same-frame stereo at the headset's rate**: game set to Unlocked and paced by the OpenXR runtime (`xrWaitFrame`) at 90 Hz (or 75), the 3D pass rendered once per eye each game frame (render routine called twice, or the scene pass recorded twice with per-eye view/projection) | The game can run above 60 on PC and is very light, so a 4090 can render two eyes at 90. One head pose and one game state per pair: no inter-eye smear, no reprojection during gameplay. Depends on gameplay being correct at 90 (§3). |
| 2 | Same-frame at 60 + **re-present at headset rate** (redraw the last game frame's 3D pass with a fresh head pose) | Fallback if 90 Hz gameplay misbehaves, and still needed for **30 fps cutscenes** unless those go on a flat cinema panel (simpler; proposed for M1). |
| 3 | Alternate-eye | Halves the stereo rate and smears fast combat between eyes. **Not recommended.** |
| — | Depth-based fake stereo | Blocked by missing depth (§1) and not true stereo. |

**What's needed for rank 1:** (0) game timing correct at 90 fps; (a) the view/projection constants found (§3); (b) proof the 3D scene
pass can be re-issued without side effects (particle/animation state advancing twice, per-frame
GPU counters, render-target ping-pong); (c) the HUD pass separated. All three are RenderDoc +
small experiments, not big builds.

## 5. Gameplay contexts, hooks and feasible scope

**Contexts in KH2FM [confirmed from the game's design; bindings to verify in the PC controls menu]:**
field exploration (3rd person, jump, glide in Final Form, Reaction Commands on △), real-time combat
via the command menu (Attack, Magic, Items, Drive, Summon, Limits, shortcuts, lock-on), Drive Form
transformations, cutscenes (real-time event cameras and pre-rendered movies), menus (pause/camp,
shops, save points), Gummi Ship (rail shooter — a vehicle context), and minigames (Light Cycle,
skateboarding, Atlantica rhythm, Struggle).

**What a mod can control (current evidence):**

| Capability | Status |
|---|---|
| Read/write game memory per frame | **[confirmed]** LuaBackend / Panacea plugin |
| Camera & viewport/FOV | **[reported reachable]** (ultrawide/FOV mods); exact structure **[unknown]** |
| Game state for observation (HP/MP, world/room, current command, drive state) | **[reported]** community RAM maps used by Randomizer/Archipelago; verify per build |
| Input injection | **[hypothesis]** gamepad via XInput hook or a virtual pad; keyboard also supported by the game. Steam Input sits in between on Steam — check which API the game actually reads |
| Sora's skeleton / weapon transform for tracked keyblade | **[unknown]**; attacks are animation- and combo-driven, hit detection is the game's |

**Feasible scope, proposed (camera and input chosen separately):**

- **Milestone 1 — camera swap, third-person.** Head-tracked stereo camera riding the game's chase
  camera (or a stable "follow" camera behind Sora), gamepad as-is, HUD/menus on a floating panel,
  cutscenes on a cinema panel or comfort-locked. Lowest risk; proves stereo.
- **Milestone 2 — diorama option** for combat-heavy areas (fixed world-anchored view, re-centre on
  room change). KH2's camera swings a lot; this may be more comfortable than the chase camera.
- **Later / optional:** tracked aiming for magic/lock-on targeting; tracked keyblade swings are
  possible only as *gestures that trigger the game's Attack*, not physical hits — decide after
  playing M1. Details go to `CONTROLS.md` via `vr-controls-design`.

**Requested action vs. outcome:** for every injected action, confirm the result from memory (e.g.
combo counter/animation ID changed, MP dropped after a spell, menu cursor index moved), not from
"button sent". Which addresses carry those signals is an open item for the discovery pass.

## 6. First flat-screen experiment (before any headset work)

Run on your Windows machine with LuaBackend (hot-reloadable), in a quiet room (e.g. Twilight Town
Sandlot, no enemies):

1. Find the camera eye position (and target) by float scan while orbiting; confirm by freezing it.
2. Take a screenshot; shift the eye **and** target 0.5 units along the camera's right vector;
   screenshot again.
3. Predict the horizontal screen shift of a marker at known distance `z`:
   `Δx_px = (d / (2·z·tan(hfov/2))) · width`, with `hfov` from the projection.
   Pass if measured within ~5%; also check a yaw of exactly 5° shifts distant geometry by
   `tan(5°)/tan(hfov/2) · width/2` pixels.
4. Restore the camera; confirm no drift. Record values, build ID and screenshots' run folder in
   `STATUS.md` (screenshots stay out of git).

This proves we own the camera before anything is built on it.

## 7. Rendering baseline and evidence plan

- **Required capabilities:** the game's API and feature level (probably D3D12 at FL 11_x, §2;
  **[to confirm]**); OpenXR swapchain images for that API (`XR_KHR_D3D12_enable` or
  `XR_KHR_D3D11_enable`); no vendor SDK. **Vendor-neutral baseline:** stereo, frame tracking and
  diagnostics work on any GPU that runs the game.
- **AA / upscaling / temporal state:** the game's PC graphics options and any temporal effects
  (e.g. motion blur during Drive Forms, screen blurs in cutscenes) are **[unknown]** — inventory in
  RenderDoc. If any effect keeps a history buffer, it must be per eye.
- **Known GPU history:** RX 7000-series and ROG Ally crashes at Steam launch, fixed by the
  26 Aug 2024 update (and an AMD driver). **[confirmed, Steam news]** Keep AMD in the test matrix.
- **Evidence matrix:** empty. Your GPU/driver/headset are **[unknown]**; every combination is
  *untested* until measured.
- **Representative performance scenes (planned):** small: Twilight Town Sandlot; dense interior:
  Hollow Bastion Castle Postern / Radiant Garden town; terrain/outdoor: Land of Dragons or Pride
  Lands; busiest gameplay: the 1,000 Heartless battle; effects-heavy: Space Paranoids; vehicle:
  a Gummi Ship mission. Matched settings, repeated A/B runs, both eyes checked with `frame_check.py`.
- **Unsupported-setting behaviour:** if a mode can't produce true stereo (e.g. an unrecognised
  build or pass layout), fall back visibly to a flat cinema panel with a message, never silent
  mono-in-stereo.

## Decisions and target rig (2026-11-09)

- **Rig:** Bigscreen Beyond 2, seated, gamepad, RTX 4090. **Scope:** personal experiment; maybe
  public later. Not using any mods yet; happy to install what's needed. Plan agreed 2026-11-09.
  Voice playtests: later.
- **Public source release (decided 2026-11-11):** the human chose to publish the source publicly at
  github.com/OtacatTV/KH2VR as an unofficial, non-commercial early experimental build, inviting others to
  continue development, after being shown that Square Enix's Material Usage Policy restricts modifying and
  reverse-engineering its games without permission (§2). Licence MIT, credited to "KH2VR contributors".
  Source only (no installer or binaries); the repo was checked for personal data, game files and game code
  before publishing. The Steam EULA for app 2552430 is still unread here ([unknown], §2).
- **Pinned build:** Steam app 2552430, build ID 15194255, `KINGDOM HEARTS II FINAL MIX.exe`
  SHA-256 `9002b2de6a1f91a790bd0673de125d1cf833f7942bfec827cdcf6ba64d5849ed`, 8,659,216 bytes,
  PE timestamp 2024-07-22 10:45:30 UTC, version resource 1.0.0.2 (the collection's version).
  This digest is the first row of the build table; the exe itself is not stored anywhere in the repo.
- **Headset implications:** the Beyond 2 tracks with SteamVR base stations, so it runs on the
  SteamVR runtime: OpenXR through SteamVR is still the recommendation, and the kit's OpenVR mirror
  recorder works for both-eye checks. Displays are 2560×2560 per eye at 75 Hz, or 1920×1920
  upscaled at 90 Hz ([UploadVR](https://www.uploadvr.com/bigscreen-beyond-2-and-beyond-2e-announced/),
  [Road to VR](https://roadtovr.com/pc-vr-bigscreen-beyond-2-fov-boost-release-date-price/)).
  The game's own 120 fps option is real (§3), but the headset tops out at 90, so the target is
  the game Unlocked and paced to 90 by the VR runtime, not the 120 setting. 75 Hz (full panel
  resolution) is the fallback if 90 shows game-timing problems.
- **GPU:** a 4090 has large headroom for drawing a PS2-era scene twice at ~2.5k per eye, so
  render cost won't drive design choices; the evidence matrix still starts with only this one
  NVIDIA combination, and AMD/Intel stay *untested*.
- **Input:** gamepad as-is fits milestone 1 (camera swap). No motion controllers needed yet.

## Open questions for you

All answered 2026-11-09. Next is the on-PC discovery pass (STATUS.md "Waiting on the human").
