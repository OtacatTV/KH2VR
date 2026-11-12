// kh2vr_probe: discovery probe loaded by OpenKH Panacea from <mod_path>\kh2\dll\.
//
// Answers discovery step 2 (which graphics API owns the game's swapchain) and logs presentation
// timing for step 3. It uses no game addresses: it finds DXGI's swapchain vtable(s) by creating
// throwaway D3D11 and D3D12 swapchains on a hidden window, then patches the Present/Present1
// slots. Patching happens on the first Panacea OnFrame (game main thread), never in OnInit,
// because Panacea calls OnInit from inside its DllMain (loader lock).
//
// Settings (live, read at OnInit) from <dll folder>\kh2vr_probe.ini, key=value per line:
//   enabled=1            0 = load but do nothing
//   log_dir=<folder>     where kh2vr_probe.log goes (default: the dll folder)
//   stats_ms=1000        timing summary interval
//   max_swapchains=8     distinct swapchains to describe in detail
//   dummy_d3d11=1        find the DXGI vtable via a throwaway D3D11 device+swapchain
//   dummy_d3d12=0        same via D3D12. OFF by default: D3D12CreateDevice on the default adapter
//                        returns the game's own device, and the 2026-11-09 run with this on hung
//                        the GPU (LiveKernelEvent 1a8/1b8) right after the game's first Present.
//   tick_cap_hz=0        >0: pace Panacea OnFrame (the game's main-loop tick) to this rate by
//                        waiting at the start of the tick, a stand-in for OpenXR xrWaitFrame pacing
//   pad_log=1            log XInput button/left-stick changes (times the human's test runs)
//   stick_threshold=0.5  left-stick magnitude counted as "moving"
//   cmd=scan|report|clear, cmd_seq=<n>  run a camera-discovery command once per new cmd_seq
//   scan_max_candidates=200000, rest_settle_ticks=30, report_top=30  (camera discovery tuning)
//   reload_ms=500        how often the ini's timestamp is checked; changes apply live
//   drive=0              1 = test driver: redirect the game's XInputGetState import (pass-through when idle)
//   drive_script=<file>, drive_seq=<n>  play a pad script once per new drive_seq (format: see drive_load)
//   drive_pad=0          which XInput slot the script replaces
//   drive_focus=0        1 = the game's GetForegroundWindow returns its own window (input while unfocused)
//   drive_release_ms=200 neutral pad held after a script ends, before the real pad passes through again
//                        (log_dir and the dummy_* keys only take effect at start)
// Fail closed: any setup failure is logged and leaves the game untouched.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

struct Settings {
    bool enabled = true;
    std::wstring log_dir;
    int stats_ms = 1000;
    int max_swapchains = 8;
    bool dummy_d3d11 = true;
    bool dummy_d3d12 = false;
    int tick_cap_hz = 0;
    bool pad_log = true;
    bool pad_record = false;       // log the physical pad 0's full state on every change (PADREC lines,
                                   // sticks quantised to pad_record_q), for turning a demo into a pad script
    double pad_record_q = 0.05;
    double pad_record_deadzone = 0.12;
    double stick_threshold = 0.5;
    int reload_ms = 500;
    std::wstring cmd;              // scan | report | clear; runs once when cmd_seq changes
    int cmd_seq = 0;
    int scan_max_candidates = 200000;
    int rest_settle_ticks = 30;    // idle ticks ignored after any stick input (camera glide)
    int report_top = 30;
    int report_min_rest = -1;      // >=0: report only candidates whose rest-tick change count is in [min, max]
    int report_max_rest = -1;      // (finds matrices derived from a camera edit toggled N times at rest)
    std::wstring addr;             // command target: exe+0x<rva> or 0x<absolute>
    int count = 3;                 // floats affected by freeze
    float add[3] = {0, 0, 0};      // freeze: offset added to the captured value
    float tol = 0.5f;              // find_eye: match tolerance in world units
    int max_hits = 60;
    float mv_tol = 0.02f;
    std::wstring floats;           // find_floats: comma-separated values, '*' = any
    std::wstring mon;              // monitor: comma-separated addresses (exe+0x.., 0x.., or <base>+0x.. with
                                   // mon_base), each logged as int and float every mon_ms on a MON line
    bool mon_bytes = false, mon_change = false; // mon: read single bytes; log only on change (checked every tick)
    std::wstring mon_base;         // optional base for mon entries written as +0x..
    int mon_ms = 1000;
    float float_tol = 1e-3f;          // find_mv: tolerance on the yaw-flip test
    bool mv_scan_wc = false;       // find_mv: also read write-combined / mapped memory (GPU upload buffers)
    std::wstring watch[3];         // watch: up to 3 addresses (exe+0x.. / 0x..) for hardware write watchpoints
    int watch_len[3] = {4, 4, 4};  // bytes per watch slot: 1, 2, 4 or 8 (address must be aligned to it)
    int watch_rw[3] = {1, 1, 1};   // 1 = break on write, 3 = break on read or write, 0 = execute (code address:
                                   // logs the first exec_log hits' argument registers and the matrices they point at)
    int exec_log = 8;
    bool d3d_observe = false;      // read-only D3D12 command-list observer (see d3d_install); installs once
    int d3d_inject = 0;            // 1 = replay the main scene segment into the same targets (stage A test)
    int d3d_inject_min_bundles = 2;    // was 10; open areas draw the whole world in 2 bundles (ledge hitch, run ledge3)
    bool d3d_inject_at_close = true; // replay at the list's Close when the segment never reached a post-stage clear
    int d3d_eye_lock_segments = 30;  // eye targets lock onto a scene target only after this many consecutive segments start on it
    bool d3d_eye_msaa = true;       // also replay the game's 8x MSAA scene path (it switches to it when the camera is still) into MSAA eye clones, resolved into the eye images
    bool d3d_eye_single_sample = true; // and only onto a 1-sample target (run head3 locked onto an 8x MSAA load-screen target, 2026-11-10)
    bool d3d_debug = false;        // enable the D3D12 debug layer for the game's device (read at start only)
    bool d3d_capture = false;      // map render-target/depth views to resources from device creation on (read at start)
    int d3d_eye_show = 2;          // copy an eye image over the game's scene target (monitor shows it): 0 none, 1 left, 2 right
    float d3d_eye_offset = 0.0f;   // half eye separation override in game units (0 = derive from ipd_m, units_per_m, world_scale)
    bool hud = true;               // HUD as a floating panel: the game's HUD bundles are drawn again into our own
                                   // transparent image and shown as an OpenXR quad layer
    bool bscan_small = true;       // bscan_find lists only bytes whose values are 0..3
    int bscan_label = 0;           // label for cmd=bscan_snap (0 field, 1 menu, 2 pause)
    float hud_dist = 1.4f, hud_down = 0.35f, hud_width = 1.5f, hud_tilt_deg = 15.0f; // metres / degrees, live
    int hud_debug = 0;             // 1 = show the HUD drawn over black, 2 = over white (opaque), to see what matting gets
    float hud_clear_alpha = 0.0f;  // alpha the HUD images are cleared to (the minimap masks through destination alpha)
    bool skip_nonworld = true;     // no eye replay for segments without a list-level camera binding (menu models; run flat4)
    std::wstring flat_flag = L"exe+0x717418"; // build 15194255: 1 while the player is in gameplay, 0 in menus, pause,
                                   // title, loading (found 2026-11-11, runs flag2/flag3); empty = old tick heuristic
    int flat_flag_play = 1;        // the flag's value during gameplay
    bool flat = true;              // menus, pause, loads (no eye image for flat_after ticks): the game's own finished frame
                                   // on a flat screen in front of the recentred view
    int flat_noreplay = 6;         // ticks in gameplay without an eye image before the flat screen takes over (0 = off)
    int flat_after = 2;            // ticks of 'not gameplay' (flag) or without an eye replay (no flag) before switching
    float flat_dist = 2.0f, flat_width = 2.6f, flat_down = 0.0f; // metres
    bool hud_fx_offscreen = false; // also send readable HUD-stage geometry outside the screen rectangle to the effects layer
    bool hud_fx = true;            // screen-wide effects (battle-start impact, keyhole) on a head-locked layer over the whole view
    float fx_dist = 2.0f;          // metres; the layer is sized to cover the headset FOV at this distance
    std::wstring hud_minimap_rect = L"2140,55,2500,395"; // game pixels; inside, where the HUD copy has content, the panel
                                   // takes the colour from the game's finished image (the minimap's backdrop doesn't
                                   // survive the redraw, run hud8). "0,0,0,0" = off
    bool hud_skip_fullscreen = true; // leave bundles whose quad covers ~the whole screen off the HUD (glow composite, battle afterimage)
    bool hud_skip_composite = false; // leave the first scene-bound bundle after the glow chain (the glow composite) off the HUD
    bool hud_premul = false;       // treat the HUD image's colour as premultiplied by its alpha
    int hud_skip_lo = -1, hud_skip_hi = -1; // mapping tool: skip post-stage bundles [lo, hi] on the game's own list
    bool eye_history = true;       // copies in the replay from a past-frame image (same size as the scene, not the scene)
                                   // read the eye's own previous image instead (ghost of the game view, run hist1)
    bool eye_copy_fix = true;      // copies the replay makes from the game's scene image read the eye's image instead
                                   // (scaled by a compute shader when the sizes differ); jump-attack flash, 2026-11-11
    int eye_w = 2560, eye_h = 2336; // eye image size (0 = the game's scene size). Read when the eye targets are created
                                    // (restart to change). 2336 = 2560 / (tan 55.05 / tan 53.3), this headset's FOV shape
    int eye_clip_fx_mode = 1;      // 2 = diagnostic: hide those draws (zero matrix)
    bool eye_clip_fx = true;       // draws with an identity projection (vertices pre-projected by the CPU with the game's
                                   // projection, e.g. attack effects): remap game clip space -> eye clip space
    int eye_fov = 1;               // eye projection: 0 = the game's, 1 = the headset's per-eye FOV (symmetric union of both eyes)
    float eye_fov_scale = 1.0f;    // multiplies the tangents of the headset FOV (>1 = render a margin beyond the visible area)
    bool xr_own_queue = true;      // XR session + copies on a queue of ours (fenced after the game's eye-image list), not the game's
    bool xr_copy = true;           // diagnostic: 0 = the XR thread submits frames without layers (no swapchain use)
    bool xr_thread = true;         // XR frame loop on its own thread at the headset rate (0 = v0: wait/begin/end on the game thread)
    bool xr_head = true;           // head tracking: eye poses (relative to the recentre pose) applied to the eye cameras
    float xr_test_yaw_deg = 0.0f;  // synthetic extra head yaw for tests (degrees, right positive)
    int xr_recenter_seq = 0;       // change to recentre on the current head pose
    bool stack_watch = true;       // at start: log the main thread's call stack when its committed stack grows
    int stack_watch_step = 64;     // KB of growth between logs
    bool crash_catch = true;       // at start: log + minidump on a stack overflow anywhere or an access violation in the probe
    bool xr = false;               // OpenXR output (needs d3d_inject>=2, d3d_capture=1; headset on, SteamVR running)
    float xr_xs = 0.805f, xr_ys = 1.431f; // the eyes' projection x/y scales (must match the game's / the widened one)
    std::wstring xr_loader;        // openxr_loader.dll path override (default: the active runtime's, SteamVR layout)
    float ipd_m = 0.063f;          // the player's eye separation, metres
    float units_per_m = 100.0f;    // KH2 game units per metre: ~1 unit = 1 cm (Sora ~161 units, Donald ~94; 2026-11-10)
    float world_scale = 1.0f;      // >1 makes the world feel smaller (larger eye separation in game units)
    float d3d_eye_wvp_scale = 0.0f;   // stage C: x term added to combined r0 blocks = offset * this. Off: it isn't what
                                      // places the shadows (calibration not linear, 2026-11-10); kept for experiments   // stage C: view-space x offset added to the eye's camera blocks (game units)      // stage B/C test: copy the eye image over the game's scene target (monitor shows it)
    // Replay a call once per tick (double-render experiment): execute watch replay_start_slot on the call's
    // first argument-setup instruction (timing start), replay_end_slot on its return address. At the return,
    // if replay_n > 0 and this tick hasn't replayed yet, RIP is set back to replay_rip (the setup
    // instruction) so the game makes the same call again in its own context. Times every call (QPC).
    int replay_start_slot = -1;
    int replay_end_slot = -1;
    std::wstring replay_rip;
    int replay_n = 0;
    int poke_when = 0;             // execute-watch pokes: 0 always, 1 only inside a replayed call, 2 only outside
    // Poke on write: when watch slot poke_slot is written, add poke_add to the float at
    // (watched address + poke_offset bytes). Edits a value right after the game rebuilds it, before its
    // readers run. The target must not overlap any watched bytes (fail closed, counted as refused).
    int poke2_offset = 0;          // optional second poke target (same base, same set/add mode); 0 add = off
    float poke2_add = 0.0f;
    int poke_slot = -1;
    int poke_reg = -1;             // -1: target = watched address + poke_offset. 0..3 (execute watch only):
                                   // target = rcx/rdx/r8/r9 + poke_offset, applied at function entry
    int poke_offset = 0;
    float poke_add = 0.0f;
    std::wstring poke_abs;         // execute watch only: poke this absolute address (exe+0x.. / 0x..) instead
                                   // of a register-relative one (poke_reg is then ignored)
    bool poke_set = false;
    bool poke_int = false;         // write poke_add as a 32-bit integer (set, not add)         // 1: write poke_add as the value instead of adding it (idempotent when the
                                   // same buffer is passed to several calls)
    std::wstring exec_arg;         // execute breakpoints: only log calls where rcx/rdx/r8/r9 equals this address
    // Matrix yaw edit through a watch slot: on a write to the watched address, rotate the 3 rotation rows
    // that start mat_rows_offset bytes before it about the world y axis (rows stride 16, xyz edited, w kept).
    int mat_slot = -1;
    int mat_rows_offset = 56;      // watched address minus row0 address (0x8aedb8 - 0x8aed80)
    float mat_yaw_deg = 0.0f;
    int mat_include_t = 0;         // 1 = also rotate the translation row (camera-space yaw of a view matrix)
    int mat_threads = 3;           // 1 = render (non-main) thread writes only, 2 = main thread only, 3 = both
    int ptrack_min_orbit = 1;      // ptrack only candidates that changed at least this often while orbiting
    int ptrack_max = 3000;         // cap on tracked candidates (per-Present cost; 36k slowed Presents to ~28/s)
    // Camera adjust through the watchpoints. Live; 0 = off.
    int cam_eye_slot = -1;         // slot watching part of the render eye vector (xyzw)
    int cam_target_slot = -1;      // slot watching part of the render target vector
    int cam_watch_offset = 12;     // bytes from the vector's x to the watched address (12 = w only).
                                   // Must not overlap xyz: a watched byte we write ourselves re-traps
                                   // inside the handler (2026-11-10 crash, 0x80000004 in the probe).
    float offset_right = 0.0f;     // move eye and target along the camera's right vector (world units)
    float yaw_deg = 0.0f;          // rotate the target about the eye around the world up axis
    float cam_up[3] = {0.0f, -1.0f, 0.0f}; // world up (KH2 is y-down)
    // Test driver (scripted pad). See drive_load / xget_hook.
    bool drive = false;
    std::wstring drive_script;
    int drive_seq = 0;
    int drive_pad = 0;
    bool drive_focus = false;
    int drive_release_ms = 200;
};
int g_cmd_seq_done = -1;

Settings g_cfg;
HWND g_game_hwnd = nullptr;        // the game's swapchain window (set at its first Present)
std::mutex g_log_mutex;
FILE* g_log = nullptr;
bool g_hook_attempted = false;
LARGE_INTEGER g_qpf;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

constexpr int kPresentSlot = 8;   // IDXGISwapChain::Present
constexpr int kPresent1Slot = 22; // IDXGISwapChain1::Present1

struct PatchedVtable {
    void** vtbl;
    PresentFn orig_present;
    Present1Fn orig_present1;
};
std::vector<PatchedVtable> g_patched; // written once on the main thread before any hook fires

// Timing (guarded by g_log_mutex; Present is called from whichever thread the game presents on).
std::vector<void*> g_described;
std::vector<double> g_intervals_ms;
LONGLONG g_last_present = 0;
LONGLONG g_window_start = 0;
std::atomic<long> g_frames_in_window{0}; // Panacea OnFrame calls
long g_presents_in_window = 0;
long long g_total_presents = 0;
DWORD g_last_thread = 0;
std::atomic<DWORD> g_frame_thread{0}; // thread Panacea's OnFrame runs on (game main thread)
long g_presents_on_frame_thread = 0;
int g_thread_change_notes = 0;      // only the first few are logged; STATS carries the counts
std::atomic<long long> g_total_ticks{0};
std::atomic<long long> g_cap_wait_us_in_window{0};
LONGLONG g_last_reload_check = 0;   // main thread only
LONGLONG g_cap_deadline = 0;        // main thread only

double now_ms(LONGLONG t) { return 1000.0 * double(t) / double(g_qpf.QuadPart); }
#pragma warning(push)
#pragma warning(disable : 4717) // recursive on all paths: on purpose
__declspec(noinline) int crash_recurse(volatile int d) { volatile char pad[512]; pad[0] = (char)d; return crash_recurse(d + 1) + pad[0]; }
#pragma warning(pop)
DWORD WINAPI crash_test_thread(LPVOID) { return (DWORD)crash_recurse(0); }

LONGLONG qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_log);
}

std::wstring trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::wstring g_ini_path;
FILETIME g_ini_mtime = {};

bool ini_mtime(FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_ini_path.c_str(), GetFileExInfoStandard, &a)) return false;
    *out = a.ftLastWriteTime;
    return true;
}

// initial=true at OnInit; on live reloads log_dir/dummy_* keep their start values.
void load_settings(bool initial) {
    std::wstring keep_log_dir = g_cfg.log_dir;
    bool keep_d11 = g_cfg.dummy_d3d11, keep_d12 = g_cfg.dummy_d3d12;
    // Start from defaults: a key missing from the ini must not keep its value from an earlier run
    // (2026-11-10: camera-adjust slots from one run stayed active in the next).
    if (!initial) { g_cfg = Settings{}; g_cfg.log_dir = keep_log_dir; }
    ini_mtime(&g_ini_mtime);
    FILE* f = _wfopen(g_ini_path.c_str(), L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring l = trim(line);
        if (l.empty() || l[0] == L'#' || l[0] == L';') continue;
        size_t eq = l.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (k == L"enabled") g_cfg.enabled = v != L"0";
        else if (k == L"log_dir" && !v.empty()) g_cfg.log_dir = v;
        else if (k == L"stats_ms") g_cfg.stats_ms = std::max(100, _wtoi(v.c_str()));
        else if (k == L"max_swapchains") g_cfg.max_swapchains = std::max(1, _wtoi(v.c_str()));
        else if (k == L"dummy_d3d11") g_cfg.dummy_d3d11 = v != L"0";
        else if (k == L"dummy_d3d12") g_cfg.dummy_d3d12 = v != L"0";
        else if (k == L"tick_cap_hz") g_cfg.tick_cap_hz = std::max(0, _wtoi(v.c_str()));
        else if (k == L"pad_log") g_cfg.pad_log = v != L"0";
        else if (k == L"pad_record") g_cfg.pad_record = v != L"0";
        else if (k == L"pad_record_q") g_cfg.pad_record_q = std::min(0.5, std::max(0.005, _wtof(v.c_str())));
        else if (k == L"pad_record_deadzone") g_cfg.pad_record_deadzone = std::min(0.9, std::max(0.0, _wtof(v.c_str())));
        else if (k == L"stick_threshold") g_cfg.stick_threshold = std::min(1.0, std::max(0.05, _wtof(v.c_str())));
        else if (k == L"reload_ms") g_cfg.reload_ms = std::max(100, _wtoi(v.c_str()));
        else if (k == L"cmd") g_cfg.cmd = v;
        else if (k == L"cmd_seq") g_cfg.cmd_seq = _wtoi(v.c_str());
        else if (k == L"scan_max_candidates") g_cfg.scan_max_candidates = std::max(1000, _wtoi(v.c_str()));
        else if (k == L"rest_settle_ticks") g_cfg.rest_settle_ticks = std::max(0, _wtoi(v.c_str()));
        else if (k == L"report_top") g_cfg.report_top = std::max(1, _wtoi(v.c_str()));
        else if (k == L"report_min_rest") g_cfg.report_min_rest = _wtoi(v.c_str());
        else if (k == L"report_max_rest") g_cfg.report_max_rest = _wtoi(v.c_str());
        else if (k == L"addr") g_cfg.addr = v;
        else if (k == L"count") g_cfg.count = std::min(16, std::max(1, _wtoi(v.c_str())));
        else if (k == L"add") swscanf(v.c_str(), L"%f,%f,%f", &g_cfg.add[0], &g_cfg.add[1], &g_cfg.add[2]);
        else if (k == L"tol") g_cfg.tol = (float)_wtof(v.c_str());
        else if (k == L"mon") g_cfg.mon = v;
        else if (k == L"mon_bytes") g_cfg.mon_bytes = _wtoi(v.c_str()) != 0;
        else if (k == L"mon_change") g_cfg.mon_change = _wtoi(v.c_str()) != 0;
        else if (k == L"mon_base") g_cfg.mon_base = v;
        else if (k == L"mon_ms") g_cfg.mon_ms = std::max(50, _wtoi(v.c_str()));
        else if (k == L"floats") g_cfg.floats = v;
        else if (k == L"float_tol") g_cfg.float_tol = (float)_wtof(v.c_str());
        else if (k == L"mv_tol") g_cfg.mv_tol = (float)_wtof(v.c_str());
        else if (k == L"mv_scan_wc") g_cfg.mv_scan_wc = v != L"0";
        else if (k == L"max_hits") g_cfg.max_hits = std::max(1, _wtoi(v.c_str()));
        else if (k == L"watch0") g_cfg.watch[0] = v;
        else if (k == L"watch1") g_cfg.watch[1] = v;
        else if (k == L"watch2") g_cfg.watch[2] = v;
        else if (k == L"watch_len") swscanf(v.c_str(), L"%d,%d,%d", &g_cfg.watch_len[0], &g_cfg.watch_len[1], &g_cfg.watch_len[2]);
        else if (k == L"mat_slot") g_cfg.mat_slot = _wtoi(v.c_str());
        else if (k == L"mat_rows_offset") g_cfg.mat_rows_offset = _wtoi(v.c_str());
        else if (k == L"mat_yaw_deg") g_cfg.mat_yaw_deg = (float)_wtof(v.c_str());
        else if (k == L"mat_threads") g_cfg.mat_threads = _wtoi(v.c_str());
        else if (k == L"mat_include_t") g_cfg.mat_include_t = _wtoi(v.c_str());
        else if (k == L"ptrack_min_orbit") g_cfg.ptrack_min_orbit = std::max(0, _wtoi(v.c_str()));
        else if (k == L"ptrack_max") g_cfg.ptrack_max = std::max(1, _wtoi(v.c_str()));
        else if (k == L"poke_reg") g_cfg.poke_reg = std::min(3, std::max(-1, _wtoi(v.c_str())));
        else if (k == L"poke_abs") g_cfg.poke_abs = v;
        else if (k == L"poke_int") g_cfg.poke_int = v != L"0";
        else if (k == L"poke2_offset") g_cfg.poke2_offset = _wtoi(v.c_str());
        else if (k == L"poke2_add") g_cfg.poke2_add = (float)_wtof(v.c_str());
        else if (k == L"poke_set") g_cfg.poke_set = v != L"0";
        else if (k == L"poke_slot") g_cfg.poke_slot = _wtoi(v.c_str());
        else if (k == L"poke_offset") g_cfg.poke_offset = _wtoi(v.c_str());
        else if (k == L"poke_add") g_cfg.poke_add = (float)_wtof(v.c_str());
        else if (k == L"exec_arg") g_cfg.exec_arg = v;
        else if (k == L"replay_start_slot") g_cfg.replay_start_slot = _wtoi(v.c_str());
        else if (k == L"replay_end_slot") g_cfg.replay_end_slot = _wtoi(v.c_str());
        else if (k == L"replay_rip") g_cfg.replay_rip = v;
        else if (k == L"poke_when") g_cfg.poke_when = std::min(2, std::max(0, _wtoi(v.c_str())));
        else if (k == L"replay_n") g_cfg.replay_n = std::max(0, _wtoi(v.c_str()));
        else if (k == L"d3d_observe") g_cfg.d3d_observe = v != L"0";
        else if (k == L"d3d_inject") g_cfg.d3d_inject = std::max(0, _wtoi(v.c_str()));
        if (k == L"d3d_inject_at_close") g_cfg.d3d_inject_at_close = _wtoi(v.c_str()) != 0;
        else if (k == L"d3d_eye_lock_segments") g_cfg.d3d_eye_lock_segments = std::max(1, _wtoi(v.c_str()));
        else if (k == L"d3d_eye_msaa") g_cfg.d3d_eye_msaa = _wtoi(v.c_str()) != 0;
        else if (k == L"d3d_eye_single_sample") g_cfg.d3d_eye_single_sample = _wtoi(v.c_str()) != 0;
        else if (k == L"d3d_inject_min_bundles") g_cfg.d3d_inject_min_bundles = std::max(1, _wtoi(v.c_str()));
        else if (k == L"d3d_debug") g_cfg.d3d_debug = v != L"0";
        else if (k == L"d3d_capture") g_cfg.d3d_capture = v != L"0";
        else if (k == L"d3d_eye_show") g_cfg.d3d_eye_show = std::min(2, std::max(0, _wtoi(v.c_str())));
        else if (k == L"d3d_eye_offset") g_cfg.d3d_eye_offset = (float)_wtof(v.c_str());
        else if (k == L"ipd_m") g_cfg.ipd_m = (float)_wtof(v.c_str());
        else if (k == L"xr") g_cfg.xr = v != L"0";
        else if (k == L"crash_catch") g_cfg.crash_catch = v != L"0";
        else if (k == L"stack_watch") g_cfg.stack_watch = v != L"0";
        else if (k == L"stack_watch_step") g_cfg.stack_watch_step = std::max(4, _wtoi(v.c_str()));
        else if (k == L"xr_thread") g_cfg.xr_thread = v != L"0";
        else if (k == L"hud") g_cfg.hud = _wtoi(v.c_str()) != 0;
        else if (k == L"bscan_small") g_cfg.bscan_small = _wtoi(v.c_str()) != 0;
        else if (k == L"bscan_label") g_cfg.bscan_label = _wtoi(v.c_str());
        else if (k == L"hud_dist") g_cfg.hud_dist = (float)_wtof(v.c_str());
        else if (k == L"hud_down") g_cfg.hud_down = (float)_wtof(v.c_str());
        else if (k == L"hud_width") g_cfg.hud_width = (float)_wtof(v.c_str());
        else if (k == L"hud_tilt_deg") g_cfg.hud_tilt_deg = (float)_wtof(v.c_str());
        else if (k == L"hud_debug") g_cfg.hud_debug = _wtoi(v.c_str());
        else if (k == L"hud_clear_alpha") g_cfg.hud_clear_alpha = (float)_wtof(v.c_str());
        else if (k == L"skip_nonworld") g_cfg.skip_nonworld = _wtoi(v.c_str()) != 0;
        else if (k == L"flat_flag") g_cfg.flat_flag = v;
        else if (k == L"flat_flag_play") g_cfg.flat_flag_play = _wtoi(v.c_str());
        else if (k == L"flat") g_cfg.flat = _wtoi(v.c_str()) != 0;
        else if (k == L"flat_noreplay") g_cfg.flat_noreplay = std::max(0, _wtoi(v.c_str()));
        else if (k == L"flat_after") g_cfg.flat_after = std::max(1, _wtoi(v.c_str()));
        else if (k == L"flat_dist") g_cfg.flat_dist = (float)_wtof(v.c_str());
        else if (k == L"flat_width") g_cfg.flat_width = (float)_wtof(v.c_str());
        else if (k == L"flat_down") g_cfg.flat_down = (float)_wtof(v.c_str());
        else if (k == L"hud_fx_offscreen") g_cfg.hud_fx_offscreen = _wtoi(v.c_str()) != 0;
        else if (k == L"hud_fx") g_cfg.hud_fx = _wtoi(v.c_str()) != 0;
        else if (k == L"fx_dist") g_cfg.fx_dist = (float)_wtof(v.c_str());
        else if (k == L"hud_minimap_rect") g_cfg.hud_minimap_rect = v;
        else if (k == L"hud_skip_fullscreen") g_cfg.hud_skip_fullscreen = _wtoi(v.c_str()) != 0;
        else if (k == L"hud_skip_composite") g_cfg.hud_skip_composite = _wtoi(v.c_str()) != 0;
        else if (k == L"hud_premul") g_cfg.hud_premul = _wtoi(v.c_str()) != 0;
        else if (k == L"hud_skip_lo") g_cfg.hud_skip_lo = _wtoi(v.c_str());
        else if (k == L"hud_skip_hi") g_cfg.hud_skip_hi = _wtoi(v.c_str());
        else if (k == L"eye_history") g_cfg.eye_history = _wtoi(v.c_str()) != 0;
        else if (k == L"eye_copy_fix") g_cfg.eye_copy_fix = _wtoi(v.c_str()) != 0;
        else if (k == L"eye_w") g_cfg.eye_w = std::max(0, _wtoi(v.c_str()));
        else if (k == L"eye_h") g_cfg.eye_h = std::max(0, _wtoi(v.c_str()));
        else if (k == L"eye_clip_fx") { g_cfg.eye_clip_fx = _wtoi(v.c_str()) != 0; g_cfg.eye_clip_fx_mode = _wtoi(v.c_str()); }
        else if (k == L"eye_fov") g_cfg.eye_fov = _wtoi(v.c_str());
        else if (k == L"eye_fov_scale") g_cfg.eye_fov_scale = std::max(0.5f, (float)_wtof(v.c_str()));
        else if (k == L"xr_own_queue") g_cfg.xr_own_queue = v != L"0";
        else if (k == L"xr_copy") g_cfg.xr_copy = v != L"0";
        else if (k == L"xr_head") g_cfg.xr_head = v != L"0";
        else if (k == L"xr_test_yaw_deg") g_cfg.xr_test_yaw_deg = (float)_wtof(v.c_str());
        else if (k == L"xr_recenter_seq") g_cfg.xr_recenter_seq = _wtoi(v.c_str());
        else if (k == L"xr_xs") g_cfg.xr_xs = (float)_wtof(v.c_str());
        else if (k == L"xr_ys") g_cfg.xr_ys = (float)_wtof(v.c_str());
        else if (k == L"xr_loader") g_cfg.xr_loader = v;
        else if (k == L"units_per_m") g_cfg.units_per_m = (float)_wtof(v.c_str());
        else if (k == L"world_scale") g_cfg.world_scale = (float)_wtof(v.c_str());
        else if (k == L"d3d_eye_wvp_scale") g_cfg.d3d_eye_wvp_scale = (float)_wtof(v.c_str());
        else if (k == L"exec_log") g_cfg.exec_log = std::max(0, _wtoi(v.c_str()));
        else if (k == L"watch_rw") swscanf(v.c_str(), L"%d,%d,%d", &g_cfg.watch_rw[0], &g_cfg.watch_rw[1], &g_cfg.watch_rw[2]);
        else if (k == L"cam_eye_slot") g_cfg.cam_eye_slot = _wtoi(v.c_str());
        else if (k == L"cam_target_slot") g_cfg.cam_target_slot = _wtoi(v.c_str());
        else if (k == L"cam_watch_offset") g_cfg.cam_watch_offset = _wtoi(v.c_str());
        else if (k == L"offset_right") g_cfg.offset_right = (float)_wtof(v.c_str());
        else if (k == L"yaw_deg") g_cfg.yaw_deg = (float)_wtof(v.c_str());
        else if (k == L"drive") g_cfg.drive = v != L"0";
        else if (k == L"drive_script") g_cfg.drive_script = v;
        else if (k == L"drive_seq") g_cfg.drive_seq = _wtoi(v.c_str());
        else if (k == L"drive_pad") g_cfg.drive_pad = std::min(3, std::max(0, _wtoi(v.c_str())));
        else if (k == L"drive_focus") g_cfg.drive_focus = v != L"0";
        else if (k == L"drive_release_ms") g_cfg.drive_release_ms = std::max(0, _wtoi(v.c_str()));
        else if (k == L"cam_up") swscanf(v.c_str(), L"%f,%f,%f", &g_cfg.cam_up[0], &g_cfg.cam_up[1], &g_cfg.cam_up[2]);
    }
    fclose(f);
    if (!initial) {
        g_cfg.log_dir = keep_log_dir;
        g_cfg.dummy_d3d11 = keep_d11;
        g_cfg.dummy_d3d12 = keep_d12;
    }
}

const char* fmt_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
    }
}

const char* swap_effect_name(DXGI_SWAP_EFFECT e) {
    switch (e) {
    case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
    default: return "other";
    }
}

// Called with g_log_mutex held, once per distinct swapchain.
void describe(IDXGISwapChain* sc, UINT sync, UINT flags) {
    logf("SWAPCHAIN %p first Present on thread %lu (sync_interval=%u flags=0x%x)", (void*)sc, GetCurrentThreadId(), sync, flags);

    const char* api = "UNKNOWN";
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* dev12 = nullptr;
    ID3D11Device* dev11 = nullptr;
    IUnknown* any = nullptr;
    HRESULT hr12 = sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev12);
    HRESULT hrq = sc->GetDevice(__uuidof(ID3D12CommandQueue), (void**)&queue);
    HRESULT hr11 = sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev11);
    HRESULT hrany = sc->GetDevice(__uuidof(IUnknown), (void**)&any);
    logf("  GetDevice hr: ID3D12Device=0x%08lx ID3D12CommandQueue=0x%08lx ID3D11Device=0x%08lx IUnknown=0x%08lx", hr12, hrq, hr11, hrany);
    if (any) {
        ID3D12CommandQueue* q2 = nullptr; ID3D12Device* d2 = nullptr; ID3D11Device* d3 = nullptr;
        bool isq = SUCCEEDED(any->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&q2));
        bool isd12 = SUCCEEDED(any->QueryInterface(__uuidof(ID3D12Device), (void**)&d2));
        bool isd11 = SUCCEEDED(any->QueryInterface(__uuidof(ID3D11Device), (void**)&d3));
        logf("  IUnknown device object %p QI: ID3D12CommandQueue=%d ID3D12Device=%d ID3D11Device=%d", (void*)any, isq, isd12, isd11);
        if (q2) { if (!queue) queue = q2; else q2->Release(); }
        if (d2) { if (!dev12) dev12 = d2; else d2->Release(); }
        if (d3) { if (!dev11) dev11 = d3; else d3->Release(); }
        any->Release();
    }
    if (queue || dev12) {
        api = "D3D12";
        ID3D12Device* dev = dev12;
        dev12 = nullptr;
        if (queue) {
            D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
            logf("  device: ID3D12CommandQueue %p type=%d priority=%d flags=0x%x node=%u", (void*)queue, qd.Type, qd.Priority, qd.Flags, qd.NodeMask);
            if (!dev) queue->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
            queue->Release();
            queue = nullptr;
        }
        if (dev) {
            static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                       D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
            fl.NumFeatureLevels = _countof(levels);
            fl.pFeatureLevelsRequested = levels;
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
                logf("  D3D12 device %p max supported feature level 0x%x (creation level not queryable here)", (void*)dev, fl.MaxSupportedFeatureLevel);
            LUID luid = dev->GetAdapterLuid();
            logf("  adapter LUID %08lx:%08lx nodes=%u", luid.HighPart, luid.LowPart, dev->GetNodeCount());
            dev->Release();
        }
    } else if (dev11) {
        api = "D3D11";
        logf("  device: ID3D11Device %p feature level 0x%x", (void*)dev11, dev11->GetFeatureLevel());
    }
    if (dev11) dev11->Release();
    logf("  API=%s", api);

    DXGI_SWAP_CHAIN_DESC d = {};
    if (SUCCEEDED(sc->GetDesc(&d))) {
        logf("  desc: %ux%u %s (fmt %d) buffers=%u swap_effect=%s windowed=%d flags=0x%x refresh=%u/%u hwnd=%p",
             d.BufferDesc.Width, d.BufferDesc.Height, fmt_name(d.BufferDesc.Format), d.BufferDesc.Format, d.BufferCount,
             swap_effect_name(d.SwapEffect), d.Windowed, d.Flags, d.BufferDesc.RefreshRate.Numerator,
             d.BufferDesc.RefreshRate.Denominator, (void*)d.OutputWindow);
        if (d.OutputWindow) {
            if (!g_game_hwnd) g_game_hwnd = d.OutputWindow;
            char title[256] = {};
            GetWindowTextA(d.OutputWindow, title, sizeof(title));
            RECT r = {};
            GetClientRect(d.OutputWindow, &r);
            logf("  window: \"%s\" client %ldx%ld", title, r.right - r.left, r.bottom - r.top);
        }
    }
    IDXGISwapChain4* sc4 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain4), (void**)&sc4)) && sc4) {
        logf("  supports IDXGISwapChain4; current back buffer index %u", sc4->GetCurrentBackBufferIndex());
        sc4->Release();
    }
    BOOL fs = FALSE;
    if (SUCCEEDED(sc->GetFullscreenState(&fs, nullptr))) logf("  exclusive fullscreen=%d", fs);
}

void present_probe_hook(); // defined after the matrix scanner
void d3d_install(IDXGISwapChain* sc);
// Video memory: process usage vs the OS budget for the GPU's local memory (per STATS window; the crash log repeats it)
IDXGIAdapter3* g_adapter3 = nullptr;
HMODULE g_d3d12core = nullptr; // for the crash catcher's C++-exception filter
std::atomic<unsigned long long> g_vram_use{0}, g_vram_budget{0};
void vram_log(IDXGISwapChain* sc) {
    if (!g_d3d12core) g_d3d12core = GetModuleHandleW(L"D3D12Core.dll");
    if (!g_adapter3 && sc) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&d)) && d) {
            LUID luid = d->GetAdapterLuid();
            IDXGIFactory4* f = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&f)) && f) { f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3), (void**)&g_adapter3); f->Release(); }
            d->Release();
        }
    }
    if (!g_adapter3) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
    if (FAILED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return;
    g_vram_use = mi.CurrentUsage; g_vram_budget = mi.Budget;
    logf("VRAM local usage %.0f MB of budget %.0f MB (%.0f%%)", mi.CurrentUsage / 1048576.0, mi.Budget / 1048576.0, mi.Budget ? 100.0 * mi.CurrentUsage / mi.Budget : 0.0);
}
void d3d_log_window(double win_ms);
void d3d_debug_drain();
extern bool g_d3d_installed;

