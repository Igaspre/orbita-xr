#include "selftest.h"
#include "fusion.h"
#include "imu.h"
#include "layout.h"
#include "settings.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ox {
namespace {

struct Check {
    const std::function<void(const std::string&)>& print;
    int failed = 0, total = 0;

    void ok(bool cond, const char* what) {
        total++;
        if (!cond) {
            failed++;
            print(std::string("    failed: ") + what);
        }
    }
    void near(float got, float want, float tol, const char* what) {
        total++;
        if (std::fabs(got - want) <= tol) return;
        failed++;
        char b[256];
        std::snprintf(b, sizeof(b), "    failed: %s (got %.4f, want %.4f +- %.4f)", what, got, want, tol);
        print(b);
    }
};

std::vector<uint8_t> frame(const Vec3& g, const Vec3& a, uint64_t ts) {
    static const uint8_t hdr[6] = {0x28, 0x36, 0x00, 0x00, 0x00, 0x80};
    static const uint8_t tag[6] = {0x00, 0x40, 0x1f, 0x00, 0x00, 0x40};
    std::vector<uint8_t> f(FrameParser::kFrameSize, 0);
    std::memcpy(f.data(), hdr, 6);
    std::memcpy(f.data() + 60, tag, 6);
    for (int i = 0; i < 8; i++) f[14 + i] = (uint8_t)(ts >> (8 * i));
    const float v[6] = {g.x, g.y, g.z, a.x, a.y, a.z};
    std::memcpy(f.data() + 34, v, sizeof(v));  // x86 and arm64 are both little endian
    return f;
}

// Fake glasses: head orientation in, sensor readings out.
struct FakeGlasses {
    Quat head;
    Quat mount;           // sensor = mount.rotate(head vector)
    float accelSign = 1;
    Vec3 bias;
    uint64_t ts = 1000000;
    float dt = 1.0f / 500.0f;

    ImuSample step(const Vec3& omegaHead) {
        head = (head * Quat::fromRotationVector(omegaHead * dt)).normalized();
        ImuSample s;
        s.gyro = mount.rotate(omegaHead) + bias;
        s.accel = mount.rotate(head.conj().rotate({0, 9.80665f, 0})) * accelSign;
        ts += (uint64_t)(dt * 1e9);
        s.timestamp = ts;
        return s;
    }

