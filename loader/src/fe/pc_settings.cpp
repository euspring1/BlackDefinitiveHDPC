// The port's settings as rows of the in-game PC SETTINGS pages (fe/pages.cpp builds the pages, fe/calls.cpp receives
// "PCChange(row, -1 / +1)" from their list boxes and "PCSlider<row>(0-100)" from their sliders). Each row is a list of choices: the current one is found from
// g_cfg, a step applies the next one and saves black.ini. Aspect ratio and frame rate are
// read by the game at boot: a new choice is saved for the next run and shown with "NEXT START" until then.
#include "fe/pc_settings.h"

#include "common.h"
#include "d3d/device.h"
#include "d3d/host.h"
#include "drawdist.h"
#include "fe/bind_list.h"
#include "fe/vars.h"
#include "fov.h"
#include "input/autoaim.h"
#include "lang.h"
#include "mods.h"
#include "settings.h"
#include "timing.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace pcset {
namespace {

bool close_to(float a, float b) { return std::fabs(a - b) < 0.01f; }
std::wstring on_off(bool on) { return on ? L"ON" : L"OFF"; }
std::wstring number(float v, const wchar_t *fmt = L"%.1f")
{
    wchar_t s[32];
    swprintf(s, 32, fmt, v);
    return s;
}

// render size: native multiples of the window, fixed heights, or RESOLUTION SCALE (below) x Xbox
struct Res {
    bool native;
    float factor;
    int height;
    const wchar_t *text;
};
const Res kRes[] = {{true, 1.0f, 0, L"WINDOW SIZE"}, {true, 1.5f, 0, L"1.5 X WINDOW"}, {true, 2.0f, 0, L"2 X WINDOW"},
                    {false, 1.0f, 720, L"720P"},     {false, 1.0f, 1080, L"1080P"},   {false, 1.0f, 1440, L"1440P"},
                    {false, 1.0f, 2160, L"2160P"},   {false, 1.0f, 0, L"RESOLUTION SCALE"}};
constexpr int kResN = sizeof kRes / sizeof kRes[0];
int res_get()
{
    for (int i = 0; i < kResN; i++) {
        const Res &r = kRes[i];
        if (r.native ? g_cfg.render_native && close_to(g_cfg.render_factor, r.factor)
                     : !g_cfg.render_native && !g_cfg.render_w && g_cfg.render_h == r.height)
            return i;
    }
    return -1; // a size from the command line
}
void res_set(int i)
{
    g_cfg.render_native = kRes[i].native;
    g_cfg.render_factor = kRes[i].factor;
    g_cfg.render_w = 0;
    g_cfg.render_h = kRes[i].height;
    xd3d::request_render_change();
}

// WINDOW SIZE (PC SETTINGS > VIDEO): the window's client size, as the picture's height (the width follows from the
// aspect the game plays at). Entry 0 is the largest size of that aspect the desktop leaves free. The port renders
// at its own size (RESOLUTION SCALE below): this only decides how big the window is (in fullscreen it is the whole
// monitor, and the choice applies again when you leave it).
const int kWindowHeights[] = {0, 480, 720, 768, 900, 1080, 1440, 2160};
constexpr int kWindowN = sizeof kWindowHeights / sizeof kWindowHeights[0];
int window_get()
{
    if (g_cfg.window_w <= 0 && g_cfg.window_h <= 0) return 0; // the desktop fit
    if (g_cfg.window_w <= 0)
        for (int i = 1; i < kWindowN; i++)
            if (kWindowHeights[i] == g_cfg.window_h) return i;
    return -1; // --window WxH from the command line or black.ini
}
void window_set(int i)
{
    g_cfg.window_w = 0;
    g_cfg.window_h = kWindowHeights[i];
    host3d::set_window_size(g_cfg.window_w, g_cfg.window_h);
}
std::wstring window_text(int i)
{
    wchar_t s[32];
    if (i < 0) {
        if (g_cfg.window_w > 0 && g_cfg.window_h > 0) {
            swprintf(s, 32, L"%d X %d", g_cfg.window_w, g_cfg.window_h);
            return s;
        }
        return L"CUSTOM";
    }
    if (i == 0) return L"MONITOR";
    int h = kWindowHeights[i], aw = g_cfg.widescreen ? 16 : 4, ah = g_cfg.widescreen ? 9 : 3;
    swprintf(s, 32, L"%d X %d", h * aw / ah, h);
    return s;
}

// RESOLUTION SCALE: the game's internal resolution, K x the Xbox buffer (640x480, drawn 16:9 in widescreen), used
// while RESOLUTION is on RESOLUTION SCALE (choosing a scale puts it there). The host render targets follow it and the
// picture is then shown at the window's size. While RESOLUTION holds a size of its own this shows CUSTOM.
const float kScales[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr int kScaleN = sizeof kScales / sizeof kScales[0];
int scale_get()
{
    if (g_cfg.render_native || g_cfg.render_h > 0) return -1;
    for (int i = 0; i < kScaleN; i++)
        if (close_to(g_cfg.scale, kScales[i])) return i;
    return -1;
}
void scale_set(int i)
{
    g_cfg.render_native = false;
    g_cfg.render_w = g_cfg.render_h = 0;
    g_cfg.scale = kScales[i];
    xd3d::request_render_change();
}
std::wstring scale_text(int i)
{
    if (i < 0) return L"CUSTOM";
    wchar_t s[32];
    swprintf(s, 32, L"%gX%s", kScales[i], i == 1 ? L" (ORIGINAL)" : L"");
    return s;
}

const int kAniso[] = {0, 2, 4, 8, 16};
int aniso_get()
{
    for (int i = 0; i < 5; i++)
        if (kAniso[i] == g_cfg.aniso) return i;
    return 0;
}

// A slider row has the 26 positions of the game's slider clip (Sound Setup's bullet bar): field of view 70-120 in
// steps of 2, brightness and gamma 0.5-3.0 in steps of 0.1, mouse sensitivity 0.25-6.5 in steps of 0.25.
constexpr int kSteps = 26;
int step_of(float v, float first, float step) { return std::clamp((int)std::lround((v - first) / step), 0, kSteps - 1); }
const float kReverb[] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}; // 0 = off

template <size_t N> int nearest(const float (&list)[N], float v)
{
    int best = 0;
    for (int i = 1; i < (int)N; i++)
        if (std::fabs(list[i] - v) < std::fabs(list[best] - v)) best = i;
    return best;
}

int shown_fps() { return g_cfg.fps_next ? g_cfg.fps_next : g_cfg.fps; }
constexpr int kFpsN = sizeof kFrameRates / sizeof kFrameRates[0];
int fps_index(int fps)
{
    for (int i = 0; i < kFpsN; i++)
        if (kFrameRates[i] == fps) return i;
    return 0;
}
// DRAW DISTANCE: times the game's LOD and draw distances
const int kDistances[] = {1, 2, 4, 8, 12, 16};
constexpr int kDistN = sizeof kDistances / sizeof kDistances[0];
int distance_get()
{
    for (int i = 0; i < kDistN; i++)
        if (kDistances[i] == g_cfg.draw_distance) return i;
    return 0;
}

bool shown_wide() { return g_cfg.widescreen_next >= 0 ? g_cfg.widescreen_next != 0 : g_cfg.widescreen; }

struct Row {
    const char *label;
    int choices;
    int (*get)();
    void (*set)(int);
    std::wstring (*text)(int);
    bool slider = false;
    bool custom = false; // get() may be outside the list (a size from black.ini or the command line): CUSTOM
};

const Row kRows[] = {
    // VIDEO
    {"RESOLUTION", kResN, res_get, res_set, [](int i) { return std::wstring(i < 0 ? L"CUSTOM" : kRes[i].text); }, false, true},
    {"ANTI-ALIASING", 2, [] { return (int)g_cfg.aa; }, [](int i) { g_cfg.aa = i != 0; }, [](int i) { return on_off(i); }},
    {"FIELD OF VIEW", kSteps, [] { return step_of(g_cfg.fov, 70, 2); },
     [](int i) {
         g_cfg.fov = 70.0f + 2 * i;
         fov_set(g_cfg.fov);
     },
     [](int i) { return number(70.0f + 2 * i, L"%.0f") + (i == 0 ? L" (ORIGINAL)" : L""); }, true},
    {"BRIGHTNESS", kSteps, [] { return step_of(g_cfg.gain, 0.5f, 0.1f); }, [](int i) { g_cfg.gain = 0.5f + i * 0.1f; },
     [](int i) { return number(0.5f + i * 0.1f); }, true},
    {"GAMMA", kSteps, [] { return step_of(g_cfg.gamma, 0.5f, 0.1f); }, [](int i) { g_cfg.gamma = 0.5f + i * 0.1f; },
     [](int i) { return number(0.5f + i * 0.1f); }, true},
    {"FULLSCREEN", 2, [] { return (int)host3d::fullscreen(); }, [](int i) { host3d::set_fullscreen(i != 0); },
     [](int i) { return on_off(i); }},
    // DISPLAY
    {"ASPECT RATIO", 2, [] { return (int)!shown_wide(); },
     [](int i) {
         bool wide = i == 0;
         g_cfg.widescreen_next = wide == g_cfg.widescreen ? -1 : wide;
     },
     [](int i) { return std::wstring(i == 0 ? L"16:9" : L"4:3") + ((i == 0) != g_cfg.widescreen ? L" (NEXT START)" : L""); }},
    {"FRAME RATE", kFpsN, [] { return fps_index(shown_fps()); },
     [](int i) {
         int fps = kFrameRates[i];
         g_cfg.fps_next = fps == g_cfg.fps ? 0 : fps;
     },
     [](int i) {
         int fps = kFrameRates[i];
         return number((float)fps, L"%.0f FPS") + (fps == 30 ? L" (ORIGINAL)" : L"") + (fps != g_cfg.fps ? L" (NEXT START)" : L"");
     }},
    {"FILL GEOMETRY SEAMS", 2, [] { return (int)g_cfg.seam_fill; }, [](int i) { g_cfg.seam_fill = i != 0; }, [](int i) { return on_off(i); }},
    {"EFFECTS AT XBOX SIZE", 2, [] { return (int)g_cfg.effects_native; },
     [](int i) {
         g_cfg.effects_native = i != 0;
         xd3d::request_render_rebuild();
     },
     [](int i) { return on_off(i); }},
    // AUDIO AND GAME
    {"ROOM REVERB", 5, [] { return g_cfg.reverb ? std::max(1, nearest(kReverb, g_cfg.reverb_gain)) : 0; },
     [](int i) {
         g_cfg.reverb = i != 0;
         if (i) g_cfg.reverb_gain = kReverb[i];
     },
     [](int i) { return i ? number(kReverb[i] * 100, L"%.0f%%") : std::wstring(L"OFF"); }},
    {"AUTO-AIM", 3, [] { return autoaim_mode(); }, [](int i) { autoaim_set_mode(i); },
     [](int i) { return std::wstring(i == 2 ? L"CONTROLLER ONLY" : i ? L"ON" : L"OFF"); }},
    {"SKIP ANY MOVIE", 2, [] { return (int)g_cfg.skip_movies; }, [](int i) { g_cfg.skip_movies = i != 0; }, [](int i) { return on_off(i); }},
    // CONTROLS
    {"MOUSE SENSITIVITY", kSteps, [] { return step_of(g_cfg.mouse_sens, 0.25f, 0.25f); },
     [](int i) { g_cfg.mouse_sens = 0.25f * (i + 1); }, [](int i) { return number(0.25f * (i + 1), L"%.2f"); }, true},
    {"INVERT MOUSE", 2, [] { return (int)g_cfg.mouse_invert; }, [](int i) { g_cfg.mouse_invert = i != 0; }, [](int i) { return on_off(i); }},
    {"KEY BINDINGS", 1, [] { return 0; }, [](int) {}, [](int) { return std::wstring(); }}, // opens the bindings page
    {"AIM", 2, [] { return (int)g_cfg.aim_hold; }, [](int i) { g_cfg.aim_hold = i != 0; },
     [](int i) { return std::wstring(i ? L"HOLD" : L"TOGGLE (ORIGINAL)"); }},
    {"CROUCH", 2, [] { return (int)g_cfg.crouch_hold; }, [](int i) { g_cfg.crouch_hold = i != 0; },
     [](int i) { return std::wstring(i ? L"HOLD" : L"PROFILE OPTION"); }},
    // DISPLAY (after the rows above)
    {"WEAPON FOV", kSteps, [] { return step_of(g_cfg.viewmodel_fov, 70, 1); }, // 70-95 by 1: the default 85 is a step
     [](int i) { g_cfg.viewmodel_fov = 70.0f + i; },
     [](int i) { return number(70.0f + i, L"%.0f") + (i == 0 ? L" (ORIGINAL)" : L""); }, true},
    {"FPS COUNTER", 2, [] { return (int)g_cfg.fpscounter; }, [](int i) { g_cfg.fpscounter = i != 0; },
     [](int i) { return on_off(i); }},
    // AUDIO AND GAME
    {"MOVIE SUBTITLES", 2, [] { return (int)g_cfg.subtitles; }, [](int i) { g_cfg.subtitles = i != 0; },
     [](int i) { return on_off(i); }},
    // VIDEO (after the rows above)
    {"RESOLUTION SCALE", kScaleN, scale_get, scale_set, scale_text, false, true},
    {"ANISOTROPIC FILTERING", 5, [] { return aniso_get(); }, [](int i) { g_cfg.aniso = kAniso[i]; },
     [](int i) { return i ? number((float)kAniso[i], L"%.0fX") : std::wstring(L"OFF"); }},
    {"WINDOW SIZE", kWindowN, window_get, window_set, window_text, false, true},
    // MODS (first row of the page)
    {"CAMERA SHAKE", 2, [] { return (int)g_cfg.camera_shake; }, [](int i) { g_cfg.camera_shake = i != 0; },
     [](int i) { return on_off(i); }},
    // MODS (after RELOAD BLUR)
    {"V-SYNC", 2, [] { return (int)g_cfg.vsync; },
     [](int i) {
         g_cfg.vsync = i != 0;
         host3d::set_vsync(g_cfg.vsync);
     },
     [](int i) { return on_off(i); }},
    // MODS (after CAMERA SHAKE)
    {"RELOAD BLUR", 2, [] { return (int)g_cfg.reload_blur; }, [](int i) { g_cfg.reload_blur = i != 0; },
     [](int i) { return on_off(i); }},
    // MODS (after V-SYNC): the texture pack and the game's text are read at start, as the frame rate
    {"TEXTURES", 2, [] { return (int)(g_cfg.textures_next >= 0 ? g_cfg.textures_next != 0 : g_cfg.textures); },
     [](int i) { g_cfg.textures_next = (i != 0) == g_cfg.textures ? -1 : i; },
     [](int i) { return on_off(i) + ((i != 0) != g_cfg.textures ? L" (NEXT START)" : L""); }},
    {"LANGUAGE", 2, [] { return g_cfg.language_next >= 0 ? g_cfg.language_next : g_cfg.language; },
     [](int i) { g_cfg.language_next = i == g_cfg.language ? -1 : i; },
     [](int i) {
         // each language in its own words
         return std::wstring(i == lang::kPortuguese ? L"PORTUGUÊS (BR)" : L"ENGLISH") + (i != g_cfg.language ? L" (NEXT START)" : L"");
     }},
    // MODS (after LANGUAGE): applies at once
    {"DRAW DISTANCE", kDistN, [] { return distance_get(); },
     [](int i) {
         g_cfg.draw_distance = kDistances[i];
         drawdist_set(g_cfg.draw_distance);
     },
     [](int i) { return number((float)kDistances[i], L"%.0fX") + (i == 0 ? L" (ORIGINAL)" : L""); }},
    // INPUT: the prompts' buttons, built into the game's text at start
    {"INPUT DEVICE", 2, [] { return (int)(g_cfg.input_pad_next >= 0 ? g_cfg.input_pad_next != 0 : g_cfg.input_pad); },
     [](int i) { g_cfg.input_pad_next = (i != 0) == g_cfg.input_pad ? -1 : i; },
     [](int i) { return std::wstring(i ? L"CONTROLLER" : L"MOUSE/KEYBOARD") + ((i != 0) != g_cfg.input_pad ? L" (NEXT START)" : L""); }},
    // CHEATS (cheats.cpp)
    {"UNLOCK ALL", 2, [] { return (int)g_cfg.cheat_unlock_all; }, [](int i) { g_cfg.cheat_unlock_all = i != 0; },
     [](int i) { return on_off(i); }},
    {"GOD MODE", 2, [] { return (int)g_cfg.cheat_god; }, [](int i) { g_cfg.cheat_god = i != 0; },
     [](int i) { return on_off(i); }},
    {"INFINITE HEALTH PACKS", 2, [] { return (int)g_cfg.cheat_health_packs; }, [](int i) { g_cfg.cheat_health_packs = i != 0; },
     [](int i) { return on_off(i); }},
};
constexpr int kRowN = sizeof kRows / sizeof kRows[0];
constexpr int kKeyBindings = 15;
constexpr int kAimMode = 16, kCrouchMode = 17, kWeaponFov = 18, kFpsCounter = 19, kSubtitles = 20;
constexpr int kScale = 21, kAnisoRow = 22, kWindowRow = 23, kCameraShake = 24, kVsync = 25;
constexpr int kReloadBlur = 26, kTextures = 27, kLanguage = 28, kDrawDistance = 29, kInputDevice = 30;
constexpr int kCheatUnlock = 31, kCheatGod = 32, kCheatPacks = 33;
// Rows past the fixed ones are the mods found in <port>\mods: one ON/OFF line each (mods.h).
int mod_count() { return (int)mods::list().size(); }

} // namespace

