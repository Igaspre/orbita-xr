#pragma once
#include "util.h"

namespace ox {

struct Monitor {
    std::wstring gdi;       // \\.\DISPLAY3
    std::wstring name;      // EDID name
    std::wstring path;      // monitor device path
    std::wstring edidMfg;
    RECT rect{};
    int width = 0, height = 0, refresh = 0;
    int orientation = 0;    // DMDO_*
    bool primary = false;
    bool mirrorsPrimary = false;
    bool isVirtual = false;   // one of the driver's screens
    bool otherVirtual = false;
    bool looksLikeGlasses = false;

    std::wstring label() const;
};

std::vector<Monitor> listMonitors();  // active ones only

// Manual choice first, then the EDID name, then (if allowed) the only external
// screen with a glasses-like resolution.
const Monitor* findGlasses(const std::vector<Monitor>& mons, const std::wstring& manual, bool guess);
float guessFov(const std::wstring& modelName);

bool mirrorsPrimary(const std::vector<Monitor>& mons);
bool extendDesktop();
bool mirrorDesktop();

struct ModeChange {
    std::wstring gdi;
    int width = 0, height = 0, refresh = 0;  // 0 = keep
    int orientation = -1;                    // -1 = keep
    bool move = false;
    int x = 0, y = 0;
};
int applyModes(const std::vector<ModeChange>& changes, std::wstring* warnings);

// Screens of the virtual display driver, connected to the desktop or not.
int virtualScreensAvailable();
int virtualScreensActive();
// Connect the first `count` driver screens to the desktop and disconnect the rest.
bool setVirtualScreens(int count);

} // namespace ox
