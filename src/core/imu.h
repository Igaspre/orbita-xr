#pragma once
// IMU stream of the XREAL One family (One, One Pro, 1S).
// The glasses expose a USB network adapter and push 84 byte frames over TCP
// on 169.254.2.1:52998. Frame layout from rohitsangwan01/xreal_one_driver (MIT).
#include "vmath.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ox {

struct ImuSample {
    Vec3 gyro;   // rad/s, sensor axes
    Vec3 accel;  // m/s^2, sensor axes
    uint64_t timestamp = 0;
};

class FrameParser {
public:
    static constexpr size_t kFrameSize = 84;

    void feed(const uint8_t* data, size_t len);
    bool next(ImuSample& out);
    void reset() { buf_.clear(); pos_ = 0; rejected_ = 0; }

    size_t buffered() const { return buf_.size() - pos_; }
    uint64_t rejected() const { return rejected_; }

    static bool decode(const uint8_t* frame, ImuSample& out);

private:
    void compact();
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    uint64_t rejected_ = 0;
};

// Wraps the parser and assigns a time step to each sample. A recv() that only
// returns half a frame must not eat wall time, so the clock only advances when
// complete samples come out.
class ImuStream {
public:
    template <class F>
    int onBytes(const uint8_t* data, size_t len, double now, F&& sink) {
        parser_.feed(data, len);
        batch_.clear();
        ImuSample s;
        while (parser_.next(s)) batch_.push_back(s);
        if (batch_.empty()) return 0;
        double wall = haveLast_ ? now - last_ : 0.001 * (double)batch_.size();
        haveLast_ = true;
        last_ = now;
        float per = (float)(wall / (double)batch_.size());
        for (auto& b : batch_) sink(b, per);
        return (int)batch_.size();
    }
    void reset() { parser_.reset(); haveLast_ = false; }

private:
    FrameParser parser_;
    std::vector<ImuSample> batch_;
    double last_ = 0;
    bool haveLast_ = false;
};

} // namespace ox
