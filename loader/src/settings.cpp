#include "settings.h"

#include "common.h"
#include "input/bindings.h"
#include "lang.h"
#include "timing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace {

std::wstring ini_file() { return g_cfg.port_dir.empty() ? L"black.ini" : g_cfg.port_dir + L"\\black.ini"; }
std::set<std::string> g_cli; // keys given on the command line
std::mutex g_lock;           // the menu (window thread) and the exit path may both save

bool cli(const char *key) { return g_cli.count(key) != 0; }

// Everything after the '=' of a line, without the spaces around it (a path may hold spaces).
std::string trim_value(const char *line)
{
    const char *p = strchr(line, '=');
    std::string s = p ? p + 1 : "";
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

} // namespace

void settings_mark_cli(const char *key) { g_cli.insert(key); }

bool settings_from_cli(const char *key) { return cli(key); }

void settings_load()
{
    if (g_cfg.background) return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, ini_file().c_str(), L"r") || !f) {
        g_cfg.first_run = true; // no black.ini yet (autotune.cpp)
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64] = "", val[128] = "";
        if (line[0] == '#' || sscanf_s(line, " %63[^= ] = %127s", key, (unsigned)sizeof key, val, (unsigned)sizeof val) != 2) continue;
        std::string k = key;
        if (cli(k.c_str())) continue;
        float v = (float)atof(val); // the C runtime's default "C" locale: '.' decimals
        if (k == "render") {
            g_cfg.render_native = !strcmp(val, "native");
            g_cfg.render_h = g_cfg.render_native ? 0 : std::clamp(atoi(val), 0, 8192);
            g_cfg.render_w = 0;
            if (g_cfg.render_h && g_cfg.render_h < 240) g_cfg.render_h = 0;
        } else if (k == "render_factor") g_cfg.render_factor = std::clamp(v, 1.0f, 2.0f);
        else if (k == "scale") g_cfg.scale = std::clamp(v, 0.5f, 8.0f);
        else if (k == "aa") g_cfg.aa = v != 0;
        else if (k == "seamfill") g_cfg.seam_fill = v != 0;
        else if (k == "effectsnative") g_cfg.effects_native = v != 0;
        else if (k == "fps") g_cfg.fps = valid_fps(atoi(val));
        else if (k == "widescreen") g_cfg.widescreen = v != 0;
        else if (k == "fullscreen") g_cfg.fullscreen = v != 0;
        else if (k == "gain") g_cfg.gain = std::clamp(v, 0.5f, 3.0f);
        else if (k == "gamma") g_cfg.gamma = std::clamp(v, 0.5f, 3.0f);
        else if (k == "mouse_sens") g_cfg.mouse_sens = std::clamp(v, 0.1f, 50.0f);
        else if (k == "mouse_invert") g_cfg.mouse_invert = v != 0;
        else if (k == "pad_sens") g_cfg.pad_sens = std::clamp(v, 0.1f, 10.0f);
        // the release's black.ini says so: its first start counts as one (autotune.cpp); never written back
        else if (k == "first_start") g_cfg.first_run = v != 0;        else if (k == "aim_hold") g_cfg.aim_hold = v != 0;
        else if (k == "crouch_hold") g_cfg.crouch_hold = v != 0;
        else if (k == "hd_reticle") g_cfg.hd_reticle = v != 0;
        else if (k == "walk_speed") g_cfg.walk_speed = std::clamp(v, 0.1f, 1.0f);
        else if (k == "texture_pack") g_cfg.texture_pack = widen(trim_value(line));
        else if (k == "window") { // WxH; 0 width = from the height and the aspect, both 0 = the desktop fit
            const char *x = strpbrk(val, "xX");
            g_cfg.window_w = atoi(val);
            g_cfg.window_h = x ? atoi(x + 1) : 0;
        } else if (k == "aniso") {
            int n = atoi(val);
            g_cfg.aniso = n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 0;
        } else if (k == "viewmodel_fov") g_cfg.viewmodel_fov = std::clamp(v, 50.0f, 120.0f);
        else if (k == "fpscounter") g_cfg.fpscounter = v != 0;
        else if (k == "camera_shake") g_cfg.camera_shake = v != 0;
        else if (k == "vsync") g_cfg.vsync = v != 0;
        else if (k == "reload_blur") g_cfg.reload_blur = v != 0;
        else if (k == "textures") g_cfg.textures = v != 0;
        else if (k == "draw_distance") g_cfg.draw_distance = std::clamp(atoi(val), 1, 16);
        else if (k == "ambient_occlusion") g_cfg.ambient_occlusion = !strcmp(val, "gtao") ? 2 : !strcmp(val, "ssao") ? 1 : std::clamp(atoi(val), 0, 2);
        // input_mode, not v1.3's "input" (always written, so it would fix every old setup to one device)
        else if (k == "input_mode") g_cfg.input_mode = !strcmp(val, "controller") ? 2 : !strcmp(val, "keyboard") ? 1 : 0;
        else if (k == "input") {}
        else if (k == "language") {
            int l = lang::from_code(val);
            if (l >= 0) g_cfg.language = l;
        }
        else if (k == "subtitles") g_cfg.subtitles = v != 0;
        else if (k == "texture_pack_max") g_cfg.texture_pack_max = std::clamp(atoi(val), 256, 8192);
        else if (k == "reticle_colour") g_cfg.reticle_colour = strcmp(val, "game") ? (int)(strtoul(val, nullptr, 16) & 0xFFFFFF) : -1;
        else if (k == "reticle_enemy_colour") g_cfg.reticle_enemy_colour = strcmp(val, "game") ? (int)(strtoul(val, nullptr, 16) & 0xFFFFFF) : -1;
        else if (k == "autoaim") {
            g_cfg.autoaim = v != 0;
            g_cfg.autoaim_pad_only = atoi(val) == 2;
        }
        else if (k == "fov") g_cfg.fov = std::clamp(v, 50.0f, 120.0f);
        else if (k == "skipanymovie") g_cfg.skip_movies = v != 0;
        else if (k == "skip_intro") g_cfg.skip_intro = v != 0;
        else if (k == "cheat_unlock_all") g_cfg.cheat_unlock_all = v != 0;
        else if (k == "cheat_god") g_cfg.cheat_god = v != 0;
        else if (k == "cheat_health_packs") g_cfg.cheat_health_packs = v != 0;
        else if (k == "reverb") g_cfg.reverb = v != 0;
        else if (k == "reverb_gain") g_cfg.reverb_gain = std::clamp(v, 0.0f, 8.0f);
        else bindings::load_ini(k.c_str(), val); // key_<action> = Key[,Key]
    }
    fclose(f);
}

