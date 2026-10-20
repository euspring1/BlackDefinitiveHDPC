// Saved games from an Xbox, or from another language of the game, dropped into savedata\Partition1\UDATA\45410083.
//
// BLACK keeps one profile per save name: XCreateSaveGame (0x23C05, statically linked XAPI) names the folder after the
// UTF-16 save name (FUN_000237CB: h = (h * 0x10000 + c) mod 0xFFFFFFFFFFC5 over its characters, 12 hex digits), and
// the profile file inside it after the save name too. The name comes from the language file: "Profile" in the
// original, "Perfil" in the Portuguese translation, so a save from another language sits in a folder the game never
// opens. The profile is 0x1800 bytes signed at 0x17EC with XCalculateSignature(1) (FUN_001F2DC0): HMAC-SHA1 with the
// console's signature key over the first 0x17EC bytes, then HMAC-SHA1 of that with its hard disk key. Both keys are
// zero on the port, so a profile signed by a real console fails the check and the game starts without it.
//
// At start every profile whose signature is not the port's is signed again. The game looks for its profile in
// FUN_000C5100(this, name): XFindFirstSaveGame over U:\, comparing each save's name with `name` (ANSI, from the
// language file). Before it does, when the game's own folder is missing while another profile folder is there, that
// profile is moved to the game's name: folder, file and the Name= line of its SaveMeta.xbx.
#include "saveimport.h"

#include "common.h"
#include "sha1.h"

#include <cstring>
#include <string>
#include <vector>

extern "C" {
uint32_t g_find_resume = 0x000C5106; // the original FUN_000C5100 after its first instruction
void __stdcall saveimport_before_find(const char *name);
}

namespace {

constexpr uint32_t kProfileSize = 0x1800, kSigAt = 0x17EC;
constexpr uint32_t kFindProfile = 0x000C5100;
const uint8_t kFindProfileStart[6] = {0x81, 0xEC, 0x90, 0x06, 0x00, 0x00}; // sub esp, 0x690

std::wstring udata() { return g_cfg.hdd_dir + L"\\Partition1\\UDATA\\45410083"; }

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool read_file(const std::wstring &path, std::vector<uint8_t> &data)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    bool ok = size.QuadPart < (1 << 20);
    DWORD got = 0;
    if (ok) {
        data.resize((size_t)size.QuadPart);
        ok = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
    }
    CloseHandle(f);
    return ok;
}

bool write_file(const std::wstring &path, const std::vector<uint8_t> &data)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(f);
    return ok;
}

// the subfolders (or files) of a folder
std::vector<std::wstring> entries(const std::wstring &dir, bool dirs)
{
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d == dirs && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) v.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}

bool is_meta(const std::wstring &file) { return file.size() > 4 && _wcsicmp(file.c_str() + file.size() - 4, L".xbx") == 0; }

void signature(const std::vector<uint8_t> &f, uint8_t out[20])
{
    static const uint8_t zero[16] = {};
    uint8_t inner[20];
    hmac_sha1(zero, sizeof zero, f.data(), kSigAt, nullptr, 0, inner);
    hmac_sha1(zero, sizeof zero, inner, sizeof inner, nullptr, 0, out);
}

// Signs a profile again when its signature is not the port's; false when the file is not a profile.
bool fix_signature(const std::wstring &path)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f) || f.size() != kProfileSize) return false;
    uint8_t sig[20];
    signature(f, sig);
    if (memcmp(sig, f.data() + kSigAt, 20) == 0) return true;
    memcpy(f.data() + kSigAt, sig, 20);
    if (write_file(path, f)) logc("[save] %s signed again for the port (it was saved on another console)", narrow(path).c_str());
    return true;
}

// The game's folder name for a save name (FUN_000237CB).
std::wstring folder_name(const wchar_t *name)
{
    unsigned long long h = 0;
    for (const wchar_t *c = name; *c; c++) h = (h * 0x10000ull + (unsigned short)*c) % 0xFFFFFFFFFFC5ull;
    wchar_t s[16];
    swprintf(s, 16, L"%012llX", h);
    return s;
}

// The profile file of a save folder (its one file besides the .xbx metadata), or "" when there is none.
std::wstring profile_file(const std::wstring &dir)
{
    std::wstring found;
    for (auto &f : entries(dir, false)) {
        if (is_meta(f)) continue;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW((dir + L"\\" + f).c_str(), GetFileExInfoStandard, &a) || a.nFileSizeLow != kProfileSize) continue;
        if (!found.empty()) return L""; // more than one: not a profile folder this knows
        found = f;
    }
    return found;
}

