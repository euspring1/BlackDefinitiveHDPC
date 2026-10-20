// DRAW DISTANCE (PC SETTINGS > MODS): level of detail and draw distances reach 1 (the game's), 2, 4, 8, 12 or 16
// times farther. The game decides what to draw, and how detailed, by comparing a distance to the camera with a limit
// in many places (characters, props, pickups, ground cover, particles, bullet holes); at each of them the port makes
// the distance that much shorter or the limit that much longer, read live, so a change applies at once.
//
// Most places hold the value in an SSE register just before the compare: a jump to a small stub that scales the
// register, runs the instructions the jump replaced and jumps back. Three need more:
// - particles (0x16DFFB): a particle type's fade starts as much farther as its far cut grew;
// - ground cover (0x160913): it keeps 63 patches a frame; with a longer reach the list fills up, so a patch nearer
//   than the farthest kept one takes its place instead of being dropped;
// - draw lists (0x1EE5EA, every frame): lists 0, 2 and 3 of the display (*0x2D67F4 + 0xCA6C, 0x18 each: +0 room,
//   +4 the second part of the room, +0xC capacity) hold every mesh in reach; they get more room, so a full list does
//   not drop meshes.
#include "drawdist.h"

#include "common.h"

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
float g_dd_reach = 1.0f, g_dd_reach2 = 1.0f; // the factor and its square
float g_dd_particle = 1.0f;                  // the particle fade's factor, set by the stub
uint8_t g_dd_xmm[8 * 16];                    // the SSE registers around a call into C++
uint32_t g_dd_particle_back = 0x0016E003, g_dd_cover_back = 0x00160919, g_dd_lists_back = 0x001EE5F0;
void __cdecl dd_particle(uint32_t type, uint32_t camera);
void __cdecl dd_cover(uint32_t cover, uint32_t count, uint32_t mesh, uint32_t ecx, uint32_t edx);
void __cdecl dd_lists();
}

