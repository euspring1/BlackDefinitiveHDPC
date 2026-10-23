// Settings for weaker PCs: the game is kept at 60 fps or more where the PC allows it. BLACK runs its world on one CPU
// core, once a frame, so the frame rate is what a slow CPU feels; the render size is what a slow graphics chip feels.
//
// Only the first session (no black.ini at start) is tuned; from the next start the settings are the saved ones, and a
// PC that keeps 60 fps keeps the defaults (window 1080p, 120 fps, resolution scale 4X).
// - First start on integrated graphics (Intel graphics other than Arc, or a primary adapter with less than 1 GB of its
//   own video memory, as integrated chips report): the game starts light: 60 fps, resolution scale 1.5X, anisotropic filtering 4X,
//   effects at Xbox size, no ambient occlusion, draw distance 1X, no HD texture pack. Saved at once.
// - Through the first session, every 10 s (menus and play; loads and hitches not counted): under 55 frames drawn a second,
//   the resolution scale goes one step down at once (4X, 3X, 2X, 1.5X, 1X: the Xbox's own size is the floor) and is
//   saved. Above 60 fps, if play needs the frameskip for more than a fifth of its frames or runs under 90 % of real
//   time, the next start uses 60 fps. Nothing goes under 60 fps.
// - Choosing a frame rate, render resolution or resolution scale in PC SETTINGS turns auto_tune off for good.
// Command-line flags keep their values throughout.
#include "autotune.h"

#include "common.h"
#include "d3d/device.h"
#include "input/mouselook.h"
#include "settings.h"

#include <dxgi.h>

#pragma comment(lib, "dxgi.lib")

namespace autotune {
namespace {

constexpr double kWindow = 10.0;   // seconds measured at a time
constexpr double kTooSlow = 55.0;  // frames drawn a second under which the render size goes down
const float kScales[] = {4.0f, 3.0f, 2.0f, 1.5f, 1.0f}; // the steps down (PC SETTINGS' own values)

// tests: BLACK_AUTOTUNE_TEST=1 takes any graphics for integrated and any frame rate for too slow
bool test_mode()
{
    static const bool on = GetEnvironmentVariableW(L"BLACK_AUTOTUNE_TEST", nullptr, 0) > 0;
    return on;
}

bool integrated_graphics(std::string &name)
{
    if (test_mode()) {
        name = "test";
        return true;
    }
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return false;
    IDXGIAdapter1 *a = nullptr;
    bool igpu = false;
    if (SUCCEEDED(f->EnumAdapters1(0, &a))) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d))) {
            name = narrow(d.Description);
            // the memory is capped near 3 GB for a 32-bit process; Intel graphics other than Arc are integrated
            bool intel_igpu = d.VendorId == 0x8086 && name.find("Arc") == std::string::npos;
            igpu = intel_igpu || d.DedicatedVideoMemory < (SIZE_T)1024 * 1024 * 1024;
            logf("autotune: graphics '%s', %u MB of its own memory%s", name.c_str(), (unsigned)(d.DedicatedVideoMemory >> 20),
                 igpu ? " (integrated)" : "");
        }
        a->Release();
    }
    f->Release();
    return igpu;
}

bool scale_is_ours() { return !g_cfg.render_native && !g_cfg.render_h && !settings_from_cli("render"); }

// the next step down from the current scale, 0 at the floor
float lower_scale()
{
    for (float s : kScales)
        if (s < g_cfg.scale - 0.01f) return s;
    return 0.0f;
}

} // namespace

void start()
{
    if (g_cfg.background || !g_cfg.first_run) return;
    g_cfg.auto_tune = true;
    std::string name;
    if (integrated_graphics(name)) {
        if (!settings_from_cli("fps")) g_cfg.fps = 60;
        if (scale_is_ours()) g_cfg.scale = 1.5f;
        if (!settings_from_cli("aniso")) g_cfg.aniso = 4;
        if (!settings_from_cli("ambient_occlusion")) g_cfg.ambient_occlusion = 0;
        if (!settings_from_cli("draw_distance")) g_cfg.draw_distance = 1;
        if (!settings_from_cli("textures")) g_cfg.textures = false; // the HD texture pack, when one is installed
        g_cfg.effects_native = true;
        logc("First start on integrated graphics (%s): a light profile (%d fps, resolution scale %gX); "
             "PC SETTINGS changes it.", name.c_str(), g_cfg.fps, g_cfg.scale);
    }
    settings_save();
}

void frame(bool skipped)
{
    if (!g_cfg.auto_tune || g_cfg.background) return;
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    static int64_t last;
    static float last_clock;
    static double real, play_real, play_game;
    static uint32_t frames, undrawn, play_frames, play_undrawn;
    static bool rate_done;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint32_t clock = *(const uint32_t *)(uintptr_t)0x002D45D4;
    float c = clock ? *(const float *)(uintptr_t)(clock + 0x20) : 0.0f;
    double dt = last ? (double)(now.QuadPart - last) / freq.QuadPart : 0.0;
    float dc = c - last_clock;
    last = now.QuadPart;
    last_clock = c;
    if (!(dt > 0.0 && dt < 0.25)) return; // the first frame, a load or a hitch
    real += dt;
    frames++;
    undrawn += skipped;
    // play: the player's update ran this frame or the one before
    if (g_frame_count - mouselook_play_frame() <= 2 && dc >= 0.0f && dc < 0.25f) {
        play_real += dt;
        play_game += dc;
        play_frames++;
        play_undrawn += skipped;
    }
    if (real < kWindow) return;
    double drawn = (frames - undrawn) / real;
    if (g_cfg.vsync) { // V-Sync on a display under 60 Hz holds it under 60 by design
        DEVMODEW dm = {};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 && dm.dmDisplayFrequency < 59)
            drawn = 60.0;
    }
    // the render size: one step down at once while under 55 a second (a frame rate chosen under 60 is the player's)
    if ((drawn < kTooSlow || test_mode()) && g_cfg.fps >= 60 && scale_is_ours()) {
        if (float s = lower_scale()) {
            logc("Drawing %.0f frames a second: resolution scale %gX -> %gX to keep 60.", drawn, g_cfg.scale, s);
            g_cfg.scale = s;
            xd3d::request_render_change();
            settings_save();
        } else {
            static bool told;
            if (!told) logf("autotune: %.0f frames a second at resolution scale %gX (the floor): the CPU sets the pace", drawn, g_cfg.scale);
            told = true;
        }
    }
    // the frame rate above 60: from the next start, when play needs the frameskip a lot or runs slow
    if (!rate_done && g_cfg.fps > 60 && play_real >= kWindow && !settings_from_cli("fps")) {
        double speed = play_game / play_real, skip_share = (double)play_undrawn / play_frames;
        logf("autotune: %.0f s of play at %d fps: game speed %.0f%%, %.0f%% of the frames not drawn", play_real, g_cfg.fps,
             speed * 100, skip_share * 100);
        if (skip_share > 0.2 || speed < 0.9) {
            g_cfg.fps_next = 60;
            settings_save();
            logc("This PC cannot keep %d fps in play: 60 fps from the next start.", g_cfg.fps);
        }
        rate_done = true;
    }
    real = 0;
    frames = undrawn = 0;
    if (play_real >= kWindow) play_real = play_game = 0, play_frames = play_undrawn = 0;
}

void user_changed()
{
    if (!g_cfg.auto_tune) return;
    g_cfg.auto_tune = false;
    logf("autotune: off (a frame rate or render size was chosen)");
}

} // namespace autotune
