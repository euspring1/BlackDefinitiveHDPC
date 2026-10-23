// Movie subtitles drawn by the port (black.ini subtitles, --no-subtitles).
//
// The game has no subtitle support and .xmv (the Xbox movie container) has no public encoder, so the text cannot be
// baked into the movies. Instead the port draws it over the movie: the file is a plain .srt (the format every
// subtitle editor exports) named after the movie it belongs to, in the language's folder (lang.h; the BLACK-SUBTITLES
// hub's tracks, English for a movie the language has none of) or an enabled mod's subtitles\ folder:
//
//   mods\language\ptbr\subtitles\03_N.srt
//   mods\my_subtitles\subtitles\03_N.srt
//
// so a mod carries its own subtitles and turning the mod off turns them off. The movie player's state and file name
// are the ones the port already reads (input/movies.cpp): game object *0x2D45F0 + 0x2026C, name at +0x84, state at
// +0x1A8 (0x1C playing). Time is the real time since the movie started playing (the movie keeps to it); a movie
// that plays again starts the clock over.
#include "d3d/subtitles.h"

#include "common.h"
#include "d3d/device.h"
#include "d3d/host.h"
#include "lang.h"
#include "mods.h"

#include <d3d9.h>
#include <gdiplus.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr uint32_t kGameObject = 0x002D45F0, kPlayer = 0x2026C, kState = 0x1A8, kName = 0x84;
constexpr int kPlaying = 0x1C;

struct Line {
    double from, to; // seconds
    std::wstring text;
};
std::map<std::string, std::vector<Line>> g_subs; // movie name (lower case) -> lines
bool g_loaded;
uint32_t g_loaded_rev;

std::string g_movie;        // the movie being timed
uint32_t g_frames;          // frames it has been playing
bool g_was_playing;

IDirect3DTexture9 *g_tex = nullptr;
std::wstring g_tex_text;
int g_tex_w, g_tex_h;
ULONG_PTR g_gdiplus;

const uint8_t *player()
{
    uint32_t game = *(const uint32_t *)(uintptr_t)kGameObject;
    return game ? (const uint8_t *)(uintptr_t)(game + kPlayer) : nullptr;
}

std::wstring from_utf8(const std::string &s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string lower(const std::string &s)
{
    std::string o = s;
    for (char &c : o) c = (char)tolower((unsigned char)c);
    return o;
}

double parse_time(const char *s, const char *end)
{
    // HH:MM:SS,mmm (or .mmm), with the hour part optional
    int h = 0, m = 0, sec = 0, ms = 0;
    int n = sscanf_s(s, "%d:%d:%d", &h, &m, &sec);
    if (n == 3) {
        const char *p = strchr(s, ',');
        if (!p) p = strchr(s, '.');
        if (p && p < end) ms = atoi(p + 1);
        return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
    }
    m = 0;
    n = sscanf_s(s, "%d:%d", &m, &sec);
    if (n == 2) {
        const char *p = strchr(s, ',');
        if (!p) p = strchr(s, '.');
        if (p && p < end) ms = atoi(p + 1);
        return m * 60.0 + sec + ms / 1000.0;
    }
    return -1.0;
}

void load_srt(const std::wstring &path, std::vector<Line> &out)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    fclose(f);
    size_t i = 0;
    Line cur;
    bool have = false;
    while (i < data.size()) {
        size_t e = data.find('\n', i);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            if (have && !cur.text.empty()) out.push_back(cur);
            cur = Line{};
            cur.from = parse_time(line.c_str(), line.c_str() + arrow);
            cur.to = parse_time(line.c_str() + arrow + 3, line.c_str() + line.size());
            have = true;
            continue;
        }
        if (!have || line.empty()) continue;
        if (!cur.text.empty()) cur.text += L"\n";
        cur.text += from_utf8(line);
    }
    if (have && !cur.text.empty()) out.push_back(cur);
}