void on_present(IDXGISwapChain* sc, UINT sync, UINT flags, bool is_present1) {
    if (flags & DXGI_PRESENT_TEST) return;
    if (GetCurrentThreadId() != g_frame_thread.load()) present_probe_hook();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (std::find(g_described.begin(), g_described.end(), (void*)sc) == g_described.end()) {
        if ((int)g_described.size() < g_cfg.max_swapchains) {
            logf("hook: %s", is_present1 ? "Present1" : "Present");
            describe(sc, sync, flags);
        }
        g_described.push_back(sc);
    }
    if ((g_cfg.d3d_observe || g_cfg.d3d_inject > 0) && !g_d3d_installed) d3d_install(sc);
    d3d_debug_drain();
    LONGLONG t = qpc();
    if (g_last_present) g_intervals_ms.push_back(now_ms(t - g_last_present));
    g_last_present = t;
    ++g_presents_in_window;
    ++g_total_presents;
    DWORD tid = GetCurrentThreadId();
    if (tid == g_frame_thread.load()) ++g_presents_on_frame_thread;
    if (g_last_thread && tid != g_last_thread && g_thread_change_notes < 4) {
        ++g_thread_change_notes;
        logf("note: Present thread changed %lu -> %lu (further changes counted in STATS only)", g_last_thread, tid);
    }
    g_last_thread = tid;

    if (!g_window_start) g_window_start = t;
    double win = now_ms(t - g_window_start);
    if (win >= g_cfg.stats_ms) {
        std::vector<double> v = g_intervals_ms;
        std::sort(v.begin(), v.end());
        double mean = 0; for (double x : v) mean += x;
        mean = v.empty() ? 0 : mean / v.size();
        auto pct = [&](double p) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))]; };
        long frames = g_frames_in_window.exchange(0);
        double cap_wait_ms = g_cap_wait_us_in_window.exchange(0) / 1000.0;
        logf("STATS window=%.0fms presents=%ld (%.1f/s) on_main_thread=%ld onframe=%ld (%.1f/s) interval_ms mean=%.2f p50=%.2f p99=%.2f max=%.2f sync=%u total=%lld ticks=%lld cap_hz=%d cap_wait_ms_per_tick=%.2f",
             win, g_presents_in_window, 1000.0 * g_presents_in_window / win, g_presents_on_frame_thread, frames, 1000.0 * frames / win, mean, pct(0.5),
             pct(0.99), v.empty() ? 0.0 : v.back(), sync, g_total_presents, g_total_ticks.load(), g_cfg.tick_cap_hz,
             frames ? cap_wait_ms / frames : 0.0);
        vram_log(sc);
        d3d_log_window(win);
        g_intervals_ms.clear();
        g_presents_in_window = 0;
        g_presents_on_frame_thread = 0;
        g_window_start = t;
    }
}

PresentFn orig_present_for(void** vtbl) {
    for (auto& p : g_patched) if (p.vtbl == vtbl) return p.orig_present;
    return nullptr;
}
Present1Fn orig_present1_for(void** vtbl) {
    for (auto& p : g_patched) if (p.vtbl == vtbl) return p.orig_present1;
    return nullptr;
}

// CPU time accumulators (microseconds, count) for the timing line in STATS.
struct TAcc { std::atomic<long long> us{0}; std::atomic<long> n{0}; long long p_us = 0; long p_n = 0; };
TAcc g_t_present, g_t_inj, g_t_ecl;
long long qpc_us() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / f.QuadPart;
}
struct TScope { TAcc& a; long long t0; TScope(TAcc& x) : a(x), t0(qpc_us()) {} ~TScope() { a.us.fetch_add(qpc_us() - t0); a.n.fetch_add(1); } };

void flat_present(IDXGISwapChain* sc);
HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    on_present(sc, sync, flags, false);
    PresentFn orig = orig_present_for(*(void***)sc);
    if (!(flags & DXGI_PRESENT_TEST)) flat_present(sc);
    TScope ts(g_t_present);
    return orig ? orig(sc, sync, flags) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    on_present(sc, sync, flags, true);
    Present1Fn orig = orig_present1_for(*(void***)sc);
    if (!(flags & DXGI_PRESENT_TEST)) flat_present(sc);
    TScope ts(g_t_present);
    return orig ? orig(sc, sync, flags, pp) : DXGI_ERROR_INVALID_CALL;
}

int patch_imports(void* target, void* repl);
bool write_slot(void** vtbl, int slot, void* value) {
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    vtbl[slot] = value;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}

// ---------------------------------------------------------------------------------------------
// D3D12 command-list observer and scene-segment replay. With d3d_observe=1 (or d3d_inject>0), the first
// Present after it's set creates a throwaway allocator, a direct list and a bundle list on the game's device
// only to find the ID3D12GraphicsCommandList vtable(s) (direct and bundle tables are separate), then patches
// the recording methods listed in kHooked (indices from d3d12.h order). Hooks count (D3D stats lines),
// record main-thread scene segments when d3d_inject>0 (see the replay block below), and forward.
// cmd=d3d_dump logs the op sequence of 3 bundle-executing direct lists per thread.
using CL = ID3D12GraphicsCommandList;
using CloseFn = HRESULT(STDMETHODCALLTYPE*)(CL*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(CL*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using DrawInstFn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT, UINT);
using DrawIdxFn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT, INT, UINT);
using ExecBundleFn = void(STDMETHODCALLTYPE*)(CL*, CL*);
using RootCbvFn = void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using OmRtFn = void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearRtFn = void(STDMETHODCALLTYPE*)(CL*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
using GetTypeFn = D3D12_COMMAND_LIST_TYPE(STDMETHODCALLTYPE*)(CL*);

struct D3dVt { void** vt; void* orig[64]; };
D3dVt g_vts[2];
int g_nvts = 0;
bool g_d3d_installed = false;
std::recursive_mutex g_d3d_mutex;

struct ListRec {
    CL* list = nullptr;
    int type = -1;
    bool main = false;
    int draws = 0, bundles = 0, rts = 0, clears = 0, r2n = 0;
    int verts = 0, max_verts = 0;  // vertices (or indices) over all draws, and the largest single draw
    UINT64 vb_va = 0; UINT vb_size = 0, vb_stride = 0; // first vertex buffer bound (slot 0)
    D3D12_GPU_VIRTUAL_ADDRESS r2[8] = {};
    bool dump = false;
    std::string ops;
};
const size_t kListRecs = 8192; // bundles too (512 overflowed: post_log showed draws=-1)
ListRec g_lists[kListRecs];

struct D3dAgg { long lists = 0, draws = 0, bundles = 0, rts = 0, clears = 0, r2 = 0; };
D3dAgg g_agg[2][3]; // [main?][type: 0 direct, 1 bundle, 2 other]
bool g_dump_main = false, g_dump_other = false;
int g_dump_left_main = 0, g_dump_left_other = 0;

const D3dVt* vt_of(CL* l) {
    void** v = *(void***)l;
    for (int i = 0; i < g_nvts; ++i) if (g_vts[i].vt == v) return &g_vts[i];
    return &g_vts[0];
}
template <class F> F orig_of(CL* l, int slot) { return (F)vt_of(l)->orig[slot]; }

ListRec* rec_of(CL* l, bool create) {
    size_t h = ((uintptr_t)l >> 4) % kListRecs;
    for (size_t k = 0; k < kListRecs; ++k) {
        ListRec& r = g_lists[(h + k) % kListRecs];
        if (r.list == l) return &r;
        if (!r.list) { if (!create) return nullptr; r = ListRec{}; r.list = l; return &r; }
    }
    return nullptr;
}

void op(ListRec* r, const char* fmt, ...) {
    if (!r || !r->dump || r->ops.size() > 6000) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    r->ops += b;
    r->ops += ' ';
}

// ---------------------------------------------------------------------------------------------
// Scene-segment recorder and replay (d3d_inject). Works on main-thread direct lists only.
//   * From Reset, the list's current state is tracked (root signature, heaps, viewports, scissors, PSO,
//     topology, blend factor, stencil ref, all graphics root parameters, index/vertex buffers).
//   * The segment starts at the first OMSetRenderTargets with a depth buffer (the scene target); from then
//     on every op is recorded (copied by value).
//   * Trigger: the first ClearRenderTargetView of a render target not bound during the segment, after at
//     least d3d_inject_min_bundles bundle executes (the post stage starting). Before forwarding that clear,
//     the state captured at segment start is re-applied and the segment's ops are replayed through the
//     original methods (d3d_inject=1: into the same targets, the picture must not change).
// Fail closed: lists that don't match (no scene target, too few bundles, too many ops) are never touched.
enum OpK { O_RTS, O_CLRRT, O_CLRDS, O_DI, O_DII, O_EB, O_BAR, O_COPYRES, O_COPYTEX, O_PSO, O_TOPO, O_VP, O_SC, O_BF, O_SR,
           O_HEAPS, O_RSIG, O_CBV, O_SRV, O_UAV, O_TABLE, O_C32, O_C32N, O_IB, O_VB, O_RESOLVE };
struct Op {
    OpK k;
    UINT u[6] = {};
    INT i0 = 0;
    UINT64 q = 0;
    void* p[3] = {};
    float f[4] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE h{}, h2{};
    bool b = false;
    std::vector<uint8_t> blob; // arrays copied by value
};
struct SegState {
    ID3D12RootSignature* rsig = nullptr;
    std::vector<ID3D12DescriptorHeap*> heaps;
    std::vector<D3D12_VIEWPORT> vps;
    std::vector<D3D12_RECT> scs;
    ID3D12PipelineState* pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    float bf[4] = {};
    bool has_bf = false;
    UINT sr = 0;
    bool has_sr = false;
    std::vector<Op> roots; // last op per root parameter index (CBV/SRV/UAV/table/constants)
    D3D12_INDEX_BUFFER_VIEW ib{};
    bool has_ib = false;
    std::vector<uint8_t> vbs;
    UINT vb_start = 0, vb_n = 0;
};
struct InjRec {
    CL* list = nullptr;
    bool active = false, segment = false, done = false, failed = false;
    SegState cur, start;
    std::vector<Op> ops;
    std::vector<UINT64> rts;
    int bundles = 0;
    bool segment_fired_here = false;
    int post_n = 0;        // bundles executed after the replay fired in this list (the post stage)
    UINT64 last_rt = 0;    // last render target bound (low bits logged by post_log)
    UINT64 last_ds = 0;    // its depth target (0 = none)
    bool saw_other_rt = false; // a target other than the scene image was bound (the glow chain): later scene binds = HUD
    bool hud_drawn = false;    // HUD bundles were drawn again into our HUD image in this list
    ID3D12Resource* hud_scene = nullptr; UINT hud_n = 0; // scene image the HUD went into, and its sample count
    bool post_started = false; // a clear of a target this list hadn't used yet: the post stage began (per list, no learning)
    bool list_cam = false;     // the list itself bound the camera block (a scene list)
    bool hud_list = false;     // this list is the HUD pass taken for the latest replay
    int hud_seen = 0;          // scene-bound bundles after the glow chain so far in this list
    bool bound_scene = false;  // the scene image was bound in this list
    bool is_twin = false;  // a game bundle being mirrored into `twin` (d3d_inject>=3), one per eye
    CL* twin[2] = {};
};
InjRec g_inj[16];
void twin_emit(InjRec* r, const Op& o); // defined with the eye camera ring
std::atomic<long> g_inj_done{0}, g_inj_skipped{0}, g_inj_ops{0}, g_inj_prebarriers{0};

InjRec* inj_of(CL* l, bool create) {
    for (auto& r : g_inj) if (r.list == l) return &r;
    if (!create) return nullptr;
    for (auto& r : g_inj) if (!r.list || !r.active) { r = InjRec{}; r.list = l; return &r; }
    return nullptr;
}
bool inj_on() { return g_cfg.d3d_inject > 0; }

void set_root(SegState& s, const Op& o) {
    for (auto& r : s.roots) if (r.u[0] == o.u[0]) { r = o; return; }
    s.roots.push_back(o);
}

// Main-thread direct lists only; called with g_d3d_mutex held by the hooks.
InjRec* inj_track(CL* l) {
    if (!inj_on() || GetCurrentThreadId() != g_frame_thread.load()) return nullptr;
    InjRec* r = inj_of(l, false);
    return r && r->active && !r->done && !r->failed ? r : nullptr;
}
void inj_record(InjRec* r, Op&& o) {
    if (!r || !r->segment) return;
    if (r->is_twin) { twin_emit(r, o); return; }
    if (r->ops.size() > 6000) { r->failed = true; return; }
    r->ops.push_back(std::move(o));
}

// Emit through the original methods (never through the hooks).
void emit(const D3dVt* v, CL* l, const Op& o);
void emit_state(const D3dVt* v, CL* l, const SegState& s) {
    if (!s.heaps.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))v->orig[28])(l, (UINT)s.heaps.size(), s.heaps.data());
    if (s.rsig) ((void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*))v->orig[30])(l, s.rsig);
    if (!s.vps.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_VIEWPORT*))v->orig[21])(l, (UINT)s.vps.size(), s.vps.data());
    if (!s.scs.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RECT*))v->orig[22])(l, (UINT)s.scs.size(), s.scs.data());
    if (s.pso) ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))v->orig[25])(l, s.pso);
    if (s.topo != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED) ((void(STDMETHODCALLTYPE*)(CL*, D3D12_PRIMITIVE_TOPOLOGY))v->orig[20])(l, s.topo);
    if (s.has_bf) ((void(STDMETHODCALLTYPE*)(CL*, const FLOAT*))v->orig[23])(l, s.bf);
    if (s.has_sr) ((void(STDMETHODCALLTYPE*)(CL*, UINT))v->orig[24])(l, s.sr);
    for (auto& o : s.roots) emit(v, l, o);
    if (s.has_ib) ((void(STDMETHODCALLTYPE*)(CL*, const D3D12_INDEX_BUFFER_VIEW*))v->orig[43])(l, &s.ib);
    if (s.vb_n) ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*))v->orig[44])(l, s.vb_start, s.vb_n,
                                                                                                       (const D3D12_VERTEX_BUFFER_VIEW*)s.vbs.data());
}
void emit(const D3dVt* v, CL* l, const Op& o) {
    void* const* f = v->orig;
    switch (o.k) {
    case O_RTS: {
        auto* rt = o.u[0] ? (const D3D12_CPU_DESCRIPTOR_HANDLE*)o.blob.data() : nullptr;
        ((OmRtFn)f[46])(l, o.u[0], rt, o.b, o.u[1] ? &o.h2 : nullptr);
        break;
    }
    case O_CLRRT: ((ClearRtFn)f[48])(l, o.h, o.f, 0, nullptr); break;
    case O_CLRDS: ((void(STDMETHODCALLTYPE*)(CL*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*))f[47])(
                      l, o.h, (D3D12_CLEAR_FLAGS)o.u[0], o.f[0], (UINT8)o.u[1], 0, nullptr); break;
    case O_DI: ((DrawInstFn)f[12])(l, o.u[0], o.u[1], o.u[2], o.u[3]); break;
    case O_DII: ((DrawIdxFn)f[13])(l, o.u[0], o.u[1], o.u[2], o.i0, o.u[3]); break;
    case O_EB: ((ExecBundleFn)f[27])(l, (CL*)o.p[0]); break;
    case O_BAR: ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))f[26])(l, o.u[0], (const D3D12_RESOURCE_BARRIER*)o.blob.data()); break;
    case O_COPYRES: ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))f[17])(l, (ID3D12Resource*)o.p[0], (ID3D12Resource*)o.p[1]); break;
    case O_COPYTEX: {
        auto* d = (const D3D12_TEXTURE_COPY_LOCATION*)o.blob.data();
        const D3D12_BOX* box = o.b ? (const D3D12_BOX*)(o.blob.data() + 2 * sizeof(D3D12_TEXTURE_COPY_LOCATION)) : nullptr;
        ((void(STDMETHODCALLTYPE*)(CL*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*))f[16])(
            l, &d[0], o.u[0], o.u[1], o.u[2], &d[1], box);
        break;
    }
    case O_RESOLVE: ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT))f[19])(
                        l, (ID3D12Resource*)o.p[0], o.u[0], (ID3D12Resource*)o.p[1], o.u[1], (DXGI_FORMAT)o.u[2]); break;
    case O_PSO: ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))f[25])(l, (ID3D12PipelineState*)o.p[0]); break;
    case O_TOPO: ((void(STDMETHODCALLTYPE*)(CL*, D3D12_PRIMITIVE_TOPOLOGY))f[20])(l, (D3D12_PRIMITIVE_TOPOLOGY)o.u[0]); break;
    case O_VP: ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_VIEWPORT*))f[21])(l, o.u[0], (const D3D12_VIEWPORT*)o.blob.data()); break;
    case O_SC: ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RECT*))f[22])(l, o.u[0], (const D3D12_RECT*)o.blob.data()); break;
    case O_BF: ((void(STDMETHODCALLTYPE*)(CL*, const FLOAT*))f[23])(l, o.f); break;
    case O_SR: ((void(STDMETHODCALLTYPE*)(CL*, UINT))f[24])(l, o.u[0]); break;
    case O_HEAPS: ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))f[28])(l, o.u[0], (ID3D12DescriptorHeap* const*)o.blob.data()); break;
    case O_RSIG: ((void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*))f[30])(l, (ID3D12RootSignature*)o.p[0]); break;
    case O_CBV: ((RootCbvFn)f[38])(l, o.u[0], o.q); break;
    case O_SRV: ((RootCbvFn)f[40])(l, o.u[0], o.q); break;
    case O_UAV: ((RootCbvFn)f[42])(l, o.u[0], o.q); break;
    case O_TABLE: { D3D12_GPU_DESCRIPTOR_HANDLE g{o.q}; ((void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))f[32])(l, o.u[0], g); break; }
    case O_C32: ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT))f[34])(l, o.u[0], o.u[1], o.u[2]); break;
    case O_C32N: ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const void*, UINT))f[36])(l, o.u[0], o.u[1], o.blob.data(), o.u[2]); break;
    case O_IB: ((void(STDMETHODCALLTYPE*)(CL*, const D3D12_INDEX_BUFFER_VIEW*))f[43])(l, (const D3D12_INDEX_BUFFER_VIEW*)o.blob.data()); break;
    case O_VB: ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*))f[44])(l, o.u[0], o.u[1], (const D3D12_VERTEX_BUFFER_VIEW*)o.blob.data()); break;
    }
}

// View capture (d3d_capture=1): device CreateRenderTargetView (slot 20) / CreateDepthStencilView (slot 21)
// hooked from device creation on, so every RTV/DSV handle maps to its resource and view description.
struct ViewRec { ID3D12Resource* res = nullptr; bool has_desc = false; D3D12_RENDER_TARGET_VIEW_DESC rtv{}; D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; };
std::mutex g_view_mutex;
std::vector<std::pair<UINT64, ViewRec>> g_views;
void* g_dev_orig[64] = {};
void view_put(UINT64 h, const ViewRec& v) {
    std::lock_guard<std::mutex> lk(g_view_mutex);
    for (auto& e : g_views) if (e.first == h) { e.second = v; return; }
    g_views.push_back({h, v});
}
ID3D12Resource* rt_res(UINT64 h);
bool view_get(UINT64 h, ViewRec& out) {
    std::lock_guard<std::mutex> lk(g_view_mutex);
    for (auto& e : g_views) if (e.first == h) { out = e.second; return true; }
    return false;
}
void STDMETHODCALLTYPE hk_crtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    ViewRec v; v.res = r; v.has_desc = desc != nullptr; if (desc) v.rtv = *desc; view_put(h.ptr, v);
    ((void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE))g_dev_orig[20])(d, r, desc, h);
}
void STDMETHODCALLTYPE hk_cdsv(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    ViewRec v; v.res = r; v.has_desc = desc != nullptr; if (desc) v.dsv = *desc; view_put(h.ptr, v);
    ((void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE))g_dev_orig[21])(d, r, desc, h);
}

// Eye targets (d3d_inject>=2): two clones (0 = left, 1 = right) of the scene colour target and depth buffer,
// created once on the first replay. Fail closed: views not captured or creation failing turns the eye replay
// off (replays skipped).
struct EyeTarget {
    bool tried = false, ok = false;
    UINT64 scene_rtv = 0, scene_dsv = 0;
    ID3D12Resource* scene_rt = nullptr;
    ID3D12Resource* scene_ds = nullptr;
    ID3D12Resource* rt[2] = {};
    ID3D12Resource* ds[2] = {};
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {}, dsv[2] = {};
} g_eye, g_eye_ms[3]; // g_eye: the 1-sample scene path and the eye images everything downstream uses;
                      // g_eye_ms: clones for the game's MSAA scene paths, resolved into g_eye.rt after the replay
EyeTarget* g_es = &g_eye; // the set the current replay renders into
std::atomic<long> g_eye_copies{0}, g_eye_resolves{0};

bool eye_create(CL* l, UINT64 rtv, UINT64 dsv, EyeTarget& t = g_eye) {
    t.tried = true;
    ViewRec vr, vd;
    bool fr = view_get(rtv, vr), fd = view_get(dsv, vd);
    if (!fr || !fd || !vr.res || !vd.res) {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("EYE: scene views not captured (rtv %llx found=%d, dsv %llx found=%d): start with d3d_capture=1; eyes off",
             (unsigned long long)rtv, (int)fr, (unsigned long long)dsv, (int)fd);
        return false;
    }
    ID3D12Device* dev = nullptr;
    l->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return false;
    D3D12_RESOURCE_DESC rd = vr.res->GetDesc(), dd = vd.res->GetDesc();
    if (g_cfg.eye_w > 0 && g_cfg.eye_h > 0) { rd.Width = dd.Width = (UINT64)g_cfg.eye_w; rd.Height = dd.Height = (UINT)g_cfg.eye_h; }
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    if (FAILED(vr.res->GetHeapProperties(&hp, &hf)) || hp.Type == D3D12_HEAP_TYPE_CUSTOM) { hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; }
    HRESULT hr = S_OK;
    for (int e = 0; e < 2 && SUCCEEDED(hr); ++e) {
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource), (void**)&t.rt[e]);
        if (SUCCEEDED(hr))
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, __uuidof(ID3D12Resource), (void**)&t.ds[e]);
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 2;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HRESULT h3 = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&t.rtv_heap);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HRESULT h4 = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&t.dsv_heap);
    bool ok = SUCCEEDED(hr) && SUCCEEDED(h3) && SUCCEEDED(h4);
    if (ok) {
        UINT rinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        UINT dinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        for (int e = 0; e < 2; ++e) {
            t.rtv[e] = t.rtv_heap->GetCPUDescriptorHandleForHeapStart();
            t.rtv[e].ptr += (SIZE_T)e * rinc;
            t.dsv[e] = t.dsv_heap->GetCPUDescriptorHandleForHeapStart();
            t.dsv[e].ptr += (SIZE_T)e * dinc;
            dev->CreateRenderTargetView(t.rt[e], vr.has_desc ? &vr.rtv : nullptr, t.rtv[e]);
            dev->CreateDepthStencilView(t.ds[e], vd.has_desc ? &vd.dsv : nullptr, t.dsv[e]);
        }
        t.scene_rtv = rtv;
        t.scene_dsv = dsv;
        t.scene_rt = vr.res;
        t.scene_ds = vd.res;
    }
    dev->Release();
    t.ok = ok;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("EYE: 2 targets (%s set) %s: colour %llux%u fmt %d samples %u, depth fmt %d (hr %08lx, heaps %08lx %08lx)",
         &t == &g_eye ? "main" : "MSAA", ok ? "created" : "FAILED (eyes off)", (unsigned long long)rd.Width, rd.Height, (int)rd.Format, rd.SampleDesc.Count, (int)dd.Format, hr, h3, h4);
    return ok;
}

// Buffer capture (d3d_capture=1): device CreateCommittedResource (slot 27) / CreatePlacedResource (slot 29)
// hooked from device creation, so a GPU virtual address maps back to its buffer (and whether it's an upload
// buffer the CPU can read).
struct BufRec { UINT64 va = 0, size = 0; ID3D12Resource* res = nullptr; bool upload = false; void* cpu = nullptr; };
std::mutex g_buf_mutex;
std::vector<BufRec> g_bufs;
void buf_record(void** ppv, const D3D12_RESOURCE_DESC* d, bool upload) {
    if (!ppv || !*ppv || !d || d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    ID3D12Resource* r = nullptr;
    if (FAILED(((IUnknown*)*ppv)->QueryInterface(__uuidof(ID3D12Resource), (void**)&r)) || !r) return;
    BufRec b; b.va = r->GetGPUVirtualAddress(); b.size = d->Width; b.res = r; b.upload = upload;
    r->Release(); // not owned: the game keeps it alive; an entry is replaced when its range is reused
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    for (auto& e : g_bufs) if (e.va < b.va + b.size && b.va < e.va + e.size) { e = b; return; }
    g_bufs.push_back(b);
}
HRESULT STDMETHODCALLTYPE hk_ccr(ID3D12Device* dv, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* d,
                                 D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                              D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**))g_dev_orig[27])(dv, hp, hf, d, st, cv, riid, ppv);
    if (SUCCEEDED(hr)) buf_record(ppv, d, hp && hp->Type == D3D12_HEAP_TYPE_UPLOAD);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_cpr(ID3D12Device* dv, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
                                 const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, REFIID, void**))g_dev_orig[29])(dv, heap, off, d, st, cv, riid, ppv);
    if (SUCCEEDED(hr)) buf_record(ppv, d, heap && heap->GetDesc().Properties.Type == D3D12_HEAP_TYPE_UPLOAD);
    return hr;
}
// CPU pointer to `n` bytes at GPU address `va`, or nullptr (not an upload buffer we know).
const uint8_t* buf_cpu(UINT64 va, size_t n) {
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    for (auto& e : g_bufs) {
        if (va < e.va || va + n > e.va + e.size) continue;
        if (!e.upload) return nullptr;
        if (!e.cpu) {
            D3D12_RANGE none{0, 0};
            if (FAILED(e.res->Map(0, &none, &e.cpu))) { e.cpu = nullptr; return nullptr; }
        }
        return (const uint8_t*)e.cpu + (va - e.va);
    }
    return nullptr;
}

// Head tracking: each eye's pose relative to the recentre pose, in the game's view space (x right, y down,
// z forward; OpenXR: x right, y up, -z forward -> a 180 deg turn about x), positions in game units. The eye
// camera = game view x inverse(eye transform), as a row-vector 4x4 (g_head_minv[eye]).
struct Quat { float x, y, z, w; };
Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat qconj(Quat a) { return {-a.x, -a.y, -a.z, a.w}; }
void qrot(Quat q, const float v[3], float out[3]) {
    Quat p{v[0], v[1], v[2], 0};
    Quat r = qmul(qmul(q, p), qconj(q));
    out[0] = r.x; out[1] = r.y; out[2] = r.z;
}
void qmat(Quat q, float m[3][3]) { // column-vector rotation matrix
    float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}
bool g_head_ok = false;              // this tick's eye transforms are valid
float g_head_minv[2][16] = {};       // row-vector 4x4 per eye, applied after the game's view
bool g_head_has_ref = false;
Quat g_head_ref_q{0, 0, 0, 1};
float g_head_ref_p[3] = {};
int g_head_recenter_done = -1;
std::atomic<long> g_head_frames{0};

// From the two located views (OpenXR LOCAL space): recentre if needed, then build g_head_minv.
void head_compute(const XrView* views) {
    Quat q[2];
    float pos[2][3];
    for (int e = 0; e < 2; ++e) {
        q[e] = {views[e].pose.orientation.x, views[e].pose.orientation.y, views[e].pose.orientation.z, views[e].pose.orientation.w};
        pos[e][0] = views[e].pose.position.x; pos[e][1] = views[e].pose.position.y; pos[e][2] = views[e].pose.position.z;
    }
    if (!g_head_has_ref || g_cfg.xr_recenter_seq != g_head_recenter_done) {
        g_head_recenter_done = g_cfg.xr_recenter_seq;
        g_head_ref_q = q[0];
        for (int i = 0; i < 3; ++i) g_head_ref_p[i] = 0.5f * (pos[0][i] + pos[1][i]);
        g_head_has_ref = true;
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("XR head: recentred at (%.3f %.3f %.3f) m", g_head_ref_p[0], g_head_ref_p[1], g_head_ref_p[2]);
    }
    float yaw = -g_cfg.xr_test_yaw_deg * 3.14159265f / 180.0f; // OpenXR yaw about +y; right turn = negative
    Quat qy{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    float scale = g_cfg.units_per_m * g_cfg.world_scale;
    for (int e = 0; e < 2; ++e) {
        Quat rq = qmul(qy, qmul(qconj(g_head_ref_q), q[e]));
        float d[3] = {pos[e][0] - g_head_ref_p[0], pos[e][1] - g_head_ref_p[1], pos[e][2] - g_head_ref_p[2]};
        float rp0[3], rp[3];
        qrot(qconj(g_head_ref_q), d, rp0);
        qrot(qy, rp0, rp);
        float R[3][3];
        qmat(rq, R);
        // to game view axes: C = diag(1, -1, -1): R_g = C R C, t_g = C t * scale
        float c[3] = {1, -1, -1};
        float Rg[3][3], tg[3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) Rg[i][j] = c[i] * R[i][j] * c[j];
            tg[i] = c[i] * rp[i] * scale;
        }
        // eye point q -> camera point p = Rg q + tg; inverse (row-vector): q_row = (p_row - tg) x Rg
        float* m = g_head_minv[e];
        for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) m[i * 4 + j] = Rg[i][j]; m[i * 4 + 3] = 0; }
        for (int j = 0; j < 3; ++j) m[12 + j] = -(tg[0] * Rg[0][j] + tg[1] * Rg[1][j] + tg[2] * Rg[2][j]);
        m[15] = 1;
    }
    g_head_ok = true;
    g_head_frames.fetch_add(1);
}
// view (row-vector 4x4 at blk[0..15]) = view x g_head_minv[eye]
void head_apply(float* blk, int eye) {
    float v[16];
    memcpy(v, blk, sizeof(v));
    const float* m = g_head_minv[eye];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) blk[r * 4 + c] = v[r * 4] * m[c] + v[r * 4 + 1] * m[4 + c] + v[r * 4 + 2] * m[8 + c] + v[r * 4 + 3] * m[12 + c];
}

// Eye camera blocks: our own upload ring, mapped once. 4 regions (main-list copies left/right, twin copies
// left/right) x 3 frames x kEcamBlocks blocks x 256 bytes. Main regions advance once per eye-replay frame,
// twin regions once per tick.
struct EyeCam {
    bool tried = false, ok = false;
    ID3D12Resource* buf = nullptr;
    uint8_t* cpu = nullptr;
    UINT64 va = 0;
    int frame = 0, tframe = 0;
    int used[4] = {};
    long long ttick = -1;
} g_ecam;
std::atomic<long> g_ecam_blocks{0}, g_ecam_miss{0};
const int kEcamBlocks = 1024; // per region per frame; twins can need more than one block each

