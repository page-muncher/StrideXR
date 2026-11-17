#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <optional>

struct MotionSettings {
    float threshold = 0.24f;  // m/s
    float fullSpeed = 1.10f;  // m/s
    float minimum = 0.38f;    // normalized stick
    float maximum = 1.0f;
    float slowSpeed = 0.0f; // Zero uses the start threshold.
    float midSpeed = 0.0f;  // Zero uses the midpoint of the swing range.
    float midStick = 0.0f;  // Zero uses the midpoint of the stick range.
    float peakHold = 0.70f / 1.5f;
    float slowdown = 0.10f;
    float stopDelay = 0.50f;
    float stopFade = 0.33f;
    float topAcceleration = 0.80f;
    float smoothing = 0.085f;
    float startConfirmation = 0.06f;
};

inline float stickForSpeed(const MotionSettings& settings, float speed) {
    const float slow = std::clamp(settings.slowSpeed, settings.threshold, settings.fullSpeed - 0.02f);
    const float mid = settings.midSpeed > slow && settings.midSpeed < settings.fullSpeed
        ? settings.midSpeed : (slow + settings.fullSpeed) * 0.5f;
    const float middleStick = settings.midStick > 0.0f
        ? std::clamp(settings.midStick, settings.minimum, settings.maximum)
        : (settings.minimum + settings.maximum) * 0.5f;
    if (speed < mid) {
        const float fraction = std::clamp((speed - slow) / (mid - slow), 0.0f, 1.0f);
        return std::lerp(settings.minimum, middleStick, fraction);
    }
    const float fraction = std::clamp((speed - mid) / (settings.fullSpeed - mid), 0.0f, 1.0f);
    return std::lerp(middleStick, settings.maximum, fraction);
}

inline std::optional<float> swingSpeed(float dt, const std::array<std::optional<float>, 2>& deltas) {
    std::optional<float> speed;
    for (auto delta : deltas) {
        if (!delta) continue;
        if (!std::isfinite(*delta)) continue;
        speed = std::max(speed.value_or(0.0f), std::abs(*delta) / dt);
    }
    return speed;
}

inline float smoothSwingSpeed(float current, float sample, float dt, float smoothing) {
    return std::lerp(current, sample, 1.0f - std::exp(-dt / smoothing));
}

class Swing {
public:
    void reset() {
        peaks.clear();
        elapsed = 0.0;
        filtered = movementTime = quiet = output = 0.0f;
        crest = {};
        previousCrestTime = 0.0;
        trough = 0.0f;
        rising = true;
        peakWindow = 0.0;
    }

    float update(const MotionSettings& settings, float dt,
                 const std::array<std::optional<float>, 2>& deltas) {
        const auto speed = swingSpeed(dt, deltas);
        const float measured = speed.value_or(0.0f);

        elapsed += dt;
        filtered = smoothSwingSpeed(filtered, measured, dt, settings.smoothing);
        updateCadence(settings.peakHold);

        // Bridge a turnaround, then fade the last moving output to zero.
        if (output > 0.0f && measured < settings.threshold) {
            quiet += dt;
            const float scale = stopScale(settings);
            if (scale == 0.0f) {
                reset();
                return 0.0f;
            }
            return output * scale;
        }

        if (quiet > settings.stopDelay) {
            output *= stopScale(settings);
            peaks.clear(); // Resume from the faded pace without reusing a pre-stop peak.
        }
        quiet = 0.0f;
        if (output == 0.0f) {
            movementTime = speed && filtered >= settings.threshold ? movementTime + dt : 0.0f;
            if (movementTime < settings.startConfirmation) return 0.0f;
        }

        // Use the fastest part of a recent stroke through the next reversal.
        // Keep only samples that can still become the maximum as older ones expire.
        while (!peaks.empty() && elapsed - peaks.front().time > peakWindow) peaks.pop_front();
        while (!peaks.empty() && filtered >= peaks.back().speed) peaks.pop_back();
        peaks.push_back({elapsed, filtered});

        const float target = stickForSpeed(settings, peaks.front().speed);
        if (target < output) {
            output = settings.slowdown > 0.0f
                ? std::lerp(output, target, 1.0f - std::exp(-dt / settings.slowdown)) : target;
        } else {
            // Preserve quick walking response and give the top quarter of the stick a gradual ramp.
            const float upperRange = std::lerp(settings.minimum, settings.maximum, 0.75f);
            const float rise = (settings.maximum - settings.minimum) * settings.topAcceleration * dt;
            output = std::min(target, std::max(output, upperRange) + rise);
        }
        return output;
    }

private:
    struct Peak {
        double time = 0.0;
        float speed = 0.0f;
    };
    float stopScale(const MotionSettings& settings) const {
        if (quiet <= settings.stopDelay) return 1.0f;
        if (settings.stopFade == 0.0f) return 0.0f;
        return 1.0f - std::clamp((quiet - settings.stopDelay) / settings.stopFade, 0.0f, 1.0f);
    }

    void updateCadence(double defaultPeakWindow) {
        peakWindow = std::max(peakWindow, defaultPeakWindow);
        // Confirm a crest after the speed has fallen enough to reject tracking noise.
        // A slower cadence needs a longer window to bridge the gap between crests.
        if (rising) {
            if (filtered >= crest.speed) {
                crest = {elapsed, filtered};
            } else if (crest.speed - filtered >= std::max(0.025f, crest.speed * 0.08f)) {
                const double spacing = crest.time - previousCrestTime;
                if (previousCrestTime > 0.0 && spacing >= 0.22 && spacing <= 1.10) {
                    peakWindow = std::max(defaultPeakWindow, spacing + 0.05);
                }
                previousCrestTime = crest.time;
                trough = filtered;
                rising = false;
            }
        } else {
            trough = std::min(trough, filtered);
            if (filtered - trough >= std::max(0.025f, filtered * 0.08f)) {
                crest = {elapsed, filtered};
                rising = true;
            }
        }
        if (elapsed - previousCrestTime > peakWindow + 0.18) peakWindow = defaultPeakWindow;
    }

    std::deque<Peak> peaks;
    Peak crest;
    double elapsed = 0.0;
    double previousCrestTime = 0.0;
    double peakWindow = 0.0;
    float trough = 0.0f;
    float filtered = 0.0f;
    float movementTime = 0.0f;
    float quiet = 0.0f;
    float output = 0.0f;
    bool rising = true;
};
