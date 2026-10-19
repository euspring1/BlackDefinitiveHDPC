// The menu cursor: the port's own seta, drawn over the picture while a menu page is up (d3d/cursor.cpp).
#pragma once

// Draws the cursor at the pointer's place. Called once a frame, before the picture is presented.
void cursor_tick(int screen_w, int screen_h);
// The cursor's texture is up: the Windows cursor can stay hidden in the menus (d3d/host.cpp, WM_SETCURSOR).
bool cursor_ready();
