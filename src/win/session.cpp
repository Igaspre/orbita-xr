#include "session.h"
#include "gpu.h"
#include "vdd.h"
#include <algorithm>

namespace ox {

static std::vector<std::wstring> split(const std::string& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : widen(s)) {
        if (c == L';') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

template <class F>
static bool waitFor(int ms, F&& cond) {
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;
    while (GetTickCount64() < end) {
        if (cond()) return true;
        Sleep(250);
    }
    return cond();
}

// Where the virtual screens go in Windows' layout, so moving the mouse between
// them matches what you see: a row above the laptop screen (or around it), and
// a grid stacks two rows. The glasses go below, out of the way.
static std::vector<POINT> arrange(const std::vector<SIZE>& sizes, const Monitor* primary, bool grid, bool aroundPrimary) {
    int pw = primary ? primary->width : 1920;
    std::vector<POINT> pos(sizes.size());
    if (aroundPrimary) {
        size_t left = sizes.size() / 2;
        int x = 0;
        for (size_t i = left; i-- > 0;) {
            x -= sizes[i].cx;
            pos[i] = {x, 0};
        }
        x = pw;
        for (size_t i = left; i < sizes.size(); i++) {
            pos[i] = {x, 0};
            x += sizes[i].cx;
        }
        return pos;
    }
    size_t cols = grid ? (sizes.size() + 1) / 2 : sizes.size();
    int bottomRowH = 0, topRowH = 0;
    for (size_t i = 0; i < sizes.size(); i++) {
        int& h = (grid && i < cols) ? topRowH : bottomRowH;
        h = std::max(h, (int)sizes[i].cy);
    }
    for (int row = 0; row < (grid ? 2 : 1); row++) {
        size_t a = row == 0 ? 0 : cols, b = grid && row == 0 ? cols : sizes.size();
        int total = 0;
        for (size_t i = a; i < b; i++) total += sizes[i].cx;
        int x = (pw - total) / 2;
        int rowBottom = grid && row == 0 ? -bottomRowH : 0;
        for (size_t i = a; i < b; i++) {
            pos[i] = {x, rowBottom - sizes[i].cy};
            x += sizes[i].cx;
        }
    }
    return pos;
}

// Puts our virtual screens where Windows should have them for the current layout
// and re-reads the monitor list. Same order every time (by GDI name), so the
// renderer's screen order still matches after switching between one and two rows.
void Session::placeVirtual(const Settings& s, std::vector<Monitor>& mons) {
    // Right after the driver restarts (new GPU), Windows brings the screens back
    // under new names and they take a moment to accept changes. Check that they
    // ended up where we put them and try again if not.
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) {
            Sleep(1000);
            mons = listMonitors();
        }
        const Monitor* primary = nullptr;
        std::vector<const Monitor*> chosen;
        for (auto& m : mons) {
            if (m.primary) primary = &m;
            if (m.isVirtual && !m.mirrorsPrimary) chosen.push_back(&m);
        }
        std::sort(chosen.begin(), chosen.end(), [](auto a, auto b) { return a->gdi < b->gdi; });
        bool ready = !chosen.empty();
        for (auto* m : chosen) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsExW(m->gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) ready = false;
        }
        if (!ready) {
            logf("session: screens not ready yet");
            continue;
        }

        std::vector<SIZE> sizes;
        for (size_t i = 0; i < chosen.size(); i++) {
            bool rot = i < s.portrait.size() && s.portrait[i] == '1';
            sizes.push_back(rot ? SIZE{s.resH, s.resW} : SIZE{s.resW, s.resH});
        }
        bool grid = s.layout.arrangement == Arrangement::Grid && chosen.size() > 1 && !s.includePrimary;
        auto pos = arrange(sizes, primary, grid, s.includePrimary);
        std::vector<ModeChange> changes;
        std::vector<std::wstring> names;
        for (size_t i = 0; i < chosen.size(); i++) {
            ModeChange c;
            c.gdi = chosen[i]->gdi;
            c.width = s.resW;
            c.height = s.resH;
            c.refresh = s.refreshHz;
            c.orientation = i < s.portrait.size() && s.portrait[i] == '1' ? DMDO_90 : DMDO_DEFAULT;
            c.move = s.autoArrange;
            c.x = pos[i].x;
            c.y = pos[i].y;
            changes.push_back(c);
            names.push_back(c.gdi);
        }
        if (s.autoArrange) {
            ModeChange c;
            c.gdi = glassesGdi_;
            c.move = true;
            c.y = primary ? primary->height : 1080;
            changes.push_back(c);
        }
        std::wstring warnings;
        applyModes(changes, &warnings);
        if (!warnings.empty()) logf("session: %s", narrow(warnings).c_str());
        Sleep(600);
        mons = listMonitors();
        if (!s.autoArrange) return;

