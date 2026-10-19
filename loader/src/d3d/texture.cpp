// Xbox textures -> host textures. Swizzled (Morton order) and linear layouts are decoded on the CPU into
// A8R8G8B8, DXT data is uploaded as is. Host copies are cached per resource header and re-uploaded when
// the header changes, the game locks the texture, or a sampled hash of the texel data changes (checked at the
// first use in a frame, every 16th frame for textures that have not changed for a while).
#include "d3d/device.h"
#include "d3d/dispatch.h"
#include "d3d/host.h"
#include "d3d/state.h"
#include "perf.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace xd3d {
namespace {

struct Entry {
    IDirect3DBaseTexture9 *tex = nullptr;
    uint32_t data = 0, format = 0, size = 0;
    const void *palette = nullptr;
    uint32_t hash = 0;
    bool dirty = false;
    uint32_t checked_frame = ~0u; // the frame the contents were last checked (hashing on every bind cost ~8 ms a frame)
    uint32_t stable = 0;          // consecutive checks without a change
    bool replaced = false;        // tex is a PNG from textures\ (see replacement), not the game's texels
};

// A texture whose contents did not change for this many checks is checked every 16th frame only (busy
// scenes bind hundreds of textures a frame; hashing each once a frame still cost ~3 ms). Textures that
// change (video frames, render-to-memory) stay at one check a frame; a Lock marks a texture dirty at once.
constexpr uint32_t kStableChecks = 30;

std::unordered_map<const Resource *, Entry> g_cache;
CRITICAL_SECTION g_cs;
struct Init {
    Init() { InitializeCriticalSection(&g_cs); }
} g_init;

uint32_t fmt_of(const PixelContainer *pc) { return (pc->Format >> FMT_FORMAT_SHIFT) & 0xFF; }

uint32_t sample_hash(const uint8_t *p, uint32_t bytes)
{
    uint32_t h = 2166136261u;
    uint32_t n = bytes / 4;
    const uint32_t *d = (const uint32_t *)p;
    uint32_t step = n > 8192 ? n / 4096 : 1;
    for (uint32_t i = 0; i < n; i += step) h = (h ^ d[i]) * 16777619u;
    return h;
}

// Morton-order offset tables for one mip level.
struct Swizzle {
    uint32_t mx = 0, my = 0;
    Swizzle(uint32_t w, uint32_t h)
    {
        uint32_t bit = 1;
        for (uint32_t i = 1; i < w || i < h; i <<= 1) {
            if (i < w) { mx |= bit; bit <<= 1; }
            if (i < h) { my |= bit; bit <<= 1; }
        }
    }
    static uint32_t deposit(uint32_t v, uint32_t mask)
    {
        uint32_t r = 0;
        for (uint32_t b = 1; mask; b <<= 1) {
            uint32_t low = mask & (~mask + 1);
            if (v & b) r |= low;
            mask &= mask - 1;
        }
        return r;
    }
};

uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
uint32_t expand4(uint32_t v) { return v * 17; }

// Decode one texel of an uncompressed Xbox format into A8R8G8B8.
uint32_t decode(uint32_t fmt, const uint8_t *p, const uint32_t *pal)
{
    uint32_t v16 = p[0] | (p[1] << 8);
    switch (fmt) {
    case XFMT_A8R8G8B8: case XFMT_LIN_A8R8G8B8: return *(const uint32_t *)p;
    case XFMT_X8R8G8B8: case XFMT_LIN_X8R8G8B8: return *(const uint32_t *)p | 0xFF000000;
    case XFMT_R5G6B5: case XFMT_LIN_R5G6B5:
        return 0xFF000000 | expand5(v16 >> 11) << 16 | expand6((v16 >> 5) & 63) << 8 | expand5(v16 & 31);
    case XFMT_A1R5G5B5: case XFMT_LIN_A1R5G5B5:
        return ((v16 & 0x8000) ? 0xFF000000 : 0) | expand5((v16 >> 10) & 31) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_X1R5G5B5: case XFMT_LIN_X1R5G5B5:
        return 0xFF000000 | expand5((v16 >> 10) & 31) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_A4R4G4B4: case XFMT_LIN_A4R4G4B4:
        return expand4(v16 >> 12) << 24 | expand4((v16 >> 8) & 15) << 16 | expand4((v16 >> 4) & 15) << 8 | expand4(v16 & 15);
    case XFMT_R6G5B5: case XFMT_LIN_R6G5B5:
        return 0xFF000000 | expand6(v16 >> 10) << 16 | expand5((v16 >> 5) & 31) << 8 | expand5(v16 & 31);
    case XFMT_L8: case XFMT_LIN_L8: return 0xFF000000 | p[0] * 0x010101u;
    case XFMT_AL8: case XFMT_LIN_AL8: return p[0] * 0x01010101u;
    case XFMT_A8: case XFMT_LIN_A8: return (uint32_t)p[0] << 24 | 0xFFFFFF;
    case XFMT_A8L8: case XFMT_LIN_A8L8: return (uint32_t)p[1] << 24 | p[0] * 0x010101u;
    case XFMT_L16: case XFMT_LIN_L16: return 0xFF000000 | p[1] * 0x010101u;
    case XFMT_P8: return pal ? pal[p[0]] : 0xFF000000 | p[0] * 0x010101u;
    case XFMT_R8B8: case XFMT_LIN_R8B8: return 0xFF000000 | (uint32_t)p[1] << 16 | p[0];
    case XFMT_G8B8: case XFMT_LIN_G8B8: return 0xFF000000 | (uint32_t)p[1] << 8 | p[0];
    case XFMT_V8U8: return 0xFF000000 | ((p[0] ^ 0x80u) << 16) | ((p[1] ^ 0x80u) << 8);
    case XFMT_A8B8G8R8: case XFMT_LIN_A8B8G8R8: { uint32_t v = *(const uint32_t *)p; return (v & 0xFF00FF00) | (v & 0xFF) << 16 | (v >> 16 & 0xFF); }
    case XFMT_B8G8R8A8: case XFMT_LIN_B8G8R8A8: { uint32_t v = *(const uint32_t *)p; return (v << 24) | (v >> 8); }
    case XFMT_R8G8B8A8: case XFMT_LIN_R8G8B8A8: { uint32_t v = *(const uint32_t *)p; return (v << 24) | (v >> 24 & 0xFF) << 16 | (v >> 16 & 0xFF) << 8 | (v >> 8 & 0xFF); }
    case XFMT_D24S8: case XFMT_LIN_D24S8: case XFMT_F24S8: case XFMT_LIN_F24S8: { uint32_t d = *(const uint32_t *)p >> 24; return 0xFF000000 | d * 0x010101u; }
    case XFMT_D16: case XFMT_LIN_D16: case XFMT_F16: case XFMT_LIN_F16: return 0xFF000000 | p[1] * 0x010101u;
    default: return 0xFFFF00FF; // magenta: format not handled
    }
}

uint32_t yuv_to_rgb(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    auto clamp = [](int x) { return x < 0 ? 0 : x > 255 ? 255 : x; };
    int r = clamp((298 * c + 409 * e + 128) >> 8), g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8),
        b = clamp((298 * c + 516 * d + 128) >> 8);
    return 0xFF000000 | r << 16 | g << 8 | b;
}

