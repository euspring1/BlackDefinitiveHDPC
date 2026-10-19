// First run (no saved profile in the port's savedata yet): the game's audio settings start at sound effects 60 % and
// music 40 % instead of the game's own defaults.
//
// The settings block of the game object (*0x2D45F0, saved with the profile): +0x2015C sound effects and +0x20160
// music, floats 0..1. The audio options page shows them as 0..100 from 0x2F416C / 0x2F4170 (FUN_0011CD20 fills those
// from the block after a profile loads), and FUN_00119B50 hands the block's volumes to the mixer.
//
// Until a profile exists the port puts its values in whenever the block holds the game's defaults (the first values
// seen); a value the player chose differs from them and is left alone. Once the profile is saved it carries the
// values and the port stops.
#include "audio_defaults.h"

#include "common.h"

#include <cstdint>

namespace {

constexpr uint32_t kGameObject = 0x002D45F0, kSoundObject = 0x002D0D60;
constexpr uint32_t kSfx = 0x2015C, kMusic = 0x20160;          // floats in the game object
constexpr uint32_t kSfxMenu = 0x002F416C, kMusicMenu = 0x002F4170; // the options page's 0..100
constexpr uint32_t kApplyVolumes = 0x00119B50;
constexpr float kFirstSfx = 0.60f, kFirstMusic = 0.40f;

bool g_active;          // no profile at start: this is the first run
bool g_seen;            // the game's defaults were read
float g_def_sfx, g_def_music;
uint32_t g_frames;

std::wstring profiles_dir()
{
    std::wstring hdd = g_cfg.hdd_dir.empty() ? g_cfg.port_dir + L"\\savedata" : g_cfg.hdd_dir;
    return hdd + L"\\Partition1\\UDATA\\45410083";
}

bool profile_exists()
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((profiles_dir() + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do found = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.';
    while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

} // namespace

void audio_defaults_init()
{
    g_active = !profile_exists();
    if (g_active) logf("[audio] first run: sound effects %d %%, music %d %%", (int)(kFirstSfx * 100 + 0.5f), (int)(kFirstMusic * 100 + 0.5f));
}

void audio_defaults_frame()
{
    if (!g_active) return;
    uint32_t game = *(const uint32_t *)(uintptr_t)kGameObject;
    if (!game || !*(const uint32_t *)(uintptr_t)kSoundObject) return;
    float *sfx = (float *)(uintptr_t)(game + kSfx), *music = (float *)(uintptr_t)(game + kMusic);
    if (!g_seen) {
        if (*sfx <= 0.0f || *sfx > 1.0f || *music <= 0.0f || *music > 1.0f) return; // not set up yet
        g_def_sfx = *sfx;
        g_def_music = *music;
        g_seen = true;
        logf("[audio] the game's default volumes: sound effects %.2f, music %.2f", g_def_sfx, g_def_music);
    }
    if (*sfx == g_def_sfx && *music == g_def_music && (g_def_sfx != kFirstSfx || g_def_music != kFirstMusic)) {
        *sfx = kFirstSfx;
        *music = kFirstMusic;
        *(int32_t *)(uintptr_t)kSfxMenu = (int32_t)(kFirstSfx * 100 + 0.5f);
        *(int32_t *)(uintptr_t)kMusicMenu = (int32_t)(kFirstMusic * 100 + 0.5f);
        ((void(__cdecl *)())(uintptr_t)kApplyVolumes)();
        logf("[audio] first-run volumes set");
    }
    if (++g_frames % 120 == 0 && profile_exists()) {
        g_active = false;
        logf("[audio] a profile was saved: first-run volumes done (sound effects %.2f, music %.2f)", *sfx, *music);
    }
}
