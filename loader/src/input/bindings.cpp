// Keyboard and mouse bound to the game's own actions.
//
// The game reads every player action through its binding table (*0x2F3BD4: 37 dwords, action -> controller
// channel; FUN_001F1520 fills it from one of the presets at 0x2A7AF0, 0x94 bytes each: default, assault, scout,
// southpaw, custom; the custom-controls screen edits single entries). The channels are made from the pad by the
// controller update FUN_000C5A80 (channel object: floats at +0x44, current/previous digital state at +0x25/+0x09):
//   0-3 D-pad up/down/left/right, 4/5 left stick up/down, 6/7 left/right, 8/9 right stick up/down, 10/11 left/right,
//   12 START, 13 BACK, 14/15 left/right stick click, 16-23 A B X Y BLACK WHITE, left trigger, right trigger,
//   24-27 the four stick axes signed; 28 = unbound.
// A bound key looks up its action's channel in the live table and presses the pad input that feeds that channel,
// so the keys mean the same action whatever controller layout is chosen in the game, and controllers keep working.
//
// Menus, pauses and cutscenes (no player update, see in_play) take no bound keys and nothing goes through the pad:
// arrows, Enter and Backspace are the menus' own inputs (up, down, left, right, select, back; held arrows repeat),
// which fe/calls.cpp gives the menus directly. The mouse (its Windows cursor, free while a menu page is up) drives
// them the same way: the wheel and vertical movement walk the highlight, the left button selects, the right one goes
// back. Outside the menus the game itself
// waits for the pad (PRESS A TO CONTINUE after a hint, START to skip the mission's intro text): there Enter is A or
// START (see apply). A key held across a switch between play and menus is ignored until it is released (a press that
// starts play must not also act in it).
#include "input/bindings.h"

#include "common.h"
#include "d3d/host.h"
#include "fe/calls.h"
#include "fe/pause_ui.h"
#include "fe/pc_settings.h"
#include "fe/vars.h"
#include "input/movies.h"
#include "input/mouselook.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <cmath>
#include <cstring>