namespace {

enum Reg { X0 = 0x05, X1 = 0x0D, X2 = 0x15 }; // ModRM of "op xmmN, [disp32]"
enum Op { Div = 0x5E, Mul = 0x59 };

// A stub: [op before] the replaced instructions [op after], back to the site's end.
struct Site {
    uint32_t at;
    std::vector<uint8_t> replaced; // the instructions there (checked), relocatable as they are
    Op op;
    Reg reg;
    bool squared; // by the factor's square
    bool after;   // scale after the replaced instructions (they load the value)
};

// From a study of the places where the game compares draw distances (see the top).
const Site kSites[] = {
    // characters' detail: mulss xmm1, xmm0; mov eax, esi (xmm1 the distance)
    {0x001C5651, {0xF3, 0x0F, 0x59, 0xC8, 0x8B, 0xC6}, Div, X1, false, true},
    // distances: movss xmm0, [ecx+0x16A0]; mulss xmm0, xmm1
    {0x001C8419, {0xF3, 0x0F, 0x10, 0x81, 0xA0, 0x16, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0xC1}, Div, X0, false, true},
    // comiss xmm0, [limit] with xmm0 the distance
    {0x001CA677, {0x0F, 0x2F, 0x05, 0x98, 0x5C, 0x29, 0x00}, Div, X0, false, false},
    {0x001D0FB1, {0x0F, 0x2F, 0x05, 0xEC, 0x5C, 0x29, 0x00}, Div, X0, false, false},
    {0x001CA942, {0x0F, 0x2F, 0x05, 0x38, 0x5D, 0x29, 0x00}, Div, X0, false, false},
    {0x0017DAD3, {0x0F, 0x2F, 0x05, 0x08, 0x7F, 0x2A, 0x00}, Div, X0, false, false},
    // ground cover: subss xmm0, [eax+0x430] with xmm0 the distance
    {0x0016086D, {0xF3, 0x0F, 0x5C, 0x80, 0x30, 0x04, 0x00, 0x00}, Div, X0, false, false},
    // limits: movss xmm1, [0x28B51C] then comiss xmm1, xmm0 (xmm0 the limit)
    {0x001CA694, {0xF3, 0x0F, 0x10, 0x0D, 0x1C, 0xB5, 0x28, 0x00}, Mul, X0, false, true},
    {0x001D0FCE, {0xF3, 0x0F, 0x10, 0x0D, 0x1C, 0xB5, 0x28, 0x00}, Mul, X0, false, true},
    // comiss xmm0, [ebx+0xC0] with xmm0 the limit
    {0x001CA77A, {0x0F, 0x2F, 0x83, 0xC0, 0x00, 0x00, 0x00}, Mul, X0, false, false},
    {0x001CB465, {0x0F, 0x2F, 0x83, 0xC0, 0x00, 0x00, 0x00}, Mul, X0, false, false},
    {0x001D12C1, {0x0F, 0x2F, 0x83, 0xC0, 0x00, 0x00, 0x00}, Mul, X0, false, false},
    // particles' far cut: mulss xmm0, xmm1; movss [esp+0x20], xmm0 (xmm0 the type's cut)
    {0x0016DF94, {0xF3, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20}, Mul, X0, false, false},
    // mov eax, [edi+0x54]; movss xmm1, [0x295C74] (xmm0 the limit)
    {0x0015ADA8, {0x8B, 0x47, 0x54, 0xF3, 0x0F, 0x10, 0x0D, 0x74, 0x5C, 0x29, 0x00}, Mul, X0, false, false},
    // squared distance: subss xmm0, [0x295CF8]
    {0x0016A1D8, {0xF3, 0x0F, 0x5C, 0x05, 0xF8, 0x5C, 0x29, 0x00}, Div, X0, true, false},
    // props: movaps xmm2, xmm0; mulss xmm2, xmm0 (xmm2 the squared limit)
    {0x001CA658, {0x0F, 0x28, 0xD0, 0xF3, 0x0F, 0x59, 0xD0}, Mul, X2, true, true},
};

uint8_t *g_code;
size_t g_used;

uint8_t *emit(const std::vector<uint8_t> &b)
{
    uint8_t *p = g_code + g_used;
    memcpy(p, b.data(), b.size());
    g_used += b.size();
    return p;
}

void put32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

void jump(uint32_t from, uint32_t to, size_t room)
{
    auto *p = (uint8_t *)(uintptr_t)from;
    p[0] = 0xE9;
    int32_t rel = (int32_t)(to - (from + 5));
    memcpy(p + 1, &rel, 4);
    memset(p + 5, 0x90, room - 5);
}

bool matches(uint32_t at, const uint8_t *b, size_t n) { return memcmp((const void *)(uintptr_t)at, b, n) == 0; }

const uint8_t kParticle[] = {0xF3, 0x0F, 0x59, 0xD9, 0xF3, 0x0F, 0x5C, 0xD3};       // mulss xmm3, xmm1; subss xmm2, xmm3
const uint8_t kCover[] = {0x8B, 0x83, 0x28, 0x04, 0x00, 0x00};                     // mov eax, [ebx+0x428]
const uint8_t kLists[] = {0x8B, 0x15, 0xF0, 0x45, 0x2D, 0x00};                     // mov edx, [0x2D45F0]

// the SSE registers into g_dd_xmm and back
#define DD_SAVE_XMM                                                                                                    \
    __asm movups xmmword ptr [g_dd_xmm + 0x00], xmm0 __asm movups xmmword ptr [g_dd_xmm + 0x10], xmm1                    \
    __asm movups xmmword ptr [g_dd_xmm + 0x20], xmm2 __asm movups xmmword ptr [g_dd_xmm + 0x30], xmm3                    \
    __asm movups xmmword ptr [g_dd_xmm + 0x40], xmm4 __asm movups xmmword ptr [g_dd_xmm + 0x50], xmm5                    \
    __asm movups xmmword ptr [g_dd_xmm + 0x60], xmm6 __asm movups xmmword ptr [g_dd_xmm + 0x70], xmm7
#define DD_LOAD_XMM                                                                                                    \
    __asm movups xmm0, xmmword ptr [g_dd_xmm + 0x00] __asm movups xmm1, xmmword ptr [g_dd_xmm + 0x10]                    \
    __asm movups xmm2, xmmword ptr [g_dd_xmm + 0x20] __asm movups xmm3, xmmword ptr [g_dd_xmm + 0x30]                    \
    __asm movups xmm4, xmmword ptr [g_dd_xmm + 0x40] __asm movups xmm5, xmmword ptr [g_dd_xmm + 0x50]                    \
    __asm movups xmm6, xmmword ptr [g_dd_xmm + 0x60] __asm movups xmm7, xmmword ptr [g_dd_xmm + 0x70]

// 0x16DFFB: edi the particle type, edx the camera, xmm1 the view's scale, xmm2 the far cut, xmm3 the fade start
__declspec(naked) void particle_thunk()
{
    __asm {
        pushfd
        pushad
    }
    DD_SAVE_XMM
    __asm {
        push edx
        push edi
        call dd_particle
        add esp, 8
    }
    DD_LOAD_XMM
    __asm {
        popad
        popfd
        mulss xmm3, dword ptr [g_dd_particle]
        mulss xmm3, xmm1
        subss xmm2, xmm3
        jmp dword ptr [g_dd_particle_back]
    }
}

// 0x160913: ebx the ground cover, eax its patch count (after the load), [ebp+8] the patch's mesh, ecx / edx its
// kind, xmm1 / xmm2 its values
__declspec(naked) void cover_thunk()
{
    __asm {
        mov eax, dword ptr [ebx + 0x428]
        pushfd
        pushad
    }
    DD_SAVE_XMM
    __asm {
        push edx
        push ecx
        push dword ptr [ebp + 8]
        push eax
        push ebx
        call dd_cover
        add esp, 20
    }
    DD_LOAD_XMM
    __asm {
        popad
        popfd
        jmp dword ptr [g_dd_cover_back]
    }
}

// 0x1EE5EA, once a frame before the draw lists fill
__declspec(naked) void lists_thunk()
{
    __asm {
        pushfd
        pushad
    }
    DD_SAVE_XMM
    __asm call dd_lists
    DD_LOAD_XMM
    __asm {
        popad
        popfd
        mov edx, dword ptr ds:[0x002D45F0]
        jmp dword ptr [g_dd_lists_back]
    }
}

} // namespace

