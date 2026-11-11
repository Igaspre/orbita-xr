#include "capture.h"
#include <algorithm>
#include <cstdint>

namespace ox {

void ScreenCapture::open(ID3D11Device* dev, const LUID& devLuid, const std::wstring& gdi, bool mips) {
    release();
    dev_ = dev;
    luid_ = devLuid;
    gdi_ = gdi;
    mips_ = mips;
    refreshRect();
    init();
}

void ScreenCapture::release() {
    dupl_.reset();
    for (int i = 0; i < 2; i++) {
        staging_[i].reset();
        pending_[i] = false;
    }
    dupCtx_.reset();
    dupDev_.reset();
}

void ScreenCapture::refreshRect() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsExW(gdi_.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
        rect_ = {dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + (LONG)dm.dmPelsWidth,
                 dm.dmPosition.y + (LONG)dm.dmPelsHeight};
}

int ScreenCapture::width() const {
    if (!texW_) return std::max(1, (int)(rect_.right - rect_.left));
    return (rotation_ & 1) ? (int)texH_ : (int)texW_;
}

int ScreenCapture::height() const {
    if (!texH_) return std::max(1, (int)(rect_.bottom - rect_.top));
    return (rotation_ & 1) ? (int)texW_ : (int)texH_;
}

bool ScreenCapture::ensureTexture(UINT w, UINT h, DXGI_FORMAT fmt) {
    if (tex_ && w == texW_ && h == texH_ && fmt == texFmt_) return true;
    tex_.reset();
    srv_.reset();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips_ ? 0 : 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips_ ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = mips_ ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, tex_.put());
    if (FAILED(hr) && mips_) {
        mips_ = false;
        td.MipLevels = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        hr = dev_->CreateTexture2D(&td, nullptr, tex_.put());
    }
    if (FAILED(hr) || FAILED(dev_->CreateShaderResourceView(tex_.get(), nullptr, srv_.put()))) {
        err_ = "texture " + hex(hr);
        tex_.reset();
        return false;
    }
    texW_ = w;
    texH_ = h;
    texFmt_ = fmt;
    return true;
}

bool ScreenCapture::init() {
    release();
    retryAt_ = GetTickCount64() + 500;
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.put()))) return false;
    Com<IDXGIAdapter1> adapter;
    Com<IDXGIOutput> output;
    for (UINT a = 0; !output; a++) {
        Com<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; o++) {
            Com<IDXGIOutput> out;
            if (ad->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(out->GetDesc(&od)) && _wcsicmp(od.DeviceName, gdi_.c_str()) == 0) {
                adapter = ad;
                output = out;
                break;
            }
        }
    }
    if (!output) {
        err_ = "no DXGI output for " + narrow(gdi_);
        return false;
    }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    otherGpu_ = ad.AdapterLuid.LowPart != luid_.LowPart || ad.AdapterLuid.HighPart != luid_.HighPart;
    if (otherGpu_) {
        // duplication must run on the GPU that owns the output; copy through the CPU
        HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, dupDev_.put(), nullptr, dupCtx_.put());
        if (FAILED(hr)) {
            err_ = "D3D11CreateDevice " + hex(hr);
            return false;
        }
    } else {
        dupDev_ = Com<ID3D11Device>::addRef(dev_);
    }
    HRESULT hr = E_FAIL;
    Com<IDXGIOutput5> out5;
    if (SUCCEEDED(output.as(out5))) {
        DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;  // also converts HDR desktops
        hr = out5->DuplicateOutput1(dupDev_.get(), 0, 1, &fmt, dupl_.put());
    }
    if (FAILED(hr)) {
        Com<IDXGIOutput1> out1;
        if (SUCCEEDED(output.as(out1))) hr = out1->DuplicateOutput(dupDev_.get(), dupl_.put());
    }
    if (FAILED(hr)) {
        err_ = "DuplicateOutput " + hex(hr);
        dupl_.reset();
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    dupl_->GetDesc(&dd);
    rotation_ = dd.Rotation == DXGI_MODE_ROTATION_ROTATE90    ? 1
                : dd.Rotation == DXGI_MODE_ROTATION_ROTATE180 ? 2
                : dd.Rotation == DXGI_MODE_ROTATION_ROTATE270 ? 3
                                                              : 0;
    err_.clear();
    refreshRect();
    logf("capture %s %ux%u rot %d%s", narrow(gdi_).c_str(), dd.ModeDesc.Width, dd.ModeDesc.Height, rotation_,
         otherGpu_ ? " (other gpu)" : "");
    return true;
}