namespace bindings {
namespace {

// Names as on the game's controls screen (FE_ACTION_* strings; the tables built at 0x214820 pair them with the
// binding-table index). The movement axes are split into their stick halves.
const Action kActions[] = {
    {"run_forward", L"Run forward", 1, {'W', 0}},
    {"run_back", L"Run back", 2, {'S', 0}},
    {"step_left", L"Sidestep left", 3, {'A', 0}},
    {"step_right", L"Sidestep right", 4, {'D', 0}},
    {"fire", L"Fire", 9, {VK_LBUTTON, 0}},
    {"aim", L"Aim", 16, {VK_RBUTTON, 0}},
    {"reload", L"Reload", 11, {'R', 0}},
    {"use", L"Action (use, pick up)", 13, {'E', 0}},
    {"crouch", L"Crouch", 12, {'C', 0}},
    {"grenade", L"Grenade", 27, {'G', 0}},
    {"melee", L"Melee", 32, {'F', 0}},
    {"next_weapon", L"Next weapon", 14, {KEY_WHEEL_DOWN, 0}},
    {"prev_weapon", L"Previous weapon", 15, {KEY_WHEEL_UP, 0}},
    {"firing_mode", L"Change firing mode", 30, {'Z', 0}},
    {"suppressor", L"Toggle suppressor", 31, {'Q', 0}},
    {"health_pack", L"Health pack", 33, {'X', 0}},
    {"music", L"Next music track", 34, {0, 0}},
    {"turn_left", L"Turn left", 5, {0, 0}},
    {"turn_right", L"Turn right", 6, {0, 0}},
    {"look_up", L"Look up", 7, {0, 0}},
    {"look_down", L"Look down", 8, {0, 0}},
    {"pause", L"Pause", 25, {VK_ESCAPE, 0}},
    {"objectives", L"Show objectives", 36, {VK_TAB, 0}},
    // The port's own: take the first / second of the two weapons the player carries (presses "next weapon" when
    // the other one is in hand; see weapon_slot_press).
    {"weapon_1", L"Weapon 1", 14, {'1', 0}},
    {"weapon_2", L"Weapon 2", 14, {'2', 0}},
    // Walk: while held, the movement keys push the left stick only part of the way (walk_speed); the game's speed
    // follows the stick's deflection. No game action of its own (-1).
    {"walk", L"Walk (hold)", -1, {VK_LSHIFT, 0}},
};
constexpr int kCount = sizeof kActions / sizeof kActions[0];
constexpr int kAim = 5;
constexpr int kPause = 21;
constexpr int kWeapon1 = 23, kWeapon2 = 24, kWalk = 25;

uint16_t g_keys[kCount][kSlots];
bool g_keys_set; // defaults applied

void ensure_defaults()
{
    if (g_keys_set) return;
    g_keys_set = true;
    for (int i = 0; i < kCount; i++)
        for (int s = 0; s < kSlots; s++) g_keys[i][s] = kActions[i].defaults[s];
}

struct Named {
    uint16_t key;
    const char *name;
};
const Named kNames[] = {
    {VK_LBUTTON, "Mouse1"}, {VK_RBUTTON, "Mouse2"}, {VK_MBUTTON, "Mouse3"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {KEY_WHEEL_UP, "WheelUp"}, {KEY_WHEEL_DOWN, "WheelDown"}, {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Esc"},
    {VK_TAB, "Tab"}, {VK_BACK, "Backspace"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"},
    {VK_LSHIFT, "LShift"}, {VK_RSHIFT, "RShift"}, {VK_LCONTROL, "LCtrl"}, {VK_RCONTROL, "RCtrl"}, {VK_LMENU, "LAlt"},
    {VK_RMENU, "RAlt"}, {VK_CAPITAL, "CapsLock"}, {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"},
    {VK_NEXT, "PageDown"}, {VK_MULTIPLY, "Num*"}, {VK_ADD, "Num+"}, {VK_SUBTRACT, "Num-"}, {VK_DECIMAL, "Num."},
    {VK_DIVIDE, "Num/"}, {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"}, {VK_NUMLOCK, "NumLock"}, {VK_APPS, "Menu"},
};

// black.ini spelling: the names above, letters and digits as themselves, F1-F24, Num0-Num9, others as 0xNN.
std::string ini_name(uint16_t k)
{
    if (!k) return "none";
    for (const Named &n : kNames)
        if (n.key == k) return n.name;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return std::string(1, (char)k);
    if (k >= VK_F1 && k <= VK_F24) return "F" + std::to_string(k - VK_F1 + 1);
    if (k >= VK_NUMPAD0 && k <= VK_NUMPAD9) return "Num" + std::to_string(k - VK_NUMPAD0);
    char buf[8];
    snprintf(buf, sizeof buf, "0x%02X", k);
    return buf;
}

uint16_t parse_key(const std::string &s)
{
    if (s.empty() || _stricmp(s.c_str(), "none") == 0) return 0;
    for (const Named &n : kNames)
        if (_stricmp(n.name, s.c_str()) == 0) return n.key;
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return (uint16_t)toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && s.size() <= 3 && isdigit((unsigned char)s[1])) {
        int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return (uint16_t)(VK_F1 + n - 1);
    }
    if (_strnicmp(s.c_str(), "Num", 3) == 0 && s.size() == 4 && isdigit((unsigned char)s[3])) return (uint16_t)(VK_NUMPAD0 + s[3] - '0');
    if (_strnicmp(s.c_str(), "0x", 2) == 0) return (uint16_t)strtoul(s.c_str(), nullptr, 16);
    return 0;
}

// ---- per-poll state (game thread) ----

std::bitset<0x102> g_stale;      // held across a play/menu switch: ignored until released
std::bitset<0x102> g_script;     // --press k:Name, this poll only
bool g_was_play;
bool g_freed_by_esc; // the pause key (or Esc in a menu) let go of the mouse: take it again when play starts
bool g_release_was_down;

// Menu keys -> the menus' input ids (FEInputId); held arrows repeat like the game's own pad input.
struct MenuKey {
    uint16_t vk;
    int input;
    bool repeats;
};
const MenuKey kMenuKeys[] = {{VK_UP, 1, true}, {VK_DOWN, 2, true}, {VK_LEFT, 3, true}, {VK_RIGHT, 4, true},
                             {VK_RETURN, 5, false}, {VK_BACK, 6, false}};
constexpr int kMenuKeyCount = sizeof kMenuKeys / sizeof kMenuKeys[0];
constexpr ULONGLONG kRepeatDelay = 400, kRepeatEvery = 120; // ms
bool g_menu_down[kMenuKeyCount];
int g_repeat = -1; // the held arrow that repeats
ULONGLONG g_repeat_at;
std::atomic<int> g_menu_input;            // pressed at the last poll, until the menus take it
std::atomic<uint32_t> g_menu_input_frame; // when
bool g_menu_select;                       // Enter held (movies: skip)

// A hint that stops the game (PRESS A TO CONTINUE: the HUD's hint code at 0x131C47 sets game object (*0x2D45F0) +
// 0x21098 = 3 for the hint ids at 0x28AA80) is closed by the game on A or START of the player's controller (its channel
// block: A pressed +0x35, START +0x31; checks at 0x129B10 and around), not through the menus' input.
bool hint_stopped()
{
    uint32_t game = *(const uint32_t *)(uintptr_t)0x002D45F0;
    return game && *(const int32_t *)(uintptr_t)(game + 0x21098) == 3;
}

// No menu page is up: the front end's current page name (FE object *0x2D199C + 0x3840, as FUN_00118A80 compares it with
// "PauseMenu") is empty. So it is while a mission's intro text types out (the location and time before play), which the
// game skips on START of the player's controller.
bool no_menu_page()
{
    uint32_t fe = *(const uint32_t *)(uintptr_t)0x002D199C;
    return fe && *(const char *)(uintptr_t)(fe + 0x3840) == 0;
}

// The front end's current page name ("" when none is up), the same field no_menu_page reads.
const char *fe_page()
{
    uint32_t fe = *(const uint32_t *)(uintptr_t)0x002D199C;
    return fe ? (const char *)(uintptr_t)(fe + 0x3840) : "";
}

// The pages the port builds (fe/pages.cpp) follow the pointer themselves: they read externObject.FEPointerX /
// FEPointerY and put the focus on the row under it, so the highlight and the cursor never drift apart. The game's own
// pages the port gives rows to do the same: the main menu, Options, the PC SETTINGS menu and the pause menu carry a
// list of the game's own buttons (fe/pages.cpp heal_update), every other one the page walk (game_page: the page's own
// menu ring gives the rows). The names are the front end's own (fe_page, the port logs them at "[input] menu page").
bool pointer_page_up()
{
    static const char *kPages[] = {"PCSettings", "PCVideo", "PCDisplay", "PCGame", "PCControls", "PCMods", "PCBindings",
                                   "PCPageIG", "OptionsMenuXbox", "blacktitlemenu", "blacktitlemenu2", "PauseMenu",
                                   "ControllerSetup", "ControllerCustomXbox", "SoundSetupXbox", "PictureSetupXbox",
                                   "ProfileOptions", "OperatorProfile", "Difficulty",
                                   "VirtualKeyboard", "CreateBlackProfile",
                                   "MemCardMessage", "MemCardSlots", "MissionModeSelect", "MissionModeSelect2",
                                   "Unlocks", "UnlockWeapons", "ResultsScreen", "ResultsEasy", "ChooseDifficulty"};
    const char *name = fe_page();
    for (const char *p : kPages)
        if (_stricmp(name, p) == 0) return true;
    return false;
}

// The row the pointer is on (set by fe/calls.cpp from the page's PCHover): whether it carries a value list, the
// setting it is (-1: none) and the row's middle in the picture's pixels. The side of a row a click lands on is read
// from the pointer's place against that middle when the button goes down (take_menu_input), never stored: the page
// reports a row once and the pointer may well move along it before the click.
static bool s_row_value;
static int s_row_on = -1, s_row_id = -1, s_row_mid = -1, s_row_w = 0;
static int g_game_value_seq; // how many times the front end has reported one of the game's settings (game_value_call)
static int g_game_value;     // the value it reported last: a couple of steps for a list, 0-100 for a slider

bool raw_held(uint16_t k, bool focus, int mouse)
{
    if (k < g_script.size() && g_script[k]) return true;
    switch (k) {
    case VK_LBUTTON: return mouse & 1;
    case VK_RBUTTON: return mouse & 2;
    case VK_MBUTTON: return mouse & 4;
    case VK_XBUTTON1: return mouse & 8;
    case VK_XBUTTON2: return mouse & 16;
    case KEY_WHEEL_UP:
    case KEY_WHEEL_DOWN: return false; // handled per notch
    }
    return focus && k < 0x100 && (GetAsyncKeyState(k) & 0x8000);
}

// Each wheel notch is one press of the key bound to that direction: held 2 polls, then 2 released so quick notches
// stay separate presses.
uint16_t wheel_key(int wheel)
{
    static int pending, hold, gap; // pending: notches, + = up, - = down
    static uint16_t held;
    pending = std::clamp(pending + wheel, -4, 4);
    if (hold) {
        hold--;
        if (!hold) gap = 2;
        return held;
    }
    if (gap) {
        gap--;
        return 0;
    }
    if (!pending) return 0;
    held = pending > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    pending += pending > 0 ? -1 : 1;
    hold = 1;
    return held;
}

// The HUD's button prompts ("Press %s to pick up", "Press %s to swap": FUN_001314E0 at 0x1328EC) take the button of
// an action from the ten assignable buttons (channels at 0x2A7DD4) and print the one-character string at
// 0x2AB72C + 4 x button, which the game's font draws as that button (index 0 Y "\xB0", 1 A "\xB2", 2 B "\xB9",
// 3 X "\xB3"; the others have none). Those four pointers are pointed at "[KEY]" strings: the key bound to the first
// action on that button in the current controller layout. Kept up to date at every poll.
void update_hud_buttons(const int32_t *table)
{
    static const uint32_t kChannels = 0x002A7DD4, kStrings = 0x002AB72C;
    static char text[4][32];
    static uint32_t original[4];
    static bool ready;
    if (!ready) {
        DWORD old;
        if (!VirtualProtect((void *)(uintptr_t)kStrings, 16, PAGE_READWRITE, &old)) return;
        for (int b = 0; b < 4; b++) original[b] = *(const uint32_t *)(uintptr_t)(kStrings + 4 * b);
        ready = true;
    }
    for (int b = 0; b < 4; b++) {
        int32_t ch = *(const int32_t *)(uintptr_t)(kChannels + 4 * b);
        std::string key;
        for (int i = 0; i < kCount && key.empty(); i++)
            if (kActions[i].game >= 0 && i != kWeapon1 && i != kWeapon2 && table[kActions[i].game] == ch) key = hud_action_key(i);
        uint32_t *slot = (uint32_t *)(uintptr_t)(kStrings + 4 * b);
        if (key.empty()) {
            *slot = original[b];
            continue;
        }
        snprintf(text[b], sizeof text[b], "[%s]", key.c_str());
        *slot = (uint32_t)(uintptr_t)text[b];
    }
}

const int32_t *game_table()
{
    if (!mouselook_installed()) return nullptr; // not the build these addresses are for
    const int32_t *t = *(const int32_t *const *)(uintptr_t)0x002F3BD4;
    return t ? t : (const int32_t *)(uintptr_t)0x002A7AF0; // before the game sets it up: its default preset
}

struct Pad {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int axis[4] = {}; // left x, left y, right x, right y: sum of +1 / -1 pushes
};

void press_channel(int ch, Pad &p)
{
    static const int8_t kAxis[8][2] = {{1, 1}, {1, -1}, {0, -1}, {0, 1}, {3, 1}, {3, -1}, {2, -1}, {2, 1}}; // channels 4-11
    if (ch < 0) return;
    if (ch < 4) p.buttons |= (uint16_t)(1 << ch);
    else if (ch < 12) p.axis[kAxis[ch - 4][0]] += kAxis[ch - 4][1];
    else if (ch < 16) p.buttons |= (uint16_t)(0x10 << (ch - 12));
    else if (ch < 24) p.analog[ch - 16] = 255;
}

// Hold to aim: the game's aim action toggles the weapon zoom on each press. With aim_hold the key's state is the
// wanted aim, and a short press is made whenever the game's aim flag (mouselook_aiming) differs from it; then a
// pause lets the flag follow before the next comparison. Returns whether the aim channel is pressed this poll.
bool aim_hold_press(bool play, bool want)
{
    constexpr int kPressPolls = 2, kSettlePolls = 10;
    static int pressing, settling;
    if (!play) {
        pressing = settling = 0;
        return false;
    }
    if (pressing) {
        pressing--;
        return true;
    }
    if (settling) {
        settling--;
        return false;
    }
    int aiming = mouselook_aiming();
    if (aiming < 0 || want == (aiming != 0)) return false;
    pressing = kPressPolls - 1;
    settling = kSettlePolls;
    return true;
}

// Weapon 1 / 2: the player carries two weapons and the game's "next weapon" swaps to the other (FUN_001BAB10). A
// press of a weapon key asks for that slot; "next weapon" is pressed while the other slot is in hand and no swap
// runs, for at most ~2 s. pressed_slot: 0 / 1 for a weapon key going down this poll, -1 none.
bool weapon_slot_press(bool play, int pressed_slot)
{
    constexpr int kPressPolls = 2, kSettlePolls = 10, kGiveUpPolls = 240;
    static int target = -1, pressing, settling, left;
    if (!play) {
        target = -1;
        pressing = settling = 0;
        return false;
    }
    if (pressed_slot >= 0) {
        target = pressed_slot;
        left = kGiveUpPolls;
    }
    if (pressing) {
        pressing--;
        return true;
    }
    if (settling) settling--;
    if (target < 0) return false;
    WeaponState w = mouselook_weapon();
    if (--left <= 0 || w.slot < 0 || w.slot == target || !w.other) { // done, unknown, or nothing to swap to
        target = -1;
        return false;
    }
    if (settling || w.swapping) return false;
    pressing = kPressPolls - 1;
    settling = kSettlePolls;
    return true;
}

// A slider's position (0-100) for the pointer's place along its row: the middle and the width the page reported. -1
// when the width is unknown (a report without one): the caller then steps the slider instead.
int slider_from_pointer(int mid, int width, float x)
{
    if (width <= 0) return -1;
    float left = mid - width * 0.5f;
    float f = (x - left) / (float)width;
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    return (int)(f * 100.0f + 0.5f);
}

} // namespace

// The page that follows the pointer reports the row it just focused (s_row_on / s_row_value / s_row_id above) and the
// row's middle in the picture's pixels (s_row_mid): the port reads the pointer's place against the middle when the
// button goes down (below), so the side of a row cannot go stale while the pointer stays on it.
void menu_row(int row, bool value, int id, int mid, int width)
{
    s_row_on = row;
    s_row_value = value;
    s_row_id = id;
    s_row_mid = mid;
    s_row_w = width;
}

// The front end's own pages (fe/calls.cpp fe_dispatch) named one of the game's settings and the value the widget
// handed it: a list of choices reports the one it moved to (a couple of steps), a slider the place on its bar (0-100).
void game_value_call(int value)
{
    g_game_value_seq++;
    g_game_value = value;
}

const Action *actions(int *count)
{
    *count = kCount;
    return kActions;
}

uint16_t key(int action, int slot)
{
    ensure_defaults();
    return g_keys[action][slot];
}

void set_key(int action, int slot, uint16_t k)
{
    ensure_defaults();
    if (!fixed(action, slot)) g_keys[action][slot] = k;
}

void reset_defaults()
{
    g_keys_set = false;
    ensure_defaults();
}

bool fixed(int action, int slot) { return action == kPause && slot == 0; }

std::wstring key_name(uint16_t k)
{
    if (!k) return L"";
    switch (k) {
    case VK_LBUTTON: return L"Left mouse button";
    case VK_RBUTTON: return L"Right mouse button";
    case VK_MBUTTON: return L"Middle mouse button";
    case VK_XBUTTON1: return L"Mouse button 4";
    case VK_XBUTTON2: return L"Mouse button 5";
    case KEY_WHEEL_UP: return L"Mouse wheel up";
    case KEY_WHEEL_DOWN: return L"Mouse wheel down";
    case VK_LSHIFT: return L"Left Shift";
    case VK_RSHIFT: return L"Right Shift";
    case VK_LCONTROL: return L"Left Ctrl";
    case VK_RCONTROL: return L"Right Ctrl";
    case VK_LMENU: return L"Left Alt";
    case VK_RMENU: return L"Right Alt";
    }
    bool oem = (k >= 0xBA && k <= 0xC0) || (k >= 0xDB && k <= 0xDF) || k == 0xE2;
    if (oem) { // the character on the key in the current layout
        wchar_t buf[32];
        UINT sc = MapVirtualKeyW(k, MAPVK_VK_TO_VSC);
        if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 32) > 0) return buf;
    }
    return widen(ini_name(k));
}

std::string hud_key_name(uint16_t k)
{
    switch (k) {
    case 0: return "";
    case VK_LBUTTON: return "MOUSE ESQ";
    case VK_RBUTTON: return "MOUSE DIR";
    case VK_MBUTTON: return "MOUSE MEIO";
    case VK_XBUTTON1: return "MOUSE 4";
    case VK_XBUTTON2: return "MOUSE 5";
    case KEY_WHEEL_UP: return "RODA CIMA";
    case KEY_WHEEL_DOWN: return "RODA BAIXO";
    case VK_SPACE: return "ESPACO";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "SHIFT";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "CTRL";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "ALT";
    case VK_UP: return "CIMA";
    case VK_DOWN: return "BAIXO";
    case VK_LEFT: return "ESQUERDA";
    case VK_RIGHT: return "DIREITA";
    }
    std::string s = ini_name(k);
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string hud_action_key(int action)
{
    ensure_defaults();
    if (action < 0 || action >= kCount) return "";
    for (int s = 0; s < kSlots; s++)
        if (g_keys[action][s]) return hud_key_name(g_keys[action][s]);
    return "";
}

bool load_ini(const char *k, const char *value)
{
    if (strncmp(k, "key_", 4) != 0) return false;
    ensure_defaults();
    for (int i = 0; i < kCount; i++) {
        if (strcmp(k + 4, kActions[i].ini) != 0) continue;
        std::string v = value;
        size_t comma = v.find(',');
        for (int s = 0; s < kSlots; s++) {
            std::string part = s == 0 ? v.substr(0, comma) : comma == std::string::npos ? "" : v.substr(comma + 1);
            set_key(i, s, parse_key(part));
        }
        return true;
    }
    return true; // a key_ line for an action this build does not have
}

void save_ini(FILE *f)
{
    ensure_defaults();
    for (int i = 0; i < kCount; i++) {
        std::string v = ini_name(g_keys[i][0]);
        if (g_keys[i][1]) v += "," + ini_name(g_keys[i][1]);
        fprintf(f, "key_%s = %s\n", kActions[i].ini, v.c_str());
    }
}

void script_hold(const std::string &name)
{
    uint16_t k = parse_key(name);
    if (k && k < g_script.size()) g_script[k] = true;
}

int take_menu_input()
{
    int v = g_menu_input.exchange(0);
    return v && g_frame_count - g_menu_input_frame <= 2 ? v : 0;
}

bool menu_select_held() { return g_menu_select; }

bool in_play()
{
    uint32_t f = mouselook_play_frame();
    return f && g_frame_count - f <= 3;
}

void apply(uint16_t &buttons, uint8_t analog[8], int16_t &lx, int16_t &ly, int16_t &rx, int16_t &ry, bool focus,
           bool mouse_live, int wheel)
{
    ensure_defaults();
    bool play = in_play();
    int mouse = mouse_live ? host3d::mouse_buttons() : 0;
    uint16_t wkey = wheel_key(mouse_live ? wheel : 0);

    if (play != g_was_play) { // keys held now belong to the other mode: ignore them until released
        logf("[input] %s (frame %u)", play ? "in play: keys drive the game's actions" : "menu, pause or cutscene: "
             "arrows / Enter / Backspace drive the menus", g_frame_count.load());
        for (int i = 0; i < kCount; i++)
            for (int s = 0; s < kSlots; s++)
                if (raw_held(g_keys[i][s], focus, mouse)) g_stale[g_keys[i][s]] = true;
        for (uint16_t k : {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_ESCAPE})
            if (raw_held(k, focus, 0)) g_stale[k] = true;
        if (play && g_freed_by_esc && focus) host3d::request_capture(true);
        if (play) g_freed_by_esc = false;
        g_was_play = play;
    }
    auto held = [&](uint16_t k) {
        if (!k) return false;
        if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) return k == wkey || g_script[k];
        if (!raw_held(k, focus, mouse)) {
            g_stale[k] = false;
            return false;
        }
        return !g_stale[k];
    };

    Pad p;
    const int32_t *table = game_table();
    if (table) update_hud_buttons(table);
    bool pause_down = false;
    static bool weapon_was[2];
    int weapon_pressed = -1;
    bool walk = false;
    for (int i = 0; play && i < kCount && table; i++) {
        bool down = false;
        for (int s = 0; s < kSlots; s++) down |= held(g_keys[i][s]);
        if (i == kWalk) {
            walk = down;
            continue;
        }
        if (i == kAim && g_cfg.aim_hold) down = aim_hold_press(play, down);
        if (i == kWeapon1 || i == kWeapon2) { // edges only; the swap itself is pressed below
            int slot = i - kWeapon1;
            if (down && !weapon_was[slot]) weapon_pressed = slot;
            weapon_was[slot] = down;
            continue;
        }
        if (!down) continue;
        if (i == kPause) pause_down = true;
        int ch = table[kActions[i].game];
        if (ch < 0 || ch >= 24) {
            static std::bitset<kCount> told;
            if (!told[i]) logf("[input] %ls: the game's controller layout has no button for this action", kActions[i].name);
            told[i] = true;
            continue;
        }
        press_channel(ch, p);
    }
    if (weapon_slot_press(play && table, weapon_pressed)) press_channel(table[kActions[kWeapon1].game], p);
    if (!play) aim_hold_press(false, false);
    int input = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kMenuKeyCount; i++) {
        bool h = !play && held(kMenuKeys[i].vk);
        if (h && !g_menu_down[i]) {
            input = kMenuKeys[i].input;
            g_repeat = kMenuKeys[i].repeats ? i : -1;
            g_repeat_at = now + kRepeatDelay;
        }
        g_menu_down[i] = h;
    }
    if (g_repeat >= 0 && !g_menu_down[g_repeat]) g_repeat = -1;
    if (!input && g_repeat >= 0 && now >= g_repeat_at) {
        input = kMenuKeys[g_repeat].input;
        g_repeat_at = now + kRepeatEvery;
    }
    // The wheel walks the highlight like the up/down arrows do (the player asked for it): one notch is one step, up
    // away from the user. It is a key-like input, so it hands the highlight to the keyboard (pointer_drives below) and
    // not to the pointer. One step per poll: the front end takes one input per update anyway, and the notches that
    // arrive together are dropped rather than queued up behind it.
    if (!play && !input) {
        int notches = host3d::menu_wheel();
        if (notches) input = notches > 0 ? 1 : 2;
    }
    // The mouse in the menus (the cursor is free while a menu page is up: see the capture below).
    //
    // The mouse never walks the highlight: the cursor is a pointer, not an arrow key. The port's own pages
    // (pointer_page_up) read the pointer themselves: the port feeds each the pointer's place (externObject.FEPointerX /
    // FEPointerY, the picture's pixels, -1 while the pointer is not the input used last) and the page puts the focus on
    // the row under the cursor, so the highlight follows the cursor and never drifts; the cursor anywhere else leaves
    // the focus where it was, and only the keyboard moves it. The left button confirms the row under the cursor (what
    // Enter does) and, on a row that carries a value, takes one step of that value (the row the page last reported:
    // fe/calls.cpp pc_hover); the right one goes back (what Backspace does). The pointer drives while it is the input
    // used last: a menu key hands the highlight to the keyboard until the pointer is moved again.
    static bool mouse_left_was, mouse_right_was, pointer_drives;
    static int click_mode; // a click waiting for the page to report the row under the cursor: 1 select, 2 the pause page
    static float mouse_x = -1.0f, mouse_y = -1.0f;
    // The front end's pages do not live in the picture's pixels: the game lays every page out on its own 640x480 stage
    // drawn at 80% and centred, so the page's x=0 sits at the picture's x=64 and 640 page units span 512 of the
    // picture's. A page compares the pointer with the places of its own clips (both the port's pages and the walk on
    // the game's), so the pointer must be handed to them in PAGE units: in picture pixels every page was right at the
    // centre and drifted away from it, which put the highlight a little left of the cursor, made the slider knob not
    // sit under the pointer away from the centre, and made the profile keyboard pick the key beside the one clicked.
    // The pause pages are the exception: they come from InGameOn/PseMenu and lay their rows out on the picture itself
    // (their clips' places are picture pixels), so they take the pointer as it is.
    auto pause_space = [] {
        const char *n = fe_page();
        return !_stricmp(n, "PauseMenu") || !_stricmp(n, "PCPageIG") || !_stricmp(n, "PauseMenuOptionXbox") ||
               !_stricmp(n, "ConfirmWarning") || !_stricmp(n, "PauseMenuObjectives") || !_stricmp(n, "objectives");
    };
    auto page_of_x = [&](float x) { return pause_space() ? x : (x - 64.0f) * 1.25f; };
    auto page_of_y = [&](float y) { return pause_space() ? y : (y - 48.0f) * 1.25f; };
    static char s_page[40];
    static int s_page_gen; // bumped at every page change; the pages read it (externObject.FEPCPage) and report again
    // A left button held on a row that carries a value steps it again and again (a slider sweeps instead of moving
    // once): hold_kind 1 the port's own setting, 2 one of the game's own rows, 3 a row of the pause page.
    static int hold_kind, hold_id, hold_dir;
    static ULONGLONG hold_at;  // when a held button steps the value again
    static bool hold_repeats;  // the hold has stepped once: the ones after it come faster than the first
    static ULONGLONG click_at; // when a click still waiting for its row gives up (the click_mode block above)
    if (play) {
        pointer_drives = false;
        click_mode = 0;
        hold_kind = 0;
        s_row_on = -1;
        s_row_value = false;
        s_row_id = -1;
        s_row_mid = -1;
        s_row_w = 0;
    } else {
        const char *page = fe_page();
        if (strcmp(page, s_page) != 0) { // a new page: the keyboard drives until the pointer is moved
            strncpy(s_page, page, sizeof s_page - 1);
            s_page[sizeof s_page - 1] = 0;
            pointer_drives = false;
            click_mode = 0;
            hold_kind = 0;
            s_row_on = -1;      // the page's report was for the page that just went
            s_row_value = false;
            s_row_id = -1;
            s_row_mid = -1;
            s_row_w = 0;
            s_page_gen++; // the pages read this and report the row under the cursor again (below, FEPCPage)
            logf("[input] menu page '%s'", s_page);
        }
    }
    float sx = 0.0f, sy = 0.0f;
    bool pointer = !play && host3d::cursor_stage(&sx, &sy);
    bool pointer_page = !play && pointer_page_up();
    // the pointer moved: it drives. Its first place on a menu (mouse_x -1: in play, or off the stage) is not a move,
    // or a still cursor would take the highlight from the keyboard on every page that opens under it.
    if (pointer && mouse_x >= 0.0f && (sx != mouse_x || sy != mouse_y)) pointer_drives = true;
    if (pointer) mouse_x = sx, mouse_y = sy;
    else mouse_x = mouse_y = -1.0f;
    if (input) pointer_drives = false; // a menu key moved the highlight: the keyboard drives
    if (click_mode) { // a click: the page reports the row under the cursor within an update or two
        if (pointer && !play && !input && s_row_on >= 0) {
            // the side of the row the pointer is on now: asked of the pointer's place and the row's middle, so a move
            // along a row (which the page does not report again) still steps the value the way the click's place asks.
            // Both are page units (page_x above): the middle a page reports is in its own space.
            const int low = page_of_x(mouse_x) < (float)s_row_mid ? -1 : 1;
            if (click_mode == 2) {
                bool value = pauseui::row_is_value(s_row_on); // the pause page takes the input as the game's rows
                input = value ? (low < 0 ? 3 : 4) : 5;
                if (value) hold_kind = 3, hold_dir = low;
            } else if (s_row_value && s_row_id >= 0 && s_row_id < pcset::count()) {
                // The port's own value row. A slider takes the pointer's place along the row (its width came with
                // the report); any other steps once, towards the side that was clicked.
                int pos = pcset::is_slider(s_row_id) ? slider_from_pointer(s_row_mid, s_row_w, page_of_x(mouse_x)) : -1;
                if (pos >= 0) {
                    pcset::slide(s_row_id, (float)pos);
                    hold_kind = 1, hold_id = s_row_id, hold_dir = low; // a hold follows the pointer along the row
                    logf("[input] menu slider row %d follows the pointer (page '%s')", s_row_id, fe_page());
                } else {
                    pcset::step(s_row_id, low);
                    if (pcset::is_slider(s_row_id)) hold_kind = 1, hold_id = s_row_id, hold_dir = low; // a hold sweeps it
                    logf("[input] menu pointer picks row %d (page '%s')", s_row_id, fe_page());
                }
                fe_refresh();
            } else if (s_row_value) {
                // One of the game's own value rows (no setting of the port's): one direction of the game's own
                // widget, which takes an input and not a place (select would confirm the row and leave the page).
                // A list of choices stops at its first and last value, so the same side twice in the same place
                // would step nowhere the second time at all (the value words sit right of the label, so both of
                // them ask for the same side). Such a row is handed the other direction on the repeated click and
                // the value then flips -- what the game's own pages carry there is a switch. A slider is told apart
                // by the value the game reports back for it (game_value_call): its steps are meant to add up in the
                // direction asked, so it keeps the side.
                static int row_gen = -1, row_mid = -1, row_low = 0, row_dir = 0, row_seq = 0;
                static float row_x;
                static bool row_slider;
                float at_x = page_of_x(mouse_x); // where along the row the click landed, in page units
                bool same = s_row_id < 0 && s_page_gen == row_gen && s_row_mid == row_mid;
                if (!same) row_slider = false;
                else if (g_game_value_seq != row_seq) { // the game answered the click before this one
                    bool was = row_slider;
                    row_slider = g_game_value > 4 || g_game_value < -4;
                    if (row_slider && !was)
                        logf("[input] menu row mid %d answers with the value %d: a slider, the click keeps its side", s_row_mid, g_game_value);
                }
                // The flip is for a click at the SAME SPOT on a switch (the second click there would otherwise step
                // nowhere). A slider's clicks add up in the direction of the click even on the same side: the picture
                // menu's BRILHO bar sits right of its label, so every click on it is the same side, and the second one
                // used to turn the brightness back. A click a few pixels from the last one keeps its own direction.
                bool same_spot = same && (at_x > row_x ? at_x - row_x : row_x - at_x) <= 8.0f;
                bool flip = same_spot && !row_slider && low == row_low && row_dir;
                int dir = flip ? -row_dir : low;
                row_gen = s_page_gen, row_mid = s_row_mid, row_low = low, row_dir = dir, row_seq = g_game_value_seq;
                row_x = at_x;
                input = dir < 0 ? 3 : 4;
                hold_kind = 2, hold_dir = dir;
            } else {
                input = 5; // a row without a value (it opens a screen or waits for a key): the click confirms it
            }
            logf("[input] menu click row %d value %d id %d side %d -> input %d (page '%s')", s_row_on,
                 (int)s_row_value, s_row_id, low, input, fe_page());
            click_mode = 0;
        } else if (play || input || now >= click_at + 250) {
            // the page never reported a row (the click landed between two rows), or it is gone
            if (!input && !play) logf("[input] menu click with no row (page '%s')", fe_page());
            click_mode = 0;
        }
        // else: the row has not come round yet. The port polls far faster than the front end updates (twice in the
        // same frame is common), so dropping the click on the first poll after it would throw away the ones whose
        // page had not run yet -- which is what made the left button miss now and then. Give it 250 ms.
    }
    // A click never steps a value straight away: it waits one update for the page to report the row under the cursor
    // (click_mode, above). The row the port kept may be a step behind -- the pointer can have moved on to another row
    // in the very update of the click -- so stepping at once stepped the row the cursor had just left. The wait costs
    // one frame and takes the row the pointer is really on, which is what the click asked for.
    //
    // The pause page's rows are the port's own (fe/pause_ui.cpp) and take the input as the game's own rows do: a click
    // sends one direction of the value (a row that carries one) or the select (a row that opens a screen or waits for
    // a key). Every other page of the port has its value stepped by the port itself (fe/pc_settings.cpp step) and the
    // game's own rows handed a direction (input 3 or 4) -- both in the click_mode block above, one update after the
    // click, when the page has reported the row the pointer is on.
    bool pause_page = pointer_page && _stricmp(fe_page(), "PCPageIG") == 0;
    // The button latches are read on EVERY poll (never only when the poll goes on to act on them): a press latched while
    // a menu key or a click already waiting kept the block below from running stayed queued and fired many frames later,
    // as a click on whatever row the pointer had moved on to. The edges are kept in step on both paths.
    bool mleft = false, mright = false;
    if (!play) mleft = host3d::cursor_down(0), mright = host3d::cursor_down(1);
    if (!input && !play) {
        bool l = mleft, r = mright;
        if (l && !mouse_left_was) {
            // The pointer is the input in use again. A page change hands the highlight back to the keyboard until the
            // pointer is moved (above), and a click is the pointer speaking: without this one, a click in place on a
            // page the keyboard (or the mouse's own click) just opened was taken with no row known and went nowhere.
            pointer_drives = true;
            hold_kind = 0;
            hold_repeats = false;
            hold_at = now + 400; // a click steps once: only a longer hold sweeps a value
            if (pause_page && pointer) {
                click_mode = 2; // the page reports the row under the cursor: one update later
            } else if (pointer_page && pointer) {
                // the page has just put the focus on the row under the cursor: its click is taken one update later,
                // so it acts on the row the pointer is on and not the one the keyboard left focused
                click_mode = 1;
            } else if (!pointer_page) {
                input = 5; // a page the port gives no rows: the click takes the focus the keyboard left
            }
            if (click_mode) click_at = now, logf("[input] menu click waits for the row (mode %d, page '%s')", click_mode, fe_page());
        } else if (l && hold_kind) {
            // A click must not step twice: a row is only swept once the button has been held a while, and the steps
            // after that come quickly. A row of many values (a slider) is the one a hold is for; a list or a switch
            // only flips, and there a second step would undo the first.
            if (now >= hold_at) {
                hold_at = now + (hold_repeats ? 120 : 400);
                hold_repeats = true;
                if (hold_kind == 1) {
                    // the port's own slider: it follows the pointer along the row for as long as it is held
                    int pos = slider_from_pointer(s_row_mid, s_row_w, page_of_x(mouse_x));
                    if (pos >= 0) pcset::slide(hold_id, (float)pos);
                    else pcset::step(hold_id, hold_dir);
                    fe_refresh();
                } else {
                    input = hold_dir < 0 ? 3 : 4; // the game's own row (or the pause page's): its widget takes it
                }
            }
        } else if (!l && r && !mouse_right_was) {
            input = 6;
        }
        if (!l) hold_kind = 0;
        mouse_left_was = l;
        mouse_right_was = r;
    } else if (!play) {
        // a menu key or a click waiting kept this poll from acting: the edges still move on, so a press made now is not
        // replayed later against another row
        if (!mleft) hold_kind = 0;
        mouse_left_was = mleft;
        mouse_right_was = mright;
    }
    // the pages that follow the pointer (a missing value reads as undefined: their test is false, no row)
    bool live = pointer && pointer_drives;
    int px = live ? (int)sx : -1, py = live ? (int)sy : -1;
    int pgx = live ? (int)page_of_x(sx) : -1, pgy = live ? (int)page_of_y(sy) : -1;
    static bool live_was;
    if (live != live_was) { // tests: which of the two drives the menus
        logf("[input] menu pointer %s (page '%s')", live ? "drives" : "idle", fe_page());
        live_was = live;
    }
    fe_int_set("FEPointerX", px);
    fe_int_set("FEPointerY", py);
    // The same place again under names of the port's own: the game's own front-end code writes back to FEPointerX/Y
    // (it clears them when its own pointer is not the input in use), so a page that reads them mid-update can see -1
    // where the port had just put the cursor. The pages (fe/pages.cpp) read these instead. Unlike the game's own pair
    // these go in page units (page_x / page_y above): they are only ever compared with the pages' own clips.
    fe_int_set("FEPCPointerX", pgx);
    fe_int_set("FEPCPointerY", pgy);
    // A page a player comes back to (right click back, then the row again) puts the same row under the cursor, and a
    // page that reports its row only when it changes would then never report it -- while the port, which drops the row
    // it kept at every page change, waits for a report that never comes: the click lands on no row. The pages watch
    // this number and report the row under the cursor once more when it moves (fe/pages.cpp hover_block, hover_walk).
    fe_int_set("FEPCPage", s_page_gen);
    if (input) {
        g_menu_input_frame = g_frame_count.load();
        g_menu_input = input;
    }
    g_menu_select = g_menu_down[4];
    // Enter is the pad's A for the whole press when, as it went down, a hint stopped the game, or START when no menu
    // page was up (the intro text); a press a menu took stays the menu's.
    enum { kMenu, kA, kStart };
    static bool enter_was;
    static int enter_as, told;
    if (g_menu_select && !enter_was) {
        enter_as = hint_stopped() ? kA : no_menu_page() ? kStart : kMenu;
        if (enter_as != kMenu && told++ < 20) logf("[input] Enter = %s", enter_as == kA ? "A (a hint stops the game)" : "START (no menu page)");
    }
    enter_was = g_menu_select;
    if (g_menu_select && enter_as == kA) p.analog[0] = 255;
    if (g_menu_select && enter_as == kStart) p.buttons |= 0x10;
    // The mouse: captured while play runs (mouse look), free while a menu page is up, where its cursor drives the
    // menus (above). Esc in play frees it until play resumes; play starting or resuming takes it again.
    bool menu_up = !play && !no_menu_page() && !movie_playing_once();
    host3d::set_cursor_free(menu_up); // a click in the window is the menu's: it must not capture the mouse (d3d/host.cpp)
    bool release = play ? pause_down : menu_up;
    if (release && host3d::mouse_captured()) {
        host3d::request_capture(false);
        g_freed_by_esc = true;
    }
    g_release_was_down = pause_down;
    g_script.reset();

    buttons |= p.buttons;
    for (int i = 0; i < 8; i++) analog[i] = std::max(analog[i], p.analog[i]);
    int16_t *axes[4] = {&lx, &ly, &rx, &ry};
    for (int i = 0; i < 4; i++) { // keys push the stick fully, unless a pad already pushes that axis further
        int v = std::clamp(p.axis[i], -1, 1) * 32767;
        if (v && abs(v) > abs(*axes[i])) *axes[i] = (int16_t)v;
    }
    // The walk key: the left (movement) stick runs at walk_speed of the running speed. The game shapes the sticks
    // through a dead zone (FUN_000C24D0, +0xCC: the Xbox SDK's 7849/32767 = 0.24) before the movement channels, so
    // the deflection above the dead zone is what is scaled (scaling the raw one put a slow walk inside the dead zone).
    if (walk && g_cfg.walk_speed < 1.0f) {
        auto walk_axis = [](int16_t v, float share) -> int16_t {
            constexpr float kDead = 0.24f;
            float d = (float)v / 32767.0f, mag = d < 0.0f ? -d : d;
            if (mag <= kDead) return v;
            float s = kDead + (mag - kDead) * share;
            return (int16_t)((d < 0.0f ? -s : s) * 32767.0f);
        };
        lx = walk_axis(lx, g_cfg.walk_speed);
        ly = walk_axis(ly, g_cfg.walk_speed);
    }
}


// ---- in-game capture (game thread) ----
namespace {
int g_cap_action = -1, g_cap_slot;
bool g_cap_wait_up, g_cap_changed;

bool candidate(int vk) // keys that can be bound: mouse buttons and keyboard keys, Shift/Ctrl/Alt as left/right
{
    if (vk == 3 || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return false;
    return vk >= 1 && vk <= 0xFE;
}
} // namespace

void capture_start(int action, int slot)
{
    if (action < 0 || action >= kCount || fixed(action, slot)) return;
    g_cap_action = action;
    g_cap_slot = slot;
    g_cap_wait_up = true;
    logf("[input] key bindings: press a key for %ls (key %d)", kActions[action].name, slot + 1);
}

bool capturing() { return g_cap_action >= 0; }

bool capture_take_change()
{
    bool c = g_cap_changed;
    g_cap_changed = false;
    return c;
}

bool capture_poll(bool focus, int wheel)
{
    if (g_cap_action < 0) return false;
    ensure_defaults();
    uint16_t pressed = 0;
    bool any = false;
    for (int vk = 1; vk <= 0xFE; vk++) {
        bool down = g_script[vk] || (focus && (GetAsyncKeyState(vk) & 0x8000)); // tests: --press-file k:Name
        if (!candidate(vk) || !down) continue;
        any = true;
        if (!pressed) pressed = (uint16_t)vk;
    }
    g_script.reset(); // apply() does not run while capturing
    if (g_cap_wait_up) { // the key that started the capture is still down
        if (!any) g_cap_wait_up = false;
        return true;
    }
    if (!pressed && wheel && focus) pressed = wheel > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    if (!pressed) return true;
    int a = g_cap_action, s = g_cap_slot;
    g_cap_action = -1;
    g_cap_changed = true;
    for (uint16_t k : {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, VK_ESCAPE})
        if (focus && (GetAsyncKeyState(k) & 0x8000)) g_stale[k] = true; // not also a menu press when capture ends
    if (pressed == VK_ESCAPE) {
        logf("[input] key bindings: cancelled");
        return true;
    }
    uint16_t k = pressed == VK_DELETE ? 0 : pressed;
    if (k)
        for (int i = 0; i < kCount; i++)
            for (int j = 0; j < kSlots; j++)
                if ((i != a || j != s) && g_keys[i][j] == k && !fixed(i, j)) g_keys[i][j] = 0; // taken from that action
    g_keys[a][s] = k;
    settings_save();
    logf("[input] key bindings: %ls key %d = %s", kActions[a].name, s + 1, k ? ini_name(k).c_str() : "none");
    return true;
}
} // namespace bindings
