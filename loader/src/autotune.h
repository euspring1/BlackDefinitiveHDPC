// Settings for weaker PCs (autotune.cpp): a light profile on a first start with integrated graphics, then a lower
// render size at once (and 60 fps from the next start) while the PC cannot keep 60 fps. The first session only.
#pragma once

#include <cstdint>

namespace autotune {
void start();                    // after settings_load, before the frame rate patches
void frame(bool skipped);        // every Swap (d3d/device.cpp): whether the frameskip left this frame undrawn
void user_changed();             // the player chose a frame rate or render size: theirs from now on
} // namespace autotune
