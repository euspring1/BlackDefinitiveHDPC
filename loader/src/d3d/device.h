#pragma once

#include "d3d/xd3d.h"

#include <d3d9.h>

namespace xd3d {

extern uint8_t *g_dev;        // Xbox device object
extern uint32_t *g_push_base; // push buffer start
extern uint32_t g_push_dwords;
extern int g_width, g_height; // back buffer size

void hle_flush(); // interpret pending inline push-buffer commands

// Render resolution: host render targets are scale_x() x scale_y() times the Xbox size (--scale; in widescreen
// the Xbox pixels are 4/3 wider than tall, so x is scaled 4/3 more to keep host pixels square).
float scale_x();
float scale_y();
float target_scale_x(); // the bound render target's scale (1 for the small post-processing buffers)
float target_scale_y();
uint32_t host_w(uint32_t xbox_w);
uint32_t host_h(uint32_t xbox_h);
void request_render_change(); // the render-size settings in g_cfg changed (PC SETTINGS): applied after the next Swap
void request_render_rebuild(); // re-create every host target after the next Swap, also at the same render size
// Frameskip: this frame is not drawn (its draws and clears are dropped, nothing is presented) so a late world
// catches up with real time (device.cpp, decided at each Swap).
bool frame_skipped();

// The bound render target and its depth buffer as host textures (ambient occlusion, d3d/occlusion.cpp): false
// unless the depth is an INTZ texture as large as the target. vp: the game's viewport in the target's pixels.
struct SceneTargets {
    IDirect3DTexture9 *color, *depth;
    IDirect3DSurface9 *depth_surface;
    uint32_t w, h;
    float vp[4], min_z, max_z;
};
bool scene_targets(SceneTargets &out);
void forget_host_state(); // the host device's state was changed outside the game's draws: re-send it

} // namespace xd3d
