#include "gpu.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ox {

static const char* kShaders = R"(
cbuffer Frame : register(b0) { row_major float4x4 viewProj; };
cbuffer PanelData : register(b1) { float4 cursor; float4 params; float4 accent; float4 border; float4 extra; };
cbuffer Hud : register(b2) { float4 hudRect; float4 hudParams; };
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState samp : register(s0);

struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

PSIn panelVS(VSIn i) {
    PSIn o;
    o.pos = mul(float4(i.pos, 1.0), viewProj);
    o.uv = i.uv;
    return o;
}

// desktop duplication hands rotated screens over unrotated
float2 unrotate(float2 uv, float r) {
    if (r < 0.5) return uv;
    if (r < 1.5) return float2(uv.y, 1.0 - uv.x);
    if (r < 2.5) return float2(1.0 - uv.x, 1.0 - uv.y);
    return float2(1.0 - uv.y, uv.x);
}

float4 panelPS(PSIn i) : SV_Target {
    float2 uv = unrotate(i.uv, params.z);
    float3 c = tex0.Sample(samp, uv).rgb;
    // Unsharp mask one glasses pixel wide: the desktop gets resampled on its way
    // to the glasses, and this wins back some of the edge contrast text needs.
    float2 dx = ddx(uv), dy = ddy(uv);
    float3 n = tex0.Sample(samp, uv + dx).rgb + tex0.Sample(samp, uv - dx).rgb +
               tex0.Sample(samp, uv + dy).rgb + tex0.Sample(samp, uv - dy).rgb;
    c = saturate(c + (c - n * 0.25) * extra.x);
    if (params.w > 0.5) {
        float2 cu = (i.uv - cursor.xy) / cursor.zw;
        if (all(cu >= 0.0) && all(cu <= 1.0)) {
            float4 k = tex1.Sample(samp, cu);
            c = lerp(c, k.rgb, k.a);
        }
    }
    c *= params.x;
    float2 d = min(i.uv, 1.0 - i.uv);
    if (params.y > 0.5 && (d.x < border.x || d.y < border.y)) c = lerp(c, accent.rgb, accent.a);
    return float4(c, 1.0);
}

PSIn hudVS(uint id : SV_VertexID) {
    float2 t = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    PSIn o;
    o.pos = float4(lerp(hudRect.x, hudRect.z, t.x), lerp(hudRect.y, hudRect.w, t.y), 0.0, 1.0);
    o.uv = t;
    return o;
}

float4 hudPS(PSIn i) : SV_Target {
    float4 k = tex0.Sample(samp, i.uv);
    return float4(k.rgb, k.a * hudParams.x);
}
)";

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

// d3dcompiler_47.dll ships with Windows 8.1 and later (x64 and ARM64)
static bool compile(const char* entry, const char* target, Com<ID3DBlob>& out, std::string& err) {
    static HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto fn = dll ? (D3DCompileFn)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!fn) {
        err = "d3dcompiler_47.dll not found";
        return false;
    }
    Com<ID3DBlob> errors;
    HRESULT hr = fn(kShaders, strlen(kShaders), "shaders.hlsl", nullptr, nullptr, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.put(), errors.put());
    if (FAILED(hr)) {
        err = std::string(entry) + ": " + hex(hr);
        if (errors) err += std::string(" ") + (const char*)errors->GetBufferPointer();
        return false;
    }
    return true;
}

bool Pipeline::init(ID3D11Device* dev, std::string& err) {
    Com<ID3DBlob> vs, ps, hvs, hps;
    if (!compile("panelVS", "vs_4_0", vs, err) || !compile("panelPS", "ps_4_0", ps, err) ||
        !compile("hudVS", "vs_4_0", hvs, err) || !compile("hudPS", "ps_4_0", hps, err))
        return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, panelVS.put());
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, panelPS.put());
    dev->CreateVertexShader(hvs->GetBufferPointer(), hvs->GetBufferSize(), nullptr, hudVS.put());
    dev->CreatePixelShader(hps->GetBufferPointer(), hps->GetBufferSize(), nullptr, hudPS.put());
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(elems, 2, vs->GetBufferPointer(), vs->GetBufferSize(), layout.put());
    if (!panelVS || !panelPS || !hudVS || !hudPS || !layout) {
        err = "creating shaders failed";
        return false;
    }
    auto cb = [&](UINT size, Com<ID3D11Buffer>& b) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = (size + 15) & ~15u;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&d, nullptr, b.put()));
    };
    if (!cb(sizeof(FrameConstants), frameCB) || !cb(sizeof(PanelConstants), panelCB) || !cb(sizeof(HudConstants), hudCB)) {
        err = "constant buffers";
        return false;
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, sampler.put()))) {
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        dev->CreateSamplerState(&sd, sampler.put());
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, raster.put());
    D3D11_BLEND_DESC bd = {};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, alphaBlend.put());
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, noDepth.put());
    if (!sampler || !raster || !alphaBlend || !noDepth) {
        err = "pipeline states";
        return false;
    }
    return true;
}

