// What the game does once a frame, made for 30 frames a second, at the port's frame rate (timing.cpp steps the world
// 1/fps s a frame). Each step below is something the game changes by a fixed amount or a fixed factor a frame: at
// 120 fps it happened four times as often. Here each takes its 30 fps share of a frame, share = 30 / fps: an amount
// times share, a factor to the power of share, a countdown only on every (1 / share)th frame.
//
// Study: BLACK-XBERecomp (TGP482), whose recompiled game hooks the same places of the same XBE; redone here as
// mid-function hooks (detour.h). The physics' sleep rule and damping are in timing.cpp.
//
// Physics (the solver of the physics world, *0x2D45F4 -> +0x18):
// - a body's top speed is a move a step (0x77DFF linear, 0x77E74 angular, compared squared);
// - a joint lets its bodies turn apart by at most an angle a step (0x7941E..0x7946F), and puts a turn past its
//   limit or lock right within one step (0x79327), its anchors' gap too (0x78B0F): faster steps pushed harder;
// - contacts are looked for at least twice the fall of a step ahead (0xA18A5: 1 / the step) - at 30 fps steps;
// - a ragdoll brakes for a wall within 8 frames of its speed (0xE054A, 0xE056D) - frames at 30.
// Without these, bodies falling at high frame rates came to rest in mid-air (corpses, dropped guns: issue #1).
// Glass shards (0x1690D0) move, turn and count down once a frame; the sound manager counts its shard sound slots and
// four timers down once a call (0x14B4F5). The player: recoil dies away by a factor a frame (0x1E5FB7), the gun's
// sway trails the stick by a share a frame (0x1E6392 .. 0x1E644A), the aim's zoom moves by a share a frame
// (0x1BB2BA), the move speed eases to the stick's by a step a frame (0x1CC672).
#include "framerate.h"

#include "common.h"
#include "detour.h"

#include <cmath>

