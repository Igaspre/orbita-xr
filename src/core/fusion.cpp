#include "fusion.h"
#include <cmath>
#include <cstdio>

namespace ox {

static const float kG = 9.80665f;

static Quat fromTwoVectors(const Vec3& aIn, const Vec3& bIn) {
    Vec3 a = normalize(aIn), b = normalize(bIn);
    float d = clampf(dot(a, b), -1.0f, 1.0f);
    if (d > 0.999999f) return Quat::identity();
    if (d < -0.999999f) {
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-3f) axis = cross({0, 0, 1}, a);
        return Quat::fromAxisAngle(axis, kPi);
    }
    return Quat::fromAxisAngle(cross(a, b), std::acos(d));
}

std::string AxisCalibration::serialize() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d;%.6f,%.6f,%.6f;%.6f,%.6f,%.6f;%.6f,%.6f,%.6f;%.0f", (int)mode, right.x,
                  right.y, right.z, up.x, up.y, up.z, back.x, back.y, back.z, accelSign);
    return buf;
}

bool AxisCalibration::deserialize(const std::string& s) {
    int m = 0;
    float v[9];
    float sign = 1;
    int n = std::sscanf(s.c_str(), "%d;%f,%f,%f;%f,%f,%f;%f,%f,%f;%f", &m, &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6], &v[7], &v[8], &sign);
    if (n != 11 || m < 0 || m > 2) return false;
    for (float f : v)
        if (!std::isfinite(f)) return false;
    AxisCalibration c;
    c.mode = (Mode)m;
    c.right = normalize({v[0], v[1], v[2]});
    c.up = normalize({v[3], v[4], v[5]});
    c.back = normalize({v[6], v[7], v[8]});
    c.accelSign = sign >= 0 ? 1.0f : -1.0f;
    if (c.mode != None && length(c.up) < 0.5f) return false;
    if (c.mode == Full && (length(c.right) < 0.5f || length(c.back) < 0.5f)) return false;
    *this = c;
    return true;
}

void OrientationFilter::setCalibration(const AxisCalibration& c) {
    cal_ = c;
    reset();
}

void OrientationFilter::reset() {
    q_ = Quat::identity();
    rate_ = {};
    yaw_ = 0;
    ready_ = false;
    stillTime_ = 0;
    initSum_ = {};
    initCount_ = 0;
}

void OrientationFilter::update(const ImuSample& s, float dt) {
    if (!(dt >= 0) || dt > 0.1f) dt = 0.001f;
    float am = length(s.accel);

    if (cal_.mode == AxisCalibration::None) {
        if (!autoGravity) return;
        if (std::fabs(am - kG) < 0.8f && length(s.gyro) < 0.3f) {
            initSum_ += s.accel;
            initCount_++;
        }
        if (initCount_ >= 50) {
            AxisCalibration c;
            c.mode = AxisCalibration::GravityOnly;
            c.up = normalize(initSum_);
            setCalibration(c);
        }
        return;
    }

    Vec3 g = s.gyro - bias_;
    if (length(g) < 0.04f && std::fabs(am - kG) < 0.6f) stillTime_ += dt;
    else stillTime_ = 0;
    if (learnBias && stillTime_ > 1.0f) {
        bias_ += (s.gyro - bias_) * std::min(1.0f, dt / 3.0f);
        g = s.gyro - bias_;
    }

    if (!ready_) {
        initSum_ += s.accel;
        if (++initCount_ < 20) return;
        if (cal_.mode == AxisCalibration::Full) {
            Vec3 avg = initSum_ * (1.0f / (float)initCount_);
            q_ = fromTwoVectors(normalize(cal_.toHead(avg * cal_.accelSign)), {0, 1, 0});
            q_ = (yawOnly(q_).conj() * q_).normalized();  // start facing forward
        } else {
            yaw_ = 0;
            q_ = Quat::identity();
        }
        ready_ = true;
        return;
    }

    if (cal_.mode == AxisCalibration::GravityOnly) {
        float rate = dot(g, normalize(cal_.up)) * (invertYaw ? -1.0f : 1.0f);
        yaw_ = wrapPi(yaw_ + rate * dt);
        q_ = Quat::fromAxisAngle({0, 1, 0}, yaw_);
        rate_ += (Vec3(0, rate, 0) - rate_) * std::min(1.0f, dt / 0.004f);
        return;
    }

    Vec3 w = cal_.toHead(g);
    rate_ += (w - rate_) * std::min(1.0f, dt / 0.004f);
    if (std::fabs(am - kG) < 2.0f) {
        // pull the estimated "up" towards the measured gravity
        Vec3 measured = normalize(cal_.toHead(s.accel * cal_.accelSign));
        Vec3 estimated = q_.conj().rotate({0, 1, 0});
        w += cross(measured, estimated) * tiltGain;
    }
    q_ = (q_ * Quat::fromRotationVector(w * dt)).normalized();
}

void CalibrationWizard::start() {
    step_ = HoldStill;
    err_ = NoError;
    t_ = stillT_ = 0;
    accelSum_ = gyroSum_ = {};
    n_ = 0;
    turn_ = tilt_ = {};
    turnedLeft_ = false;
    result_ = AxisCalibration();
}

float CalibrationWizard::progress() const {
    switch (step_) {
    case HoldStill: return clampf(stillT_ / 1.5f, 0, 1);
    case TurnRight: return clampf(length(turn_) / deg2rad(35), 0, 1);
    case LookUp: return clampf(length(tilt_) / deg2rad(20), 0, 1);
    case Done: return 1;
    default: return 0;
    }
}

