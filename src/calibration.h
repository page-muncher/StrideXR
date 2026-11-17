#pragma once

#include "motion.h"
#include <vector>

// Use a typical peak across several short windows so one abrupt motion does
// not set the whole profile's pace.
class SwingSample {
public:
    void reset() { *this = {}; }

    void update(float dt, std::optional<float> speed, float smoothing) {
        elapsed += dt;
        if (speed) {
            filtered = smoothSwingSpeed(filtered, *speed, dt, smoothing);
            if (elapsed > 0.5f) {
                covered += dt;
                peak = std::max(peak, filtered);
            }
        } else {
            filtered = 0.0f;
        }
        if (elapsed - window >= 0.65f) {
            if (covered >= 0.30f) peaks.push_back(peak);
            window = elapsed;
            covered = peak = 0.0f;
        }
    }

    std::optional<float> result() const {
        if (peaks.size() < 4) return {};
        auto ordered = peaks;
        std::sort(ordered.begin(), ordered.end());
        const float typical = ordered[ordered.size() / 2];
        if (typical < 0.04f) return {};
        return std::min(typical, 3.20f);
    }

private:
    float elapsed = 0.0f;
    float filtered = 0.0f;
    float window = 0.0f;
    float covered = 0.0f;
    float peak = 0.0f;
    std::vector<float> peaks;
};

inline MotionSettings calibratedCurve(MotionSettings settings, const std::array<float, 3>& speeds) {
    settings.threshold = std::clamp(speeds[0] * 0.8f, 0.03f, speeds[2] - 0.10f);
    settings.slowSpeed = speeds[0];
    settings.midSpeed = speeds[1];
    settings.fullSpeed = speeds[2];
    return settings;
}
