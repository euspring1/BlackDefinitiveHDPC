// HD textures from a PCSX2 texture pack, matched to the game's textures by picture (see texpack.h).
//
// A signature is the image boxed down to 16x16 cells of alpha-weighted colour and coverage. Two textures match when
// their aspect ratios agree and a score of colour difference plus (1 - luma correlation) is low and clearly lower
// than the next candidate's, and their transparency agrees (additive effects on black in the Xbox version are often
// alpha-blended in the PS2 one and would draw as boxes). Measured on the first level: about one game texture in three
// found, all of them right on inspection.
//
// PS2 alpha is 0..0x80 (0x80 opaque); a pack image whose alpha stays at or under 0x82 is doubled when the game's
// texture is opaque. Pack textures go to D3DPOOL_DEFAULT (video memory) so that hundreds of them at 2048x2048 do not
// fill the 32-bit address space; they are dropped before a device Reset and loaded again on demand.
#include "d3d/texpack.h"

#include "common.h"
#include "d3d/host.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace xd3d {
namespace texpack {
namespace {

constexpr int S = 16;
constexpr uint32_t kIndexMagic = 0x31505442; // "BTP1"

struct Sig {
    float c[S * S][4]; // r g b (alpha-weighted mean) and coverage, 0..255
};

struct PackEntry {
    std::wstring file; // name in the pack folder
    uint32_t w = 0, h = 0;
    bool ps2_alpha = false; // alpha on the PS2 scale (doubled in the signature)
    Sig sig;
};

bool g_tried, g_on;
std::wstring g_dir;                                     // the pack folder
uint64_t g_key;                                         // its file list (count, sizes, dates): cache validity
std::vector<PackEntry> g_pack;                          // the index
std::unordered_map<std::wstring, std::wstring> g_map;   // decisions: game texture id -> pack file ("-" = none)
std::map<std::wstring, IDirect3DTexture9 *> g_loaded;   // pack file -> its texture (one reference held here)
std::set<IDirect3DBaseTexture9 *> g_owned;

std::wstring cache_dir() { return (g_cfg.port_dir.empty() ? std::wstring(L".") : g_cfg.port_dir) + L"\\cache"; }

bool is_dir(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> list(const std::wstring &dir, const wchar_t *pattern, bool dirs)
{
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d == dirs && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) v.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end());
    return v;
}

// The folder holding the pack's images: the one given or the first one below it that holds DDS or PNG files (a
// pack is often unpacked as <download>\SLUS-21376\replacements, copied whole or in part).
std::wstring find_images(const std::wstring &dir, int depth = 0)
{
    if (!list(dir, L"*.dds", false).empty() || !list(dir, L"*.png", false).empty()) return dir;
    if (depth >= 5) return L"";
    for (auto &sub : list(dir, L"*", true)) {
        std::wstring found = find_images(dir + L"\\" + sub, depth + 1);
        if (!found.empty()) return found;
    }
    return L"";
}

// ---- images ----

void make_sig(const std::vector<uint32_t> &px, UINT w, UINT h, Sig &s)
{
    for (int cy = 0; cy < S; cy++)
        for (int cx = 0; cx < S; cx++) {
            UINT x0 = cx * w / S, x1 = std::max(x0 + 1, (cx + 1) * w / S), y0 = cy * h / S, y1 = std::max(y0 + 1, (cy + 1) * h / S);
            double a = 0, r = 0, g = 0, b = 0, n = 0;
            for (UINT y = y0; y < y1 && y < h; y++)
                for (UINT x = x0; x < x1 && x < w; x++) {
                    uint32_t p = px[(size_t)y * w + x];
                    double pa = (p >> 24) / 255.0;
                    a += pa;
                    r += ((p >> 16) & 255) * pa;
                    g += ((p >> 8) & 255) * pa;
                    b += (p & 255) * pa;
                    n++;
                }
            float *c = s.c[cy * S + cx];
            c[3] = n ? (float)(a / n * 255) : 0;
            c[0] = a > 1e-6 ? (float)(r / a) : 0;
            c[1] = a > 1e-6 ? (float)(g / a) : 0;
            c[2] = a > 1e-6 ? (float)(b / a) : 0;
        }
}

uint32_t max_alpha(const std::vector<uint32_t> &px)
{
    uint32_t m = 0;
    for (uint32_t p : px) m = std::max(m, p >> 24);
    return m;
}

void double_alpha(std::vector<uint32_t> &px)
{
    for (uint32_t &p : px) p = (p & 0xFFFFFF) | std::min(255u, (p >> 24) * 2) << 24;
}

bool load_png(const std::wstring &path, std::vector<uint32_t> &px, UINT &w, UINT &h)
{
    IWICImagingFactory *f = wic_factory();
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *fr = nullptr;
    IWICFormatConverter *cv = nullptr;
    bool ok = f && SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h));
    if (ok) {
        px.resize((size_t)w * h);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE *)px.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return ok;
}

