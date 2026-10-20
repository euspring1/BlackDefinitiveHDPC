// PC SETTINGS > CHEATS. BLACK on the Xbox has no button codes: what it gives is earned by finishing the game (silver
// weapons with unlimited ammo, the Black Ops difficulty, the M16A2) and a BFG for a profile named with a code. The
// port's cheats use the game's own state for each:
//
// UNLOCK ALL. The profile's mission table (0x2D0D84: 4 difficulties x 8 missions, 0x20 bytes each; +0 the state,
// 2 = finished, -1 = never played; +0x1A unlocked) is what the front end reads (FUN_00115030 builds FELevelUnlocked1-8
// and the finished flags from it) and what decides the rewards: a difficulty finished gives its silver weapons, Hard
// the Black Ops difficulty, Black Ops the M16A2. Marking every mission finished and unlocked is a 100% profile; the
// game saves it with the profile, so it stays after the cheat is turned off.
//
// GOD MODE. A character's damage handlers (FUN_001ED1E0, FUN_001ED2B0, FUN_001ED700, FUN_001ED990; ecx/esi/ebx = the
// character) all leave it alone while its byte +0x8F2 is set, the flag the cutscene cameras set (FUN_002002C0). The
// player's character ([controller +0x8C], its +0x324 the controller again) gets it at each player update, and its
// health (+0x2F0, the full value 1200 on Easy/Normal, 0x288898 / 0x2888A0 per difficulty as FUN_00013F10 scales it)
// is kept full so the HUD bar shows it.
//
// INFINITE HEALTH PACKS. The packs carried are the byte +0x8E1 (FUN_001EA730 uses one and decrements it; the HUD
// shows three slots): kept at three.
#include "cheats.h"

#include "common.h"
#include "input/bindings.h"

namespace {

constexpr uint32_t kMissions = 0x002D0D84, kGame = 0x002D45F0, kDifficulty = 0x2014C;
constexpr uint32_t kHealth = 0x2F0, kPacks = 0x8E1, kInvulnerable = 0x8F2;
constexpr uint8_t kMaxPacks = 3;

bool g_god_set; // the port set the flag (so it clears it when GOD MODE goes off)

} // namespace

void cheats_frame()
{
    if (!g_cfg.known_build || !g_cfg.cheat_unlock_all || bindings::in_play()) return;
    if (!*(const uint32_t *)(uintptr_t)kGame) return;
    int changed = 0;
    for (int i = 0; i < 32; i++) {
        uint8_t *r = (uint8_t *)(uintptr_t)(kMissions + i * 0x20);
        if (*(int32_t *)r != 2 || r[0x1A] != 1) {
            *(int32_t *)r = 2;
            r[0x1A] = 1;
            changed++;
        }
    }
    if (changed) logf("cheats: UNLOCK ALL marked %d missions finished", changed);
}

void cheats_player(uint8_t *chr)
{
    if (!chr) return;
    if (g_cfg.cheat_god) {
        chr[kInvulnerable] = 1;
        g_god_set = true;
        uint32_t game = *(const uint32_t *)(uintptr_t)kGame;
        int diff = game ? *(const int32_t *)(uintptr_t)(game + kDifficulty) : 0;
        float full = *(const float *)(uintptr_t)(diff >= 2 ? 0x002888A0 : 0x00288898);
        if (full > 0.0f && *(float *)(chr + kHealth) < full) *(float *)(chr + kHealth) = full;
    } else if (g_god_set) {
        chr[kInvulnerable] = 0;
        g_god_set = false;
    }
    if (g_cfg.cheat_health_packs && chr[kPacks] < kMaxPacks) chr[kPacks] = kMaxPacks;
    if (g_cfg.verbose) {
        static uint32_t last;
        if (g_frame_count - last >= 600) {
            last = g_frame_count;
            logf("cheats: health %.0f, packs %u, invulnerable %u", *(const float *)(chr + kHealth), chr[kPacks], chr[kInvulnerable]);
        }
    }
}
