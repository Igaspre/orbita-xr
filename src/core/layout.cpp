#include "layout.h"
#include <cmath>

namespace ox {

// Piece of a cylinder around the viewer. u runs left to right, v top to bottom.
static void buildCurved(Panel& g, float yawCenter, float yCenter, float W, float H, float R, int seg) {
    if (seg < 2) seg = 2;
    float a = W / R;
    g.verts.clear();
    g.indices.clear();
    for (int i = 0; i <= seg; i++) {
        float t = (float)i / (float)seg;
        float ang = yawCenter + a * 0.5f - a * t;
        float x = -R * std::sin(ang), z = -R * std::cos(ang);
        g.verts.push_back({x, yCenter + H * 0.5f, z, t, 0.0f});
        g.verts.push_back({x, yCenter - H * 0.5f, z, t, 1.0f});
    }
    for (int i = 0; i < seg; i++) {
        uint16_t b = (uint16_t)(i * 2);
        g.indices.insert(g.indices.end(), {b, (uint16_t)(b + 2), (uint16_t)(b + 1), (uint16_t)(b + 1),
                                           (uint16_t)(b + 2), (uint16_t)(b + 3)});
    }
}

static void buildFlat(Panel& g, float yaw, float pitch, float W, float H, float R) {
    Quat q = Quat::fromYawPitch(yaw, pitch);
    const Vec3 c[4] = {{-W / 2, H / 2, -R}, {W / 2, H / 2, -R}, {-W / 2, -H / 2, -R}, {W / 2, -H / 2, -R}};
    const float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    g.verts.clear();
    for (int i = 0; i < 4; i++) {
        Vec3 p = q.rotate(c[i]);
        g.verts.push_back({p.x, p.y, p.z, uv[i][0], uv[i][1]});
    }
    g.indices = {0, 1, 2, 2, 1, 3};
}

std::vector<Panel> computeLayout(const std::vector<float>& aspects, const LayoutParams& p, int focusIndex,
                                 float focusAmount) {
    std::vector<Panel> out;
    int n = (int)aspects.size();
    if (n == 0) return out;
    float R = std::max(0.2f, p.radius);
    float w = deg2rad(clampf(p.panelWidthDeg, 5.0f, 170.0f));
    float gap = deg2rad(clampf(p.gapDeg, 0.0f, 30.0f));
    float off = deg2rad(clampf(p.pitchOffsetDeg, -60.0f, 60.0f));
    bool flat = p.flat || p.arrangement == Arrangement::FlatRow;
    bool grid = p.arrangement == Arrangement::Grid && n > 1;
    int cols = grid ? (n + 1) / 2 : n;

    // same angular width for every panel
    float worldW = flat ? 2.0f * R * std::tan(w * 0.5f) : R * w;
    float maxH = 0;
    for (float a : aspects) maxH = std::max(maxH, worldW / std::max(0.2f, a));
    float rowStep = maxH + R * gap;

    out.resize(n);
    for (int i = 0; i < n; i++) {
        Panel& g = out[i];
        int row = grid && i >= cols ? 1 : 0;
        int col = row ? i - cols : i;
        int inRow = grid ? (row == 0 ? cols : n - cols) : n;
        float rowSpan = inRow * w + (inRow - 1) * gap;
        float yaw = rowSpan * 0.5f - w * 0.5f - col * (w + gap);
        float H = worldW / std::max(0.2f, aspects[i]);
        float y = R * std::tan(off);
        if (grid) y += (row == 0 ? 0.5f : -0.5f) * rowStep;

        float Ri = R;
        if (i == focusIndex && focusAmount > 0) Ri = R * (1.0f - 0.4f * clampf(focusAmount, 0, 1));

        if (flat) {
            g.pitch = std::atan2(y, R);
            buildFlat(g, yaw, g.pitch, worldW, H, Ri);
        } else {
            float yc = y * (Ri / R);
            buildCurved(g, yaw, yc, worldW, H, Ri, p.segments);
            g.pitch = std::atan2(yc, Ri);
        }
        g.yaw = yaw;
        g.widthRad = flat ? 2.0f * std::atan(worldW * 0.5f / Ri) : worldW / Ri;
        g.heightRad = 2.0f * std::atan(H * 0.5f / Ri);
    }
    return out;
}

int panelAt(const std::vector<Panel>& panels, const Vec3& dirIn, bool* inside) {
    int best = -1;
    float bestScore = 1e9f;
    Vec3 d = normalize(dirIn);
    float yaw = std::atan2(-d.x, -d.z);
    float pitch = std::asin(clampf(d.y, -1, 1));
    for (int i = 0; i < (int)panels.size(); i++) {
        const Panel& p = panels[i];
        float ey = std::fabs(wrapPi(yaw - p.yaw)) / std::max(1e-3f, p.widthRad * 0.5f);
        float ep = std::fabs(pitch - p.pitch) / std::max(1e-3f, p.heightRad * 0.5f);
        float score = std::max(ey, ep);
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (inside) *inside = best >= 0 && bestScore <= 1.0f;
    return best;
}

} // namespace ox
