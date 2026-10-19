// Frame rate counter drawn over the picture (black.ini fpscounter, --fpscounter, PC SETTINGS > DISPLAY).
// Same technique as the movie subtitles (d3d/subtitles.cpp): GDI+ renders the number into a texture once per
// displayed value, and a small textured quad draws it in the top left corner, before the picture is presented.
#include "d3d/fps.h"

#include "common.h"
#include "d3d/device.h"
#include "d3d/host.h"

#include <d3d9.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")

namespace {

IDirect3DTexture9 *g_tex = nullptr;
int g_shown = -1; // the fps value in the texture
int g_tex_w, g_tex_h;
ULONG_PTR g_gdiplus;
LARGE_INTEGER g_last;
double g_ema = 0.0; // smoothed frames per second

bool make_texture(int fps, int &w, int &h)
{
    if (g_tex && fps == g_shown) {
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
    char text[16];
    snprintf(text, sizeof text, "%d FPS", fps);
    std::wstring wtext(text, text + strlen(text));
    const int size = 13, pad = 4;
    {
        Gdiplus::Bitmap probe(8, 8, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&probe);
        Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::RectF box;
        g.MeasureString(wtext.c_str(), (INT)wtext.size(), &font, Gdiplus::RectF(0, 0, 4096, 256), &box);
        w = (int)std::ceil(box.Width) + pad * 2;
        h = (int)std::ceil(box.Height) + pad * 2;
    }
    if (w <= 0 || h <= 0 || w > 512 || h > 128) return false;
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF layout((Gdiplus::REAL)pad, (Gdiplus::REAL)pad, (Gdiplus::REAL)(w - pad * 2),
                              (Gdiplus::REAL)(h - pad * 2));
        Gdiplus::SolidBrush back(Gdiplus::Color(140, 0, 0, 0));
        g.FillRectangle(&back, 0, 0, w, h);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 140, 255, 140));
        g.DrawString(wtext.c_str(), (INT)wtext.size(), &font, layout, &fmt, &fill);
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
            memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (const uint8_t *)bits.Scan0 + (size_t)y * bits.Stride,
                   (size_t)w * 4);
        g_tex->UnlockRect(0);
    }
    bmp.UnlockBits(&bits);
    g_shown = fps;
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
    const float scale = screen_h / 480.0f; // the HUD's own scale, so the counter keeps its size in the picture
    const float tw = w * scale, th = h * scale, m = 6.0f * scale;
    const float x0 = m, y0 = m; // small, in the top left corner
    const float x1 = x0 + tw, y1 = y0 + th;
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

void fps_tick(int screen_w, int screen_h)
{
    if (!g_cfg.fpscounter || screen_w <= 1 || screen_h <= 1) {
        if (g_tex) {
            g_tex->Release();
            g_tex = nullptr;
            g_shown = -1;
        }
        g_last.QuadPart = 0;
        return;
    }
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    if (g_last.QuadPart) {
        double dt = (double)(now.QuadPart - g_last.QuadPart) / (double)f.QuadPart;
        if (dt > 0.0 && dt < 1.0) {
            double fps = 1.0 / dt;
            g_ema = g_ema > 0.0 ? g_ema + (fps - g_ema) * 0.1 : fps;
        }
    }
    g_last = now;
    if (g_ema <= 0.0) return;
    int w = 0, h = 0;
    if (!make_texture((int)(g_ema + 0.5), w, h)) return;
    draw_quad(screen_w, screen_h, w, h);
}
