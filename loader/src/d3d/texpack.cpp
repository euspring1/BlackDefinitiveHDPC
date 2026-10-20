// HD textures from a PCSX2 texture pack, matched to the game's textures by picture (see texpack.h).
//
// A signature is the image boxed down to 16x16 cells of alpha-weighted colour and coverage, plus a 32x32 "detail"
// picture (luma minus its local mean, normalised) whose dot product with another's is their structure correlation
// (NCC): it follows the drawing whatever the grading. Two textures match when their aspect ratios agree, their
// transparency agrees (additive effects on black in the Xbox version are often alpha-blended in the PS2 one and would
// draw as boxes), and either
//   - a score of colour difference plus (1 - 16x16 luma correlation) is low, or low and clearly lower than the next
//     candidate that is not a copy of the same picture, and the structure agrees (NCC >= 0.7, or the candidate is
//     also the best by structure, 0.15 ahead of the next different picture), or
//   - the score is within reach and the candidate is the best by structure, NCC >= 0.75 and 0.12 ahead.
// Measured over the first minute of all eight missions: 346 of 862 game textures found (227 before the structure
// check and the PS2 alpha fix), the wrong ones a handful of plain greys.
//
// PS2 alpha is 0..0x80 (0x80 opaque); a pack image whose alpha stays at or under 0xA0 is on that scale (DXT5 rounding
// takes many to 0x87-0x8A) and is doubled when the game's texture is opaque. Pack textures go to D3DPOOL_DEFAULT (video memory) so that hundreds of them at 2048x2048 do not
// fill the 32-bit address space; they are dropped before a device Reset and loaded again on demand.
#include "d3d/texpack.h"

#include "common.h"
#include "d3d/host.h"

#include <wincodec.h>
#include <emmintrin.h>

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

constexpr int S = 16, N = 32;
constexpr uint32_t kIndexMagic = 0x32505442; // "BTP2": with the detail picture and the 0xA0 PS2 alpha rule
constexpr uint32_t kPs2AlphaMax = 0xA0;

struct Sig {
    float c[S * S][4]; // r g b (alpha-weighted mean) and coverage, 0..255
    float l[N * N];    // detail luma: zero mean, unit length (all zero for a flat picture)
};

// A signature laid out for fast comparison (made from Sig, never stored): its 16x16 cells as separate channels, and
// its 16x16 luma (alpha-weighted) with zero mean and unit length, so that the luma correlation is one dot product.
struct Fast {
    float r[S * S], g[S * S], b[S * S], a[S * S];
    float lum[S * S];
};

struct PackEntry {
    std::wstring file; // name in the pack folder
    uint32_t w = 0, h = 0;
    bool ps2_alpha = false; // alpha on the PS2 scale (doubled in the signature)
    Sig sig;
    Fast fast;
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
    // the detail picture: alpha-weighted luma in 32x32 cells, minus its 5x5 neighbourhood mean (wrapping: textures
    // tile), then zero mean and unit length
    std::vector<double> L(N * N);
    for (int cy = 0; cy < N; cy++)
        for (int cx = 0; cx < N; cx++) {
            UINT x0 = cx * w / N, x1 = std::max(x0 + 1, (cx + 1) * w / N), y0 = cy * h / N, y1 = std::max(y0 + 1, (cy + 1) * h / N);
            double v = 0, n = 0;
            for (UINT y = y0; y < y1 && y < h; y++)
                for (UINT x = x0; x < x1 && x < w; x++) {
                    uint32_t p = px[(size_t)y * w + x];
                    v += (0.3 * ((p >> 16) & 255) + 0.59 * ((p >> 8) & 255) + 0.11 * (p & 255)) * (p >> 24) / 255.0;
                    n++;
                }
            L[cy * N + cx] = n ? v / n : 0;
        }
    double m = 0, ss = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            double b = 0;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) b += L[((y + dy + N) % N) * N + (x + dx + N) % N];
            s.l[y * N + x] = (float)(L[y * N + x] - b / 25);
            m += s.l[y * N + x];
        }
    m /= N * N;
    for (float &v : s.l) {
        v -= (float)m;
        ss += (double)v * v;
    }
    double inv = ss > 1e-9 ? 1 / sqrt(ss) : 0;
    for (float &v : s.l) v = (float)(v * inv);
}