void settings_save()
{
    if (g_cfg.background) return;
    std::lock_guard<std::mutex> g(g_lock);
    FILE *f = nullptr;
    if (_wfopen_s(&f, ini_file().c_str(), L"w") || !f) return;
    fprintf(f, "# BlackDefinitiveHDPC settings (written by PC SETTINGS and on exit; command-line flags override them)\n");
    if (g_cfg.render_native) fprintf(f, "render = native\n");
    else if (g_cfg.render_h) fprintf(f, "render = %d\n", g_cfg.render_h);
    else fprintf(f, "render = scale\n");
    fprintf(f, "render_factor = %.2f\nscale = %.3f\n", g_cfg.render_factor, g_cfg.scale);
    if (g_cfg.window_w || g_cfg.window_h) // RESOLUTION: the window size, in the ini so it sticks
        fprintf(f, "window = %dx%d\n", g_cfg.window_w, g_cfg.window_h);
    fprintf(f, "aniso = %d\n", g_cfg.aniso);
    int fps = g_cfg.fps_next ? g_cfg.fps_next : g_cfg.fps; // a menu choice for the next run wins
    bool wide = g_cfg.widescreen_next >= 0 ? g_cfg.widescreen_next != 0 : g_cfg.widescreen;
    fprintf(f, "aa = %d\nfps = %d\nwidescreen = %d\nfullscreen = %d\n", g_cfg.aa ? 1 : 0, fps, wide ? 1 : 0, g_cfg.fullscreen ? 1 : 0);
    fprintf(f, "gain = %.2f\ngamma = %.2f\n", g_cfg.gain, g_cfg.gamma);
    fprintf(f, "mouse_sens = %.3f\nmouse_invert = %d\n", g_cfg.mouse_sens, g_cfg.mouse_invert ? 1 : 0);
    fprintf(f, "pad_sens = %.3f\n", g_cfg.pad_sens);    fprintf(f, "aim_hold = %d\ncrouch_hold = %d\nwalk_speed = %.2f\n", g_cfg.aim_hold ? 1 : 0, g_cfg.crouch_hold ? 1 : 0, g_cfg.walk_speed);
    fprintf(f, "hd_reticle = %d\n", g_cfg.hd_reticle ? 1 : 0);
    fprintf(f, "texture_pack = %s\ntexture_pack_max = %d\n", narrow(g_cfg.texture_pack).c_str(), g_cfg.texture_pack_max);
    if (g_cfg.reticle_colour >= 0) fprintf(f, "reticle_colour = %06X\n", g_cfg.reticle_colour);
    else fprintf(f, "reticle_colour = game\n");
    if (g_cfg.reticle_enemy_colour >= 0) fprintf(f, "reticle_enemy_colour = %06X\n", g_cfg.reticle_enemy_colour);
    else fprintf(f, "reticle_enemy_colour = game\n");
    fprintf(f, "reverb = %d\nreverb_gain = %.2f\n", g_cfg.reverb ? 1 : 0, g_cfg.reverb_gain);
    fprintf(f, "autoaim = %d\n", !g_cfg.autoaim ? 0 : g_cfg.autoaim_pad_only ? 2 : 1); // 2 = controller only
    fprintf(f, "fov = %.1f\nviewmodel_fov = %.1f\n", g_cfg.fov, g_cfg.viewmodel_fov);
    fprintf(f, "fpscounter = %d\nsubtitles = %d\n", g_cfg.fpscounter ? 1 : 0, g_cfg.subtitles ? 1 : 0);
    fprintf(f, "camera_shake = %d\nvsync = %d\n", g_cfg.camera_shake ? 1 : 0, g_cfg.vsync ? 1 : 0);
    fprintf(f, "reload_blur = %d\n", g_cfg.reload_blur ? 1 : 0);
    bool textures = g_cfg.textures_next >= 0 ? g_cfg.textures_next != 0 : g_cfg.textures; // read at start, as fps
    int language = g_cfg.language_next >= 0 ? g_cfg.language_next : g_cfg.language;
    fprintf(f, "textures = %d\nlanguage = %s\n", textures ? 1 : 0, lang::code(language));
    fprintf(f, "draw_distance = %d\n", g_cfg.draw_distance);
    fprintf(f, "ambient_occlusion = %s\n", g_cfg.ambient_occlusion == 2 ? "gtao" : g_cfg.ambient_occlusion ? "ssao" : "off");
    fprintf(f, "input_mode = %s\n", g_cfg.input_mode == 2 ? "controller" : g_cfg.input_mode == 1 ? "keyboard" : "auto");
    fprintf(f, "skipanymovie = %d\nskip_intro = %d\n", g_cfg.skip_movies ? 1 : 0, g_cfg.skip_intro ? 1 : 0);
    fprintf(f, "cheat_unlock_all = %d\ncheat_god = %d\ncheat_health_packs = %d\n", g_cfg.cheat_unlock_all ? 1 : 0,
            g_cfg.cheat_god ? 1 : 0, g_cfg.cheat_health_packs ? 1 : 0);
    fprintf(f, "seamfill = %d\n", g_cfg.seam_fill ? 1 : 0);
    fprintf(f, "effectsnative = %d\n", g_cfg.effects_native ? 1 : 0);
    bindings::save_ini(f);
    fclose(f);
}