const std::vector<Group> &groups()
{
    static const std::vector<Group> g = {
        {"PCVideo", lang::tr("VIDEO"), {0, kWindowRow, kScale, 1, kAnisoRow, 5}},
        {"PCDisplay", lang::tr("DISPLAY"), {6, 7, 8, 9, 3, 4}},                         // + brightness, gamma
        {"PCGame", lang::tr("AUDIO AND GAME"), {10, 12, kSubtitles, kFpsCounter, 2, kWeaponFov}}, // + both fields of view
        {"PCControls", lang::tr("CONTROLS"), {13, 14, 11, kAimMode, kCrouchMode, kKeyBindings}},  // + auto-aim
    };
    return g;
}

// The MODS page: CAMERA SHAKE, RELOAD BLUR, V-SYNC, TEXTURES, LANGUAGE, DRAW DISTANCE, then one row per mod found under
// <port>\mods (a page holds six rows: mods beyond them are not shown).
const Group &mods_group()
{
    static Group g;
    g = {"PCMods", lang::tr("MODS"), {kCameraShake, kReloadBlur, kVsync, kTextures, kLanguage, kDrawDistance}};
    const int room = fevar::kRows - (int)g.rows.size();
    for (int i = 0; i < mod_count() && i < room; i++) g.rows.push_back(kRowN + i);
    if (mod_count() > room) {
        static int told;
        if (told++ < 1) logf("mods: %d mods installed, %d of them fit the MODS page", mod_count(), room);
    }
    return g;
}

