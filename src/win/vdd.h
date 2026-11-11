#pragma once
// Virtual Display Driver (github.com/VirtualDrivers/Virtual-Display-Driver, MIT).
// A signed copy for the right architecture is embedded in the exe and installed
// on demand, so there is nothing to download separately.
#include "util.h"

namespace ox::vdd {

enum class State { Missing, Ready, Broken };

State state();
bool ours();              // installed by Orbita XR (our marker is in its config file)
int configuredCount();    // screens in the driver config, -1 if unknown
bool setCount(int n);     // through the driver's pipe, used only for installs that aren't ours
// Which GPU draws the virtual screens: a DXGI adapter name, or "default" (the
// one with the most video memory). Changing it restarts the driver, no admin needed.
std::wstring configuredGpu();
bool setGpu(const std::wstring& name);

constexpr int kScreens = 6;  // what we install it with

// These need admin rights; the app re-launches itself elevated to run them.
DWORD install();
DWORD uninstall();

// Starts this exe elevated with `arg` and waits. ERROR_CANCELLED if the UAC prompt was declined.
DWORD runElevated(const wchar_t* arg);

} // namespace ox::vdd
