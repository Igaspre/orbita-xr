#pragma once
#include "util.h"
#include "../core/layout.h"
#include <d3d11.h>
#include <dxgi1_6.h>

namespace ox {

struct alignas(16) FrameConstants { float viewProj[16]; };
struct alignas(16) PanelConstants {
    float cursor[4];   // cursor rect in panel uv (x, y, w, h)
    float params[4];   // brightness, highlight, rotation, has cursor
    float accent[4];
    float border[4];   // border thickness in u and v
    float extra[4];    // sharpening amount
};
struct alignas(16) HudConstants { float rect[4]; float params[4]; };

struct Pipeline {
    Com<ID3D11VertexShader> panelVS, hudVS;
    Com<ID3D11PixelShader> panelPS, hudPS;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> frameCB, panelCB, hudCB;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11RasterizerState> raster;
    Com<ID3D11BlendState> alphaBlend;
    Com<ID3D11DepthStencilState> noDepth;

    bool init(ID3D11Device* dev, std::string& err);
    void bindPanels(ID3D11DeviceContext* ctx) const;
    void bindHud(ID3D11DeviceContext* ctx) const;
};

template <class T>
void upload(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const T& v) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &v, sizeof(T));
        ctx->Unmap(b, 0);
    }
}

struct Texture {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    bool set(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<uint32_t>& bgra, int W, int H);
};

// The system cursor, re-rendered whenever its shape changes.
struct CursorTexture {
    Texture tex;
    HCURSOR shape = (HCURSOR)-1;
    int w = 0, h = 0, hotX = 0, hotY = 0;
    bool update(ID3D11Device* dev, ID3D11DeviceContext* ctx, HCURSOR cur);
};

// Rounded text box (toasts, calibration steps), with an optional progress bar.
struct HudTexture {
    Texture tex;
    std::wstring text;
    float progress = -2;
    bool update(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& t, float prog, int screenW);
};

struct PanelMesh {
    Com<ID3D11Buffer> vb, ib;
    UINT indexCount = 0, vcap = 0, icap = 0;
    bool set(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Panel& p);
    void draw(ID3D11DeviceContext* ctx) const;
};

bool createDevice(IDXGIAdapter1* adapter, Com<ID3D11Device>& dev, Com<ID3D11DeviceContext>& ctx, std::string& err);
bool adapterForOutput(const std::wstring& gdi, Com<IDXGIAdapter1>& out);

// Compiles the shaders and draws one panel off screen. Returns a report.
std::string gpuSelfTest(bool& ok);

} // namespace ox
