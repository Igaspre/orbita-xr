#pragma once
#include "imu.h"
#include <string>

namespace ox {

// How the sensor axes map to head axes.
// GravityOnly: we only know which way is up, so only yaw is tracked.
// Full: filled in by the calibration wizard, full 3DoF.
struct AxisCalibration {
    enum Mode { None = 0, GravityOnly = 1, Full = 2 };
    Mode mode = None;
    Vec3 right{1, 0, 0};  // head axes expressed in sensor coordinates
    Vec3 up{0, 1, 0};
    Vec3 back{0, 0, 1};
    float accelSign = 1.0f;  // +1 if the accelerometer reads +g pointing up at rest

    Vec3 toHead(const Vec3& s) const { return {dot(s, right), dot(s, up), dot(s, back)}; }
    std::string serialize() const;
    bool deserialize(const std::string& s);
};

// Mahony style complementary filter. Gyro bias is re-learned whenever the
// glasses sit still for a second.
class OrientationFilter {
public:
    void setCalibration(const AxisCalibration& c);
    const AxisCalibration& calibration() const { return cal_; }
    void reset();
    void update(const ImuSample& s, float dt);

    Quat orientation() const { return q_; }  // head -> world
    bool ready() const { return ready_; }
    Vec3 gyroBias() const { return bias_; }
    Vec3 angularVelocity() const { return rate_; }  // head axes, rad/s
    void setGyroBias(const Vec3& b) { bias_ = b; }

    float tiltGain = 1.5f;
    bool learnBias = true;
    bool autoGravity = true;  // uncalibrated: guess "up" from gravity and track yaw only
    bool invertYaw = false;

private:
    AxisCalibration cal_;
    Quat q_;
    Vec3 bias_;
    Vec3 rate_;
    float yaw_ = 0;
    bool ready_ = false;
    float stillTime_ = 0;
    Vec3 initSum_;
    int initCount_ = 0;
};

// Hold still -> turn right -> look up. That's enough to recover all three axes.
class CalibrationWizard {
public:
    enum Step { Idle, HoldStill, TurnRight, LookUp, Done, Failed };
    enum Error { NoError, NeverStill, TiltedTurn, NoTurn, NoLookUp };

    void start();
    void cancel() { step_ = Idle; }
    void update(const ImuSample& s, float dt);
    bool active() const { return step_ == HoldStill || step_ == TurnRight || step_ == LookUp; }

    Step step() const { return step_; }
    Error error() const { return err_; }
    float progress() const;
    const AxisCalibration& result() const { return result_; }
    Vec3 measuredBias() const { return bias_; }

    // Which way the accelerometer points at rest (+1 up, -1 down) when the
    // uncalibrated mode turns the right way. Lets the wizard notice that you
    // turned left instead of right; 0 = unknown, trust the turn.
    float expectedAccelSign = 0;
    bool turnedLeft() const { return turnedLeft_; }

private:
    void fail(Error e) { step_ = Failed; err_ = e; }
    Step step_ = Idle;
    Error err_ = NoError;
    float t_ = 0, stillT_ = 0;
    Vec3 accelSum_, gyroSum_;
    int n_ = 0;
    Vec3 accelAvg_, bias_;
    Vec3 turn_, tilt_;
    bool turnedLeft_ = false;
    AxisCalibration result_;
};

// Turns the filter output into the view rotation: recentering, travel mode,
// "lying down" mode and a bit of smoothing.
class HeadTracker {
public:
    OrientationFilter filter;
    bool travelMode = false;
    float travelTimeConstant = 6;  // seconds
    bool bedMode = false;          // recenter pitch too, not just yaw
    float smoothing = 0.25f;       // 0..1, only applies while the head is (nearly) still
    float predictSeconds = 0.02f;  // look ahead to hide the time a frame takes to reach the eyes
    bool freeze = false;
    bool followPitch = false;      // screens go up and down with the head, only turning is tracked

    void ingest(const ImuSample& s, float wallDt);
    void recenter() { recenterPending_ = true; }
    Quat viewOrientation(float frameDt);
    uint64_t samples() const { return samples_; }
    double timestampScale() const { return tsScale_; }

private:
    Quat ref_, out_;
    bool haveRef_ = false;
    bool recenterPending_ = true;
    uint64_t lastTs_ = 0;
    uint64_t samples_ = 0;
    // timestamp units are figured out at runtime (ns on the units I know of)
    double tsScale_ = 0, tsRawSum_ = 0, wallSum_ = 0;
    int tsCount_ = 0;
};

} // namespace ox
