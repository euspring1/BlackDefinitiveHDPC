// Frame rate counter drawn over the picture (black.ini fpscounter, --fpscounter, PC SETTINGS > DISPLAY).
// See d3d/fps.cpp.
#pragma once

// Called once per frame with the render size, before the picture is presented.
void fps_tick(int screen_w, int screen_h);