void Pipeline::bindPanels(ID3D11DeviceContext* ctx) const {
    ctx->RSSetState(raster.get());
    ctx->OMSetDepthStencilState(noDepth.get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->IASetInputLayout(layout.get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(panelVS.get(), nullptr, 0);
    ctx->PSSetShader(panelPS.get(), nullptr, 0);
    ID3D11Buffer* cbs[3] = {frameCB.get(), panelCB.get(), hudCB.get()};
    ctx->VSSetConstantBuffers(0, 3, cbs);
    ctx->PSSetConstantBuffers(0, 3, cbs);
    ID3D11SamplerState* s = sampler.get();
    ctx->PSSetSamplers(0, 1, &s);
}

void Pipeline::bindHud(ID3D11DeviceContext* ctx) const {
    ctx->OMSetBlendState(alphaBlend.get(), nullptr, 0xffffffff);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(hudVS.get(), nullptr, 0);
    ctx->PSSetShader(hudPS.get(), nullptr, 0);
}

bool Texture::set(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<uint32_t>& bgra, int W, int H) {
    if (!tex || W != w || H != h) {
        tex.reset();
        srv.reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)W;
        td.Height = (UINT)H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, tex.put())) ||
            FAILED(dev->CreateShaderResourceView(tex.get(), nullptr, srv.put())))
            return false;
        w = W;
        h = H;
    }
    ctx->UpdateSubresource(tex.get(), 0, nullptr, bgra.data(), (UINT)W * 4, 0);
    return true;
}

namespace {
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* px = nullptr;
    int w = 0, h = 0;
    bool create(int W, int H, uint32_t fill) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (!bmp) return false;
        old = SelectObject(dc, bmp);
        w = W;
        h = H;
        std::fill(px, px + (size_t)W * H, fill);
        return true;
    }
    ~Dib() {
        if (!dc) return;
        if (old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
    }
};
} // namespace

bool CursorTexture::update(ID3D11Device* dev, ID3D11DeviceContext* ctx, HCURSOR cur) {
    if (cur == shape) return (bool)tex.srv;
    shape = cur;
    if (!cur) return false;
    ICONINFO ii = {};
    if (!GetIconInfo(cur, &ii)) return false;
    BITMAP bm = {};
    int W = 32, H = 32;
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) { W = bm.bmWidth; H = bm.bmHeight; }
    else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bm), &bm)) { W = bm.bmWidth; H = bm.bmHeight / 2; }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    hotX = (int)ii.xHotspot;
    hotY = (int)ii.yHotspot;
    W = std::clamp(W, 8, 256);
    H = std::clamp(H, 8, 256);

    // Draw it on black and on white; the difference gives us alpha. Pixels that
    // invert the background (text cursor) come out "negative" and get special care.
    Dib black, white;
    if (!black.create(W, H, 0xFF000000u) || !white.create(W, H, 0xFFFFFFFFu)) return false;
    DrawIconEx(black.dc, 0, 0, cur, W, H, 0, nullptr, DI_NORMAL);
    DrawIconEx(white.dc, 0, 0, cur, W, H, 0, nullptr, DI_NORMAL);
    GdiFlush();
    std::vector<uint32_t> out((size_t)W * H, 0);
    for (int i = 0; i < W * H; i++) {
        uint32_t b = black.px[i], wv = white.px[i];
        int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
        int dr = (int)((wv >> 16) & 255) - br, dg = (int)((wv >> 8) & 255) - bg, db = (int)(wv & 255) - bb;
        if (dr < 0 || dg < 0 || db < 0) {
            out[i] = 0xFFFFFFFFu;
            continue;
        }
        int a = 255 - std::max(dr, std::max(dg, db));
        if (a <= 0) continue;
        uint32_t r = (uint32_t)std::min(255, br * 255 / a), g = (uint32_t)std::min(255, bg * 255 / a),
                 bl = (uint32_t)std::min(255, bb * 255 / a);
        out[i] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | bl;
    }
    // dark outline around inverted pixels so the I-beam shows on white pages
    std::vector<uint32_t> outlined = out;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (out[(size_t)y * W + x] >> 24) continue;
            for (int k = 0; k < 9; k++) {
                int xx = x + k % 3 - 1, yy = y + k / 3 - 1;
                if (xx >= 0 && yy >= 0 && xx < W && yy < H && out[(size_t)yy * W + xx] == 0xFFFFFFFFu) {
                    outlined[(size_t)y * W + x] = 0xC0000000u;
                    break;
                }
            }
        }
    w = W;
    h = H;
    return tex.set(dev, ctx, outlined, W, H);
}

