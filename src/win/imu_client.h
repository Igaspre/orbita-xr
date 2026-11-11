#pragma once
#include "util.h"
#include "../core/fusion.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace ox {

// Reads the glasses' IMU over TCP on its own thread and feeds the head tracker.
class ImuClient {
public:
    ~ImuClient() { stop(); }
    void start(const std::string& address);
    void stop();

    bool connected() const { return connected_; }
    float rate() const { return rate_; }
    float takeMaxGapMs() { return maxGapMs_.exchange(0); }  // longest wait for data since the last call
    bool ready();

    template <class F>
    auto with(F&& f) {
        std::lock_guard<std::mutex> lk(m_);
        return f(tracker_, wizard_);
    }
    Quat view(float frameDt);
    void recenter();

    void startCalibration();
    // true once, when the wizard has just finished (ok or not)
    bool calibrationFinished(AxisCalibration& cal, bool& ok, CalibrationWizard::Error& err);

private:
    void run(std::string address);
    std::mutex m_;
    HeadTracker tracker_;
    CalibrationWizard wizard_;
    bool wizardPending_ = false;
    std::thread thread_;
    std::atomic<bool> running_{false}, connected_{false};
    std::atomic<float> rate_{0}, maxGapMs_{0};
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
};

} // namespace ox
