// The port's languages (lang.h). The port's own texts come from the language's menu.jsonl ({"key","text"} rows, the
// BLACK-SUBTITLES hub's translations of the port's menus); the few the port has added since are below, and a text
// neither has stays English.
#include "lang.h"

#include "common.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace lang {
namespace {

struct Info {
    const char *code;
    const wchar_t *name;     // its own name (Russian: in Cyrillic, which only its font can show)
    const char *computer;    // XBOX in the game's text becomes this
};
const Info kInfo[kCount] = {
    {"en", L"ENGLISH", "COMPUTER"},
    {"ptbr", L"PORTUGUÊS (BR)", "COMPUTADOR"},
    {"es", L"ESPAÑOL", "PC"},
    {"fr", L"FRANÇAIS", "PC"},
    {"de", L"DEUTSCH", "PC"},
    {"it", L"ITALIANO", "PC"},
    {"id", L"BAHASA INDONESIA", "PC"},
    {"ru", L"РУССКИЙ", "ПК"},
};

// Texts the port added after the hub's catalog was made: en, pt, es, fr, de, it, id, ru.
const char *const kExtra[][kCount] = {
    {"CONTROLLER SENSITIVITY", "SENSIBILIDADE DO CONTROLE", "SENSIBILIDAD DEL MANDO", "SENSIBILITÉ DE LA MANETTE",
     "CONTROLLER-EMPFINDLICHKEIT", "SENSIBILITÀ DEL CONTROLLER", "SENSITIVITAS KONTROLER", "ЧУВСТВИТЕЛЬНОСТЬ ГЕЙМПАДА"},
    {"AUTOMATIC", "AUTOMÁTICO", "AUTOMÁTICO", "AUTOMATIQUE", "AUTOMATISCH", "AUTOMATICO", "OTOMATIS", "АВТО"},
    {"MONITOR", "MONITOR", "MONITOR", "ÉCRAN", "MONITOR", "MONITOR", "MONITOR", "МОНИТОР"},
    {"AMBIENT OCCLUSION", "OCLUSÃO DE AMBIENTE", "OCLUSIÓN AMBIENTAL", "OCCLUSION AMBIANTE", "UMGEBUNGSVERDECKUNG",
     "OCCLUSIONE AMBIENTALE", "OKLUSI AMBIEN", "ЗАТЕНЕНИЕ"},
    {"V-SYNC", "V-SYNC", "V-SYNC", "V-SYNC", "V-SYNC", "V-SYNC", "V-SYNC", "V-SYNC"},
};

// The endings a value can carry, translated on their own (their keys in the catalog).
const char *const kSuffixes[] = {" (NEXT START)", " (RESTART)", " (LANGUAGE)", " (ORIGINAL)"};

std::mutex g_lock;
bool g_loaded;
std::map<std::string, std::string> g_table;  // English -> translated (UTF-8, before font_text)
std::map<std::string, std::string> g_shown;  // tr(const char *)'s results: they must outlive the call

// UTF-8 <-> code points
std::vector<uint32_t> decode(const std::string &s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) { // a stray byte: kept as it is
            out.push_back(c);
            i++;
            continue;
        }
        uint32_t cp = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
        for (int k = 1; k <= n; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
        out.push_back(cp);
        i += n + 1;
    }
    return out;
}

void put(std::string &o, uint32_t cp)
{
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) o += (char)(0xC0 | cp >> 6), o += (char)(0x80 | (cp & 0x3F));
    else if (cp < 0x10000) o += (char)(0xE0 | cp >> 12), o += (char)(0x80 | ((cp >> 6) & 0x3F)), o += (char)(0x80 | (cp & 0x3F));
    else o += (char)(0xF0 | cp >> 18), o += (char)(0x80 | ((cp >> 12) & 0x3F)), o += (char)(0x80 | ((cp >> 6) & 0x3F)), o += (char)(0x80 | (cp & 0x3F));
}

// The Russian font draws А..Я on these Latin capitals (from the Russian game text against its English: ВЫБОР "BS6OP",
// ПРОФИЛЬ "ZPOVNLJ", НАЖМИ "HAGMN", ШУМЫ "WYMS", ПЕЧАТЬЮ "ZE4ATJU", ЗАЩИЩЕНЫ "3AWNWEHS"...).
const char kRussianGlyphs[] = "A6BFDEG3NNKLMHOZPCTYVXQ4WWJSJ3UR";

