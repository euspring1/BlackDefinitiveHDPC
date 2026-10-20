#include "mods.h"

#include "common.h"
#include "lang.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mods {
namespace {

std::vector<Mod> g_mods;
std::vector<int> g_order; // resolution order: priority, then folder
bool g_scanned;
bool g_all_off;
uint32_t g_revision = 1;

std::wstring mods_dir() { return g_cfg.port_dir + L"\\mods"; }

// mod.ini is a line per setting: "name = <shown in the menu>", "priority = <n>" (highest wins when two mods
// bring the same game file) and "lang = <code>" (the game language a mod is for: en, ptbr, ...). See language():
// a mod whose language is not the one being played is left out, so e.g. the English keyboard prompts cannot
// leak into the Portuguese game.
void read_ini(const std::wstring &ini, std::string &name, int &priority, std::string &lang)
{
    FILE *f = _wfopen(ini.c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k(line);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        for (char &c : k) c = (char)tolower((unsigned char)c);
        const char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (k == "priority") {
            priority = atoi(v);
            continue;
        }
        if (k == "lang") {
            lang = v;
            while (!lang.empty() && (lang.back() == '\n' || lang.back() == '\r' || lang.back() == ' ' || lang.back() == '\t'))
                lang.pop_back();
            for (char &c : lang) c = (char)tolower((unsigned char)c);
            continue;
        }
        if (k != "name") continue;
        name = v;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
    }
    fclose(f);
}

// The language being played: PC SETTINGS > MODS > LANGUAGE (lang.h). A mod that names another language is
// blocked: its files are not served at all, so a file only it brings cannot leak into the running game (mods of the
// same language, or of none, still layer by priority).
std::string language() { return lang::code(g_cfg.language); }

void apply_language()
{
    std::string active = language();
    for (Mod &m : g_mods) m.blocked = !m.lang.empty() && m.lang != active;
}

std::string pretty(const std::string &folder)
{
    std::string s;
    for (char c : folder) {
        if (c == '_' || c == '-' || c == '.') c = ' ';
        s += (char)toupper((unsigned char)c);
    }
    return s;
}

} // namespace

void scan()
{
    g_mods.clear();
    g_scanned = true;
    std::wstring dir = mods_dir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return; // no mods folder at all
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        std::wstring base = dir + L"\\" + fd.cFileName;
        // only folders that bring game files are file mods; the rest (port\blackrt.dll only) are not listed
        if (GetFileAttributesW((base + L"\\game").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        Mod m;
        m.folder = narrow(fd.cFileName);
        std::string custom;
        read_ini(base + L"\\mod.ini", custom, m.priority, m.lang);
        m.title = custom.empty() ? pretty(m.folder) : custom;
        m.base = base;
        m.root = base + L"\\game";
        m.on = GetFileAttributesW((base + L"\\.off").c_str()) == INVALID_FILE_ATTRIBUTES;
        m.at_start = m.on;
        g_mods.push_back(std::move(m));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod &a, const Mod &b) { return a.folder < b.folder; });
    g_order.resize(g_mods.size());
    for (size_t i = 0; i < g_mods.size(); i++) g_order[i] = i;
    std::sort(g_order.begin(), g_order.end(), [](int a, int b) {
        if (g_mods[a].priority != g_mods[b].priority) return g_mods[a].priority > g_mods[b].priority;
        return g_mods[a].folder < g_mods[b].folder;
    });
    apply_language();
    for (const Mod &m : g_mods)
        logf("mods: %s (%s) priority %d%s%s%s", m.folder.c_str(), m.title.c_str(), m.priority,
             m.lang.empty() ? "" : (" lang " + m.lang).c_str(), m.on ? "" : " - off",
             m.blocked ? " - off (another language is being played)" : "");
}

const std::vector<Mod> &list() { return g_mods; }

uint32_t revision() { return g_revision; }

bool restart_needed()
{
    for (const Mod &m : g_mods)
        if (m.on != m.at_start) return true;
    return false;
}

void set_all(bool on)
{
    if (g_mods.empty()) scan();
    g_all_off = !on;
    for (size_t i = 0; i < g_mods.size(); i++)
        if (g_mods[i].on != on) toggle((int)i);
    logf("mods: all %s", on ? "on" : "off");
}

bool toggle(int i)
{    if (i < 0 || i >= (int)g_mods.size()) return false;
    Mod &m = g_mods[i];
    std::wstring marker = mods_dir() + L"\\" + widen(m.folder) + L"\\.off";
    if (m.on) { // turning it off: a marker file in the mod's own folder
        FILE *f = _wfopen(marker.c_str(), L"w");
        if (f) fclose(f);
        m.on = false;
    } else {
        _wremove(marker.c_str());
        m.on = true;
    }
    g_revision++;
    apply_language();
    logf("mods: %s %s", m.folder.c_str(), m.on ? "on" : "off");
    return m.on;
}

bool resolve(const std::string &relative, std::wstring &out)
{
    if (!g_scanned) scan();
    if (g_all_off) return false;
    for (int i : g_order) {
        const Mod &m = g_mods[i];
        if (!m.on || m.blocked) continue;
        std::wstring alt = m.root + widen(relative);
        DWORD a = GetFileAttributesW(alt.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            out = alt;
            return true;
        }
    }
    return false;
}

} // namespace mods