// INPUT: the row below MODS on the PC SETTINGS page
const Group &input_group()
{
    static const Group g = {"PCInput", lang::tr("INPUT"), {kInputDevice}};
    return g;
}

// CHEATS: the row below INPUT
const Group &cheats_group()
{
    static const Group g = {"PCCheats", lang::tr("CHEATS"), {kCheatUnlock, kCheatGod, kCheatPacks}};
    return g;
}

int count() { return kRowN + mod_count(); }

const char *label(int n)
{
    if (n >= 0 && n < kRowN) return lang::tr(kRows[n].label);
    int i = n - kRowN;
    return i >= 0 && i < mod_count() ? mods::list()[i].title.c_str() : "";
}

std::wstring value(int n)
{
    if (n >= kRowN) {
        int i = n - kRowN;
        if (i < 0 || i >= mod_count()) return L"";
        const mods::Mod &m = mods::list()[i];
        std::wstring v = m.on && !m.blocked ? L"ON" : L"OFF";
        if (m.blocked) v += L" (LANGUAGE)"; // a mod for another language than the one being played (mods.h)
        if (m.on != m.at_start) v += L" (RESTART)"; // read at start: the port restarts at the main menu (fe/restart.cpp)
        return lang::tr(v);
    }
    if (n < 0) return L"";
    const Row &r = kRows[n];
    int i = std::min(r.get(), r.choices - 1);
    if (i < 0 && !r.custom) i = 0; // only the rows that can hold a value from outside their list show CUSTOM
    return lang::tr(r.text(i));
}

