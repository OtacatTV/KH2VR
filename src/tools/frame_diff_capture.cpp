// frame_diff_capture: watches what actually reaches the desktop for one window, from outside the game.
//
// Uses DXGI Desktop Duplication on the monitor that shows the target window (no code in the game
// process, no objects on the game's device). For every desktop update it copies the window's client
// area to a staging texture, subsamples it, and compares it with the previous update. A frame whose
// pixels exactly match the previous one is a repeat; a sim-decoupled interpolated render differs.
//
// Usage: frame_diff_capture.exe <out.csv> [seconds=60] [window title substring] [step=4]
//        frame_diff_capture.exe <out.bmp> [skip_updates=10]   one full-resolution snapshot
//        frame_diff_capture.exe <out_%03d.bmp> [frames=60] [title] [step=4]  consecutive downsampled frames
//   step: sample every Nth pixel in x and y (live setting; smaller = more sensitive, slower)
// Prints a summary at the end; the CSV has one row per desktop update.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

struct FindCtx { const wchar_t* needle; HWND found; };

static BOOL CALLBACK find_window(HWND h, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t t[512];
    GetWindowTextW(h, t, 512);
    if (wcsstr(t, c->needle)) { c->found = h; return FALSE; }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fwprintf(stderr, L"usage: %ls out.csv [seconds] [title] [step]\n", argv[0]); return 2; }
    const wchar_t* out_path = argv[1];
    int seconds = argc > 2 ? _wtoi(argv[2]) : 60;
    const wchar_t* title = argc > 3 ? argv[3] : L"KINGDOM HEARTS";
    int step = argc > 4 ? std::max(1, _wtoi(argv[4])) : 4;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    FindCtx fc{title, nullptr};
    EnumWindows(find_window, (LPARAM)&fc);
    if (!fc.found) { fwprintf(stderr, L"window containing \"%ls\" not found\n", title); return 1; }
    HMONITOR mon = MonitorFromWindow(fc.found, MONITOR_DEFAULTTONEAREST);

    IDXGIFactory1* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return 1;
    IDXGIAdapter1* adapter = nullptr; IDXGIOutput1* output = nullptr; DXGI_OUTPUT_DESC od = {};
    for (UINT a = 0; !output && fac->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* o = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            o->GetDesc(&od);
            if (od.Monitor == mon) { o->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output); o->Release(); break; }
            o->Release();
        }
        if (!output) { adapter->Release(); adapter = nullptr; }
    }
    if (!output) { fwprintf(stderr, L"no DXGI output for the window's monitor\n"); return 1; }

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        fwprintf(stderr, L"D3D11CreateDevice failed\n"); return 1;
    }
    IDXGIOutputDuplication* dup = nullptr;
    HRESULT hr = output->DuplicateOutput(dev, &dup);
    if (FAILED(hr)) { fwprintf(stderr, L"DuplicateOutput failed 0x%08lx\n", hr); return 1; }

    // Client rect in output-local desktop coordinates.
    RECT cr; GetClientRect(fc.found, &cr);
    POINT tl{cr.left, cr.top}, br{cr.right, cr.bottom};
    ClientToScreen(fc.found, &tl); ClientToScreen(fc.found, &br);
    LONG ox = od.DesktopCoordinates.left, oy = od.DesktopCoordinates.top;
    LONG ow = od.DesktopCoordinates.right - ox, oh = od.DesktopCoordinates.bottom - oy;
    LONG x0 = std::max(0L, tl.x - ox), y0 = std::max(0L, tl.y - oy);
    LONG x1 = std::min(ow, br.x - ox), y1 = std::min(oh, br.y - oy);
    if (x1 <= x0 || y1 <= y0) { fwprintf(stderr, L"window not on the duplicated output\n"); return 1; }
    UINT w = UINT(x1 - x0), h = UINT(y1 - y0);
    fwprintf(stdout, L"capturing \"%ls\" client %ux%u at output (%ld,%ld) for %d s, step %d\n", title, w, h, x0, y0, seconds, step);

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return 1;

    // Snapshot mode: an out path ending in .bmp saves one frame (after skipping `seconds` desktop updates,
    // so a just-changed setting has taken effect) instead of writing the diff CSV.
    size_t plen = wcslen(out_path);
    bool snap_mode = plen > 4 && _wcsicmp(out_path + plen - 4, L".bmp") == 0 && !wcsstr(out_path, L"%03d"); // %03d = burst
    long snap_skip = snap_mode ? std::max(0, seconds) : 0;
    if (snap_mode) seconds = 30; // time limit while waiting for updates
    // Burst mode: an out path containing %03d keeps `seconds` consecutive updates as downsampled frames
    // (1 in `step` pixels) in memory and writes them as numbered BMPs at the end, so writing can't drop frames.
    bool burst_mode = wcsstr(out_path, L"%03d") != nullptr;
    long burst_n = burst_mode ? std::max(2, seconds) : 0;
    if (burst_mode) seconds = 30;
    std::vector<std::vector<uint8_t>> burst;
    FILE* csv = (snap_mode || burst_mode) ? _wfopen(L"NUL", L"w") : _wfopen(out_path, L"w");
    if (!csv) { fwprintf(stderr, L"cannot open %ls\n", out_path); return 1; }
    fprintf(csv, "idx,t_ms,last_present_qpc,accumulated_frames,mean_abs_diff,changed_frac,identical\n");

    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    std::vector<uint8_t> prev, cur;
    size_t samples_x = (w + step - 1) / step, samples_y = (h + step - 1) / step;
    cur.resize(samples_x * samples_y * 3);
    long idx = 0, identical = 0, moving = 0, identical_while_moving = 0, accum_gt1 = 0;
    std::vector<long> pattern; // 1 = changed, 0 = identical (first 2000 updates)
    for (;;) {
        QueryPerformanceCounter(&t);
        double el = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (el >= seconds) break;
        DXGI_OUTDUPL_FRAME_INFO fi = {};
        IDXGIResource* res = nullptr;
        hr = dup->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { fwprintf(stderr, L"AcquireNextFrame 0x%08lx (mode change or lost access); stopping\n", hr); break; }
        if (fi.LastPresentTime.QuadPart == 0) { res->Release(); dup->ReleaseFrame(); continue; } // pointer-only update
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        D3D11_BOX box{UINT(x0), UINT(y0), 0, UINT(x1), UINT(y1), 1};
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
        tex->Release(); res->Release();
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            if (snap_mode && idx >= snap_skip) {
                // Snapshot: one full-resolution 24-bit BMP of the client area, then stop.
                fclose(csv);
                FILE* b = _wfopen(out_path, L"wb");
                if (!b) { fwprintf(stderr, L"cannot open %ls\n", out_path); return 1; }
                UINT rowb = (w * 3 + 3) & ~3u;
                BITMAPFILEHEADER fh = {0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + rowb * h), 0, 0,
                                       DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))};
                BITMAPINFOHEADER ih = {sizeof(ih), LONG(w), -LONG(h), 1, 24, BI_RGB};
                fwrite(&fh, sizeof(fh), 1, b);
                fwrite(&ih, sizeof(ih), 1, b);
                std::vector<uint8_t> line(rowb, 0);
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* row = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
                    for (UINT x = 0; x < w; ++x) { line[x * 3] = row[x * 4]; line[x * 3 + 1] = row[x * 4 + 1]; line[x * 3 + 2] = row[x * 4 + 2]; }
                    fwrite(line.data(), 1, rowb, b);
                }
                fclose(b);
                ctx->Unmap(staging, 0);
                dup->ReleaseFrame();
                fwprintf(stdout, L"SNAPSHOT %ls %ux%u after %ld update(s)\n", out_path, w, h, idx + 1);
                return 0;
            }
            size_t k = 0;
            for (UINT y = 0; y < h; y += step) {
                const uint8_t* row = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
                for (UINT x = 0; x < w; x += step) { cur[k++] = row[x * 4]; cur[k++] = row[x * 4 + 1]; cur[k++] = row[x * 4 + 2]; }
            }
            ctx->Unmap(staging, 0);
        }
        dup->ReleaseFrame();
        if (burst_mode) {
            burst.push_back(cur);
            ++idx;
            if ((long)burst.size() >= burst_n) break;
            continue;
        }
        double mad = 0; long changed = 0;
        bool same = false;
        if (!prev.empty()) {
            uint64_t sum = 0;
            for (size_t i = 0; i < cur.size(); i += 3) {
                int d = abs(cur[i] - prev[i]) + abs(cur[i + 1] - prev[i + 1]) + abs(cur[i + 2] - prev[i + 2]);
                sum += d; if (d) ++changed;
            }
            mad = double(sum) / cur.size();
            same = changed == 0;
            if (same) ++identical;
            // "moving" = the scene changed recently: this or the previous update differed by > 0.5% of samples
            static bool last_changed_lot = false;
            bool changed_lot = changed > long(cur.size() / 3 / 200);
            if (changed_lot || last_changed_lot) { ++moving; if (same) ++identical_while_moving; }
            last_changed_lot = changed_lot;
            if (pattern.size() < 2000) pattern.push_back(same ? 0 : 1);
        }
        if (fi.AccumulatedFrames > 1) ++accum_gt1;
        fprintf(csv, "%ld,%.3f,%lld,%u,%.4f,%.5f,%d\n", idx, el * 1000.0, fi.LastPresentTime.QuadPart, fi.AccumulatedFrames, mad,
                prev.empty() ? 0.0 : double(changed) / (cur.size() / 3), same ? 1 : 0);
        prev.swap(cur);
        if (cur.size() != prev.size()) cur.resize(prev.size());
        ++idx;
    }
    fclose(csv);
    if (burst_mode) {
        UINT bw = UINT(samples_x), bh = UINT(samples_y), rowb = (bw * 3 + 3) & ~3u;
        for (size_t i = 0; i < burst.size(); ++i) {
            wchar_t name[MAX_PATH];
            swprintf(name, MAX_PATH, out_path, int(i));
            FILE* b = _wfopen(name, L"wb");
            if (!b) { fwprintf(stderr, L"cannot open %ls\n", name); return 1; }
            BITMAPFILEHEADER fh = {0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + rowb * bh), 0, 0,
                                   DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))};
            BITMAPINFOHEADER ih = {sizeof(ih), LONG(bw), -LONG(bh), 1, 24, BI_RGB};
            fwrite(&fh, sizeof(fh), 1, b);
            fwrite(&ih, sizeof(ih), 1, b);
            std::vector<uint8_t> line(rowb, 0);
            for (UINT y = 0; y < bh; ++y) {
                memcpy(line.data(), burst[i].data() + size_t(y) * bw * 3, bw * 3);
                fwrite(line.data(), 1, rowb, b);
            }
            fclose(b);
        }
        QueryPerformanceCounter(&t);
        fwprintf(stdout, L"BURST %zu frames %ux%u in %.2f s\n", burst.size(), bw, bh, double(t.QuadPart - t0.QuadPart) / f.QuadPart);
        return 0;
    }
    QueryPerformanceCounter(&t);
    double el = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
    fwprintf(stdout, L"SUMMARY updates=%ld (%.1f/s) identical_to_previous=%ld (%.1f%%) moving_updates=%ld identical_while_moving=%ld (%.1f%%) updates_with_accumulated_frames_gt1=%ld\n",
             idx, idx / el, identical, idx > 1 ? 100.0 * identical / (idx - 1) : 0.0, moving, identical_while_moving,
             moving ? 100.0 * identical_while_moving / moving : 0.0, accum_gt1);
    // A window of the changed(1)/identical(0) sequence from the middle of the run: "1010..." = every other frame repeats.
    size_t mid = pattern.size() / 2, from = mid > 60 ? mid - 60 : 0;
    std::string s;
    for (size_t i = from; i < pattern.size() && i < from + 120; ++i) s += pattern[i] ? '1' : '0';
    fprintf(stdout, "PATTERN(mid) %s\n", s.c_str());
    return 0;
}
