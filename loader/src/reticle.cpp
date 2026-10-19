// The aiming reticle's colours. FUN_00139BF0 (HUD reticle update, once a frame) multiplies the reticle's texels by
// an RGB tint chosen from what is under it: 0x2ADC20 nothing in particular (0.776 grey), 0x2ADC30 a character
// without AI (0.7 green), 0x2ADC40 a character with an AI part, an enemy (0.7 red); two more cases come from the
// HUD's own settings (+0x190, +0x220). Only that function reads these tables. black.ini reticle_colour and
// reticle_enemy_colour (RRGGBB) replace the first and the third; with a white reticle texture (textures\) the
// tint is the reticle's colour.
#include "reticle.h"

#include "common.h"

#include <cstring>

namespace {

void set_rgb(uint32_t addr, uint32_t rgb)
{
    float *f = (float *)(uintptr_t)addr;
    DWORD old;
    if (!VirtualProtect(f, 12, PAGE_READWRITE, &old)) return;
    f[0] = ((rgb >> 16) & 0xFF) / 255.0f;
    f[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    f[2] = (rgb & 0xFF) / 255.0f;
}

} // namespace

void install_reticle_colours()
{
    static const float kExpect[3] = {0.0f, 0.7f, 0.0f}; // the green table at 0x2ADC30, as a check of the build
    if (memcmp((const void *)(uintptr_t)0x002ADC30, kExpect, sizeof kExpect) != 0) {
        logf("reticle: colour tables do not match this build; colours unchanged");
        return;
    }
    if (g_cfg.reticle_colour >= 0) set_rgb(0x002ADC20, (uint32_t)g_cfg.reticle_colour);
    if (g_cfg.reticle_enemy_colour >= 0) set_rgb(0x002ADC40, (uint32_t)g_cfg.reticle_enemy_colour);
    if (g_cfg.reticle_colour >= 0 || g_cfg.reticle_enemy_colour >= 0)
        logf("reticle: colour %06X, on an enemy %06X", g_cfg.reticle_colour >= 0 ? g_cfg.reticle_colour : 0xC6C6C6,
             g_cfg.reticle_enemy_colour >= 0 ? g_cfg.reticle_enemy_colour : 0xB30000);
}