// One JSON string value after "name": in a line of a .jsonl catalog (escapes \" \\ \/ \n \t \uXXXX).
bool json_field(const std::string &line, const char *name, std::string &out)
{
    std::string pat = std::string("\"") + name + "\"";
    size_t p = line.find(pat);
    if (p == std::string::npos) return false;
    p = line.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = line.find('"', p);
    if (p == std::string::npos) return false;
    out.clear();
    for (size_t i = p + 1; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= line.size()) break;
        switch (line[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'u': {
            if (i + 4 >= line.size()) return false;
            uint32_t cp = strtoul(line.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < line.size() && line[i + 1] == '\\' && line[i + 2] == 'u') {
                uint32_t lo = strtoul(line.substr(i + 3, 4).c_str(), nullptr, 16);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            put(out, cp);
            break;
        }
        default: out += line[i];
        }
    }
    return false;
}

// every line of the current language's menu.jsonl
template <class F> bool each_row(F f)
{
    FILE *fp = _wfopen((root() + L"\\menu.jsonl").c_str(), L"rb");
    if (!fp) return false;
    std::string line;
    char buf[4096];
    while (fgets(buf, sizeof buf, fp)) {
        line += buf;
        if (line.empty() || line.back() != '\n') {
            if (!feof(fp)) continue;
        }
        f(line);
        line.clear();
    }
    if (!line.empty()) f(line);
    fclose(fp);
    return true;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
    g_table.clear();
    int l = current();
    for (const auto &e : kExtra) g_table[e[0]] = e[l];
    if (l == kEnglish) return;
    int n = 0;
    bool found = each_row([&](const std::string &line) {
        std::string k, t;
        if (json_field(line, "key", k) && json_field(line, "text", t) && !t.empty()) {
            g_table[k] = t;
            n++;
        }
    });
    if (found) logf("lang: %s: %d of the port's texts from %s", code(l), n, narrow(root() + L"\\menu.jsonl").c_str());
    else logf("lang: %s: no %s; the port's menus stay English", code(l), narrow(root() + L"\\menu.jsonl").c_str());
}

} // namespace

int current() { return g_cfg.language >= 0 && g_cfg.language < kCount ? g_cfg.language : kEnglish; }

bool portuguese() { return current() == kPortuguese; }

std::string font_text(const std::string &s)
{
    int l = current();
    if (l != kPortuguese && l != kRussian) return s;
    std::string o;
    for (uint32_t cp : decode(s)) {
        if (l == kPortuguese) {
            if (cp == 0xE3) cp = 0xE4;      // ã -> ä
            else if (cp == 0xF5) cp = 0xF6; // õ -> ö
            else if (cp == 0xC3) cp = 0xC4; // Ã -> Ä
            else if (cp == 0xD5) cp = 0xD6; // Õ -> Ö
        } else {
            if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A'; // Latin stays Latin in lower case
            else if (cp == 0x401 || cp == 0x451) cp = 'E';  // Ё
            else if (cp >= 0x410 && cp <= 0x42F) cp = (uint8_t)kRussianGlyphs[cp - 0x410];
            else if (cp >= 0x430 && cp <= 0x44F) cp = (uint8_t)kRussianGlyphs[cp - 0x430];
        }
        put(o, cp);
    }
    return o;
}

const char *tr(const char *en)
{
    if (!en) return en;
    std::lock_guard<std::mutex> g(g_lock);
    load();
    auto it = g_table.find(en);
    std::string shown = font_text(it == g_table.end() ? std::string(en) : it->second);
    if (shown == en) return en;
    auto &slot = g_shown[en];
    slot = shown;
    return slot.c_str();
}

std::wstring tr(const std::wstring &en)
{
    if (en.empty()) return en;
    std::string s = narrow(en), tail;
    {
        std::lock_guard<std::mutex> g(g_lock);
        load();
        for (bool found = true; found;) { // the endings, last first
            found = false;
            for (const char *suf : kSuffixes) {
                size_t n = strlen(suf);
                if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) {
                    auto it = g_table.find(suf);
                    tail = (it == g_table.end() ? std::string(suf) : it->second) + tail;
                    s.resize(s.size() - n);
                    found = true;
                }
            }
        }
        auto it = g_table.find(s);
        if (it != g_table.end()) s = it->second;
    }
    return widen(font_text(s + tail));
}

std::wstring name(int language)
{
    if (language < 0 || language >= kCount) return L"";
    if (language == kRussian && current() != kRussian) return L"RUSSIAN"; // Cyrillic needs the Russian font
    return kInfo[language].name; // tr() fits it to the font, as every shown value
}

const char *code(int language) { return kInfo[language >= 0 && language < kCount ? language : 0].code; }

int from_code(const char *c)
{
    if (!_stricmp(c, "pt-br") || !_stricmp(c, "pt") || !_stricmp(c, "pt_br")) return kPortuguese;
    if (!_stricmp(c, "en-us") || !_stricmp(c, "en-gb") || !_stricmp(c, "english")) return kEnglish;
    for (int i = 0; i < kCount; i++)
        if (!_stricmp(c, kInfo[i].code)) return i;
    return -1;
}

std::wstring root(int language) { return g_cfg.port_dir + L"\\mods\\language\\" + widen(code(language)); }

std::wstring root() { return root(current()); }

std::string computer() { return font_text(kInfo[current()].computer); }

void reload()
{
    std::lock_guard<std::mutex> g(g_lock);
    g_loaded = false;
}

bool game_text(std::map<uint32_t, std::string> &out)
{
    int l = current();
    if (l == kEnglish) return false;
    out.clear();
    each_row([&](const std::string &line) {
        std::string id, t;
        if (!json_field(line, "id", id) || !json_field(line, "text", t)) return;
        if (l == kRussian) { // the hub keeps the Russian bank's bytes as Latin-1 characters: back to those bytes
            std::string b;
            for (uint32_t cp : decode(t)) b += (char)(cp < 0x100 ? cp : '?');
            t = b;
        } else {
            t = font_text(t);
        }
        out[(uint32_t)strtoul(id.c_str(), nullptr, 16)] = t;
    });
    return !out.empty();
}

} // namespace lang