// Write level `level` of `pc` (from Xbox memory) into a locked host rect.
void upload_level(const PixelContainer *pc, uint32_t face, uint32_t level, uint8_t *dst, uint32_t dst_pitch, const uint32_t *pal)
{
    uint32_t fmt = fmt_of(pc);
    const FormatInfo &fi = format_info(fmt);
    uint32_t pitch, w, h;
    uint32_t off = texture_level_offset(pc, face, level, &pitch, &w, &h);
    const uint8_t *src = (const uint8_t *)phys_to_virt(pc->Data) + off;
    if (fi.compressed) {
        uint32_t rows = (h + 3) / 4;
        for (uint32_t y = 0; y < rows; y++) memcpy(dst + y * dst_pitch, src + y * pitch, pitch < dst_pitch ? pitch : dst_pitch);
        return;
    }
    uint32_t bpp = fi.bpp / 8;
    if (fmt == XFMT_YUY2 || fmt == XFMT_UYVY) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * pitch;
            uint32_t *d = (uint32_t *)(dst + y * dst_pitch);
            for (uint32_t x = 0; x + 1 < w; x += 2, s += 4) {
                int y0, y1, u, v;
                if (fmt == XFMT_YUY2) { y0 = s[0]; u = s[1]; y1 = s[2]; v = s[3]; }
                else { u = s[0]; y0 = s[1]; v = s[2]; y1 = s[3]; }
                d[x] = yuv_to_rgb(y0, u, v);
                d[x + 1] = yuv_to_rgb(y1, u, v);
            }
        }
        return;
    }
    if (fi.linear) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * pitch;
            uint32_t *d = (uint32_t *)(dst + y * dst_pitch);
            for (uint32_t x = 0; x < w; x++) d[x] = decode(fmt, s + x * bpp, pal);
        }
        return;
    }
    Swizzle sw(w, h);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t oy = Swizzle::deposit(y, sw.my);
        uint32_t *d = (uint32_t *)(dst + y * dst_pitch);
        uint32_t ox = 0;
        for (uint32_t x = 0; x < w; x++) {
            d[x] = decode(fmt, src + (ox | oy) * bpp, pal);
            ox = (ox - sw.mx) & sw.mx; // next x in Morton order
        }
    }
}