void __cdecl dd_particle(uint32_t type, uint32_t camera)
{
    g_dd_particle = 1.0f;
    if (g_dd_reach <= 1.0f) return;
    const float *x = (const float *)g_dd_xmm;
    float scale = x[4], cut_now = x[8]; // xmm1, xmm2 (lane 0)
    float cut = std::min(*(const float *)(uintptr_t)(type + 0xC4) * scale, *(const float *)(uintptr_t)(camera + 0x88));
    if (cut > 0) g_dd_particle = cut_now / cut;
}

void __cdecl dd_cover(uint32_t cover, uint32_t count, uint32_t mesh, uint32_t ecx, uint32_t edx)
{
    constexpr uint32_t kPatches = 0x28, kCount = 63, kSize = 16;
    if (count < kCount || g_dd_reach <= 1.0f) return;
    const float *x = (const float *)g_dd_xmm;
    float v1 = x[4], dist = x[8]; // xmm1, xmm2
    uint32_t list = cover + kPatches, last = list;
    for (uint32_t at = list + kSize; at < list + kCount * kSize; at += kSize)
        if (*(const float *)(uintptr_t)(at + 4) > *(const float *)(uintptr_t)(last + 4)) last = at;
    if (!(dist < *(const float *)(uintptr_t)(last + 4))) return;
    *(uint32_t *)(uintptr_t)last = mesh;
    *(float *)(uintptr_t)(last + 4) = dist;
    *(float *)(uintptr_t)(last + 8) = v1;
    *(uint8_t *)(uintptr_t)(last + 0xC) = (uint8_t)ecx;
    *(uint8_t *)(uintptr_t)(last + 0xD) = edx == 2;
}