bool HudTexture::update(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& t, float prog, int screenW) {
    if (t == text && std::fabs(prog - progress) < 0.01f && tex.srv) return true;
    text = t;
    progress = prog;
    int W = std::clamp(screenW * 6 / 10, 200, 1400);
    int fontPx = std::max(18, screenW / 48);
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                             L"Segoe UI");
    HDC mdc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(mdc, font);
    RECT mr = {0, 0, W - fontPx * 2, 0};
    DrawTextW(mdc, t.c_str(), -1, &mr, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(mdc, of);
    DeleteDC(mdc);
    int barH = prog >= 0 ? fontPx * 3 / 2 : 0;
    int H = std::max((int)(mr.bottom - mr.top) + fontPx + barH, fontPx * 2);

    Dib d;
    if (!d.create(W, H, 0xFF000000u)) { DeleteObject(font); return false; }
    of = SelectObject(d.dc, font);
    SetBkMode(d.dc, TRANSPARENT);
    SetTextColor(d.dc, RGB(255, 255, 255));
    RECT tr = {fontPx, fontPx / 2, W - fontPx, H - barH};
    DrawTextW(d.dc, t.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK);
    SelectObject(d.dc, of);
    DeleteObject(font);
    GdiFlush();

    std::vector<uint32_t> px((size_t)W * H, 0);
    float rad = fontPx * 0.7f;
    int bx0 = fontPx * 2, bx1 = W - fontPx * 2, by0 = H - barH + fontPx / 4, by1 = by0 + fontPx / 2;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float cx = std::max(0.0f, std::max(rad - x, x - (W - 1 - rad)));
            float cy = std::max(0.0f, std::max(rad - y, y - (H - 1 - rad)));
            float cover = std::clamp(rad - std::sqrt(cx * cx + cy * cy) + 0.5f, 0.0f, 1.0f);
            if (cover <= 0) continue;
            float lum = (float)(d.px[(size_t)y * W + x] & 255) / 255.0f;
            float r = 18 + 237 * lum, g = 24 + 231 * lum, b = 32 + 223 * lum;
            if (barH && y >= by0 && y < by1 && x >= bx0 && x < bx1) {
                bool filled = (float)(x - bx0) / (float)std::max(1, bx1 - bx0) <= prog;
                r = filled ? 47 : 58;
                g = filled ? 220 : 64;
                b = filled ? 195 : 78;
            }
            px[(size_t)y * W + x] = ((uint32_t)(0.88f * cover * 255) << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    return tex.set(dev, ctx, px, W, H);
}

bool PanelMesh::set(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Panel& p) {
    UINT nv = (UINT)p.verts.size(), ni = (UINT)p.indices.size();
    auto ensure = [&](Com<ID3D11Buffer>& b, UINT& cap, UINT need, UINT stride, UINT bind) {
        if (b && need <= cap) return true;
        b.reset();
        cap = std::max<UINT>(need, 512);
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = cap * stride;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = bind;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&d, nullptr, b.put()));
    };
    if (!ensure(vb, vcap, nv, sizeof(PanelVertex), D3D11_BIND_VERTEX_BUFFER) ||
        !ensure(ib, icap, ni, sizeof(uint16_t), D3D11_BIND_INDEX_BUFFER))
        return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(vb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, p.verts.data(), nv * sizeof(PanelVertex));
    ctx->Unmap(vb.get(), 0);
    if (FAILED(ctx->Map(ib.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, p.indices.data(), ni * sizeof(uint16_t));
    ctx->Unmap(ib.get(), 0);
    indexCount = ni;
    return true;
}

void PanelMesh::draw(ID3D11DeviceContext* ctx) const {
    UINT stride = sizeof(PanelVertex), offset = 0;
    ID3D11Buffer* b = vb.get();
    ctx->IASetVertexBuffers(0, 1, &b, &stride, &offset);
    ctx->IASetIndexBuffer(ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->DrawIndexed(indexCount, 0, 0);
}

bool createDevice(IDXGIAdapter1* adapter, Com<ID3D11Device>& dev, Com<ID3D11DeviceContext>& ctx, std::string& err) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, 4, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
    if (hr == E_INVALIDARG)  // no 11_1 runtime
        hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
    if (FAILED(hr) && !adapter)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, dev.put(),
                               nullptr, ctx.put());
    if (FAILED(hr)) err = "D3D11CreateDevice " + hex(hr);
    return SUCCEEDED(hr);
}

