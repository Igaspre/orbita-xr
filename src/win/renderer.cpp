#include "renderer.h"
#include "capture.h"
#include "gpu.h"
#include "strings.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace ox {

namespace {

std::atomic<bool>* g_closeDemo = nullptr;
long g_rawDown = 0;  // mouse movement downwards since the last frame, raw counts (render thread only)

LRESULT CALLBACK viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool demo = GetWindowLongPtrW(h, GWLP_USERDATA) != 0;
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (!demo && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_INPUT: {
        // Raw mouse input keeps coming when the pointer is stuck against an edge,
        // which is how we tell "pushing down" from "resting at the bottom".
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            g_rawDown += ri.data.mouse.lLastY;
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_closeDemo) *g_closeDemo = true;
        break;
    case WM_CLOSE:
        if (g_closeDemo) *g_closeDemo = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND createWindow(const RenderSetup& s) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = viewProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"OrbitaXRView";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    if (s.demo) {
        RECT r = {0, 0, 1280, 800};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND h = CreateWindowExW(0, wc.lpszClassName, tr(L"Orbita XR (Esc para cerrar)", L"Orbita XR (Esc to close)"),
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60,
                                 60, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
        if (h) SetWindowLongPtrW(h, GWLP_USERDATA, 1);
        return h;
    }
    const RECT& g = s.glassesRect;
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Orbita XR", WS_POPUP,
                             g.left, g.top, g.right - g.left, g.bottom - g.top, nullptr, nullptr, hi, nullptr);
    if (h) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_INPUTSINK, h};  // generic desktop, mouse
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) logf("render: no raw mouse input (%lu)", GetLastError());
    }
    return h;
}

Com<IDXGISwapChain> createSwapChain(ID3D11Device* dev, HWND hwnd, UINT w, UINT h, std::string& err) {
    Com<IDXGIDevice> dxgiDev;
    Com<IDXGIAdapter> adapter;
    Com<IDXGISwapChain> swap;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgiDev.put());
    if (!dxgiDev || FAILED(dxgiDev->GetAdapter(adapter.put()))) return swap;
    Com<IDXGIFactory2> f2;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)f2.put());
    HRESULT hr = E_FAIL;
    if (f2) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        Com<IDXGISwapChain1> s1;
        hr = f2->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, s1.put());
        if (FAILED(hr)) {
            d.Flags = 0;
            hr = f2->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, s1.put());
        }
        if (SUCCEEDED(hr)) s1.as(swap);
        f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    if (FAILED(hr)) {  // pre flip-model fallback
        Com<IDXGIFactory> f;
        adapter->GetParent(__uuidof(IDXGIFactory), (void**)f.put());
        DXGI_SWAP_CHAIN_DESC d = {};
        d.BufferDesc.Width = w;
        d.BufferDesc.Height = h;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 1;
        d.OutputWindow = hwnd;
        d.Windowed = TRUE;
        hr = f ? f->CreateSwapChain(dev, &d, swap.put()) : E_FAIL;
    }
    if (FAILED(hr)) err = "swap chain " + hex(hr);
    return swap;
}

void saveBmp(const std::wstring& path, const uint8_t* data, UINT pitch, int W, int H) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    BITMAPINFOHEADER ih = {sizeof(ih)};
    ih.biWidth = W;
    ih.biHeight = -H;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)(W * H * 4);
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = 0; y < H; y++) fwrite(data + (size_t)y * pitch, 4, (size_t)W, f);
    fclose(f);
}

void centerCursorOn(const RECT& r) { SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2); }

} // namespace

bool Renderer::start(const RenderSetup& setup, const Settings& s, ImuClient* imu) {
    stop();
    setup_ = setup;
    imu_ = imu;
    {
        std::lock_guard<std::mutex> lk(m_);
        settings_ = s;
        settingsChanged_ = true;
        commands_.clear();
        stats_ = {};
    }
    stop_ = false;
    finished_ = false;
    started_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    WaitForSingleObject(started_, 15000);
    CloseHandle(started_);
    started_ = nullptr;
    return running_;
}

void Renderer::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void Renderer::apply(const Settings& s) {
    std::lock_guard<std::mutex> lk(m_);
    settings_ = s;
    settingsChanged_ = true;
}

void Renderer::send(Command c) {
    std::lock_guard<std::mutex> lk(m_);
    commands_.push_back(c);
}

