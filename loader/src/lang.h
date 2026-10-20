// LANGUAGE (PC SETTINGS > MODS): English (the game's own text) or Brazilian Portuguese. The game reads its text at
// boot, so a choice applies at the next start; the session keeps the language it started with (g_cfg.language).
#pragma once

#include <string>

namespace lang {
enum { kEnglish = 0, kPortuguese = 1 };
bool portuguese();                         // the language this session plays in
const char *tr(const char *en);            // one of the port's own texts (UTF-8) in that language
std::wstring tr(const std::wstring &en);   // the same for a shown value: its known parts are translated
const char *code(int language);            // black.ini: "en" / "ptbr"
int from_code(const char *code);           // -1 if unknown
// The Portuguese files ship with the port in <port>\mods\language\ptbr, laid out like the game's language folder
// (strings\MainUS.bin, fonts\*.bin). While the session plays in Portuguese, a game file under language\ is served
// from there when it has one (kernel/k_file.cpp); empty in English.
std::wstring root();
} // namespace lang
