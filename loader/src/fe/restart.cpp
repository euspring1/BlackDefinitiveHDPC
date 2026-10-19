// Restart the game after a mod's files change.
//
// A file mod (mods\<name>\game\...) changes the text, the icons and the likes, and the game reads those files when
// it starts: the table of strings stays in RAM for the whole session (checked: turning the PT-BR mod off and
// backing out to the main menu kept the Portuguese text). So the port restarts itself: once the player is back at
// the main menu with a changed mod, it starts a fresh black.exe (a couple of seconds later, so this process has
// exited and released its files) and closes this one. Nothing is lost: the main menu is up, and settings_save runs
// first.
#include "fe/restart.h"

#include "common.h"
#include "d3d/host.h"
#include "input/bindings.h"
#include "mods.h"
#include "settings.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {
bool g_done;
constexpr uint32_t kFeObject = 0x002D199C, kFePage = 0x3840;

// The main menu is up (the two title pages), so there is no mission to lose.
bool at_title_menu()
{
    uint32_t fe = *(const uint32_t *)(uintptr_t)kFeObject;
    if (!fe) return false;
    const char *page = (const char *)(uintptr_t)(fe + kFePage);
    return !strcmp(page, "blacktitlemenu") || !strcmp(page, "blacktitlemenu2");
}

// Everything the current process was started with, minus the executable itself.
std::wstring args_after_exe()
{
    std::wstring s = GetCommandLineW();
    size_t i;
    if (!s.empty() && s[0] == L'"') {
        size_t close = s.find(L'"', 1);
        i = close == std::wstring::npos ? s.size() : close + 1;
    } else {
        i = s.find(L' ');
        if (i == std::wstring::npos) i = s.size();
    }
    return s.substr(i);
}
} // namespace

void restart_poll()
{
    if (g_done || !g_cfg.known_build) return;
    if (!mods::restart_needed()) return;
    if (bindings::in_play()) return;      // in a mission: keep playing, it applies at the next start
    if (!at_title_menu()) return;         // let the player leave the menus first
    g_done = true;
    logf("mods: a mod changed; restarting the game");
    settings_save();
    std::wstring exe = g_cfg.port_dir + L"\\black.exe";
    std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & start \"\" \"" + exe + L"\"" + args_after_exe();
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_cfg.port_dir.c_str(),
                       &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        logf("mods: could not start the restart helper (error %lu)", GetLastError());
    }
    PostMessageW(host3d::window(), WM_CLOSE, 0, 0); // as QUIT: the port saves and exits
}
