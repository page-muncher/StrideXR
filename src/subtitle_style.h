#pragma once

#include <algorithm>
#include <array>

struct SubtitleStyle {
    int x = 0, y = 40, opacity = 100, lines = 4, layout = 0;
    bool operator==(const SubtitleStyle&) const = default;

    void clamp() {
        x = std::clamp(x, -90, 90);
        y = std::clamp(y, -70, 90);
        opacity = std::clamp(opacity, 0, 100);
        lines = std::clamp(lines, 1, 6);
        layout = std::clamp(layout, 0, 4);
    }
};