D3DFORMAT host_format(uint32_t fmt)
{
    switch (fmt) {
    case XFMT_DXT1: return D3DFMT_DXT1;
    case XFMT_DXT3: return D3DFMT_DXT3;
    case XFMT_DXT5: return D3DFMT_DXT5;
    default: return D3DFMT_A8R8G8B8;
    }
}

uint32_t total_bytes(const PixelContainer *pc)
{
    uint32_t w, h, d, levels;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    uint32_t pitch, lw, lh;
    bool cube = (pc->Format & FMT_CUBEMAP) != 0;
    uint32_t last = texture_level_offset(pc, cube ? 5 : 0, levels - 1, &pitch, &lw, &lh);
    const FormatInfo &fi = format_info(fmt_of(pc));
    return last + pitch * (fi.compressed ? (lh + 3) / 4 : lh);
}

// Refill an existing host texture (same size/format) from Xbox memory.
void refill(IDirect3DBaseTexture9 *base, Resource *r, Palette *pal)
{
    auto *pc = (PixelContainer *)r;
    const uint32_t *palette = pal ? (const uint32_t *)phys_to_virt(pal->Data) : nullptr;
    if (base->GetType() == D3DRTYPE_CUBETEXTURE) {
        auto *t = (IDirect3DCubeTexture9 *)base;
        for (uint32_t f = 0; f < 6; f++)
            for (uint32_t l = 0; l < t->GetLevelCount(); l++) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, 0))) {
                    upload_level(pc, f, l, (uint8_t *)lr.pBits, lr.Pitch, palette);
                    t->UnlockRect((D3DCUBEMAP_FACES)f, l);
                }
            }
        return;
    }
    auto *t = (IDirect3DTexture9 *)base;
    for (uint32_t l = 0; l < t->GetLevelCount(); l++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0))) {
            upload_level(pc, 0, l, (uint8_t *)lr.pBits, lr.Pitch, palette);
            t->UnlockRect(l);
        }
    }
}

