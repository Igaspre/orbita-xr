#include "imu.h"
#include <cmath>
#include <cstring>

namespace ox {

static const uint8_t kHeader[6] = {0x28, 0x36, 0x00, 0x00, 0x00, 0x80};
static const uint8_t kSensorTag[6] = {0x00, 0x40, 0x1f, 0x00, 0x00, 0x40};

static bool contains(const uint8_t* hay, size_t n, const uint8_t* needle, size_t m) {
    for (size_t i = 0; i + m <= n; i++)
        if (std::memcmp(hay + i, needle, m) == 0) return true;
    return false;
}

static float f32le(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

static uint64_t u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

bool FrameParser::decode(const uint8_t* f, ImuSample& out) {
    if (std::memcmp(f, kHeader, 6) != 0) return false;
    if (!contains(f, kFrameSize, kSensorTag, 6)) return false;
    float g[3], a[3];
    for (int i = 0; i < 3; i++) {
        g[i] = f32le(f + 34 + i * 4);
        a[i] = f32le(f + 46 + i * 4);
        if (!std::isfinite(g[i]) || !std::isfinite(a[i])) return false;
        if (std::fabs(g[i]) > 1000.0f || std::fabs(a[i]) > 100.0f) return false;
    }
    float gm = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    float am = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (gm < 1e-6f && am < 1e-6f) return false;
    if (am < 5.0f || am > 15.0f) return false;  // should be roughly 1 g
    out.gyro = {g[0], g[1], g[2]};
    out.accel = {a[0], a[1], a[2]};
    out.timestamp = u64le(f + 14);
    return true;
}

void FrameParser::feed(const uint8_t* data, size_t len) {
    if (!data || !len) return;
    compact();
    buf_.insert(buf_.end(), data, data + len);
    if (buffered() > (1u << 20)) { buf_.clear(); pos_ = 0; }  // garbage, start over
}

void FrameParser::compact() {
    if (pos_ > 0 && (pos_ > 4096 || pos_ * 2 > buf_.size())) {
        buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)pos_);
        pos_ = 0;
    }
}

bool FrameParser::next(ImuSample& out) {
    for (;;) {
        size_t n = buf_.size() - pos_;
        if (n < 6) return false;
        const uint8_t* p = buf_.data() + pos_;
        size_t h = n;
        for (size_t i = 0; i + 6 <= n; i++)
            if (std::memcmp(p + i, kHeader, 6) == 0) { h = i; break; }
        if (h == n) {
            pos_ += n - 5;  // keep a possible partial header
            return false;
        }
        pos_ += h;
        if (n - h < kFrameSize) return false;
        if (decode(buf_.data() + pos_, out)) {
            pos_ += kFrameSize;
            return true;
        }
        rejected_++;
        pos_ += 6;
    }
}

} // namespace ox