void __cdecl dd_lists()
{
    if (g_dd_reach <= 1.0f) return;
    static const uint32_t caps[4] = {4096, 0, 2048, 512};
    static uint8_t *room[4];
    uint32_t display = *(const uint32_t *)(uintptr_t)0x002D67F4;
    if (!display) return;
    uint32_t list = display + 0xCA6C;
    for (int i = 0; i < 4; i++, list += 0x18) {
        if (!caps[i]) continue;
        uint32_t &cap = *(uint32_t *)(uintptr_t)(list + 0xC);
        if (room[i] && *(uint32_t *)(uintptr_t)list == (uint32_t)(uintptr_t)room[i]) continue; // ours already
        if (cap >= caps[i]) continue; // the game's room is as large
        if (!room[i]) {
            room[i] = (uint8_t *)VirtualAlloc(nullptr, caps[i] * 28, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!room[i]) continue;
            logf("drawdist: draw list %d: room for %u meshes (the game's %u)", i, caps[i], cap);
        }
        *(uint32_t *)(uintptr_t)list = (uint32_t)(uintptr_t)room[i];
        if (*(uint32_t *)(uintptr_t)(list + 4)) *(uint32_t *)(uintptr_t)(list + 4) = (uint32_t)(uintptr_t)(room[i] + caps[i] * 20);
        cap = caps[i];
    }
}

void drawdist_set(int factor)
{
    float f = (float)std::clamp(factor, 1, 16);
    g_dd_reach2 = f * f;
    g_dd_reach = f;
}

void install_drawdist_patches()
{
    drawdist_set(g_cfg.draw_distance);
    for (const Site &s : kSites)
        if (!matches(s.at, s.replaced.data(), s.replaced.size())) {
            logf("drawdist: code at 0x%08x does not match this build; DRAW DISTANCE unavailable", s.at);
            return;
        }
    if (!matches(0x0016DFFB, kParticle, sizeof kParticle) || !matches(0x00160913, kCover, sizeof kCover) ||
        !matches(0x001EE5EA, kLists, sizeof kLists)) {
        logf("drawdist: particle / ground cover / draw list code does not match this build; DRAW DISTANCE unavailable");
        return;
    }
    g_code = (uint8_t *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_code) return;
    for (const Site &s : kSites) {
        std::vector<uint8_t> scale = {0xF3, 0x0F, (uint8_t)s.op, (uint8_t)s.reg};
        put32(scale, (uint32_t)(uintptr_t)(s.squared ? &g_dd_reach2 : &g_dd_reach));
        std::vector<uint8_t> b;
        if (!s.after) b.insert(b.end(), scale.begin(), scale.end());
        b.insert(b.end(), s.replaced.begin(), s.replaced.end());
        if (s.after) b.insert(b.end(), scale.begin(), scale.end());
        uint32_t back = s.at + (uint32_t)s.replaced.size();
        b.push_back(0xE9);
        put32(b, 0); // filled below, relative to where it lands
        uint8_t *stub = emit(b);
        int32_t rel = (int32_t)(back - ((uint32_t)(uintptr_t)stub + (uint32_t)b.size()));
        memcpy(stub + b.size() - 4, &rel, 4);
        jump(s.at, (uint32_t)(uintptr_t)stub, s.replaced.size());
    }
    jump(0x0016DFFB, (uint32_t)(uintptr_t)&particle_thunk, sizeof kParticle);
    jump(0x00160913, (uint32_t)(uintptr_t)&cover_thunk, sizeof kCover);
    jump(0x001EE5EA, (uint32_t)(uintptr_t)&lists_thunk, sizeof kLists);
    logf("drawdist: %zu distance compares hooked (draw distance %dx)", sizeof kSites / sizeof kSites[0] + 2, (int)g_dd_reach);
}