struct Dds {
    UINT w = 0, h = 0, mips = 1;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
};

UINT block_bytes(D3DFORMAT f) { return f == D3DFMT_DXT1 ? 8 : 16; }
size_t level_size(UINT w, UINT h, D3DFORMAT f) { return (size_t)std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * block_bytes(f); }

bool read_dds_header(HANDLE f, Dds &d)
{
    uint8_t hd[128];
    DWORD got = 0;
    if (!ReadFile(f, hd, 128, &got, nullptr) || got != 128 || memcmp(hd, "DDS ", 4)) return false;
    d.h = *(uint32_t *)(hd + 12);
    d.w = *(uint32_t *)(hd + 16);
    d.mips = std::max(1u, *(uint32_t *)(hd + 28));
    if (!memcmp(hd + 84, "DXT1", 4)) d.fmt = D3DFMT_DXT1;
    else if (!memcmp(hd + 84, "DXT3", 4)) d.fmt = D3DFMT_DXT3;
    else if (!memcmp(hd + 84, "DXT5", 4)) d.fmt = D3DFMT_DXT5;
    else return false;
    return d.w && d.h;
}

// Level `l` of an open DDS (file pointer anywhere).
bool read_dds_level(HANDLE f, const Dds &d, UINT l, std::vector<uint8_t> &data, UINT &lw, UINT &lh)
{
    uint64_t off = 128;
    lw = d.w;
    lh = d.h;
    for (UINT i = 0; i < l; i++) {
        off += level_size(lw, lh, d.fmt);
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    data.resize(level_size(lw, lh, d.fmt));
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
}

// A small picture of a pack image for its signature: the DDS level whose short side is the last at or over 64.
bool pack_picture(const std::wstring &path, std::vector<uint32_t> &px, UINT &w, UINT &h, UINT &fw, UINT &fh)
{
    if (_wcsicmp(path.c_str() + path.size() - 4, L".png") == 0) {
        if (!load_png(path, px, w, h)) return false;
        fw = w;
        fh = h;
        return true;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    Dds d;
    bool ok = read_dds_header(f, d);
    UINT l = 0, lw = d.w, lh = d.h;
    while (ok && l + 1 < d.mips && std::min(lw, lh) / 2 >= 64) {
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
        l++;
    }
    std::vector<uint8_t> data;
    ok = ok && read_dds_level(f, d, l, data, w, h);
    CloseHandle(f);
    if (!ok) return false;
    px = decode_dxt(data.data(), (UINT)(std::max(1u, (w + 3) / 4) * block_bytes(d.fmt)), w, h, d.fmt);
    fw = d.w;
    fh = d.h;
    return true;
}

// ---- index ----

uint64_t folder_key(const std::wstring &dir)
{
    uint64_t k = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
    for (const wchar_t *pat : {L"*.dds", L"*.png"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            for (const wchar_t *c = fd.cFileName; *c; c++) mix(*c);
            mix(fd.nFileSizeLow);
            mix(fd.ftLastWriteTime.dwLowDateTime);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return k;
}

bool load_index(const std::wstring &path)
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    uint32_t magic = 0, count = 0;
    uint64_t key = 0;
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == kIndexMagic && fread(&key, 8, 1, f) == 1 && key == g_key && fread(&count, 4, 1, f) == 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        PackEntry e;
        uint32_t len = 0;
        uint8_t ps2 = 0;
        ok = fread(&len, 4, 1, f) == 1 && len < 1024;
        if (!ok) break;
        e.file.resize(len);
        ok = fread(&e.file[0], 2, len, f) == len && fread(&e.w, 4, 1, f) == 1 && fread(&e.h, 4, 1, f) == 1 && fread(&ps2, 1, 1, f) == 1 &&
             fread(e.sig.c, sizeof e.sig.c, 1, f) == 1;
        e.ps2_alpha = ps2 != 0;
        if (ok) g_pack.push_back(std::move(e));
    }
    fclose(f);
    if (!ok) g_pack.clear();
    return ok;
}

void save_index(const std::wstring &path)
{
    CreateDirectoryW(cache_dir().c_str(), nullptr);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return;
    uint32_t count = (uint32_t)g_pack.size();
    fwrite(&kIndexMagic, 4, 1, f);
    fwrite(&g_key, 8, 1, f);
    fwrite(&count, 4, 1, f);
    for (auto &e : g_pack) {
        uint32_t len = (uint32_t)e.file.size();
        uint8_t ps2 = e.ps2_alpha;
        fwrite(&len, 4, 1, f);
        fwrite(e.file.data(), 2, len, f);
        fwrite(&e.w, 4, 1, f);
        fwrite(&e.h, 4, 1, f);
        fwrite(&ps2, 1, 1, f);
        fwrite(e.sig.c, sizeof e.sig.c, 1, f);
    }
    fclose(f);
}

void build_index()
{
    ULONGLONG t0 = GetTickCount64();
    for (const wchar_t *pat : {L"*.dds", L"*.png"})
        for (auto &name : list(g_dir, pat, false)) {
            std::vector<uint32_t> px;
            UINT w, h, fw, fh;
            if (!pack_picture(g_dir + L"\\" + name, px, w, h, fw, fh)) {
                logf("[texpack] skipped %s (not DXT1/3/5 or PNG)", narrow(name).c_str());
                continue;
            }
            PackEntry e;
            e.file = name;
            e.w = fw;
            e.h = fh;
            e.ps2_alpha = max_alpha(px) <= 0x82;
            if (e.ps2_alpha) double_alpha(px);
            make_sig(px, w, h, e.sig);
            g_pack.push_back(std::move(e));
        }
    logf("[texpack] indexed %zu pack textures in %.1f s", g_pack.size(), (GetTickCount64() - t0) / 1000.0);
}

// ---- decisions ----

std::wstring map_path() { return cache_dir() + L"\\texpack.map"; }

void load_map()
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, map_path().c_str(), L"r") || !f) return;
    char line[512];
    bool same_pack = false;
    while (fgets(line, sizeof line, f)) {
        unsigned long long key = 0;
        if (sscanf_s(line, "# pack %llx", &key) == 1) {
            same_pack = key == g_key;
            continue;
        }
        char id[64] = "", file[400] = "";
        if (!same_pack || line[0] == '#' || sscanf_s(line, " %63s = %399s", id, (unsigned)sizeof id, file, (unsigned)sizeof file) != 2) continue;
        g_map[widen(id)] = widen(file);
    }
    fclose(f);
    if (!same_pack) g_map.clear();
}