IDirect3DBaseTexture9 *create_and_upload(Resource *r, Palette *pal)
{
    auto *pc = (PixelContainer *)r;
    uint32_t fmt = fmt_of(pc);
    uint32_t w, h, d, levels;
    pixel_container_dims(pc, &w, &h, &d, &levels);
    if (levels == 0) levels = 1;
    bool cube = (pc->Format & FMT_CUBEMAP) != 0;
    uint32_t dim = (pc->Format >> FMT_DIMENSION_SHIFT) & 0xF;
    if (dim == 3) {
        KNOTE_ONCE("volume textures are not uploaded yet (%ux%ux%u fmt %s)", w, h, d, format_info(fmt).name);
        return nullptr;
    }
    const uint32_t *palette = pal ? (const uint32_t *)phys_to_virt(pal->Data) : nullptr;
    D3DFORMAT hf = host_format(fmt);
    IDirect3DDevice9 *dev = host3d::dev();
    if (cube) {
        IDirect3DCubeTexture9 *t = nullptr;
        if (FAILED(dev->CreateCubeTexture(w, levels, 0, hf, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
        for (uint32_t f = 0; f < 6; f++)
            for (uint32_t l = 0; l < levels; l++) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, 0))) {
                    upload_level(pc, f, l, (uint8_t *)lr.pBits, lr.Pitch, palette);
                    t->UnlockRect((D3DCUBEMAP_FACES)f, l);
                }
            }
        return t;
    }
    IDirect3DTexture9 *t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, 0, hf, D3DPOOL_MANAGED, &t, nullptr))) {
        logf("[tex] CreateTexture %ux%u x%u %s failed", w, h, levels, format_info(fmt).name);
        return nullptr;
    }
    for (uint32_t l = 0; l < levels; l++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0))) {
            upload_level(pc, 0, l, (uint8_t *)lr.pBits, lr.Pitch, palette);
            t->UnlockRect(l);
        }
    }
    return t;
}

// ---- Texture replacement ----
// A 2D texture is named by a hash of all its texel data and its size ("1a2b3c4d_64x64"). A PNG of that name in the
// port folder's textures\ replaces it (any size: texture coordinates are normalised); --dump-textures writes each
// uncompressed texture the game uploads to textures\dump\ under that name, to find the one to replace.
std::wstring textures_dir() { return (g_cfg.port_dir.empty() ? std::wstring(L".") : g_cfg.port_dir) + L"\\textures"; }

std::wstring texture_id(const PixelContainer *pc, uint32_t bytes)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)phys_to_virt(pc->Data);
    for (uint32_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 16777619u;
    h = (h ^ pc->Format) * 16777619u;
    uint32_t w, ht, d, l;
    pixel_container_dims(pc, &w, &ht, &d, &l);
    wchar_t s[48];
    swprintf(s, 48, L"%08x_%ux%u", h, w, ht);
    return s;
}

const std::set<std::wstring> &replacement_ids()
{
    static std::set<std::wstring> ids = [] {
        std::set<std::wstring> s;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((textures_dir() + L"\\*.png").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                s.insert(n.substr(0, n.size() - 4));
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        logf("[tex] %zu replacement textures in %s", s.size(), narrow(textures_dir()).c_str());
        return s;
    }();
    return ids;
}

IWICImagingFactory *wic()
{
    static IWICImagingFactory *f = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory *p = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p));
        return p;
    }();
    return f;
}

std::set<IDirect3DBaseTexture9 *> g_replaced; // host textures made from PNGs (draws force mipmapped sampling)

// The next mip level of a BGRA image: 2x2 boxes averaged with alpha weighting (no dark fringes at edges).
std::vector<uint32_t> half_size(const std::vector<uint32_t> &src, UINT w, UINT h, UINT nw, UINT nh)
{
    std::vector<uint32_t> dst((size_t)nw * nh);
    for (UINT y = 0; y < nh; y++)
        for (UINT x = 0; x < nw; x++) {
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (UINT dy = 0; dy < 2; dy++)
                for (UINT dx = 0; dx < 2; dx++) {
                    UINT sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                    uint32_t p = src[(size_t)sy * w + sx], pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xFF) * pa;
                    g += ((p >> 8) & 0xFF) * pa;
                    b += (p & 0xFF) * pa;
                    n++;
                }
            dst[(size_t)y * nw + x] = a ? (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
        }
    return dst;
}

// A host texture with a full mip chain from a BGRA image. A replacement is usually drawn smaller than it is (an HD
// version of a small texture): the mips keep thin features (outlines) from flickering away when it is minified.
IDirect3DTexture9 *mipmapped_texture(std::vector<uint32_t> px, UINT w, UINT h)
{
    IDirect3DTexture9 *t = nullptr;
    if (FAILED(host3d::dev()->CreateTexture(w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    UINT lw = w, lh = h;
    for (DWORD l = 0; l < t->GetLevelCount(); l++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0))) {
            for (UINT y = 0; y < lh; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, &px[(size_t)y * lw], lw * 4);
            t->UnlockRect(l);
        }
        UINT nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        px = half_size(px, lw, lh, nw, nh);
        lw = nw;
        lh = nh;
    }
    g_replaced.insert(t);
    return t;
}

