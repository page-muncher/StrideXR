#pragma once
#include <openxr/openxr.h>

struct Input {
    bool left = false;
    bool right = false;
    bool focused = true;
    bool tracked = true;
    bool rejectExtras = false;
    bool rejectAttach = false;
    float leftZ = 0;
    float rightZ = 0;
    float headZ = 0;
    float leftX = 0;
    float rightX = 0;
    float headX = 0;
    float stickX = 0;
    float stickY = 0;
    bool menuDown = false;
    bool menuChanged = false;
    bool inverted = false;
    bool leftOccluded = false;
    bool hasVelocity = false;
    float controllerSpeed = 0;
};

struct Calls {
    uint32_t bindings;
    uint32_t attached;
    uint32_t synced;
    uint32_t layers;
    uint32_t menuBindings;
};

using SetInput = void (*)(const Input*);
using ReadCalls = Calls (*)();