        int wrong = 0;
        for (size_t i = 0; i < names.size(); i++) {
            bool ok = false;
            for (auto& m : mons)
                if (m.gdi == names[i]) ok = m.rect.left == pos[i].x && m.rect.top == pos[i].y;
            if (!ok) wrong++;
        }
        if (!wrong) return;
        logf("session: %d screens not where they should be, trying again", wrong);
    }
}

bool Session::rearrange(const Settings& s) {
    if (!running_ || !usedVirtual_ || !s.autoArrange) return false;
    logf("session: placing the screens again (%s)", s.layout.arrangement == Arrangement::Grid ? "two rows" : "one row");
    auto mons = listMonitors();
    placeVirtual(s, mons);
    return true;
}

StartResult Session::start(const Settings& s, const std::function<void(const std::wstring&)>& progress) {
    StartResult res;
    if (running_) {
        res.ok = true;
        return res;
    }
    auto say = [&](const std::wstring& t) {
        logf("session: %s", narrow(t).c_str());
        if (progress) progress(t);
    };
    auto fail = [&](const std::wstring& why) {
        logf("session: failed: %s", narrow(why).c_str());
        stop();
        res.message = why;
        return res;
    };
    active_ = true;
    int n = std::clamp(s.screenCount, 1, 6);

    say(tr(L"Buscando las gafas…", L"Looking for the glasses…"));
    auto mons = listMonitors();
    const Monitor* g = findGlasses(mons, widen(s.glassesOverride), true);
    if (!g)
        return fail(tr(L"No se detectan las gafas. Conéctalas por USB-C y espera unos segundos, o elígelas a mano en Pantallas.",
                       L"Can't find the glasses. Connect them over USB-C and wait a few seconds, or pick them in Screens."));
    glassesName_ = g->name.empty() ? tr(L"Gafas", L"Glasses") : g->name;
    if (g->mirrorsPrimary) {
        if (!s.autoExtend)
            return fail(tr(L"Las gafas están duplicando la pantalla del portátil. Pulsa Win+P y elige Extender.",
                           L"The glasses are duplicating the laptop screen. Press Win+P and choose Extend."));
        say(tr(L"Pasando las gafas a modo extendido…", L"Switching the glasses to extend mode…"));
        switchedToExtend_ = extendDesktop();
        Sleep(2000);
        mons = listMonitors();
        g = findGlasses(mons, widen(s.glassesOverride), true);
        if (!g || g->mirrorsPrimary)
            return fail(tr(L"No he podido extender el escritorio. Pulsa Win+P y elige Extender.",
                           L"Couldn't extend the desktop. Press Win+P and choose Extend."));
    }
    glassesGdi_ = g->gdi;
    float fov = guessFov(g->name);

    usedVirtual_ = s.source == Source::VirtualScreens;
    if (usedVirtual_) {
        if (vdd::state() == vdd::State::Missing) {
            res.needsDriver = true;
            return fail(tr(L"Falta el driver de pantallas virtuales.", L"The virtual screen driver is missing."));
        }
        if (vdd::ours()) {
            std::wstring want = s.vddGpu.empty() ? L"default" : widen(s.vddGpu);
            if (_wcsicmp(vdd::configuredGpu().c_str(), want.c_str()) != 0) {
                say(tr(L"Cambiando la gráfica de las pantallas…", L"Changing the screens' GPU…"));
                vdd::setGpu(want);
                Sleep(2000);
            }
        }
        say(tr(L"Preparando las pantallas virtuales…", L"Preparing the virtual screens…"));
        virtualBefore_ = virtualScreensActive();
        // someone else's install may be set up with fewer screens than we need
        int configured = vdd::configuredCount();
        if (!vdd::ours() && configured >= 0 && configured < n && vdd::setCount(n)) countBefore_ = configured;
        if (!waitFor(20000, [&] { return virtualScreensAvailable() >= n; }))
            return fail(wfmt(tr(L"El driver solo ofrece %d pantallas. Prueba a reiniciar el PC.",
                                L"The driver only offers %d screens. Try restarting the PC."),
                             virtualScreensAvailable()));
        bool ok = waitFor(8000, [&] { return setVirtualScreens(n) && virtualScreensActive() == n; });
        if (!ok)
            return fail(wfmt(tr(L"Windows solo ha activado %d de %d pantallas virtuales.",
                                L"Windows only turned on %d of %d virtual screens."),
                             virtualScreensActive(), n));
        Sleep(500);
        mons = listMonitors();
    }

    const Monitor* primary = nullptr;
    for (auto& m : mons)
        if (m.primary) primary = &m;
    g = findGlasses(mons, glassesGdi_, true);
    if (!g) return fail(tr(L"Las gafas se han desconectado.", L"The glasses were disconnected."));

    std::vector<const Monitor*> chosen;
    if (usedVirtual_) {
        say(tr(L"Colocando las pantallas…", L"Placing the screens…"));
        placeVirtual(s, mons);
        g = findGlasses(mons, glassesGdi_, true);
        if (!g) return fail(tr(L"Las gafas se han desconectado.", L"The glasses were disconnected."));
        primary = nullptr;  // mons was re-read
        for (auto& m : mons) {
            if (m.isVirtual && !m.mirrorsPrimary) chosen.push_back(&m);
            if (m.primary) primary = &m;
        }
        virtualGpu_.clear();
        Com<IDXGIAdapter1> a;
        if (!chosen.empty() && adapterForOutput(chosen.front()->gdi, a)) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            virtualGpu_ = d.Description;
            logf("session: virtual screens drawn on %s (asked for %s)", narrow(virtualGpu_).c_str(),
                 s.vddGpu.empty() ? "default" : s.vddGpu.c_str());
        }
    } else {
        for (auto& name : split(s.existingMonitors))
            for (auto& m : mons)
                if (_wcsicmp(m.gdi.c_str(), name.c_str()) == 0 && m.gdi != glassesGdi_ && !m.mirrorsPrimary)
                    chosen.push_back(&m);
        if (chosen.empty() && !s.includePrimary)
            return fail(tr(L"No has elegido ninguna pantalla para mostrar.", L"You haven't picked any screen to show."));
    }
    if (s.includePrimary && primary && std::find(chosen.begin(), chosen.end(), primary) == chosen.end())
        chosen.push_back(primary);
    bool grid = s.layout.arrangement == Arrangement::Grid;
    std::sort(chosen.begin(), chosen.end(), [grid](auto a, auto b) {
        if (grid && a->rect.top != b->rect.top) return a->rect.top < b->rect.top;
        return a->rect.left < b->rect.left;
    });
    glassesRect_ = g->rect;

    if (s.tracking == Tracking::Head) {
        AxisCalibration cal;
        if (!cal.deserialize(s.calibration)) cal = {};
        imu_.with([&](HeadTracker& t, CalibrationWizard&) {
            t.filter.setCalibration(cal);
            return 0;
        });
        imu_.start(s.imuAddress);
    }

    say(tr(L"Abriendo la vista en las gafas…", L"Opening the view in the glasses…"));
    RenderSetup rs;
    rs.glasses = glassesGdi_;
    rs.glassesRect = glassesRect_;
    for (auto* m : chosen) rs.screens.push_back(m->gdi);
    rs.fov = fov;
    if (!renderer_.start(rs, s, &imu_))
        return fail(tr(L"No he podido dibujar en las gafas: ", L"Couldn't draw in the glasses: ") + widen(renderer_.error()));
    running_ = true;
    res.ok = true;
    return res;
}

void Session::stop() {
    if (!active_) return;
    running_ = false;
    renderer_.stop();
    imu_.stop();
    if (usedVirtual_) {
        // ours: leave none connected. Someone else's: leave it like we found it.
        int restore = vdd::ours() ? 0 : std::max(0, virtualBefore_);
        setVirtualScreens(restore);
        if (countBefore_ >= 0) vdd::setCount(countBefore_);
    }
    if (switchedToExtend_) {
        Sleep(500);
        mirrorDesktop();
    }
    active_ = usedVirtual_ = switchedToExtend_ = false;
    virtualBefore_ = countBefore_ = -1;
    logf("session: stopped");
}

bool Session::glassesConnected() const {
    for (auto& m : listMonitors())
        if (_wcsicmp(m.gdi.c_str(), glassesGdi_.c_str()) == 0 && !m.primary) return true;
    return false;
}

void Session::apply(const Settings& s) {
    if (renderer_.running()) renderer_.apply(s);
}

} // namespace ox
