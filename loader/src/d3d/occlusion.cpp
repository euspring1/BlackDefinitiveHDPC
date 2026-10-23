// AMBIENT OCCLUSION (PC SETTINGS > MODS): screen-space ambient occlusion from the scene's depth, SSAO or GTAO.
//
// Redone for the port's Direct3D 9 after BLACK-XBERecomp's (TGP482), which does it in Direct3D 12 at the same place
// of the same game: once a frame, after the world, the characters and the first-person models are drawn and before
// the alpha-blended list (leaves, glass, particles, decals, bloom, HUD) - the frame's draw of list 2 at 0x1EE999.
// From the depth buffer (an INTZ texture here, d3d/device.cpp) and the camera's projection (0x297720, row vectors)
// each pixel gets its view-space position; a pass works out how much of its surroundings hides it (SSAO: 16 samples
// in a disc, or GTAO: 3 slices of horizon search, Jimenez 2016 at XeGTAO's high preset), a 5-tap blur across and one
// down (depth edges kept) smooth the 4x4 noise, and the second blur multiplies the result into the scene's colour.
// The game's fog is already drawn by then: only the share of the colour under it is darkened, the fog's colour
// (the linear table fog its vertex programs give, oFog = min(clip z, c18.z), latched from the frame's draws) is not.
#include "d3d/occlusion.h"