void step(int n, int dir)
{
    if (!dir) return;
    if (n >= kRowN) { // a mod: the marker file in its own folder is the state
        int i = n - kRowN;
        if (i < 0 || i >= mod_count()) return;
        bool on = mods::toggle(i);
        logf("fe: mod %s -> %s", mods::list()[i].title.c_str(), on ? "ON" : "OFF");
        return;
    }
    if (n < 0) return;
    const Row &r = kRows[n];
    int i = std::min(r.get(), r.choices - 1);
    i = i < 0 ? 0 : (i + (dir > 0 ? 1 : r.choices - 1)) % r.choices;
    r.set(i);
    settings_save();
    logf("fe: %s -> %s", r.label, narrow(r.text(i)).c_str());
}

bool is_slider(int n) { return n >= 0 && n < kRowN && kRows[n].slider; }

// aspect ratio, frame rate: read by the game at boot; the mods' files too
bool restart_only(int n) { return n == 6 || n == 7 || n >= kRowN; }
bool is_frame_rate(int n) { return n == 7; }

const char *link(int n) { return n == kKeyBindings ? "PCBindings" : nullptr; }

int slider_pos(int n)
{
    if (!is_slider(n)) return 0;
    return std::clamp(kRows[n].get(), 0, kSteps - 1) * 100 / (kSteps - 1);
}

