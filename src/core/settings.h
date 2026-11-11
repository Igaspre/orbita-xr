#pragma once
#include "layout.h"
#include <string>

namespace ox {

enum class Source { VirtualScreens = 0, ExistingMonitors = 1 };
enum class Tracking { Head = 0, Mouse = 1, Fixed = 2, Count };
// Up and down: Auto follows the head with one row and stays put with two.
enum class Vertical { Auto = 0, Follow = 1, Fixed = 2, Count };

struct Settings {
    // screens
    int screenCount = 3;  // 1..6
    int resW = 1920, resH = 1080;
    int refreshHz = 60;
    Source source = Source::VirtualScreens;
    std::string existingMonitors;  // GDI names, ';' separated
    std::string portrait;          // "001000": which screens are rotated
    bool includePrimary = false;
    bool autoArrange = true;
    bool autoExtend = true;
    std::string glassesOverride;   // GDI name, empty = auto
    std::string vddGpu;            // GPU that draws the virtual screens, empty = the driver picks

    // look
    LayoutParams layout;
    float diagonalFovDeg = 0;  // 0 = guess from the model
    int brightness = 100;
    bool highlightFocused = true;
    bool showCursor = true;
    bool mipmaps = true;
    Vertical vertical = Vertical::Auto;
    int sharpness = 35;  // 0..100

    // tracking
    Tracking tracking = Tracking::Head;
    float smoothing = 0.25f;
    float predictMs = 20;  // 0..60
    bool travelMode = false;
    float travelSeconds = 6.0f;
    bool bedMode = false;
    bool invertYaw = false;
    bool cursorFollowsGaze = false;
    bool cursorGuard = true;
    bool dropToLaptop = true;  // push down on any bottom screen to get to the laptop
    float mouseFollowSeconds = 0.35f;
    std::string calibration;
    std::string imuAddress = "169.254.2.1:52998";

    // app
    bool runAtLogin = false;
    bool startHidden = false;
    bool autoStart = true;
    std::string language;  // "es" or "en", empty = same as Windows

    std::string serialize() const;
    void parse(const std::string& text);
    void clamp();
    bool followPitch() const {
        return vertical == Vertical::Follow || (vertical == Vertical::Auto && layout.arrangement != Arrangement::Grid);
    }
};

} // namespace ox
