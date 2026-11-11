#pragma once
#include "imu_client.h"
#include "monitors.h"
#include "renderer.h"
#include <functional>

namespace ox {

struct StartResult {
    bool ok = false;
    bool needsDriver = false;
    std::wstring message;
};

// Everything between pressing Start and Stop: virtual screens, IMU, renderer.
// start() and stop() block, call them from a worker thread.
class Session {
public:
    ~Session() { stop(); }
    StartResult start(const Settings& s, const std::function<void(const std::wstring&)>& progress);
    void stop();
    bool running() const { return running_; }

    Renderer& renderer() { return renderer_; }
    ImuClient& imu() { return imu_; }
    RECT glassesRect() const { return glassesRect_; }
    std::wstring glassesName() const { return glassesName_; }
    std::wstring virtualGpu() const { return virtualGpu_; }  // where the virtual screens are drawn
    bool glassesConnected() const;
    void apply(const Settings& s);
    // switching between one and two rows: move the screens in Windows too
    bool rearrange(const Settings& s);

private:
    void placeVirtual(const Settings& s, std::vector<Monitor>& mons);
    std::atomic<bool> running_{false};
    bool active_ = false;  // something to undo
    int virtualBefore_ = -1;
    int countBefore_ = -1;
    bool usedVirtual_ = false;
    bool switchedToExtend_ = false;
    RECT glassesRect_{};
    std::wstring glassesName_, glassesGdi_, virtualGpu_;
    ImuClient imu_;
    Renderer renderer_;
};

} // namespace ox