bool ecam_create(CL* l) {
    g_ecam.tried = true;
    ID3D12Device* dev = nullptr;
    l->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 4 * 3 * kEcamBlocks * 256; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT h = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&g_ecam.buf);
    D3D12_RANGE none{0, 0};
    if (SUCCEEDED(h) && SUCCEEDED(g_ecam.buf->Map(0, &none, (void**)&g_ecam.cpu))) { g_ecam.va = g_ecam.buf->GetGPUVirtualAddress(); g_ecam.ok = true; }
    dev->Release();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("EYE: camera ring %s (hr %08lx, 4 regions x 3 frames x %d blocks)", g_ecam.ok ? "created" : "FAILED", h, kEcamBlocks);
    return g_ecam.ok;
}
float eye_sign(int eye) { return eye == 0 ? -1.0f : 1.0f; }
// Half eye separation in game units: the explicit override, else ipd/2 x units per metre x world scale.
float eye_half_offset() {
    if (g_cfg.d3d_eye_offset != 0.0f) return g_cfg.d3d_eye_offset;
    return 0.5f * g_cfg.ipd_m * g_cfg.units_per_m * g_cfg.world_scale;
}
std::atomic<long> g_ecam_wvp{0}, g_ecam_r0_plain{0}, g_ecam_r0_fov{0}, g_ecam_r0_clipfx{0}, g_ecam_r0_screen{0};
float g_game_proj[16] = {};   // the game's latest plain projection (r0+80), before our eye edit
bool g_game_proj_ok = false;
// 4x4 inverse (general, row-major). Returns false if singular.
bool inv4(const float* m, float* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c) for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
        double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r * 4 + c] = (float)a[r][c + 4];
    return true;
}
void mul4(const float* a, const float* b, float* o) { // row-major a x b
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
// A perspective projection in screen units: x/y scales, centre offsets in the z row, w = z (row-vector), no rotation.
bool is_screen_projection(const float* m) {
    auto z = [](float x) { return x > -1e-4f && x < 1e-4f; };
    return z(m[1]) && z(m[2]) && z(m[3]) && z(m[4]) && z(m[6]) && z(m[7]) && z(m[12]) && z(m[13]) && z(m[15]) &&
           std::fabs(m[11] - 1.0f) < 1e-4f && std::fabs(m[0]) > 1.5f && std::fabs(m[5]) > 1.5f && !(z(m[8]) && z(m[9]));
}
bool is_identity4(const float* m) {
    for (int i = 0; i < 16; ++i) { float want = (i % 5 == 0) ? 1.0f : 0.0f; if (std::fabs(m[i] - want) > 1e-4f) return false; }
    return true;
}
// Headset FOV (from the XR thread's first located views): half-angle tangents of the symmetric union of both eyes.
std::atomic<bool> g_hmd_fov_ok{false};
float g_hmd_tan_x = 0, g_hmd_tan_y = 0;
// Projection scales the eyes render with (row-vector projection m[0], m[5]) and the layer FOV that must match.
bool eye_proj(float& xs, float& ys) {
    if (g_cfg.eye_fov == 1 && g_hmd_fov_ok.load()) {
        xs = 1.0f / (g_hmd_tan_x * g_cfg.eye_fov_scale);
        ys = 1.0f / (g_hmd_tan_y * g_cfg.eye_fov_scale);
        return true;
    }
    xs = g_cfg.xr_xs; ys = g_cfg.xr_ys;
    return false;
}
std::atomic<int> g_ecam_log_left{0}, g_ecam_log_r0{0}, g_ecam_log_eb{0};
std::atomic<long> g_eye_eb_untwinned{0};
float g_ecam_log_r0_seen[64][16]; int g_ecam_log_r0_n = 0; // cmd=ecam_log: log the next N camera blocks (before/after the eye edit)
void log_m16(const char* tag, UINT64 va, int eye, bool twin, const float* m) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("ECAM %s va=%llx eye=%d twin=%d [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.2f %.2f %.2f %.4f]", tag, va, eye, (int)twin,
         m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}
// A 4x4 at `m` is a plain perspective projection (row-vector): no rotation/translation terms, row 3 = (0,0,z,0).
bool is_plain_projection(const float* m) {
    auto z = [](float x) { return x > -1e-5f && x < 1e-5f; };
    return z(m[1]) && z(m[2]) && z(m[3]) && z(m[4]) && z(m[6]) && z(m[7]) && z(m[8]) && z(m[9]) && z(m[12]) && z(m[13]) && z(m[15]) &&
           !z(m[0]) && !z(m[5]);
}
// An eye copy of the 256-byte block at `va` for `eye` (offset = -/+ d3d_eye_offset). kind 0: camera block, the
// view matrix (row-vector, translation row at floats 12..14) gets view-space x += offset. kind 1: r0 per-draw
// block, a combined matrix at +80 gets offset * d3d_eye_wvp_scale (off by default). Returns the copy's GPU
// address, or 0 (use the original).
UINT64 ecam_copy(UINT64 va, bool twin, int kind, int eye) {
    int region = (twin ? 2 : 0) + eye;
    int& used = g_ecam.used[region];
    if (!g_ecam.ok || used >= kEcamBlocks) { g_ecam_miss.fetch_add(1); return 0; }
    const uint8_t* src = buf_cpu(va, 256);
    if (!src) { g_ecam_miss.fetch_add(1); return 0; }
    size_t off = ((size_t)region * 3 * kEcamBlocks + (size_t)(twin ? g_ecam.tframe : g_ecam.frame) * kEcamBlocks + used++) * 256;
    float d = eye_sign(eye) * eye_half_offset();
    float blk[64];
    memcpy(blk, src, 256);
    if (kind == 0) {
        bool lg = g_ecam_log_left.load() > 0 && g_ecam_log_left.fetch_sub(1) > 0;
        if (lg) log_m16("in ", va, eye, twin, blk);
        if (g_cfg.xr && g_cfg.xr_head && g_head_ok) head_apply(blk, eye); // eye poses include the IPD
        else blk[12] += d;
        if (lg) { log_m16("out", va, eye, twin, blk); if (g_head_ok) log_m16("minv", 0, eye, twin, g_head_minv[eye]); }
    }
    else if (is_plain_projection(blk + 20)) {
        g_ecam_r0_plain.fetch_add(1);
        memcpy(g_game_proj, blk + 20, 64);
        g_game_proj_ok = true;
        float xs, ys;
        if (eye_proj(xs, ys)) { // the eye's own FOV instead of the game's
            blk[20 + 0] = std::copysign(xs, blk[20 + 0]);
            blk[20 + 5] = std::copysign(ys, blk[20 + 5]);
            g_ecam_r0_fov.fetch_add(1);
        }
    }
    else {
        if (g_ecam_log_r0.load() > 0) { // distinct non-plain r0+80 matrices
            bool seen = false;
            for (int i = 0; i < g_ecam_log_r0_n && !seen; ++i) { seen = true; for (int j = 0; j < 16; ++j) if (std::fabs(g_ecam_log_r0_seen[i][j] - blk[20 + j]) > 1e-3f * (1 + std::fabs(blk[20 + j]))) { seen = false; break; } }
            if (!seen && g_ecam_log_r0_n < 64) { memcpy(g_ecam_log_r0_seen[g_ecam_log_r0_n++], blk + 20, 64); g_ecam_log_r0.fetch_sub(1); log_m16("r0+80", va, eye, twin, blk + 20); log_m16("r0+0 ", va, eye, twin, blk); }
        }
        if (g_cfg.eye_clip_fx && g_game_proj_ok && is_identity4(blk + 20)) {
            float pinv[16], peye[16], minv[16], t[16], m[16];
            memcpy(peye, g_game_proj, 64);
            float xs, ys;
            if (eye_proj(xs, ys)) { peye[0] = std::copysign(xs, peye[0]); peye[5] = std::copysign(ys, peye[5]); }
            if (g_cfg.xr && g_cfg.xr_head && g_head_ok) memcpy(minv, g_head_minv[eye], 64);
            else { memset(minv, 0, 64); minv[0] = minv[5] = minv[10] = minv[15] = 1; minv[12] = d; }
            if (g_cfg.eye_clip_fx_mode == 2) { memset(blk + 20, 0, 64); g_ecam_r0_clipfx.fetch_add(1); } // diagnostic: hide them
            else if (inv4(g_game_proj, pinv)) {
                mul4(pinv, minv, t);
                mul4(t, peye, m);
                memcpy(blk + 20, m, 64);
                g_ecam_r0_clipfx.fetch_add(1);
            }
        } else if (g_cfg.eye_clip_fx && g_game_proj_ok && is_screen_projection(blk + 20)) {
            // the game's projection in PS2 screen units (x/y scale ~275/298, centre 2048 via the z row; effects in
            // Land of Dragons, run fx4): vertices are view-space, the camera block isn't used -> apply the eye pose
            // here and scale x/y to the eye FOV (centre terms unchanged)
            float pe[16], minv[16], m[16];
            memcpy(pe, blk + 20, 64);
            float xs, ys;
            if (eye_proj(xs, ys)) { pe[0] *= std::fabs(xs / g_game_proj[0]); pe[5] *= std::fabs(ys / g_game_proj[5]); }
            if (g_cfg.xr && g_cfg.xr_head && g_head_ok) memcpy(minv, g_head_minv[eye], 64);
            else { memset(minv, 0, 64); minv[0] = minv[5] = minv[10] = minv[15] = 1; minv[12] = d; }
            mul4(minv, pe, m);
            memcpy(blk + 20, m, 64);
            g_ecam_r0_screen.fetch_add(1);
        } else blk[20 + 12] += d * g_cfg.d3d_eye_wvp_scale;
        g_ecam_wvp.fetch_add(1);
    }
    memcpy(g_ecam.cpu + off, blk, 256);
    g_ecam_blocks.fetch_add(1);
    return g_ecam.va + off;
}

// Twin bundles (d3d_inject>=3): every bundle the game records on the main thread is mirrored op for op into two
// bundles of ours (one per eye) whose r2 (camera block) and r0 bindings point at eye copies; the eye replay
// executes the eye's twin while the eye's scene target is bound. Pool per eye: 4 tick slots x kTwinsPerTick,
// each twin with its own bundle allocator (reset when its slot comes round again, 4 ticks later). Static
// bundles recorded long ago have no twin and inherit the main list's eye camera.
struct Twin { ID3D12CommandAllocator* alloc = nullptr; CL* list = nullptr; CL* game = nullptr; long long tick = -1; };
const int kTwinsPerTick = 512; // 160 overflowed by ~90 in one tick of the load_spot walk (runs head2-5, 2026-11-10); allocators are created lazily
std::atomic<int> g_twin_peak{0};   // most twins used by one eye in one tick
Twin g_twins[2][4][kTwinsPerTick];
int g_twin_used[2][4] = {};
long long g_twin_slot_tick[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
std::atomic<long> g_twin_made{0}, g_twin_used_in_replay{0}, g_twin_fail{0}, g_twin_fail_pool{0}, g_twin_fail_reset{0};

CL* twin_begin(CL* game, ID3D12PipelineState* pso, int eye) {
    long long tick = g_total_ticks.load();
    int s = (int)(tick % 4);
    if (g_twin_slot_tick[eye][s] != tick) { g_twin_slot_tick[eye][s] = tick; g_twin_used[eye][s] = 0; }
    if (g_twin_used[eye][s] >= kTwinsPerTick) { g_twin_fail_pool.fetch_add(1); return nullptr; }
    Twin& t = g_twins[eye][s][g_twin_used[eye][s]];
    if (!t.alloc) {
        ID3D12Device* dev = nullptr;
        game->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
        if (!dev) { g_twin_fail.fetch_add(1); return nullptr; }
        HRESULT h1 = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, __uuidof(ID3D12CommandAllocator), (void**)&t.alloc);
        HRESULT h2 = SUCCEEDED(h1) ? dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, t.alloc, pso, __uuidof(CL), (void**)&t.list) : E_FAIL;
        dev->Release();
        if (FAILED(h2)) { g_twin_fail.fetch_add(1); return nullptr; }
        ((HRESULT(STDMETHODCALLTYPE*)(CL*))vt_of(t.list)->orig[9])(t.list); // new lists start open
    }
    HRESULT ha = t.alloc->Reset();
    if (FAILED(ha) || FAILED(((HRESULT(STDMETHODCALLTYPE*)(CL*, ID3D12CommandAllocator*, ID3D12PipelineState*))vt_of(t.list)->orig[10])(t.list, t.alloc, pso))) {
        g_twin_fail_reset.fetch_add(1);
        return nullptr;
    }
    ++g_twin_used[eye][s];
    if (g_twin_used[eye][s] > g_twin_peak.load()) g_twin_peak = g_twin_used[eye][s];
    t.game = game;
    t.tick = tick;
    if (g_ecam.ttick != tick) { g_ecam.ttick = tick; g_ecam.tframe = (g_ecam.tframe + 1) % 3; g_ecam.used[2] = g_ecam.used[3] = 0; }
    g_twin_made.fetch_add(1);
    return t.list;
}
void twin_emit(InjRec* r, const Op& o) {
    for (int e = 0; e < 2; ++e) {
        CL* tw = r->twin[e];
        if (!tw) continue;
        if (o.k == O_CBV && (o.u[0] == 2 || o.u[0] == 0)) {
            Op c = o;
            if (UINT64 dst = ecam_copy(o.q, true, o.u[0] == 2 ? 0 : 1, e)) c.q = dst;
            emit(vt_of(tw), tw, c);
        } else emit(vt_of(tw), tw, o);
    }
}
void twin_end(InjRec* r) {
    for (int e = 0; e < 2; ++e)
        if (r->twin[e]) ((HRESULT(STDMETHODCALLTYPE*)(CL*))vt_of(r->twin[e])->orig[9])(r->twin[e]);
}
CL* twin_for(CL* game, int eye) {
    long long tick = g_total_ticks.load();
    for (int s = 0; s < 4; ++s)
        for (int i = 0; i < g_twin_used[eye][s]; ++i)
            if (g_twins[eye][s][i].game == game && g_twins[eye][s][i].tick == tick) return g_twins[eye][s][i].list;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Eye-image blit (eye_copy_fix): screen-space effects copy the finished scene image into a texture and draw it
// back. In the eye replay that copy must read the eye's image; when the eye image is larger than the copy's
// destination, a compute shader scales it into an intermediate texture of the destination's size and format,
// which is then copied into the destination. Own root signature, PSO and shader-visible heap; the game's
// descriptor heaps are re-bound afterwards by the caller.
static const char* kBlitHlsl =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "SamplerState smp : register(s0);\n"
    "cbuffer C : register(b0) { float2 inv_size; };\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    dst[id.xy] = src.SampleLevel(smp, (float2(id.xy) + 0.5) * inv_size, 0);\n"
    "}\n";
struct BlitInter { D3D12_RESOURCE_DESC desc{}; ID3D12Resource* tex = nullptr; };
struct Blit {
    bool tried = false, ok = false;
    ID3D12Device* dev = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr; // [0..1] eye SRVs, [2..3] history SRVs, [4..7] intermediate UAVs
    UINT inc = 0;
    ID3D12Resource* srv_for[4] = {};
    ID3D12Resource* hist[2] = {};   // each eye's previous image (eye size, RENDER_TARGET at rest)
    BlitInter inter[4];
} g_blit;
std::atomic<long> g_blit_done{0}, g_blit_fail{0}, g_copy_crop{0};
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
using SerializeRsFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
bool blit_init(CL* l) {
    g_blit.tried = true;
    l->GetDevice(__uuidof(ID3D12Device), (void**)&g_blit.dev);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (D3DCompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    auto ser = (SerializeRsFn)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    const char* why = nullptr;
    ID3DBlob *cs = nullptr, *err = nullptr, *rsb = nullptr;
    if (!g_blit.dev || !compile || !ser) why = "device/d3dcompiler_47/serializer missing";
    else if (FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), "kh2vr_blit", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) why = "shader compile";
    if (!why) {
        D3D12_DESCRIPTOR_RANGE rg[2] = {};
        rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors = 1;
        rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rg[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 2;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = {1, &rg[0]};
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable = {1, &rg[1]};
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rd{3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (FAILED(ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
            FAILED(g_blit.dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_blit.rs))) why = "root signature";
    }
    if (!why) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_blit.rs;
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(g_blit.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_blit.pso))) why = "compute PSO";
    }
    if (!why) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(g_blit.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_blit.heap))) why = "descriptor heap";
        else g_blit.inc = g_blit.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (cs) cs->Release();
    if (rsb) rsb->Release();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (why) logf("EYE blit: setup failed (%s%s%s); copies from the scene keep the old crop/skip behaviour", why, err ? ": " : "", err ? (const char*)err->GetBufferPointer() : "");
    else logf("EYE blit: compute downscale ready");
    if (err) err->Release();
    g_blit.ok = why == nullptr;
    return g_blit.ok;
}
D3D12_CPU_DESCRIPTOR_HANDLE blit_cpu(int i) { auto h = g_blit.heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * g_blit.inc; return h; }
D3D12_GPU_DESCRIPTOR_HANDLE blit_gpu(int i) { auto h = g_blit.heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)i * g_blit.inc; return h; }
// Records: eye image `src` (RENDER_TARGET, 1 sample) scaled into `dst` (left in COPY_DEST, as a game copy leaves
// it). Returns false if it couldn't (caller falls back).
bool blit_eye_into(const D3dVt* v, CL* l, ID3D12Resource* src, int eye, ID3D12Resource* dst) { // eye: SRV slot 0..3
    if (!g_blit.tried) blit_init(l);
    if (!g_blit.ok) return false;
    D3D12_RESOURCE_DESC dd = dst->GetDesc(), sd = src->GetDesc();
    if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.SampleDesc.Count != 1 || sd.SampleDesc.Count != 1) return false;
    int k = -1;
    for (int i = 0; i < 4; ++i) {
        auto& in = g_blit.inter[i];
        if (in.tex && in.desc.Width == dd.Width && in.desc.Height == dd.Height && in.desc.Format == dd.Format) { k = i; break; }
        if (!in.tex) {
            D3D12_RESOURCE_DESC id = dd;
            id.MipLevels = 1; id.DepthOrArraySize = 1; id.Alignment = 0;
            id.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(g_blit.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &id, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&in.tex))) return false;
            in.desc = dd;
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = dd.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_blit.dev->CreateUnorderedAccessView(in.tex, nullptr, &ud, blit_cpu(4 + i));
            k = i;
            break;
        }
    }
    if (k < 0) return false;
    if (g_blit.srv_for[eye] != src) {
        D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = sd.Format; vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        g_blit.dev->CreateShaderResourceView(src, &vd, blit_cpu(eye));
        g_blit.srv_for[eye] = src;
    }
    auto f = v->orig;
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))f[26];
    ID3D12Resource* it = g_blit.inter[k].tex;
    D3D12_RESOURCE_BARRIER b[2] = {};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bar(l, 1, b);
    ID3D12DescriptorHeap* hp = g_blit.heap;
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))f[28])(l, 1, &hp);
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*))f[29])(l, g_blit.rs);           // SetComputeRootSignature
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))f[25])(l, g_blit.pso);
    float inv[2] = {1.0f / (float)dd.Width, 1.0f / (float)dd.Height};
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const void*, UINT))f[35])(l, 0, 2, inv, 0);  // SetComputeRoot32BitConstants
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))f[31])(l, 1, blit_gpu(eye)); // SetComputeRootDescriptorTable
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))f[31])(l, 2, blit_gpu(4 + k));
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT))f[14])(l, (UINT)((dd.Width + 7) / 8), (dd.Height + 7) / 8, 1); // Dispatch
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition = {it, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    bar(l, 2, b);
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))f[17])(l, dst, it);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    bar(l, 1, &b[1]);
    g_blit_done.fetch_add(1);
    return true;
}

// Per-eye history: created on first use like the eye image.
bool hist_ensure(int eye) {
    if (g_blit.hist[eye]) return true;
    if (!g_blit.dev) return false;
    D3D12_RESOURCE_DESC d = g_eye.rt[eye]->GetDesc();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(g_blit.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource), (void**)&g_blit.hist[eye]));
}
// After the eye images are final for this frame: eye image -> history (both RENDER_TARGET at rest).
void hist_store(const D3dVt* v, CL* l) {
    if (!g_cfg.eye_history) return;
    if (!g_blit.tried) blit_init(l);
    if (!g_blit.ok) return;
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    for (int e = 0; e < 2; ++e) {
        if (!hist_ensure(e)) return;
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = {g_eye.rt[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        b[1].Transition = {g_blit.hist[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
        bar(l, 2, b);
        ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17])(l, g_blit.hist[e], g_eye.rt[e]);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        bar(l, 2, b);
    }
}
std::atomic<long> g_hist_used{0};

// Replay the segment into one eye's target: scene colour/depth handles substituted in binds and clears,
// barriers on the scene colour/depth dropped (ours stay RENDER_TARGET / DEPTH_WRITE), main-list camera blocks
// replaced by the eye's copies, bundles replaced by the eye's twins while the eye's scene target is bound.
void eye_replay(const D3dVt* v, CL* l, InjRec* r, int eye) {
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    UINT64 last_src = 0, last_dst = 0, last_src0 = 0, last_dst0 = 0;
    // viewports and scissors: scaled to the eye target while it is bound (it can be larger than the game's)
    std::vector<ID3D12DescriptorHeap*> cur_heaps = r->start.heaps;
    std::vector<ID3D12Resource*> seg_written;
    ID3D12PipelineState* cur_pso = r->start.pso;
    std::vector<D3D12_VIEWPORT> cur_vps = r->start.vps;
    std::vector<D3D12_RECT> cur_scs = r->start.scs;
    D3D12_RESOURCE_DESC gdesc = g_es->scene_rt->GetDesc(), edesc = g_es->rt[eye]->GetDesc();
    float vsx = (float)edesc.Width / (float)gdesc.Width, vsy = (float)edesc.Height / (float)gdesc.Height;
    bool scale_vp = vsx != 1.0f || vsy != 1.0f;
    auto emit_vpsc = [&](bool eye_rt) {
        if (!scale_vp) return;
        std::vector<D3D12_VIEWPORT> vp = cur_vps;
        std::vector<D3D12_RECT> sc = cur_scs;
        if (eye_rt) {
            for (auto& x : vp) { x.TopLeftX *= vsx; x.Width *= vsx; x.TopLeftY *= vsy; x.Height *= vsy; }
            for (auto& x : sc) { x.left = (LONG)(x.left * vsx); x.right = (LONG)(x.right * vsx); x.top = (LONG)(x.top * vsy); x.bottom = (LONG)(x.bottom * vsy); }
        }
        if (!vp.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_VIEWPORT*))v->orig[21])(l, (UINT)vp.size(), vp.data());
        if (!sc.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RECT*))v->orig[22])(l, (UINT)sc.size(), sc.data());
    };
    bool rt_is_eye = false; // twins only while the eye's scene target is bound: bundles drawn into the shadow
                            // target bind a light camera as r2, which must not get the eye offset (shadows came
                            // apart from the characters when it did, seen by the human, 2026-11-10)
    for (auto& o : r->ops) {
        if (o.k == O_CBV && o.u[0] == 2) {
            UINT64 dst = 0;
            if (o.q == last_src) dst = last_dst;
            else { dst = ecam_copy(o.q, false, 0, eye); if (dst) { last_src = o.q; last_dst = dst; } }
            Op c = o;
            if (dst) c.q = dst;
            emit(v, l, c);
            continue;
        }
        if (o.k == O_CBV && o.u[0] == 0 && g_cfg.eye_fov && rt_is_eye) { // per-draw block while the eye target is bound: the eye projection
            UINT64 dst = 0;
            if (o.q == last_src0) dst = last_dst0;
            else { dst = ecam_copy(o.q, false, 1, eye); if (dst) { last_src0 = o.q; last_dst0 = dst; } }
            Op c = o;
            if (dst) c.q = dst;
            emit(v, l, c);
            continue;
        }
        if (o.k == O_HEAPS) cur_heaps.assign((ID3D12DescriptorHeap* const*)o.blob.data(), (ID3D12DescriptorHeap* const*)o.blob.data() + o.u[0]);
        if (o.k == O_PSO) cur_pso = (ID3D12PipelineState*)o.p[0];
        if (o.k == O_COPYRES) seg_written.push_back((ID3D12Resource*)o.p[0]);
        if (g_cfg.eye_history && o.k == O_COPYRES && g_blit.hist[eye]) {
            // a copy from a past-frame image: same size/format as the game's scene image, not a scene image, and not
            // written earlier in this segment -> the eye's previous image instead
            ID3D12Resource* src = (ID3D12Resource*)o.p[1];
            bool written = false;
            for (size_t i = 0; i + 1 < seg_written.size(); ++i) if (seg_written[i] == src) written = true;
            D3D12_RESOURCE_DESC sd = src->GetDesc(), gd = g_eye.scene_rt->GetDesc();
            bool scene = src == g_eye.scene_rt;
            for (auto& ms : g_eye_ms) if (ms.ok && src == ms.scene_rt) scene = true;
            if (!written && !scene && sd.Width == gd.Width && sd.Height == gd.Height && sd.Format == gd.Format && sd.SampleDesc.Count == 1 &&
                blit_eye_into(v, l, g_blit.hist[eye], 2 + eye, (ID3D12Resource*)o.p[0])) {
                if (!cur_heaps.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))v->orig[28])(l, (UINT)cur_heaps.size(), cur_heaps.data());
                if (cur_pso) ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))v->orig[25])(l, cur_pso);
                g_hist_used.fetch_add(1);
                continue;
            }
        }
        if (g_cfg.eye_copy_fix && o.k == O_COPYRES && (ID3D12Resource*)o.p[1] == g_es->scene_rt && g_es == &g_eye) {
            ID3D12Resource* dst = (ID3D12Resource*)o.p[0];
            D3D12_RESOURCE_DESC dd = dst->GetDesc(), ed = g_eye.rt[eye]->GetDesc();
            bool same = dd.Width == ed.Width && dd.Height == ed.Height;
            if (same || blit_eye_into(v, l, g_eye.rt[eye], eye, dst)) {
                if (same) { // eye image in COPY_SOURCE for a plain copy (the game's own barrier on the scene image was dropped)
                    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition = {g_eye.rt[eye], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
                    bar(l, 1, &b);
                    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17])(l, dst, g_eye.rt[eye]);
                    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                    bar(l, 1, &b);
                } else { // the blit changed heaps and the PSO: put the game's back
                    if (!cur_heaps.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))v->orig[28])(l, (UINT)cur_heaps.size(), cur_heaps.data());
                    if (cur_pso) ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))v->orig[25])(l, cur_pso);
                }
                continue;
            }
            g_blit_fail.fetch_add(1);
        }
        if (g_cfg.eye_copy_fix && o.k == O_RESOLVE && (ID3D12Resource*)o.p[1] == g_es->scene_rt && g_es != &g_eye && (ID3D12Resource*)o.p[0] != g_eye.scene_rt) {
            // MSAA path: the game resolves its scene image into a texture for an effect. Resolve the eye's MSAA image
            // into the eye's 1-sample image (overwritten by the final resolve later), then scale that into the
            // destination (game left it in RESOLVE_DEST; the blit copies in COPY_DEST)
            ID3D12Resource* dst = (ID3D12Resource*)o.p[0];
            D3D12_RESOURCE_BARRIER b[2] = {};
            for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[0].Transition = {g_es->rt[eye], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE};
            b[1].Transition = {g_eye.rt[eye], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST};
            bar(l, 2, b);
            ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT))v->orig[19])(l, g_eye.rt[eye], 0, g_es->rt[eye], 0, (DXGI_FORMAT)o.u[2]);
            for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            bar(l, 2, b);
            D3D12_RESOURCE_BARRIER d{}; d.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            d.Transition = {dst, o.u[0], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_DEST};
            bar(l, 1, &d);
            bool ok = blit_eye_into(v, l, g_eye.rt[eye], eye, dst);
            std::swap(d.Transition.StateBefore, d.Transition.StateAfter);
            bar(l, 1, &d);
            if (ok) {
                if (!cur_heaps.empty()) ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))v->orig[28])(l, (UINT)cur_heaps.size(), cur_heaps.data());
                if (cur_pso) ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))v->orig[25])(l, cur_pso);
            } else g_blit_fail.fetch_add(1);
            continue;
        }
        if (scale_vp && (o.k == O_VP || o.k == O_SC)) {
            if (o.k == O_VP) cur_vps.assign((const D3D12_VIEWPORT*)o.blob.data(), (const D3D12_VIEWPORT*)o.blob.data() + o.u[0]);
            else cur_scs.assign((const D3D12_RECT*)o.blob.data(), (const D3D12_RECT*)o.blob.data() + o.u[0]);
            emit_vpsc(rt_is_eye);
            continue;
        }
        if (o.k == O_RTS || o.k == O_CLRRT || o.k == O_CLRDS) {
            bool was_eye = rt_is_eye;
            Op c = o;
            if (c.k == O_RTS) {
                auto* h = (D3D12_CPU_DESCRIPTOR_HANDLE*)c.blob.data();
                UINT nh = c.b ? (c.u[0] ? 1 : 0) : c.u[0];
                rt_is_eye = false;
                for (UINT i = 0; i < nh; ++i) if (h[i].ptr == g_es->scene_rtv || rt_res(h[i].ptr) == g_es->scene_rt) { h[i] = g_es->rtv[eye]; rt_is_eye = true; }
                if (c.u[1] && (c.h2.ptr == g_es->scene_dsv || rt_res(c.h2.ptr) == g_es->scene_ds)) c.h2 = g_es->dsv[eye];
            } else if (c.h.ptr == g_es->scene_rtv || (c.k == O_CLRRT && rt_res(c.h.ptr) == g_es->scene_rt)) c.h = g_es->rtv[eye];
            else if (c.h.ptr == g_es->scene_dsv || (c.k == O_CLRDS && rt_res(c.h.ptr) == g_es->scene_ds)) c.h = g_es->dsv[eye];
            emit(v, l, c);
            if (c.k == O_RTS && was_eye != rt_is_eye) emit_vpsc(rt_is_eye);
        } else if (o.k == O_BAR) {
            auto* b = (const D3D12_RESOURCE_BARRIER*)o.blob.data();
            std::vector<D3D12_RESOURCE_BARRIER> keep;
            for (UINT i = 0; i < o.u[0]; ++i)
                if (!(b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                      (b[i].Transition.pResource == g_es->scene_rt || b[i].Transition.pResource == g_es->scene_ds)))
                    keep.push_back(b[i]);
            if (!keep.empty()) bar(l, (UINT)keep.size(), keep.data());
        } else if (o.k == O_EB && g_cfg.d3d_inject >= 3) {
            Op c = o;
            CL* tw = rt_is_eye ? twin_for((CL*)o.p[0], eye) : nullptr;
            if (tw) { c.p[0] = tw; g_twin_used_in_replay.fetch_add(1); }
            else if (rt_is_eye) {
                g_eye_eb_untwinned.fetch_add(1);
                static CL* seen[64]; static int nseen = 0;
                CL* b = (CL*)o.p[0];
                bool known = false;
                for (int i = 0; i < nseen; ++i) if (seen[i] == b) known = true;
                if (!known && g_ecam_log_eb.load() > 0 && nseen < 64) {
                    seen[nseen++] = b; g_ecam_log_eb.fetch_sub(1);
                    ListRec* lr = rec_of(b, false);
                    const float* m = lr && lr->r2n ? (const float*)buf_cpu(lr->r2[0], 64) : nullptr;
                    std::lock_guard<std::mutex> lk(g_log_mutex);
                    logf("EYE untwinned bundle %p: rec=%d main=%d draws=%d r2n=%d r2[0]=%llx", (void*)b, lr ? 1 : 0, lr ? (int)lr->main : -1,
                         lr ? lr->draws : -1, lr ? lr->r2n : -1, lr && lr->r2n ? (unsigned long long)lr->r2[0] : 0ull);
                    if (m) logf("  r2 block [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.2f %.2f %.2f %.4f]",
                                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                }
            }
            emit(v, l, c);
        } else emit(v, l, o);
    }
}

// Copy one eye's image over the game's scene colour so the monitor shows it (d3d_eye_show 1 = left, 2 = right).
void eye_show(const D3dVt* v, CL* l, int eye) {
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    D3D12_RESOURCE_BARRIER b[2] = {};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {g_es->scene_rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
    b[1].Transition = {g_es->rt[eye], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    bar(l, 2, b);
    D3D12_RESOURCE_DESC sd = g_es->scene_rt->GetDesc(), ed = g_es->rt[eye]->GetDesc();
    if (sd.Width == ed.Width && sd.Height == ed.Height)
        ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17])(l, g_es->scene_rt, g_es->rt[eye]);
    else if (sd.SampleDesc.Count == 1 && ed.Width >= sd.Width && ed.Height >= sd.Height) { // centre crop
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = g_es->scene_rt; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = g_es->rt[eye]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{};
        box.left = (UINT)((ed.Width - sd.Width) / 2); box.top = (ed.Height - sd.Height) / 2;
        box.right = box.left + (UINT)sd.Width; box.bottom = box.top + sd.Height; box.back = 1;
        ((void(STDMETHODCALLTYPE*)(CL*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*))v->orig[16])(l, &dst, 0, 0, 0, &src, &box);
    }
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    bar(l, 2, b);
    g_eye_copies.fetch_add(1);
}

// ---------------------------------------------------------------------------------------------
// OpenXR output v0 (xr=1). Loads the active runtime's loader (SteamVR ships openxr_loader.dll; path from the
// registry's ActiveRuntime, or xr_loader), creates a session on the game's own D3D12 device and direct queue
// (XR_KHR_D3D12_enable), two swapchains matching the eye targets, and per tick (after the eye replays, on the
// main thread): xrWaitFrame/xrBeginFrame/xrLocateViews, acquire both images and record copies eye target ->
// swapchain image into the game's list. Right after the game submits that list (queue ExecuteCommandLists
// hook) the images are released and xrEndFrame submits a projection layer: each view's pose as located
// (v0: head-locked, the eyes aren't rendered with the head pose yet) and the symmetric FOV of the projection
// the eyes were rendered with (xr_xs / xr_ys must match it). Fail closed: any failed step logs and turns XR off.

struct XrState {
    bool tried = false, failed = false, running = false, ok = false;
    HMODULE loader = nullptr;
    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSwapchain sc[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageD3D12KHR> imgs[2];
    int64_t format = 0;
    uint32_t w = 0, h = 0;
    ID3D12CommandQueue* queue = nullptr;  // the game's direct queue
    ID3D12CommandQueue* xq = nullptr;     // the queue the session is bound to (ours with xr_own_queue, else the game's)
    ID3D12Fence* game_fence = nullptr;    // signalled on the game's queue after each eye-image list (xr_own_queue)
    UINT64 game_fval = 0;
    ID3D12Resource* stage[3][2] = {};     // staging copies of the eye images (COPY_SOURCE between uses)
    int stage_next = 0, pending_stage = -1;
    XrSwapchain hud_sc = XR_NULL_HANDLE;  // HUD panel
    std::vector<XrSwapchainImageD3D12KHR> hud_imgs;
    uint32_t hud_w = 0, hud_h = 0;
    ID3D12Resource* hud_stage[3] = {};    // staging copies of the HUD image (COPY_SOURCE between uses)
    XrSwapchain fx_sc = XR_NULL_HANDLE;   // screen-effects layer (head-locked, full view)
    std::vector<XrSwapchainImageD3D12KHR> fx_imgs;
    ID3D12Resource* fx_stage[3] = {};
    XrSpace view_space = XR_NULL_HANDLE;
    XrSwapchain flat_sc = XR_NULL_HANDLE;  // flat fallback screen (menus, pause, loads)
    std::vector<XrSwapchainImageD3D12KHR> flat_imgs;
    ID3D12Resource* flat_stage[3] = {};
    uint32_t flat_w = 0, flat_h = 0;     // backbuffer size
    int flat_stage_next = 0, pending_flat_stage = -1;
    CL* pending_flat = nullptr;
    int hud_stage_next = 0, pending_hud_stage = -1;
    CL* pending_hud = nullptr;
    // frame in flight
    CL* pending = nullptr;
    long long pending_tick = 0;
    bool frame_begun = false, should_render = false, acquired = false;
    XrTime display_time = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    // function pointers
    PFN_xrGetInstanceProcAddr gipa = nullptr;
    PFN_xrGetSystem GetSystem = nullptr;
    PFN_xrGetD3D12GraphicsRequirementsKHR GetReq = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrCreateReferenceSpace CreateRefSpace = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumImages = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrAcquireSwapchainImage Acquire = nullptr;
    PFN_xrWaitSwapchainImage WaitImage = nullptr;
    PFN_xrReleaseSwapchainImage Release = nullptr;
} g_xr;
std::atomic<long> g_xr_frames{0}, g_xr_ended{0}, g_xr_skipped{0}, g_xr_forced{0};
void xr_frame_submit();
void xr_thread_start();
using EclFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
EclFn g_ecl_orig = nullptr;

// Threaded XR (xr_thread=1). The XR thread owns every OpenXR frame call: per headset frame it waits, begins,
// locates the views (published for the game thread), copies the most recent eye images into the swapchains with
// its own command list on the game's queue, and ends the frame with the poses those images were rendered with
// (the compositor reprojects them to the display pose). The game thread never waits on the headset.
struct XrShared {
    std::mutex m;                              // held by the game thread while it submits an eye-image list, and by
                                               // the XR thread while it reads `img_views` and submits its copies
    XrView pose_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; // latest located views (XR thread -> game thread)
    long long pose_seq = 0;
    XrView img_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};  // poses the latest submitted eye images used
    long long img_seq = 0;                     // count of eye-image lists submitted
    // xr_own_queue: the game's list copies the eye images into staging slot `img_stage` (3 slots, rotating), so
    // the XR queue never reads the eye targets the game's next frame renders into
    int img_stage = 0;
    XrView stage_views[3][2] = {};
    UINT64 stage_fval[3] = {};
    int flat_stage = -1;                       // latest flat-frame staging slot, its fence value, and how many published
    UINT64 flat_fval[3] = {};
    long long flat_seq = 0;
    int hud_stage = -1;                        // latest HUD staging slot and its fence value
    UINT64 hud_fval[3] = {};
} g_xs;
XrView g_render_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; // game thread: poses this tick renders with
bool g_render_views_ok = false;
std::atomic<long> g_xrt_frames{0}, g_xrt_new{0}, g_xrt_nolayer{0}, g_game_imgs{0};
std::atomic<long long> g_xrt_wait_us{0};
std::atomic<long> g_xr_pending_lost{0}; // eye-image lists never seen submitted before the next one

void xr_fail(const char* what, XrResult r) {
    g_xr.failed = true;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("XR: %s failed (XrResult %d); XR off", what, (int)r);
}
std::wstring xr_loader_path() {
    if (!g_cfg.xr_loader.empty()) return g_cfg.xr_loader;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return L"";
    std::wstring p = buf;
    p = p.substr(0, p.find_last_of(L"\\/"));
    return p + L"\\bin\\win64\\openxr_loader.dll"; // SteamVR's layout
}
template <class F> bool xr_fn(const char* name, F& out) {
    out = nullptr;
    return XR_SUCCEEDED(g_xr.gipa(g_xr.inst, name, (PFN_xrVoidFunction*)&out)) && out;
}

// Main thread, inside the replay (device from the game's list; queue from the ExecuteCommandLists hook).
bool xr_init() {
    g_xr.tried = true;
    if (!g_xr.queue) { g_xr.tried = false; return false; } // try again once the queue is known
    std::wstring lp = xr_loader_path();
    g_xr.loader = lp.empty() ? nullptr : LoadLibraryW(lp.c_str());
    if (!g_xr.loader) { std::lock_guard<std::mutex> lk(g_log_mutex); logf("XR: loader not found (%ls); XR off", lp.c_str()); g_xr.failed = true; return false; }
    g_xr.gipa = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_xr.loader, "xrGetInstanceProcAddr");
    auto create_inst = (PFN_xrCreateInstance)GetProcAddress(g_xr.loader, "xrCreateInstance");
    if (!g_xr.gipa || !create_inst) { xr_fail("loader exports", XR_ERROR_FUNCTION_UNSUPPORTED); return false; }
    const char* exts[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "kh2vr");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    XrResult r = create_inst(&ci, &g_xr.inst);
    if (XR_FAILED(r)) { xr_fail("xrCreateInstance", r); return false; }
    bool fns = xr_fn("xrGetSystem", g_xr.GetSystem) && xr_fn("xrGetD3D12GraphicsRequirementsKHR", g_xr.GetReq) &&
               xr_fn("xrCreateSession", g_xr.CreateSession) && xr_fn("xrPollEvent", g_xr.PollEvent) && xr_fn("xrBeginSession", g_xr.BeginSession) &&
               xr_fn("xrEndSession", g_xr.EndSession) && xr_fn("xrCreateReferenceSpace", g_xr.CreateRefSpace) &&
               xr_fn("xrEnumerateSwapchainFormats", g_xr.EnumFormats) && xr_fn("xrCreateSwapchain", g_xr.CreateSwapchain) &&
               xr_fn("xrEnumerateSwapchainImages", g_xr.EnumImages) && xr_fn("xrWaitFrame", g_xr.WaitFrame) && xr_fn("xrBeginFrame", g_xr.BeginFrame) &&
               xr_fn("xrEndFrame", g_xr.EndFrame) && xr_fn("xrLocateViews", g_xr.LocateViews) && xr_fn("xrAcquireSwapchainImage", g_xr.Acquire) &&
               xr_fn("xrWaitSwapchainImage", g_xr.WaitImage) && xr_fn("xrReleaseSwapchainImage", g_xr.Release);
    if (!fns) { xr_fail("function lookup", XR_ERROR_FUNCTION_UNSUPPORTED); return false; }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = g_xr.GetSystem(g_xr.inst, &sgi, &g_xr.sys);
    if (XR_FAILED(r)) { xr_fail("xrGetSystem (headset on and SteamVR running?)", r); return false; }
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    r = g_xr.GetReq(g_xr.inst, g_xr.sys, &req);
    if (XR_FAILED(r)) { xr_fail("xrGetD3D12GraphicsRequirementsKHR", r); return false; }
    ID3D12Device* dev = nullptr;
    g_xr.queue->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    LUID luid = dev ? dev->GetAdapterLuid() : LUID{};
    XrGraphicsBindingD3D12KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    gb.device = dev;
    g_xr.xq = g_xr.queue;
    if (g_cfg.xr_thread && g_cfg.xr_own_queue && dev) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* oq = nullptr;
        if (SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&oq)) &&
            SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_xr.game_fence)))
            g_xr.xq = oq;
        D3D12_RESOURCE_DESC sd = g_eye.rt[0]->GetDesc();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (int k = 0; k < 3 && g_xr.xq != g_xr.queue; ++k)
            for (int e = 0; e < 2; ++e)
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&g_xr.stage[k][e]))) {
                    g_xr.xq = g_xr.queue; // fall back to the game's queue (no staging)
                }
    }
    gb.queue = g_xr.xq;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_xr.sys;
    r = g_xr.CreateSession(g_xr.inst, &sci, &g_xr.session);
    if (XR_FAILED(r)) { if (dev) dev->Release(); xr_fail("xrCreateSession", r); return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    r = g_xr.CreateRefSpace(g_xr.session, &rs, &g_xr.space);
    if (XR_FAILED(r)) { if (dev) dev->Release(); xr_fail("xrCreateReferenceSpace", r); return false; }
    // swapchains like the eye targets; sRGB view of the same bits preferred (the game's pixels are gamma-encoded)
    D3D12_RESOURCE_DESC ed = g_eye.rt[0]->GetDesc();
    uint32_t nf = 0;
    g_xr.EnumFormats(g_xr.session, 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf);
    g_xr.EnumFormats(g_xr.session, nf, &nf, fmts.data());
    int64_t want_srgb = ed.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : ed.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : 0;
    g_xr.format = 0;
    for (auto f : fmts) if (f == want_srgb) g_xr.format = f;
    if (!g_xr.format) for (auto f : fmts) if (f == (int64_t)ed.Format) g_xr.format = f;
    if (!g_xr.format) { if (dev) dev->Release(); xr_fail("no swapchain format matching the eye images", XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED); return false; }
    g_xr.w = (uint32_t)ed.Width;
    g_xr.h = ed.Height;
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = g_xr.format;
        sc.sampleCount = 1;
        sc.width = g_xr.w;
        sc.height = g_xr.h;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        r = g_xr.CreateSwapchain(g_xr.session, &sc, &g_xr.sc[e]);
        if (XR_FAILED(r)) { if (dev) dev->Release(); xr_fail("xrCreateSwapchain", r); return false; }
        uint32_t ni = 0;
        g_xr.EnumImages(g_xr.sc[e], 0, &ni, nullptr);
        g_xr.imgs[e].assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        g_xr.EnumImages(g_xr.sc[e], ni, &ni, (XrSwapchainImageBaseHeader*)g_xr.imgs[e].data());
    }
    if (g_xr.xq != g_xr.queue && dev) { // HUD panel swapchain (game scene size), only with our own queue
        D3D12_RESOURCE_DESC gd = g_eye.scene_rt->GetDesc();
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = g_xr.format; sc.sampleCount = 1; sc.width = (uint32_t)gd.Width; sc.height = gd.Height;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        if (XR_SUCCEEDED(g_xr.CreateSwapchain(g_xr.session, &sc, &g_xr.hud_sc)) && XR_SUCCEEDED(g_xr.CreateSwapchain(g_xr.session, &sc, &g_xr.fx_sc))) {
            uint32_t nf = 0;
            g_xr.EnumImages(g_xr.fx_sc, 0, &nf, nullptr);
            g_xr.fx_imgs.assign(nf, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            g_xr.EnumImages(g_xr.fx_sc, nf, &nf, (XrSwapchainImageBaseHeader*)g_xr.fx_imgs.data());
            XrReferenceSpaceCreateInfo vs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            vs.poseInReferenceSpace.orientation.w = 1.0f;
            g_xr.CreateRefSpace(g_xr.session, &vs, &g_xr.view_space);
            uint32_t ni = 0;
            g_xr.EnumImages(g_xr.hud_sc, 0, &ni, nullptr);
            g_xr.hud_imgs.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            g_xr.EnumImages(g_xr.hud_sc, ni, &ni, (XrSwapchainImageBaseHeader*)g_xr.hud_imgs.data());
            g_xr.hud_w = sc.width; g_xr.hud_h = sc.height;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            for (int k = 0; k < 3; ++k)
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &gd, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&g_xr.hud_stage[k])) ||
                    FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &gd, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&g_xr.fx_stage[k]))) { g_xr.hud_w = 0; break; }
        }
    }
    if (dev) dev->Release();
    g_xr.ok = true;
    if (g_cfg.xr_thread) xr_thread_start();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("XR: session created on the game's device (adapter LUID %08lx:%08lx, runtime wants %08lx:%08lx, min feature level 0x%x); "
         "swapchains 2 x %ux%u format %lld (%s), %zu images each",
         luid.HighPart, luid.LowPart, req.adapterLuid.HighPart, req.adapterLuid.LowPart, (int)req.minFeatureLevel, g_xr.w, g_xr.h,
         (long long)g_xr.format, g_xr.format == want_srgb ? "sRGB view" : "same format", g_xr.imgs[0].size());
    return true;
}