// The HD reticle (black.ini hd_reticle): the HUD atlas (512x256: reticles, stance figures, ammo icons) at 4x its size
// from the game's own texels, with its centre dot (x 137-151, y 169-182) drawn anew: a white disc in a black ring,
// 10% smaller than the original's together. The game tints the reticle (reticle.cpp), so white takes its colours.
// Built from the player's own game data, so the release carries none of it.
const wchar_t kHudAtlas[] = L"4f787977_512x256"; // texture_id of the NTSC-U HUD atlas

IDirect3DBaseTexture9 *make_hd_reticle(Resource *r, Palette *pal)
{
    auto *pc = (PixelContainer *)r;
    constexpr UINT W = 512, H = 256, K = 4;
    std::vector<uint32_t> src((size_t)W * H);
    upload_level(pc, 0, 0, (uint8_t *)src.data(), W * 4, pal ? (const uint32_t *)phys_to_virt(pal->Data) : nullptr);
    std::vector<uint32_t> px((size_t)W * K * H * K);
    auto at = [&](int x, int y) { return src[(size_t)std::clamp(y, 0, (int)H - 1) * W + std::clamp(x, 0, (int)W - 1)]; };
    for (UINT y = 0; y < H * K; y++) // bilinear, alpha-weighted
        for (UINT x = 0; x < W * K; x++) {
            float fx = (x + 0.5f) / K - 0.5f, fy = (y + 0.5f) / K - 0.5f;
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float tx = fx - x0, ty = fy - y0;
            float a = 0, c[3] = {};
            uint32_t q[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
            float wgt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            for (int i = 0; i < 4; i++) {
                float qa = (q[i] >> 24) * wgt[i];
                a += qa;
                for (int ch = 0; ch < 3; ch++) c[ch] += ((q[i] >> (16 - 8 * ch)) & 0xFF) * qa;
            }
            px[(size_t)y * W * K + x] = a < 0.5f ? 0
                : (uint32_t)(a + 0.5f) << 24 | (uint32_t)(c[0] / a + 0.5f) << 16 | (uint32_t)(c[1] / a + 0.5f) << 8 | (uint32_t)(c[2] / a + 0.5f);
        }
    const float cx = 144.0f * K, cy = 175.5f * K, clear = 9.5f * K, ring = 4.95f * K, disc = 3.7f * K;
    for (int y = (int)(cy - clear); y <= (int)(cy + clear); y++)
        for (int x = (int)(cx - clear); x <= (int)(cx + clear); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            if (d > clear) continue;
            float cover = std::clamp(ring + 0.5f - d, 0.0f, 1.0f); // anti-aliased edges, one pixel wide
            float white = std::clamp(disc + 0.5f - d, 0.0f, 1.0f);
            uint32_t g = (uint32_t)(255 * (white / std::max(cover, 1e-3f)) + 0.5f);
            px[(size_t)y * W * K + x] = cover <= 0 ? 0 : (uint32_t)(255 * cover + 0.5f) << 24 | g << 16 | g << 8 | g;
        }
    return mipmapped_texture(std::move(px), W * K, H * K);
}

IDirect3DBaseTexture9 *load_replacement(const std::wstring &path)
{
    IWICImagingFactory *f = wic();
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    IDirect3DTexture9 *t = nullptr;
    UINT w = 0, h = 0;
    if (f && SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h))) {
        std::vector<uint32_t> px((size_t)w * h);
        conv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE *)px.data());
        t = mipmapped_texture(std::move(px), w, h);
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (!t) logf("[tex] could not load %s", narrow(path).c_str());
    return t;
}

