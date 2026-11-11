#pragma once
#include "util.h"
#include "imu_client.h"
#include "../core/settings.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace ox {

enum class Command { Recenter, Freeze, Zoom, NextScreen, PrevScreen, CursorToGaze };

struct RenderSetup {
    std::wstring glasses;         // GDI name of the glasses, empty for demo
    RECT glassesRect{};
    std::vector<std::wstring> screens;  // left to right (grid: top row first)
    float fov = 52;               // diagonal, used when the setting is on "auto"
    // demo: plain window with test pictures instead of real screens
    bool demo = false;
    int demoScreens = 3;
    std::wstring screenshot;      // demo: save a .bmp and quit
    float screenshotAfter = 0.5f;
};

struct RenderStats {
    float fps = 0;
    int gaze = -1;
    bool mouseFallback = false;
    int live = 0, total = 0;
    std::string error;
};

// Owns the window on the glasses and draws on its own thread.
class Renderer {
public:
    ~Renderer() { stop(); }
    bool start(const RenderSetup& setup, const Settings& s, ImuClient* imu);
    void stop();
    bool running() const { return running_; }
    bool finished() const { return finished_; }
    void apply(const Settings& s);
    void send(Command c);
    void toast(const std::wstring& text, float seconds = 2.5f);
    RenderStats stats();
    std::string error();

private:
    void run();
    RenderSetup setup_;
    ImuClient* imu_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false}, running_{false}, finished_{false};
    HANDLE started_ = nullptr;
    std::mutex m_;
    Settings settings_;
    bool settingsChanged_ = true;
    std::deque<Command> commands_;
    std::wstring toast_;
    float toastSeconds_ = 0;
    bool toastPending_ = false;
    RenderStats stats_;
};

} // namespace ox
