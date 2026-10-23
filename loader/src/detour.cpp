// Mid-function hooks (detour.h). A site's stub, in executable memory of the port's:
//
//   push <after>               the place to go on: the replaced instructions' copy, unless the handler says false
//   pushfd ; pushad            the game's registers: HookRegs
//   mov ebx, esp ; and esp, -16 ; sub esp, 512 ; fxsave [esp]   x87 / SSE: HookFx
//   fninit ; fldcw [0x27F] ; cld                                the C++ code's own FPU state
//   push esp ; push ebx ; call handler ; add esp, 8
//   test al, al ; jnz +7 ; mov [ebx+36], <after false>
//   fxrstor [esp] ; mov esp, ebx ; popad ; popfd ; ret
//   after:       the replaced instructions ; jmp back
//   after false: its instructions ; jmp back false
#include "detour.h"

#include "common.h"

#include <cstring>

namespace {

uint8_t *g_code;
size_t g_used, g_size;
const uint16_t kControlWord = 0x027F; // MSVC's: 53-bit precision, round to nearest, exceptions masked

void put32(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

uint8_t *room(size_t n)
{
    if (!g_code || g_used + n > g_size) {
        g_size = 64 * 1024;
        g_code = (uint8_t *)VirtualAlloc(nullptr, g_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_used = 0;
        if (!g_code) return nullptr;
    }
    uint8_t *p = g_code + g_used;
    g_used += (n + 15) & ~(size_t)15;
    return p;
}

void jump_rel(std::vector<uint8_t> &b, uint32_t base, uint32_t to)
{
    b.push_back(0xE9);
    put32(b, to - (base + (uint32_t)b.size() + 4));
}

} // namespace

bool hook_install(const char *what, std::initializer_list<HookSite> sites)
{
    for (const HookSite &s : sites) {
        if (s.expect.size() < 5 || memcmp((const void *)(uintptr_t)s.at, s.expect.data(), s.expect.size()) != 0) {
            logf("%s: code at 0x%08x does not match this build; not installed", what, s.at);
            return false;
        }
    }
    for (const HookSite &s : sites) {
        const std::vector<uint8_t> &run = s.run.empty() ? s.expect : s.run;
        uint32_t back = s.back ? s.back : s.at + (uint32_t)s.expect.size();
        uint8_t *stub = room(128 + run.size() + s.run_false.size());
        if (!stub) return false;
        uint32_t base = (uint32_t)(uintptr_t)stub;
        std::vector<uint8_t> b;
        b.push_back(0x68); // push <after>: filled below
        size_t after_at = b.size();
        put32(b, 0);
        b.insert(b.end(), {0x9C, 0x60, 0x8B, 0xDC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00, 0x0F, 0xAE, 0x04, 0x24,
                           0xDB, 0xE3, 0xD9, 0x2D});
        put32(b, (uint32_t)(uintptr_t)&kControlWord);
        b.insert(b.end(), {0xFC, 0x8B, 0xC4, 0x50, 0x53, 0xB8});
        put32(b, (uint32_t)(uintptr_t)s.fn);
        b.insert(b.end(), {0xFF, 0xD0, 0x83, 0xC4, 0x08, 0x84, 0xC0, 0x75, 0x07, 0xC7, 0x43, 0x24});
        size_t false_at = b.size();
        put32(b, 0);
        b.insert(b.end(), {0x0F, 0xAE, 0x0C, 0x24, 0x8B, 0xE3, 0x61, 0x9D, 0xC3});
        uint32_t after = base + (uint32_t)b.size();
        b.insert(b.end(), run.begin(), run.end());
        jump_rel(b, base, back);
        uint32_t after_false = after;
        if (s.skip_false) {
            after_false = base + (uint32_t)b.size();
            b.insert(b.end(), s.run_false.begin(), s.run_false.end());
            jump_rel(b, base, s.back_false ? s.back_false : back);
        }
        memcpy(&b[after_at], &after, 4);
        memcpy(&b[false_at], &after_false, 4);
        memcpy(stub, b.data(), b.size());
        auto *p = (uint8_t *)(uintptr_t)s.at;
        p[0] = 0xE9;
        int32_t rel = (int32_t)(base - (s.at + 5));
        memcpy(p + 1, &rel, 4);
        memset(p + 5, 0x90, s.expect.size() - 5);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}