void xr_poll() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (g_xr.PollEvent(g_xr.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            {
                std::lock_guard<std::mutex> lk(g_log_mutex);
                logf("XR: session state %d", (int)s->state);
            }
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = g_xr.BeginSession(g_xr.session, &bi);
                g_xr.running = XR_SUCCEEDED(r);
                if (!g_xr.running) xr_fail("xrBeginSession", r);
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                g_xr.EndSession(g_xr.session);
                g_xr.running = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_xr.running = false;
                g_xr.failed = true;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Main thread, after the eye replays in the game's list `l`: begin the XR frame and record the copies.
// Start of each tick (main thread, OnFrame): end a frame left open, then wait/begin/locate and compute the eye
// transforms, so everything rendered in this tick (twins included) uses this frame's poses.
void xr_tick_begin() {
    if (g_xr.failed || !g_cfg.xr || !g_xr.ok) return;
    if (g_cfg.xr_thread) { // latest pose from the XR thread; no waiting here
        long long seq;
        {
            std::lock_guard<std::mutex> lk(g_xs.m);
            seq = g_xs.pose_seq;
            g_render_views[0] = g_xs.pose_views[0];
            g_render_views[1] = g_xs.pose_views[1];
        }
        g_render_views_ok = seq > 0;
        g_head_ok = false;
        if (g_render_views_ok) head_compute(g_render_views);
        return;
    }
    xr_poll();
    if (g_xr.frame_begun) {
        if (g_xr.pending && g_total_ticks.load() - g_xr.pending_tick <= 2) return; // copies recorded, not yet submitted
        if (g_xr.pending) g_xr_forced.fetch_add(1); // never seen submitted on the captured queue
        xr_frame_submit(); // also ends a frame that got no copies this tick (no layer)
    }
    if (!g_xr.running) return;
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrResult r = g_xr.WaitFrame(g_xr.session, &wi, &fs);
    if (XR_FAILED(r)) { xr_fail("xrWaitFrame", r); return; }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = g_xr.BeginFrame(g_xr.session, &bi);
    if (XR_FAILED(r)) { xr_fail("xrBeginFrame", r); return; }
    g_xr.frame_begun = true;
    g_xr.display_time = fs.predictedDisplayTime;
    g_xr.should_render = fs.shouldRender == XR_TRUE;
    g_xr.acquired = false;
    g_xr.pending = nullptr;
    g_head_ok = false;
    if (!g_xr.should_render) return;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = g_xr.space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    g_xr.views[0] = {XR_TYPE_VIEW};
    g_xr.views[1] = {XR_TYPE_VIEW};
    r = g_xr.LocateViews(g_xr.session, &li, &vs, 2, &nv, g_xr.views);
    if (XR_FAILED(r) || nv != 2) { g_xr.should_render = false; return; }
    if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) head_compute(g_xr.views);
}

// In the replay (main thread), after both eye replays in the game's list `l`: acquire and record the copies.
void xr_frame_record(const D3dVt* v, CL* l) {
    if (g_xr.failed || !g_cfg.xr) return;
    if (!g_xr.ok) { // session setup takes ~2 s (a 2.3 s game-thread stall when VR started, run lod2): on a thread of its own
        if (!g_xr.tried && g_xr.queue) {
            g_xr.tried = true;
            HANDLE h = CreateThread(nullptr, 0, [](void*) -> DWORD { xr_init(); return 0; }, nullptr, 0, nullptr);
            if (h) CloseHandle(h); else g_xr.tried = false;
        }
        return;
    }
    if (g_cfg.xr_thread) {
        if (!g_render_views_ok) return;
        if (g_xr.xq != g_xr.queue) { // eye targets -> staging slot, in the game's list
            int k = g_xr.stage_next;
            g_xr.stage_next = (k + 1) % 3;
            auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
            auto copy = (void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17];
            D3D12_RESOURCE_BARRIER b[4] = {};
            for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            for (int e = 0; e < 2; ++e) {
                b[2 * e].Transition = {g_eye.rt[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
                b[2 * e + 1].Transition = {g_xr.stage[k][e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
            }
            bar(l, 4, b);
            for (int e = 0; e < 2; ++e) copy(l, g_xr.stage[k][e], g_eye.rt[e]);
            for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            bar(l, 4, b);
            g_xr.pending_stage = k;
        }
        if (g_xr.pending && g_xr.pending != l) g_xr_pending_lost.fetch_add(1); // previous list never seen submitted
        g_xr.pending = l;
        g_xr.pending_tick = g_total_ticks.load();
        return;
    }
    if (!g_xr.frame_begun || !g_xr.should_render || g_xr.pending) return;
    XrResult r;
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    auto copy = (void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17];
    for (int e = 0; e < 2; ++e) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(r = g_xr.Acquire(g_xr.sc[e], &ai, &idx))) { xr_fail("xrAcquireSwapchainImage", r); return; }
        XrSwapchainImageWaitInfo wti{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wti.timeout = 100000000; // 100 ms
        if (XR_FAILED(r = g_xr.WaitImage(g_xr.sc[e], &wti))) { xr_fail("xrWaitSwapchainImage", r); return; }
        ID3D12Resource* dst = g_xr.imgs[e][idx].texture;
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = {g_eye.rt[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        b[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
        bar(l, 2, b);
        copy(l, dst, g_eye.rt[e]);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        bar(l, 2, b);
    }
    g_xr.acquired = true;
    g_xr.pending = l;
    g_xr.pending_tick = g_total_ticks.load();
    g_xr_frames.fetch_add(1);
}

// After the game's ExecuteCommandLists that contained the copies: release the images and end the frame.
void xr_frame_submit() {
    if (!g_xr.frame_begun) return;
    XrResult r = XR_SUCCESS;
    if (g_xr.acquired) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        for (int e = 0; e < 2; ++e) g_xr.Release(g_xr.sc[e], &ri);
    }
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    float ax = std::atan(1.0f / g_cfg.xr_xs), ay = std::atan(1.0f / g_cfg.xr_ys);
    for (int e = 0; e < 2; ++e) {
        pv[e].pose = g_xr.views[e].pose;
        pv[e].fov = {-ax, ax, ay, -ay};
        pv[e].subImage.swapchain = g_xr.sc[e];
        pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_xr.w, (int32_t)g_xr.h}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_xr.space;
    layer.viewCount = 2;
    layer.views = pv;
    const XrCompositionLayerBaseHeader* layers[] = {(XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_xr.display_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = g_xr.acquired ? 1 : 0;
    ei.layers = layers;
    r = g_xr.EndFrame(g_xr.session, &ei);
    g_xr.frame_begun = false;
    g_xr.acquired = false;
    g_xr.pending = nullptr;
    if (XR_FAILED(r)) xr_fail("xrEndFrame", r); else g_xr_ended.fetch_add(1);
}

// ID3D12CommandQueue::ExecuteCommandLists (slot 10): remembers the game's direct queue and ends the XR frame
// once the list holding the copies has been submitted.
void STDMETHODCALLTYPE hk_ecl(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    TScope ts(g_t_ecl);
    if (!g_xr.queue && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { q->AddRef(); g_xr.queue = q; }
    bool has_hud = false;
    if (g_xr.pending_hud && q == g_xr.queue && g_xr.game_fence)
        for (UINT i = 0; i < n; ++i) if ((void*)lists[i] == (void*)g_xr.pending_hud) has_hud = true;
    bool has_flat = false;
    if (g_xr.pending_flat && q == g_xr.queue && g_xr.game_fence)
        for (UINT i = 0; i < n; ++i) if ((void*)lists[i] == (void*)g_xr.pending_flat) has_flat = true;
    bool has = false;
    if (g_cfg.xr_thread && g_xr.pending && q == g_xr.queue)
        for (UINT i = 0; i < n; ++i) if ((void*)lists[i] == (void*)g_xr.pending) has = true;
    if (has || has_hud || has_flat) {
        std::lock_guard<std::mutex> lk(g_xs.m);
        g_ecl_orig(q, n, lists);
        if (has_flat) {
            q->Signal(g_xr.game_fence, ++g_xr.game_fval);
            g_xs.flat_stage = g_xr.pending_flat_stage;
            g_xs.flat_fval[g_xr.pending_flat_stage] = g_xr.game_fval;
            ++g_xs.flat_seq;
            g_xr.pending_flat = nullptr;
        }
        if (has_hud) { // HUD image staged in that list
            q->Signal(g_xr.game_fence, ++g_xr.game_fval);
            g_xs.hud_stage = g_xr.pending_hud_stage;
            g_xs.hud_fval[g_xr.pending_hud_stage] = g_xr.game_fval;
            g_xr.pending_hud = nullptr;
        }
        if (has) { // publish the poses the eye images were rendered with, atomically for the XR thread
            if (g_xr.xq != g_xr.queue) {
                q->Signal(g_xr.game_fence, ++g_xr.game_fval);
                int k = g_xr.pending_stage;
                if (k >= 0) {
                    g_xs.img_stage = k;
                    g_xs.stage_views[k][0] = g_render_views[0];
                    g_xs.stage_views[k][1] = g_render_views[1];
                    g_xs.stage_fval[k] = g_xr.game_fval;
                }
            }
            g_xs.img_views[0] = g_render_views[0];
            g_xs.img_views[1] = g_render_views[1];
            ++g_xs.img_seq;
            g_xr.pending = nullptr;
            g_game_imgs.fetch_add(1);
        }
        return;
    }
    g_ecl_orig(q, n, lists);
    if (g_xr.pending && q == g_xr.queue) {
        for (UINT i = 0; i < n; ++i)
            if ((void*)lists[i] == (void*)g_xr.pending) { xr_frame_submit(); break; }
    }
}

struct XrCopyCtx {
    ID3D12Device* dev = nullptr;
    ID3D12CommandAllocator* alloc[3] = {};
    CL* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 fval = 0, slot_val[3] = {};
    int slot = 0, cur = 0;
} g_xc;
bool xr_copy_init() {
    g_xr.queue->GetDevice(__uuidof(ID3D12Device), (void**)&g_xc.dev);
    if (!g_xc.dev) return false;
    for (auto& a : g_xc.alloc)
        if (FAILED(g_xc.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a))) return false;
    if (FAILED(g_xc.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_xc.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_xc.list))) return false;
    ((HRESULT(STDMETHODCALLTYPE*)(CL*))vt_of(g_xc.list)->orig[9])(g_xc.list); // our list: original methods only
    if (FAILED(g_xc.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_xc.fence))) return false;
    g_xc.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_xc.ev != nullptr;
}
// XR thread: copy both eye targets into acquired swapchain images, submitted on the game's queue right after the
// latest eye-image list (the caller holds g_xs.m). Returns false on an OpenXR failure.
int g_xc_hud_stage = -1; // HUD slot copied in the current copy list (-1 none)
int g_xc_flat_stage = -1; // flat-frame slot copied in the current copy list (-1 none)
bool g_flat_mode = false; // XR thread: showing the flat screen
bool xr_copy_submit(int stage) {
    int s = g_xc.slot;
    g_xc.slot = (s + 1) % 3;
    if (g_xc.fence->GetCompletedValue() < g_xc.slot_val[s]) { g_xc.fence->SetEventOnCompletion(g_xc.slot_val[s], g_xc.ev); WaitForSingleObject(g_xc.ev, 200); }
    const D3dVt* v = vt_of(g_xc.list);
    g_xc.alloc[s]->Reset();
    ((HRESULT(STDMETHODCALLTYPE*)(CL*, ID3D12CommandAllocator*, ID3D12PipelineState*))v->orig[10])(g_xc.list, g_xc.alloc[s], nullptr);
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    auto copy = (void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17];
    XrResult r;
    for (int e = 0; e < 2; ++e) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(r = g_xr.Acquire(g_xr.sc[e], &ai, &idx))) { xr_fail("xrAcquireSwapchainImage", r); return false; }
        XrSwapchainImageWaitInfo wti{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wti.timeout = 100000000;
        if (XR_FAILED(r = g_xr.WaitImage(g_xr.sc[e], &wti))) { xr_fail("xrWaitSwapchainImage", r); return false; }
        ID3D12Resource* dst = g_xr.imgs[e][idx].texture;
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = {g_eye.rt[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        b[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
        if (stage >= 0) { // staging slot stays COPY_SOURCE; only the swapchain image moves
            bar(g_xc.list, 1, &b[1]);
            copy(g_xc.list, dst, g_xr.stage[stage][e]);
            std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
            bar(g_xc.list, 1, &b[1]);
            continue;
        }
        bar(g_xc.list, 2, b);
        copy(g_xc.list, dst, g_eye.rt[e]);
        std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
        std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
        bar(g_xc.list, 2, b);
    }
    g_xc_flat_stage = -1;
    int fsl = g_xs.flat_stage;
    if (g_flat_mode && fsl >= 0) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wti{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wti.timeout = 100000000;
        if (XR_SUCCEEDED(g_xr.Acquire(g_xr.flat_sc, &ai, &idx)) && XR_SUCCEEDED(g_xr.WaitImage(g_xr.flat_sc, &wti))) {
            ID3D12Resource* dst = g_xr.flat_imgs[idx].texture;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
            bar(g_xc.list, 1, &b);
            copy(g_xc.list, dst, g_xr.flat_stage[fsl]);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            bar(g_xc.list, 1, &b);
            g_xc_flat_stage = fsl;
        }
    }
    g_xc_hud_stage = -1;
    int hs = g_flat_mode ? -1 : g_xs.hud_stage; // hint, like the eye slot (no HUD panel over the flat screen)
    if (g_cfg.hud && g_xr.hud_w && hs >= 0) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wti{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wti.timeout = 100000000;
        if (XR_SUCCEEDED(g_xr.Acquire(g_xr.hud_sc, &ai, &idx)) && XR_SUCCEEDED(g_xr.WaitImage(g_xr.hud_sc, &wti))) {
            ID3D12Resource* dst = g_xr.hud_imgs[idx].texture;
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
            bar(g_xc.list, 1, &b);
            copy(g_xc.list, dst, g_xr.hud_stage[hs]);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            bar(g_xc.list, 1, &b);
            g_xc_hud_stage = hs;
            uint32_t fi = 0;
            if (XR_SUCCEEDED(g_xr.Acquire(g_xr.fx_sc, &ai, &fi)) && XR_SUCCEEDED(g_xr.WaitImage(g_xr.fx_sc, &wti))) {
                ID3D12Resource* fd = g_xr.fx_imgs[fi].texture;
                b.Transition = {fd, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
                bar(g_xc.list, 1, &b);
                copy(g_xc.list, fd, g_xr.fx_stage[hs]);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                bar(g_xc.list, 1, &b);
            }
        }
    }
    ((HRESULT(STDMETHODCALLTYPE*)(CL*))v->orig[9])(g_xc.list);
    g_xc.cur = s;
    return true;
}
// Submit the recorded copies (caller holds g_xs.m, so they run right after the latest eye-image list).
void xr_copy_exec(UINT64 wait_fval) {
    ID3D12CommandList* cl[] = {(ID3D12CommandList*)g_xc.list};
    if (g_xr.xq != g_xr.queue) g_xr.xq->Wait(g_xr.game_fence, wait_fval); // after the list that filled the staging slot
    if (g_xc_hud_stage >= 0) g_xr.xq->Wait(g_xr.game_fence, g_xs.hud_fval[g_xc_hud_stage]); // and the HUD's (caller holds g_xs.m)
    if (g_xc_flat_stage >= 0) g_xr.xq->Wait(g_xr.game_fence, g_xs.flat_fval[g_xc_flat_stage]);
    g_ecl_orig(g_xr.xq, 1, cl);
    g_xc.slot_val[g_xc.cur] = ++g_xc.fval;
    g_xr.xq->Signal(g_xc.fence, g_xc.fval);
}
void xr_copy_release() {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (int e = 0; e < 2; ++e) g_xr.Release(g_xr.sc[e], &ri);
    if (g_xc_hud_stage >= 0) { g_xr.Release(g_xr.hud_sc, &ri); g_xr.Release(g_xr.fx_sc, &ri); }
    if (g_xc_flat_stage >= 0) g_xr.Release(g_xr.flat_sc, &ri);
}
DWORD WINAPI xr_thread_main(void*) {
    if (!xr_copy_init()) { xr_fail("XR thread copy setup", XR_ERROR_RUNTIME_FAILURE); return 0; }
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("XR: frame thread started (session and copies on %s)", g_xr.xq != g_xr.queue ? "our own queue, fenced after the game's eye images" : "the game's queue");
    }
    long long last_img = 0;
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    while (!g_xr.failed) { // not g_cfg.xr: a settings reload passes through its defaults
        xr_poll();
        if (!g_xr.running) { Sleep(10); continue; }
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        XrResult r = g_xr.WaitFrame(g_xr.session, &wi, &fs);
        QueryPerformanceCounter(&t1);
        g_xrt_wait_us.fetch_add((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
        if (XR_FAILED(r)) { xr_fail("xrWaitFrame", r); break; }
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(r = g_xr.BeginFrame(g_xr.session, &bi))) { xr_fail("xrBeginFrame", r); break; }
        if (fs.shouldRender) {
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = g_xr.space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t nv = 0;
            if (XR_SUCCEEDED(g_xr.LocateViews(g_xr.session, &li, &vs, 2, &nv, views)) && nv == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
                if (!g_hmd_fov_ok.load()) {
                    float tx = 0, ty = 0;
                    for (int e = 0; e < 2; ++e) {
                        const XrFovf& f = views[e].fov;
                        tx = std::max(tx, std::max(std::tan(-f.angleLeft), std::tan(f.angleRight)));
                        ty = std::max(ty, std::max(std::tan(f.angleUp), std::tan(-f.angleDown)));
                    }
                    g_hmd_tan_x = tx; g_hmd_tan_y = ty;
                    g_hmd_fov_ok = true;
                    std::lock_guard<std::mutex> lg(g_log_mutex);
                    logf("XR: headset FOV deg L(%.1f %.1f %.1f %.1f) R(%.1f %.1f %.1f %.1f) [left right up down]; symmetric union %.1f x %.1f deg",
                         views[0].fov.angleLeft * 57.29578f, views[0].fov.angleRight * 57.29578f, views[0].fov.angleUp * 57.29578f, views[0].fov.angleDown * 57.29578f,
                         views[1].fov.angleLeft * 57.29578f, views[1].fov.angleRight * 57.29578f, views[1].fov.angleUp * 57.29578f, views[1].fov.angleDown * 57.29578f,
                         2 * std::atan(tx) * 57.29578f, 2 * std::atan(ty) * 57.29578f);
                }
                std::lock_guard<std::mutex> lk(g_xs.m);
                g_xs.pose_views[0] = views[0];
                g_xs.pose_views[1] = views[1];
                ++g_xs.pose_seq;
            }
        }
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        bool layer_ok = false, copy_failed = false;
        { // flat mode: flat frames keep coming while eye images don't
            static long long last_flat_seq = 0, last_img_seq = 0;
            static int frames_since_flat = 1000, frames_since_img = 0;
            long long fsq = g_xs.flat_seq, isq = g_xs.img_seq;
            frames_since_flat = fsq != last_flat_seq ? 0 : frames_since_flat + 1;
            frames_since_img = isq != last_img_seq ? 0 : frames_since_img + 1;
            last_flat_seq = fsq; last_img_seq = isq;
            g_flat_mode = g_cfg.flat && frames_since_flat < 15; // the game side only stages flat frames after flat_after ticks without a world replay
            (void)frames_since_img;
        }
        if (fs.shouldRender && g_xs.img_seq > 0 && g_cfg.xr_copy) { // img_seq only grows; read unlocked as a hint
            // acquire, wait and record outside the lock (they can block); under it only read the poses and submit
            bool staged = g_xr.xq != g_xr.queue;
            int stage = staged ? g_xs.img_stage : -1; // hint; its poses and fence value are read under the lock
            if (xr_copy_submit(stage)) {
                {
                    std::lock_guard<std::mutex> lk(g_xs.m); // no game eye-image submission between reading the poses and our copies
                    float pxs, pys;
                    eye_proj(pxs, pys);
                    float ax = std::atan(1.0f / pxs), ay = std::atan(1.0f / pys);
                    for (int e = 0; e < 2; ++e) {
                        pv[e].pose = staged ? g_xs.stage_views[stage][e].pose : g_xs.img_views[e].pose;
                        pv[e].fov = {-ax, ax, ay, -ay};
                        pv[e].subImage.swapchain = g_xr.sc[e];
                        pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_xr.w, (int32_t)g_xr.h}};
                    }
                    xr_copy_exec(staged ? g_xs.stage_fval[stage] : 0);
                    if (g_xs.img_seq != last_img) { g_xrt_new.fetch_add(1); last_img = g_xs.img_seq; }
                }
                xr_copy_release();
                layer_ok = true;
            } else copy_failed = true;
        }
        if (copy_failed) break;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = g_xr.space;
        layer.viewCount = 2;
        layer.views = pv;
        // HUD panel: in front of and below the recentred view (yaw only), tilted toward the eyes
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        bool quad_ok = layer_ok && g_xc_hud_stage >= 0;
        if (quad_ok) {
            Quat rq = g_head_ref_q;
            float f[3], fwd[3] = {0, 0, -1};
            qrot(rq, fwd, f);
            float yaw = std::atan2(-f[0], -f[2]);
            Quat qyaw{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
            float tilt = -g_cfg.hud_tilt_deg * 3.14159265f / 180.0f;
            Quat qt{std::sin(tilt / 2), 0, 0, std::cos(tilt / 2)};
            Quat qo = qmul(qyaw, qt);
            float off[3] = {0, -g_cfg.hud_down, -g_cfg.hud_dist}, po[3];
            qrot(qyaw, off, po);
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | (g_cfg.hud_premul ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
            quad.space = g_xr.space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_xr.hud_sc;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)g_xr.hud_w, (int32_t)g_xr.hud_h}};
            quad.pose.orientation = {qo.x, qo.y, qo.z, qo.w};
            quad.pose.position = {g_head_ref_p[0] + po[0], g_head_ref_p[1] + po[1], g_head_ref_p[2] + po[2]};
            quad.size = {g_cfg.hud_width, g_cfg.hud_width * (float)g_xr.hud_h / (float)g_xr.hud_w};
        }
        XrCompositionLayerQuad fxq{XR_TYPE_COMPOSITION_LAYER_QUAD};
        bool fx_ok = quad_ok && g_cfg.hud_fx && g_xr.view_space && g_hmd_fov_ok.load();
        if (fx_ok) { // head-locked, 16:9, sized to cover the whole view at fx_dist
            float d = g_cfg.fx_dist, wv = 2 * d * g_hmd_tan_x, hv = 2 * d * g_hmd_tan_y;
            float w = std::max(wv, hv * (float)g_xr.hud_w / (float)g_xr.hud_h);
            fxq.layerFlags = quad.layerFlags;
            fxq.space = g_xr.view_space;
            fxq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            fxq.subImage.swapchain = g_xr.fx_sc;
            fxq.subImage.imageRect = quad.subImage.imageRect;
            fxq.pose.orientation = {0, 0, 0, 1};
            fxq.pose.position = {0, 0, -d};
            fxq.size = {w, w * (float)g_xr.hud_h / (float)g_xr.hud_w};
        }
        XrCompositionLayerQuad flq{XR_TYPE_COMPOSITION_LAYER_QUAD};
        bool flat_ok = layer_ok && g_xc_flat_stage >= 0;
        if (flat_ok) { // world-locked screen in front of the recentred view (yaw only), opaque
            Quat rq = g_head_ref_q;
            float f[3], fwd[3] = {0, 0, -1};
            qrot(rq, fwd, f);
            float yaw = std::atan2(-f[0], -f[2]);
            Quat qyaw{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
            float off[3] = {0, -g_cfg.flat_down, -g_cfg.flat_dist}, po[3];
            qrot(qyaw, off, po);
            flq.space = g_xr.space;
            flq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            flq.subImage.swapchain = g_xr.flat_sc;
            flq.subImage.imageRect = {{0, 0}, {(int32_t)g_xr.flat_w, (int32_t)g_xr.flat_h}};
            flq.pose.orientation = {qyaw.x, qyaw.y, qyaw.z, qyaw.w};
            flq.pose.position = {g_head_ref_p[0] + po[0], g_head_ref_p[1] + po[1], g_head_ref_p[2] + po[2]};
            flq.size = {g_cfg.flat_width, g_cfg.flat_width * (float)g_xr.flat_h / (float)g_xr.flat_w};
        }
        const XrCompositionLayerBaseHeader* layers[4];
        uint32_t nl = 0;
        layers[nl++] = (XrCompositionLayerBaseHeader*)&layer;
        if (flat_ok) layers[nl++] = (XrCompositionLayerBaseHeader*)&flq;
        if (fx_ok) layers[nl++] = (XrCompositionLayerBaseHeader*)&fxq;
        if (quad_ok) layers[nl++] = (XrCompositionLayerBaseHeader*)&quad;
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = layer_ok ? nl : 0;
        ei.layers = layers;
        if (!layer_ok) g_xrt_nolayer.fetch_add(1);
        if (XR_FAILED(r = g_xr.EndFrame(g_xr.session, &ei))) { xr_fail("xrEndFrame", r); break; }
        g_xrt_frames.fetch_add(1);
    }
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("XR: frame thread stopped (failed=%d)", (int)g_xr.failed);
    return 0;
}
void xr_thread_start() {
    HANDLE h = CreateThread(nullptr, 0, xr_thread_main, nullptr, 0, nullptr);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(h); }
    else xr_fail("CreateThread", XR_ERROR_RUNTIME_FAILURE);
}

// Skip reasons: 0 no bundles, 1 fewer than d3d_inject_min_bundles (1..min-1), 2 waiting to lock the eye targets,
// 3 segment on another target than the eye scene target, 4 segment closed without reaching the post stage
std::atomic<long> g_skip[5];
std::atomic<long> g_inj_at_close{0};
std::atomic<long> g_seg_samples[5]; // scene segments by the game's sample count: other, 1, 2, 4, 8

void log_skip_target(UINT64 srtv, UINT64 sdsv, int bundles) {
    static UINT64 seen[16]; static int n = 0;
    for (int i = 0; i < n; ++i) if (seen[i] == srtv) return;
    if (n >= 16) return;
    seen[n++] = srtv;
    ViewRec vr;
    D3D12_RESOURCE_DESC d{};
    if (view_get(srtv, vr) && vr.res) d = vr.res->GetDesc();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("EYE skip: segment target rtv %llx (dsv %llx) %llux%u fmt %d samples %u, %d bundles; eye scene target is %llx",
         (unsigned long long)srtv, (unsigned long long)sdsv, (unsigned long long)d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, bundles,
         (unsigned long long)g_eye.scene_rtv);
}

std::atomic<long long> g_last_fire_tick{-1};
std::atomic<long long> g_fire_seq{0}, g_hud_list_fire{-1};
// Render targets used inside replayed scene segments before the replay (shadow passes): a scene list binding the scene
// image again after them isn't a HUD stage (the glow chain's targets only appear after the replay point; run tick2).
UINT64 g_segment_rts[32]; int g_n_segment_rts = 0;
bool g_firing_at_close = false;
// The glow chain renders into downscaled targets with the scene's aspect (640x360 seen); shadow maps have another
// shape. "Scene image bound again after a glow target" marks the HUD stage in every frame layout seen (runs tick2-6).
// The resource behind a render-target view handle (the game uses more than one handle for its scene image; comparing
// handles froze the HUD, run tick7)
ID3D12Resource* rt_res(UINT64 h) { // uncached: the game re-points handles at other resources
    ViewRec vr;
    return view_get(h, vr) ? vr.res : nullptr;
}
// Sample count of the scene image behind h (1 = the main scene image, n = an MSAA set's), 0 if it isn't one.
UINT scene_samples(UINT64 h, ID3D12Resource** res = nullptr, ID3D12Resource** ds = nullptr) {
    ID3D12Resource* r = rt_res(h);
    if (h == g_eye.scene_rtv || (g_eye.scene_rt && r == g_eye.scene_rt)) { if (res) *res = g_eye.scene_rt; if (ds) *ds = g_eye.scene_ds; return 1; }
    for (auto& ms : g_eye_ms)
        if (ms.ok && (h == ms.scene_rtv || r == ms.scene_rt)) { if (res) *res = ms.scene_rt; if (ds) *ds = ms.scene_ds; return ms.scene_rt->GetDesc().SampleDesc.Count; }
    return 0;
}
bool rt_is_scene(UINT64 h) { return scene_samples(h) != 0; }
// A scene-sized target (any sample count): the HUD can be drawn into the scene image or, in some frame layouts, into
// another full-size image the post stage composites into (run hudms1dbg). Returns its sample count, 0 if not one.
UINT hud_target(UINT64 h, ID3D12Resource** res) {
    ID3D12Resource* r = rt_res(h);
    if (!r || !g_eye.scene_rt) return 0;
    D3D12_RESOURCE_DESC d = r->GetDesc(), sd = g_eye.scene_rt->GetDesc();
    if (d.Width != sd.Width || d.Height != sd.Height || d.Format != sd.Format) return 0;
    if (res) *res = r;
    return d.SampleDesc.Count;
}
bool rt_is_glow(UINT64 h) {
    static UINT64 seen[64]; static bool val[64]; static int n = 0;
    for (int i = 0; i < n; ++i) if (seen[i] == h) return val[i];
    ViewRec vr;
    bool glow = false;
    D3D12_RESOURCE_DESC d{};
    if (view_get(h, vr) && vr.res && g_eye.scene_rt) {
        d = vr.res->GetDesc();
        D3D12_RESOURCE_DESC sd = g_eye.scene_rt->GetDesc();
        double a = (double)d.Width / d.Height, sa = (double)sd.Width / sd.Height;
        glow = std::fabs(a - sa) < 0.02 * sa && d.Width < sd.Width;
    }
    if (n < 64) { seen[n] = h; val[n] = glow; ++n;
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("RT kind: %llx %llux%u fmt %d -> %s", (unsigned long long)h, (unsigned long long)d.Width, d.Height, (int)d.Format, glow ? "glow (downscaled 16:9)" : "other"); }
    return glow;
}
bool is_segment_rt(UINT64 h) { for (int i = 0; i < g_n_segment_rts; ++i) if (g_segment_rts[i] == h) return true; return false; }
std::atomic<int> g_ticklog_left{0};
std::string g_ticklog_buf; // under g_d3d_mutex // eye replays so far (the HUD stage belongs to the latest one)
std::atomic<long> g_flat_frames{0}, g_inj_nonworld{0};
std::atomic<long> g_np_lists{0}, g_np_scene_lists{0}; // lists closed while 'not gameplay', and those that bound the scene image
std::atomic<int> g_notplay_ticks{0}; // consecutive ticks the gameplay flag read 'not gameplay' (-1: no flag)
uintptr_t parse_addr(const std::wstring& a);
bool safe_copy(void* dst, const void* src, size_t n);
void flat_flag_tick() { // main thread, once per tick
    static std::wstring last; static uintptr_t a = 0;
    if (g_cfg.flat_flag != last) { last = g_cfg.flat_flag; a = last.empty() ? 0 : parse_addr(last); }
    uint8_t v = 0;
    if (!a || !safe_copy(&v, (void*)a, 1)) { g_notplay_ticks = -1; return; }
    if (v == (uint8_t)g_cfg.flat_flag_play) g_notplay_ticks = 0; else if (g_notplay_ticks < 1000000) ++g_notplay_ticks;
}
// Flat screen source: while the gameplay flag says 'not gameplay' (menus, pause, title, loading), copy the finished
// backbuffer right before Present with a small list of ours on the game's queue, and publish it like the HUD.
struct FlatCap { ID3D12CommandAllocator* alloc[3] = {}; CL* list = nullptr; ID3D12Fence* fence = nullptr; HANDLE ev = nullptr; UINT64 val = 0, slot_val[3] = {}; int slot = 0; bool tried = false, ok = false; } g_fc;
void flat_present(IDXGISwapChain* sc) {
    if (!g_cfg.flat || !g_xr.ok || !g_xr.queue || !g_xr.game_fence || g_xr.xq == g_xr.queue) return;
    if (GetCurrentThreadId() != g_frame_thread.load()) return; // main-thread Presents only (g_fc isn't shared)
    int np = g_notplay_ticks.load();
    // also while the flag says gameplay but no eye image was made for flat_noreplay ticks: sequences drawn without
    // the camera block on the list (summons, run mm2: 300 'non-world' skips, the headset kept the fade-to-black frame)
    bool no_eyes = g_cfg.flat_noreplay > 0 && g_total_ticks.load() - g_last_fire_tick.load() >= g_cfg.flat_noreplay;
    if (np < g_cfg.flat_after && !no_eyes) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) return;
    ID3D12Resource* bb = nullptr;
    sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&bb);
    sc3->Release();
    if (!bb) return;
    ID3D12Device* dev = nullptr;
    bb->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (!g_fc.tried && dev) {
        g_fc.tried = true;
        bool ok = true;
        for (auto& a : g_fc.alloc) ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a));
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_fc.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_fc.list));
        if (ok) ((HRESULT(STDMETHODCALLTYPE*)(CL*))vt_of(g_fc.list)->orig[9])(g_fc.list);
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fc.fence));
        g_fc.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_RESOURCE_DESC sd = bd; sd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (auto& st : g_xr.flat_stage) ok = ok && SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&st));
        XrSwapchainCreateInfo xc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        xc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        xc.format = bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : (int64_t)bd.Format;
        xc.sampleCount = 1; xc.width = (uint32_t)bd.Width; xc.height = bd.Height; xc.faceCount = 1; xc.arraySize = 1; xc.mipCount = 1;
        XrSwapchain fsc = XR_NULL_HANDLE;
        if (ok && XR_SUCCEEDED(g_xr.CreateSwapchain(g_xr.session, &xc, &fsc))) {
            uint32_t nq = 0;
            g_xr.EnumImages(fsc, 0, &nq, nullptr);
            g_xr.flat_imgs.assign(nq, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            g_xr.EnumImages(fsc, nq, &nq, (XrSwapchainImageBaseHeader*)g_xr.flat_imgs.data());
            g_xr.flat_w = xc.width; g_xr.flat_h = xc.height;
            g_xr.flat_sc = fsc;
        } else ok = false;
        g_fc.ok = ok;
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("FLAT: capture %s (backbuffer %llux%u fmt %d)", ok ? "ready" : "FAILED", (unsigned long long)bd.Width, bd.Height, (int)bd.Format);
    }
    if (dev) dev->Release();
    if (!g_fc.ok) { bb->Release(); return; }
    int s = g_fc.slot;
    g_fc.slot = (s + 1) % 3;
    if (g_fc.fence->GetCompletedValue() < g_fc.slot_val[s]) { g_fc.fence->SetEventOnCompletion(g_fc.slot_val[s], g_fc.ev); WaitForSingleObject(g_fc.ev, 100); }
    const D3dVt* v = vt_of(g_fc.list);
    g_fc.alloc[s]->Reset();
    ((HRESULT(STDMETHODCALLTYPE*)(CL*, ID3D12CommandAllocator*, ID3D12PipelineState*))v->orig[10])(g_fc.list, g_fc.alloc[s], nullptr);
    int k = g_xr.flat_stage_next;
    g_xr.flat_stage_next = (k + 1) % 3;
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {g_xr.flat_stage[k], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26])(g_fc.list, 1, &b);
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17])(g_fc.list, g_xr.flat_stage[k], bb); // backbuffer: PRESENT (COMMON) promotes to COPY_SOURCE
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26])(g_fc.list, 1, &b);
    ((HRESULT(STDMETHODCALLTYPE*)(CL*))v->orig[9])(g_fc.list);
    {
        std::lock_guard<std::mutex> lk(g_xs.m);
        ID3D12CommandList* cl[] = {(ID3D12CommandList*)g_fc.list};
        g_ecl_orig(g_xr.queue, 1, cl);
        g_fc.slot_val[s] = ++g_fc.val;
        g_xr.queue->Signal(g_fc.fence, g_fc.val);
        g_xr.queue->Signal(g_xr.game_fence, ++g_xr.game_fval);
        g_xs.flat_stage = k;
        g_xs.flat_fval[k] = g_xr.game_fval;
        ++g_xs.flat_seq;
    }
    bb->Release();
    g_flat_frames.fetch_add(1);
}
void inj_fire(const D3dVt* v, CL* l, InjRec* r) {
    TScope ts(g_t_inj);
    r->done = true;
    { // replay signature log: logged when it changes (bundles, list-level r2 binds, bundles binding r2 inside, RT binds, clears)
        int r2l = 0, r2b = 0, rts = 0, clr = 0, r2s = 0;
        for (auto& o : r->start.roots) if (o.k == O_CBV && o.u[0] == 2) ++r2s;
        for (auto& o : r->ops) {
            if (o.k == O_CBV && o.u[0] == 2) ++r2l;
            else if (o.k == O_EB) { ListRec* br = rec_of((CL*)o.p[0], false); if (br && br->r2n) ++r2b; }
            else if (o.k == O_RTS) ++rts;
            else if (o.k == O_CLRRT) ++clr;
        }
        static long long last_sig = -1;
        long long sig = ((long long)r->bundles << 40) ^ ((long long)r2l << 30) ^ ((long long)r2b << 20) ^ ((long long)rts << 10) ^ clr ^ ((long long)r2s << 50);
        if (sig != last_sig) {
            last_sig = sig;
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("INJ sig: bundles=%d r2_on_list=%d r2_in_start_state=%d bundles_with_r2=%d rt_binds=%d clears=%d", r->bundles, r2l, r2s, r2b, rts, clr);
        }
        // a world frame binds the camera block on the list (in the segment or the state it starts from); menus
        // only inside their model bundles (run flat3): no eye replay then, the flat screen takes over
        if (g_cfg.skip_nonworld && r2l == 0 && r2s == 0) { g_inj_nonworld.fetch_add(1); g_inj_skipped.fetch_add(1); return; }
    }
    if ((int)r->bundles < g_cfg.d3d_inject_min_bundles || r->ops.empty()) { g_inj_skipped.fetch_add(1); g_skip[r->bundles ? 1 : 0].fetch_add(1); return; }
    bool to_eye = false;
    if (g_cfg.d3d_inject >= 2) {
        // scene target = the first render-target bind with depth that started the segment
        UINT64 srtv = 0, sdsv = 0;
        for (auto& o : r->ops)
            if (o.k == O_RTS && o.u[0] == 1 && o.u[1]) { srtv = ((const D3D12_CPU_DESCRIPTOR_HANDLE*)o.blob.data())->ptr; sdsv = o.h2.ptr; break; }
        { // the game's own MSAA choice per scene segment (it varies it at runtime)
            ViewRec sv;
            UINT n = view_get(srtv, sv) && sv.res ? sv.res->GetDesc().SampleDesc.Count : 0;
            g_seg_samples[n == 1 ? 1 : n == 2 ? 2 : n == 4 ? 3 : n == 8 ? 4 : 0].fetch_add(1);
        }
        if (!g_eye.tried) {
            // Lock onto the gameplay scene target: the same target for d3d_eye_lock_segments segments in a row,
            // 1-sample unless d3d_eye_single_sample=0. Until then, no replay.
            static UINT64 cand_rtv = 0, cand_dsv = 0;
            static int cand_n = 0;
            static long rejected_msaa = 0;
            if (srtv == cand_rtv && sdsv == cand_dsv) ++cand_n; else { cand_rtv = srtv; cand_dsv = sdsv; cand_n = 1; }
            ViewRec cv;
            bool msaa = g_cfg.d3d_eye_single_sample && view_get(srtv, cv) && cv.res && cv.res->GetDesc().SampleDesc.Count != 1;
            if (msaa && rejected_msaa++ == 0) { std::lock_guard<std::mutex> lk(g_log_mutex); logf("EYE: segment target %llx is multisampled: not locking eyes onto it", (unsigned long long)srtv); }
            if (msaa || cand_n < g_cfg.d3d_eye_lock_segments) { g_inj_skipped.fetch_add(1); g_skip[2].fetch_add(1); return; }
            eye_create(l, srtv, sdsv);
        }
        if (!g_ecam.tried) ecam_create(l);
        g_es = nullptr;
        if (g_eye.ok && (srtv == g_eye.scene_rtv || rt_res(srtv) == g_eye.scene_rt) && (sdsv == g_eye.scene_dsv || rt_res(sdsv) == g_eye.scene_ds)) g_es = &g_eye;
        else if (g_eye.ok && g_cfg.d3d_eye_msaa) {
            // the game has several MSAA scene targets (2x and 8x seen); one eye set per target, created on first use
            for (auto& ms : g_eye_ms) if (ms.ok && srtv == ms.scene_rtv && sdsv == ms.scene_dsv) g_es = &ms;
            if (!g_es) {
                EyeTarget* fresh = nullptr;
                for (auto& ms : g_eye_ms) if (!ms.tried) { fresh = &ms; break; }
                ViewRec cv;
                if (fresh && view_get(srtv, cv) && cv.res) {
                    D3D12_RESOURCE_DESC a = cv.res->GetDesc(), b = g_eye.scene_rt->GetDesc(); // the game's 1-sample target (eye images can be larger)
                    if (a.SampleDesc.Count > 1 && a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && eye_create(l, srtv, sdsv, *fresh))
                        g_es = fresh;
                }
            }
        }
        to_eye = g_es && g_ecam.ok;
        if (!g_es) g_es = &g_eye;
        if (!to_eye) { g_inj_skipped.fetch_add(1); g_skip[3].fetch_add(1); if (g_eye.ok) log_skip_target(srtv, sdsv, (int)r->bundles); return; }
    }
    // The segment isn't balanced (the shadow target starts as RENDER_TARGET and ends as PIXEL_SHADER_RESOURCE,
    // debug layer ids 527/538, 2026-11-10): before each replay, put every resource the segment transitions back
    // into the state it had at the segment's start (StateBefore of its first transition). Each replay then ends
    // in the same end state the game's next commands expect.
    struct ResState { ID3D12Resource* res; UINT sub; D3D12_RESOURCE_STATES first_before, last_after; };
    std::vector<ResState> rs;
    for (auto& o : r->ops) {
        if (o.k != O_BAR) continue;
        auto* b = (const D3D12_RESOURCE_BARRIER*)o.blob.data();
        for (UINT i = 0; i < o.u[0]; ++i) {
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
            const auto& t = b[i].Transition;
            if (to_eye && (t.pResource == g_es->scene_rt || t.pResource == g_es->scene_ds)) continue; // ours start in place
            ResState* e = nullptr;
            for (auto& x : rs) if (x.res == t.pResource && x.sub == t.Subresource) e = &x;
            if (!e) rs.push_back({t.pResource, t.Subresource, t.StateBefore, t.StateAfter});
            else e->last_after = t.StateAfter;
        }
    }
    std::vector<D3D12_RESOURCE_BARRIER> pre;
    for (auto& x : rs) {
        if (x.last_after == x.first_before) continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = x.res;
        b.Transition.Subresource = x.sub;
        b.Transition.StateBefore = x.last_after;
        b.Transition.StateAfter = x.first_before;
        pre.push_back(b);
    }
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    if (!to_eye) {
        if (!pre.empty()) bar(l, (UINT)pre.size(), pre.data());
        g_inj_prebarriers.fetch_add((long)pre.size());
        emit_state(v, l, r->start);
        for (auto& o : r->ops) emit(v, l, o);
    } else {
        g_ecam.frame = (g_ecam.frame + 1) % 3;
        g_ecam.used[0] = g_ecam.used[1] = 0;
        for (int eye = 0; eye < 2; ++eye) {
            if (!pre.empty()) bar(l, (UINT)pre.size(), pre.data());
            g_inj_prebarriers.fetch_add((long)pre.size());
            emit_state(v, l, r->start);
            eye_replay(v, l, r, eye);
        }
        if (g_es != &g_eye) { // MSAA eye images -> the 1-sample eye images
            auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
            auto resolve = (void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT))v->orig[19];
            DXGI_FORMAT f = g_eye.rt[0]->GetDesc().Format;
            D3D12_RESOURCE_BARRIER b[4] = {};
            for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            for (int e = 0; e < 2; ++e) {
                b[2 * e].Transition = {g_es->rt[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE};
                b[2 * e + 1].Transition = {g_eye.rt[e], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST};
            }
            bar(l, 4, b);
            for (int e = 0; e < 2; ++e) resolve(l, g_eye.rt[e], 0, g_es->rt[e], 0, f);
            for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
            bar(l, 4, b);
            g_eye_resolves.fetch_add(1);
        }
        hist_store(v, l);
        xr_frame_record(v, l);
        if (g_cfg.d3d_eye_show == 1 || g_cfg.d3d_eye_show == 2) eye_show(v, l, g_cfg.d3d_eye_show - 1);
    }
    // Re-apply the state the game had right before this clear (= the segment's end state, already set by the
    // replayed ops; the tracked current state covers anything the replay left different).
    emit_state(v, l, r->cur);
    g_inj_done.fetch_add(1);
    g_last_fire_tick = g_total_ticks.load();
    g_fire_seq.fetch_add(1);

    g_inj_ops.fetch_add((long)r->ops.size());
}

