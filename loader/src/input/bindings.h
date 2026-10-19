// Keyboard and mouse bound to the game's own actions (fire, reload, crouch, ...), not to pad buttons.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace bindings {

constexpr int kSlots = 2; // keys per action
// A key: a Windows virtual-key code (mouse buttons are VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1/2), or one
// of these.
enum : uint16_t { KEY_NONE = 0, KEY_WHEEL_UP = 0x100, KEY_WHEEL_DOWN = 0x101 };

struct Action {
    const char *ini;     // black.ini: key_<ini> = Key[,Key]
    const wchar_t *name; // shown on the KEY BINDINGS page (upper case)
    int game;            // the action's index in the game's binding table
    uint16_t defaults[kSlots];
};
const Action *actions(int *count);
uint16_t key(int action, int slot);
void set_key(int action, int slot, uint16_t k);
void reset_defaults();
bool fixed(int action, int slot); // Esc is always the first pause key (it also cancels a key capture)
std::wstring key_name(uint16_t k);
// Short upper-case ASCII name for the game's own text (button prompts): "R", "CTRL", "MOUSE ESQ"; "" for none.
std::string hud_key_name(uint16_t k);
// The first key bound to action (index into actions()), as hud_key_name; "" if none.
std::string hud_action_key(int action);

bool load_ini(const char *key, const char *value); // black.ini lines key_<ini>; false if not one of ours
void save_ini(FILE *f);

// Once per pad poll (input/xinput.cpp): in play, the bound keys and mouse buttons pressed into the Xbox pad, through
// the game's current binding table; in menus, the menu keys for take_menu_input. focus: the game window is in front;
// mouse_live: the mouse is captured.
void apply(uint16_t &buttons, uint8_t analog[8], int16_t &lx, int16_t &ly, int16_t &rx, int16_t &ry, bool focus,
           bool mouse_live, int wheel);
bool in_play(); // the player is being updated (not a menu, pause or cutscene)
// The menu key pressed (or repeating) at the last poll as the menus' input id, once: 1 up, 2 down, 3 left, 4 right,
// 5 select (Enter), 6 back (Backspace); 0 none. The front end's input (fe/calls.cpp) takes it.
int take_menu_input();
// The page that follows the pointer reports (fe/calls.cpp, PCHover) the row it just put the focus on: the row's index
// (-1: the pointer is on none), whether it carries a value list, the setting it is (-1: none), the row's middle and
// its width in the picture's pixels. Such a row has no action of its own — the game's select would confirm it and back
// out of the page — and the game's widgets only take a direction that is held, which a pointer cannot make: so the
// port changes the value itself while the pointer drives. A slider uses the middle and the width to take the pointer's
// place along the row.
void menu_row(int row, bool value, int id, int mid, int width);
// A function of the front end's own pages changed a setting and the game reported the value it handed it (fe/calls.cpp
// fe_dispatch): a couple of steps for a list of choices, 0-100 for a slider. The port tells the two apart by it when
// the pointer clicks the same place twice: a list stops at its ends and would answer the second click with nothing at
// all, so the click comes back the other way (the row then flips, which is what the game's own pages carry there);
// a slider's steps are meant to add up in the direction asked.
void game_value_call(int value);
bool menu_select_held(); // Enter held in a menu or movie (movies: skip)
// Tests: --press "SECONDSs:k:Name" holds a key (black.ini spelling) for the next poll, without focus.
void script_hold(const std::string &name);

// In-game key capture (the KEY BINDINGS page, fe/bind_list.cpp): the next key or mouse button pressed after all are
// released binds to (action, slot) and is taken from any other action; Esc cancels, Delete clears the slot. Meanwhile
// keys reach neither the game nor the menus. capture_poll() runs once per pad poll before apply() and returns true
// while capturing (apply() is skipped then); wheel: notches, also while the mouse is not captured.
void capture_start(int action, int slot);
bool capturing();
bool capture_poll(bool focus, int wheel);
bool capture_take_change(); // true once after a capture ended (the page refreshes its rows)


} // namespace bindings