namespace {

float g_share = 1.0f; // 30 / fps

template <class T> T &at(uint32_t a) { return *(T *)(uintptr_t)a; }

// a countdown that runs once a frame at 30 fps: true on those frames
struct Tick {
    float n = 0.0f;
    bool step()
    {
        n += g_share;
        if (n < 1.0f) return false;
        n -= 1.0f;
        return true;
    }
};

bool __cdecl scale_x0(HookRegs *, HookFx *fx)
{
    fx->xmm[0][0] *= g_share;
    return true;
}
bool __cdecl scale_x1(HookRegs *, HookFx *fx)
{
    fx->xmm[1][0] *= g_share;
    return true;
}
bool __cdecl scale_x6(HookRegs *, HookFx *fx)
{
    fx->xmm[6][0] *= g_share;
    return true;
}

// xmm1 the world's rate (1 / step): the game's 30
bool __cdecl contact_reach(HookRegs *, HookFx *fx)
{
    fx->xmm[1][0] = 30.0f;
    return true;
}

// xmm0 the ragdoll's move this frame: in frames at 30
bool __cdecl ragdoll_look_ahead(HookRegs *, HookFx *fx)
{
    fx->xmm[0][0] /= g_share;
    return true;
}

// xmm1 the gap of a joint's anchors after the step, xmm2 the part of it the bodies' own motion makes
bool __cdecl joint_anchor_gap(HookRegs *, HookFx *fx)
{
    for (int i = 0; i < 4; i++) {
        float motion = fx->xmm[2][i];
        fx->xmm[1][i] = motion + (fx->xmm[1][i] - motion) * g_share;
    }
    return true;
}

// the turn a step may make on each of three axes, [esp+0x60] low to [esp+0x80] high: a lock, or a limit already
// passed, asks for a turn back
bool __cdecl joint_turn_back(HookRegs *r, HookFx *)
{
    for (uint32_t axis = 0; axis < 12; axis += 4) {
        float &low = at<float>(r->esp() + 0x60 + axis), &high = at<float>(r->esp() + 0x80 + axis);
        bool locked = low == high;
        if (locked || high < 0) high *= g_share;
        if (locked || low > 0) low *= g_share;
    }
    return true;
}

Tick g_shards;
bool g_shard_tick;
bool __cdecl shards_frame(HookRegs *, HookFx *)
{
    g_shard_tick = g_shards.step();
    return true;
}
bool __cdecl shard_tick(HookRegs *, HookFx *) { return g_shard_tick; }

// ebx the shard, xmm1 the new turn a frame: the angle (+0xF4) takes a share of it
bool __cdecl shard_turn(HookRegs *r, HookFx *fx)
{
    fx->xmm[0][0] = at<float>(r->ebx + 0xF4) + fx->xmm[1][0] * g_share;
    return true;
}

// xmm0 a shard's move a frame
bool __cdecl shard_step(HookRegs *, HookFx *fx)
{
    for (int i = 0; i < 4; i++) fx->xmm[0][i] *= g_share;
    return true;
}

Tick g_sound;
bool __cdecl sound_countdown(HookRegs *, HookFx *) { return g_sound.step(); }

// xmm0 what is left of the recoil after a frame, a factor
bool __cdecl recoil_decay(HookRegs *, HookFx *fx)
{
    float &k = fx->xmm[0][0];
    if (k > 0.0f) k = powf(k, g_share);
    return true;
}

// eax the weapon's data: +0x104 and +0x108, the two shares a frame the gun trails the stick by, for this update only
uint32_t g_sway;
float g_sway_share[2];
bool __cdecl sway_begin(HookRegs *r, HookFx *)
{
    g_sway = r->eax + 0x104;
    for (int i = 0; i < 2; i++) {
        float &k = at<float>(g_sway + 4 * i);
        g_sway_share[i] = k;
        k = 1.0f - powf(1.0f - k, g_share);
    }
    return true;
}
bool __cdecl sway_end(HookRegs *, HookFx *)
{
    if (!g_sway) return true;
    for (int i = 0; i < 2; i++) at<float>(g_sway + 4 * i) = g_sway_share[i];
    g_sway = 0;
    return true;
}

// ecx the weapon, xmm0 the zoom after this frame's step: the step's share, towards 1 when the aim is let go
bool __cdecl zoom_step(HookRegs *r, HookFx *fx)
{
    uint32_t owner = at<uint32_t>(r->ecx + 0xF0);
    if (!owner) return true;
    float old = at<float>(owner + 0x2A4), &now = fx->xmm[0][0];
    if (now == old || old <= 0.0f) return true;
    bool out = !at<uint8_t>(r->ecx + 0x105) && old > 1.0f;
    if (out) {
        float f = (now - 1.0f) / (old - 1.0f);
        if (f > 0.0f) now = 1.0f + (old - 1.0f) * powf(f, g_share);
    } else {
        float f = now / old;
        if (f > 0.0f) now = old * powf(f, g_share);
    }
    return true;
}

} // namespace