template <class T> std::vector<uint8_t> bytes_of(const T* p, size_t n) {
    std::vector<uint8_t> b(sizeof(T) * n);
    if (n && p) memcpy(b.data(), p, b.size());
    return b;
}


// t_in_hook: set for the whole duration of a hook (bookkeeping and forward). Nested calls on the same thread
// (the debug layer can call back through the same vtable; the replay emits through the originals) only
// forward. g_d3d_mutex is recursive so a nested lock can't deadlock (a deadlock here left the GPU waiting
// until it timed out, 2026-11-10).
thread_local int t_in_hook = 0;
struct HookScope { bool outer; HookScope() : outer(t_in_hook == 0) { ++t_in_hook; } ~HookScope() { --t_in_hook; } };
HRESULT STDMETHODCALLTYPE hk_reset(CL* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    HookScope guard;
    if (guard.outer) {
        std::lock_guard<std::recursive_mutex> lk(g_d3d_mutex);
        int type = (int)orig_of<GetTypeFn>(l, 8)(l);
        bool main = GetCurrentThreadId() == g_frame_thread.load();
        if (ListRec* r = rec_of(l, true)) {
            *r = ListRec{};
            r->list = l;
            r->type = type;
            r->main = main;
            r->dump = type == D3D12_COMMAND_LIST_TYPE_DIRECT && (main ? g_dump_main : g_dump_other);
        }
        if (inj_on() && main && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            if (InjRec* ir = inj_of(l, true)) { *ir = InjRec{}; ir->list = l; ir->active = true; ir->cur.pso = p; }
        }
        if (g_cfg.d3d_inject >= 3 && g_ecam.ok && main && type == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
            if (InjRec* ir = inj_of(l, true)) {
                *ir = InjRec{};
                ir->list = l;
                ir->active = true;
                ir->segment = true;
                ir->is_twin = true;
                ir->twin[0] = twin_begin(l, p, 0);
                ir->twin[1] = twin_begin(l, p, 1);
                if (!ir->twin[0] && !ir->twin[1]) *ir = InjRec{};
            }
        }
    }
    return orig_of<ResetFn>(l, 10)(l, a, p);
}

// HUD image: the game's own HUD bundles executed a second time with our transparent target bound.
// A bundle whose first vertices span (nearly) the whole screen: positions are the game's screen units, the full
// screen being 2560 x 1440 of them (1792..4352, 1840..3280; run hud10).
std::atomic<long> g_hud_skipped_fs{0};
bool bundle_bounds(CL* bundle, float* bb) { // x0 y0 x1 y1 over all vertices of the bundle's first vertex buffer
    ListRec* b = rec_of(bundle, false);
    if (!b || !b->vb_va || b->vb_stride < 8) return false;
    UINT nv = std::min<UINT>(4096, b->vb_size / b->vb_stride);
    const uint8_t* vp = buf_cpu(b->vb_va, (size_t)nv * b->vb_stride);
    if (!vp || nv < 3) return false;
    bb[0] = bb[1] = 1e30f; bb[2] = bb[3] = -1e30f;
    for (UINT i = 0; i < nv; ++i) { const float* f = (const float*)(vp + (size_t)i * b->vb_stride); bb[0] = std::min(bb[0], f[0]); bb[2] = std::max(bb[2], f[0]); bb[1] = std::min(bb[1], f[1]); bb[3] = std::max(bb[3], f[1]); }
    return true;
}
// cmd=big_log: log HUD-stage bundles covering >=15% of the screen, once each (bounds, draws, vertices, routing)
std::atomic<int> g_biglog_left{0};
void big_log(CL* bundle, int idx, const char* route) {
    if (g_biglog_left.load() <= 0) return;
    static long long seen[64]; static int n = 0; // by shape: vertex count + rounded bounds (bundles are re-recorded each frame)
    float bb[4];
    ListRec* b = rec_of(bundle, false);
    bool ok = bundle_bounds(bundle, bb);
    if (ok && (bb[2] - bb[0]) * (bb[3] - bb[1]) < 0.15f * 2560.0f * 1440.0f) return;
    long long sig = (long long)(b ? b->verts : -1) * 1000003LL + (ok ? (long long)(bb[0] / 64) * 7919 + (long long)(bb[1] / 64) * 104729 + (long long)(bb[2] / 64) * 31 + (long long)(bb[3] / 64) : 0) + idx * 17;
    for (int i = 0; i < n; ++i) if (seen[i] == sig) return;
    if (n < 64) seen[n++] = sig;
    g_biglog_left.fetch_sub(1);
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("BIG hud-stage bundle %p #%d -> %s: draws=%d verts=%d vb=%u/%u bounds %s(%.0f %.0f)-(%.0f %.0f)", (void*)bundle, idx, route, b ? b->draws : -1, b ? b->verts : -1,
         b ? b->vb_size : 0, b ? b->vb_stride : 0, ok ? "" : "n/a ", ok ? bb[0] : 0, ok ? bb[1] : 0, ok ? bb[2] : 0, ok ? bb[3] : 0);
}
bool bundle_fullscreen(CL* bundle) {
    ListRec* b = rec_of(bundle, false);
    if (!b || !b->vb_va || b->vb_stride < 8) return false;
    UINT nv = std::min<UINT>(4096, b->vb_size / b->vb_stride); // all vertices: shattered-screen effects are many small quads
    const uint8_t* vp = buf_cpu(b->vb_va, (size_t)nv * b->vb_stride);
    if (!vp || nv < 3) return false;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (UINT i = 0; i < nv; ++i) { const float* f = (const float*)(vp + (size_t)i * b->vb_stride); x0 = std::min(x0, f[0]); x1 = std::max(x1, f[0]); y0 = std::min(y0, f[1]); y1 = std::max(y1, f[1]); }
    return (x1 - x0) >= 0.9f * 2560.0f && (y1 - y0) >= 0.9f * 1440.0f;
}
struct HudImg {
    bool tried = false, ok = false;
    ID3D12Resource* rt[2] = {};    // the HUD drawn over opaque black (0) and opaque white (1)
    ID3D12Resource* ds[2] = {};
    ID3D12Resource* out = nullptr; // matte result: straight colour + alpha (UAV)
    ID3D12Resource* pre = nullptr; // the scene image right before the HUD (after the glow composite)
    ID3D12Resource* fin = nullptr; // the finished scene image (HUD drawn), 1 sample (copied or resolved at list end)
    UINT msn = 0;                  // MSAA HUD targets (the game's MSAA path draws the HUD into its MSAA scene image)
    ID3D12Resource* mrt[2] = {}; ID3D12Resource* mds[2] = {};
    ID3D12DescriptorHeap* mrh = nullptr; ID3D12DescriptorHeap* mdh = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE mrtv[2]{}, mdsv[2]{};
    struct MsSet { UINT n = 0; ID3D12Resource* mrt[2] = {}; ID3D12Resource* mds[2] = {}; ID3D12DescriptorHeap* mrh = nullptr; ID3D12DescriptorHeap* mdh = nullptr; D3D12_CPU_DESCRIPTOR_HANDLE mrtv[2]{}, mdsv[2]{}; };
    MsSet parked[4];               // sets for other sample counts, kept (never released while running)
    long long pre_tick = -1;
    ID3D12DescriptorHeap* rh = nullptr;
    ID3D12DescriptorHeap* dh = nullptr;
    ID3D12DescriptorHeap* sh = nullptr; // shader-visible: SRV black, SRV white, UAV out
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{}, dsv[2]{};
    UINT inc = 0, w = 0, h = 0;
    long long tick = -1;
    float depth_clear = 1.0f;
    UINT8 stencil_clear = 0;
} g_hud, g_fx; // HUD panel image, screen-effects image
std::atomic<long> g_hud_frames{0}, g_hud_bundles{0}, g_fx_bundles{0};
// Difference matting: over black the HUD gives C_b = a*c, over white C_w = a*c + (1-a) -> a = 1 - (C_w - C_b),
// c = C_b / a. Exact for any "over" blend the game uses; additive glows come out as partly transparent light.
static const char* kMatteHlsl =
    "Texture2D<float4> cb : register(t0);\n"
    "Texture2D<float4> cw : register(t1);\n"
    "Texture2D<float4> fin : register(t2);\n"
    "Texture2D<float4> pre : register(t3);\n"
    "RWTexture2D<float4> o : register(u0);\n"
    "cbuffer C : register(b0) { uint premul; uint rx0; uint ry0; uint rx1; uint ry1; };\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    float3 b = cb[id.xy].rgb, w = cw[id.xy].rgb;\n"
    "    float a = saturate(1.0 - dot(w - b, float3(1.0, 1.0, 1.0) / 3.0));\n"
    "    float3 c = premul == 1 ? b : (a > 0.004 ? saturate(b / a) : float3(0, 0, 0));\n"
    "    if (id.x >= rx0 && id.y >= ry0 && id.x < rx1 && id.y < ry1) {\n"
    "        // minimap: compare the finished image with the scene right before the HUD. Unchanged: nothing there.\n"
    "        // Darker: the see-through backdrop -> black with matching alpha. Brighter: lines and icons, opaque.\n"
    "        float3 f = fin[id.xy].rgb, q = pre[id.xy].rgb;\n"
    "        float lf = dot(f, 1.0 / 3.0), lq = dot(q, 1.0 / 3.0);\n"
    "        if (all(abs(f - q) < 0.012)) { c = 0; a = 0; }\n"
    "        else if (lf > lq + 0.03) { c = f; a = 1; }\n"
    "        else { c = 0; a = saturate(1.0 - lf / max(lq, 0.03)); }\n"
    "    }\n"
    "    o[id.xy] = premul == 2 ? float4(b, 1) : premul == 3 ? float4(w, 1) : float4(c, a);\n"
    "}\n";
bool hud_create(CL* l, HudImg& H) {
    H.tried = true;
    ViewRec vr, vd;
    if (!view_get(g_eye.scene_rtv, vr) || !vr.res || !view_get(g_eye.scene_dsv, vd) || !vd.res) return false;
    ID3D12Device* dev = nullptr;
    l->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev) return false;
    D3D12_RESOURCE_DESC rd = vr.res->GetDesc(), dd = vd.res->GetDesc();
    H.w = (UINT)rd.Width; H.h = rd.Height;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_DESCRIPTOR_HEAP_DESC h1{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC h2{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC h3{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const char* why = nullptr;
    for (int i = 0; i < 2 && !why; ++i)
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource), (void**)&H.rt[i])) ||
            FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, __uuidof(ID3D12Resource), (void**)&H.ds[i]))) why = "targets";
    D3D12_RESOURCE_DESC od = rd;
    od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; od.MipLevels = 1; od.Alignment = 0;
    if (!why && FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &od, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&H.out))) why = "matte output";
    if (!why && (FAILED(dev->CreateDescriptorHeap(&h1, __uuidof(ID3D12DescriptorHeap), (void**)&H.rh)) ||
                 FAILED(dev->CreateDescriptorHeap(&h2, __uuidof(ID3D12DescriptorHeap), (void**)&H.dh)) ||
                 FAILED(dev->CreateDescriptorHeap(&h3, __uuidof(ID3D12DescriptorHeap), (void**)&H.sh)))) why = "heaps";
    if (!why) {
        UINT ri = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), di = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        H.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int i = 0; i < 2; ++i) {
            H.rtv[i] = H.rh->GetCPUDescriptorHandleForHeapStart(); H.rtv[i].ptr += (SIZE_T)i * ri;
            H.dsv[i] = H.dh->GetCPUDescriptorHandleForHeapStart(); H.dsv[i].ptr += (SIZE_T)i * di;
            dev->CreateRenderTargetView(H.rt[i], vr.has_desc ? &vr.rtv : nullptr, H.rtv[i]);
            dev->CreateDepthStencilView(H.ds[i], vd.has_desc ? &vd.dsv : nullptr, H.dsv[i]);
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = rd.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            auto c = H.sh->GetCPUDescriptorHandleForHeapStart(); c.ptr += (SIZE_T)i * H.inc;
            dev->CreateShaderResourceView(H.rt[i], &sv, c);
        }
        { // t2: the game's finished scene image (HUD drawn into it)
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = rd.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            auto c2 = H.sh->GetCPUDescriptorHandleForHeapStart(); c2.ptr += (SIZE_T)2 * H.inc;
            if (&H == &g_hud) {
                D3D12_RESOURCE_DESC fd = rd; fd.Flags = D3D12_RESOURCE_FLAG_NONE;
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &fd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&H.fin))) why = "fin image";
            }
            ID3D12Resource* fin = g_hud.fin;
            if (fin) dev->CreateShaderResourceView(fin, &sv, c2);
            // t3: copy of the scene image right before the HUD
            D3D12_RESOURCE_DESC pd = rd; pd.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &pd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&H.pre))) why = "pre image";
            else { auto c3 = H.sh->GetCPUDescriptorHandleForHeapStart(); c3.ptr += (SIZE_T)3 * H.inc; dev->CreateShaderResourceView(H.pre, &sv, c3); }
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = rd.Format; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto c = H.sh->GetCPUDescriptorHandleForHeapStart(); c.ptr += (SIZE_T)4 * H.inc;
        dev->CreateUnorderedAccessView(H.out, nullptr, &uv, c);
        // matte pipeline
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? (D3DCompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        auto ser = (SerializeRsFn)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
        ID3DBlob *cs = nullptr, *err = nullptr, *rsb = nullptr;
        D3D12_DESCRIPTOR_RANGE rg[2] = {};
        rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors = 4;
        rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rg[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 5;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = {1, &rg[0]};
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable = {1, &rg[1]};
        D3D12_ROOT_SIGNATURE_DESC rsd{3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (!compile || !ser || FAILED(compile(kMatteHlsl, strlen(kMatteHlsl), "kh2vr_matte", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) why = "matte shader";
        else if (FAILED(ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
                 FAILED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&H.rs))) why = "matte root signature";
        else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = H.rs; pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
            if (FAILED(dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&H.pso))) why = "matte PSO";
        }
        if (cs) cs->Release();
        if (rsb) rsb->Release();
        if (err) err->Release();
    }
    dev->Release();
    H.ok = why == nullptr;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("HUD: %s images %s%s (%llux%u fmt %d, black/white matting)", &H == &g_hud ? "panel" : "screen-effects", H.ok ? "created" : "FAILED: ", why ? why : "", (unsigned long long)rd.Width, rd.Height, (int)rd.Format);
    return H.ok;
}
// At the end of the list that drew the HUD: matte black/white -> out (left in COPY_SOURCE for the caller's copy).
void hud_matte(const D3dVt* v, CL* l, HudImg& H) {
    auto f = v->orig;
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))f[26];
    D3D12_RESOURCE_BARRIER b[4] = {};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {H.rt[0], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    b[1].Transition = {H.rt[1], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bar(l, 2, b);
    ID3D12DescriptorHeap* hp = H.sh;
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))f[28])(l, 1, &hp);
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*))f[29])(l, H.rs);
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))f[25])(l, H.pso);
    UINT premul = g_cfg.hud_debug == 1 ? 2u : g_cfg.hud_debug == 2 ? 3u : g_cfg.hud_premul ? 1u : 0u; // debug: raw black/white images
    UINT cst[5] = {premul, 0, 0, 0, 0};
    if (&H == &g_hud) swscanf(g_cfg.hud_minimap_rect.c_str(), L"%u,%u,%u,%u", &cst[1], &cst[2], &cst[3], &cst[4]);
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const void*, UINT))f[35])(l, 0, 5, cst, 0); // SetComputeRoot32BitConstants
    auto g0 = H.sh->GetGPUDescriptorHandleForHeapStart();
    auto g2 = g0; g2.ptr += (UINT64)4 * H.inc;
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))f[31])(l, 1, g0);
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))f[31])(l, 2, g2);
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT))f[14])(l, (H.w + 7) / 8, (H.h + 7) / 8, 1);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    b[2].Transition = {H.out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    bar(l, 3, b);
}

// MSAA HUD targets for sample count n (created on first use per count, then kept). Switching counts parks the
// current set in H.parked and never releases it: leaving the menu the game draws one 8x frame then 2x, and freeing
// the 8x set while the GPU was still drawing into it hung the GPU (driver reset, then the game's next D3D12 call
// threw and the game crashed; runs camcrash2/3, stack2, nvlddmkm 153 at each 8x->2x switch).
bool hud_ensure_ms(CL* l, HudImg& H, UINT n, ID3D12Resource* scene, ID3D12Resource* scene_ds) {
    if (H.msn == n && H.mrt[0]) return true;
    auto take = [](HudImg::MsSet& d, HudImg& s) { // current set -> d, H's fields cleared
        d.n = s.msn; for (int i = 0; i < 2; ++i) { d.mrt[i] = s.mrt[i]; d.mds[i] = s.mds[i]; d.mrtv[i] = s.mrtv[i]; d.mdsv[i] = s.mdsv[i]; s.mrt[i] = s.mds[i] = nullptr; }
        d.mrh = s.mrh; d.mdh = s.mdh; s.mrh = s.mdh = nullptr; s.msn = 0;
    };
    if (H.mrt[0] && H.msn) // park the current set
        for (auto& p : H.parked) if (!p.n) { take(p, H); break; }
    for (auto& p : H.parked) // a parked set for n: make it current
        if (p.n == n) {
            if (H.mrt[0]) for (auto& q : H.parked) if (!q.n) { take(q, H); break; } // (current couldn't be parked: no free slot)
            for (int i = 0; i < 2; ++i) { H.mrt[i] = p.mrt[i]; H.mds[i] = p.mds[i]; H.mrtv[i] = p.mrtv[i]; H.mdsv[i] = p.mdsv[i]; }
            H.mrh = p.mrh; H.mdh = p.mdh; H.msn = n; p = HudImg::MsSet{};
            static int reuse_logs = 0;
            if (reuse_logs < 40) { ++reuse_logs; std::lock_guard<std::mutex> lk(g_log_mutex); logf("HUD: %s MSAA targets (%ux) reused (kept set)", &H == &g_hud ? "panel" : "screen-effects", n); }
            return true;
        }
    if (H.mrt[0]) return false; // no free slot to park the current set (more than 4 sample counts): don't free it in flight
    ID3D12Device* dev = nullptr;
    l->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    if (!dev || !scene || !scene_ds) { if (dev) dev->Release(); return false; }
    D3D12_RESOURCE_DESC rd = scene->GetDesc(), dd = scene_ds->GetDesc();
    dd.SampleDesc = rd.SampleDesc; dd.Width = rd.Width; dd.Height = rd.Height; dd.Alignment = 0;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    bool ok = true;
    if (!H.mrh) {
        D3D12_DESCRIPTOR_HEAP_DESC h1{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC h2{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ok = SUCCEEDED(dev->CreateDescriptorHeap(&h1, __uuidof(ID3D12DescriptorHeap), (void**)&H.mrh)) && SUCCEEDED(dev->CreateDescriptorHeap(&h2, __uuidof(ID3D12DescriptorHeap), (void**)&H.mdh));
    }
    UINT ri = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), di = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (int i = 0; i < 2 && ok; ++i) {
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource), (void**)&H.mrt[i])) &&
             SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, __uuidof(ID3D12Resource), (void**)&H.mds[i]));
        if (!ok) break;
        H.mrtv[i] = H.mrh->GetCPUDescriptorHandleForHeapStart(); H.mrtv[i].ptr += (SIZE_T)i * ri;
        H.mdsv[i] = H.mdh->GetCPUDescriptorHandleForHeapStart(); H.mdsv[i].ptr += (SIZE_T)i * di;
        dev->CreateRenderTargetView(H.mrt[i], nullptr, H.mrtv[i]);
        dev->CreateDepthStencilView(H.mds[i], nullptr, H.mdsv[i]);
    }
    dev->Release();
    H.msn = ok ? n : 0;
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("HUD: %s MSAA targets (%ux) %s", &H == &g_hud ? "panel" : "screen-effects", n, ok ? "created" : "FAILED");
    return ok;
}
// dst (1 sample, resting in NON_PIXEL_SHADER_RESOURCE) <- the scene image src (RENDER_TARGET), copied or resolved.
void scene_to_1x(const D3dVt* v, CL* l, ID3D12Resource* dst, ID3D12Resource* src, UINT n) {
    auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
    D3D12_RESOURCE_BARRIER b[2] = {};
    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, n > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, n > 1 ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST};
    bar(l, 2, b);
    if (n > 1) ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT))v->orig[19])(l, dst, 0, src, 0, src->GetDesc().Format);
    else ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17])(l, dst, src);
    for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    bar(l, 2, b);
}

HRESULT STDMETHODCALLTYPE hk_close(CL* l) {
    HookScope guard;
    std::string dump;
    if (guard.outer) {
        std::lock_guard<std::recursive_mutex> lk(g_d3d_mutex);
        if (ListRec* r = rec_of(l, false)) {
            int t = r->type == D3D12_COMMAND_LIST_TYPE_DIRECT ? 0 : r->type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? 1 : 2;
            D3dAgg& g = g_agg[r->main ? 1 : 0][t];
            ++g.lists; g.draws += r->draws; g.bundles += r->bundles; g.rts += r->rts; g.clears += r->clears; g.r2 += r->r2n;
            if (r->dump && r->bundles > 0) {
                char hdr[160];
                snprintf(hdr, sizeof(hdr), "D3D_DUMP %s direct list %p: draws=%d bundles=%d rt_sets=%d clears=%d r2_distinct=%d | ",
                         r->main ? "main" : "other", (void*)l, r->draws, r->bundles, r->rts, r->clears, r->r2n);
                dump = hdr + r->ops;
                if (r->main) { if (--g_dump_left_main <= 0) g_dump_main = false; }
                else { if (--g_dump_left_other <= 0) g_dump_other = false; }
            }
        }
        if (InjRec* ir = inj_of(l, false)) {
            if (g_ticklog_left.load() > 0 && !ir->is_twin && (ir->bundles || ir->hud_seen || ir->done)) { // cmd=tick_log
                char b[160];
                snprintf(b, sizeof(b), " [list %llx%s seg=%d fired=%d bundles=%d hudstage=%d huddrawn=%d hudlist=%d scene=%d]", (unsigned long long)((uintptr_t)l & 0xffff),
                         GetCurrentThreadId() == g_frame_thread.load() ? "" : " OTHER-THREAD", (int)ir->segment, (int)ir->done, ir->bundles, ir->hud_seen, (int)ir->hud_drawn, (int)ir->hud_list, (int)ir->bound_scene);
                g_ticklog_buf += b;
            }
            if (ir->hud_drawn && g_xr.hud_w && g_hud.ok && g_fx.ok) {
                const D3dVt* v = vt_of(l);
                int k = g_xr.hud_stage_next;
                g_xr.hud_stage_next = (k + 1) % 3;
                if (ir->hud_n > 1) // MSAA HUD targets -> the 1-sample images the matte reads
                    for (HudImg* I : {&g_hud, &g_fx}) {
                        if (!hud_ensure_ms(l, *I, ir->hud_n, ir->hud_scene, g_eye.scene_ds)) continue; // this list's set (another list may have switched counts)
                        auto bar = (void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26];
                        D3D12_RESOURCE_BARRIER b[4] = {};
                        for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        for (int i = 0; i < 2; ++i) {
                            b[2 * i].Transition = {I->mrt[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE};
                            b[2 * i + 1].Transition = {I->rt[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST};
                        }
                        bar(l, 4, b);
                        for (int i = 0; i < 2; ++i)
                            ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT))v->orig[19])(l, I->rt[i], 0, I->mrt[i], 0, I->mrt[i]->GetDesc().Format);
                        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
                        bar(l, 4, b);
                    }
                if (g_hud.fin && ir->hud_scene) scene_to_1x(v, l, g_hud.fin, ir->hud_scene, ir->hud_n); // the finished image (minimap)
                for (int which = 0; which < 2; ++which) { // 0 panel, 1 screen effects
                    HudImg& H = which ? g_fx : g_hud;
                    ID3D12Resource* st = which ? g_xr.fx_stage[k] : g_xr.hud_stage[k];
                    D3D12_RESOURCE_BARRIER b[2] = {};
                    for (auto& x : b) x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    hud_matte(v, l, H); // out is in COPY_SOURCE now
                    b[0].Transition = {H.out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
                    b[1].Transition = {st, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
                    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26])(l, 1, &b[1]);
                    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))v->orig[17])(l, st, H.out);
                    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
                    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))v->orig[26])(l, 2, b);
                }
                g_xr.pending_hud = l;
                g_xr.pending_hud_stage = k;
            }
            long long tick_now = g_total_ticks.load();
            if (ir->is_twin) twin_end(ir);
            else if (ir->active && !ir->done && ir->segment) {
                // the list ends without the post stage's clear (e.g. Land of Dragons: post in a later list, run fx3,
                // 2026-11-10): replay at the end of the list instead
                bool has_cam = false; // a scene list binds the camera block (r2) itself; post-stage/HUD lists don't (run flash4)
                int cam_bundles = 0;  // bundles executed in the segment that bind r2 inside
                for (auto& o : ir->ops) {
                    if (o.k == O_CBV && o.u[0] == 2) has_cam = true;
                    if (o.k == O_EB) { ListRec* br = rec_of((CL*)o.p[0], false); if (br && br->r2n > 0) ++cam_bundles; }
                }
                static int rej_logged = 0;
                if (!has_cam && rej_logged < 6 && (int)ir->bundles >= g_cfg.d3d_inject_min_bundles) {
                    ++rej_logged;
                    std::string t; char b[64];
                    for (auto& o : ir->ops) {
                        if (o.k == O_EB) { ListRec* br = rec_of((CL*)o.p[0], false); t += br ? (br->r2n ? "Bc " : "B ") : "B? "; }
                        else if (o.k == O_RTS) { snprintf(b, sizeof(b), "RT(%llx%s) ", o.u[0] ? (unsigned long long)(((const D3D12_CPU_DESCRIPTOR_HANDLE*)o.blob.data())->ptr & 0xffff) : 0ull, o.u[1] ? ",ds" : ""); t += b; }
                        else if (o.k == O_CLRRT) { snprintf(b, sizeof(b), "CLR(%llx) ", (unsigned long long)(o.h.ptr & 0xffff)); t += b; }
                        else if (o.k == O_COPYRES || o.k == O_COPYTEX) t += "COPY ";
                    }
                    std::lock_guard<std::mutex> lk3(g_log_mutex);
                    logf("INJ rejected at close (no r2 on the list; %d of %d bundles bind r2 inside): %s", cam_bundles, ir->bundles, t.substr(0, 1800).c_str());
                }
                if (g_cfg.d3d_inject_at_close && has_cam && (int)ir->bundles >= g_cfg.d3d_inject_min_bundles && g_last_fire_tick.load() != g_total_ticks.load()) {
                    // only when this tick had no replay yet: in Twilight Town a later list (post stage + HUD) would
                    // otherwise replay over the eye images (HUD baked in, glare; run hist2dbg)
                    g_inj_at_close.fetch_add(1);
                    static int logged = 0;
                    if (logged < 6) { // op shape of lists that reach Close unfired: RT binds (low handle bits), clears, bundles, draws, copies
                        ++logged;
                        std::string t;
                        char b[48];
                        int nb = 0;
                        auto flush = [&] { if (nb) { snprintf(b, sizeof(b), "B%d ", nb); t += b; nb = 0; } };
                        for (auto& o : ir->ops) {
                            if (o.k == O_EB) { ++nb; continue; }
                            if (o.k == O_RTS) { flush(); snprintf(b, sizeof(b), "RT(%llx%s) ", o.u[0] ? (unsigned long long)(((const D3D12_CPU_DESCRIPTOR_HANDLE*)o.blob.data())->ptr & 0xffff) : 0ull, o.u[1] ? ",ds" : ""); t += b; }
                            else if (o.k == O_CLRRT) { flush(); snprintf(b, sizeof(b), "CLR(%llx) ", (unsigned long long)(o.h.ptr & 0xffff)); t += b; }
                            else if (o.k == O_DI || o.k == O_DII) { flush(); t += "D "; }
                            else if (o.k == O_COPYRES || o.k == O_COPYTEX || o.k == O_RESOLVE) { flush(); t += "COPY "; }
                        }
                        flush();
                        std::lock_guard<std::mutex> lk2(g_log_mutex);
                        logf("INJ at close: %d bundles, %zu ops: %s", ir->bundles, ir->ops.size(), t.substr(0, 1800).c_str());
                    }
                    g_firing_at_close = true;
                    inj_fire(vt_of(l), l, ir);
                    g_firing_at_close = false;
                }
                else { g_inj_skipped.fetch_add(1); g_skip[4].fetch_add(1); }
            }
            *ir = InjRec{};
        }
    }
    if (!dump.empty()) {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        for (size_t p = 0; p < dump.size(); p += 1500) logf("%s%s", p ? "  ...cont " : "", dump.substr(p, 1500).c_str());
    }
    return orig_of<CloseFn>(l, 9)(l);
}

// Hooks below: observer counting + segment state tracking/recording, then forward.
#define HOOK_LOCK std::lock_guard<std::recursive_mutex> lk(g_d3d_mutex); ListRec* r = guard.outer ? rec_of(l, false) : nullptr;     InjRec* ir = guard.outer ? inj_track(l) : nullptr; (void)r; (void)ir

