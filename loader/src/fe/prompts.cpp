// Prompts that follow the device in use (fe/prompts.h).
//
// The game's text: fe/pages.cpp builds every string in two forms, with the controller's button icons and with the
// bound keys, and writes the bank of the device in use at start. The game loads its bank (language\strings\MainUS.bin)
// once, and its load callback (0x1F8150) turns the bank's table of entry offsets into pointers before it registers the
// bank in the game object (*0x2D45D8 + 0xCE8): the hook after that (0x1F8189, eax the bank: +8 the count, +0xC the
// table) remembers it. Each string's two forms are entries of the port's ({key, text}, as the bank's); a switch of
// device points the table's entries at the other set. Text the game already took (an open page) changes the next
// time it is built.
//
// The menus' legend (Core.bin, fe/pages.cpp legend_keys): its three texts are bound to &PC_V19..21, set here.
#include "fe/prompts.h"

#include "common.h"
#include "detour.h"
#include "fe/vars.h"
#include "lang.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace prompts {
namespace {

constexpr uint32_t kBankMagic = 0x586A0308;

std::mutex g_lock;
std::vector<Text> g_text;
std::vector<uint32_t> g_pad_entries, g_key_entries; // each string's entry in both forms
uint32_t g_bank;                                    // the game's loaded bank, 0 before it is
std::atomic<bool> g_pad{false};
std::vector<std::wstring> g_legend_keys;

// {key, text NUL} entries in memory of the port's that is never freed (the game holds pointers into it). The controller
// icons in the text (U+F000 + an action's index in the binding table) are drawn by the game as the button that action
// has in the layout in use (the text code at 0x113C33: *0x2F3BD4[code] -> the button's icon by its name at 0x2AC290),
// so every layout, the player's own too, shows the right buttons.
std::vector<uint32_t> entries(bool pad)
{
    size_t size = 0;
    for (const Text &t : g_text) size += 4 + (((pad ? t.pad : t.keys).size() + 1 + 3) & ~(size_t)3);
    auto *mem = (uint8_t *)VirtualAlloc(nullptr, size ? size : 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::vector<uint32_t> out;
    if (!mem) return out;
    uint8_t *p = mem;
    for (const Text &t : g_text) {
        const std::string &s = pad ? t.pad : t.keys;
        out.push_back((uint32_t)(uintptr_t)p);
        memcpy(p, &t.key, 4);
        memcpy(p + 4, s.c_str(), s.size() + 1);
        p += 4 + ((s.size() + 1 + 3) & ~(size_t)3);
    }
    return out;
}

void apply_text()
{
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_bank) return;
    const std::vector<uint32_t> &e = g_pad ? g_pad_entries : g_key_entries;
    uint32_t n = *(const uint32_t *)(uintptr_t)(g_bank + 8);
    auto *table = (uint32_t *)(uintptr_t) * (const uint32_t *)(uintptr_t)(g_bank + 0xC);
    if (e.size() != n) return;
    for (uint32_t i = 0; i < n; i++) table[i] = e[i];
}

// a legend text for the controller: the button's icon where the key's text ended, the label staying where it is
std::wstring padded(const std::wstring &keys, const wchar_t *icon)
{
    float chars = 0;
    for (wchar_t c : keys) chars += c == L'W' || c == L'M' ? 2.0f : 1.0f;
    float icons = 2.6f * (float)wcslen(icon) + 0.5f; // an icon is about this many letters wide, and a little gap
    int spaces = chars > icons ? (int)((chars - icons) * 2.0f + 0.5f) : 0; // a space about half a letter
    return std::wstring((size_t)spaces, L' ') + icon;
}

void apply_legend()
{
    if (g_legend_keys.size() != 3) return;
    // the D-pad up and down, A and B as the game's menu text draws them (its controller screen: "PRESS ▲ :", "press ²")
    const wchar_t *icons[3] = {L"▲▼", L"²", L"¹"};
    for (int i = 0; i < 3; i++)
        fe_var_set(fevar::kLegend + i, g_pad ? padded(g_legend_keys[i], icons[i]) : g_legend_keys[i]);
}

void apply()
{
    apply_text();
    apply_legend();
}

// 0x1F8189: eax the bank, its table already pointers
bool __cdecl bank_loaded(HookRegs *r, HookFx *)
{
    uint32_t bank = r->eax;
    if (!bank || *(const uint32_t *)(uintptr_t)bank != kBankMagic) return true;
    std::lock_guard<std::mutex> g(g_lock);
    uint32_t n = *(const uint32_t *)(uintptr_t)(bank + 8);
    const uint32_t *table = (const uint32_t *)(uintptr_t) * (const uint32_t *)(uintptr_t)(bank + 0xC);
    if (n != g_text.size()) {
        logf("prompts: the game's text has %u strings, the port built %zu; prompts stay as loaded", n, g_text.size());
        return true;
    }
    for (uint32_t i = 0; i < n; i++)
        if (*(const uint32_t *)(uintptr_t)table[i] != g_text[i].key) {
            logf("prompts: string %u of the game's text is not the port's; prompts stay as loaded", i);
            return true;
        }
    g_pad_entries = entries(true);
    g_key_entries = entries(false);
    if (g_pad_entries.size() != n || g_key_entries.size() != n) return true;
    g_bank = bank;
    const std::vector<uint32_t> &e = g_pad ? g_pad_entries : g_key_entries; // the device in use now
    auto *t = (uint32_t *)(uintptr_t) * (const uint32_t *)(uintptr_t)(bank + 0xC);
    for (uint32_t i = 0; i < n; i++) t[i] = e[i];
    logf("prompts: the game's text (%u strings) follows the device in use", n);
    return true;
}

void set_pad(bool pad)
{
    if (g_pad.exchange(pad) == pad) return;
    g_cfg.input_pad = pad;
    apply();
    logf("prompts: %s", pad ? "the controller's buttons" : "the keys");
}

} // namespace

void set_text(std::vector<Text> text)
{
    std::lock_guard<std::mutex> g(g_lock);
    g_text = std::move(text);
}

const std::vector<std::wstring> &legend_keys()
{
    if (g_legend_keys.empty())
        g_legend_keys = {widen(lang::tr("[ARROWS]")), widen(lang::font_text("[ENTER]")), widen(lang::font_text("[BACKSPACE]"))};
    return g_legend_keys;
}

void install()
{
    g_pad = g_cfg.input_mode == 2 || (g_cfg.input_mode == 0 && g_cfg.input_pad);
    g_cfg.input_pad = g_pad;
    legend_keys();
    apply_legend();
    hook_install("prompts", {{0x001F8189, {0x8B, 0x15, 0xD8, 0x45, 0x2D, 0x00}, bank_loaded}}); // mov edx, [0x2D45D8]
}

bool pad() { return g_pad; }

void set_mode(int mode)
{
    g_cfg.input_mode = mode;
    if (mode == 1) set_pad(false);
    else if (mode == 2) set_pad(true);
}

void note_input(bool pad, bool keyboard_mouse)
{
    if (g_cfg.input_mode != 0) return;
    if (pad && !g_pad) set_pad(true);
    else if (keyboard_mouse && !pad && g_pad) set_pad(false);
}

} // namespace prompts