void install_framerate_patches(int fps)
{
    if (fps <= 30) return;
    g_share = 30.0f / (float)fps;
    using B = std::vector<uint8_t>;
    bool physics = hook_install("framerate (physics)", {
        {0x00077DFF, B{0xF3, 0x0F, 0x59, 0xC9, 0x0F, 0x2F, 0xC1}, scale_x1}, // mulss xmm1, xmm1; comiss xmm0, xmm1
        {0x00077E74, B{0xF3, 0x0F, 0x59, 0xC9, 0x0F, 0x2F, 0xC1}, scale_x1},
        {0x0007941E, B{0xF3, 0x0F, 0x11, 0x44, 0x24, 0x70}, scale_x0},       // movss [esp+0x70], xmm0
        {0x00079429, B{0xF3, 0x0F, 0x11, 0x44, 0x24, 0x74}, scale_x0},
        {0x00079464, B{0xF3, 0x0F, 0x11, 0x74, 0x24, 0x70}, scale_x6},       // movss [esp+0x70], xmm6
        {0x0007946F, B{0xF3, 0x0F, 0x11, 0x74, 0x24, 0x74}, scale_x6},
        {0x00078B0F, B{0x0F, 0x28, 0x9C, 0x24, 0xF8, 0x01, 0x00, 0x00}, joint_anchor_gap}, // movaps xmm3, [esp+0x1F8]
        {0x00079327, B{0x0F, 0x28, 0x83, 0xA0, 0x00, 0x00, 0x00}, joint_turn_back},        // movaps xmm0, [ebx+0xA0]
        {0x000A18A5, B{0x8D, 0x54, 0x24, 0x20, 0xF3, 0x0F, 0x11, 0x02}, contact_reach},    // lea edx, [esp+0x20]; movss [edx], xmm0
        {0x000E054A, B{0x0F, 0x2F, 0x05, 0x24, 0xB5, 0x28, 0x00}, ragdoll_look_ahead},     // comiss xmm0, [0x28B524]
        {0x000E056D, B{0x8D, 0x54, 0x24, 0x40, 0x52}, ragdoll_look_ahead},                 // lea edx, [esp+0x40]; push edx
    });
    bool shards = hook_install("framerate (glass)", {
        {0x001690D0, B{0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, shards_frame}, // the shards' update: its prologue
        // dec word [ebx+0x10A] (a shard's countdown), skipped off the tick
        {0x00169160, B{0x66, 0xFF, 0x8B, 0x0A, 0x01, 0x00, 0x00}, shard_tick, {}, 0, {}, 0, true},
        // dec eax (the next countdown); mov edi, ebx; add esi, 0x11164 - the dec skipped off the tick
        {0x001691CF, B{0x48, 0x8B, 0xFB, 0x81, 0xC6, 0x64, 0x11, 0x01, 0x00}, shard_tick, {}, 0,
         B{0x8B, 0xFB, 0x81, 0xC6, 0x64, 0x11, 0x01, 0x00}, 0, true},
        {0x00169368, B{0xF3, 0x0F, 0x58, 0x8B, 0xF8, 0x00, 0x00, 0x00}, scale_x1}, // addss xmm1, [ebx+0xF8]: the spin's gain
        {0x001693A2, B{0xF3, 0x0F, 0x11, 0x83, 0xF4, 0x00, 0x00, 0x00}, shard_turn}, // movss [ebx+0xF4], xmm0
        {0x001693ED, B{0x0F, 0x58, 0xC8, 0x0F, 0x29, 0x08}, shard_step},             // addps xmm1, xmm0; movaps [eax], xmm1
        {0x001693FB, B{0x0F, 0x58, 0xC8, 0x83, 0xC4, 0x04}, shard_step},             // addps xmm1, xmm0; add esp, 4
    });
    // xor eax, eax; jmp 0x14B500 (the countdown loop), and alignment: off the tick, past the loop
    bool sound = hook_install("framerate (sound countdowns)", {
        {0x0014B4F5, B{0x33, 0xC0, 0xEB, 0x07, 0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00}, sound_countdown, B{0x33, 0xC0},
         0x0014B500, {}, 0x0014B559, true},
    });
    bool player = hook_install("framerate (player)", {
        {0x001E5FB7, B{0xF3, 0x0F, 0x59, 0x83, 0xC4, 0x04, 0x00, 0x00}, recoil_decay}, // mulss xmm0, [ebx+0x4C4]
        {0x001E6392, B{0xF3, 0x0F, 0x10, 0x88, 0xF4, 0x00, 0x00, 0x00}, sway_begin},   // movss xmm1, [eax+0xF4]
        {0x001E644A, B{0x89, 0x44, 0x24, 0x1C, 0xC7, 0x44, 0x24, 0x14, 0x00, 0x00, 0x00, 0x80}, sway_end},
        {0x001BB2BA, B{0xA1, 0xB8, 0x12, 0x2D, 0x00}, zoom_step},                        // mov eax, [0x2D12B8]
        {0x001CC672, B{0x0F, 0x2F, 0xC8, 0x0F, 0x57, 0xD2}, scale_x1},                   // comiss xmm1, xmm0; xorps xmm2, xmm2
    });
    logf("framerate: %d fps, a frame's share %.3f: physics %s, glass %s, sound countdowns %s, recoil / sway / zoom / "
         "move %s", fps, g_share, physics ? "on" : "off", shards ? "on" : "off", sound ? "on" : "off", player ? "on" : "off");
}
