#pragma once

#include <windows.h>
#include "motion.h"
#include <cwchar>
#include <filesystem>
#include <string>

inline std::wstring iniText(const std::filesystem::path& path, const std::wstring& section, const wchar_t* key,
                            const wchar_t* fallback = L"") {
    std::wstring value(32768, L'\0');
    auto count = GetPrivateProfileStringW(section.c_str(), key, fallback, value.data(), DWORD(value.size()), path.c_str());
    value.resize(count);
    return value;
}

inline MotionSettings readMotionSettings(const std::filesystem::path& ini, const std::wstring& section) {
    MotionSettings motion;
    auto read = [&](const wchar_t* key, float fallback, float lo, float hi) {
        auto text = iniText(ini, section, key);
        wchar_t* end = nullptr;
        float value = std::wcstof(text.c_str(), &end);
        return end != text.c_str() && *end == L'\0' && std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    motion.threshold = read(L"StartSpeed", motion.threshold, 0.03f, 3.10f);
    motion.fullSpeed = read(L"FullSpeed", motion.fullSpeed, 0.15f, 3.20f);
    motion.fullSpeed = std::max(motion.fullSpeed, motion.threshold + 0.10f);
    motion.minimum = read(L"MinimumStick", motion.minimum, 0.05f, 1.0f);
    motion.maximum = read(L"MaximumStick", motion.maximum, motion.minimum, 1.0f);
    motion.slowSpeed = read(L"SlowSpeed", motion.slowSpeed, 0.0f, 3.20f);
    motion.midSpeed = read(L"MidSpeed", motion.midSpeed, 0.0f, 3.20f);
    motion.midStick = read(L"MidStick", motion.midStick, 0.0f, 1.0f);
    motion.peakHold = read(L"PeakHold", motion.peakHold, 0.15f, 2.0f);
    motion.slowdown = read(L"Slowdown", motion.slowdown, 0.0f, 1.0f);
    motion.stopDelay = read(L"StopDelay", motion.stopDelay, 0.0f, 2.0f);
    motion.stopFade = read(L"StopFade", motion.stopFade, 0.0f, 2.0f);
    motion.topAcceleration = read(L"TopAcceleration", motion.topAcceleration, 0.10f, 5.0f);
    motion.smoothing = read(L"Smoothing", motion.smoothing, 0.01f, 0.5f);
    motion.startConfirmation = read(L"StartConfirmation", motion.startConfirmation, 0.0f, 0.5f);
    return motion;
}

enum class ThumbGate { Both, Either, Left, Right };

inline ThumbGate readThumbGate(const std::filesystem::path& ini, const std::wstring& section) {
    const auto mode = iniText(ini, section, L"Thumbrests", L"Both");
    if (mode == L"Either") return ThumbGate::Either;
    if (mode == L"Left") return ThumbGate::Left;
    if (mode == L"Right") return ThumbGate::Right;
    return ThumbGate::Both;
}

inline bool thumbsArmed(ThumbGate gate, const std::array<bool, 2>& touch) {
    switch (gate) {
    case ThumbGate::Both: return touch[0] && touch[1];
    case ThumbGate::Either: return touch[0] || touch[1];
    case ThumbGate::Left: return touch[0];
    case ThumbGate::Right: return touch[1];
    }
    return false;
}