void load_dir(const std::wstring &dir, const std::string &tag)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.srt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        size_t dot = name.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        std::string movie = lower(narrow(name.substr(0, dot)));
        std::vector<Line> lines;
        load_srt(dir + L"\\" + name, lines);
        if (lines.empty()) continue;
        g_subs[movie] = std::move(lines);
        logf("subtitles: %s: %s (%zu lines)", tag.c_str(), narrow(name).c_str(), g_subs[movie].size());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Later sets replace earlier ones movie by movie: English, the language played (lang.h), then the enabled mods.
void load_all()
{
    g_loaded = true;
    g_loaded_rev = mods::revision();
    g_subs.clear();
    load_dir(lang::root(lang::kEnglish) + L"\\subtitles", "en");
    if (lang::current() != lang::kEnglish) load_dir(lang::root() + L"\\subtitles", lang::code(lang::current()));
    for (const mods::Mod &m : mods::list()) {
        if (!m.on || m.blocked) continue; // a subtitle file belongs to its mod: off is off
        load_dir(m.base + L"\\subtitles", m.folder);
    }
}

// The subtitle as a texture, drawn with GDI+ into a 32-bit bitmap (proper alpha for antialiased text).
bool make_texture(const std::wstring &text, int &w, int &h)
{
    if (g_tex && g_tex_text == text) {
        w = g_tex_w;
        h = g_tex_h;
        return true;
    }
    IDirect3DDevice9 *dev = host3d::dev();
    if (!dev) return false;
    if (!g_gdiplus) {
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr) != Gdiplus::Ok) return false;
    }
    const int size = 26; // px of the text; the outline goes outside it
    const int pad = 6;
    // measure
    {
        Gdiplus::Bitmap probe(8, 8, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&probe);
        Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::RectF box;
        int lines = 1;
        for (wchar_t c : text)
            if (c == L'\n') lines++;
        Gdiplus::RectF layout(0, 0, 4096, (Gdiplus::REAL)(size * 2 * lines));
        g.MeasureString(text.c_str(), (INT)text.size(), &font, layout, &box);
        w = (int)std::ceil(box.Width) + pad * 2;
        h = (int)std::ceil(box.Height) + pad * 2;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 512) return false;
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF layout((Gdiplus::REAL)pad, (Gdiplus::REAL)pad, (Gdiplus::REAL)(w - pad * 2), (Gdiplus::REAL)(h - pad * 2));
        Gdiplus::SolidBrush shadow(Gdiplus::Color(230, 0, 0, 0));
        for (int dx = -2; dx <= 2; dx++)
            for (int dy = -2; dy <= 2; dy++)
                if (dx || dy) {
                    Gdiplus::RectF o = layout;
                    o.X += (Gdiplus::REAL)dx;
                    o.Y += (Gdiplus::REAL)dy;
                    g.DrawString(text.c_str(), (INT)text.size(), &font, o, &fmt, &shadow);
                }
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
        g.DrawString(text.c_str(), (INT)text.size(), &font, layout, &fmt, &fill);
    }
    Gdiplus::Rect lock(0, 0, w, h);
    Gdiplus::BitmapData bits;
    if (bmp.LockBits(&lock, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bits) != Gdiplus::Ok) return false;
    if (g_tex) {
        g_tex->Release();
        g_tex = nullptr;
    }
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
        bmp.UnlockBits(&bits);
        g_tex = nullptr;
        return false;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < h; y++)
            memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (const uint8_t *)bits.Scan0 + (size_t)y * bits.Stride, (size_t)w * 4);
        g_tex->UnlockRect(0);
    }
    bmp.UnlockBits(&bits);
    g_tex_text = text;
    g_tex_w = w;
    g_tex_h = h;
    return true;
}