// Copies whichever staged frame the other GPU has finished, without waiting.
bool ScreenCapture::drainStaging(ID3D11DeviceContext* ctx) {
    bool changed = false;
    for (int k = 0; k < 2; k++) {
        int i = (nextStaging_ + k) % 2;  // oldest first
        if (!pending_[i] || !tex_) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = dupCtx_->Map(staging_[i].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        pending_[i] = false;
        if (FAILED(hr)) continue;
        ctx->UpdateSubresource(tex_.get(), 0, nullptr, m.pData, m.RowPitch, 0);
        dupCtx_->Unmap(staging_[i].get(), 0);
        changed = true;
    }
    if (changed) {
        if (mips_) ctx->GenerateMips(srv_.get());
        placeholder_ = false;
    }
    return changed;
}

bool ScreenCapture::update(ID3D11DeviceContext* ctx) {
    if (!dupl_) {
        if (GetTickCount64() >= retryAt_) init();
        if (!dupl_) return false;
    }
    bool changed = otherGpu_ && drainStaging(ctx);
    DXGI_OUTDUPL_FRAME_INFO fi;
    Com<IDXGIResource> res;
    HRESULT hr = dupl_->AcquireNextFrame(0, &fi, res.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return changed;
    if (FAILED(hr)) {
        // mode change, UAC secure desktop, ... just reopen in a moment
        err_ = "AcquireNextFrame " + hex(hr);
        release();
        return changed;
    }
    bool newImage = fi.LastPresentTime.QuadPart != 0 || fi.AccumulatedFrames > 0;
    Com<ID3D11Texture2D> frame;
    if ((newImage || !tex_ || placeholder_) && SUCCEEDED(res.as(frame))) {
        D3D11_TEXTURE2D_DESC fd;
        frame->GetDesc(&fd);
        if (ensureTexture(fd.Width, fd.Height, fd.Format)) {
            if (!otherGpu_) {
                ctx->CopySubresourceRegion(tex_.get(), 0, 0, 0, 0, frame.get(), 0, nullptr);
                if (mips_) ctx->GenerateMips(srv_.get());
                placeholder_ = false;
                changed = true;
            } else {
                int i = nextStaging_;
                if (!staging_[i]) {
                    D3D11_TEXTURE2D_DESC sd = fd;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    sd.MiscFlags = 0;
                    sd.MipLevels = 1;
                    dupDev_->CreateTexture2D(&sd, nullptr, staging_[i].put());
                }
                if (staging_[i]) {
                    dupCtx_->CopyResource(staging_[i].get(), frame.get());
                    dupCtx_->Flush();
                    pending_[i] = true;
                    nextStaging_ = 1 - i;
                }
            }
        }
    }
    dupl_->ReleaseFrame();
    return changed;
}

// Grid with a big number, shown until the real image arrives (and in demo mode).
void ScreenCapture::placeholder(ID3D11DeviceContext* ctx, int index, const wchar_t* text) {
    UINT w = (UINT)(rect_.right - rect_.left), h = (UINT)(rect_.bottom - rect_.top);
    if (w < 64 || h < 64) { w = 1920; h = 1080; }
    w = std::min(w, 4096u);
    h = std::min(h, 4096u);
    if (!ensureTexture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM)) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, bmp);

    static const COLORREF tint[6] = {RGB(30, 58, 92), RGB(24, 84, 76), RGB(88, 52, 96),
                                     RGB(104, 74, 30), RGB(52, 56, 108), RGB(30, 82, 100)};
    RECT all = {0, 0, (LONG)w, (LONG)h};
    HBRUSH br = CreateSolidBrush(tint[index % 6]);
    FillRect(dc, &all, br);
    DeleteObject(br);
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(170, 185, 205));
    HGDIOBJ op = SelectObject(dc, grid);
    for (UINT x = 0; x < w; x += 80) { MoveToEx(dc, (int)x, 0, nullptr); LineTo(dc, (int)x, (int)h); }
    for (UINT y = 0; y < h; y += 80) { MoveToEx(dc, 0, (int)y, nullptr); LineTo(dc, (int)w, (int)y); }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT big = CreateFontW(-(int)(h / 5), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-(int)(h / 24), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, big);
    std::wstring num = wfmt(L"%d", index + 1);
    RECT r = all;
    r.bottom = (LONG)(h * 0.62);
    DrawTextW(dc, num.c_str(), -1, &r, DT_CENTER | DT_SINGLELINE | DT_BOTTOM);
    SelectObject(dc, small);
    r = all;
    r.top = (LONG)(h * 0.66);
    DrawTextW(dc, text ? text : L"", -1, &r, DT_CENTER | DT_WORDBREAK);
    SelectObject(dc, of);
    SelectObject(dc, op);
    DeleteObject(big);
    DeleteObject(small);
    DeleteObject(grid);
    GdiFlush();
    auto* px = (uint32_t*)bits;
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xFF000000u;
    ctx->UpdateSubresource(tex_.get(), 0, nullptr, bits, w * 4, 0);
    if (mips_) ctx->GenerateMips(srv_.get());
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    rotation_ = 0;
    placeholder_ = true;
}

} // namespace ox