    // yaw: left positive, pitch: up positive, both in degrees
    template <class F>
    void move(float yawDeg, float pitchDeg, float secs, F&& sink) {
        int n = (int)(secs / dt);
        for (int i = 0; i < n; i++) {
            Vec3 upInHead = head.conj().rotate({0, 1, 0});
            Vec3 omega = upInHead * (deg2rad(yawDeg) / secs) + Vec3(deg2rad(pitchDeg) / secs, 0, 0);
            sink(step(omega));
        }
    }
};

Quat oddMount() { return Quat::fromAxisAngle(normalize({0.3f, -1.0f, 0.5f}), deg2rad(127)); }

void mathTests(Check& c) {
    Quat q = Quat::fromYawPitch(deg2rad(30), deg2rad(10));
    c.near(rad2deg(yawOf(q)), 30, 0.01f, "yawOf");
    c.near(rad2deg(pitchOf(q)), 10, 0.01f, "pitchOf");
    Vec3 v(0.3f, -0.2f, 0.9f), a = q.rotate(v), b = transformPoint(v, Mat4::fromQuat(q));
    c.near(a.x, b.x, 1e-5f, "quat vs matrix (x)");
    c.near(a.y, b.y, 1e-5f, "quat vs matrix (y)");
    c.near(a.z, b.z, 1e-5f, "quat vs matrix (z)");
    c.near(dirFromYawPitch(deg2rad(30), deg2rad(10)).x, q.forward().x, 1e-5f, "dirFromYawPitch");
    float w;
    Vec3 p = transformPoint({0, 0, -2}, Mat4::perspectiveRH(deg2rad(30), 1.6f, 0.05f, 100), &w);
    c.near(p.x / w, 0, 1e-5f, "projection center");
    c.ok(p.z / w > 0 && p.z / w < 1, "depth in 0..1");
    c.near(rad2deg(verticalFovFromDiagonal(deg2rad(52), 1920.0f / 1200.0f)), 28.98f, 0.1f, "1S vertical fov");
}

void parserTests(Check& c) {
    FrameParser p;
    ImuSample s;
    std::vector<uint8_t> data = {1, 2, 3, 0x28, 0x36, 9};  // junk and a fake half header first
    auto f1 = frame({0.1f, -0.2f, 0.3f}, {0.0f, 9.8f, 0.1f}, 123456789ull);
    auto f2 = frame({1, 2, 3}, {9.8f, 0, 0}, 223456789ull);
    data.insert(data.end(), f1.begin(), f1.end());
    data.insert(data.end(), f2.begin(), f2.end());
    int got = 0;
    for (size_t i = 0; i < data.size(); i += 7) {
        p.feed(data.data() + i, std::min<size_t>(7, data.size() - i));
        while (p.next(s)) {
            if (++got == 1) {
                c.near(s.gyro.y, -0.2f, 1e-6f, "gyro value");
                c.near(s.accel.y, 9.8f, 1e-6f, "accel value");
                c.ok(s.timestamp == 123456789ull, "timestamp");
            }
        }
    }
    c.ok(got == 2, "two frames out of a 7 byte trickle");
    c.ok(!FrameParser::decode(frame({NAN, 0, 0}, {0, 9.8f, 0}, 1).data(), s), "rejects NaN");
    c.ok(!FrameParser::decode(frame({0, 0, 0}, {0, 0.5f, 0}, 1).data(), s), "rejects impossible gravity");
    std::vector<uint8_t> junk(300000);
    for (size_t i = 0; i < junk.size(); i++) junk[i] = (uint8_t)(i * 131 + 7);
    p.feed(junk.data(), junk.size());
    while (p.next(s)) {}
    c.ok(p.buffered() < 200, "drops junk");
}

void calibrationTests(Check& c) {
    FakeGlasses g;
    g.mount = oddMount();
    g.accelSign = -1;
    g.bias = {0.004f, -0.003f, 0.002f};

    CalibrationWizard wz;
    wz.start();
    auto feed = [&](const ImuSample& s) { wz.update(s, g.dt); };
    g.move(0, 0, 2.0f, feed);
    c.ok(wz.step() == CalibrationWizard::TurnRight, "wizard: turn right step");
    g.move(-45, 0, 1.0f, feed);
    g.move(0, 0, 0.3f, feed);
    c.ok(wz.step() == CalibrationWizard::LookUp, "wizard: look up step");
    g.move(45, 0, 1.0f, feed);  // back to center, must be ignored
    g.move(0, 25, 1.0f, feed);
    c.ok(wz.step() == CalibrationWizard::Done, "wizard finished");
    AxisCalibration cal = wz.result();
    c.ok(cal.accelSign < 0, "accelerometer sign");
    c.near(cal.toHead(g.mount.rotate({1, 0, 0})).x, 1, 0.05f, "right axis");
    c.near(cal.toHead(g.mount.rotate({0, 1, 0})).y, 1, 0.05f, "up axis");
    AxisCalibration back;
    c.ok(back.deserialize(cal.serialize()), "calibration round trip");
    c.near(back.up.y, cal.up.y, 1e-4f, "calibration round trip value");

    g.head = Quat::identity();
    HeadTracker ht;
    ht.smoothing = 0;
    ht.predictSeconds = 0;
    ht.filter.setCalibration(cal);
    ht.filter.setGyroBias(wz.measuredBias());
    auto sink = [&](const ImuSample& s) { ht.ingest(s, g.dt); };
    g.move(0, 0, 0.5f, sink);
    ht.viewOrientation(0.016f);
    g.move(30, 0, 1.0f, sink);
    c.near(rad2deg(yawOf(ht.viewOrientation(0.016f))), 30, 1.5f, "follows yaw");
    g.move(0, 15, 0.5f, sink);
    c.near(rad2deg(pitchOf(ht.viewOrientation(0.016f))), 15, 1.5f, "follows pitch");
    ht.recenter();
    Quat v = ht.viewOrientation(0.016f);
    c.near(rad2deg(yawOf(v)), 0, 0.5f, "recenter resets yaw");
    c.near(rad2deg(pitchOf(v)), 15, 1.5f, "recenter keeps pitch");
    ht.followPitch = true;
    c.near(rad2deg(pitchOf(ht.viewOrientation(0.016f))), 0, 0.5f, "follow pitch: view stays level");
    c.near(rad2deg(yawOf(ht.viewOrientation(0.016f))), 0, 0.5f, "follow pitch: yaw still tracked");
    ht.followPitch = false;
    ht.bedMode = true;
    ht.recenter();
    c.near(rad2deg(pitchOf(ht.viewOrientation(0.016f))), 0, 0.5f, "bed mode recenters pitch");

    // unknown bias, 20 s still: bias learning should keep drift small
    HeadTracker h2;
    h2.smoothing = 0;
    h2.predictSeconds = 0;
    h2.filter.setCalibration(cal);
    FakeGlasses g2 = g;
    g2.head = Quat::identity();
    g2.bias = {0.01f, 0.01f, -0.01f};
    auto sink2 = [&](const ImuSample& s) { h2.ingest(s, g2.dt); };
    g2.move(0, 0, 0.2f, sink2);
    h2.viewOrientation(0.016f);
    g2.move(0, 0, 20.0f, sink2);
    c.ok(std::fabs(rad2deg(yawOf(h2.viewOrientation(0.016f)))) < 3.0f, "drift while still");

    // travel mode: after a sustained turn the reference catches up
    HeadTracker h3;
    h3.smoothing = 0;
    h3.predictSeconds = 0;
    h3.travelMode = true;
    h3.travelTimeConstant = 2.0f;
    h3.filter.setCalibration(cal);
    h3.filter.setGyroBias(g.bias);
    FakeGlasses g3 = g;
    g3.head = Quat::identity();
    auto sink3 = [&](const ImuSample& s) { h3.ingest(s, g3.dt); };
    g3.move(0, 0, 0.2f, sink3);
    h3.viewOrientation(0.016f);
    g3.move(60, 0, 0.5f, sink3);
    for (int i = 0; i < 600; i++) {
        g3.move(0, 0, 1.0f / 60.0f, sink3);
        h3.viewOrientation(1.0f / 60.0f);
    }
    c.ok(std::fabs(rad2deg(yawOf(h3.viewOrientation(0.016f)))) < 5.0f, "travel mode recenters");
}

// Turning at a steady 60 deg/s and drawing at 60 fps.
void latencyTests(Check& c) {
    AxisCalibration cal;
    cal.mode = AxisCalibration::Full;
    auto turning = [&](float smoothing, float predict) {
        FakeGlasses g;
        HeadTracker t;
        t.smoothing = smoothing;
        t.predictSeconds = predict;
        t.filter.setCalibration(cal);
        auto sink = [&](const ImuSample& s) { t.ingest(s, g.dt); };
        g.move(0, 0, 0.3f, sink);
        t.viewOrientation(1.0f / 60);
        float err = 0;
        for (int i = 0; i < 30; i++) {
            g.move(1, 0, 1.0f / 60, sink);
            err = rad2deg(yawOf(t.viewOrientation(1.0f / 60)) - yawOf(g.head));
        }
        return err;  // positive: ahead of the head
    };
    c.near(turning(0, 0), 0, 0.3f, "no smoothing, no prediction: on time");
    c.near(turning(1, 0), 0, 0.6f, "full smoothing doesn't lag while turning");
    c.near(turning(0, 0.05f), 3.0f, 0.4f, "prediction leads by speed x time");

    // holding still with a noisy gyro: smoothing should still calm it down
    auto jitter = [&](float smoothing) {
        FakeGlasses g;
        HeadTracker t;
        t.smoothing = smoothing;
        t.filter.setCalibration(cal);
        t.filter.learnBias = false;
        auto sink = [&](const ImuSample& s) { t.ingest(s, g.dt); };
        g.move(0, 0, 0.3f, sink);
        t.viewOrientation(1.0f / 60);
        unsigned seed = 7;
        float prev = 0, sum = 0;
        for (int i = 0; i < 240; i++) {
            for (int k = 0; k < 8; k++) {
                seed = seed * 1103515245u + 12345u;
                float n = ((float)((seed >> 8) & 0xffff) / 65535.0f - 0.5f) * 0.12f;  // +-3.4 deg/s noise
                ImuSample s = g.step({0, 0, 0});
                s.gyro.y += n;
                sink(s);
            }
            float y = rad2deg(yawOf(t.viewOrientation(1.0f / 60)));
            if (i > 0) sum += std::fabs(y - prev);
            prev = y;
        }
        return sum;
    };
    c.ok(jitter(1) < jitter(0) * 0.6f, "smoothing calms a still head");
}

// Turning left instead of right would mirror every turn afterwards. Gravity can't
// tell (looking down instead of up can't be told apart at all, so that one stays
// on the user), but the known accelerometer direction can.
void wrongWayTests(Check& c) {
    for (float sign : {-1.0f, 1.0f}) {
        FakeGlasses g;
        g.mount = oddMount();
        g.accelSign = sign;
        CalibrationWizard wz;
        wz.start();
        wz.expectedAccelSign = sign;
        auto feed = [&](const ImuSample& s) { wz.update(s, g.dt); };
        g.move(0, 0, 2.0f, feed);
        g.move(45, 0, 1.0f, feed);  // left
        g.move(0, 0, 0.3f, feed);
        g.move(-45, 0, 1.0f, feed);
        g.move(0, 25, 1.0f, feed);
        c.ok(wz.step() == CalibrationWizard::Done && wz.turnedLeft(), "turned left: noticed");
        AxisCalibration cal = wz.result();
        c.near(cal.toHead(g.mount.rotate({0, 1, 0})).y, 1, 0.05f, "turned left: up axis");
        c.near(cal.toHead(g.mount.rotate({1, 0, 0})).x, 1, 0.05f, "turned left: right axis");
        c.ok(cal.accelSign == sign, "turned left: accelerometer sign");
    }
}

void uncalibratedTests(Check& c) {
    FakeGlasses g;
    g.mount = Quat::fromAxisAngle(normalize({1, 0.2f, -0.4f}), deg2rad(70));
    HeadTracker ht;
    ht.smoothing = 0;
    ht.predictSeconds = 0;
    auto sink = [&](const ImuSample& s) { ht.ingest(s, g.dt); };
    g.move(0, 0, 0.5f, sink);
    c.ok(ht.filter.calibration().mode == AxisCalibration::GravityOnly, "auto calibration from gravity");
    g.move(0, 0, 0.2f, sink);
    ht.viewOrientation(0.016f);
    g.move(-40, 0, 1.0f, sink);
    c.near(rad2deg(yawOf(ht.viewOrientation(0.016f))), -40, 2.0f, "yaw only: 40 deg right");
}

void timestampTests(Check& c) {
    const uint64_t div[3] = {1, 1000, 1000000};
    const double want[3] = {1e-9, 1e-6, 1e-3};
    for (int k = 0; k < 3; k++) {
        FakeGlasses g;
        if (k == 2) g.dt = 1.0f / 200.0f;  // ms resolution is coarse
        HeadTracker ht;
        ht.smoothing = 0;
        AxisCalibration cal;
        cal.mode = AxisCalibration::GravityOnly;
        ht.filter.setCalibration(cal);
        auto sink = [&](const ImuSample& s0) {
            ImuSample s = s0;
            s.timestamp = s0.timestamp / div[k];
            ht.ingest(s, g.dt);
        };
        g.move(0, 0, 3.0f, sink);
        ht.viewOrientation(0.016f);
        g.move(50, 0, 1.0f, sink);
        c.ok(std::fabs(ht.timestampScale() / want[k] - 1.0) < 1e-6, "timestamp unit detected");
        c.near(rad2deg(yawOf(ht.viewOrientation(0.016f))), 50, 2.5f, "turn with scaled timestamps");
    }
}

// Bytes arrive in small pieces, each a bit later than the previous one, in
// 4 ms bursts, and the head starts turning before timestamp units are known.
void streamTests(Check& c) {
    FakeGlasses g;
    g.mount = oddMount();
    std::vector<uint8_t> bytes;
    std::vector<double> when;
    double t = 0;
    auto add = [&](const ImuSample& s) {
        auto f = frame(s.gyro, s.accel, s.timestamp);
        bytes.insert(bytes.end(), f.begin(), f.end());
        t += g.dt;
        when.push_back(t);
    };
    g.move(0, 0, 0.2f, add);
    g.move(20, 0, 0.3f, add);
    g.move(0, 0, 1.0f, add);
    g.move(15, 0, 0.5f, add);
    g.move(0, 0, 0.5f, add);

    ImuStream st;
    HeadTracker ht;
    ht.smoothing = 0;
    AxisCalibration cal;
    cal.mode = AxisCalibration::GravityOnly;
    cal.up = g.mount.rotate({0, 1, 0});
    ht.filter.setCalibration(cal);
    size_t pos = 0, step = 37;
    double tick = 0;
    bool ref = false;
    while (pos < bytes.size()) {
        tick += 0.004;
        size_t avail = 0;
        while (avail < when.size() && when[avail] <= tick) avail++;
        size_t limit = avail * FrameParser::kFrameSize;
        double now = tick;
        while (pos < limit) {
            size_t n = std::min(step, limit - pos);
            st.onBytes(bytes.data() + pos, n, now, [&](const ImuSample& s, float dt) { ht.ingest(s, dt); });
            pos += n;
            now += 0.0003;
            step = 37 + (pos % 211);
        }
        if (!ref && ht.filter.ready()) {
            ht.viewOrientation(0.016f);
            ref = true;
        }
    }
    c.near(rad2deg(yawOf(ht.viewOrientation(0.016f))), 35, 2.5f, "turn with chopped tcp data");
    c.ok(std::fabs(ht.timestampScale() / 1e-9 - 1.0) < 1e-6, "units detected with chopped data");
}

void layoutTests(Check& c) {
    LayoutParams p;
    p.panelWidthDeg = 40;
    p.gapDeg = 2;
    std::vector<float> three(3, 16.0f / 9.0f);
    auto L = computeLayout(three, p);
    c.ok(L.size() == 3, "three panels");
    c.near(rad2deg(L[1].yaw), 0, 1e-3f, "middle panel in front");
    c.near(rad2deg(L[0].yaw), 42, 1e-3f, "left panel");
    c.near(rad2deg(L[2].yaw), -42, 1e-3f, "right panel");
    c.near(rad2deg(L[1].widthRad), 40, 0.01f, "angular width");
    c.ok(panelAt(L, {0, 0, -1}) == 1, "looking ahead hits the middle one");
    c.ok(panelAt(L, dirFromYawPitch(deg2rad(-40), 0)) == 2, "looking right hits the right one");
    c.ok(L[0].verts[0].x < 0 && L[0].verts[0].u == 0, "uv runs left to right");
    p.arrangement = Arrangement::Grid;
    auto G = computeLayout(std::vector<float>(5, 16.0f / 9.0f), p);
    c.ok(G[0].pitch > 0 && G[4].pitch < 0, "grid has two rows");
    p.flat = true;
    auto FG = computeLayout(std::vector<float>(6, 16.0f / 9.0f), p);
    c.ok(FG[0].pitch > 0 && FG[5].pitch < 0 && FG[0].indices.size() == 6, "two rows of flat panels");
    p.arrangement = Arrangement::CurvedRow;
    auto F = computeLayout(three, p, 1, 1.0f);
    c.ok(F[1].widthRad > deg2rad(40), "zoom brings the panel closer");
    for (auto& f : F) c.ok(f.indices.size() == 6, "flat panel is a quad");
}

void settingsTests(Check& c) {
    Settings a;
    a.screenCount = 5;
    a.layout.panelWidthDeg = 55;
    a.tracking = Tracking::Mouse;
    a.existingMonitors = "\\\\.\\DISPLAY3;\\\\.\\DISPLAY4";
    Settings b;
    b.parse(a.serialize());
    c.ok(b.screenCount == 5, "screens round trip");
    c.near(b.layout.panelWidthDeg, 55, 1e-3f, "panel width round trip");
    c.ok(b.tracking == Tracking::Mouse, "tracking round trip");
    c.ok(b.existingMonitors == a.existingMonitors, "monitor list round trip");
    Settings bad;
    bad.parse("screens=99\nwidth=1\ngap=abc\nbrightness=-5\n");
    c.ok(bad.screenCount == 6 && bad.resW == 1920 && bad.brightness == 10, "bad values are clamped");
    Settings e;
    e.layout.arrangement = Arrangement::Grid;
    c.ok(!e.followPitch(), "two rows stay put vertically");
    e.layout.arrangement = Arrangement::CurvedRow;
    c.ok(e.followPitch(), "one row follows up and down");
    Settings old;
    old.parse("arrangement=1\n");
    c.ok(old.layout.arrangement == Arrangement::CurvedRow && old.layout.flat, "old flat row setting carries over");
    Settings d;
    d.parse("sharpness=250\npredict_ms=-3\n");
    c.ok(d.sharpness == 100 && d.predictMs == 0, "sharpness and prediction are clamped");
    Settings en, odd;
    en.parse("language=en\n");
    odd.parse("language=fr\n");
    c.ok(en.language == "en" && odd.language.empty() && Settings().language.empty(), "language setting");
}

} // namespace

int runSelfTest(const std::function<void(const std::string&)>& print) {
    Check c{print};
    struct { const char* name; void (*fn)(Check&); } suites[] = {
        {"math", mathTests},
        {"imu frames", parserTests},
        {"calibration and 3dof", calibrationTests},
        {"uncalibrated tracking", uncalibratedTests},
        {"latency", latencyTests},
        {"calibration turning the wrong way", wrongWayTests},
        {"timestamp units", timestampTests},
        {"chopped tcp stream", streamTests},
        {"layout", layoutTests},
        {"settings", settingsTests},
    };
    for (auto& s : suites) {
        int before = c.failed;
        s.fn(c);
        print(std::string(c.failed == before ? "ok    " : "FAIL  ") + s.name);
    }
    char b[96];
    std::snprintf(b, sizeof(b), "%d checks, %d failed", c.total, c.failed);
    print(b);
    return c.failed;
}

} // namespace ox
