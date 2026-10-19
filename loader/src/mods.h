// A mod is a folder under <port>\mods\<name> whose game\ files stand in for the game's own while the mod is on:
// the port serves them from the file layer (kernel/k_file.cpp), so a mod can be turned on and off from the in-game
// PC SETTINGS > MODS page without ever touching the game's files. Turning one off is just a .off marker file in
// the mod's own folder (default: on), so a mod folder carries its own state and no ini parsing is needed.
//
// A mod can also name the game language it is for (mod.ini "lang = en" / "ptbr"): the enabled mod of the highest
// priority sets the language being played and mods of another language are blocked (Mod::blocked), so e.g. the
// English keyboard prompts cannot leak a file into the Portuguese game.
#pragma once

#include <string>
#include <vector>

namespace mods {
struct Mod {
    std::string folder;    // folder name under mods\ (stable id in logs)
    std::string title;     // shown in PC SETTINGS > MODS (mod.ini "name", else the folder name prettied)
    std::wstring base;     // <mods>\<name>
    std::wstring root;     // <mods>\<name>\game: the files served in place of the game's
    int priority = 0;      // mod.ini "priority": the highest wins when two mods bring the same file
    std::string lang;      // mod.ini "lang": the game language this mod is for ("" = any; see below)
    bool on = true;
    bool at_start = true;  // its state when this session started (a change needs a restart: the game's text is in RAM)
    bool blocked = false;  // a mod of another language is being played: its files are not served at all
};

const std::vector<Mod> &list();
void scan();                 // (re)read the mods folder; call before the front-end pages are built
uint32_t revision();         // bumped when a mod is turned on or off (subtitle files reload on it)
// A file mod was turned on or off since this session started: the game read its files at boot, so the change only
// takes effect at the next start (the port restarts itself for that: fe/restart.cpp).
bool restart_needed();
bool toggle(int i);          // flips mod i and returns its new state
void set_all(bool on);       // --no-mods / --all-mods
// The enabled mod that provides `relative` (a game-relative path, e.g. "language\strings\MainUS.bin").
bool resolve(const std::string &relative, std::wstring &out);
} // namespace mods