void draw_quad(int screen_w, int screen_h, int w, int h)
{
    IDirect3DDevice9 *dev = host3d::dev();
    if (!dev || !g_tex) return;
    struct V {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    const float scale = screen_h / 480.0f; // the HUD's own scale, so text keeps its size in the picture
    const float tw = w * scale, th = h * scale;
    const float x0 = (screen_w - tw) / 2.0f;
    const float y1 = screen_h - 24.0f * scale;
    const float x1 = x0 + tw, y0 = y1 - th;
    const V quad[4] = {{x0, y0, 0, 1, 0xFFFFFFFF, 0, 0},
                       {x1, y0, 0, 1, 0xFFFFFFFF, 1, 0},
                       {x0, y1, 0, 1, 0xFFFFFFFF, 0, 1},
                       {x1, y1, 0, 1, 0xFFFFFFFF, 1, 1}};
    // Save everything this quad touches: the game's draws re-send their state only when the HLE cache sees a
    // change, so a leaked state (no depth test, blending left on) would corrupt every draw after this one.
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DBaseTexture9 *tex0 = nullptr;
    DWORD fvf = 0;
    DWORD rs[8], s0[4], s1[2];
    const D3DRENDERSTATETYPE rk[8] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING, D3DRS_CULLMODE,
                                     D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE};
    const D3DTEXTURESTAGESTATETYPE sk[4] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1};
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    dev->GetFVF(&fvf);
    dev->GetTexture(0, &tex0);
    for (int i = 0; i < 8; i++) dev->GetRenderState(rk[i], &rs[i]);
    for (int i = 0; i < 4; i++) dev->GetTextureStageState(0, sk[i], &s0[i]);
    for (int i = 0; i < 2; i++) dev->GetTextureStageState(1, sk[i * 2], &s1[i]);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetTexture(0, g_tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    for (int i = 0; i < 8; i++) dev->SetRenderState(rk[i], rs[i]);
    for (int i = 0; i < 4; i++) dev->SetTextureStageState(0, sk[i], s0[i]);
    for (int i = 0; i < 2; i++) dev->SetTextureStageState(1, sk[i * 2], s1[i]);
    dev->SetTexture(0, tex0);
    dev->SetFVF(fvf);
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    if (tex0) tex0->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
}
} // namespace

void subtitles_tick(int screen_w, int screen_h)
{
    if (!g_cfg.subtitles || screen_w <= 1 || screen_h <= 1) return;
    if (!g_loaded || g_loaded_rev != mods::revision()) load_all(); // mods on/off: the .srt set changes with them
    const uint8_t *p = player();
    bool playing = p && *(const int32_t *)(p + kState) == kPlaying;
    if (!playing) {
        g_was_playing = false;
        if (g_tex) { // no movie: free the text
            g_tex->Release();
            g_tex = nullptr;
            g_tex_text.clear();
        }
        return;
    }
    std::string name = lower(std::string((const char *)p + kName));
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    static LONGLONG start;
    if (!g_was_playing || name != g_movie) { // a new movie (or the same one playing again): time from here
        g_movie = name;
        g_frames = 0;
        start = now.QuadPart;
    }
    g_was_playing = true;
    g_frames++;
    auto it = g_subs.find(g_movie);
    if (it == g_subs.end()) {
        static bool told;
        if (!told) {
            logf("subtitles: no .srt for movie '%s'", g_movie.c_str());
            told = true;
        }
        return;
    }
    double t = (double)(now.QuadPart - start) / (double)freq.QuadPart; // drawn frames are fewer under frameskip
    const std::wstring *text = nullptr;
    for (const Line &l : it->second)
        if (t >= l.from && t <= l.to) {
            text = &l.text;
            break;
        }
    if (!text) return;
    int w = 0, h = 0;
    if (!make_texture(*text, w, h)) return;
    draw_quad(screen_w, screen_h, w, h);
}

void subtitles_reload()
{
    g_subs.clear();
    g_loaded = false;
    if (g_tex) {
        g_tex->Release();
        g_tex = nullptr;
        g_tex_text.clear();
    }
}
