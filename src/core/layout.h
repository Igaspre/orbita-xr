#pragma once
#include "vmath.h"
#include <cstdint>
#include <vector>

namespace ox {

enum class Arrangement { CurvedRow = 0, FlatRow = 1, Grid = 2, Count };

struct LayoutParams {
    Arrangement arrangement = Arrangement::CurvedRow;  // CurvedRow (one row) or Grid (two rows)
    bool flat = false;                                  // flat panels instead of curved ones
    float panelWidthDeg = 38.0f;
    float gapDeg = 1.5f;
    float pitchOffsetDeg = 0.0f;
    // Rendering is mono, so the radius only matters for flat panels' perspective.
    float radius = 1.6f;
    int segments = 48;
};

struct PanelVertex {
    float x, y, z;
    float u, v;
};

struct Panel {
    float yaw = 0, pitch = 0;           // direction of the center
    float widthRad = 0, heightRad = 0;  // angular size
    std::vector<PanelVertex> verts;
    std::vector<uint16_t> indices;
};

// aspects[i] = width/height of screen i, left to right (grid: top row first).
// focusIndex/focusAmount pull one panel closer (zoom).
std::vector<Panel> computeLayout(const std::vector<float>& aspects, const LayoutParams& p, int focusIndex = -1,
                                 float focusAmount = 0.0f);

// Closest panel to a world direction, -1 if there are none.
int panelAt(const std::vector<Panel>& panels, const Vec3& dir, bool* inside = nullptr);

} // namespace ox
