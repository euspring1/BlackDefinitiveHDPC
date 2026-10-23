// Mid-function hooks into the game's code: a jump at a site to a stub that saves every register (general, flags,
// x87 / SSE), calls a C++ handler that may change them, restores them and runs the instructions the jump replaced.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

// The general registers as pushad left them; a handler may change any of them but esp.
struct HookRegs {
    uint32_t edi, esi, ebp, esp_, ebx, edx, ecx, eax;
    uint32_t eflags;
    uint32_t resume_; // the stub's own
    uint32_t esp() const { return esp_ + 8; } // the game's esp at the site
};

// The FXSAVE image: xmm[n] the SSE registers.
struct HookFx {
    uint8_t head[160];
    float xmm[8][4];
    uint8_t tail[224];
};

// true: run `run` (the replaced instructions, or what replaces them) and go on; false: run `run_false` instead.
using HookFn = bool(__cdecl *)(HookRegs *r, HookFx *fx);

struct HookSite {
    uint32_t at;                    // where the jump goes (at least 5 bytes of whole instructions)
    std::vector<uint8_t> expect;    // the bytes there now, checked and replaced; their length is the room
    HookFn fn;
    std::vector<uint8_t> run;       // run after a true return (empty: `expect`, which must be relocatable)
    uint32_t back = 0;              // where it goes on after them (0: at + room)
    std::vector<uint8_t> run_false; // after a false return (only used when `skip_false`)
    uint32_t back_false = 0;
    bool skip_false = false;
};

// Checks every site first: all are hooked, or none (false, the reason logged with `what`).
bool hook_install(const char *what, std::initializer_list<HookSite> sites);
