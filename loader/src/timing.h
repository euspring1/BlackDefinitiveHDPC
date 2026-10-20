// Game timing: make the Xbox time-stamp counter users tick at the Xbox CPU clock (733.33 MHz).
#pragma once

void install_timing_patches(); // after image_map; known build only

// 60 fps: BLACK steps its world by a fixed frame time chosen at start-up from the video standard (game init
// 0x20C282: NTSC 1/30 s and 33 ms, PAL 1/25 s and 40 ms, into 0x2F3A38 / 0x2F3800; the game's own
// setFrameRate FUN_000BE560 does the same for any rate). Make the NTSC choice 1/60 s and 16 ms, and point the
// two other uses of the 1/30 constant (0x295FC0) at 1/60. Any higher rate works the same way (1/fps s and
// 1000/fps ms). Returns false if the code does not match.
bool install_fps_patches(int fps);
void timing_frame_swapped(); // at each Swap: a late frame gives the world a longer step (no slow motion)

// The frame rates the port offers (30 = the original); anything else becomes the nearest of them.
constexpr int kFrameRates[] = {30, 60, 120, 144, 165, 240};
int valid_fps(int fps);