void STDMETHODCALLTYPE hk_di(CL* l, UINT a, UINT b, UINT c, UINT d) {
    HookScope guard;
    { HOOK_LOCK; if (r) { ++r->draws; r->verts += (int)(a * b); r->max_verts = std::max(r->max_verts, (int)a); op(r, "D%u", a); }
      if (ir) { Op o{O_DI}; o.u[0] = a; o.u[1] = b; o.u[2] = c; o.u[3] = d; inj_record(ir, std::move(o)); } }
    orig_of<DrawInstFn>(l, 12)(l, a, b, c, d);
}
void STDMETHODCALLTYPE hk_dii(CL* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    HookScope guard;
    { HOOK_LOCK; if (r) { ++r->draws; r->verts += (int)(a * b); r->max_verts = std::max(r->max_verts, (int)a); op(r, "DI%u", a); }
      if (ir) { Op o{O_DII}; o.u[0] = a; o.u[1] = b; o.u[2] = c; o.i0 = d; o.u[3] = e; inj_record(ir, std::move(o)); } }
    orig_of<DrawIdxFn>(l, 13)(l, a, b, c, d, e);
}
// cmd=copy_log: log the next 80 copies/resolves on any list (where they come from, sizes, whether inside a
// replay segment, and whether the source is the game's scene image) to find textures that carry a past frame.
std::atomic<int> g_copylog_left{0};
std::atomic<int> g_postlog_left{0}; // cmd=post_log: log the next 80 post-stage bundles
const char* res_name(ID3D12Resource* r, char* buf, size_t n) {
    if (!r) { snprintf(buf, n, "null"); return buf; }
    D3D12_RESOURCE_DESC d = r->GetDesc();
    const char* tag = r == g_eye.scene_rt ? " SCENE" : r == g_eye.scene_ds ? " SCENE_DS" : "";
    for (auto& ms : g_eye_ms) if (ms.ok && r == ms.scene_rt) tag = " SCENE_MSAA";
    snprintf(buf, n, "%p %llux%u f%d s%u%s", (void*)r, (unsigned long long)d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, tag);
    return buf;
}
void copylog(const char* kind, CL* l, ID3D12Resource* d, ID3D12Resource* src, bool inseg) {
    if (g_copylog_left.load() <= 0 || g_copylog_left.fetch_sub(1) <= 0) return;
    char a[128], b[128];
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("COPYLOG %s list=%p main=%d seg=%d tick=%lld  %s <- %s", kind, (void*)l, (int)(GetCurrentThreadId() == g_frame_thread.load()), (int)inseg,
         g_total_ticks.load(), res_name(d, a, sizeof(a)), res_name(src, b, sizeof(b)));
}
void STDMETHODCALLTYPE hk_copytex(CL* l, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    HookScope guard;
    { HOOK_LOCK;
      copylog("CopyTextureRegion", l, dst->pResource, src->pResource, ir && ir->segment);
      if (ir && ir->segment) { Op o{O_COPYTEX}; o.u[0] = x; o.u[1] = y; o.u[2] = z; o.b = box != nullptr;
          o.blob.resize(2 * sizeof(*dst) + sizeof(D3D12_BOX)); memcpy(o.blob.data(), dst, sizeof(*dst)); memcpy(o.blob.data() + sizeof(*dst), src, sizeof(*src));
          if (box) memcpy(o.blob.data() + 2 * sizeof(*dst), box, sizeof(*box)); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*))vt_of(l)->orig[16])(l, dst, x, y, z, src, box);
}
void STDMETHODCALLTYPE hk_copyres(CL* l, ID3D12Resource* d, ID3D12Resource* s) {
    HookScope guard;
    { HOOK_LOCK; copylog("CopyResource", l, d, s, ir && ir->segment); if (ir) { Op o{O_COPYRES}; o.p[0] = d; o.p[1] = s; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, ID3D12Resource*))vt_of(l)->orig[17])(l, d, s);
}
void STDMETHODCALLTYPE hk_resolve(CL* l, ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f) {
    HookScope guard;
    { HOOK_LOCK; copylog("Resolve", l, d, s, ir && ir->segment); if (ir) { Op o{O_RESOLVE}; o.p[0] = d; o.u[0] = ds; o.p[1] = s; o.u[1] = ss; o.u[2] = f; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT))vt_of(l)->orig[19])(l, d, ds, s, ss, f);
}
void STDMETHODCALLTYPE hk_topo(CL* l, D3D12_PRIMITIVE_TOPOLOGY t) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.topo = t; Op o{O_TOPO}; o.u[0] = t; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, D3D12_PRIMITIVE_TOPOLOGY))vt_of(l)->orig[20])(l, t);
}
void STDMETHODCALLTYPE hk_vp(CL* l, UINT n, const D3D12_VIEWPORT* v) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.vps.assign(v, v + n); Op o{O_VP}; o.u[0] = n; o.blob = bytes_of(v, n); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_VIEWPORT*))vt_of(l)->orig[21])(l, n, v);
}
void STDMETHODCALLTYPE hk_sc(CL* l, UINT n, const D3D12_RECT* rc) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.scs.assign(rc, rc + n); Op o{O_SC}; o.u[0] = n; o.blob = bytes_of(rc, n); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RECT*))vt_of(l)->orig[22])(l, n, rc);
}
void STDMETHODCALLTYPE hk_bf(CL* l, const FLOAT* f) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.has_bf = f != nullptr; if (f) memcpy(ir->cur.bf, f, 16); Op o{O_BF}; if (f) memcpy(o.f, f, 16); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, const FLOAT*))vt_of(l)->orig[23])(l, f);
}
void STDMETHODCALLTYPE hk_sr(CL* l, UINT s) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.has_sr = true; ir->cur.sr = s; Op o{O_SR}; o.u[0] = s; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT))vt_of(l)->orig[24])(l, s);
}
void STDMETHODCALLTYPE hk_pso(CL* l, ID3D12PipelineState* p) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.pso = p; Op o{O_PSO}; o.p[0] = p; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*))vt_of(l)->orig[25])(l, p);
}
void STDMETHODCALLTYPE hk_bar(CL* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { Op o{O_BAR}; o.u[0] = n; o.blob = bytes_of(b, n); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*))vt_of(l)->orig[26])(l, n, b);
}
void STDMETHODCALLTYPE hk_eb(CL* l, CL* bundle) {
    HookScope guard;
    {
        HOOK_LOCK;
        if (r) { ++r->bundles; ListRec* b = rec_of(bundle, false); op(r, "B[%d%s]", b ? b->draws : -1, b && b->r2n ? ",r2" : ""); }
        if (ir && ir->segment) { ++ir->bundles; Op o{O_EB}; o.p[0] = bundle; inj_record(ir, std::move(o)); }
        if (ir && (ir->done || (!ir->segment_fired_here && g_total_ticks.load() - g_last_fire_tick.load() <= 1))) { // post stage (mapping tool): bundles after this tick's replay
            int k = ir->post_n++;
            if (g_postlog_left.load() > 0 && g_postlog_left.fetch_sub(1) > 0) {
                ListRec* b = rec_of(bundle, false);
                std::lock_guard<std::mutex> lk2(g_log_mutex);
                char bb[96] = "";
                if (b && b->vb_va && b->vb_stride >= 8) { // bounds of the first (up to) 6 vertices, first two floats each
                    UINT nv = std::min<UINT>(6, b->vb_size / b->vb_stride);
                    const uint8_t* vp = buf_cpu(b->vb_va, (size_t)nv * b->vb_stride);
                    if (vp) {
                        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                        for (UINT i = 0; i < nv; ++i) { const float* f = (const float*)(vp + (size_t)i * b->vb_stride); x0 = std::min(x0, f[0]); x1 = std::max(x1, f[0]); y0 = std::min(y0, f[1]); y1 = std::max(y1, f[1]); }
                        snprintf(bb, sizeof(bb), " bbox=(%.1f %.1f)-(%.1f %.1f) stride=%u", x0, y0, x1, y1, b->vb_stride);
                    }
                }
                logf("POST bundle %d list=%p: draws=%d verts=%d maxdraw=%d r2=%d rt=%llx%s", k, (void*)l, b ? b->draws : -1, b ? b->verts : -1, b ? b->max_verts : -1, b ? b->r2n : -1, (unsigned long long)(ir->last_rt & 0xffff), bb);
            }
            if (g_cfg.hud_skip_lo >= 0 && k >= g_cfg.hud_skip_lo && k <= g_cfg.hud_skip_hi) return;
        }
    }
    orig_of<ExecBundleFn>(l, 27)(l, bundle);
    if (guard.outer && g_cfg.hud && g_cfg.xr && g_eye.ok) {
        std::lock_guard<std::recursive_mutex> lk(g_d3d_mutex);
        InjRec* ir = inj_of(l, false);
        // a HUD-stage bundle: in a list after this tick's replay, the scene image bound again after the glow chain.
        // Screen-covering ones: the first is the glow composite (dropped); later ones are screen effects (impact,
        // keyhole) for the full-view layer; the rest is HUD for the panel.
        // only the first HUD pass after each replay (Unlocked draws a second, in-between frame per tick with its own
        // HUD pass; alternating between them made the panel flash, run hudf2)
        bool hud_list_ok = ir && (ir->hud_list || g_hud_list_fire.load() != g_fire_seq.load());
        // HUD stage: after this frame's replay. In the replayed list itself (one-list frames: scene + glow + HUD), or in
        // a later list that isn't a scene list (it doesn't bind the camera block itself; a scene list before its own
        // replay would otherwise pass: world geometry on the panel, run tick2)
        bool after_replay = ir && (ir->done || g_total_ticks.load() - g_last_fire_tick.load() <= 1);
        if (ir && after_replay && hud_list_ok && ir->saw_other_rt && hud_target(ir->last_rt, nullptr)) {
            if (!ir->hud_list) { ir->hud_list = true; g_hud_list_fire = g_fire_seq.load(); }
            ID3D12Resource* scn = nullptr; ID3D12Resource* scn_ds = g_eye.scene_ds; // depth layout from the scene's; samples set below
            UINT sn = hud_target(ir->last_rt, &scn);
            if (!ir->hud_scene) { ir->hud_scene = scn; ir->hud_n = sn; }
            bool first = ir->hud_seen++ == 0;
            if (first && g_hud.ok && g_hud.pre && g_hud.pre_tick != g_fire_seq.load() && scn) { // scene right before the HUD (once per replay)
                g_hud.pre_tick = g_fire_seq.load();
                scene_to_1x(vt_of(l), l, g_hud.pre, scn, sn);
            }
            bool fs = g_cfg.hud_skip_fullscreen && bundle_fullscreen(bundle);
            // HUD sprites have CPU-readable vertices inside the screen rectangle (game screen units 1792..4352 x
            // 1840..3280). Anything else here is 3D effect geometry (battle-start shards, particles; run fx9).
            float bb[4];
            bool known = bundle_bounds(bundle, bb);
            bool in_screen = known && bb[0] >= 1792 - 64 && bb[2] <= 4352 + 64 && bb[1] >= 1840 - 64 && bb[3] <= 3280 + 64;
            HudImg* H = &g_hud;
            if (fs) { g_hud_skipped_fs.fetch_add(1); H = (!first && g_cfg.hud_fx) ? &g_fx : nullptr; }
            else if (first && g_cfg.hud_skip_composite) H = nullptr;
            // readable geometry outside the screen rectangle (battle-start particles) -> effects. Unreadable (GPU-only)
            // meshes stay on the panel: the command-menu frames are such meshes, like the battle-start shards (fx10)
            else if (known && !in_screen && g_cfg.hud_fx && g_cfg.hud_fx_offscreen) H = &g_fx; // off: command-menu frames and the minimap
                                                                                     // are readable but outside the rectangle too (mm4)
            big_log(bundle, ir->hud_seen - 1, H == &g_fx ? "effects" : H ? "panel" : "dropped");
            if (H) {
                if (!g_hud.tried) { hud_create(l, g_hud); hud_create(l, g_fx); }
                bool ms = ir->hud_n > 1;
                if (ms && !(hud_ensure_ms(l, g_hud, ir->hud_n, scn, scn_ds) && hud_ensure_ms(l, g_fx, ir->hud_n, scn, scn_ds))) ms = false, H = nullptr;
                if (H && g_hud.ok && g_fx.ok) {
                    const D3dVt* v = vt_of(l);
                    if (!ir->hud_drawn) { // first HUD-stage bundle in this list: clear both images to opaque black / white
                        g_hud.tick = g_fire_seq.load();
                        FLOAT ca = g_cfg.hud_clear_alpha, bk[4] = {0, 0, 0, ca}, wh[4] = {1, 1, 1, ca};
                        for (HudImg* I : {&g_hud, &g_fx})
                            for (int i = 0; i < 2; ++i) {
                                ((ClearRtFn)v->orig[48])(l, ms ? I->mrtv[i] : I->rtv[i], i ? wh : bk, 0, nullptr);
                                ((void(STDMETHODCALLTYPE*)(CL*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*))v->orig[47])(
                                    l, ms ? I->mdsv[i] : I->dsv[i], D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, g_hud.depth_clear, g_hud.stencil_clear, 0, nullptr);
                            }
                        g_hud_frames.fetch_add(1);
                    }
                    for (int i = 0; i < 2; ++i) {
                        orig_of<OmRtFn>(l, 46)(l, 1, ms ? &H->mrtv[i] : &H->rtv[i], FALSE, ms ? &H->mdsv[i] : &H->dsv[i]);
                        orig_of<ExecBundleFn>(l, 27)(l, bundle);
                    }
                    D3D12_CPU_DESCRIPTOR_HANDLE rt{ir->last_rt}, ds{ir->last_ds};
                    orig_of<OmRtFn>(l, 46)(l, 1, &rt, FALSE, ir->last_ds ? &ds : nullptr);
                    ir->hud_drawn = true;
                    (H == &g_fx ? g_fx_bundles : g_hud_bundles).fetch_add(1);
                }
            }
        }
    }
}
void STDMETHODCALLTYPE hk_heaps(CL* l, UINT n, ID3D12DescriptorHeap* const* h) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.heaps.assign(h, h + n); Op o{O_HEAPS}; o.u[0] = n; o.blob = bytes_of(h, n); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*))vt_of(l)->orig[28])(l, n, h);
}
void STDMETHODCALLTYPE hk_rsig(CL* l, ID3D12RootSignature* s) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.rsig = s; ir->cur.roots.clear(); Op o{O_RSIG}; o.p[0] = s; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*))vt_of(l)->orig[30])(l, s);
}
void STDMETHODCALLTYPE hk_table(CL* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE g) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { Op o{O_TABLE}; o.u[0] = i; o.q = g.ptr; set_root(ir->cur, o); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))vt_of(l)->orig[32])(l, i, g);
}
void STDMETHODCALLTYPE hk_c32(CL* l, UINT i, UINT v, UINT off) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { Op o{O_C32}; o.u[0] = i; o.u[1] = v; o.u[2] = off; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT))vt_of(l)->orig[34])(l, i, v, off);
}
void STDMETHODCALLTYPE hk_c32n(CL* l, UINT i, UINT n, const void* d, UINT off) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { Op o{O_C32N}; o.u[0] = i; o.u[1] = n; o.u[2] = off; o.blob = bytes_of((const uint32_t*)d, n); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const void*, UINT))vt_of(l)->orig[36])(l, i, n, d, off);
}
void STDMETHODCALLTYPE hk_cbv(CL* l, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    HookScope guard;
    {
        HOOK_LOCK;
        if (r && idx == 2) {
            bool seen = false;
            for (int i = 0; i < r->r2n && i < 8; ++i) if (r->r2[i] == va) seen = true;
            if (!seen) { if (r->r2n < 8) r->r2[r->r2n] = va; ++r->r2n; }
            op(r, "r2=%llx", (unsigned long long)(va & 0xffffff));
        }
        if (ir) { if (idx == 2) ir->list_cam = true; Op o{O_CBV}; o.u[0] = idx; o.q = va; set_root(ir->cur, o); inj_record(ir, std::move(o)); }
    }
    orig_of<RootCbvFn>(l, 38)(l, idx, va);
}
void STDMETHODCALLTYPE hk_srv(CL* l, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { Op o{O_SRV}; o.u[0] = idx; o.q = va; set_root(ir->cur, o); inj_record(ir, std::move(o)); } }
    orig_of<RootCbvFn>(l, 40)(l, idx, va);
}
void STDMETHODCALLTYPE hk_uav(CL* l, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { Op o{O_UAV}; o.u[0] = idx; o.q = va; set_root(ir->cur, o); inj_record(ir, std::move(o)); } }
    orig_of<RootCbvFn>(l, 42)(l, idx, va);
}
void STDMETHODCALLTYPE hk_ib(CL* l, const D3D12_INDEX_BUFFER_VIEW* v) {
    HookScope guard;
    { HOOK_LOCK; if (ir) { ir->cur.has_ib = v != nullptr; if (v) ir->cur.ib = *v; if (v) { Op o{O_IB}; o.blob = bytes_of(v, 1); inj_record(ir, std::move(o)); } } }
    ((void(STDMETHODCALLTYPE*)(CL*, const D3D12_INDEX_BUFFER_VIEW*))vt_of(l)->orig[43])(l, v);
}
void STDMETHODCALLTYPE hk_vb(CL* l, UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    HookScope guard;
    { HOOK_LOCK; if (r && v && n && s == 0 && !r->vb_va) { r->vb_va = v[0].BufferLocation; r->vb_size = v[0].SizeInBytes; r->vb_stride = v[0].StrideInBytes; }
      if (ir && v) { ir->cur.vb_start = s; ir->cur.vb_n = n; ir->cur.vbs = bytes_of(v, n); Op o{O_VB}; o.u[0] = s; o.u[1] = n; o.blob = bytes_of(v, n); inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*))vt_of(l)->orig[44])(l, s, n, v);
}
void STDMETHODCALLTYPE hk_om(CL* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    HookScope guard;
    {
        HOOK_LOCK;
        if (r) { ++r->rts; op(r, "RT(%u:%llx%s)", n, n && rt ? (unsigned long long)(rt[0].ptr & 0xffff) : 0ull, ds ? ",ds" : ""); }
        if (ir) {
            if (n && rt) { ir->last_rt = rt[0].ptr; ir->last_ds = ds ? ds->ptr : 0; if (g_eye.ok && rt_is_scene(rt[0].ptr)) ir->bound_scene = true; if (g_eye.ok && !rt_is_scene(rt[0].ptr) && rt_is_glow(rt[0].ptr)) ir->saw_other_rt = true; }
            if (!ir->segment && ds && n == 1 && !single) { ir->segment = true; ir->start = ir->cur; }
            if (ir->segment) {
                Op o{O_RTS}; o.u[0] = n; o.u[1] = ds != nullptr; o.b = single != 0; if (ds) o.h2 = *ds;
                o.blob = bytes_of(rt, single ? (n ? 1 : 0) : n);
                for (UINT i = 0; i < n && rt; ++i) ir->rts.push_back(single ? rt[0].ptr + i : rt[i].ptr); // (single: contiguous handles)
                inj_record(ir, std::move(o));
            }
        } else if (guard.outer && GetCurrentThreadId() == g_frame_thread.load()) {
            // after this list's replay fired, inj_track no longer returns it; the HUD stage still needs the binds
            // (one-list frames: scene + glow + HUD; the HUD comes after the replay; run hudms2)
            InjRec* d = inj_of(l, false);
            if (d && d->done && n && rt) {
                d->last_rt = rt[0].ptr; d->last_ds = ds ? ds->ptr : 0;
                if (g_eye.ok && !rt_is_scene(rt[0].ptr) && rt_is_glow(rt[0].ptr)) d->saw_other_rt = true;
            }
        }
    }
    orig_of<OmRtFn>(l, 46)(l, n, rt, single, ds);
}
void STDMETHODCALLTYPE hk_clrds(CL* l, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT* rc) {
    HookScope guard;
    if (g_eye.ok && h.ptr == g_eye.scene_dsv) { g_hud.depth_clear = d; g_hud.stencil_clear = s; }
    { HOOK_LOCK; if (ir && ir->segment && n == 0) { Op o{O_CLRDS}; o.h = h; o.u[0] = f; o.u[1] = s; o.f[0] = d; inj_record(ir, std::move(o)); } }
    ((void(STDMETHODCALLTYPE*)(CL*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*))vt_of(l)->orig[47])(l, h, f, d, s, n, rc);
}
void STDMETHODCALLTYPE hk_clr(CL* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT n, const D3D12_RECT* rc) {
    HookScope guard;
    {
        HOOK_LOCK;
        if (r) { ++r->clears; op(r, "CLR(%llx)", (unsigned long long)(h.ptr & 0xffff)); }
        if (ir && ir->segment) {
            bool known = std::find(ir->rts.begin(), ir->rts.end(), (UINT64)h.ptr) != ir->rts.end();
            if (!known) ir->post_started = true; // the post stage (glow chain) starts here; HUD binds of the scene image come after
            if (!known && ir->bundles >= g_cfg.d3d_inject_min_bundles && g_last_fire_tick.load() != g_total_ticks.load())
                inj_fire(vt_of(l), l, ir); // post stage starts: replay first (once per tick: a later list mustn't overwrite the eyes)
            else if (n == 0) { Op o{O_CLRRT}; o.h = h; if (c) memcpy(o.f, c, 16); inj_record(ir, std::move(o)); }
        }
    }
    orig_of<ClearRtFn>(l, 48)(l, h, c, n, rc);
}

const struct { int slot; void* fn; } kHooked[] = {
    {9, (void*)&hk_close}, {10, (void*)&hk_reset}, {12, (void*)&hk_di}, {13, (void*)&hk_dii}, {16, (void*)&hk_copytex},
    {17, (void*)&hk_copyres}, {19, (void*)&hk_resolve}, {20, (void*)&hk_topo}, {21, (void*)&hk_vp}, {22, (void*)&hk_sc},
    {23, (void*)&hk_bf}, {24, (void*)&hk_sr}, {25, (void*)&hk_pso}, {26, (void*)&hk_bar}, {27, (void*)&hk_eb},
    {28, (void*)&hk_heaps}, {30, (void*)&hk_rsig}, {32, (void*)&hk_table}, {34, (void*)&hk_c32}, {36, (void*)&hk_c32n},
    {38, (void*)&hk_cbv}, {40, (void*)&hk_srv}, {42, (void*)&hk_uav}, {43, (void*)&hk_ib}, {44, (void*)&hk_vb},
    {46, (void*)&hk_om}, {47, (void*)&hk_clrds}, {48, (void*)&hk_clr}};

void d3d_patch_vt(void** vt) {
    for (int i = 0; i < g_nvts; ++i) if (g_vts[i].vt == vt) return;
    if (g_nvts >= 2) return;
    D3dVt& v = g_vts[g_nvts];
    v.vt = vt;
    for (int i = 0; i < 64; ++i) v.orig[i] = vt[i];
    ++g_nvts; // published before the slots are swapped, so a hook always finds its originals
    for (auto& e : kHooked) write_slot(vt, e.slot, e.fn);
}

// Called from the Present hook (game thread, g_log_mutex held), at most once.
void d3d_install(IDXGISwapChain* sc) {
    g_d3d_installed = true;
    ID3D12Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) { logf("D3D observe: no ID3D12Device from the swapchain; off"); return; }
    ID3D12CommandAllocator* ad = nullptr; ID3D12CommandAllocator* ab = nullptr;
    CL* ld = nullptr; CL* lb = nullptr;
    HRESULT h1 = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&ad);
    HRESULT h2 = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, __uuidof(ID3D12CommandAllocator), (void**)&ab);
    HRESULT h3 = ad ? dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ad, nullptr, __uuidof(CL), (void**)&ld) : E_FAIL;
    HRESULT h4 = ab ? dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, ab, nullptr, __uuidof(CL), (void**)&lb) : E_FAIL;
    void** vd = ld ? *(void***)ld : nullptr;
    void** vb = lb ? *(void***)lb : nullptr;
    if (ld) ld->Close();
    if (lb) lb->Close();
    if (vd) d3d_patch_vt(vd);
    if (vb) d3d_patch_vt(vb);
    // command queue vtable (ExecuteCommandLists, slot 10) from a throwaway queue: XR output needs the game's
    // queue and the moment its list is submitted
    ID3D12CommandQueue* tq = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&tq)) && tq) {
        void** qvt = *(void***)tq;
        if (!g_ecl_orig) { g_ecl_orig = (EclFn)qvt[10]; write_slot(qvt, 10, (void*)&hk_ecl); }
        tq->Release();
    }
    logf("D3D observe: create hr %08lx %08lx %08lx %08lx; vtables direct=%p bundle=%p (%s); patched %d table(s), %d slots each", h1, h2, h3,
         h4, (void*)vd, (void*)vb, vd == vb ? "shared" : "separate", g_nvts, (int)(sizeof(kHooked) / sizeof(kHooked[0])));
    if (ld) ld->Release();
    if (lb) lb->Release();
    if (ad) ad->Release();
    if (ab) ab->Release();
    dev->Release();
}

void d3d_unpatch() {
    for (int i = 0; i < g_nvts; ++i)
        for (auto& e : kHooked) write_slot(g_vts[i].vt, e.slot, g_vts[i].orig[e.slot]);
}

// D3D12 debug layer (d3d_debug=1, read at start): the game's D3D12CreateDevice import is redirected so the
// layer is enabled before its device exists; the device's info queue is drained into the log at each Present.
using CreateDevFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
CreateDevFn g_create_dev_real = nullptr;
ID3D12InfoQueue* g_infoq = nullptr;
long g_dbg_err = 0, g_dbg_warn = 0, g_dbg_logged = 0;
// Synchronous error log: the info-queue callback runs on the thread that made the bad call, before any hang,
// and writes straight to <log_dir>\d3d_debug.log with its own lock (never g_log_mutex: a thread holding it
// can be the one making D3D calls).
std::mutex g_dbgcb_mutex;
FILE* g_dbgcb_file = nullptr;
void CALLBACK d3d_msg_cb(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR desc, void*) {
    if (sev > D3D12_MESSAGE_SEVERITY_ERROR) return;
    std::lock_guard<std::mutex> lk(g_dbgcb_mutex);
    if (!g_dbgcb_file) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    fprintf(g_dbgcb_file, "qpc=%lld tid=%lu main=%d in_hook=%d inject_replays=%ld sev=%d id=%d %s\n", t.QuadPart, GetCurrentThreadId(),
            GetCurrentThreadId() == g_frame_thread.load(), t_in_hook, g_inj_done.load(), (int)sev, (int)id, desc ? desc : "");
    fflush(g_dbgcb_file);
}
HRESULT WINAPI hk_create_device(IUnknown* ad, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    using GetDbgFn = HRESULT(WINAPI*)(REFIID, void**);
    auto getdbg = (GetDbgFn)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetDebugInterface");
    ID3D12Debug* dbg = nullptr;
    HRESULT hd = getdbg ? getdbg(__uuidof(ID3D12Debug), (void**)&dbg) : E_FAIL;
    if (dbg) { if (g_cfg.d3d_debug) dbg->EnableDebugLayer(); dbg->Release(); }
    HRESULT hr = g_create_dev_real(ad, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out && g_cfg.d3d_capture && !g_dev_orig[20]) {
        ID3D12Device* dv = nullptr;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12Device), (void**)&dv)) && dv) {
            void** vt = *(void***)dv;
            for (int i = 0; i < 64; ++i) g_dev_orig[i] = vt[i];
            write_slot(vt, 20, (void*)&hk_crtv);
            write_slot(vt, 21, (void*)&hk_cdsv);
            write_slot(vt, 27, (void*)&hk_ccr);
            write_slot(vt, 29, (void*)&hk_cpr);
            dv->Release();
        }
    }
    if (SUCCEEDED(hr) && out && *out) ((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&g_infoq);
    DWORD cookie = 0;
    HRESULT hcb = E_NOINTERFACE;
    ID3D12InfoQueue1* q1 = nullptr;
    if (g_infoq && SUCCEEDED(g_infoq->QueryInterface(__uuidof(ID3D12InfoQueue1), (void**)&q1)) && q1) {
        if (!g_dbgcb_file) g_dbgcb_file = _wfopen((g_cfg.log_dir + L"\\d3d_debug.log").c_str(), L"a");
        hcb = q1->RegisterMessageCallback(d3d_msg_cb, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        q1->Release();
    }
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("D3D debug layer: GetDebugInterface hr=%08lx, CreateDevice hr=%08lx, info queue %s, sync callback hr=%08lx", hd, hr,
         g_infoq ? "on" : "unavailable", hcb);
    return hr;
}
void d3d_debug_install() {
    HMODULE d = GetModuleHandleW(L"d3d12.dll");
    void* real = d ? (void*)GetProcAddress(d, "D3D12CreateDevice") : nullptr;
    int n = real ? patch_imports(real, (void*)&hk_create_device) : 0;
    if (n) g_create_dev_real = (CreateDevFn)real;
    logf("D3D debug layer: D3D12CreateDevice imports patched=%d%s", n, n ? "" : " (debug layer OFF)");
}
// With g_log_mutex held (Present hook).
void d3d_debug_drain() {
    if (!g_infoq) return;
    UINT64 n = g_infoq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_infoq->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<uint8_t> buf(len);
        auto* m = (D3D12_MESSAGE*)buf.data();
        if (FAILED(g_infoq->GetMessage(i, m, &len))) continue;
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++g_dbg_err; else if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++g_dbg_warn; else continue;
        // Errors and corruption only (the game alone emits thousands of clear-value warnings, ids 820/821).
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR && g_dbg_logged < 80) {
            ++g_dbg_logged;
            logf("D3DDBG sev=%d id=%d inject_replays=%ld %.500s", (int)m->Severity, (int)m->ID, g_inj_done.load(), m->pDescription);
        }
    }
    g_infoq->ClearStoredMessages();
}

// Called with g_log_mutex held from the STATS block.
void d3d_log_window(double win_ms) {
    if (g_infoq || g_dbg_err || g_dbg_warn) logf("D3DDBG totals errors=%ld warnings=%ld (first 80 errors logged)", g_dbg_err, g_dbg_warn);
    if (g_cfg.xr || g_xr.tried) logf("XR ok=%d running=%d frames=%ld ended=%ld forced=%ld head_frames=%ld head=%d test_yaw=%.1f queue=%p", (int)g_xr.ok,
                                     (int)g_xr.running, g_xr_frames.load(), g_xr_ended.load(), g_xr_forced.load(), g_head_frames.load(),
                                     (int)(g_cfg.xr_head && g_head_ok), g_cfg.xr_test_yaw_deg, (void*)g_xr.queue);
    {
        auto ms = [](TAcc& a, long& n) { long long u = a.us.load(); n = a.n.load() - a.p_n; double r = n ? (u - a.p_us) / 1000.0 / n : 0.0; a.p_us = u; a.p_n = a.n.load(); return r; };
        long np, ni, ne;
        double tp = ms(g_t_present, np), ti = ms(g_t_inj, ni), te = ms(g_t_ecl, ne);
        logf("TIMING game-thread CPU per call: Present %.2f ms (x%ld), eye replay %.2f ms (x%ld), ExecuteCommandLists %.3f ms (x%ld)", tp, np, ti, ni, te, ne);
    }
    if (g_cfg.xr_thread && g_xr.ok) {
        static long p_fr = 0, p_new = 0, p_img = 0;
        static long long p_wait = 0;
        long fr = g_xrt_frames.load(), nw = g_xrt_new.load(), im = g_game_imgs.load();
        long long wt = g_xrt_wait_us.load();
        double s = win_ms / 1000.0;
        logf("XRT headset frames %.1f/s (new image %.1f/s, repeated %.1f/s, no layer total %ld) game eye images %.1f/s, mean xrWaitFrame %.2f ms, flat frames total %ld, flat mode %d, not-gameplay lists %ld (scene-bound %ld), flag ticks %d, hud frames %ld bundles %ld fx %ld",
             (fr - p_fr) / s, (nw - p_new) / s, ((fr - p_fr) - (nw - p_new)) / s, g_xrt_nolayer.load(), (im - p_img) / s,
             fr > p_fr ? (wt - p_wait) / 1000.0 / (fr - p_fr) : 0.0, g_flat_frames.load(), (int)g_flat_mode, g_np_lists.load(), g_np_scene_lists.load(), g_notplay_ticks.load(), g_hud_frames.load(), g_hud_bundles.load(), g_fx_bundles.load());
        p_fr = fr; p_new = nw; p_img = im; p_wait = wt;
    }
    if (g_ecam.tried) logf("EYE camera blocks=%ld missed=%ld half_offset=%.2f twins made=%ld used=%ld failed: create=%ld pool=%ld reset=%ld | scene copies blit %ld failed %ld, history used %ld | r0 wvp=%ld (clip fx %ld, screen proj %ld) plain=%ld (eye FOV %ld) | eye bundles untwinned=%ld twin peak/tick=%d",
                           g_ecam_blocks.load(),
                           g_ecam_miss.load(), eye_half_offset(), g_twin_made.load(), g_twin_used_in_replay.load(), g_twin_fail.load(),
                           g_twin_fail_pool.load(), g_twin_fail_reset.load(), g_blit_done.load(), g_blit_fail.load(), g_hist_used.load(), g_ecam_wvp.load(), g_ecam_r0_clipfx.load(), g_ecam_r0_screen.load(), g_ecam_r0_plain.load(), g_ecam_r0_fov.load(), g_eye_eb_untwinned.load(), g_twin_peak.load());
    if (inj_on()) {
        static long p[5] = {};
        long c[5];
        for (int i = 0; i < 5; ++i) { c[i] = g_seg_samples[i].load(); }
        logf("SCENE samples per segment (this window): 1x %ld, 2x %ld, 4x %ld, 8x %ld, other %ld", c[1] - p[1], c[2] - p[2], c[3] - p[3], c[4] - p[4], c[0] - p[0]);
        for (int i = 0; i < 5; ++i) p[i] = c[i];
    }
    if (inj_on() || g_inj_done.load()) logf("D3D inject mode=%d replays=%ld skipped=%ld (none %ld, few bundles %ld, locking %ld, other target %ld, no post stage %ld) at list end %ld, non-world %ld, MSAA-path replays %ld, xr images lost %ld ops_replayed=%ld pre_barriers=%ld",
                                           g_cfg.d3d_inject, g_inj_done.load(), g_inj_skipped.load(), g_skip[0].load(), g_skip[1].load(), g_skip[2].load(), g_skip[3].load(), g_skip[4].load(),
                                           g_inj_at_close.load(), g_inj_nonworld.load(), g_eye_resolves.load(), g_xr_pending_lost.load(), g_inj_ops.load(), g_inj_prebarriers.load());
    if (!g_nvts) return;
    D3dAgg a[2][3];
    {
        std::lock_guard<std::recursive_mutex> lk(g_d3d_mutex);
        memcpy(a, g_agg, sizeof(a));
        memset(g_agg, 0, sizeof(g_agg));
    }
    const char* tn[3] = {"direct", "bundle", "other"};
    for (int m = 1; m >= 0; --m)
        for (int t = 0; t < 3; ++t) {
            const D3dAgg& g = a[m][t];
            if (!g.lists) continue;
            logf("D3D %s %s: lists %.1f/s draws/list %.1f bundles/list %.1f rt_sets/list %.1f clears/list %.1f r2_distinct/list %.1f",
                 m ? "main " : "other", tn[t], 1000.0 * g.lists / win_ms, (double)g.draws / g.lists, (double)g.bundles / g.lists,
                 (double)g.rts / g.lists, (double)g.clears / g.lists, (double)g.r2 / g.lists);
        }
}

void patch_vtable(void** vtbl, const char* source) {
    for (auto& p : g_patched) if (p.vtbl == vtbl) { logf("vtable %p (%s) already patched", (void*)vtbl, source); return; }
    PatchedVtable p{vtbl, (PresentFn)vtbl[kPresentSlot], (Present1Fn)vtbl[kPresent1Slot]};
    if (p.orig_present == (PresentFn)hk_present) return;
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p.orig_present, &owner);
    wchar_t mod[MAX_PATH] = L"?";
    if (owner) GetModuleFileNameW(owner, mod, MAX_PATH);
    g_patched.push_back(p); // record originals before the slots go live
    bool ok = write_slot(vtbl, kPresentSlot, (void*)&hk_present) && write_slot(vtbl, kPresent1Slot, (void*)&hk_present1);
    logf("patch vtable %p from %s: %s (Present in %ls)", (void*)vtbl, source, ok ? "ok" : "FAILED", mod);
}

void unpatch_all() {
    for (auto& p : g_patched) {
        write_slot(p.vtbl, kPresentSlot, (void*)p.orig_present);
        write_slot(p.vtbl, kPresent1Slot, (void*)p.orig_present1);
    }
}

HWND make_dummy_window() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"kh2vr_probe_dummy";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"kh2vr_probe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

void install_hooks() {
    HWND hwnd = make_dummy_window();
    if (!hwnd) { logf("setup: dummy window failed (%lu); no hooks", GetLastError()); return; }

    // D3D11 dummy swapchain (blt model, the classic path).
    if (g_cfg.dummy_d3d11) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) { patch_vtable(*(void***)sc, "D3D11 dummy"); sc->Release(); ctx->Release(); dev->Release(); }
        else logf("setup: D3D11 dummy swapchain failed hr=0x%08lx", hr);
    }
    // D3D12 dummy swapchain (flip model, created via a command queue). Off by default; see header.
    if (g_cfg.dummy_d3d12) {
        IDXGIFactory4* fac = nullptr; ID3D12Device* dev = nullptr; ID3D12CommandQueue* q = nullptr; IDXGISwapChain1* sc = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&fac);
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
        if (SUCCEEDED(hr)) {
            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q);
        }
        if (SUCCEEDED(hr)) {
            DXGI_SWAP_CHAIN_DESC1 sd = {};
            sd.Width = 64; sd.Height = 64;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            hr = fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc);
        }
        if (SUCCEEDED(hr)) patch_vtable(*(void***)sc, "D3D12 dummy");
        else logf("setup: D3D12 dummy swapchain failed hr=0x%08lx", hr);
        if (sc) sc->Release();
        if (q) q->Release();
        if (dev) dev->Release();
        if (fac) fac->Release();
    }
    DestroyWindow(hwnd);
    logf("setup: %zu vtable(s) patched; waiting for the game's Present", g_patched.size());
}

void log_settings(const char* why) {
    logf("SETTINGS %s: enabled=%d stats_ms=%d tick_cap_hz=%d pad_log=%d stick_threshold=%.2f reload_ms=%d", why,
         g_cfg.enabled, g_cfg.stats_ms, g_cfg.tick_cap_hz, g_cfg.pad_log, g_cfg.stick_threshold, g_cfg.reload_ms);
}

// Main thread, start of each game tick. Waits until the next deadline of a fixed-rate schedule;
// if the game fell more than one period behind, the schedule restarts from now (no catch-up burst).
void pace_tick() {
    if (g_cfg.tick_cap_hz <= 0) { g_cap_deadline = 0; return; }
    LONGLONG period = g_qpf.QuadPart / g_cfg.tick_cap_hz;
    LONGLONG now = qpc();
    if (g_cap_deadline == 0 || now > g_cap_deadline + period) { g_cap_deadline = now + period; return; }
    LONGLONG start = now;
    while (now < g_cap_deadline) {
        double left_ms = now_ms(g_cap_deadline - now);
        if (left_ms > 2.0) Sleep(DWORD(left_ms - 1.5));
        else YieldProcessor();
        now = qpc();
    }
    g_cap_wait_us_in_window.fetch_add(LONGLONG(now_ms(now - start) * 1000.0));
    g_cap_deadline += period;
}

