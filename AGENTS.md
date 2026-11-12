# Kingdom Hearts II Final Mix VR mod

Adds VR to Kingdom Hearts II Final Mix from *KINGDOM HEARTS -HD 1.5+2.5 ReMIX-* on Steam
(app 2552430, `KINGDOM HEARTS II FINAL MIX.exe`; Square Enix's own PS2-derived engine, Direct3D 12 likely (D3D11 for video; to confirm), build 15194255 pinned by SHA-256 in RESEARCH.md)
for the Bigscreen Beyond 2 (SteamVR runtime, 75 Hz native / 90 Hz upscaled), played seated with a
gamepad, on an RTX 4090. Loads as an OpenKH Panacea plugin DLL; LuaBackend Hook for discovery scripts.
Project scope (decided 2026-11-09, updated 2026-11-11): source published publicly as an unofficial,
non-commercial early experimental build (MIT, github.com/OtacatTV/KH2VR), open to contributors; a player
package later via `vr-mod-release` and `checklists/before-publishing.md`. Publisher guidance:
RESEARCH.md §2. Camera/input modes: proposed in RESEARCH.md §5, not yet agreed.

## Before you change anything

- Read `STATUS.md` (the shared scratch pad; add a dated entry before you stop), `RESEARCH.md` and
  `CONTROLS.md` if they exist. Build on what's there; don't redo or undo another agent's work.
  Old handoff notes are historical: check the latest status before acting on them.
- Decisions, measurements and open issues go in versioned files with dates and run or commit IDs.
  Nothing should depend on chat history or private memory.

## Rules

- **Don't ask the human to check what you can check.** You can see the headset through SteamVR's
  VR View (or the mod's own eye snapshots), drive the controllers with the test driver, and read
  the trace and logs. Ask for the headset only for feel and comfort.
- **Look, don't count.** A submission counter going up isn't proof an image reached the headset.
  Check both eyes with `frame_check.py` (test-harness skill) and a known-bad `--reject` or golden
  `--ref`; only VERDICT PASS confirms. INCONCLUSIVE means it couldn't tell, not that it worked.
- **Verdicts before evidence.** Every image and log you read is re-sent with every later request.
  Read check verdicts and summaries first; open an image (the failure preview) or query the
  trace only when a check fails. Hand checks and per-issue work to fresh sub-agents or worktree
  sessions that report back verdicts and file paths.
- **Reproduce before you fix.** Every reported issue gets a failing reproduction (test driver
  script, eye snapshot comparison, trace check or unit test) before the fix, and the same check
  passing after. If you can't reproduce it, say what you'd need.
- **One run owns the game install.** Whoever has the game running owns its files and the headset;
  other sessions don't launch the game or install or replace the mod's files until that run ends.
- **Always clean up.** The launcher removes the mod after every run, even on failure.
- **Publisher guidance and user responsibility.** The user is responsible for checking what the
  game's publisher supports and permits for mods, anti-cheat and online play, and deciding the
  project's scope. Help by finding dated official sources, describing relevant requirements
  and flagging uncertainty. Record their decision in `RESEARCH.md` and `STATUS.md`; don't impose
  a blanket offline-only rule or treat technical feasibility as publisher approval.
- Every tunable value (thresholds, offsets, angles) is a live setting, not a constant.
- **Pin the game build and fail closed.** Every hard-coded address goes through the per-build
  lookup table; an unknown build or mismatched bytes means no hook. If the headset suddenly shows
  nothing, check the exe's build first. Porting to a new build: the `vr-mod-game-patches` skill.
- **Nothing of the game's in the repo.** No decompiler output, disassembly, crash dumps or game
  data fixtures; byte patterns from game code are digests, never literals. Shipping to players:
  the `vr-mod-release` skill.
- **Discover capabilities before choosing interactions.** Camera mode and input mode are separate
  decisions. Use only the game's actual actions and available hooks; combat, physical grabbing,
  skeletal IK and virtual gamepad output are optional. Record action semantics, contexts,
  integration routes and observable success conditions in `CONTROLS.md`.
- **Requested input isn't a game outcome.** Verify the game accepted/performed the action.
  Release/neutralise outgoing controls on context changes and focus/tracking loss, and prevent
  held inputs from becoming unrelated actions on return.
- **Keep a vendor-neutral rendering baseline.** Frame tracking, stereo delivery and essential
  diagnostics must work without an optional vendor SDK or upscaler. Separate required graphics
  capabilities from tested GPU/driver/runtime combinations; missing hardware stays untested.
  A flat fallback or a submission counter is not proof of stereo.
- **Record broad workarounds when adding them.** In `STATUS.md`, register disabled optimisations,
  forced refreshes, duplicated work, extra waits and widened bounds with their reason, scope,
  A/B switch, scene costs and revisit criterion. Revisit them on new scene types and before release.
- **Measure beyond the test room.** Rendering changes need both-eye correctness checks and
  repeated, matched performance comparisons in representative real gameplay. Keep temporal
  histories per eye and preserve visibility needed by either eye. Use the test-harness skill's
  compatibility and performance references; unresolved measurements remain explicit release gaps.

## Commands

Fill these in as the tooling gets built.

- Build: `scripts\build_probe.cmd` (discovery probe, `build\probe\kh2vr_probe.dll`; the mod itself isn't built yet)
- Tests: [command]
- Frame check from outside the game: `scripts\build_capture.cmd`, then `build\tools\frame_diff_capture.exe <out.csv> <seconds>` and `.venv\Scripts\python.exe -I scripts\analyze_capture.py <out.csv>`
- Launch a test run (unattended): `.venv\Scripts\python.exe -I scripts\kh2run.py start <run>`, then
  `scripts\kh2_flow.py load_spot <run>` (title -> save file 2 -> Back Alley test spot, ~50 s) or
  `scripts\kh2_flow.py load_combat <run>` (save file 3 -> fight in the Land of Dragons encampment, ~48 s); always finish with
  `scripts\kh2run.py stop <run>` (closes the game, removes the probe). `kh2run.py status` checks for leftovers.
- Headset image check: SteamVR's VR View (both eyes) via `scripts\kh2run.py snapwin <run> <name> "VR View"`
  (refuses when covered), split into eyes, then the test-harness `frame_check.py`.
- Test driver: probe `drive=1` + `kh2run.py pad <run> tests\pad\<script>.pad`; record a human demo with
  `pad_record=1` and `scripts\pad_from_log.py`. Screen checks: `scripts\screen_state.py` (refs in runs/refs).
- Live scenario example: `tests\live\camera_copy_edit.py <run>` (camera-copy edit test, verdict per candidate)
- Voice playtest: see the vr-playtest-loop skill

## Skills

- `vr-mod-research`: the research pass. Run it before writing mod code.
- `vr-mod-test-harness`: see, play and measure without the human.
- `vr-playtest-loop`: voice-recorded playtests and the reproduce-before-fix loop.
- `vr-mod-game-patches`: build pinning, the address table, and porting the mod after a game update.
- `vr-mod-release`: the player package and the public-repo cleanup, once the human decides to share.
- `vr-controls-design`: capability discovery, camera/input choices, context-specific actions,
  optional combat/body interactions and applicable defaults.