void note_decision(const std::wstring &id, const std::wstring &file, float score, float gap)
{
    g_map[id] = file;
    CreateDirectoryW(cache_dir().c_str(), nullptr);
    bool fresh = GetFileAttributesW(map_path().c_str()) == INVALID_FILE_ATTRIBUTES;
    FILE *f = nullptr;
    if (_wfopen_s(&f, map_path().c_str(), L"a") || !f) return;
    if (fresh) {
        fprintf(f, "# Texture pack matches: game texture = pack file (\"-\" = the game's own). Change a file to \"-\" to\n"
                   "# keep the game's texture where a match is wrong. Delete this file to match again.\n");
        fprintf(f, "# pack %llx\n", (unsigned long long)g_key);
    }
    fprintf(f, "%s = %s  # %.1f %.1f\n", narrow(id).c_str(), narrow(file).c_str(), score, gap);
    fclose(f);
}

float colour_distance(const Sig &a, const Sig &b)
{
    double d = 0, da = 0, wsum = 0;
    for (int i = 0; i < S * S; i++) {
        double wgt = std::min(a.c[i][3], b.c[i][3]) / 255.0;
        for (int k = 0; k < 3; k++) d += fabs(a.c[i][k] - b.c[i][k]) * wgt;
        wsum += wgt;
        da += fabs(a.c[i][3] - b.c[i][3]);
    }
    return (float)((wsum > 0 ? d / (3 * wsum) : 255) + da / (S * S) * 0.5);
}

float alpha_distance(const Sig &a, const Sig &b)
{
    double da = 0;
    for (int i = 0; i < S * S; i++) da += fabs(a.c[i][3] - b.c[i][3]);
    return (float)(da / (S * S));
}

float luma_correlation(const Sig &a, const Sig &b)
{
    double la[S * S], lb[S * S], ma = 0, mb = 0;
    for (int i = 0; i < S * S; i++) {
        la[i] = (0.3 * a.c[i][0] + 0.59 * a.c[i][1] + 0.11 * a.c[i][2]) * a.c[i][3] / 255;
        lb[i] = (0.3 * b.c[i][0] + 0.59 * b.c[i][1] + 0.11 * b.c[i][2]) * b.c[i][3] / 255;
        ma += la[i];
        mb += lb[i];
    }
    ma /= S * S;
    mb /= S * S;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < S * S; i++) {
        sab += (la[i] - ma) * (lb[i] - mb);
        saa += (la[i] - ma) * (la[i] - ma);
        sbb += (lb[i] - mb) * (lb[i] - mb);
    }
    if (saa < 1e-3 || sbb < 1e-3) return 0;
    return (float)(sab / sqrt(saa * sbb));
}