// XInput, as the game sees it (Steam Input's virtual pad included). Logs edges only.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
struct PadState { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct PadXState { DWORD packet; PadState pad; };
XInputGetStateFn g_xget = nullptr;
bool g_xinput_tried = false;
bool g_pad_connected[4] = {};
WORD g_pad_buttons[4] = {};
bool g_pad_moving[4] = {};
bool g_pad_rmoving[4] = {};
bool g_pad0_left_active = false;  // main thread only
bool g_pad0_right_active = false;
LONGLONG g_pad_move_start[4] = {};
LONGLONG g_pad_rescan = 0;
LONGLONG g_first_tick_qpc = 0;  // set at the first poll; PADREC times are relative to it

void poll_pads() {
    if (!g_cfg.pad_log) return;
    if (!g_xinput_tried) {
        g_xinput_tried = true;
        HMODULE x = GetModuleHandleW(L"XINPUT1_4.dll");
        if (!x) x = LoadLibraryW(L"XINPUT1_4.dll");
        if (x) g_xget = (XInputGetStateFn)GetProcAddress(x, "XInputGetState");
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("pad: XInputGetState %s", g_xget ? "found" : "NOT found; pad logging off");
    }
    if (!g_xget) return;
    LONGLONG now = qpc();
    if (!g_first_tick_qpc) g_first_tick_qpc = now;
    // Polling a disconnected slot is slow, so only connected slots every tick; others every 2 s.
    bool rescan = now > g_pad_rescan;
    if (rescan) g_pad_rescan = now + 2 * g_qpf.QuadPart;
    for (DWORD i = 0; i < 4; ++i) {
        if (!g_pad_connected[i] && !rescan) continue;
        PadXState s = {};
        bool ok = g_xget(i, &s) == ERROR_SUCCESS;
        if (ok != g_pad_connected[i]) {
            g_pad_connected[i] = ok;
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("PAD%lu %s tick=%lld", i, ok ? "connected" : "disconnected", g_total_ticks.load());
        }
        if (!ok) continue;
        WORD b = s.pad.buttons;
        double mag = std::min(1.0, std::sqrt(double(s.pad.lx) * s.pad.lx + double(s.pad.ly) * s.pad.ly) / 32767.0);
        bool moving = mag >= g_cfg.stick_threshold;
        double rmag = std::min(1.0, std::sqrt(double(s.pad.rx) * s.pad.rx + double(s.pad.ry) * s.pad.ry) / 32767.0);
        bool rmoving = rmag >= g_cfg.stick_threshold;
        if (i == 0) { g_pad0_left_active = moving; g_pad0_right_active = rmoving; }
        if (i == 0 && g_cfg.pad_record) {
            // Quantised state; a change in any field logs one PADREC line (ms since the probe's first tick).
            static int last[7] = {-1, 0, 0, 0, 0, 0, 0};
            auto qa = [](SHORT v) {
                double x = v / 32767.0;
                if (std::fabs(x) < g_cfg.pad_record_deadzone) return 0;
                return (int)std::lround(std::min(1.0, std::max(-1.0, x)) / g_cfg.pad_record_q);
            };
            auto qt = [](BYTE v) { return (int)std::lround(v / 255.0 / g_cfg.pad_record_q); };
            int cur[7] = {b, qa(s.pad.lx), qa(s.pad.ly), qa(s.pad.rx), qa(s.pad.ry), qt(s.pad.lt), qt(s.pad.rt)};
            if (memcmp(cur, last, sizeof(cur)) != 0) {
                memcpy(last, cur, sizeof(cur));
                double q = g_cfg.pad_record_q;
                std::lock_guard<std::mutex> lk(g_log_mutex);
                logf("PADREC %.0f 0x%04x %.2f %.2f %.2f %.2f %.2f %.2f tick=%lld", now_ms(now - g_first_tick_qpc), b, cur[1] * q, cur[2] * q,
                     cur[3] * q, cur[4] * q, cur[5] * q, cur[6] * q, g_total_ticks.load());
            }
        }
        if (rmoving != g_pad_rmoving[i]) {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("PAD%lu STICK_R %s mag=%.2f tick=%lld", i, rmoving ? "move start" : "move end", rmag, g_total_ticks.load());
            g_pad_rmoving[i] = rmoving;
        }
        if (b != g_pad_buttons[i] || moving != g_pad_moving[i]) {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            if (b != g_pad_buttons[i])
                logf("PAD%lu btn=0x%04x down=0x%04x up=0x%04x tick=%lld presents=%lld", i, b, WORD(b & ~g_pad_buttons[i]),
                     WORD(g_pad_buttons[i] & ~b), g_total_ticks.load(), g_total_presents);
            if (moving != g_pad_moving[i]) {
                if (moving) {
                    g_pad_move_start[i] = now;
                    logf("PAD%lu STICK_L move start mag=%.2f tick=%lld", i, mag, g_total_ticks.load());
                } else {
                    logf("PAD%lu STICK_L move end held_ms=%.0f tick=%lld", i, now_ms(now - g_pad_move_start[i]), g_total_ticks.load());
                }
            }
            g_pad_buttons[i] = b;
            g_pad_moving[i] = moving;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Test driver: scripted pad input at the game's own XInput boundary (no virtual device, no focus).
// With drive=1 the game's import of XInputGetState (and GetForegroundWindow, used only while
// drive_focus=1) is redirected through its import table; the probe's own pad log keeps calling the real
// function, so the log still shows the physical pad. While a script plays, slot drive_pad returns the
// script's state; afterwards a neutral pad is held for drive_release_ms (so a held button never leaks
// into whatever comes next), then the real pad passes through again. A script line is
//   <duration_ms> <buttons|-> [lx ly rx ry [lt rt]]   # optional note, logged when the step starts
// buttons joined by '+': A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT; sticks -1..1 (up = +y),
// triggers 0..1. A requested press is not proof: tests check the outcome (screen, camera, log).
struct DriveStep { double ms; PadState s; std::string note; };
std::mutex g_drive_mutex;                 // guards g_drive_now / g_drive_inject
std::vector<DriveStep> g_drive_steps;     // main thread only
PadState g_drive_now = {};
bool g_drive_inject = false;
DWORD g_drive_packet = 0;
LONGLONG g_drive_start = 0, g_drive_end = 0, g_drive_stats = 0;
int g_drive_seq_done = -1;
size_t g_drive_step = SIZE_MAX;
bool g_drive_playing = false, g_drive_releasing = false;
XInputGetStateFn g_xget_real = nullptr;
using GetFgFn = HWND(WINAPI*)();
GetFgFn g_getfg_real = nullptr;
std::atomic<long> g_game_xget_calls{0}, g_game_xget_injected{0}, g_game_fg_calls{0};
struct IatPatch { void** slot; void* orig; };
std::vector<IatPatch> g_iat;
bool g_drive_installed = false;

DWORD WINAPI xget_hook(DWORD i, void* out) {
    g_game_xget_calls.fetch_add(1);
    if (out && i == (DWORD)g_cfg.drive_pad) {
        std::lock_guard<std::mutex> lk(g_drive_mutex);
        if (g_drive_inject) {
            auto* x = (PadXState*)out;
            x->packet = ++g_drive_packet;
            x->pad = g_drive_now;
            g_game_xget_injected.fetch_add(1);
            return ERROR_SUCCESS;
        }
    }
    return g_xget_real ? g_xget_real(i, out) : ERROR_DEVICE_NOT_CONNECTED;
}

HWND WINAPI getfg_hook() {
    g_game_fg_calls.fetch_add(1);
    if (g_cfg.drive && g_cfg.drive_focus && g_game_hwnd) return g_game_hwnd;
    return g_getfg_real ? g_getfg_real() : nullptr;
}

// Replace every import-table entry of the game exe that points at `target`. Returns entries patched.
int patch_imports(void* target, void* repl) {
    auto* base = (BYTE*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        for (auto* t = (void**)(base + d->FirstThunk); *t; ++t) {
            if (*t != target) continue;
            DWORD old;
            if (!VirtualProtect(t, sizeof(void*), PAGE_READWRITE, &old)) continue;
            g_iat.push_back({t, *t});
            *t = repl;
            VirtualProtect(t, sizeof(void*), old, &old);
            ++n;
        }
    }
    return n;
}

void drive_unpatch() {
    for (auto& p : g_iat) {
        DWORD old;
        if (VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old)) { *p.slot = p.orig; VirtualProtect(p.slot, sizeof(void*), old, &old); }
    }
    g_iat.clear();
}

void drive_install() {
    g_drive_installed = true;
    HMODULE x = GetModuleHandleW(L"XINPUT1_4.dll");
    HMODULE u = GetModuleHandleW(L"USER32.dll");
    g_xget_real = x ? (XInputGetStateFn)GetProcAddress(x, "XInputGetState") : nullptr;
    g_getfg_real = u ? (GetFgFn)GetProcAddress(u, "GetForegroundWindow") : nullptr;
    int nx = g_xget_real ? patch_imports((void*)g_xget_real, (void*)&xget_hook) : 0;
    int nf = g_getfg_real ? patch_imports((void*)g_getfg_real, (void*)&getfg_hook) : 0;
    if (nx == 0) g_xget_real = nullptr; // fail closed: never inject without the game's own import redirected
    logf("DRIVE install: XInputGetState imports patched=%d GetForegroundWindow imports patched=%d%s", nx, nf,
         nx == 0 ? " (no XInput import found: scripted pad OFF)" : "");
}

WORD drive_button(const std::string& n) {
    static const struct { const char* n; WORD b; } t[] = {{"UP", 0x1}, {"DOWN", 0x2}, {"LEFT", 0x4}, {"RIGHT", 0x8},
        {"START", 0x10}, {"BACK", 0x20}, {"LS", 0x40}, {"RS", 0x80}, {"LB", 0x100}, {"RB", 0x200}, {"A", 0x1000},
        {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}};
    for (auto& e : t) if (n == e.n) return e.b;
    return 0xffff;
}

SHORT drive_axis(double v) { v = std::min(1.0, std::max(-1.0, v)); return (SHORT)(v * 32767.0); }

// Parses the script; logs and returns false (nothing played) on any bad line.
bool drive_load(const std::wstring& path, std::vector<DriveStep>& out) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) { logf("DRIVE script not readable: %ls", path.c_str()); return false; }
    char line[512];
    int ln = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        ++ln;
        std::string l = line, note;
        size_t h = l.find('#');
        if (h != std::string::npos) { note = l.substr(h + 1); l = l.substr(0, h); }
        while (!note.empty() && (note.back() == '\n' || note.back() == '\r' || note.back() == ' ')) note.pop_back();
        while (!note.empty() && note.front() == ' ') note.erase(0, 1);
        double ms = 0, a[6] = {0, 0, 0, 0, 0, 0};
        char btn[128] = {};
        int got = sscanf(l.c_str(), "%lf %127s %lf %lf %lf %lf %lf %lf", &ms, btn, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
        if (got <= 0) continue; // blank or comment
        if (got < 2 || ms <= 0) { logf("DRIVE script line %d: need '<ms> <buttons>'", ln); ok = false; break; }
        DriveStep s{ms, {}, note};
        std::string b = btn;
        if (b != "-") {
            size_t p = 0;
            while (true) {
                size_t q = b.find('+', p);
                std::string tok = b.substr(p, q == std::string::npos ? std::string::npos : q - p);
                WORD w = drive_button(tok);
                if (w == 0xffff) { logf("DRIVE script line %d: unknown button '%s'", ln, tok.c_str()); ok = false; break; }
                s.s.buttons |= w;
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (!ok) break;
        }
        s.s.lx = drive_axis(a[0]); s.s.ly = drive_axis(a[1]); s.s.rx = drive_axis(a[2]); s.s.ry = drive_axis(a[3]);
        s.s.lt = (BYTE)(std::min(1.0, std::max(0.0, a[4])) * 255); s.s.rt = (BYTE)(std::min(1.0, std::max(0.0, a[5])) * 255);
        out.push_back(s);
    }
    fclose(f);
    if (ok && out.empty()) { logf("DRIVE script %ls has no steps", path.c_str()); ok = false; }
    return ok;
}

void drive_set(bool inject, const PadState& s) {
    std::lock_guard<std::mutex> lk(g_drive_mutex);
    g_drive_inject = inject;
    g_drive_now = s;
}

// Main thread, once per tick.
void drive_update() {
    if (!g_cfg.drive) {
        if (g_drive_playing || g_drive_releasing) {
            drive_set(false, {});
            g_drive_playing = g_drive_releasing = false;
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("DRIVE stopped (drive=0)");
        }
        return;
    }
    if (!g_drive_installed) { std::lock_guard<std::mutex> lk(g_log_mutex); drive_install(); }
    LONGLONG now = qpc();
    if (g_cfg.drive_seq != g_drive_seq_done) {
        bool first = g_drive_seq_done == -1;
        g_drive_seq_done = g_cfg.drive_seq;
        if (!first || g_cfg.drive_seq > 0) {
            std::vector<DriveStep> steps;
            std::lock_guard<std::mutex> lk(g_log_mutex);
            if (!g_xget_real) logf("DRIVE seq %d refused: no XInput import patched", g_cfg.drive_seq);
            else if (drive_load(g_cfg.drive_script, steps)) {
                double total = 0;
                for (auto& s : steps) total += s.ms;
                g_drive_steps = steps;
                g_drive_start = now;
                g_drive_step = SIZE_MAX;
                g_drive_playing = true;
                g_drive_releasing = false;
                logf("DRIVE start seq %d: %zu steps, %.0f ms, %ls tick=%lld", g_cfg.drive_seq, steps.size(), total, g_cfg.drive_script.c_str(),
                     g_total_ticks.load());
            }
        }
    }
    if (g_drive_playing) {
        double t = now_ms(now - g_drive_start), acc = 0;
        size_t i = 0;
        for (; i < g_drive_steps.size(); ++i) { if (t < acc + g_drive_steps[i].ms) break; acc += g_drive_steps[i].ms; }
        if (i < g_drive_steps.size()) {
            if (i != g_drive_step) {
                g_drive_step = i;
                const auto& s = g_drive_steps[i];
                drive_set(true, s.s);
                std::lock_guard<std::mutex> lk(g_log_mutex);
                logf("DRIVE step %zu btn=0x%04x L=(%d %d) R=(%d %d) ms=%.0f tick=%lld%s%s", i, s.s.buttons, s.s.lx, s.s.ly, s.s.rx, s.s.ry, s.ms,
                     g_total_ticks.load(), s.note.empty() ? "" : " | ", s.note.c_str());
            }
        } else {
            g_drive_playing = false;
            g_drive_releasing = true;
            g_drive_end = now;
            drive_set(true, {});
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("DRIVE done seq %d tick=%lld; neutral for %d ms", g_cfg.drive_seq, g_total_ticks.load(), g_cfg.drive_release_ms);
        }
    }
    if (g_drive_releasing && now_ms(now - g_drive_end) >= g_cfg.drive_release_ms) {
        g_drive_releasing = false;
        drive_set(false, {});
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("DRIVE released: real pad passes through tick=%lld", g_total_ticks.load());
    }
    if (now > g_drive_stats) {
        g_drive_stats = now + 5 * g_qpf.QuadPart;
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("DRIVE stats game_xinput_polls=%ld injected=%ld game_foreground_calls=%ld playing=%d focus_override=%d",
             g_game_xget_calls.load(), g_game_xget_injected.load(), g_game_fg_calls.load(), (int)g_drive_playing, (int)g_cfg.drive_focus);
    }
}

// ---------------------------------------------------------------------------------------------
// Camera discovery: find affine 4x4 float matrices in the game's writable memory, then score each by
// how often it changes while only the right stick (camera) is held versus while the pad is idle.
// Commands arrive through the ini (cmd=<name>, cmd_seq=<n>; a new cmd_seq runs the command once on
// the main thread): scan, report, clear. Addresses are logged as exe+RVA when inside the game image.

std::atomic<bool> g_ptrack{false};

struct Candidate {
    uintptr_t addr;
    float last[16];
    uint32_t orbit_changes, rest_changes;
    bool dead;
    int layout; // 4 = padded rows, 3 = packed 3x3
};
std::vector<Candidate> g_cands;
size_t g_n_layout4 = 0, g_n_layout3 = 0;
uint32_t g_orbit_ticks = 0, g_rest_ticks = 0;
int g_settle = 0;
bool g_tracking = false;

bool safe_copy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A 3x3 rotation whose rows start every `stride` floats (4 = rows padded to 16 bytes as in 3x4/4x4
// matrices, 3 = packed 3x3). Translation/4th row are not constrained: layouts differ between engines.
bool is_rotation(const float* m, int stride) {
    const float* r0 = m; const float* r1 = m + stride; const float* r2 = m + 2 * stride;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float tol = 2e-3f;
    float l0 = dot(r0, r0);
    if (!(std::fabs(l0 - 1) <= tol)) return false; // cheap first reject; also rejects NaN
    if (!(std::fabs(dot(r1, r1) - 1) <= tol) || !(std::fabs(dot(r2, r2) - 1) <= tol)) return false;
    if (!(std::fabs(dot(r0, r1)) <= tol) || !(std::fabs(dot(r0, r2)) <= tol) || !(std::fabs(dot(r1, r2)) <= tol)) return false;
    // Reject axis-aligned matrices (every entry 0 or +-1): identity/static slots, far too common.
    bool axis = true;
    for (const float* r : {r0, r1, r2})
        for (int k = 0; k < 3; ++k) if (r[k] != 0.0f && r[k] != 1.0f && r[k] != -1.0f) axis = false;
    return !axis;
}

// Layout code: 4 or 3 (see is_rotation), 0 = none.
int rotation_layout(const float* m) {
    if (is_rotation(m, 4)) return 4;
    if (is_rotation(m, 3)) return 3;
    return 0;
}

std::string where(uintptr_t a) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    uintptr_t base = (uintptr_t)exe, end = base + nt->OptionalHeader.SizeOfImage;
    char buf[64];
    if (a >= base && a < end) snprintf(buf, sizeof(buf), "exe+0x%llx", (unsigned long long)(a - base));
    else snprintf(buf, sizeof(buf), "heap:0x%llx", (unsigned long long)a);
    return buf;
}

std::mutex g_cand_mutex; // held while the candidate list is rebuilt; the present hook only try_locks

// State-flag discovery: snapshots of the game image's writable data tagged with a state label (bscan_label:
// 0 field, 1 inventory menu, 2 battle pause, ...); bscan_find lists bytes that hold one constant value in every
// snapshot of label 0 and a different constant in every snapshot of label 1 (and, if present, label 2).
struct BRegion { uintptr_t base; size_t size; };
std::vector<BRegion> g_bregions;
std::vector<std::pair<int, std::vector<uint8_t>>> g_bsnaps;
void bscan_regions() {
    g_bregions.clear();
    HMODULE exe = GetModuleHandleW(nullptr);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = (uintptr_t)exe;
    while (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.AllocationBase == (void*)exe) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress; size_t size = mbi.RegionSize;
        p = base + size;
        DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY) && !(mbi.Protect & PAGE_GUARD)) g_bregions.push_back({base, size});
    }
}
void cmd_bscan_snap() {
    if (g_bregions.empty()) bscan_regions();
    size_t total = 0;
    for (auto& r : g_bregions) total += r.size;
    std::vector<uint8_t> snap(total);
    size_t o = 0;
    for (auto& r : g_bregions) { if (!safe_copy(snap.data() + o, (void*)r.base, r.size)) memset(snap.data() + o, 0, r.size); o += r.size; }
    g_bsnaps.push_back({g_cfg.bscan_label, std::move(snap)});
    logf("BSCAN snap #%zu label=%d: %zu regions, %.1f MB", g_bsnaps.size(), g_cfg.bscan_label, g_bregions.size(), total / 1048576.0);
}
void cmd_bscan_find() {
    if (g_bsnaps.empty()) { logf("BSCAN: no snapshots"); return; }
    size_t n = g_bsnaps[0].second.size();
    uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
    int hits = 0, shown = 0;
    bool have2 = false;
    for (auto& sn : g_bsnaps) if (sn.first == 2) have2 = true;
    for (size_t i = 0; i < n; ++i) {
        int v[3] = {-1, -1, -1};
        bool ok = true;
        for (auto& sn : g_bsnaps) {
            if (sn.first < 0 || sn.first > 2) continue;
            int x = sn.second[i];
            int& slot = v[sn.first];
            if (slot < 0) slot = x; else if (slot != x) { ok = false; break; }
        }
        if (!ok || v[0] < 0 || v[1] < 0 || v[0] == v[1]) continue;
        if (have2 && (v[2] < 0 || v[2] == v[0])) continue;
        ++hits;
        if (g_cfg.bscan_small && (v[0] > 3 || v[1] > 3 || (have2 && v[2] > 3))) continue;
        if (shown >= 150) continue;
        ++shown;
        size_t o = i; uintptr_t addr = 0;
        for (auto& r : g_bregions) { if (o < r.size) { addr = r.base + o; break; } o -= r.size; }
        logf("BSCAN hit exe+0x%llx: field=%d menu=%d pause=%d", (unsigned long long)(addr - exe), v[0], v[1], v[2]);
    }
    logf("BSCAN find: %d hits (%d shown%s) over %zu snapshots (%zu bytes each)", hits, shown, g_cfg.bscan_small ? ", values 0..3 only" : "", g_bsnaps.size(), n);
}
void cmd_scan() {
    std::lock_guard<std::mutex> cl(g_cand_mutex);
    g_ptrack = false;
    LONGLONG t0 = qpc();
    g_cands.clear();
    g_n_layout4 = g_n_layout3 = 0;
    g_orbit_ticks = g_rest_ticks = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&cmd_scan, &self);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0;
    size_t scanned = 0, regions = 0, truncated = 0;
    std::vector<uint8_t> buf(1 << 20);
    HMODULE exe = GetModuleHandleW(nullptr);
    while (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        p = base + size;
        if (p <= base) break;
        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        if (!writable || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))) continue;
        if (mbi.Type == MEM_MAPPED) continue;
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue; // only the game's own image
        if (mbi.AllocationBase == (void*)self) continue;
        ++regions;
        for (uintptr_t off = 0; off < size; off += buf.size()) {
            size_t n = std::min<uintptr_t>(buf.size(), size - off);
            if (!safe_copy(buf.data(), (void*)(base + off), n)) continue;
            scanned += n;
            for (size_t i = 0; i + 64 <= n; i += 4) {
                const float* m = (const float*)(buf.data() + i);
                int layout = rotation_layout(m);
                if (!layout) continue;
                if ((int)g_cands.size() >= g_cfg.scan_max_candidates) { ++truncated; continue; }
                ++(layout == 4 ? g_n_layout4 : g_n_layout3);
                Candidate c{base + off + i, {}, 0, 0, false, layout};
                memcpy(c.last, m, 64);
                g_cands.push_back(c);
            }
        }
    }
    g_tracking = true;
    logf("SCAN regions=%zu scanned_MB=%.0f candidates=%zu (padded_rows=%zu packed3x3=%zu) truncated=%zu took_ms=%.0f", regions,
         scanned / 1048576.0, g_cands.size(), g_n_layout4, g_n_layout3, truncated, now_ms(qpc() - t0));
}

// find_mv: find CPU-built model-view matrices M = W x V (row-vector), where V is the view matrix at
// `addr` (rows +0, translation +0x30) and W is a world matrix with only a yaw and KH2's y flip
// (W row1 = (0,-1,0)), as Sora's draws carry (RenderDoc 2026-11-10). For every 16-byte-aligned rotation
// R_M in memory: R_W = R_M x R_V^T must be such a yaw-flip; then t_W = (t_M - t_V) x R_V^T is the
// object's world position. mv_scan_wc=1 also reads write-combined/mapped memory (GPU upload buffers;
// read-only, slow). Logs up to max_hits hits with address, world position, yaw and memory type.
uintptr_t parse_addr(const std::wstring& a);
void cmd_find_mv() {
    uintptr_t va = parse_addr(g_cfg.addr);
    float V[16];
    if (!va || !safe_copy(V, (void*)va, 64) || !is_rotation(V, 4)) { logf("find_mv: no view matrix at %ls", g_cfg.addr.c_str()); return; }
    LONGLONG t0 = qpc();
    HMODULE self = nullptr, exe = GetModuleHandleW(nullptr);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&cmd_find_mv, &self);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0;
    size_t scanned = 0, rots = 0;
    int hits = 0;
    std::vector<uint8_t> buf(1 << 20);
    const float tol = g_cfg.mv_tol;
    while (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi) && hits < g_cfg.max_hits) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        p = base + size;
        if (p <= base) break;
        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        bool wc = (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0 || mbi.Type == MEM_MAPPED;
        if (!writable || (mbi.Protect & PAGE_GUARD)) continue;
        if (wc && !g_cfg.mv_scan_wc) continue;
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue;
        if (mbi.AllocationBase == (void*)self) continue;
        for (uintptr_t off = 0; off < size && hits < g_cfg.max_hits; off += buf.size()) {
            size_t n = std::min<uintptr_t>(buf.size(), size - off);
            if (!safe_copy(buf.data(), (void*)(base + off), n)) continue;
            scanned += n;
            for (size_t i = 0; i + 64 <= n; i += 16) {
                const float* m = (const float*)(buf.data() + i);
                if (base + off + i == va) continue;
                if (!is_rotation(m, 4)) continue;
                ++rots;
                float W[3][3];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) W[r][c] = m[r * 4] * V[c * 4] + m[r * 4 + 1] * V[c * 4 + 1] + m[r * 4 + 2] * V[c * 4 + 2];
                if (std::fabs(W[1][0]) > tol || std::fabs(W[1][1] + 1) > tol || std::fabs(W[1][2]) > tol) continue;
                if (std::fabs(W[0][1]) > tol || std::fabs(W[2][1]) > tol) continue;
                float d[3] = {m[12] - V[12], m[13] - V[13], m[14] - V[14]};
                float tw[3];
                for (int c = 0; c < 3; ++c) tw[c] = d[0] * V[c * 4] + d[1] * V[c * 4 + 1] + d[2] * V[c * 4 + 2];
                float yaw = std::atan2(W[0][2], W[0][0]) * 57.29578f;
                ++hits;
                logf("  MV %s %s world_t=(%.1f %.1f %.1f) yaw=%.1f mv_t=(%.1f %.1f %.1f) w=%.3f", where(base + off + i).c_str(),
                     wc ? "WC/mapped" : "private", tw[0], tw[1], tw[2], yaw, m[12], m[13], m[14], m[15]);
            }
        }
    }
    logf("FIND_MV view=%ls V_t=(%.1f %.1f %.1f) scanned_MB=%.0f rotations=%zu hits=%d%s took_ms=%.0f", g_cfg.addr.c_str(), V[12], V[13], V[14],
         scanned / 1048576.0, rots, hits, hits >= g_cfg.max_hits ? " (max_hits reached)" : "", now_ms(qpc() - t0));
}

// Field monitor: log the values at the `mon` addresses every mon_ms (main thread).
LONGLONG g_mon_next = 0;
void monitor_fields() {
    if (g_cfg.mon.empty()) return;
    LONGLONG now = qpc();
    if (!g_cfg.mon_change) {
        if (now < g_mon_next) return;
        g_mon_next = now + g_qpf.QuadPart * g_cfg.mon_ms / 1000;
    }
    uintptr_t base = g_cfg.mon_base.empty() ? 0 : parse_addr(g_cfg.mon_base);
    std::string line;
    std::wstring s = g_cfg.mon;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(L',', p);
        std::wstring tok = s.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p);
        uintptr_t a = tok.rfind(L"+", 0) == 0 ? base + (uintptr_t)wcstoull(tok.c_str() + 1, nullptr, 0) : parse_addr(tok);
        int32_t iv = 0;
        char b[96];
        uint8_t bv = 0;
        if (g_cfg.mon_bytes && a && safe_copy(&bv, (void*)a, 1)) snprintf(b, sizeof(b), " %ls=%u", tok.c_str(), bv);
        else if (a && safe_copy(&iv, (void*)a, 4)) {
            float fv;
            memcpy(&fv, &iv, 4);
            snprintf(b, sizeof(b), " %ls=%d/%.4g", tok.c_str(), iv, fv);
        } else snprintf(b, sizeof(b), " %ls=?", tok.c_str());
        line += b;
        if (q == std::wstring::npos) break;
        p = q + 1;
    }
    static std::string last;
    if (g_cfg.mon_change) { if (line == last) return; last = line; } // only when something changed (every tick)
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("MON tick=%lld%s", g_total_ticks.load(), line.c_str());
}

// find_floats: find a sequence of floats (setting floats=a,b,*,c ; '*' = any value) at 4-byte steps,
// each within float_tol relative (or absolute below 1). mv_scan_wc=1 also reads write-combined/mapped
// memory. Logs up to max_hits addresses with the 8 floats found there.
void cmd_find_floats() {
    std::vector<float> want;
    std::vector<bool> any;
    {
        std::wstring s = g_cfg.floats;
        size_t p = 0;
        while (p <= s.size()) {
            size_t q = s.find(L',', p);
            std::wstring tok = s.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p);
            any.push_back(tok == L"*");
            want.push_back(tok == L"*" ? 0.0f : (float)_wtof(tok.c_str()));
            if (q == std::wstring::npos) break;
            p = q + 1;
        }
    }
    if (want.empty() || any[0]) { logf("find_floats: need floats=a,b,... (first not '*')"); return; }
    LONGLONG t0 = qpc();
    HMODULE self = nullptr, exe = GetModuleHandleW(nullptr);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&cmd_find_floats, &self);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0;
    size_t scanned = 0, nw = want.size();
    int hits = 0;
    std::vector<uint8_t> buf((1 << 20) + 64);
    auto near_ = [&](float v, float w) { float d = std::fabs(v - w); return d <= g_cfg.float_tol * std::max(1.0f, std::fabs(w)); };
    while (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi) && hits < g_cfg.max_hits) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        p = base + size;
        if (p <= base) break;
        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        bool wc = (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0 || mbi.Type == MEM_MAPPED;
        if (!writable || (mbi.Protect & PAGE_GUARD)) continue;
        if (wc && !g_cfg.mv_scan_wc) continue;
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue;
        if (mbi.AllocationBase == (void*)self) continue;
        for (uintptr_t off = 0; off < size && hits < g_cfg.max_hits; off += (1 << 20)) {
            size_t n = std::min<uintptr_t>(buf.size(), size - off);
            if (!safe_copy(buf.data(), (void*)(base + off), n)) continue;
            scanned += std::min<size_t>(n, 1 << 20);
            for (size_t i = 0; i + nw * 4 <= n && i < (1u << 20) && hits < g_cfg.max_hits; i += 4) {
                const float* f = (const float*)(buf.data() + i);
                bool ok = true;
                for (size_t k = 0; k < nw && ok; ++k) ok = any[k] || near_(f[k], want[k]);
                if (!ok) continue;
                ++hits;
                float v[8] = {};
                safe_copy(v, (void*)(base + off + i), sizeof(v));
                logf("  FLOATS %s %s | %.5g %.5g %.5g %.5g %.5g %.5g %.5g %.5g", where(base + off + i).c_str(), wc ? "WC/mapped" : "private",
                     v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            }
        }
    }
    logf("FIND_FLOATS %ls scanned_MB=%.0f hits=%d%s took_ms=%.0f", g_cfg.floats.c_str(), scanned / 1048576.0, hits,
         hits >= g_cfg.max_hits ? " (max_hits reached)" : "", now_ms(qpc() - t0));
}

// Each tick: classify the pad state, then count which candidates changed since the last tick.
void track_candidates() {
    if (!g_tracking || g_cands.empty()) return;
    bool orbit = g_pad0_right_active && !g_pad0_left_active;
    bool idle = !g_pad0_right_active && !g_pad0_left_active;
    if (!idle) g_settle = g_cfg.rest_settle_ticks; // camera may glide on after release
    else if (g_settle > 0) --g_settle;
    bool rest = idle && g_settle == 0;
    if (orbit) ++g_orbit_ticks;
    if (rest) ++g_rest_ticks;
    float cur[16];
    for (auto& c : g_cands) {
        if (c.dead) continue;
        if (!safe_copy(cur, (void*)c.addr, 64)) { c.dead = true; continue; }
        if (memcmp(cur, c.last, 64) != 0) {
            if (orbit) ++c.orbit_changes;
            if (rest) ++c.rest_changes;
            memcpy(c.last, cur, 64);
        }
    }
}

void cmd_report() {
    if (g_cands.empty()) { logf("REPORT: no candidates (run cmd=scan first)"); return; }
    std::vector<const Candidate*> v;
    for (auto& c : g_cands) {
        if (c.dead) continue;
        if (g_cfg.report_min_rest >= 0 && ((int)c.rest_changes < g_cfg.report_min_rest || (int)c.rest_changes > g_cfg.report_max_rest)) continue;
        v.push_back(&c);
    }
    auto score = [](const Candidate* c) {
        double o = g_orbit_ticks ? double(c->orbit_changes) / g_orbit_ticks : 0;
        double r = g_rest_ticks ? double(c->rest_changes) / g_rest_ticks : 0;
        return o - r;
    };
    if (g_cfg.report_min_rest >= 0) std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->addr < b->addr; });
    else std::sort(v.begin(), v.end(), [&](auto a, auto b) { return score(a) > score(b); });
    size_t still_valid = 0;
    for (auto* c : v) { float m[16]; if (safe_copy(m, (void*)c->addr, 64) && is_rotation(m, c->layout)) ++still_valid; }
    logf("REPORT orbit_ticks=%u rest_ticks=%u live=%zu still_rotation=%zu (of %zu scanned)", g_orbit_ticks, g_rest_ticks, v.size(),
         still_valid, g_cands.size());
    int n = 0;
    for (auto* c : v) {
        if (n++ >= g_cfg.report_top) break;
        float m[16] = {};
        safe_copy(m, (void*)c->addr, 64);
        // If this is a view matrix with translation in m[12..14] (row-vector convention), the eye is -t * R^T.
        float tx = m[12], ty = m[13], tz = m[14];
        float ex = -(tx * m[0] + ty * m[1] + tz * m[2]);
        float ey = -(tx * m[4] + ty * m[5] + tz * m[6]);
        float ez = -(tx * m[8] + ty * m[9] + tz * m[10]);
        logf("  #%d %s layout=%d score=%.3f orbit_chg=%u rest_chg=%u | r0=(%.3f %.3f %.3f %.3f) r1=(%.3f %.3f %.3f %.3f) r2=(%.3f %.3f %.3f %.3f) r3=(%.2f %.2f %.2f %.2f) | eye_if_view=(%.2f %.2f %.2f)",
             n, where(c->addr).c_str(), c->layout, score(c), c->orbit_changes, c->rest_changes, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
             m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], ex, ey, ez);
    }
}

uintptr_t parse_addr(const std::wstring& a) {
    if (a.rfind(L"exe+", 0) == 0) return (uintptr_t)GetModuleHandleW(nullptr) + (uintptr_t)wcstoull(a.c_str() + 4, nullptr, 0);
    return (uintptr_t)wcstoull(a.c_str(), nullptr, 0);
}

// Reads the view matrix at `addr`, derives eye-position candidates under the possible conventions,
// then searches writable memory for float3s within `tol` of any of them.
void cmd_find_eye() {
    uintptr_t a = parse_addr(g_cfg.addr);
    float m[16];
    if (!a || !safe_copy(m, (void*)a, 64)) { logf("find_eye: cannot read %ls", g_cfg.addr.c_str()); return; }
    float t[3] = {m[12], m[13], m[14]};
    float e[3][3] = {
        {-(t[0] * m[0] + t[1] * m[1] + t[2] * m[2]), -(t[0] * m[4] + t[1] * m[5] + t[2] * m[6]), -(t[0] * m[8] + t[1] * m[9] + t[2] * m[10])},
        {-(t[0] * m[0] + t[1] * m[4] + t[2] * m[8]), -(t[0] * m[1] + t[1] * m[5] + t[2] * m[9]), -(t[0] * m[2] + t[1] * m[6] + t[2] * m[10])},
        {t[0], t[1], t[2]}};
    const char* names[3] = {"-t*R^T", "-t*R", "t"};
    for (int k = 0; k < 3; ++k) logf("find_eye: variant %s = (%.3f %.3f %.3f)", names[k], e[k][0], e[k][1], e[k][2]);
    LONGLONG t0 = qpc();
    HMODULE self = nullptr, exe = GetModuleHandleW(nullptr);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&cmd_find_eye, &self);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0;
    int hits = 0;
    std::vector<uint8_t> buf(1 << 20);
    while (hits < g_cfg.max_hits && VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        p = base + size;
        if (p <= base) break;
        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        if (!writable || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) || mbi.Type == MEM_MAPPED) continue;
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue;
        if (mbi.AllocationBase == (void*)self) continue;
        for (uintptr_t off = 0; off < size && hits < g_cfg.max_hits; off += buf.size()) {
            size_t n = std::min<uintptr_t>(buf.size(), size - off);
            if (!safe_copy(buf.data(), (void*)(base + off), n)) continue;
            for (size_t i = 0; i + 12 <= n && hits < g_cfg.max_hits; i += 4) {
                const float* v = (const float*)(buf.data() + i);
                for (int k = 0; k < 3; ++k) {
                    if (std::fabs(v[0] - e[k][0]) <= g_cfg.tol && std::fabs(v[1] - e[k][1]) <= g_cfg.tol && std::fabs(v[2] - e[k][2]) <= g_cfg.tol) {
                        const float* w = v;
                        logf("  HIT %s variant=%s value=(%.3f %.3f %.3f) next=(%.3f %.3f %.3f %.3f)", where(base + off + i).c_str(), names[k],
                             w[0], w[1], w[2], i + 28 <= n ? w[3] : 0.f, i + 28 <= n ? w[4] : 0.f, i + 28 <= n ? w[5] : 0.f, i + 28 <= n ? w[6] : 0.f);
                        ++hits;
                        break;
                    }
                }
            }
        }
    }
    logf("find_eye: %d hit(s) in %.0f ms (tol %.2f)", hits, now_ms(qpc() - t0), g_cfg.tol);
}

// Holds `count` floats at addr at (value when frozen + add) by rewriting them at the start of every tick.
uintptr_t g_freeze_addr = 0;
float g_freeze_val[16];
int g_freeze_n = 0;
void cmd_freeze() {
    uintptr_t a = parse_addr(g_cfg.addr);
    float cur[16];
    if (!a || !safe_copy(cur, (void*)a, sizeof(float) * g_cfg.count)) { logf("freeze: cannot read %ls", g_cfg.addr.c_str()); return; }
    g_freeze_n = g_cfg.count;
    for (int i = 0; i < g_freeze_n; ++i) g_freeze_val[i] = cur[i] + (i < 3 ? g_cfg.add[i] : 0.0f);
    g_freeze_addr = a;
    logf("freeze %s n=%d from (%.3f %.3f %.3f) to (%.3f %.3f %.3f)", where(a).c_str(), g_freeze_n, cur[0], g_freeze_n > 1 ? cur[1] : 0.f,
         g_freeze_n > 2 ? cur[2] : 0.f, g_freeze_val[0], g_freeze_n > 1 ? g_freeze_val[1] : 0.f, g_freeze_n > 2 ? g_freeze_val[2] : 0.f);
}
bool safe_write(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void apply_freeze() {
    if (!g_freeze_addr) return;
    if (!safe_write((void*)g_freeze_addr, g_freeze_val, sizeof(float) * g_freeze_n)) { g_freeze_addr = 0; logf("freeze: write failed; released"); }
}

// Logs `count` floats at addr (plus 16 bytes either side) for eyeballing a struct.
void cmd_dump() {
    uintptr_t a = parse_addr(g_cfg.addr);
    float v[24];
    if (!a || !safe_copy(v, (void*)(a - 16), sizeof(v))) { logf("dump: cannot read %ls", g_cfg.addr.c_str()); return; }
    logf("dump %s-16: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
         where(a).c_str(), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17],
         v[18], v[19], v[20], v[21], v[22], v[23]);
}

// ---------------------------------------------------------------------------------------------
// Hardware write watchpoints (DR0-DR2, 4-byte, write-only) on every thread that exists when armed.
// A vectored handler records which instruction wrote (RIP after the write, as exe+RVA), on which
// thread, how often, and the exe-range return addresses on the stack for the first hit. No code bytes.

struct WatchHit {
    std::atomic<uintptr_t> rip{0};
    int slot = -1;
    std::atomic<long> count{0};
    DWORD tid = 0;
    uintptr_t stack[20] = {};
    int nstack = 0;
};
WatchHit g_whits[64];
thread_local bool g_in_adjust = false;
std::atomic<long> g_whit_overflow{0};
uintptr_t g_watch_addr[3] = {};
PVOID g_veh = nullptr;
uintptr_t g_exe_lo = 0, g_exe_hi = 0;

// Camera adjust, run inside the watchpoint handler (main thread, before the game builds the view matrix).
// The render-camera setter (fn exe+0x19d3d0 on build 15194255) writes the eye twice (vector copy, then a
// separate w store) and then the target once, so everything happens on the target write: at that point
// both vectors hold the game's final values for this tick, and each is edited exactly once.
// Eye-slot hits are only counted.
std::atomic<long> g_adjust_count{0};
std::atomic<long> g_adjust_refused{0};
std::atomic<long> g_eye_hits{0};
int g_watch_bytes[3] = {};

bool overlaps_watch(uintptr_t a, size_t n) {
    for (int s = 0; s < 3; ++s)
        if (g_watch_addr[s] && a < g_watch_addr[s] + g_watch_bytes[s] && g_watch_addr[s] < a + n) return true;
    return false;
}

void camera_adjust(bool is_eye) {
    if (is_eye) { g_eye_hits.fetch_add(1); return; }
    if (g_cfg.cam_eye_slot < 0 || !g_watch_addr[g_cfg.cam_eye_slot]) return;
    uintptr_t ea = g_watch_addr[g_cfg.cam_eye_slot] - g_cfg.cam_watch_offset;
    uintptr_t ta = g_watch_addr[g_cfg.cam_target_slot] - g_cfg.cam_watch_offset;
    // Fail closed: never write bytes that any armed watch slot covers (the write would re-trap in here).
    if (overlaps_watch(ea, 12) || overlaps_watch(ta, 12)) { g_adjust_refused.fetch_add(1); return; }
    float d = g_cfg.offset_right, yaw = g_cfg.yaw_deg;
    if (d == 0.0f && yaw == 0.0f) return;
    float eye[3], tgt[3];
    if (!safe_copy(eye, (void*)ea, sizeof(eye)) || !safe_copy(tgt, (void*)ta, sizeof(tgt))) return;
    float f[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    const float* u = g_cfg.cam_up;
    float r[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]}; // up x forward
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) return;
    for (float& c : r) c /= rl;
    // Target: rotate forward about up by yaw (Rodrigues; up is unit length), then translate like the eye.
    float th = yaw * 3.14159265f / 180.0f, c = std::cos(th), sn = std::sin(th);
    float kxf[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    float kdf = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
    float eye_out[3], tgt_out[3];
    for (int i = 0; i < 3; ++i) {
        eye_out[i] = eye[i] + r[i] * d;
        tgt_out[i] = eye[i] + f[i] * c + kxf[i] * sn + u[i] * kdf * (1 - c) + r[i] * d;
    }
    // The overlap guard means these stores can't trap; g_in_adjust stays as a second guard.
    g_in_adjust = true;
    safe_write((void*)ea, eye_out, sizeof(eye_out));
    safe_write((void*)ta, tgt_out, sizeof(tgt_out));
    g_in_adjust = false;
    g_adjust_count.fetch_add(1);
}

std::atomic<long> g_mat_edits_main{0}, g_mat_edits_other{0}, g_mat_refused{0};

void mat_adjust(bool on_main) {
    float th = g_cfg.mat_yaw_deg;
    if (th == 0.0f) return;
    if (!(g_cfg.mat_threads & (on_main ? 2 : 1))) return;
    uintptr_t rows = g_watch_addr[g_cfg.mat_slot] - g_cfg.mat_rows_offset;
    // mat_include_t: also rotate the translation row, i.e. rotate the whole camera-space result about the
    // camera's y axis (a true camera yaw if the matrix is a row-vector view matrix). Needs the watch on the
    // translation's w (mat_rows_offset = 60), since this writes translation xyz.
    int nrows = g_cfg.mat_include_t ? 4 : 3;
    size_t bytes = nrows == 4 ? 60 : 48;
    if (overlaps_watch(rows, bytes)) { g_mat_refused.fetch_add(1); return; }
    float m[16];
    if (!safe_copy(m, (void*)rows, bytes)) return;
    float r = th * 3.14159265f / 180.0f, c = std::cos(r), sn = std::sin(r);
    for (int i = 0; i < nrows; ++i) {
        float x = m[i * 4], z = m[i * 4 + 2];
        m[i * 4] = x * c + z * sn;
        m[i * 4 + 2] = -x * sn + z * c;
    }
    g_in_adjust = true;
    safe_write((void*)rows, m, bytes);
    g_in_adjust = false;
    (on_main ? g_mat_edits_main : g_mat_edits_other).fetch_add(1);
}

