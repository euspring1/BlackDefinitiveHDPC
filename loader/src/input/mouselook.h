// Mouse look: the mouse rotates the player directly (patches in the game's player update).
#pragma once

#include <cstdint>

void install_mouselook_patches();
bool mouselook_installed();
// The frame (g_frame_count) of the last player update, 0 before the first: menus, pauses and cutscenes do not
// run it (input/bindings.cpp tells play from menus by it).
uint32_t mouselook_play_frame();
// Whether the player aims (weapon zoom) as of the last player update: 1 / 0, -1 if not known (no player update
// yet). The flag FUN_001bd3f0 tests for its x0.7 aim slowdown: [[controller+0x8C]+0x29C]+0x105.
int mouselook_aiming();
// The player's two-weapon inventory as of the last player update ([controller+0x8C] + 0x278, the object
// FUN_001BAB10 swaps in): slot in hand +0x43 (0 / 1, -1 = not known), a swap running (+0x38 != 0), and whether
// the other slot holds a weapon (the list at +0x20).
struct WeaponState {
    int slot = -1;
    bool swapping = false;
    bool other = false;
};
WeaponState mouselook_weapon();
// The mouse angle for the next player update, in degrees (yaw + = left, pitch + = the game's pitch rate
// direction). Called once per pad poll; replaces what the previous frame did not use.
void mouselook_set_frame(float yaw_deg, float pitch_deg);