void dump_texture(IDirect3DBaseTexture9 *base, const PixelContainer *pc, const std::wstring &id)
{
    static std::set<std::wstring> done;
    if (!base || base->GetType() != D3DRTYPE_TEXTURE || format_info(fmt_of(pc)).compressed || !done.insert(id).second) return;
    std::wstring dir = textures_dir() + L"\\dump";
    CreateDirectoryW(textures_dir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    auto *t = (IDirect3DTexture9 *)base;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    if (FAILED(t->GetLevelDesc(0, &desc)) || FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return;
    IWICImagingFactory *f = wic();
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    std::wstring path = dir + L"\\" + id + L".png";
    if (f && SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(desc.Width, desc.Height))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&pf);
        if (SUCCEEDED(fr->WritePixels(desc.Height, lr.Pitch, lr.Pitch * desc.Height, (BYTE *)lr.pBits))) {
            fr->Commit();
            enc->Commit();
        }
    }
    t->UnlockRect(0);
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
}

} // namespace

void pixel_container_dims(const PixelContainer *pc, uint32_t *w, uint32_t *h, uint32_t *d, uint32_t *levels)
{
    if (pc->Size) {
        *w = (pc->Size & SIZE_WIDTH_MASK) + 1;
        *h = ((pc->Size >> SIZE_HEIGHT_SHIFT) & 0xFFF) + 1;
        *d = 1;
        *levels = 1;
    } else {
        *w = 1u << ((pc->Format >> FMT_USIZE_SHIFT) & 0xF);
        *h = 1u << ((pc->Format >> FMT_VSIZE_SHIFT) & 0xF);
        *d = 1u << ((pc->Format >> FMT_PSIZE_SHIFT) & 0xF);
        *levels = (pc->Format >> FMT_MIPMAP_SHIFT) & 0xF;
        if (!*levels) *levels = 1;
    }
}

uint32_t texture_level_offset(const PixelContainer *pc, uint32_t face, uint32_t level, uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    uint32_t tw, th, td, levels;
    pixel_container_dims(pc, &tw, &th, &td, &levels);
    const FormatInfo &fi = format_info(fmt_of(pc));
    if (pc->Size) { // linear: one level
        *pitch = (((pc->Size >> SIZE_PITCH_SHIFT) & 0xFF) + 1) * 64;
        *w = tw;
        *h = th;
        return 0;
    }
    auto level_bytes = [&](uint32_t l, uint32_t *p, uint32_t *lw, uint32_t *lh) {
        uint32_t x = tw >> l, y = th >> l;
        if (!x) x = 1;
        if (!y) y = 1;
        *lw = x;
        *lh = y;
        if (fi.compressed) {
            uint32_t bw = (x + 3) / 4, bh = (y + 3) / 4;
            *p = bw * fi.bpp * 2;
            return *p * bh;
        }
        *p = x * fi.bpp / 8;
        return *p * y;
    };
    uint32_t off = 0, p, lw, lh;
    if (face) {
        uint32_t face_bytes = 0;
        for (uint32_t l = 0; l < levels; l++) face_bytes += level_bytes(l, &p, &lw, &lh);
        face_bytes = (face_bytes + 127) & ~127u; // cube faces are 128-byte aligned
        off = face * face_bytes;
    }
    for (uint32_t l = 0; l < level; l++) off += level_bytes(l, &p, &lw, &lh);
    level_bytes(level, pitch, w, h);
    return off;
}

