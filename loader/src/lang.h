// LANGUAGE (PC SETTINGS > MODS): the game's text, the port's menus and the movie subtitles in one of eight languages.
// The game reads its text at boot, so a choice applies at the next start; the session keeps the language it started
// with (g_cfg.language).
//
// A language's files sit in <port>\mods\language\<code> (the BLACK-SUBTITLES hub's data, by soldieg and the community):
//   menu.jsonl      the game's text by string id ({"id","text"}) and the port's own ({"key","text"}): fe/pages.cpp builds
//                   the game's language\strings\MainUS.bin from the game's English one and these rows, at every start
//   fonts\*.bin     the game's fonts where the language needs others (Indonesian, Russian): kernel/k_file.cpp serves a
//                   game file under language\ from here when the folder has one
//   subtitles\*.srt the movies' subtitles (d3d/subtitles.cpp); English when a language has none for a movie
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace lang {
enum { kEnglish, kPortuguese, kSpanish, kFrench, kGerman, kItalian, kIndonesian, kRussian, kCount };
int current();                             // the language this session plays in
bool portuguese();
const char *tr(const char *en);            // one of the port's own texts (UTF-8), translated and fit for the font
std::wstring tr(const std::wstring &en);   // the same for a shown value: its known parts are translated
std::wstring name(int language);           // its own name, as the language played can show it (before tr())
const char *code(int language);            // black.ini and the folder: en, ptbr, es, fr, de, it, id, ru
int from_code(const char *code);           // also pt-BR, en-US...; -1 if unknown
std::wstring root(int language);           // <port>\mods\language\<code>
std::wstring root();                       // the current one's
// UTF-8 text as the language's font draws it: Portuguese ã / õ as ä / ö (the game's font has no tilde vowels; the
// translation writes them that way), Russian: the Cyrillic letters on the Latin capitals its font draws them with and
// Latin letters in lower case (which it keeps Latin).
std::string font_text(const std::string &utf8);
std::string computer();                    // the word for the PC, in the game's text where it said XBOX
// The game's text in the current language from its menu.jsonl, by string id, as the game's bank holds it (UTF-8 in
// the font's terms). False when the language has none (English: the game's own).
bool game_text(std::map<uint32_t, std::string> &out);
void reload();                             // the port's texts again (after the menu catalog changed)
} // namespace lang
