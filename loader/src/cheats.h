// PC SETTINGS > CHEATS (cheats.cpp).
#pragma once

#include <cstdint>

void cheats_frame();                // once a frame: UNLOCK ALL (in the menus)
void cheats_player(uint8_t *chr);   // at each player update, with the player's character: GOD MODE, HEALTH PACKS
