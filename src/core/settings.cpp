#include "settings.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace ox {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// One list of fields for both reading and writing the ini file.
struct Writer {
    std::ostringstream o;
    void operator()(const char* k, int& v) { o << k << '=' << v << '\n'; }
    void operator()(const char* k, bool& v) { o << k << '=' << (v ? 1 : 0) << '\n'; }
    void operator()(const char* k, float& v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.3f", v);
        o << k << '=' << b << '\n';
    }
    void operator()(const char* k, std::string& v) { o << k << '=' << v << '\n'; }
};

struct Reader {
    std::map<std::string, std::string> kv;
    const std::string* get(const char* k) {
        auto it = kv.find(k);
        return it == kv.end() ? nullptr : &it->second;
    }
    void operator()(const char* k, int& v) {
        if (auto s = get(k); s && !s->empty()) v = std::atoi(s->c_str());
    }
    void operator()(const char* k, bool& v) {
        if (auto s = get(k); s && !s->empty()) v = *s != "0" && *s != "false";
    }
    void operator()(const char* k, float& v) {
        if (auto s = get(k); s && !s->empty()) {
            float f = (float)std::atof(s->c_str());
            if (std::isfinite(f)) v = f;
        }
    }
    void operator()(const char* k, std::string& v) {
        if (auto s = get(k)) v = *s;
    }
};

template <class V>
void fields(Settings& s, V& v) {
    int source = (int)s.source, arrangement = (int)s.layout.arrangement, tracking = (int)s.tracking,
        vertical = (int)s.vertical;
    v("screens", s.screenCount);
    v("width", s.resW);
    v("height", s.resH);
    v("refresh", s.refreshHz);
    v("source", source);
    v("existing", s.existingMonitors);
    v("portrait", s.portrait);
    v("include_primary", s.includePrimary);
    v("auto_arrange", s.autoArrange);
    v("auto_extend", s.autoExtend);
    v("glasses", s.glassesOverride);
    v("vdd_gpu", s.vddGpu);
    v("arrangement", arrangement);
    v("flat", s.layout.flat);
    v("vertical", vertical);
    v("panel_width", s.layout.panelWidthDeg);
    v("gap", s.layout.gapDeg);
    v("height_offset", s.layout.pitchOffsetDeg);
    v("fov", s.diagonalFovDeg);
    v("brightness", s.brightness);
    v("highlight", s.highlightFocused);
    v("cursor", s.showCursor);
    v("mipmaps", s.mipmaps);
    v("sharpness", s.sharpness);
    v("tracking", tracking);
    v("smoothing", s.smoothing);
    v("predict_ms", s.predictMs);
    v("travel", s.travelMode);
    v("travel_seconds", s.travelSeconds);
    v("bed", s.bedMode);
    v("invert_yaw", s.invertYaw);
    v("cursor_follows_gaze", s.cursorFollowsGaze);
    v("cursor_guard", s.cursorGuard);
    v("drop_to_laptop", s.dropToLaptop);
    v("mouse_follow", s.mouseFollowSeconds);
    v("calibration", s.calibration);
    v("imu", s.imuAddress);
    v("run_at_login", s.runAtLogin);
    v("start_hidden", s.startHidden);
    v("auto_start", s.autoStart);
    v("language", s.language);
    s.source = (Source)source;
    s.layout.arrangement = (Arrangement)arrangement;
    s.tracking = (Tracking)tracking;
    s.vertical = (Vertical)vertical;
}

} // namespace

std::string Settings::serialize() const {
    Settings copy = *this;
    Writer w;
    fields(copy, w);
    return w.o.str();
}

void Settings::parse(const std::string& text) {
    Reader r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) r.kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    fields(*this, r);
    clamp();
}

void Settings::clamp() {
    auto fit = [](float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); };
    if (screenCount < 1) screenCount = 1;
    if (screenCount > 6) screenCount = 6;
    if (resW < 640 || resW > 7680) resW = 1920;
    if (resH < 480 || resH > 4320) resH = 1080;
    if (refreshHz < 24 || refreshHz > 240) refreshHz = 60;
    if ((int)source < 0 || (int)source > 1) source = Source::VirtualScreens;
    if ((int)layout.arrangement < 0 || (int)layout.arrangement >= (int)Arrangement::Count)
        layout.arrangement = Arrangement::CurvedRow;
    // older versions had "flat row" as its own arrangement
    if (layout.arrangement == Arrangement::FlatRow) {
        layout.arrangement = Arrangement::CurvedRow;
        layout.flat = true;
    }
    layout.panelWidthDeg = fit(layout.panelWidthDeg, 15, 120);
    layout.gapDeg = fit(layout.gapDeg, 0, 15);
    layout.pitchOffsetDeg = fit(layout.pitchOffsetDeg, -45, 45);
    if (diagonalFovDeg != 0) diagonalFovDeg = fit(diagonalFovDeg, 30, 90);
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    if ((int)vertical < 0 || (int)vertical >= (int)Vertical::Count) vertical = Vertical::Auto;
    if ((int)tracking < 0 || (int)tracking >= (int)Tracking::Count) tracking = Tracking::Head;
    smoothing = fit(smoothing, 0, 1);
    predictMs = fit(predictMs, 0, 60);
    if (sharpness < 0) sharpness = 0;
    if (sharpness > 100) sharpness = 100;
    travelSeconds = fit(travelSeconds, 1, 30);
    mouseFollowSeconds = fit(mouseFollowSeconds, 0.05f, 2);
    if (imuAddress.empty()) imuAddress = "169.254.2.1:52998";
    if (language != "es" && language != "en") language.clear();
}

} // namespace ox
