// CAMERA SHAKE (PC SETTINGS > MODS). Explosions and other jolts in play add one of the renderer's effect cameras
// ([0x2D12B8]+0x200[9..13], vtable 0x28C1A0) to the active set; its update FUN_00202080 (vtable +0x10, thiscall,
// ecx = the effect) shakes the view: +4 time so far, +8 duration, +0xC strength, +0x10 kind. Kinds 1-5 rotate the
// camera by noise (FUN_001FA560) times the strength and widen the field of view by it; a rotation of exactly 0 is
// skipped. With the setting off the strength of those kinds is set to 0 before the game's update runs, so the view
// stays as the player camera left it; kinds 6-8 and the cutscene shake (FUN_00200850) are left alone.
#include "shake.h"

#include "common.h"

#include <cstring>

extern "C" {
uint32_t g_shake_orig;
void __cdecl shake_filter(uint8_t *fx);
}

void __cdecl shake_filter(uint8_t *fx)
{
    if (g_cfg.camera_shake) return;
    int kind = *(const int *)(fx + 0x10);
    if (kind >= 1 && kind <= 5) *(float *)(fx + 0xC) = 0.0f;
}

namespace {

constexpr uint32_t kSlot = 0x0028C1B0; // vtable 0x28C1A0 + 0x10
constexpr uint32_t kUpdate = 0x00202080;

__declspec(naked) void shake_thunk()
{
    __asm {
        pushad
        push ecx
        call shake_filter
        add esp, 4
        popad
        jmp dword ptr [g_shake_orig]
    }
}

} // namespace

void install_shake_patch()
{
    uint32_t *slot = (uint32_t *)(uintptr_t)kSlot;
    if (*slot != kUpdate) {
        logf("shake: the effect camera's update is not at 0x%08x in this build; CAMERA SHAKE unavailable", kUpdate);
        return;
    }
    g_shake_orig = kUpdate;
    *slot = (uint32_t)(uintptr_t)&shake_thunk;
    logf("shake: effect camera update FUN_00202080 hooked (camera shake %s)", g_cfg.camera_shake ? "on" : "off");
}