// ---- loading ----

// A D3DPOOL_DEFAULT texture filled through a system-memory copy.
IDirect3DTexture9 *default_texture(UINT w, UINT h, UINT levels, D3DFORMAT fmt, const std::function<void(UINT, uint8_t *, UINT)> &fill)
{
    IDirect3DDevice9 *dev = host3d::dev();
    IDirect3DTexture9 *sys = nullptr, *t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, 0, fmt, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return nullptr;
    for (UINT l = 0; l < levels; l++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(l, &lr, nullptr, 0))) {
            fill(l, (uint8_t *)lr.pBits, (UINT)lr.Pitch);
            sys->UnlockRect(l);
        }
    }
    if (FAILED(dev->CreateTexture(w, h, levels, 0, fmt, D3DPOOL_DEFAULT, &t, nullptr)) || FAILED(dev->UpdateTexture(sys, t))) {
        if (t) t->Release();
        t = nullptr;
    }
    sys->Release();
    return t;
}

// A BGRA image (any size) as a mipmapped texture, halved first while larger than texture_pack_max.
IDirect3DTexture9 *bgra_texture(std::vector<uint32_t> px, UINT w, UINT h)
{
    while (std::max(w, h) > (UINT)g_cfg.texture_pack_max && w > 1 && h > 1) {
        UINT nw = w / 2, nh = h / 2;
        px = half_size(px, w, h, nw, nh);
        w = nw;
        h = nh;
    }
    std::vector<std::vector<uint32_t>> levels{std::move(px)};
    std::vector<std::pair<UINT, UINT>> dims{{w, h}};
    while (dims.back().first > 1 || dims.back().second > 1) {
        UINT lw = dims.back().first, lh = dims.back().second, nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        levels.push_back(half_size(levels.back(), lw, lh, nw, nh));
        dims.push_back({nw, nh});
    }
    return default_texture(w, h, (UINT)levels.size(), D3DFMT_A8R8G8B8, [&](UINT l, uint8_t *dst, UINT pitch) {
        for (UINT y = 0; y < dims[l].second; y++) memcpy(dst + (size_t)y * pitch, &levels[l][(size_t)y * dims[l].first], dims[l].first * 4);
    });
}

IDirect3DTexture9 *load_dds(const std::wstring &path, bool alpha2)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    Dds d;
    IDirect3DTexture9 *t = nullptr;
    if (read_dds_header(f, d)) {
        UINT top = 0, w = d.w, h = d.h;
        while (top + 1 < d.mips && std::max(w, h) > (UINT)g_cfg.texture_pack_max) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            top++;
        }
        UINT levels = d.mips - top;
        if (levels == 1 || w % 4 || h % 4) { // no mip chain (or a size DXT cannot start at): decoded and mipmapped here
            std::vector<uint8_t> data;
            UINT lw, lh;
            if (read_dds_level(f, d, top, data, lw, lh)) {
                std::vector<uint32_t> px = decode_dxt(data.data(), std::max(1u, (lw + 3) / 4) * block_bytes(d.fmt), lw, lh, d.fmt);
                if (alpha2) double_alpha(px);
                t = bgra_texture(std::move(px), lw, lh);
            }
        } else {
            std::vector<std::vector<uint8_t>> data(levels);
            std::vector<std::pair<UINT, UINT>> dims(levels);
            bool ok = true;
            for (UINT l = 0; ok && l < levels; l++) ok = read_dds_level(f, d, top + l, data[l], dims[l].first, dims[l].second);
            if (ok && alpha2 && d.fmt == D3DFMT_DXT5) // both alpha end points doubled: every interpolated alpha doubles
                for (auto &lv : data)
                    for (size_t i = 0; i + 16 <= lv.size(); i += 16) {
                        lv[i] = (uint8_t)std::min(255, lv[i] * 2);
                        lv[i + 1] = (uint8_t)std::min(255, lv[i + 1] * 2);
                    }
            if (ok)
                t = default_texture(w, h, levels, d.fmt, [&](UINT l, uint8_t *dst, UINT pitch) {
                    UINT row = std::max(1u, (dims[l].first + 3) / 4) * block_bytes(d.fmt), rows = std::max(1u, (dims[l].second + 3) / 4);
                    for (UINT y = 0; y < rows; y++) memcpy(dst + (size_t)y * pitch, &data[l][(size_t)y * row], row);
                });
        }
    }
    CloseHandle(f);
    return t;
}

