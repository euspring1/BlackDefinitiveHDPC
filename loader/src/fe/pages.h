// The in-game PC SETTINGS pages, built at start from the user's own front-end file.
#pragma once

#include <string>

// Reads <game>\export\frontend\FEMain.bin, adds the pages and writes the result under out_root (same relative
// path); false (and the game keeps its own menus) if the file is missing or not laid out as expected.
bool fe_build_pages(const std::wstring &game_dir, const std::wstring &out_root);
