// RELOAD BLUR (PC SETTINGS > MODS). The weapon animation step FUN_0017B130 (ebp = the weapon state) asks the
// post-process controller at [0x2D1990]+0x830A0 for its depth blur when a reload animation starts: names ending in
// "101"/"501" set mode 1 (+0x34) and the focus values directly (0x17B351), "180" through FUN_001571C0 and "182" holds
// it through FUN_00159480. All of them sit behind the test of the state's +0xB6 flag at 0x17B239; with the setting
// off that test is taken as false, so the reload never starts the blur. Cutscenes start the same controller
// elsewhere (FUN_00202850) and keep their depth of field.
#include "effects.h"

#include "common.h"

#include <cstring>

namespace {

constexpr uint32_t kSite = 0x0017B239; // mov al, [ebp+0xB6]; test al, al; jz 0x17B3AA
const uint8_t kExpect[] = {0x8A, 0x85, 0xB6, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x0F, 0x84, 0x63, 0x01, 0x00, 0x00};

} // namespace

extern "C" {
uint32_t g_reload_blur_on = 0x0017B247;   // the reload blur requests
uint32_t g_reload_blur_done = 0x0017B3AA; // the function's exit (pops and returns)
int __cdecl reload_blur_wanted(const uint8_t *weapon);
}

// ecx and edx are not live at the site: both paths load them again before use.
int __cdecl reload_blur_wanted(const uint8_t *weapon) { return g_cfg.reload_blur && weapon[0xB6] != 0; }

namespace {

__declspec(naked) void reload_blur_thunk()
{
    __asm {
        push ebp
        call reload_blur_wanted
        add esp, 4
        test eax, eax
        jz skip
        jmp dword ptr [g_reload_blur_on]
    skip:
        jmp dword ptr [g_reload_blur_done]
    }
}

} // namespace

void install_effects_patches()
{
    uint8_t *site = (uint8_t *)(uintptr_t)kSite;
    if (memcmp(site, kExpect, sizeof kExpect)) {
        logf("effects: reload blur site 0x%08x does not match this build; RELOAD BLUR unavailable", kSite);
        return;
    }
    site[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)&reload_blur_thunk - (kSite + 5));
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, sizeof kExpect - 5);
    logf("effects: reload blur requests at 0x%08x hooked (reload blur %s)", kSite, g_cfg.reload_blur ? "on" : "off");
}