void slide(int n, float pos)
{
    if (!is_slider(n)) return;
    const Row &r = kRows[n];
    int i = std::clamp((int)std::lround(pos * (kSteps - 1) / 100.0f), 0, kSteps - 1);
    if (i == r.get()) return;
    r.set(i);
    settings_save();
    logf("fe: %s -> %s", r.label, narrow(r.text(i)).c_str());
}

void fill_page(int group)
{
    const std::vector<Group> &gs = groups();
    const std::vector<int> *rows = nullptr;
    if (group >= 0 && group < (int)gs.size()) rows = &gs[group].rows;
    else if (group == (int)gs.size()) rows = &mods_group().rows; // the MODS page, after the four groups
    else if (group == (int)gs.size() + 1) rows = &input_group().rows; // INPUT, after MODS
    else if (group == (int)gs.size() + 2) rows = &cheats_group().rows; // CHEATS, after INPUT
    if (!rows) return;
    for (int i = 0; i < fevar::kRows; i++) {
        int n = i < (int)rows->size() ? (*rows)[i] : -1;
        fe_var_set(fevar::kValue + i, n >= 0 ? value(n) : L"");
    }
}

// a slider takes its start from externObject.FEPCS<setting> when its page is prepared, before the page reports itself,
// so these follow the settings all the time
void refresh_sliders()
{
    for (int n = 0; n < kRowN; n++)
        if (kRows[n].slider) fe_int_set(("FEPCS" + std::to_string(n)).c_str(), slider_pos(n));
}

} // namespace pcset
