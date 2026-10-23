// Button prompts that follow the device in use (PC SETTINGS > INPUT > INPUT DEVICE: AUTOMATIC, MOUSE/KEYBOARD or
// CONTROLLER): the game's text and the menus' button legend show the bound keys while the keyboard and mouse are used
// and the controller's buttons while the controller is, switching the moment the other one is used.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prompts {

// A string of the game's text in both forms (fe/pages.cpp builds them at start, in the order of the game's bank).
struct Text {
    uint32_t key;
    std::string pad, keys;
};
void set_text(std::vector<Text> text);

// The legend's three texts for the keyboard (select, confirm, back), as fe/pages.cpp placed them.
const std::vector<std::wstring> &legend_keys();

void install(); // the hook on the game's text bank (known build only)

bool pad();                 // the controller's prompts are shown
void set_mode(int mode);    // 0 automatic, 1 keyboard and mouse, 2 controller (g_cfg.input_mode)
void note_input(bool pad, bool keyboard_mouse); // once per input poll: what was used in it

} // namespace prompts