#include "common.h"
#include "d3d/device.h"
#include "d3d/host.h"
#include "d3d/state.h"
#include "detour.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace occlusion {
namespace {

constexpr uint32_t kProjection = 0x00297720; // camera to screen, written by the camera's begin update
constexpr float kRadius = 0.8f;              // world units (metres)
constexpr float kMaxSpan = 0.15f;            // of the view's height: the most one pixel's samples may cover

struct Fog {
    float start = 0, end = 0, limit = 0;
    uint32_t colour = 0;
} g_fog;

IDirect3DPixelShader9 *g_ps[4]; // SSAO, GTAO, blur, apply
bool g_failed;
IDirect3DTexture9 *g_ao[2];
uint32_t g_ao_w, g_ao_h;

const char kShader[] = R"(
sampler2D depth : register(s0);
sampler2D ao : register(s1);
float4 proj : register(c0);     // 1 / scale x y, centre x y
float4 zp : register(c1);       // view z = x / (depth - y)
float4 vp : register(c2);       // viewport in pixels
float4 opt : register(c3);      // radius, most pixels it may span, blur step x y
float4 fog : register(c4);      // fogged share = min(saturate(x * view z + y), z)
float4 fogcolor : register(c5);
float4 texel : register(c6);    // 1 / target size

float Raw(float2 p) { return tex2Dlod(depth, float4((p + 0.5) * texel.xy, 0, 0)).r; }
float3 View(float2 px) {
    float2 p = clamp(floor(px), vp.xy, vp.xy + vp.zw - 1);
    float z = zp.x / (Raw(p) - zp.y);
    float2 ndc = (p + 0.5 - vp.xy) / vp.zw * float2(2, -2) + float2(-1, 1);
    return float3((ndc - proj.zw) * proj.xy * z, z);
}
float3 Normal(float2 pos, float3 c) {
    float3 l = View(pos - float2(1, 0));
    float3 r = View(pos + float2(1, 0));
    float3 u = View(pos - float2(0, 1));
    float3 d = View(pos + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
float Noise(float2 pos, float2 k) { return frac(52.9829189 * frac(dot(floor(pos), k))); }

float4 ssao(float2 vpos : VPOS) : COLOR0 {
    float2 pos = vpos + 0.5;
    float3 c = View(pos);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float4(1, c.z, 0, 0);
    float3 n = Normal(pos, c);
    float k = min(1, opt.y / span);
    float R = opt.x * k;
    float turn = Noise(pos, float2(0.06711056, 0.00583715)) * 6.2831853;
    float a = 0;
    [loop] for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16;
        float t = i * 2.3999632 + turn;
        float3 v = (View(pos + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v);
        float w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float4(max(1 - a * 0.25, 0), c.z, 0, 0);
}

static const float kHalfPi = 1.5707963;
float4 gtao(float2 vpos : VPOS) : COLOR0 {
    float2 pos = vpos + 0.5;
    float3 c = View(pos);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float4(1, c.z, 0, 0);
    float3 n = Normal(pos, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span);
    float R = opt.x * k;
    float slice_noise = Noise(pos, float2(0.06711056, 0.00583715));
    float step_noise = Noise(pos.yx, float2(0.06711056, 0.00583715));
    float vis = 0;
    [loop] for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        [loop] for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos + off) - c;
            float3 d1 = View(pos - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float4(pow(saturate(vis / 3), 2.2), c.z, 0, 0);
}

// 5 taps along opt.zw average the noise, depth edges left out
float Smooth(float2 pos, out float z) {
    float2 lo = vp.xy, hi = vp.xy + vp.zw - 1;
    float2 p = floor(pos);
    z = tex2Dlod(ao, float4((p + 0.5) * texel.xy, 0, 0)).g;
    float sum = 0, n = 0;
    [unroll] for (int i = -2; i <= 2; i++) {
        float2 q = clamp(p + i * opt.zw, lo, hi);
        float2 s = tex2Dlod(ao, float4((q + 0.5) * texel.xy, 0, 0)).rg;
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return sum / n;
}
float4 blur(float2 vpos : VPOS) : COLOR0 {
    float z;
    float a = Smooth(vpos + 0.5, z);
    return float4(a, z, 0, 0);
}
// blended as colour * a + rgb: only the scene's share under the fog is occluded
float4 apply(float2 vpos : VPOS) : COLOR0 {
    float z;
    float a = Smooth(vpos + 0.5, z);
    float fogged = min(saturate(fog.x * z + fog.y), fog.z);
    return float4(fogcolor.rgb * fogged * (1 - a), a);
}
)";

IDirect3DPixelShader9 *compile(const char *entry)
{
    IDirect3DPixelShader9 *ps = nullptr;
    ID3DBlob *code = nullptr, *errors = nullptr;
    if (SUCCEEDED(D3DCompile(kShader, sizeof kShader - 1, "occlusion", nullptr, nullptr, entry, "ps_3_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        host3d::dev()->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &ps);
    else
        logf("occlusion: %s: %s", entry, errors ? (const char *)errors->GetBufferPointer() : "?");
    if (code) code->Release();
    if (errors) errors->Release();
    return ps;
}

bool ready(uint32_t w, uint32_t h)
{
    if (g_failed) return false;
    if (!g_ps[0]) {
        const char *entries[4] = {"ssao", "gtao", "blur", "apply"};
        for (int i = 0; i < 4; i++)
            if (!(g_ps[i] = compile(entries[i]))) {
                g_failed = true;
                return false;
            }
    }
    if (g_ao[1] && g_ao_w == w && g_ao_h == h) return true;
    for (IDirect3DTexture9 *&t : g_ao) {
        if (t) t->Release();
        t = nullptr;
        if (FAILED(host3d::dev()->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &t, nullptr))) {
            logf("occlusion: no %ux%u G16R16F target; ambient occlusion off", w, h);
            g_failed = true;
            return false;
        }
    }
    g_ao_w = w;
    g_ao_h = h;
    logf("occlusion: %ux%u targets", w, h);
    return true;
}

void set_sampler(IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *t)
{
    d->SetTexture(s, t);
    d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}

// one full-target pass, scissored to the viewport
void pass(IDirect3DDevice9 *d, IDirect3DPixelShader9 *ps, IDirect3DSurface9 *target, const RECT &r, uint32_t w, uint32_t h)
{
    d->SetRenderTarget(0, target);
    d->SetPixelShader(ps);
    d->SetScissorRect(&r);
    D3DVIEWPORT9 v = {0, 0, w, h, 0.0f, 1.0f};
    d->SetViewport(&v);
    float x0 = -0.5f, y0 = -0.5f, x1 = w - 0.5f, y1 = h - 0.5f;
    struct V {
        float x, y, z, rhw;
    } q[4] = {{x0, y0, 0, 1}, {x1, y0, 0, 1}, {x0, y1, 0, 1}, {x1, y1, 0, 1}};
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

float channel(uint32_t argb, int shift) { return ((argb >> shift) & 0xFF) / 255.0f; }

void run(int mode)
{
    const float *m = (const float *)(uintptr_t)kProjection;
    bool perspective = m[11] == 1.0f && m[15] == 0.0f && m[0] != 0.0f && m[5] != 0.0f;
    xd3d::SceneTargets st;
    if (!perspective || !xd3d::scene_targets(st) || !ready(st.w, st.h)) return;
    IDirect3DDevice9 *d = host3d::dev();
    RECT r = {std::clamp((LONG)st.vp[0], 0L, (LONG)st.w), std::clamp((LONG)st.vp[1], 0L, (LONG)st.h),
              std::clamp((LONG)(st.vp[0] + st.vp[2]), 0L, (LONG)st.w), std::clamp((LONG)(st.vp[1] + st.vp[3]), 0L, (LONG)st.h)};
    if (r.right - r.left < 8 || r.bottom - r.top < 8) return;
    float zr = st.max_z - st.min_z;
    // fog over view depth z: clip z = m10 z + m14, fogged = (min(clip z, limit) - start) / (end - start)
    float range = g_fog.end - g_fog.start;
    float fog[4] = {0, 0, 0, 0};
    if (range > 0) {
        fog[0] = m[10] / range;
        fog[1] = (m[14] - g_fog.start) / range;
        fog[2] = std::clamp((g_fog.limit - g_fog.start) / range, 0.0f, 1.0f);
    }
    const float c[7][4] = {
        {1 / m[0], 1 / m[5], m[8], m[9]},
        {m[14] * zr, st.min_z + m[10] * zr, 0, 0},
        {(float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top)},
        {kRadius, kMaxSpan * (r.bottom - r.top), 1, 0},
        {fog[0], fog[1], fog[2], 0},
        {channel(g_fog.colour, 16), channel(g_fog.colour, 8), channel(g_fog.colour, 0), 0},
        {1.0f / st.w, 1.0f / st.h, 0, 0},
    };
    IDirect3DStateBlock9 *sb = nullptr;
    d->CreateStateBlock(D3DSBT_ALL, &sb);
    IDirect3DSurface9 *old_rt = nullptr, *old_z = nullptr, *scene = nullptr, *a0 = nullptr, *a1 = nullptr;
    d->GetRenderTarget(0, &old_rt);
    d->GetDepthStencilSurface(&old_z);
    st.color->GetSurfaceLevel(0, &scene);
    g_ao[0]->GetSurfaceLevel(0, &a0);
    g_ao[1]->GetSurfaceLevel(0, &a1);
    d->SetDepthStencilSurface(nullptr); // the depth is read now
    d->SetVertexShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW);
    for (D3DRENDERSTATETYPE rs : {D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_FOGENABLE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
                                  D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE})
        d->SetRenderState(rs, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetPixelShaderConstantF(0, &c[0][0], 7);
    set_sampler(d, 0, st.depth);
    set_sampler(d, 1, nullptr);
    host3d::begin_scene();
    pass(d, g_ps[mode == 2 ? 1 : 0], a0, r, st.w, st.h); // occlusion and view z
    set_sampler(d, 0, nullptr);
    set_sampler(d, 1, g_ao[0]);
    pass(d, g_ps[2], a1, r, st.w, st.h); // across
    const float down[4] = {kRadius, kMaxSpan * (r.bottom - r.top), 0, 1};
    d->SetPixelShaderConstantF(3, down, 1);
    set_sampler(d, 1, g_ao[1]);
    // into the scene: colour * a + fog's share; its alpha (which the game's blends read) stays
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    d->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    pass(d, g_ps[3], scene, r, st.w, st.h); // down, applied
    set_sampler(d, 1, nullptr);
    d->SetRenderTarget(0, old_rt);
    d->SetDepthStencilSurface(old_z);
    if (sb) {
        sb->Apply();
        sb->Release();
    }
    xd3d::forget_host_state();
    for (IDirect3DSurface9 *s : {old_rt, old_z, scene, a0, a1})
        if (s) s->Release();
    static bool told;
    if (!told) {
        told = true;
        logf("occlusion: %s at %ux%u, viewport %ld,%ld %ldx%ld, fog %g..%g", mode == 2 ? "GTAO" : "SSAO", st.w, st.h, r.left,
             r.top, r.right - r.left, r.bottom - r.top, g_fog.start, g_fog.end);
    }
}

bool __cdecl before_alpha(HookRegs *, HookFx *)
{
    if (g_cfg.ambient_occlusion) run(g_cfg.ambient_occlusion);
    return true;
}

} // namespace

void note_draw()
{
    const uint32_t *rs = xd3d::render_state();
    if (!rs[xd3d::XRS_FOGENABLE] || rs[xd3d::XRS_FOGTABLEMODE] != 3) return; // linear table fog
    memcpy(&g_fog.start, &rs[xd3d::XRS_FOGSTART], 4);
    memcpy(&g_fog.end, &rs[xd3d::XRS_FOGEND], 4);
    g_fog.limit = xd3d::g_st.vs_constants[96 + 18][2];
    g_fog.colour = rs[xd3d::XRS_FOGCOLOR];
}

void install()
{
    // mov esi, [0x2D67F4]; add esi, 0xCA9C (the display's draw list 2, drawn next)
    if (hook_install("occlusion", {{0x001EE999, {0x8B, 0x35, 0xF4, 0x67, 0x2D, 0x00, 0x81, 0xC6, 0x9C, 0xCA, 0x00, 0x00}, before_alpha}}))
        logf("occlusion: before the alpha-blended list (ambient occlusion %s)",
             g_cfg.ambient_occlusion == 2 ? "GTAO" : g_cfg.ambient_occlusion ? "SSAO" : "off");
}

void release()
{
    for (IDirect3DTexture9 *&t : g_ao) {
        if (t) t->Release();
        t = nullptr;
    }
    g_ao_w = g_ao_h = 0;
}

} // namespace occlusion