void Renderer::toast(const std::wstring& text, float seconds) {
    std::lock_guard<std::mutex> lk(m_);
    toast_ = text;
    toastSeconds_ = seconds;
    toastPending_ = true;
}

RenderStats Renderer::stats() {
    std::lock_guard<std::mutex> lk(m_);
    return stats_;
}

std::string Renderer::error() {
    std::lock_guard<std::mutex> lk(m_);
    return stats_.error;
}

void Renderer::run() {
    auto fail = [&](const std::string& e) {
        logf("render: %s", e.c_str());
        {
            std::lock_guard<std::mutex> lk(m_);
            stats_.error = e;
        }
        running_ = false;
        finished_ = true;
        if (started_) SetEvent(started_);
    };
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Draw on the GPU that already holds the screens we copy from. On laptops with
    // two GPUs the virtual screens usually live on the big one while the USB-C port
    // hangs off the other; copying one finished frame to the glasses is far cheaper
    // than pulling every screen across through the CPU.
    Com<IDXGIAdapter1> adapter;
    bool crossAdapter = false;  // drawing on one GPU, glasses plugged into another
    if (!setup_.demo) {
        std::vector<std::pair<Com<IDXGIAdapter1>, int>> votes;
        for (auto& gdi : setup_.screens) {
            Com<IDXGIAdapter1> a;
            if (!adapterForOutput(gdi, a)) continue;
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            bool found = false;
            for (auto& v : votes) {
                DXGI_ADAPTER_DESC1 e;
                v.first->GetDesc1(&e);
                if (e.AdapterLuid.LowPart == d.AdapterLuid.LowPart && e.AdapterLuid.HighPart == d.AdapterLuid.HighPart) {
                    v.second++;
                    found = true;
                }
            }
            if (!found) votes.push_back({a, 1});
        }
        int best = 0;
        for (auto& v : votes)
            if (v.second > best) {
                best = v.second;
                adapter = v.first;
            }
        Com<IDXGIAdapter1> glassesGpu;
        if (adapterForOutput(setup_.glasses, glassesGpu)) {
            DXGI_ADAPTER_DESC1 d;
            glassesGpu->GetDesc1(&d);
            logf("render: glasses on %s", narrow(d.Description).c_str());
            if (!adapter) adapter = glassesGpu;
            DXGI_ADAPTER_DESC1 r;
            adapter->GetDesc1(&r);
            crossAdapter = r.AdapterLuid.LowPart != d.AdapterLuid.LowPart || r.AdapterLuid.HighPart != d.AdapterLuid.HighPart;
        }
    }
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    std::string err;
    if (!createDevice(adapter.get(), dev, ctx, err)) return fail(err);
    LUID luid{};
    {
        Com<IDXGIDevice> dd;
        Com<IDXGIAdapter> ad;
        if (SUCCEEDED(dev.as(dd)) && SUCCEEDED(dd->GetAdapter(ad.put()))) {
            DXGI_ADAPTER_DESC d;
            ad->GetDesc(&d);
            luid = d.AdapterLuid;
            logf("render: gpu %s", narrow(d.Description).c_str());
        }
    }
    Pipeline pipe;
    if (!pipe.init(dev.get(), err)) return fail(err);

    std::atomic<bool> closeDemo{false};
    if (setup_.demo) g_closeDemo = &closeDemo;
    HWND hwnd = createWindow(setup_);
    if (!hwnd) return fail("can't create the window");
    RECT cr;
    GetClientRect(hwnd, &cr);
    UINT bbW = (UINT)std::max(1L, cr.right), bbH = (UINT)std::max(1L, cr.bottom);
    Com<IDXGISwapChain> swap = createSwapChain(dev.get(), hwnd, bbW, bbH, err);
    if (!swap) {
        DestroyWindow(hwnd);
        return fail(err);
    }
    // Don't let frames queue up: every queued frame is one more frame of lag
    // between turning the head and seeing the screens move. When the glasses hang
    // off the other GPU, Windows copies each frame across, which takes a frame of
    // its own; with only one in flight we'd get every other refresh (45 fps on 90 Hz).
    HANDLE frameWait = nullptr;
    UINT swapFlags = 0;
    UINT latency = crossAdapter ? 2 : 1;
    {
        Com<IDXGISwapChain2> s2;
        DXGI_SWAP_CHAIN_DESC sd;
        if (SUCCEEDED(swap->GetDesc(&sd)) && (sd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
            SUCCEEDED(swap.as(s2)) && SUCCEEDED(s2->SetMaximumFrameLatency(latency))) {
            frameWait = s2->GetFrameLatencyWaitableObject();
            swapFlags = sd.Flags;
        }
        Com<IDXGIDevice1> d1;
        if (!frameWait && SUCCEEDED(dev.as(d1))) d1->SetMaximumFrameLatency(latency);
        logf("render: frame latency %u (%s%s)", latency, frameWait ? "waitable" : "device",
             crossAdapter ? ", glasses on the other gpu" : "");
    }
    Com<ID3D11RenderTargetView> rtv;
    auto makeRtv = [&] {
        rtv.reset();
        Com<ID3D11Texture2D> bb;
        if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.put())))
            dev->CreateRenderTargetView(bb.get(), nullptr, rtv.put());
    };
    makeRtv();

    Settings cfg;
    {
        std::lock_guard<std::mutex> lk(m_);
        cfg = settings_;
    }
    int count = setup_.demo ? std::max(1, setup_.demoScreens) : (int)setup_.screens.size();
    std::vector<std::unique_ptr<ScreenCapture>> screens;
    for (int i = 0; i < count; i++) {
        screens.push_back(std::make_unique<ScreenCapture>());
        screens.back()->open(dev.get(), luid, setup_.demo ? L"demo" : setup_.screens[(size_t)i], cfg.mipmaps);
    }
    std::vector<PanelMesh> meshes(screens.size());
    std::vector<Panel> panels;
    std::vector<float> aspects;
    CursorTexture cursor;
    HudTexture hud;

    running_ = true;
    if (started_) SetEvent(started_);
    logf("render: %ux%u, %d screens", bbW, bbH, count);
    for (auto& s : screens) {
        if (setup_.demo) break;
        RECT r = s->rect();
        logf("render: %s at %ld,%ld %ldx%ld", narrow(s->gdi()).c_str(), r.left, r.top, r.right - r.left, r.bottom - r.top);
    }

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    double clock = 0, statsTime = 0, housekeeping = 0;
    int statsFrames = 0;
    Quat view;
    int selected = -1;  // keyboard selection (fixed mode)
    int zoomed = -1;
    float zoom = 0, zoomTarget = 0;
    bool frozen = false;
    int gaze = -1;
    double gazeTime = 0;
    bool gazeHandled = false;
    POINT lastCursor = {};
    bool haveSafeCursor = false;
    int lastCursorScreen = -2;
    long pushDown = 0;
    bool wasAtBottom = false;
    RECT laptop = {};
    {
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) laptop = mi.rcMonitor;
    }
    double cursorIdle = 0;
    // the one setting on the glasses that breaks everything if it's wrong
    std::wstring toastText = setup_.demo ? L"" : text::glassesMode();
    float toastLeft = setup_.demo ? 0.0f : 5.0f;
    bool relayout = true;
    bool warnedNoSensor = false, warnedVertical = false;
    double sensorGone = 0;
    RECT glassesRect = setup_.glassesRect;

    // Performance notes for the log, every 30 s. Only ever written to log.txt.
    double perfTime = 0, perfCapture = 0, perfWork = 0, perfWorkMax = 0, perfWait = 0, perfPresent = 0, lastSlowLog = -10;
    int perfFrames = 0, perfUpdates = 0, perfSlow = 0;
    std::vector<float> perfIntervals;
    perfIntervals.reserve(8192);
    double lastCopyMs = 0, lastWorkMs = 0;
    int glassesHz = 60;
    auto readGlassesHz = [&] {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (!setup_.demo && EnumDisplaySettingsExW(setup_.glasses.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) &&
            dm.dmDisplayFrequency > 1)
            glassesHz = (int)dm.dmDisplayFrequency;
    };
    readGlassesHz();
    LARGE_INTEGER t0, t1, tw0, tw1, workStart;
    bool forceDraw = true, drawnWithCursor = false;
    double sinceDraw = 0;
    Quat drawnView;
    POINT drawnCursor = {};
    HCURSOR drawnShape = nullptr;
    int drawnGaze = -2, perfSkipped = 0;
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+), plain timer otherwise
    HANDLE idleTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    if (!idleTimer) idleTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

    while (!stop_ && !closeDemo) {
        // wait until the glasses can take a new frame, then build it from the
        // freshest data we have
        QueryPerformanceCounter(&tw0);
        if (frameWait) WaitForSingleObjectEx(frameWait, 100, TRUE);
        QueryPerformanceCounter(&tw1);
        perfWait += (double)(tw1.QuadPart - tw0.QuadPart) / (double)freq.QuadPart;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        double rawDt = (double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart;
        float dt = clampf((float)rawDt, 0.0001f, 0.1f);
        prev = now;
        workStart = now;
        // a frame that took far longer than the glasses' refresh is a visible hitch
        if (!setup_.demo && clock > 3 && rawDt > std::max(0.05, 2.5 / glassesHz)) {
            perfSlow++;
            if (clock - lastSlowLog > 2) {
                logf("render: slow frame %.0f ms (copying screens %.1f ms, drawing %.1f ms)", rawDt * 1000, lastCopyMs,
                     lastWorkMs);
                lastSlowLog = clock;
            }
        }
        if (clock > 1) perfIntervals.push_back((float)(rawDt * 1000));
        clock += dt;

        std::deque<Command> commands;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (settingsChanged_) {
                cfg = settings_;
                settingsChanged_ = false;
                relayout = true;
            }
            commands.swap(commands_);
            if (toastPending_) {
                toastText = toast_;
                toastLeft = toastSeconds_;
                toastPending_ = false;
            }
        }
        bool uncalibrated = false;
        if (imu_)
            imu_->with([&](HeadTracker& t, CalibrationWizard&) {
                t.followPitch = cfg.followPitch();
                uncalibrated = t.filter.calibration().mode != AxisCalibration::Full;
                t.smoothing = cfg.smoothing;
                t.predictSeconds = cfg.predictMs / 1000.0f;
                t.travelMode = cfg.travelMode;
                t.travelTimeConstant = cfg.travelSeconds;
                t.bedMode = cfg.bedMode;
                t.filter.invertYaw = cfg.invertYaw;
                return 0;
            });

        // without calibration only turning is tracked, so a second row would stay out of reach
        bool wantsFixedPitch = cfg.tracking == Tracking::Head && !cfg.followPitch();
        if (!wantsFixedPitch || !uncalibrated) warnedVertical = false;
        else if (!warnedVertical && imu_ && imu_->ready() && clock > 5.5) {
            warnedVertical = true;
            toastText = text::needsCalibration();
            toastLeft = 7.0f;
        }

        // grab new frames
        int live = 0;
        QueryPerformanceCounter(&t0);
        for (size_t i = 0; i < screens.size(); i++) {
            auto& s = *screens[i];
            if (!setup_.demo && s.update(ctx.get())) {
                perfUpdates++;
                forceDraw = true;
            }
            if (s.live()) live++;
            if (!s.live() && !s.showingPlaceholder()) {
                std::wstring t = setup_.demo ? text::demoScreen() : std::wstring(text::waitingImage()) + L"\n" + widen(s.error());
                s.placeholder(ctx.get(), (int)i, t.c_str());
                forceDraw = true;
            }
        }
        QueryPerformanceCounter(&t1);
        lastCopyMs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart * 1000;
        perfCapture += lastCopyMs / 1000;

        // once a second: follow monitor moves and keep the window on top
        housekeeping += dt;
        if (housekeeping > 1.0 && !setup_.demo) {
            housekeeping = 0;
            readGlassesHz();
            for (auto& s : screens) s->refreshRect();
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) laptop = mi.rcMonitor;
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(setup_.glasses.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0))
                glassesRect = {dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + (LONG)dm.dmPelsWidth,
                               dm.dmPosition.y + (LONG)dm.dmPelsHeight};
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            // only touch the window when it's actually off: moving a full screen
            // window, even to the same place, can make Windows rebuild its presentation
            RECT wr;
            GetWindowRect(hwnd, &wr);
            if (!EqualRect(&wr, &glassesRect))
                SetWindowPos(hwnd, HWND_TOPMOST, glassesRect.left, glassesRect.top, glassesRect.right - glassesRect.left,
                             glassesRect.bottom - glassesRect.top, SWP_NOACTIVATE);
        }
        GetClientRect(hwnd, &cr);
        if ((UINT)cr.right != bbW || (UINT)cr.bottom != bbH) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            rtv.reset();
            ctx->Flush();
            if (cr.right > 0 && cr.bottom > 0 &&
                SUCCEEDED(swap->ResizeBuffers(0, (UINT)cr.right, (UINT)cr.bottom, DXGI_FORMAT_UNKNOWN, swapFlags))) {
                bbW = (UINT)cr.right;
                bbH = (UINT)cr.bottom;
            }
            makeRtv();
            forceDraw = true;
        }

        // layout
        std::vector<float> a;
        for (auto& s : screens) a.push_back((float)s->width() / (float)std::max(1, s->height()));
        if (a != aspects) relayout = true;
        if (std::fabs(zoomTarget - zoom) > 1e-3f) {
            zoom += (zoomTarget - zoom) * (1.0f - std::exp(-dt / 0.12f));
            if (std::fabs(zoomTarget - zoom) < 0.01f) zoom = zoomTarget;
            relayout = true;
        }
        if (relayout) {
            panels = computeLayout(a, cfg.layout, zoomed, zoom);
            for (size_t i = 0; i < panels.size(); i++) meshes[i].set(dev.get(), ctx.get(), panels[i]);
            aspects = a;
            relayout = false;
            forceDraw = true;
        }
        if (selected < 0 || selected >= (int)panels.size()) selected = (int)panels.size() / 2;

        // mouse
        CURSORINFO ci = {sizeof(ci)};
        bool haveCursor = GetCursorInfo(&ci) != 0;
        POINT cp = ci.ptScreenPos;
        if (!haveCursor) GetCursorPos(&cp);
        bool drawCursor = cfg.showCursor && haveCursor && (ci.flags & CURSOR_SHOWING) && ci.hCursor &&
                          cursor.update(dev.get(), ctx.get(), ci.hCursor);
        // The pointer must not wander into the glasses' own desktop area: it would be
        // hidden behind this window. Put it back where it was.
        if (cfg.cursorGuard && !setup_.demo && PtInRect(&glassesRect, cp)) {
            POINT back = lastCursor;
            if (PtInRect(&glassesRect, back) || (back.x == 0 && back.y == 0 && !haveSafeCursor))
                back = {GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
            SetCursorPos(back.x, back.y);
            cp = back;
        }
        haveSafeCursor = true;
        cursorIdle = (cp.x != lastCursor.x || cp.y != lastCursor.y) ? 0 : cursorIdle + dt;
        lastCursor = cp;
        int cursorScreen = -1;
        for (size_t i = 0; i < screens.size() && !setup_.demo; i++) {
            RECT r = screens[i]->rect();
            if (PtInRect(&r, cp)) cursorScreen = (int)i;
        }
        // The laptop only touches the screens right above it. Pushing down on any
        // other screen of the bottom row drops the pointer onto the laptop.
        long rawDown = g_rawDown;
        g_rawDown = 0;
        bool atBottom = false;
        if (cfg.dropToLaptop && !setup_.demo && cursorScreen >= 0 && laptop.right > laptop.left) {
            RECT r = screens[(size_t)cursorScreen]->rect();
            bool bottomRow = r.bottom == laptop.top;
            bool aboveLaptop = cp.x >= laptop.left && cp.x < laptop.right;
            atBottom = bottomRow && !aboveLaptop && cp.y >= r.bottom - 2;
            if (atBottom && wasAtBottom) {
                pushDown += std::max(0L, rawDown);
                if (pushDown > 25) {
                    long rowL = r.left, rowR = r.right;
                    for (auto& s : screens) {
                        RECT o = s->rect();
                        if (o.bottom == laptop.top) {
                            rowL = std::min(rowL, o.left);
                            rowR = std::max(rowR, o.right);
                        }
                    }
                    float f = clampf((float)(cp.x - rowL) / (float)std::max(1L, rowR - rowL), 0, 1);
                    POINT to = {laptop.left + (LONG)(f * (float)(laptop.right - laptop.left - 1)), laptop.top + 2};
                    SetCursorPos(to.x, to.y);
                    cp = to;
                    cursorScreen = -1;
                    atBottom = false;
                }
            }
        }
        if (!atBottom) pushDown = 0;
        wasAtBottom = atBottom;
        if (cfg.tracking != Tracking::Head && cursorScreen != lastCursorScreen) {
            logf("render: mouse at %ld,%ld -> screen %d", cp.x, cp.y, cursorScreen + 1);
            lastCursorScreen = cursorScreen;
        }

        for (auto c : commands) {
            switch (c) {
            case Command::Recenter:
                if (imu_) imu_->recenter();
                view = Quat::identity();
                selected = (int)panels.size() / 2;
                break;
            case Command::Freeze:
                frozen = !frozen;
                if (imu_) imu_->with([&](HeadTracker& t, CalibrationWizard&) { t.freeze = frozen; return 0; });
                toastText = frozen ? text::viewFrozen() : text::viewUnfrozen();
                toastLeft = 1.8f;
                break;
            case Command::Zoom:
                if (zoomTarget > 0.5f) zoomTarget = 0;
                else if (gaze >= 0) {
                    zoomed = gaze;
                    zoomTarget = 1;
                }
                break;
            case Command::NextScreen:
            case Command::PrevScreen:
                if (!panels.empty()) {
                    int n = (int)panels.size();
                    selected = (selected + (c == Command::NextScreen ? 1 : n - 1)) % n;
                    if (!setup_.demo) centerCursorOn(screens[(size_t)selected]->rect());
                }
                break;
            case Command::CursorToGaze:
                if (gaze >= 0 && !setup_.demo) centerCursorOn(screens[(size_t)gaze]->rect());
                break;
            }
        }

        // where are we looking
        bool fallback = false;
        if (setup_.demo && !imu_) {
            float yaw = setup_.screenshot.empty() ? 0.75f * std::sin((float)clock * 0.4f) : deg2rad(18);
            view = Quat::fromYawPitch(yaw, deg2rad(3));
        } else if (cfg.tracking == Tracking::Head && imu_ && imu_->ready()) {
            view = imu_->view(dt);
            warnedNoSensor = false;
            sensorGone = 0;
        } else if (cfg.tracking == Tracking::Head && (sensorGone += dt) < 1.5) {
            // The tracker restarts for a moment after calibrating or reconnecting.
            // Hold the view instead of jumping to the mouse and back.
        } else {
            // no sensor: turn towards the screen that has the mouse (or the selected one)
            if (cfg.tracking == Tracking::Head) {
                fallback = true;
                if (!warnedNoSensor && clock > 4.0) {
                    warnedNoSensor = true;
                    toastText = text::noSensor();
                    toastLeft = 4.0f;
                }
            }
            int target = cfg.tracking == Tracking::Fixed ? selected : cursorScreen;
            if (!frozen && target >= 0 && target < (int)panels.size()) {
                Quat goal = Quat::fromYawPitch(panels[(size_t)target].yaw, panels[(size_t)target].pitch);
                view = slerp(view, goal, 1.0f - std::exp(-dt / std::max(0.05f, cfg.mouseFollowSeconds)));
            }
        }

        bool inside = false;
        int g = panelAt(panels, view.forward(), &inside);
        if (!inside) g = -1;
        if (g != gaze) {
            gaze = g;
            gazeTime = 0;
            gazeHandled = false;
        } else {
            gazeTime += dt;
        }
        if (cfg.cursorFollowsGaze && cfg.tracking == Tracking::Head && !fallback && gaze >= 0 && !gazeHandled &&
            gazeTime > 0.6 && cursorIdle > 0.4 && cursorScreen != gaze && !setup_.demo) {
            centerCursorOn(screens[(size_t)gaze]->rect());
            gazeHandled = true;
        }

        // Nothing moved and nothing on the screens changed: the glasses keep showing
        // the last frame, so don't redraw it. Saves most of the GPU when you're
        // reading or the screens are still.
        bool calibrating = false;
        if (imu_) calibrating = imu_->with([](HeadTracker&, CalibrationWizard& w) { return w.active(); });
        float dq = std::fabs(view.w * drawnView.w + view.x * drawnView.x + view.y * drawnView.y + view.z * drawnView.z);
        float turned = 2.0f * std::acos(clampf(dq, 0.0f, 1.0f));
        bool needDraw = forceDraw || turned > deg2rad(0.015f) || cp.x != drawnCursor.x || cp.y != drawnCursor.y ||
                        drawCursor != drawnWithCursor || ci.hCursor != drawnShape || gaze != drawnGaze ||
                        toastLeft > 0 || calibrating || setup_.demo || clock < 2 || sinceDraw > 0.5;
        if (!needDraw) {
            sinceDraw += rawDt;
            statsFrames++;
            statsTime += dt;
            perfTime += rawDt;
            perfSkipped++;
            // nothing to present means nothing to wait for either; sleep about one refresh
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(10000000.0 / std::max(30, glassesHz) * 0.9);
            if (idleTimer && SetWaitableTimer(idleTimer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(idleTimer, 50);
            else
                Sleep(1);
            continue;
        }
        forceDraw = false;
        sinceDraw = 0;
        drawnView = view;
        drawnCursor = cp;
        drawnWithCursor = drawCursor;
        drawnShape = ci.hCursor;
        drawnGaze = gaze;

        // draw
        float aspect = (float)bbW / (float)bbH;
        float fov = deg2rad(cfg.diagonalFovDeg > 0 ? cfg.diagonalFovDeg : setup_.fov);
        Mat4 vp = Mat4::fromQuat(view.conj()) * Mat4::perspectiveRH(verticalFovFromDiagonal(fov, aspect), aspect, 0.05f, 100.0f);
        FrameConstants fc;
        memcpy(fc.viewProj, vp.m, sizeof(fc.viewProj));
        upload(ctx.get(), pipe.frameCB.get(), fc);

        ID3D11RenderTargetView* rt = rtv.get();
        const float black[4] = {0, 0, 0, 1};  // black is see-through on the glasses
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->ClearRenderTargetView(rt, black);
        D3D11_VIEWPORT viewport = {0, 0, (float)bbW, (float)bbH, 0, 1};
        ctx->RSSetViewports(1, &viewport);
        pipe.bindPanels(ctx.get());

        for (size_t i = 0; i < panels.size(); i++) {
            auto& s = *screens[i];
            if (!s.hasImage()) continue;
            PanelConstants pc = {};
            bool withCursor = drawCursor && (int)i == cursorScreen;
            if (withCursor) {
                RECT r = s.rect();
                float rw = (float)std::max(1L, r.right - r.left), rh = (float)std::max(1L, r.bottom - r.top);
                pc.cursor[0] = (float)(cp.x - cursor.hotX - r.left) / rw;
                pc.cursor[1] = (float)(cp.y - cursor.hotY - r.top) / rh;
                pc.cursor[2] = (float)cursor.w / rw;
                pc.cursor[3] = (float)cursor.h / rh;
            }
            pc.params[0] = (float)cfg.brightness / 100.0f;
            pc.params[1] = cfg.highlightFocused && (int)i == gaze && panels.size() > 1 ? 1.0f : 0.0f;
            pc.params[2] = (float)s.rotation();
            pc.params[3] = withCursor ? 1.0f : 0.0f;
            pc.accent[0] = 0.18f;
            pc.accent[1] = 0.86f;
            pc.accent[2] = 0.76f;
            pc.accent[3] = 0.9f;
            pc.border[0] = deg2rad(0.22f) / std::max(1e-3f, panels[i].widthRad);
            pc.border[1] = deg2rad(0.22f) / std::max(1e-3f, panels[i].heightRad);
            pc.extra[0] = (float)cfg.sharpness / 100.0f * 1.2f;
            upload(ctx.get(), pipe.panelCB.get(), pc);
            ID3D11ShaderResourceView* srvs[2] = {s.srv(), withCursor ? cursor.tex.srv.get() : nullptr};
            ctx->PSSetShaderResources(0, 2, srvs);
            meshes[i].draw(ctx.get());
        }

        // text overlay: calibration steps win over toasts
        std::wstring overlay;
        float progress = -1;
        if (imu_)
            imu_->with([&](HeadTracker&, CalibrationWizard& w) {
                if (w.active()) {
                    overlay = text::calibrationStep(w.step());
                    progress = w.progress();
                }
                return 0;
            });
        if (overlay.empty() && toastLeft > 0) {
            toastLeft -= dt;
            overlay = toastText;
        }
        if (!overlay.empty() && hud.update(dev.get(), ctx.get(), overlay, progress, (int)bbW)) {
            HudConstants hc = {};
            float w = (float)hud.tex.w / (float)bbW * 2.0f, h = (float)hud.tex.h / (float)bbH * 2.0f;
            hc.rect[0] = -w / 2;
            hc.rect[1] = -0.88f + h;
            hc.rect[2] = w / 2;
            hc.rect[3] = -0.88f;
            hc.params[0] = progress < 0 ? clampf(toastLeft * 2.0f, 0, 1) : 1.0f;  // fade out
            upload(ctx.get(), pipe.hudCB.get(), hc);
            pipe.bindHud(ctx.get());
            ID3D11ShaderResourceView* s = hud.tex.srv.get();
            ctx->PSSetShaderResources(0, 1, &s);
            ctx->Draw(4, 0);
        }
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);

        if (setup_.demo && !setup_.screenshot.empty() && clock >= setup_.screenshotAfter) {
            Com<ID3D11Texture2D> bb, copy;
            swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.put());
            D3D11_TEXTURE2D_DESC d;
            bb->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = 0;
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, copy.put()))) {
                ctx->CopyResource(copy.get(), bb.get());
                if (SUCCEEDED(ctx->Map(copy.get(), 0, D3D11_MAP_READ, 0, &m))) {
                    saveBmp(setup_.screenshot, (const uint8_t*)m.pData, m.RowPitch, (int)d.Width, (int)d.Height);
                    ctx->Unmap(copy.get(), 0);
                }
            }
            closeDemo = true;
        }

        QueryPerformanceCounter(&t1);
        lastWorkMs = (double)(t1.QuadPart - workStart.QuadPart) / (double)freq.QuadPart * 1000;
        perfWork += lastWorkMs;
        perfWorkMax = std::max(perfWorkMax, lastWorkMs);
        HRESULT pr = swap->Present(1, 0);
        QueryPerformanceCounter(&tw1);
        perfPresent += (double)(tw1.QuadPart - t1.QuadPart) / (double)freq.QuadPart;
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
            logf("render: device lost %s", hex(dev->GetDeviceRemovedReason()).c_str());
            std::lock_guard<std::mutex> lk(m_);
            stats_.error = "gpu reset";
            break;
        }
        if (pr == DXGI_STATUS_OCCLUDED) Sleep(10);

        statsFrames++;
        statsTime += dt;
        perfFrames++;
        perfTime += rawDt;
        if (perfTime >= 30 && !setup_.demo) {
            int other = 0;
            for (auto& s : screens) other += s->otherGpu() ? 1 : 0;
            auto pct = [&](float p) {
                if (perfIntervals.empty()) return 0.0f;
                size_t k = std::min(perfIntervals.size() - 1, (size_t)(p * (float)perfIntervals.size()));
                std::nth_element(perfIntervals.begin(), perfIntervals.begin() + (long)k, perfIntervals.end());
                return perfIntervals[k];
            };
            float p50 = pct(0.5f), p95 = pct(0.95f), p99 = pct(0.99f), pmax = pct(1.0f);
            int missed = 0;
            for (float ms : perfIntervals) missed += ms > 1500.0f / (float)glassesHz ? 1 : 0;
            float imuRate = imu_ ? imu_->rate() : 0, imuGap = imu_ ? imu_->takeMaxGapMs() : 0;
            logf("perf: %.0f fps on %d Hz glasses, %.0f%% of them redrawn | frame ms p50 %.1f p95 %.1f p99 %.1f max %.0f | late frames %d, "
                 "hitches %d | drawing %.1f ms avg %.1f max | copying screens %.1f ms | screen updates %.0f/s | "
                 "waiting %.0f%%, presenting %.1f ms | imu %.0f/s, longest gap %.0f ms | %d/%d screens from another gpu",
                 (perfFrames + perfSkipped) / perfTime, glassesHz,
                 100.0 * perfFrames / std::max(1, perfFrames + perfSkipped), p50, p95, p99, pmax, missed, perfSlow, perfWork / perfFrames,
                 perfWorkMax, perfCapture / std::max(1, perfFrames + perfSkipped) * 1000, perfUpdates / perfTime, perfWait / perfTime * 100,
                 perfPresent / perfFrames * 1000, imuRate, imuGap, other, (int)screens.size());
            perfTime = perfCapture = perfWork = perfWorkMax = perfWait = perfPresent = 0;
            perfFrames = perfUpdates = perfSlow = perfSkipped = 0;
            perfIntervals.clear();
        }
        if (statsTime >= 0.5) {
            std::lock_guard<std::mutex> lk(m_);
            stats_.fps = (float)(statsFrames / statsTime);
            stats_.gaze = gaze;
            stats_.mouseFallback = fallback;
            stats_.live = live;
            stats_.total = (int)screens.size();
            statsFrames = 0;
            statsTime = 0;
        }
    }

    screens.clear();
    if (frameWait) CloseHandle(frameWait);
    if (idleTimer) CloseHandle(idleTimer);
    ctx->ClearState();
    ctx->Flush();
    swap.reset();
    DestroyWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    g_closeDemo = nullptr;
    running_ = false;
    finished_ = true;
    logf("render: stopped");
}

} // namespace ox
