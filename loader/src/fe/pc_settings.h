// The port's settings as rows of the in-game PC SETTINGS pages: each row steps through its choices with left/right.
#pragma once

#include <string>
#include <vector>

namespace pcset {

// a page of rows (container entry: page in lower case)
struct Group {
    const char *page, *title;
    std::vector<int> rows;
};
const std::vector<Group> &groups();
// The MODS page (mods.h): one row per mod found under <port>\mods. Kept out of groups() because the game's Options
// page copy has room for exactly four rows; the pause menu's PC SETTINGS page (fe/pause_ui.cpp) shows it too.
const Group &mods_group();
const Group &input_group(); // INPUT: keyboard and mouse or controller prompts, the row below MODS

int count();                        // rows; row n shows its value in &PC_V<n> (fe/vars.h)
const char *label(int n);           // the row's caption
void step(int n, int dir);          // left (-1) / right (+1): the next choice, applied and saved
std::wstring value(int n);          // the current choice as shown
void fill_page(int group);          // a group's page is up: its rows' values into the page's values (fe/vars.h)
void refresh_sliders();             // externObject.FEPCS<n>: every slider's position (0-100)
bool is_slider(int n);              // a bullet bar (26 positions) instead of a list of choices
int slider_pos(int n);              // the slider's position, 0-100 (also given to the page as externObject.FEPCS<n>)
void slide(int n, float pos);       // a slider moved to pos (0-100)
bool restart_only(int n);           // applies at the next start: on the main menu's pages, not the pause menu's
bool is_frame_rate(int n);          // FRAME RATE: applies at the next start, but shown in the pause menu too
const char *link(int n);            // a row that opens a page (KEY BINDINGS), or nullptr

} // namespace pcset