IDirect3DBaseTexture9 *host_texture(Resource *r, Palette *pal)
{
    if (!r || !r->Data) return nullptr;
    perf::Scope perf_scope(perf::kTexture);
    auto *pc = (PixelContainer *)r;
    EnterCriticalSection(&g_cs);
    Entry &e = g_cache[r];
    uint32_t frame = g_frame_count.load(std::memory_order_relaxed);
    bool skip = e.checked_frame == frame || (e.stable >= kStableChecks && ((frame + ((uintptr_t)r >> 6)) & 15) != 0);
    if (skip && e.tex && !e.dirty && e.data == pc->Data && e.format == pc->Format && e.size == pc->Size &&
        e.palette == (pal ? phys_to_virt(pal->Data) : nullptr)) {
        IDirect3DBaseTexture9 *t = e.tex;
        LeaveCriticalSection(&g_cs);
        return t;
    }
    e.checked_frame = frame;
    uint32_t bytes = total_bytes(pc);
    uint32_t hash = sample_hash((const uint8_t *)phys_to_virt(pc->Data), bytes);
    const void *palptr = pal ? phys_to_virt(pal->Data) : nullptr;
    if (fmt_of(pc) == XFMT_P8 && pal) hash ^= sample_hash((const uint8_t *)palptr, 1024);
    bool same_shape = e.tex && !e.replaced && e.data == pc->Data && e.format == pc->Format && e.size == pc->Size;
    bool stale = !same_shape || e.dirty || e.hash != hash || (fmt_of(pc) == XFMT_P8 && e.palette != palptr);
    e.stable = stale ? 0 : std::min<uint32_t>(e.stable + 1, kStableChecks);
    if (stale && same_shape) { // contents changed (video frames, render-to-memory): refill in place
        refill(e.tex, r, pal);
        perf::add_texture_upload(bytes);
        e.palette = palptr;
        e.hash = hash;
        e.dirty = false;
        stale = false;
    }
    if (stale && e.replaced && e.data == pc->Data && e.format == pc->Format && e.size == pc->Size && e.hash == hash)
        stale = false; // a replaced texture whose game texels did not change keeps its PNG
    if (stale) {
        if (e.tex) {
            g_replaced.erase(e.tex);
            e.tex->Release();
        }
        e.tex = nullptr;
        e.replaced = false;
        bool flat = !(pc->Format & FMT_CUBEMAP) && ((pc->Format >> FMT_DIMENSION_SHIFT) & 0xF) != 3;
        if (flat && (g_cfg.dump_textures || g_cfg.hd_reticle || !replacement_ids().empty())) {
            std::wstring id = texture_id(pc, bytes);
            if (replacement_ids().count(id)) {
                e.tex = load_replacement(textures_dir() + L"\\" + id + L".png");
                e.replaced = e.tex != nullptr;
                if (e.replaced) logf("[tex] %s replaced from textures\\", narrow(id).c_str());
            } else if (g_cfg.hd_reticle && id == kHudAtlas) {
                e.tex = make_hd_reticle(r, pal);
                e.replaced = e.tex != nullptr;
                if (e.replaced) logf("[tex] HUD atlas rebuilt at 4x with the HD reticle");
            }
            if (!e.tex) {
                e.tex = create_and_upload(r, pal);
                if (g_cfg.dump_textures) dump_texture(e.tex, pc, id);
            }
        }
        if (!e.tex) e.tex = create_and_upload(r, pal);
        perf::add_texture_upload(bytes);
        e.data = pc->Data;
        e.format = pc->Format;
        e.size = pc->Size;
        e.palette = palptr;
        e.hash = hash;
        e.dirty = false;
        static std::atomic<int> uploads{0};
        if (++uploads <= 40 || g_trace_d3d) {
            uint32_t w, h, d, l;
            pixel_container_dims(pc, &w, &h, &d, &l);
            logf("[tex] upload %p: %ux%u x%u %s%s, data 0x%08x", r, w, h, l, format_info(fmt_of(pc)).name,
                 (pc->Format & FMT_CUBEMAP) ? " cube" : "", pc->Data);
        }
    }
    IDirect3DBaseTexture9 *t = e.tex;
    LeaveCriticalSection(&g_cs);
    return t;
}

bool texture_is_replaced(IDirect3DBaseTexture9 *t)
{
    EnterCriticalSection(&g_cs);
    bool r = g_replaced.count(t) != 0;
    LeaveCriticalSection(&g_cs);
    return r;
}

void texture_mark_dirty(const Resource *r)
{
    EnterCriticalSection(&g_cs);
    auto it = g_cache.find(r);
    if (it != g_cache.end()) it->second.dirty = true;
    LeaveCriticalSection(&g_cs);
}

void texture_forget(const Resource *r)
{
    EnterCriticalSection(&g_cs);
    auto it = g_cache.find(r);
    if (it != g_cache.end()) {
        if (it->second.tex) {
            g_replaced.erase(it->second.tex);
            it->second.tex->Release();
        }
        g_cache.erase(it);
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace xd3d