// SaveMeta.xbx: UTF-16 lines "Name=...", "NoCopy=..."; the name replaced, the rest kept.
bool rename_meta(const std::wstring &path, const std::wstring &name)
{
    std::vector<uint8_t> raw;
    if (!read_file(path, raw) || raw.size() < 2 || raw.size() % 2) return false;
    std::wstring text((const wchar_t *)raw.data(), raw.size() / 2);
    bool bom = !text.empty() && text[0] == 0xFEFF;
    if (bom) text.erase(0, 1);
    size_t at = text.find(L"Name=");
    if (at == std::wstring::npos) return false;
    size_t end = text.find_first_of(L"\r\n", at);
    text.replace(at + 5, (end == std::wstring::npos ? text.size() : end) - (at + 5), name);
    if (bom) text.insert(text.begin(), (wchar_t)0xFEFF);
    std::vector<uint8_t> out((const uint8_t *)text.data(), (const uint8_t *)(text.data() + text.size()));
    return write_file(path, out);
}

__declspec(naked) void find_profile_thunk()
{
    __asm {
        pushad
        push dword ptr [esp + 40] // the profile name (ANSI): [esp] return address, +4 this, +8 name, then pushad's 32
        call saveimport_before_find
        popad
        sub esp, 0x690
        jmp dword ptr [g_find_resume]
    }
}

} // namespace

void __stdcall saveimport_before_find(const char *ansi)
{
    static bool done;
    if (done || !ansi || !*ansi || strlen(ansi) > 64) return;
    done = true; // once a run: the game looks for the same name every time
    wchar_t name[80] = {};
    MultiByteToWideChar(28591, 0, ansi, -1, name, 80); // the game's text is Latin-1
    std::wstring want = folder_name(name), root = udata();
    if (exists(root + L"\\" + want)) return;
    // another profile folder: the newest one, if several
    std::wstring from;
    FILETIME newest = {};
    for (auto &d : entries(root, true)) {
        if (d.size() != 12 || profile_file(root + L"\\" + d).empty() || !exists(root + L"\\" + d + L"\\SaveMeta.xbx")) continue;
        WIN32_FILE_ATTRIBUTE_DATA a;
        GetFileAttributesExW((root + L"\\" + d + L"\\" + profile_file(root + L"\\" + d)).c_str(), GetFileExInfoStandard, &a);
        if (from.empty() || CompareFileTime(&a.ftLastWriteTime, &newest) > 0) {
            from = d;
            newest = a.ftLastWriteTime;
        }
    }
    if (from.empty()) return;
    std::wstring src = root + L"\\" + from, dst = root + L"\\" + want, file = profile_file(src);
    if (!MoveFileW(src.c_str(), dst.c_str())) {
        logc("[save] could not move profile folder %s to %s (error %lu)", narrow(from).c_str(), narrow(want).c_str(), GetLastError());
        return;
    }
    if (file != name) MoveFileW((dst + L"\\" + file).c_str(), (dst + L"\\" + name).c_str());
    rename_meta(dst + L"\\SaveMeta.xbx", name);
    fix_signature(dst + L"\\" + name);
    logc("[save] profile %s (\"%s\") imported as \"%s\" (%s)", narrow(from).c_str(), narrow(file).c_str(), narrow(name).c_str(),
         narrow(want).c_str());
}

void saveimport_scan()
{
    std::wstring root = udata();
    for (auto &d : entries(root, true)) {
        std::wstring file = profile_file(root + L"\\" + d);
        if (!file.empty()) fix_signature(root + L"\\" + d + L"\\" + file);
    }
}

void install_saveimport_patch()
{
    uint8_t *code = (uint8_t *)(uintptr_t)kFindProfile;
    if (memcmp(code, kFindProfileStart, sizeof kFindProfileStart) != 0) {
        logf("save: the profile search is not at 0x%08x in this build; saves from another language are not imported", kFindProfile);
        return;
    }
    code[0] = 0xE9; // jmp thunk (the 6th byte is never reached: the thunk returns past it)
    int32_t rel = (int32_t)((uintptr_t)&find_profile_thunk - (kFindProfile + 5));
    memcpy(code + 1, &rel, 4);
    logf("save: profile search hooked (profiles from another language or console are imported)");
}
