// Auto-aim switch: the game's pull of the view toward the nearest enemy ahead (patch in the player update).
#pragma once

void install_autoaim_patch();
void autoaim_set(bool on); // live; also sets g_cfg.autoaim
// 0 off, 1 on, 2 controller only (on while the controller was used last, off while the keyboard / mouse was).
void autoaim_set_mode(int mode);
int autoaim_mode();
// Once per pad poll (input/xinput.cpp): whether the controller / the keyboard or mouse was used in it.
void autoaim_note_input(bool pad, bool keyboard_mouse);
bool pad_in_use(); // the controller, not the keyboard / mouse, was used last
