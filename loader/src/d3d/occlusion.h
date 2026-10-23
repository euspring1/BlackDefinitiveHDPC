// AMBIENT OCCLUSION (d3d/occlusion.cpp): g_cfg.ambient_occlusion 0 off, 1 SSAO, 2 GTAO, read every frame.
#pragma once

namespace occlusion {
void install();   // the hook before the frame's alpha-blended list (known build only)
void note_draw(); // each draw with a vertex program (d3d/draw.cpp): the fog it draws
void release();   // before a device Reset or a new render size: the host targets go
} // namespace occlusion
