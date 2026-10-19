// Performance recorder (--perf-at).
//
// While recording: per-frame timings and counters on the game's render thread (the thread that calls
// D3DDevice_Swap), a sampling profiler of that thread (~1 kHz, from a thread off the game CPU) and the CPU
// time of every thread. Stopping writes perf\<date_time>\ next to black.exe: summary.txt (where the frame
// time goes, what grows in slow frames, hot functions), frames.csv and threads.csv.
#pragma once

#include <cstdint>

namespace perf {

// Exclusive timing categories inside a frame (a nested scope pauses its parent).
enum Cat : int {
    kDraw,    // HLE draw calls: state translation, D3D9 calls into the driver
    kConvert, // vertex conversion for vertex-program draws, pre-transformed vertex scaling, index scans
    kTexture, // texture cache checks (sampled hashes) and uploads
    kCompile, // HLSL shader compiles (new vertex/pixel shader combinations)
    kRtCopy,  // render target copies for draws that sample a render target
    kCatCount
};

extern volatile bool g_on; // recording (cheap test for the hot paths)

void toggle();             // start or stop (any thread; applied by the render thread at the next frame)
bool recording();

// Render thread, from D3DDevice_Swap.
void swap_begin();                              // the frame's work is done
void pace_end(uint32_t vblanks, uint32_t interval); // after waiting for the vblank: vblanks since the last flip
void frame_end(uint32_t render_w, uint32_t render_h); // after Present: the next frame's work starts

// Counters (any thread).
void add_draw(uint32_t vertices, uint32_t bytes);
void add_texture_upload(uint32_t bytes);
void add_compile();
void add_rt_copy();

// The report finished writing: the window shows it (host.cpp, WM_APP_PERF).
const char *last_report_text();
const wchar_t *last_report_dir();
constexpr unsigned WM_APP_PERF = 0x8000 + 0x51; // WM_APP + 0x51

struct Scope {
    explicit Scope(Cat c);
    ~Scope();
    void pause(int64_t now);
    Cat cat;
    bool on;
    int64_t start = 0;
    Scope *parent = nullptr;
};

} // namespace perf