bool adapterForOutput(const std::wstring& gdi, Com<IDXGIAdapter1>& out) {
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.put()))) return false;
    for (UINT a = 0;; a++) {
        Com<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) return false;
        for (UINT o = 0;; o++) {
            Com<IDXGIOutput> output;
            if (ad->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(output->GetDesc(&od)) && _wcsicmp(od.DeviceName, gdi.c_str()) == 0) {
                out = ad;
                return true;
            }
        }
    }
}

std::string gpuSelfTest(bool& ok) {
    ok = false;
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    std::string err;
    Pipeline p;
    if (!createDevice(nullptr, dev, ctx, err)) return "FAIL  direct3d device: " + err + "\n";
    if (!p.init(dev.get(), err)) return "FAIL  shaders: " + err + "\n";
    std::string report = "ok    shaders\n";

    // one red panel in front of the camera: red in the middle, black in the corner
    const UINT W = 256, H = 160;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> target, readback;
    Com<ID3D11RenderTargetView> rtv;
    dev->CreateTexture2D(&td, nullptr, target.put());
    dev->CreateRenderTargetView(target.get(), nullptr, rtv.put());
    Texture red;
    red.set(dev.get(), ctx.get(), std::vector<uint32_t>(64 * 64, 0xFFFF0000u), 64, 64);
    LayoutParams lp;
    lp.panelWidthDeg = 40;
    PanelMesh mesh;
    mesh.set(dev.get(), ctx.get(), computeLayout({16.0f / 9.0f}, lp)[0]);
    FrameConstants fc;
    Mat4 vp = Mat4::perspectiveRH(deg2rad(30), (float)W / H, 0.05f, 100);
    memcpy(fc.viewProj, vp.m, sizeof(fc.viewProj));
    upload(ctx.get(), p.frameCB.get(), fc);
    PanelConstants pc = {};
    pc.params[0] = 1;
    upload(ctx.get(), p.panelCB.get(), pc);
    ID3D11RenderTargetView* r = rtv.get();
    ctx->OMSetRenderTargets(1, &r, nullptr);
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(r, black);
    D3D11_VIEWPORT v = {0, 0, (float)W, (float)H, 0, 1};
    ctx->RSSetViewports(1, &v);
    p.bindPanels(ctx.get());
    ID3D11ShaderResourceView* s = red.srv.get();
    ctx->PSSetShaderResources(0, 1, &s);
    mesh.draw(ctx.get());

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, readback.put());
    ctx->CopyResource(readback.get(), target.get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(readback.get(), 0, D3D11_MAP_READ, 0, &m))) return report + "FAIL  readback\n";
    auto at = [&](UINT x, UINT y) { return ((const uint32_t*)((const uint8_t*)m.pData + y * m.RowPitch))[x]; };
    uint32_t center = at(W / 2, H / 2), corner = at(2, 2);
    ctx->Unmap(readback.get(), 0);
    ok = ((center >> 16) & 255) > 200 && ((center >> 8) & 255) < 40 && (corner & 0xFFFFFF) == 0;
    char b[96];
    std::snprintf(b, sizeof(b), "%s  draw test (center %08X, corner %08X)\n", ok ? "ok  " : "FAIL", center, corner);
    return report + b;
}

} // namespace ox
