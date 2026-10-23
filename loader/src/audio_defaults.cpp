// The audio volumes live in black.ini (sound_volume / music_volume, first start: sound effects 60 %, music 40 %).
// The game keeps them in its profile, but a profile load (every mission start loads it) puts back the profile's own
// values, 100 % on profiles saved before, and the pause menu's options never write them to the profile at all: the
// volumes the player chose went back to 100 % at the next mission.
//
// The settings block of the game object (*0x2D45F0, saved with the profile): +0x2015C sound effects and +0x20160
// music, floats 0..1. The sound options show them as 0..100 from 0x2F416C / 0x2F4170 (FUN_0011CD20 fills those from
// the block after a profile loads), and FUN_00119B50 hands the block's volumes to the mixer.
//
// On the sound options pages (the title's Options > Sound and the pause menu's options) a change is the player's: it
// goes to black.ini. Anywhere else the block is held at black.ini's values.
#include "audio_defaults.h"

#include "common.h"
#include "settings.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kGameObject = 0x002D45F0, kSoundObject = 0x002D0D60, kFrontEnd = 0x002D199C;
constexpr uint32_t kSfx = 0x2015C, kMusic = 0x20160;          // floats in the game object
constexpr uint32_t kSfxMenu = 0x002F416C, kMusicMenu = 0x002F4170; // the options pages' 0..100
constexpr uint32_t kApplyVolumes = 0x00119B50;

bool g_dirty; // a volume the player chose waits for black.ini (written when the options page closes)

bool on_sound_options()
{
    uint32_t fe = *(const uint32_t *)(uintptr_t)kFrontEnd;
    if (!fe) return false;
    const char *page = (const char *)(uintptr_t)(fe + 0x3840);
    return !_stricmp(page, "soundsetupxbox") || !_stricmp(page, "PauseMenuOptionXbox");
}

int percent(float v) { return (int)std::lround(v * 100.0f); }

} // namespace

void audio_defaults_init()
{
    logf("[audio] volumes: sound effects %d %%, music %d %% (black.ini)", g_cfg.sfx_volume, g_cfg.music_volume);
}

void audio_defaults_frame()
{
    uint32_t game = *(const uint32_t *)(uintptr_t)kGameObject;
    if (!game || !*(const uint32_t *)(uintptr_t)kSoundObject) return;
    float *sfx = (float *)(uintptr_t)(game + kSfx), *music = (float *)(uintptr_t)(game + kMusic);
    if (!(*sfx >= 0.0f && *sfx <= 1.0f && *music >= 0.0f && *music <= 1.0f)) return; // not set up yet
    // the pages may hand the sliders to the block only as they close: a change just after one is the player's too
    static ULONGLONG s_options_until;
    ULONGLONG now = GetTickCount64();
    bool options = on_sound_options();
    if (options) s_options_until = now + 1500;
    int s = percent(*sfx), m = percent(*music);
    if (options) { // the sliders themselves, in case the page writes the block only as it closes
        int ms = *(const int32_t *)(uintptr_t)kSfxMenu, mm = *(const int32_t *)(uintptr_t)kMusicMenu;
        static int s_ms = -1, s_mm = -1;
        if (s_ms >= 0 && (ms != s_ms || mm != s_mm) && ms >= 0 && ms <= 100 && mm >= 0 && mm <= 100) s = ms, m = mm;
        s_ms = ms;
        s_mm = mm;
    }
    if (s == g_cfg.sfx_volume && m == g_cfg.music_volume) {
        if (g_dirty && now > s_options_until) {
            g_dirty = false;
            settings_save();
            logf("[audio] volumes saved: sound effects %d %%, music %d %%", s, m);
        }
        return;
    }
    if (now <= s_options_until) { // the player moved a slider
        g_cfg.sfx_volume = s;
        g_cfg.music_volume = m;
        g_dirty = true;
        return;
    }
    *sfx = g_cfg.sfx_volume / 100.0f;
    *music = g_cfg.music_volume / 100.0f;
    *(int32_t *)(uintptr_t)kSfxMenu = g_cfg.sfx_volume;
    *(int32_t *)(uintptr_t)kMusicMenu = g_cfg.music_volume;
    ((void(__cdecl *)())(uintptr_t)kApplyVolumes)();
    logf("[audio] volumes put back to sound effects %d %%, music %d %% (the game had %d %%, %d %%)", g_cfg.sfx_volume,
         g_cfg.music_volume, s, m);
}
