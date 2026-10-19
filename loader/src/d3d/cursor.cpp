// The menu cursor. While a menu page is up the Windows cursor is hidden (d3d/host.cpp, WM_SETCURSOR) and this arrow is
// drawn at the pointer's place instead: a pixel-art seta, black outline around a white body, so it reads on any menu
// screen. Same technique as the frame counter (d3d/fps.cpp): one small A8R8G8B8 texture built once and a textured quad
// drawn before the picture is presented, sized with the picture (the HUD's own scale) so the cursor keeps its size in
// the menus. The outline is not a second texture: each pixel next to a filled one is painted black.
#include "d3d/cursor.h"

#include "common.h"
#include "d3d/device.h"
#include "d3d/host.h"

#include <d3d9.h>

#include <atomic>
#include <cstring>

namespace {

// The arrow in its own pixels, tip at (0,0), 15 by 21: down the head's left edge, in at the notch, down the tail, out
// along its end and up its right side, then out to the head's right corner and back to the tip.
constexpr int kArrowW = 15, kArrowH = 21;
constexpr float kArrow[7][2] = {
    {0.5f, 0.5f},   {0.5f, 15.5f}, {6.0f, 12.5f},  {9.5f, 20.5f},
    {12.5f, 19.5f}, {8.5f, 11.0f}, {14.5f, 11.0f},
};

std::atomic<bool> g_ready{false};
IDirect3DTexture9 *g_tex = nullptr;

bool inside(float x, float y)
{
    bool in = false;
    for (int i = 0, j = 6; i < 7; j = i++)
        if (((kArrow[i][1] > y) != (kArrow[j][1] > y)) &&
            x < (kArrow[j][0] - kArrow[i][0]) * (y - kArrow[i][1]) / (kArrow[j][1] - kArrow[i][1]) + kArrow[i][0])
            in = !in;
    return in;
}

bool make_texture()
{
    if (g_tex) return true;
    IDirect3DDevice9 *dev = host3d::dev();
    if (!dev) return false;
    if (FAILED(dev->CreateTexture(kArrowW, kArrowH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
        g_tex = nullptr;
        return false;
    }
    uint32_t px[kArrowW * kArrowH];
    for (int y = 0; y < kArrowH; y++)
        for (int x = 0; x < kArrowW; x++) {
            const bool body = inside((float)x + 0.5f, (float)y + 0.5f);
            const bool edge = !body &&
                              (inside((float)x + 1.5f, (float)y + 0.5f) || inside((float)x - 0.5f, (float)y + 0.5f) ||
                               inside((float)x + 0.5f, (float)y + 1.5f) || inside((float)x + 0.5f, (float)y - 0.5f));
            px[y * kArrowW + x] = body ? 0xFFFFFFFFu : edge ? 0xF0000000u : 0x00000000u;
        }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < kArrowH; y++)
            memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, px + (size_t)y * kArrowW, kArrowW * 4);
        g_tex->UnlockRect(0);
    }
    g_ready = true;
    return true;
}

// sx, sy: the pointer in the stage's own 640x480. kx, ky: the stage into the frame the port renders. The arrow's size
// follows ky alone (one scale for both axes) so the cursor is not stretched with a widescreen picture.
void draw_quad(float sx, float sy, float kx, float ky)
{
    IDirect3DDevice9 *dev = host3d::dev();
    if (!dev || !g_tex) return;
    struct V {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    const float x0 = sx * kx - 0.5f, y0 = sy * ky - 0.5f; // the tip's own pixel sits on the pointer
    const float x1 = x0 + kArrowW * ky, y1 = y0 + kArrowH * ky;
    const V quad[4] = {{x0, y0, 0, 1, 0xFFFFFFFF, 0, 0},
                       {x1, y0, 0, 1, 0xFFFFFFFF, 1, 0},
                       {x0, y1, 0, 1, 0xFFFFFFFF, 0, 1},
                       {x1, y1, 0, 1, 0xFFFFFFFF, 1, 1}};
    // Save everything this quad touches (the frame counter's own list, plus the two sampler filters, which stay POINT
    // so the arrow's pixels stay square): the game's draws re-send their state only when the HLE cache sees a change,
    // so a leaked one would corrupt every draw after this.
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DBaseTexture9 *tex0 = nullptr;
    DWORD fvf = 0;
    DWORD rs[8], s0[4], s1[2], filt[2];
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
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &filt[0]);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &filt[1]);
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
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    for (int i = 0; i < 8; i++) dev->SetRenderState(rk[i], rs[i]);
    for (int i = 0; i < 4; i++) dev->SetTextureStageState(0, sk[i], s0[i]);
    for (int i = 0; i < 2; i++) dev->SetTextureStageState(1, sk[i * 2], s1[i]);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, filt[0]);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, filt[1]);
    dev->SetTexture(0, tex0);
    dev->SetFVF(fvf);
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    if (tex0) tex0->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
}

} // namespace

bool cursor_ready() { return g_ready.load(); }

void cursor_tick(int screen_w, int screen_h)
{
    float x = 0.0f, y = 0.0f;
    // The pointer in the stage's 640x480 (the space the menus' own rows are in). Drawn only while a menu page owns
    // the pointer (cursor_free: input/bindings.cpp menu_up); a point outside the picture clamps to its edge, so the
    // arrow is always there -- the Windows cursor never shows over the window (d3d/host.cpp WM_SETCURSOR).
    if (screen_w <= 1 || screen_h <= 1 || !host3d::cursor_free()) return;
    if (!host3d::cursor_stage_any(&x, &y)) return;
    if (!make_texture()) return;
    draw_quad(x, y, (float)screen_w / 640.0f, (float)screen_h / 480.0f);
}