float dot(const float *a, const float *b, int n) // n a multiple of 4
{
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    for (; i < n; i += 4) s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    s0 = _mm_add_ps(s0, s1);
    alignas(16) float t[4];
    _mm_store_ps(t, s0);
    return t[0] + t[1] + t[2] + t[3];
}

// structure correlation of two detail pictures, -1..1
float ncc(const Sig &a, const Sig &b) { return dot(a.l, b.l, N * N); }

void make_fast(const Sig &s, Fast &f)
{
    double m = 0;
    for (int i = 0; i < S * S; i++) {
        f.r[i] = s.c[i][0];
        f.g[i] = s.c[i][1];
        f.b[i] = s.c[i][2];
        f.a[i] = s.c[i][3];
        f.lum[i] = (float)((0.3 * s.c[i][0] + 0.59 * s.c[i][1] + 0.11 * s.c[i][2]) * s.c[i][3] / 255);
        m += f.lum[i];
    }
    m /= S * S;
    double ss = 0;
    for (float &v : f.lum) {
        v -= (float)m;
        ss += (double)v * v;
    }
    double inv = ss >= 1e-3 ? 1 / sqrt(ss) : 0; // a flat picture correlates with nothing
    for (float &v : f.lum) v = (float)(v * inv);
}

// colour_distance and alpha_distance of two fast signatures in one pass
void distances(const Fast &x, const Fast &y, float *colour, float *alpha)
{
    const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), inv255 = _mm_set1_ps(1 / 255.0f);
    __m128 d = _mm_setzero_ps(), da = _mm_setzero_ps(), ws = _mm_setzero_ps();
    for (int i = 0; i < S * S; i += 4) {
        __m128 ax = _mm_loadu_ps(x.a + i), ay = _mm_loadu_ps(y.a + i);
        __m128 w = _mm_mul_ps(_mm_min_ps(ax, ay), inv255);
        __m128 dr = _mm_and_ps(_mm_sub_ps(_mm_loadu_ps(x.r + i), _mm_loadu_ps(y.r + i)), abs_mask);
        __m128 dg = _mm_and_ps(_mm_sub_ps(_mm_loadu_ps(x.g + i), _mm_loadu_ps(y.g + i)), abs_mask);
        __m128 db = _mm_and_ps(_mm_sub_ps(_mm_loadu_ps(x.b + i), _mm_loadu_ps(y.b + i)), abs_mask);
        d = _mm_add_ps(d, _mm_mul_ps(_mm_add_ps(_mm_add_ps(dr, dg), db), w));
        ws = _mm_add_ps(ws, w);
        da = _mm_add_ps(da, _mm_and_ps(_mm_sub_ps(ax, ay), abs_mask));
    }
    alignas(16) float t[3][4];
    _mm_store_ps(t[0], d);
    _mm_store_ps(t[1], ws);
    _mm_store_ps(t[2], da);
    float sd = t[0][0] + t[0][1] + t[0][2] + t[0][3], sw = t[1][0] + t[1][1] + t[1][2] + t[1][3];
    float sa = (t[2][0] + t[2][1] + t[2][2] + t[2][3]) / (S * S);
    *colour = (sw > 0 ? sd / (3 * sw) : 255) + sa * 0.5f;
    *alpha = sa;
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
             fread(e.sig.c, sizeof e.sig.c, 1, f) == 1 && fread(e.sig.l, sizeof e.sig.l, 1, f) == 1;
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
        fwrite(e.sig.l, sizeof e.sig.l, 1, f);
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
            e.ps2_alpha = max_alpha(px) <= kPs2AlphaMax;
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
    if (!same_pack) {
        // decisions for another pack (or an older matching rule): start the file again, or the new decisions would be
        // appended under the old header and be ignored at every start
        g_map.clear();
        DeleteFileW(map_path().c_str());
        logf("[texpack] %s was for another pack or matching rule; matching again", narrow(map_path()).c_str());
    }
}

