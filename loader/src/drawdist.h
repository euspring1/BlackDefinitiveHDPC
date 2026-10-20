#pragma once

void install_drawdist_patches(); // after the XBE is mapped; DRAW DISTANCE (g_cfg.draw_distance)
void drawdist_set(int factor);   // 1 (the game's), 2, 4, 8, 12 or 16; applies at once
