#pragma once

#include "settings_io.h"
#include "tracking_math.h"

class ArmGesture {
public:
    void reset() {
        previous = {};
        fast = {};
        strokes = {};
        swing.reset();
        reverse = reversedStroke = wasMoving = false;
        dwell = quiet = 0;
    }

    bool reversePose() const { return reverse; }

    float update(const MotionSettings& settings, ThumbGate gate, bool allowReverse, float dt,
                 const XrPosef& head, const std::array<XrSpaceLocation, 2>& hands,
                 const std::array<bool, 2>& touching, std::array<std::optional<float>, 2>* forwardSamples = nullptr,
                 const XrSpaceVelocity* headVelocity = nullptr, const std::array<XrSpaceVelocity, 2>* handVelocity = nullptr) {
        constexpr auto position = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        constexpr auto orientation = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const auto forward = horizontalForward(head.orientation);
        if (!forward) { reset(); return 0; }
        const XrVector3f front{forward->x, 0, forward->y};
        const XrVector3f right{-front.z, 0, front.x};
        std::array<std::optional<XrVector3f>, 2> current{};
        bool pose = allowReverse;
        for (size_t i = 0; i < hands.size(); ++i) {
            const auto& hand = hands[i];
            if ((hand.locationFlags & position) != position) { pose = false; continue; }
            current[i] = difference(hand.pose.position, head.position);
            const auto side = dot(*current[i], right) * (i == 0 ? -1 : 1);
            const auto depth = dot(*current[i], front);
            const auto up = rotateVector(hand.pose.orientation, {0, 1, 0});
            pose = pose && (hand.locationFlags & orientation) == orientation &&
                up.y < (reverse ? -.23f : -.38f) &&
                side > (reverse ? .12f : .16f) && side < .64f &&
                current[i]->y > -.30f && current[i]->y < .40f && depth > -.26f && depth < .48f;
        }
        const bool timed = dt > 0 && dt <= .12f;
        if (!timed || pose != reverse) {
            reset();
            reverse = pose;
        }
        if (!pose && !thumbsArmed(gate, touching)) { reset(); return 0; }

        std::array<std::optional<float>, 2> deltas{};
        for (size_t i = 0; i < current.size(); ++i) {
            if (!pose && !touching[i]) current[i].reset();
            if (!pose) {
                float speed = current[i] && previous[i] && timed
                    ? dot(difference(*current[i], *previous[i]), front) / dt : NAN;
                const bool candidate = std::isfinite(speed) && std::abs(speed) > 3.6f && std::abs(speed) <= 9.0f;
                const bool repeated = candidate && fast[i] * speed > 0 && std::abs(fast[i] - speed) < 2.0f;
                fast[i] = candidate ? speed : 0;
                if ((!std::isfinite(speed) || std::abs(speed) > 3.6f) && !repeated) {
                    speed = NAN;
                    constexpr auto valid = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
                    if (touching[i] && handVelocity && ((*handVelocity)[i].velocityFlags & valid)) {
                        const auto headLinear = headVelocity && (headVelocity->velocityFlags & valid)
                            ? headVelocity->linearVelocity : XrVector3f{};
                        speed = dot(difference((*handVelocity)[i].linearVelocity, headLinear), front);
                    }
                }
                if (std::isfinite(speed) && std::abs(speed) <= 9.0f)
                    deltas[i] = speed * dt;
                continue;
            }
            if (!current[i]) continue;
            if (pose) {
                // A short virtual lever picks up a wrist waggle as well as hand translation.
                const auto tip = rotateVector(hands[i].pose.orientation, {0, 0, -.10f});
                current[i]->x += tip.x;
                current[i]->y += tip.y;
                current[i]->z += tip.z;
            }
            if (!previous[i]) continue;
            const auto delta = difference(*current[i], *previous[i]);
            const float length = std::sqrt(dot(delta, delta));
            if (!std::isfinite(length) || length > dt * 9.0f) { strokes[i].reset(); continue; }
            deltas[i] = length;
            if (dwell >= .12f && length > dt * std::max(.14f, settings.threshold * .60f)) {
                const XrVector3f direction{delta.x / length, delta.y / length, delta.z / length};
                if (strokes[i] && dot(*strokes[i], direction) < -.30f) reversedStroke = true;
                if (!strokes[i] || dot(*strokes[i], direction) < .50f) strokes[i] = direction;
            }
        }
        previous = current;
        if (!timed) return 0;
        if (forwardSamples && !pose) *forwardSamples = deltas;
        if (!pose) return swing.update(settings, dt, deltas);
        dwell += dt;
        const auto speed = swingSpeed(dt, deltas);
        quiet = speed && *speed >= settings.threshold * .60f ? 0.0f : quiet + dt;
        if (!wasMoving && quiet > .38f) { reversedStroke = false; strokes = {}; }
        if (!reversedStroke) return 0;
        const auto output = swing.update(settings, dt, deltas);
        if (output > 0) wasMoving = true;
        else if (wasMoving) { reversedStroke = wasMoving = false; strokes = {}; }
        return -output;
    }

private:
    Swing swing;
    std::array<std::optional<XrVector3f>, 2> previous{}, strokes{};
    std::array<float, 2> fast{};
    bool reverse = false, reversedStroke = false, wasMoving = false;
    float dwell = 0, quiet = 0;
};