void CalibrationWizard::update(const ImuSample& s, float dt) {
    if (!active()) return;
    if (!(dt >= 0) || dt > 0.1f) dt = 0.001f;
    t_ += dt;

    switch (step_) {
    case HoldStill:
        if (length(s.gyro) < 0.08f && std::fabs(length(s.accel) - kG) < 1.0f) {
            stillT_ += dt;
            accelSum_ += s.accel;
            gyroSum_ += s.gyro;
            n_++;
        } else {
            stillT_ = 0;
            accelSum_ = gyroSum_ = {};
            n_ = 0;
        }
        if (stillT_ >= 1.5f && n_ > 0) {
            accelAvg_ = accelSum_ * (1.0f / (float)n_);
            bias_ = gyroSum_ * (1.0f / (float)n_);
            step_ = TurnRight;
            t_ = 0;
        } else if (t_ > 25) {
            fail(NeverStill);
        }
        break;

    case TurnRight:
        turn_ += (s.gyro - bias_) * dt;
        if (length(turn_) >= deg2rad(35)) {
            // turning right is a negative rotation around "up"
            Vec3 up = -normalize(turn_);
            float c = dot(normalize(accelAvg_), up);
            if (std::fabs(c) < 0.7f) {
                fail(TiltedTurn);
                break;
            }
            float sign = c >= 0 ? 1.0f : -1.0f;
            // Gravity can't tell left from right, but we know which way the
            // accelerometer points on these glasses. If it disagrees, the head
            // went left: flip, or every turn would come out mirrored.
            if (expectedAccelSign != 0 && sign != expectedAccelSign) {
                up = -up;
                sign = expectedAccelSign;
                turnedLeft_ = true;
            }
            result_.up = up;
            result_.accelSign = sign;
            step_ = LookUp;
            t_ = 0;
        } else if (t_ > 20) {
            fail(NoTurn);
        }
        break;

    case LookUp: {
        Vec3 v = s.gyro - bias_;
        Vec3 up = result_.up;
        v -= up * dot(v, up);  // ignore the turn back to the center
        tilt_ += v * dt;
        if (length(tilt_) >= deg2rad(20)) {
            Vec3 right = normalize(tilt_);  // looking up is a positive rotation around "right"
            Vec3 back = normalize(cross(right, up));
            result_.right = normalize(cross(up, back));
            result_.back = back;
            result_.mode = AxisCalibration::Full;
            step_ = Done;
        } else if (t_ > 20) {
            fail(NoLookUp);
        }
        break;
    }
    default: break;
    }
}

void HeadTracker::ingest(const ImuSample& s, float wallDt) {
    float dt = wallDt;
    if (lastTs_ != 0 && s.timestamp > lastTs_) {
        double raw = (double)(s.timestamp - lastTs_);
        if (tsScale_ == 0) {
            tsRawSum_ += raw;
            wallSum_ += wallDt;
            if (++tsCount_ >= 400 && tsRawSum_ > 0 && wallSum_ > 0.05) {
                double ratio = wallSum_ / tsRawSum_;
                const double units[3] = {1e-9, 1e-6, 1e-3};
                double best = -1, bestErr = 1e9;
                for (double u : units) {
                    double e = std::fabs(std::log10(ratio / u));
                    if (e < bestErr) { bestErr = e; best = u; }
                }
                tsScale_ = bestErr < 0.5 ? best : -1;  // -1: no idea, stick to the wall clock
            }
        }
        if (tsScale_ > 0) {
            float tsDt = (float)(raw * tsScale_);
            if (tsDt > 0 && tsDt < 0.1f) dt = tsDt;
        }
    }
    lastTs_ = s.timestamp;
    filter.update(s, clampf(dt, 0.0f, 0.05f));
    samples_++;
}

Quat HeadTracker::viewOrientation(float frameDt) {
    if (!filter.ready()) return out_;
    Quat q = filter.orientation();
    Quat target = bedMode ? q : yawOnly(q);
    if (recenterPending_ || !haveRef_) {
        ref_ = target;
        haveRef_ = true;
        recenterPending_ = false;
        out_ = (ref_.conj() * q).normalized();
        return out_;
    }
    if (freeze) return out_;
    if (travelMode && travelTimeConstant > 0.1f) ref_ = slerp(ref_, target, 1.0f - std::exp(-frameDt / travelTimeConstant));
    Quat rel = (ref_.conj() * q).normalized();
    if (followPitch) rel = yawOnly(rel);
    // Where the head will be by the time this frame is on the glasses. The gyro
    // is in head axes, so the extra turn goes on the right.
    Vec3 w = filter.angularVelocity();
    if (predictSeconds > 0) {
        rel = (rel * Quat::fromRotationVector(w * predictSeconds)).normalized();
        if (followPitch) rel = yawOnly(rel);
    }
    // Smoothing hides sensor noise when you hold still; while turning it would only
    // add lag, so it fades out with speed (~0.15 rad/s is a slow drift of the head).
    float speed = length(w) / 0.15f;
    float tau = smoothing * 0.08f / (1.0f + speed * speed);
    float a = tau <= 1e-4f ? 1.0f : 1.0f - std::exp(-frameDt / tau);
    out_ = slerp(out_, rel, clampf(a, 0, 1));
    return out_;
}

} // namespace ox