std::string func_of(uintptr_t addr);
std::atomic<int> g_exec_logged[3]; // per slot; reset when watches are (re)armed
std::atomic<long> g_pokes{0}, g_poke_refused{0};
// Replay experiment state (main thread only).
LONGLONG g_rp_t0 = 0;
long long g_rp_last_tick = -1;
int g_rp_done_this_tick = 0;
double g_rp_ms_first = 0, g_rp_ms_replay = 0, g_rp_max_first = 0, g_rp_max_replay = 0;
long g_rp_n_first = 0, g_rp_n_replay = 0;
bool g_rp_in_replay = false;
LONG CALLBACK watch_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = e->ContextRecord->Dr6;
    if (!(dr6 & 0x7)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_in_adjust) { e->ContextRecord->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    int slot = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : 2;
    uintptr_t rip = (uintptr_t)e->ContextRecord->Rip;
    bool on_main = GetCurrentThreadId() == g_frame_thread.load();
    uintptr_t key = rip ^ (uintptr_t(slot) << 60) ^ (uintptr_t(on_main ? 1 : 0) << 59);
    for (int i = 0; i < 64; ++i) {
        uintptr_t cur = g_whits[i].rip.load();
        if (cur == key) { g_whits[i].count.fetch_add(1); break; }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (g_whits[i].rip.compare_exchange_strong(expected, key)) {
                g_whits[i].slot = slot;
                g_whits[i].tid = GetCurrentThreadId();
                // Walk the interrupted frame's stack: plain scan for values inside the exe image near RSP.
                uintptr_t* sp = (uintptr_t*)e->ContextRecord->Rsp;
                int n = 0;
                for (int k = 0; k < 2048 && n < 20; ++k) {
                    uintptr_t v = 0;
                    if (!safe_copy(&v, sp + k, sizeof(v))) break;
                    DWORD64 ib = 0;
                    // keep only code addresses (an unwind entry exists): return addresses, not data pointers
                    if (v > g_exe_lo && v < g_exe_hi && RtlLookupFunctionEntry(v, &ib, nullptr)) g_whits[i].stack[n++] = v;
                }
                g_whits[i].nstack = n;
                g_whits[i].count.fetch_add(1);
                break;
            }
            if (expected == key) { g_whits[i].count.fetch_add(1); break; }
        }
        if (i == 63) g_whit_overflow.fetch_add(1);
    }
    if (g_cfg.watch_rw[slot] == 0) {
        // Execute breakpoint (fires before the instruction runs): log arguments, then set RF so the
        // instruction executes once without re-trapping.
        auto& logged = g_exec_logged;
        CONTEXT* x = e->ContextRecord;
        static uintptr_t want = 0;
        static std::wstring want_s;
        if (want_s != g_cfg.exec_arg) { want_s = g_cfg.exec_arg; want = want_s.empty() ? 0 : parse_addr(want_s); }
        bool match = !want || x->Rcx == want || x->Rdx == want || x->R8 == want || x->R9 == want;
        if (match && logged[slot].fetch_add(1) < g_cfg.exec_log) {
            uintptr_t ret = 0;
            safe_copy(&ret, (void*)x->Rsp, sizeof(ret));
            uintptr_t args[4] = {x->Rcx, x->Rdx, x->R8, x->R9};
            const char* nm[4] = {"rcx", "rdx", "r8", "r9"};
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("EXEC slot%d %s on_main=%d ret=%s%s rcx=%s rdx=%s r8=%s r9=%s", slot, where(rip).c_str(), (int)on_main, where(ret).c_str(),
                 func_of(ret).c_str(), where(args[0]).c_str(), where(args[1]).c_str(), where(args[2]).c_str(), where(args[3]).c_str());
            for (int k = 0; k < 4; ++k) {
                float m[16];
                if (args[k] < 0x10000 || !safe_copy(m, (void*)args[k], sizeof(m))) continue;
                logf("    %s -> r0=(%.3f %.3f %.3f %.3f) r1=(%.3f %.3f %.3f %.3f) r2=(%.3f %.3f %.3f %.3f) r3=(%.2f %.2f %.2f %.2f)", nm[k], m[0], m[1],
                     m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
        }
        static uintptr_t abs_t = 0;
        static std::wstring abs_s;
        if (abs_s != g_cfg.poke_abs) { abs_s = g_cfg.poke_abs; abs_t = abs_s.empty() ? 0 : parse_addr(abs_s); }
        bool when_ok = g_cfg.poke_when == 0 || (g_cfg.poke_when == 1) == g_rp_in_replay;
        if (slot == g_cfg.poke_slot && when_ok && (g_cfg.poke_reg >= 0 || abs_t) && g_cfg.poke_add != 0.0f) {
            uintptr_t regs[4] = {x->Rcx, x->Rdx, x->R8, x->R9};
            uintptr_t t = abs_t ? abs_t + g_cfg.poke_offset : regs[g_cfg.poke_reg] + g_cfg.poke_offset;
            float v;
            if (safe_copy(&v, (void*)t, 4)) {
                if (g_cfg.poke_int) { int32_t iv = (int32_t)g_cfg.poke_add; memcpy(&v, &iv, 4); }
                else v = g_cfg.poke_set ? g_cfg.poke_add : v + g_cfg.poke_add;
                safe_write((void*)t, &v, 4);
                g_pokes.fetch_add(1);
            } else g_poke_refused.fetch_add(1);
            // optional second target in the same call (same base and mode), e.g. a projection's x and y scales
            if (g_cfg.poke2_add != 0.0f) {
                uintptr_t t2 = (abs_t ? abs_t : regs[g_cfg.poke_reg]) + g_cfg.poke2_offset;
                float v2;
                if (safe_copy(&v2, (void*)t2, 4)) {
                    v2 = g_cfg.poke_set ? g_cfg.poke2_add : v2 + g_cfg.poke2_add;
                    safe_write((void*)t2, &v2, 4);
                }
            }
        }
        if (on_main && slot == g_cfg.replay_start_slot) g_rp_t0 = qpc();
        if (on_main && slot == g_cfg.replay_end_slot && g_rp_t0) {
            double ms = now_ms(qpc() - g_rp_t0);
            if (g_rp_in_replay) { g_rp_ms_replay += ms; g_rp_max_replay = std::max(g_rp_max_replay, ms); ++g_rp_n_replay; }
            else { g_rp_ms_first += ms; g_rp_max_first = std::max(g_rp_max_first, ms); ++g_rp_n_first; }
            g_rp_in_replay = false;
            long long tick = g_total_ticks.load();
            if (tick != g_rp_last_tick) { g_rp_last_tick = tick; g_rp_done_this_tick = 0; }
            static uintptr_t rp_rip = 0;
            static std::wstring rp_s;
            if (rp_s != g_cfg.replay_rip) { rp_s = g_cfg.replay_rip; rp_rip = rp_s.empty() ? 0 : parse_addr(rp_s); }
            if (rp_rip && g_rp_done_this_tick < g_cfg.replay_n) {
                ++g_rp_done_this_tick;
                g_rp_in_replay = true;
                g_rp_t0 = qpc();
                x->Rip = rp_rip; // re-run the argument setup and the call; RF below skips the start breakpoint once
            }
        }
        e->ContextRecord->EFlags |= 0x10000; // RF
    }
    if (slot == g_cfg.cam_eye_slot || slot == g_cfg.cam_target_slot) camera_adjust(slot == g_cfg.cam_eye_slot);
    if (slot == g_cfg.mat_slot) mat_adjust(on_main);
    if (slot == g_cfg.poke_slot && g_cfg.watch_rw[slot] != 0 && g_cfg.poke_reg < 0 && g_cfg.poke_add != 0.0f) {
        uintptr_t t = g_watch_addr[slot] + g_cfg.poke_offset;
        float v;
        if (overlaps_watch(t, 4)) g_poke_refused.fetch_add(1);
        else if (safe_copy(&v, (void*)t, 4)) {
            v += g_cfg.poke_add;
            g_in_adjust = true;
            safe_write((void*)t, &v, 4);
            g_in_adjust = false;
            g_pokes.fetch_add(1);
        }
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct ArmArgs { bool arm; uintptr_t addr[3]; int len_code[3]; int rw[3]; int armed_threads; int failed_threads; };

DWORD WINAPI arm_thread(LPVOID p) {
    auto* a = (ArmArgs*)p;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    DWORD tids[1024];
    int nt = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok && nt < 1024; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids[nt++] = te.th32ThreadID;
        CloseHandle(snap);
    }
    DWORD64 dr7 = 0;
    if (a->arm)
        for (int s = 0; s < 3; ++s)
            if (a->addr[s]) dr7 |= (1ull << (s * 2)) | (DWORD64(a->rw[s] == 3 ? 3 : a->rw[s] == 0 ? 0 : 1) << (16 + s * 4)) | (DWORD64(a->len_code[s]) << (18 + s * 4)); // local enable, RW, LEN
    for (int i = 0; i < nt; ++i) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tids[i]);
        if (!h) { ++a->failed_threads; continue; }
        // Nothing between Suspend and Resume allocates or takes locks.
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            bool ok = GetThreadContext(h, &c) != 0;
            if (ok) {
                c.Dr0 = a->addr[0]; c.Dr1 = a->addr[1]; c.Dr2 = a->addr[2]; c.Dr6 = 0; c.Dr7 = dr7;
                ok = SetThreadContext(h, &c) != 0;
            }
            ResumeThread(h);
            ok ? ++a->armed_threads : ++a->failed_threads;
        } else ++a->failed_threads;
        CloseHandle(h);
    }
    return 0;
}

void cmd_watch(bool arm) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_exe_lo = (uintptr_t)exe; g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    if (arm && !g_veh) g_veh = AddVectoredExceptionHandler(1, watch_handler);
    ArmArgs a{arm, {}, {}, {}, 0, 0};
    for (int s = 0; s < 3; ++s) { a.rw[s] = g_cfg.watch_rw[s]; g_exec_logged[s] = 0; }
    for (int s = 0; s < 3 && arm; ++s) {
        int len = g_cfg.watch_len[s];
        if (a.rw[s] == 0) len = 1; // execute breakpoints: LEN must be 00
        a.len_code[s] = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3; // DR7 LEN encoding for 1, 2, 8, 4 bytes
        int bytes = (len == 1 || len == 2 || len == 8) ? len : 4;
        a.addr[s] = g_cfg.watch[s].empty() ? 0 : (parse_addr(g_cfg.watch[s]) & ~uintptr_t(bytes - 1));
        g_watch_bytes[s] = a.addr[s] ? bytes : 0;
    }
    for (int s = 0; s < 3; ++s) g_watch_addr[s] = a.addr[s];
    // A helper thread does the work: a thread can't reliably set its own debug registers, and the main
    // thread (this one) gets suspended briefly like the others. Waiting here is safe: the helper takes no locks.
    HANDLE h = CreateThread(nullptr, 0, arm_thread, &a, 0, nullptr);
    if (h) { WaitForSingleObject(h, 5000); CloseHandle(h); }
    logf("%s: slots %s / %s / %s, threads set=%d failed=%d", arm ? "watch" : "unwatch", where(a.addr[0]).c_str(), where(a.addr[1]).c_str(),
         where(a.addr[2]).c_str(), a.armed_threads, a.failed_threads);
}

// "(fn exe+0xSTART)" when addr is code with an unwind entry, "(data)" otherwise. Uses only the exe's
// own exception directory (.pdata), no disassembly.
std::string func_of(uintptr_t addr) {
    DWORD64 base = 0;
    auto* rf = RtlLookupFunctionEntry((DWORD64)addr, &base, nullptr);
    if (!rf) return "(data)";
    char b[48];
    snprintf(b, sizeof(b), "(fn exe+0x%lx)", rf->BeginAddress);
    return b;
}

void cmd_watch_report() {
    logf("POKES done=%ld refused=%ld", g_pokes.load(), g_poke_refused.load());
    logf("REPLAY first calls=%ld mean_ms=%.3f max_ms=%.3f | replays=%ld mean_ms=%.3f max_ms=%.3f", g_rp_n_first,
         g_rp_n_first ? g_rp_ms_first / g_rp_n_first : 0.0, g_rp_max_first, g_rp_n_replay, g_rp_n_replay ? g_rp_ms_replay / g_rp_n_replay : 0.0,
         g_rp_max_replay);
    g_rp_ms_first = g_rp_ms_replay = g_rp_max_first = g_rp_max_replay = 0;
    g_rp_n_first = g_rp_n_replay = 0;
    logf("WATCH_REPORT ticks=%lld overflow=%ld camera_adjusts=%ld refused=%ld eye_hits=%ld mat_edits main=%ld other=%ld refused=%ld", g_total_ticks.load(), g_whit_overflow.load(),
         g_adjust_count.load(), g_adjust_refused.load(), g_eye_hits.load(), g_mat_edits_main.load(), g_mat_edits_other.load(), g_mat_refused.load());
    for (auto& h : g_whits) {
        uintptr_t key = h.rip.load();
        if (!key) continue;
        uintptr_t rip = key & ((uintptr_t(1) << 59) - 1);
        bool hit_main = (key >> 59) & 1;
        std::string st;
        for (int k = 0; k < h.nstack; ++k) { st += where(h.stack[k]); st += func_of(h.stack[k]); st += ' '; }
        logf("  slot%d %s %s access_rip=%s%s count=%ld tid=%lu%s stack_exe=[%s]", h.slot, where(g_watch_addr[h.slot]).c_str(),
             g_cfg.watch_rw[h.slot] == 3 ? "rw" : "w", where(rip).c_str(), func_of(rip).c_str(), h.count.load(), h.tid,
             hit_main ? "(main)" : "(other)", st.c_str());
    }
}

// Perspective projection matrices: zeros where a pinhole projection has them and +-1 / 0 in the w column
// (row-vector, m[11]) or w row (column-vector, m[14]). Logs implied horizontal/vertical FOV and aspect.
void cmd_find_proj() {
    HMODULE self = nullptr, exe = GetModuleHandleW(nullptr);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&cmd_find_proj, &self);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0;
    int hits = 0;
    std::vector<uint8_t> buf(1 << 20);
    while (hits < g_cfg.max_hits && VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
        p = base + size;
        if (p <= base) break;
        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        if (!writable || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) || mbi.Type == MEM_MAPPED) continue;
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue;
        if (mbi.AllocationBase == (void*)self) continue;
        for (uintptr_t off = 0; off < size && hits < g_cfg.max_hits; off += buf.size()) {
            size_t n = std::min<uintptr_t>(buf.size(), size - off);
            if (!safe_copy(buf.data(), (void*)(base + off), n)) continue;
            for (size_t i = 0; i + 64 <= n && hits < g_cfg.max_hits; i += 4) {
                const float* m = (const float*)(buf.data() + i);
                if (m[15] != 0.0f || !(m[0] > 0.05f && m[0] < 20.0f) || !(std::fabs(m[5]) > 0.05f && std::fabs(m[5]) < 20.0f)) continue;
                if (m[1] != 0.0f || m[2] != 0.0f || m[4] != 0.0f || m[6] != 0.0f || m[3] != 0.0f || m[7] != 0.0f) continue;
                bool rowv = std::fabs(std::fabs(m[11]) - 1.0f) < 1e-6f && m[12] == 0.0f && m[13] == 0.0f;
                bool colv = std::fabs(std::fabs(m[14]) - 1.0f) < 1e-6f && m[12] == 0.0f && m[13] == 0.0f;
                if (!rowv && !colv) continue;
                double hf = 2.0 * std::atan(1.0 / m[0]) * 180.0 / 3.14159265, vf = 2.0 * std::atan(1.0 / std::fabs(m[5])) * 180.0 / 3.14159265;
                logf("  PROJ %s %s xs=%.5f ys=%.5f hfov=%.3f vfov=%.3f aspect=%.4f m8=%.4f m9=%.4f m10=%.5f m11=%.1f m14=%.4f", where(base + off + i).c_str(),
                     rowv ? "row-vec" : "col-vec", m[0], m[5], hf, vf, std::fabs(m[5]) / m[0], m[8], m[9], m[10], m[11], m[14]);
                ++hits;
            }
        }
    }
    logf("find_proj: %d hit(s)", hits);
}

// ---------------------------------------------------------------------------------------------
// Second-thread Present probe (Unlocked in-between frames):
//  * call chains (exe frames only, as function starts) of Presents made off the main thread;
//  * per scanned candidate, how often it changed between two such Presents within the same game tick.
// A candidate that changes within a tick is per-drawn-frame data (an interpolated camera).
struct PStack { std::atomic<uint64_t> sig{0}; std::atomic<long> count{0}; uintptr_t frames[10] = {}; int n = 0; DWORD tid = 0; };
PStack g_pstacks[16];
std::vector<size_t> g_pidx;            // indices into g_cands that are tracked per Present
std::vector<float> g_plast;            // 16 floats per tracked candidate, last value seen at a second-thread Present
std::vector<uint32_t> g_pintra;        // per tracked candidate: changes within the same tick
long long g_plast_tick = -1;
std::atomic<long> g_ppresents{0}, g_ppresents_same_tick{0};

void present_probe_hook() {
    // Call chain signature.
    if (!g_exe_lo) {
        HMODULE exe = GetModuleHandleW(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
        g_exe_lo = (uintptr_t)exe; g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    void* raw[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, raw, nullptr);
    uintptr_t fr[10]; int k = 0;
    uint64_t sig = 1469598103934665603ull;
    for (USHORT i = 0; i < n && k < 10; ++i) {
        uintptr_t a = (uintptr_t)raw[i];
        if (a > g_exe_lo && a < g_exe_hi) { fr[k++] = a; sig = (sig ^ a) * 1099511628211ull; }
    }
    for (auto& ps : g_pstacks) {
        uint64_t cur = ps.sig.load();
        if (cur == sig) { ps.count.fetch_add(1); break; }
        if (cur == 0) {
            uint64_t exp = 0;
            if (ps.sig.compare_exchange_strong(exp, sig)) {
                memcpy(ps.frames, fr, sizeof(uintptr_t) * k); ps.n = k; ps.tid = GetCurrentThreadId();
                ps.count.fetch_add(1);
                break;
            }
            if (exp == sig) { ps.count.fetch_add(1); break; }
        }
    }
    // Candidate change tracking.
    if (!g_ptrack.load()) return;
    std::unique_lock<std::mutex> cl(g_cand_mutex, std::try_to_lock);
    if (!cl.owns_lock() || g_plast.size() != g_pidx.size() * 16) return;
    long long tick = g_total_ticks.load();
    bool same_tick = tick == g_plast_tick;
    g_plast_tick = tick;
    g_ppresents.fetch_add(1);
    if (same_tick) g_ppresents_same_tick.fetch_add(1);
    float cur[16];
    for (size_t i = 0; i < g_pidx.size(); ++i) {
        if (!safe_copy(cur, (void*)g_cands[g_pidx[i]].addr, 64)) continue;
        float* last = &g_plast[i * 16];
        if (memcmp(cur, last, 64) != 0) {
            if (same_tick) ++g_pintra[i];
            memcpy(last, cur, 64);
        }
    }
}

void cmd_ptrack(bool on) {
    std::lock_guard<std::mutex> cl(g_cand_mutex);
    if (on) {
        g_pidx.clear();
        for (size_t i = 0; i < g_cands.size() && (int)g_pidx.size() < g_cfg.ptrack_max; ++i)
            if (!g_cands[i].dead && (int)g_cands[i].orbit_changes >= g_cfg.ptrack_min_orbit) g_pidx.push_back(i);
        g_plast.assign(g_pidx.size() * 16, 0.0f);
        g_pintra.assign(g_pidx.size(), 0);
        g_ppresents = 0; g_ppresents_same_tick = 0; g_plast_tick = -1;
    }
    g_ptrack = on && !g_pidx.empty();
    logf("ptrack %s (%zu of %zu candidates, min orbit changes %d)", g_ptrack.load() ? "on" : "off", g_pidx.size(), g_cands.size(), g_cfg.ptrack_min_orbit);
}

void cmd_ptrack_report() {
    logf("PTRACK_REPORT second-thread presents=%ld same_tick=%ld", g_ppresents.load(), g_ppresents_same_tick.load());
    for (auto& ps : g_pstacks) {
        if (!ps.sig.load()) continue;
        std::string st;
        for (int i = 0; i < ps.n; ++i) { st += where(ps.frames[i]); st += func_of(ps.frames[i]); st += ' '; }
        logf("  PSTACK count=%ld tid=%lu [%s]", ps.count.load(), ps.tid, st.c_str());
    }
    std::lock_guard<std::mutex> cl(g_cand_mutex);
    if (g_pintra.size() != g_pidx.size()) return;
    std::vector<size_t> idx;
    for (size_t i = 0; i < g_pintra.size(); ++i) if (g_pintra[i]) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [](size_t a, size_t b) { return g_pintra[a] > g_pintra[b]; });
    logf("  candidates changing within a tick: %zu", idx.size());
    for (size_t j = 0; j < idx.size() && j < (size_t)g_cfg.report_top; ++j) {
        const Candidate& c = g_cands[g_pidx[idx[j]]];
        float m[16] = {};
        safe_copy(m, (void*)c.addr, 64);
        logf("  #%zu %s layout=%d intra_tick_changes=%u orbit_chg=%u rest_chg=%u | r0=(%.3f %.3f %.3f) r1=(%.3f %.3f %.3f) r2=(%.3f %.3f %.3f) t=(%.2f %.2f %.2f)",
             j + 1, where(c.addr).c_str(), c.layout, g_pintra[idx[j]], c.orbit_changes, c.rest_changes, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9],
             m[10], m[12], m[13], m[14]);
    }
}

void run_command(const std::wstring& cmd) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    logf("CMD %ls (seq %d) tick=%lld", cmd.c_str(), g_cfg.cmd_seq, g_total_ticks.load());
    if (cmd == L"crash_test") { if (HANDLE t = CreateThread(nullptr, 64 * 1024, crash_test_thread, nullptr, 0, nullptr)) CloseHandle(t); } // kills the game: catcher check
    else if (cmd == L"scan") cmd_scan();
    else if (cmd == L"report") cmd_report();
    else if (cmd == L"clear") { std::lock_guard<std::mutex> cl(g_cand_mutex); g_ptrack = false; g_cands.clear(); g_tracking = false; logf("cleared"); }
    else if (cmd == L"find_eye") cmd_find_eye();
    else if (cmd == L"freeze") cmd_freeze();
    else if (cmd == L"unfreeze") { g_freeze_addr = 0; logf("unfrozen"); }
    else if (cmd == L"dump") cmd_dump();
    else if (cmd == L"watch") cmd_watch(true);
    else if (cmd == L"unwatch") cmd_watch(false);
    else if (cmd == L"watch_report") cmd_watch_report();
    else if (cmd == L"find_proj") cmd_find_proj();
    else if (cmd == L"find_mv") cmd_find_mv();
    else if (cmd == L"d3d_dump") {
        extern bool g_dump_main, g_dump_other;
        extern int g_dump_left_main, g_dump_left_other;
        g_dump_main = g_dump_other = true;
        g_dump_left_main = g_dump_left_other = 3;
        logf("d3d_dump armed (3 bundle-executing direct lists per thread)");
    }
    else if (cmd == L"ecam_log") { g_ecam_log_left = 40; g_ecam_log_r0_n = 0; g_ecam_log_r0 = 40; g_ecam_log_eb = 40; logf("ecam_log armed (40 camera blocks, 40 distinct non-plain r0 matrices)"); }
    else if (cmd == L"bscan_snap") cmd_bscan_snap();
    else if (cmd == L"bscan_find") cmd_bscan_find();
    else if (cmd == L"bscan_clear") { g_bsnaps.clear(); logf("BSCAN cleared"); }
    else if (cmd == L"tick_log") { g_ticklog_left = 40; logf("tick_log armed (40 ticks)"); }
    else if (cmd == L"big_log") { g_biglog_left = 40; logf("big_log armed (40 large HUD-stage bundles)"); }
    else if (cmd == L"post_log") { g_postlog_left = 80; logf("post_log armed (80 post-stage bundles)"); }
    else if (cmd == L"copy_log") { g_copylog_left = 80; logf("copy_log armed (80 copies)"); }
    else if (cmd == L"find_floats") cmd_find_floats();
    else if (cmd == L"ptrack") cmd_ptrack(true);
    else if (cmd == L"ptrack_stop") cmd_ptrack(false);
    else if (cmd == L"ptrack_report") cmd_ptrack_report();
    else logf("unknown command");
}

} // namespace

// Crash catcher (crash_catch=1, at start): on a stack overflow anywhere, or an access violation inside the probe,
// the faulting thread hands its context to a waiting thread, which logs module+offset of the fault and an unwound
// backtrace (with the most repeated return addresses, for recursion) and writes a small minidump to log_dir. The
// game's .NET runtime catches native faults and reports them as clr.dll crashes (run hudms3), hiding the source.
// Dumps hold game memory: they stay in runs/ (git-ignored), never in the repo.
HANDLE g_crash_ev = nullptr, g_crash_done = nullptr;
EXCEPTION_POINTERS* g_crash_ep = nullptr;
DWORD g_crash_tid = 0;
uintptr_t g_crash_sb = 0, g_crash_sl = 0, g_crash_dealloc = 0; // faulting thread: stack base, committed limit, reserve bottom
std::atomic<int> g_cxx_logged{0};
PVOID g_crash_veh_h = nullptr;

// Type name of a thrown C++ exception (x64 ThrowInfo, image-relative); "" if unreadable.
void cxx_type(const EXCEPTION_RECORD* er, char* out, size_t n) {
    out[0] = 0;
    __try {
        if (er->NumberParameters < 4) return;
        uintptr_t base = er->ExceptionInformation[3];
        const uint8_t* ti = (const uint8_t*)er->ExceptionInformation[2];
        if (!ti || !base) return;
        int cta = *(const int*)(ti + 12);
        const int* arr = (const int*)(base + cta);
        if (arr[0] < 1) return;
        const uint8_t* ct = (const uint8_t*)(base + arr[1]);
        const char* name = (const char*)(base + *(const int*)(ct + 4) + 16);
        strncpy_s(out, n, name, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}
std::atomic<int> g_crash_once{0};
HMODULE g_self_mod = nullptr;

void crash_where(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    wchar_t p[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* b = wcsrchr(p, L'\\');
        snprintf(out, n, "%ls+0x%llx", b ? b + 1 : p, (unsigned long long)(a - (uintptr_t)m));
    } else snprintf(out, n, "%p", (void*)a);
}

LONG CALLBACK crash_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    bool ours = false;
    if (code == EXCEPTION_ACCESS_VIOLATION && g_self_mod) {
        HMODULE m = nullptr;
        ours = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &m) && m == g_self_mod;
    }
    if (code == 0xE06D7363 && g_d3d12core && ep->ExceptionRecord->NumberParameters >= 4 &&
        ep->ExceptionRecord->ExceptionInformation[3] == (ULONG_PTR)g_d3d12core && g_cxx_logged.fetch_add(1) < 20) {
        // a C++ exception thrown inside D3D12Core (camcrash2: the game's call into D3D12 threw, then the .NET handler died)
        char t[128], w[160];
        cxx_type(ep->ExceptionRecord, t, sizeof(t));
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        uintptr_t dealloc = *(uintptr_t*)((uint8_t*)NtCurrentTeb() + 0x1478);
        std::unique_lock<std::mutex> lk(g_log_mutex, std::try_to_lock);
        if (lk.owns_lock()) {
            logf("CXX exception in D3D12Core: type '%s' thread=%lu (frame thread %lu) stack used %llu KB of %llu KB reserved, VRAM %.0f/%.0f MB", t, GetCurrentThreadId(), g_frame_thread.load(),
                 (unsigned long long)(((uintptr_t)tib->StackBase - (uintptr_t)ep->ContextRecord->Rsp) / 1024), (unsigned long long)(((uintptr_t)tib->StackBase - dealloc) / 1024),
                 g_vram_use.load() / 1048576.0, g_vram_budget.load() / 1048576.0);
            void* fr[24];
            USHORT nf = RtlCaptureStackBackTrace(0, 24, fr, nullptr);
            for (USHORT i = 0; i < nf; ++i) { crash_where((uintptr_t)fr[i], w, sizeof(w)); logf("CXX  #%u %s", i, w); }
        }
    }
    if ((code == EXCEPTION_STACK_OVERFLOW || ours) && g_crash_once.exchange(1) == 0) {
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        g_crash_sb = (uintptr_t)tib->StackBase; g_crash_sl = (uintptr_t)tib->StackLimit;
        g_crash_dealloc = *(uintptr_t*)((uint8_t*)NtCurrentTeb() + 0x1478);
        g_crash_ep = ep; g_crash_tid = GetCurrentThreadId();
        SetEvent(g_crash_ev);
        WaitForSingleObject(g_crash_done, 30000);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI crash_thread(LPVOID) {
    WaitForSingleObject(g_crash_ev, INFINITE);
    EXCEPTION_POINTERS* ep = g_crash_ep;
    char w[256];
    crash_where((uintptr_t)ep->ExceptionRecord->ExceptionAddress, w, sizeof(w));
    if (g_log) {
        std::unique_lock<std::mutex> lk(g_log_mutex, std::defer_lock);
        bool locked = lk.try_lock(); // the faulting thread may hold it; log anyway
        logf("CRASH code=0x%08lx at %s thread=%lu (frame thread %lu) locked_log=%d", ep->ExceptionRecord->ExceptionCode, w, g_crash_tid, g_frame_thread.load(), (int)locked);
        CONTEXT c = *ep->ContextRecord;
        std::vector<uintptr_t> pcs;
        for (int i = 0; i < 20000 && c.Rip; ++i) {
            pcs.push_back((uintptr_t)c.Rip);
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!f) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; } // leaf function
            void* hd = nullptr; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &ef, nullptr);
        }
        logf("CRASH stack depth %zu (unwound frames); rsp %llu KB below base, committed to %llu KB, reserve %llu KB; VRAM %.0f/%.0f MB", pcs.size(),
             (unsigned long long)((g_crash_sb - (uintptr_t)ep->ContextRecord->Rsp) / 1024), (unsigned long long)((g_crash_sb - g_crash_sl) / 1024),
             (unsigned long long)((g_crash_sb - g_crash_dealloc) / 1024), g_vram_use.load() / 1048576.0, g_vram_budget.load() / 1048576.0);
        for (size_t i = 0; i < pcs.size() && i < 48; ++i) { crash_where(pcs[i], w, sizeof(w)); logf("CRASH  #%zu %s", i, w); }
        if (pcs.size() > 48) { // the deepest frames: what started it
            for (size_t i = pcs.size() > 80 ? pcs.size() - 32 : 48; i < pcs.size(); ++i) { crash_where(pcs[i], w, sizeof(w)); logf("CRASH  #%zu %s", i, w); }
            std::vector<std::pair<uintptr_t, int>> cnt;
            for (uintptr_t p : pcs) { bool f = false; for (auto& x : cnt) if (x.first == p) { ++x.second; f = true; break; } if (!f && cnt.size() < 4096) cnt.push_back({p, 1}); }
            std::sort(cnt.begin(), cnt.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < cnt.size() && i < 12; ++i) { crash_where(cnt[i].first, w, sizeof(w)); logf("CRASH  repeated x%d %s", cnt[i].second, w); }
        }
    }
    if (HMODULE dh = LoadLibraryW(L"dbghelp.dll")) {
        typedef BOOL(WINAPI * MDW)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
#pragma pack(push, 4) // MINIDUMP_EXCEPTION_INFORMATION is 4-packed
        struct MEI { DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; };
#pragma pack(pop)
        MEI mei{g_crash_tid, ep, FALSE};
        if (auto mdw = (MDW)GetProcAddress(dh, "MiniDumpWriteDump")) {
            HANDLE f = CreateFileW((g_cfg.log_dir + L"\\crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                BOOL ok = mdw(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 /*MiniDumpWithThreadInfo*/, &mei, nullptr, nullptr);
                CloseHandle(f);
                logf("CRASH minidump %s", ok ? "written (crash.dmp)" : "FAILED");
            }
        }
    }
    SetEvent(g_crash_done);
    return 0;
}

// Stack watch (stack_watch=1): a thread reads the game main thread's committed stack (TEB StackLimit) every ms; when
// it has grown by stack_watch_step KB, it pauses the thread briefly and logs its call stack (run camcrash3: the main
// thread's stack had been committed to 1020 of 1024 KB, the guard page gone, and the .NET runtime turned the next
// exception into a stack overflow).
typedef LONG(NTAPI* NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
DWORD WINAPI stack_watch_thread(LPVOID) {
    auto q = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    DWORD tid = 0;
    while (!(tid = g_frame_thread.load())) Sleep(50);
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!h || !q) return 0;
    struct TBI { LONG st; PVOID teb; PVOID pid, tid; ULONG_PTR aff; LONG pri, bpri; } tbi{};
    if (q(h, 0, &tbi, sizeof(tbi), nullptr) < 0 || !tbi.teb) return 0;
    NT_TIB* tib = (NT_TIB*)tbi.teb;
    // the main thread switches stacks (fibers: StackBase changes): track each stack by its base
    struct Stk { uintptr_t base, logged; };
    std::vector<Stk> stks;
    { std::lock_guard<std::mutex> lk(g_log_mutex); logf("STACK watch: main thread %lu", tid); }
    for (;;) {
        Sleep(1);
        uintptr_t base = (uintptr_t)*(volatile PVOID*)&tib->StackBase, lim = (uintptr_t)*(volatile PVOID*)&tib->StackLimit;
        uintptr_t dealloc = *(volatile uintptr_t*)((uint8_t*)tbi.teb + 0x1478);
        if (base != (uintptr_t)*(volatile PVOID*)&tib->StackBase || lim > base) continue; // switched while reading
        uintptr_t c = base - lim;
        Stk* k = nullptr;
        for (auto& x : stks) if (x.base == base) k = &x;
        if (!k) {
            if (stks.size() >= 256) continue;
            stks.push_back({base, c});
            std::lock_guard<std::mutex> lk(g_log_mutex);
            logf("STACK new stack on the main thread: base %p committed %llu KB reserve %llu KB (tick %lld, %zu stacks seen)", (void*)base, (unsigned long long)(c / 1024),
                 (unsigned long long)(dealloc && dealloc < base ? (base - dealloc) / 1024 : 0), g_total_ticks.load(), stks.size());
            continue;
        }
        uintptr_t logged = k->logged;
        if (c < logged + (uintptr_t)g_cfg.stack_watch_step * 1024) continue;
        k->logged = c;
        uintptr_t pcs[64]; int n = 0; uintptr_t rsp = 0;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT x{}; x.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &x)) {
                rsp = (uintptr_t)x.Rsp;
                for (; n < 64 && x.Rip; ++n) {
                    pcs[n] = (uintptr_t)x.Rip;
                    DWORD64 ib = 0;
                    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(x.Rip, &ib, nullptr);
                    if (!f) { x.Rip = *(DWORD64*)x.Rsp; x.Rsp += 8; continue; }
                    void* hd = nullptr; DWORD64 ef = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, x.Rip, f, &x, &hd, &ef, nullptr);
                }
            }
            ResumeThread(h);
        }
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("STACK main thread committed %llu KB (now at depth %llu KB, tick %lld, flat %d)", (unsigned long long)(c / 1024),
             (unsigned long long)(rsp ? (base - rsp) / 1024 : 0), g_total_ticks.load(), (int)g_flat_mode);
        char w[160];
        for (int i = 0; i < n; ++i) { crash_where(pcs[i], w, sizeof(w)); logf("STACK  #%d %s", i, w); }
    }
}

void crash_install() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&crash_install, &g_self_mod);
    g_crash_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_crash_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, crash_thread, nullptr, 0, nullptr)) CloseHandle(t);
    g_d3d12core = GetModuleHandleW(L"D3D12Core.dll"); // loaded later: set again at the first Present
    g_crash_veh_h = AddVectoredExceptionHandler(1, crash_veh);
    if (g_cfg.stack_watch) if (HANDLE t = CreateThread(nullptr, 256 * 1024, stack_watch_thread, nullptr, 0, nullptr)) CloseHandle(t);
    logf("CRASH catcher installed (stack overflow anywhere, access violation in the probe)");
}

extern "C" __declspec(dllexport) void OnInit(const wchar_t* mod_path) {
    // Inside Panacea's DllMain: no D3D, no threads waited on. Only settings and the log file.
    QueryPerformanceFrequency(&g_qpf);
    g_patched.reserve(16); // hooks read this from the present thread; never reallocate after setup starts
    wchar_t self[MAX_PATH] = {};
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OnInit, &me);
    GetModuleFileNameW(me, self, MAX_PATH);
    std::wstring dir = self;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    g_cfg.log_dir = dir;
    g_ini_path = dir + L"\\kh2vr_probe.ini";
    load_settings(true);
    g_log = _wfopen((g_cfg.log_dir + L"\\kh2vr_probe.log").c_str(), L"w");
    logf("kh2vr_probe OnInit mod_path=%ls enabled=%d stats_ms=%d dummy_d3d11=%d dummy_d3d12=%d pid=%lu", mod_path ? mod_path : L"(null)", g_cfg.enabled, g_cfg.stats_ms, g_cfg.dummy_d3d11, g_cfg.dummy_d3d12, GetCurrentProcessId());
    log_settings("initial");
    if (g_cfg.crash_catch) crash_install();
    if (g_cfg.d3d_debug || g_cfg.d3d_capture) d3d_debug_install();
    logf("modules at init: d3d12=%p D3D12Core=%p d3d11=%p dxgi=%p", (void*)GetModuleHandleW(L"d3d12.dll"),
         (void*)GetModuleHandleW(L"D3D12Core.dll"), (void*)GetModuleHandleW(L"d3d11.dll"), (void*)GetModuleHandleW(L"dxgi.dll"));
}

extern "C" __declspec(dllexport) void OnFrame() {
    static bool veh_again = false; // ahead of handlers added since OnInit (the .NET runtime's)
    if (!veh_again && g_cfg.crash_catch) { veh_again = true; if (g_crash_veh_h) RemoveVectoredExceptionHandler(g_crash_veh_h); g_crash_veh_h = AddVectoredExceptionHandler(1, crash_veh); }
    LONGLONG now = qpc();
    if (now - g_last_reload_check > g_qpf.QuadPart * g_cfg.reload_ms / 1000) {
        g_last_reload_check = now;
        FILETIME m;
        if (ini_mtime(&m) && CompareFileTime(&m, &g_ini_mtime) != 0) {
            std::lock_guard<std::mutex> lk(g_log_mutex);
            load_settings(false);
            log_settings("reloaded");
            g_cap_deadline = 0;
        }
    }
    if (!g_cfg.enabled) return;
    pace_tick();
    g_frames_in_window.fetch_add(1);
    g_total_ticks.fetch_add(1);
    g_frame_thread.store(GetCurrentThreadId());
    poll_pads();
    drive_update();
    flat_flag_tick();
    if (g_ticklog_left.load() > 0) { // flush the previous tick's list events
        std::string b;
        { std::lock_guard<std::recursive_mutex> lk(g_d3d_mutex); b.swap(g_ticklog_buf); }
        g_ticklog_left.fetch_sub(1);
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("TICK %lld (fires so far %lld):%s", g_total_ticks.load(), g_fire_seq.load(), b.c_str());
    }
    xr_tick_begin();
    if (g_cfg.cmd_seq != g_cmd_seq_done) {
        bool first = g_cmd_seq_done == -1;
        g_cmd_seq_done = g_cfg.cmd_seq;
        if (!first && !g_cfg.cmd.empty()) run_command(g_cfg.cmd); // a cmd present at start is not run
    }
    track_candidates();
    apply_freeze();
    monitor_fields();
    if (!g_hook_attempted) {
        g_hook_attempted = true;
        std::lock_guard<std::mutex> lk(g_log_mutex);
        logf("first OnFrame on thread %lu; modules: d3d12=%p D3D12Core=%p d3d11=%p", GetCurrentThreadId(),
             (void*)GetModuleHandleW(L"d3d12.dll"), (void*)GetModuleHandleW(L"D3D12Core.dll"), (void*)GetModuleHandleW(L"d3d11.dll"));
        install_hooks();
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) { // FreeLibrary, not process exit
        unpatch_all();
        drive_unpatch();
        d3d_unpatch();
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