IDirect3DTexture9 *load(const PackEntry &e, bool alpha2)
{
    std::wstring key = e.file + (alpha2 ? L"|a2" : L"");
    auto it = g_loaded.find(key);
    if (it != g_loaded.end()) return it->second;
    ULONGLONG t0 = GetTickCount64();
    std::wstring path = g_dir + L"\\" + e.file;
    IDirect3DTexture9 *t = nullptr;
    if (_wcsicmp(path.c_str() + path.size() - 4, L".png") == 0) {
        std::vector<uint32_t> px;
        UINT w, h;
        if (load_png(path, px, w, h)) {
            if (alpha2) double_alpha(px);
            t = bgra_texture(std::move(px), w, h);
        }
    } else {
        t = load_dds(path, alpha2);
    }
    if (!t) logf("[texpack] could not load %s", narrow(e.file).c_str());
    else {
        g_owned.insert(t);
        ULONGLONG ms = GetTickCount64() - t0;
        if (ms > 30) logf("[texpack] %s took %llu ms to load", narrow(e.file).c_str(), ms);
    }
    g_loaded[key] = t;
    return t;
}

const PackEntry *find_entry(const std::wstring &file)
{
    for (auto &e : g_pack)
        if (e.file == file) return &e;
    return nullptr;
}

} // namespace

bool enabled()
{
    if (g_tried) return g_on;
    g_tried = true;
    std::wstring dir = g_cfg.texture_pack;
    if (_wcsicmp(dir.c_str(), L"off") == 0) return false;
    if (dir.empty()) dir = (g_cfg.port_dir.empty() ? std::wstring(L".") : g_cfg.port_dir) + L"\\textures\\replacements";
    else if (dir.size() < 2 || (dir[1] != L':' && dir[0] != L'\\')) dir = g_cfg.port_dir + L"\\" + dir;
    if (!is_dir(dir)) {
        if (!g_cfg.texture_pack.empty()) logf("[texpack] %s is not a folder; no texture pack", narrow(dir).c_str());
        return false;
    }
    g_dir = find_images(dir);
    if (g_dir.empty()) {
        logf("[texpack] no DDS or PNG images in %s", narrow(dir).c_str());
        return false;
    }
    g_key = folder_key(g_dir);
    std::wstring idx = cache_dir() + L"\\texpack.idx";
    if (!load_index(idx)) {
        build_index();
        save_index(idx);
    }
    load_map();
    g_on = !g_pack.empty();
    logc("[texpack] %zu HD textures from %s (%zu matches known)", g_pack.size(), narrow(g_dir).c_str(), g_map.size());
    return g_on;
}

IDirect3DTexture9 *lookup(const std::wstring &id, const std::vector<uint32_t> &px, UINT w, UINT h)
{
    if (!g_on || w < 16 || h < 16 || px.empty()) return nullptr;
    bool game_opaque = max_alpha(px) > 0xC0;
    const PackEntry *best = nullptr;
    auto known = g_map.find(id);
    if (known != g_map.end()) {
        if (known->second == L"-") return nullptr;
        best = find_entry(known->second);
        if (!best) return nullptr;
    } else {
        Sig s;
        make_sig(px, w, h, s);
        float d1 = 1e9f, d2 = 1e9f;
        double ra = log((double)w / h);
        for (auto &e : g_pack) {
            if (fabs(ra - log((double)e.w / e.h)) > 0.05) continue;
            float d = colour_distance(s, e.sig) * 0.5f + (1 - luma_correlation(s, e.sig)) * 40;
            if (d < d1) {
                d2 = d1;
                d1 = d;
                best = &e;
            } else if (d < d2) {
                d2 = d;
            }
        }
        bool ok = best && (d1 < 10 || (d1 < 22 && d2 - d1 > 6)) && alpha_distance(s, best->sig) < 15;
        note_decision(id, ok ? best->file : L"-", best ? d1 : 0, best ? d2 - d1 : 0);
        if (!ok) return nullptr;
        logf("[texpack] %s -> %s (%.1f, next %.1f)", narrow(id).c_str(), narrow(best->file).c_str(), d1, d2);
    }
    IDirect3DTexture9 *t = load(*best, best->ps2_alpha && game_opaque);
    if (t) t->AddRef();
    return t;
}

bool owns(IDirect3DBaseTexture9 *t) { return t && g_owned.count(t) != 0; }

void release_all()
{
    for (auto &kv : g_loaded)
        if (kv.second) kv.second->Release();
    g_loaded.clear();
    g_owned.clear();
}

} // namespace texpack
} // namespace xd3d
