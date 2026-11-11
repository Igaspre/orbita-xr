#pragma once
#include "util.h"
#include <d3d11.h>
#include <dxgi1_6.h>

namespace ox {

// One monitor captured with DXGI desktop duplication into a mipmapped texture
// on the device that draws to the glasses.
class ScreenCapture {
public:
    ~ScreenCapture() { release(); }
    void open(ID3D11Device* dev, const LUID& devLuid, const std::wstring& gdi, bool mips);
    bool update(ID3D11DeviceContext* ctx);  // true if the image changed
    void refreshRect();
    void placeholder(ID3D11DeviceContext* ctx, int index, const wchar_t* text);

    ID3D11ShaderResourceView* srv() const { return srv_.get(); }
    bool hasImage() const { return (bool)srv_; }
    bool live() const { return (bool)dupl_; }
    bool showingPlaceholder() const { return placeholder_; }
    int width() const;   // in desktop orientation
    int height() const;
    int rotation() const { return rotation_; }  // quarter turns
    RECT rect() const { return rect_; }
    const std::wstring& gdi() const { return gdi_; }
    const std::string& error() const { return err_; }
    bool otherGpu() const { return otherGpu_ && dupl_; }

private:
    bool init();
    void release();
    bool ensureTexture(UINT w, UINT h, DXGI_FORMAT fmt);
    bool drainStaging(ID3D11DeviceContext* ctx);

    ID3D11Device* dev_ = nullptr;
    LUID luid_{};
    std::wstring gdi_;
    bool mips_ = true;
    Com<ID3D11Device> dupDev_;
    Com<ID3D11DeviceContext> dupCtx_;
    Com<IDXGIOutputDuplication> dupl_;
    Com<ID3D11Texture2D> tex_;
    // screens on another GPU come over through the CPU; two staging textures so
    // we never sit waiting for that GPU, at the cost of one frame of delay
    Com<ID3D11Texture2D> staging_[2];
    bool pending_[2] = {};
    int nextStaging_ = 0;
    Com<ID3D11ShaderResourceView> srv_;
    UINT texW_ = 0, texH_ = 0;
    DXGI_FORMAT texFmt_ = DXGI_FORMAT_UNKNOWN;
    int rotation_ = 0;
    RECT rect_{};
    bool otherGpu_ = false;
    bool placeholder_ = false;
    ULONGLONG retryAt_ = 0;
    std::string err_;
};

} // namespace ox