void note_decision(const std::wstring &id, const std::wstring &file, float score, float gap, const std::wstring &nearest)
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
    if (file == L"-" && !nearest.empty()) // a rejection names the closest pack image, to check by hand
        fprintf(f, "%s = %s  # %.1f %.1f nearest %s\n", narrow(id).c_str(), narrow(file).c_str(), score, gap, narrow(nearest).c_str());
    else
        fprintf(f, "%s = %s  # %.1f %.1f\n", narrow(id).c_str(), narrow(file).c_str(), score, gap);
    fclose(f);
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
    if (!g_cfg.textures || _wcsicmp(dir.c_str(), L"off") == 0) { // TEXTURES OFF (PC SETTINGS > MODS)
        logf("[texpack] off");
        return false;
    }
    if (dir.empty()) dir = (g_cfg.port_dir.empty() ? std::wstring(L".") : g_cfg.port_dir) + L"\\mods\\replacements";
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
    g_key = folder_key(g_dir) ^ kIndexMagic; // a new matching rule starts the index and the decisions afresh
    std::wstring idx = cache_dir() + L"\\texpack.idx";
    if (!load_index(idx)) {
        build_index();
        save_index(idx);
    }
    for (auto &e : g_pack) make_fast(e.sig, e.fast);
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
        double ra = log((double)w / h);
        std::vector<const PackEntry *> cands;
        for (auto &e : g_pack)
            if (fabs(ra - log((double)e.w / e.h)) <= 0.05) cands.push_back(&e);
        Fast fs;
        make_fast(s, fs);
        // the best by score and the best by structure (SIMD; a decision runs inside a frame)
        float d1 = 1e9f, c1 = -2;
        const PackEntry *by_struct = nullptr;
        size_t n = cands.size();
        std::vector<float> score(n), corr(n), alpha(n);
        for (size_t i = 0; i < n; i++) {
            float colour;
            distances(fs, cands[i]->fast, &colour, &alpha[i]);
            score[i] = colour * 0.5f + (1 - dot(fs.lum, cands[i]->fast.lum, S * S)) * 40;
            corr[i] = ncc(s, cands[i]->sig);
            if (score[i] < d1) d1 = score[i], best = cands[i];
            if (corr[i] > c1) c1 = corr[i], by_struct = cands[i];
        }
        // the runners-up, leaving out copies (or regradings) of the same picture as the winner: the first candidate in
        // order that is not one
        float d2 = 1e9f, c2 = -2;
        std::vector<uint32_t> order(n);
        for (size_t i = 0; i < n; i++) order[i] = (uint32_t)i;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return score[a] < score[b]; });
        for (uint32_t i : order)
            if (cands[i] != best && ncc(best->sig, cands[i]->sig) <= 0.95f) {
                d2 = score[i];
                break;
            }
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return corr[a] > corr[b]; });
        for (uint32_t i : order)
            if (cands[i] != by_struct && ncc(by_struct->sig, cands[i]->sig) <= 0.9f) {
                c2 = corr[i];
                break;
            }
        bool ok = false;
        float best_alpha = 255;
        for (size_t i = 0; i < n; i++)
            if (cands[i] == best) best_alpha = alpha[i];
        if (best && best_alpha < 15) {
            float cn = ncc(s, best->sig);
            bool lead = best == by_struct; // the score's choice is also the structure's
            bool structure = cn >= 0.7f || (lead && c1 - c2 >= 0.15f);
            ok = ((d1 < 10 || (d1 < 22 && d2 - d1 > 6)) && structure) || (d1 < 22 && lead && c1 >= 0.75f && c1 - c2 >= 0.12f);
        }
        note_decision(id, ok ? best->file : L"-", best ? d1 : 0, best ? d2 - d1 : 0, best ? best->file : L"");
        if (!ok) return nullptr;
        logf("[texpack] %s -> %s (%.1f, next %.1f, structure %.2f)", narrow(id).c_str(), narrow(best->file).c_str(), d1, d2,
             ncc(s, best->sig));
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
