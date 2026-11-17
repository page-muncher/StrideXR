#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "runtime.h"
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include "../src/calibration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace {
unsigned checks = 0;

void require(bool valid, const char* what) {
    ++checks;
    if (valid) return;
    std::fprintf(stderr, "Check %u failed: %s\n", checks, what);
    std::exit(1);
}

void success(XrResult result, const char* what) {
    if (result != XR_SUCCESS) std::fprintf(stderr, "%s returned %d\n", what, result);
    require(result == XR_SUCCESS, what);
}

template<class F>
F load(HMODULE module, const char* name) {
    auto function = GetProcAddress(module, name);
    require(function != nullptr, name);
#pragma warning(suppress: 4191)
    return reinterpret_cast<F>(function);
}

bool closeEnough(float a, float b) { return std::abs(a - b) < 0.0001f; }
} // namespace

int wmain(int argc, wchar_t** argv) {
    require(argc == 4, "Usage: StrideXRTest <openxr_loader.dll> <sim-runtime.dll or -> <mode>");
    const std::wstring_view mode(argv[3]);
    const bool audiobook = mode == L"audiobook";
    const auto loader = LoadLibraryW(argv[1]);
    require(loader != nullptr, "load the real Khronos OpenXR loader");
#define LOAD(api) const auto api = load<PFN_##api>(loader, #api)
    LOAD(xrEnumerateApiLayerProperties);
    uint32_t count = 0;
    success(xrEnumerateApiLayerProperties(0, &count, nullptr), "enumerate registered API layers");
    std::vector<XrApiLayerProperties> layers(count, {XR_TYPE_API_LAYER_PROPERTIES});
    success(xrEnumerateApiLayerProperties(count, &count, layers.data()), "read registered API layer properties");
    bool found = false;
    for (const auto& layer : layers) {
        std::printf("Loader discovered %s\n", layer.layerName);
        const char* expectedLayer = sizeof(void*) == 4 ? "XR_APILAYER_STRIDEXR_locomotion_x86" : "XR_APILAYER_STRIDEXR_locomotion";
        if (std::string_view(layer.layerName) == expectedLayer) found = true;
    }
    require(found == (mode != L"unregistered"), "layer registration matches expected state");
    if (mode == L"probe" || mode == L"unregistered") {
        if (mode == L"probe") {
            MotionSettings settings;
            require(closeEnough(stickForSpeed(settings, .65f), .38f + (.65f - .24f) / (1.10f - .24f) * .62f),
                    "legacy profiles keep their linear speed curve");
            settings.minimum = .08f;
            settings.midStick = .30f;
            settings = calibratedCurve(settings, {.25f, .65f, 1.50f});
            require(closeEnough(stickForSpeed(settings, .21f), .08f), "gentle swings hold the minimum stick");
            require(closeEnough(stickForSpeed(settings, .25f), .08f), "slow arm speed maps to minimum stick");
            require(closeEnough(stickForSpeed(settings, .65f), .30f), "medium arm speed maps to the configured middle stick");
            require(closeEnough(stickForSpeed(settings, 1.50f), 1.0f), "fast arm speed maps to maximum stick");
        }
        std::printf("PASS (%u checks)\n", checks);
        return 0;
    }

    LOAD(xrCreateInstance);
    LOAD(xrDestroyInstance);
    LOAD(xrCreateSession);
    LOAD(xrBeginSession);
    LOAD(xrDestroySession);
    LOAD(xrCreateActionSet);
    LOAD(xrCreateAction);
    LOAD(xrStringToPath);
    LOAD(xrSuggestInteractionProfileBindings);
    LOAD(xrAttachSessionActionSets);
    LOAD(xrWaitFrame);
    LOAD(xrSyncActions);
    LOAD(xrGetActionStateVector2f);
    LOAD(xrGetActionStateFloat);
    LOAD(xrGetInstanceProperties);
#undef LOAD

    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(instanceInfo.applicationInfo.applicationName,
             mode == L"vdxr" ? "StrideXR VDXR binding probe" :
             audiobook ? "StrideXR audiobook test" : "StrideXR simulated controller test");
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const char* extension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    if (audiobook) {
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.enabledExtensionNames = &extension;
    }
    if (mode == L"vdxr") {
        XrInstance instance{};
        success(xrCreateInstance(&instanceInfo, &instance), "create an instance on the active runtime");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        success(xrGetInstanceProperties(instance, &properties), "read active runtime name");
        std::printf("Active runtime: %s\n", properties.runtimeName);
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "gameplay");
        strcpy_s(setInfo.localizedActionSetName, "Gameplay");
        XrActionSet set{};
        success(xrCreateActionSet(instance, &setInfo, &set), "create gameplay action set on VDXR");
        XrPath hands[2]{};
        success(xrStringToPath(instance, "/user/hand/left", &hands[0]), "resolve left hand on VDXR");
        success(xrStringToPath(instance, "/user/hand/right", &hands[1]), "resolve right hand on VDXR");
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(actionInfo.actionName, "thumbstick");
        strcpy_s(actionInfo.localizedActionName, "Thumbstick");
        actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
        actionInfo.countSubactionPaths = 2;
        actionInfo.subactionPaths = hands;
        XrAction stick{};
        success(xrCreateAction(set, &actionInfo, &stick), "create shared thumbstick action on VDXR");
        strcpy_s(actionInfo.actionName, "thumbrest");
        strcpy_s(actionInfo.localizedActionName, "Thumbrest");
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        XrAction thumbrest{};
        success(xrCreateAction(set, &actionInfo, &thumbrest), "create the application's thumbrest action on VDXR");
        XrActionSuggestedBinding bindings[] = {{stick, XR_NULL_PATH}, {stick, XR_NULL_PATH},
                                               {thumbrest, XR_NULL_PATH}, {thumbrest, XR_NULL_PATH}};
        success(xrStringToPath(instance, "/user/hand/left/input/thumbstick", &bindings[0].binding), "resolve left stick on VDXR");
        success(xrStringToPath(instance, "/user/hand/right/input/thumbstick", &bindings[1].binding), "resolve right stick on VDXR");
        success(xrStringToPath(instance, "/user/hand/left/input/thumbrest/touch", &bindings[2].binding), "resolve left thumbrest on VDXR");
        success(xrStringToPath(instance, "/user/hand/right/input/thumbrest/touch", &bindings[3].binding), "resolve right thumbrest on VDXR");
        XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        success(xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &suggestion.interactionProfile),
                "resolve Touch profile on VDXR");
        suggestion.countSuggestedBindings = static_cast<uint32_t>(std::size(bindings));
        suggestion.suggestedBindings = bindings;
        success(xrSuggestInteractionProfileBindings(instance, &suggestion), "suggest Touch bindings on VDXR");
        success(xrDestroyInstance(instance), "destroy VDXR instance");
        std::printf("PASS (%u checks)\n", checks);
        return 0;
    }

    const auto runtime = LoadLibraryW(argv[2]);
    require(runtime != nullptr, "load simulated runtime");
    const auto setInput = load<SetInput>(runtime, "TestSetInput");
    const auto readCalls = load<ReadCalls>(runtime, "TestReadCalls");
    Input input;
    input.rejectExtras = mode == L"reject-bindings";
    input.rejectAttach = mode == L"reject-attach";
    setInput(&input);
    XrInstance instance{};
    success(xrCreateInstance(&instanceInfo, &instance), "create OpenXR instance");
    wchar_t dataDirectory[32768]{};
    require(GetEnvironmentVariableW(L"LOCALAPPDATA", dataDirectory, DWORD(std::size(dataDirectory))) > 0, "locate test data");
    const auto launchPath = std::filesystem::path(dataDirectory) / L"StrideXR" /
                            L"OpenXR" / (std::to_wstring(GetCurrentProcessId()) + L".xr");
    auto checkLaunch = [&](const std::string& expectedStage, const std::string& expectedCapture) {
        std::ifstream record(launchPath);
        std::string executable, stage, capture, runtimeName, application;
        require(bool(std::getline(record, executable) && std::getline(record, stage) && std::getline(record, capture) &&
                     std::getline(record, runtimeName) && std::getline(record, application)), "record the OpenXR process");
        require(stage == expectedStage && capture == expectedCapture, "record launch state and pending profile");
    };
    const auto pending = mode == L"profile-capture" ? "pending" : "";
    checkLaunch("OpenXR detected", pending);

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    if (audiobook) {
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                            D3D11_SDK_VERSION, &device, nullptr, nullptr)), "create test D3D11 device");
        binding.device = device.Get();
        sessionInfo.next = &binding;
        sessionInfo.systemId = 1;
    }
    XrSession session{};
    success(xrCreateSession(instance, &sessionInfo, &session), "create OpenXR session");
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    success(xrBeginSession(session, &begin), "begin a non-VR session");
    checkLaunch("OpenXR detected", pending);
    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    success(xrBeginSession(session, &begin), "begin a stereo VR session");
    checkLaunch("OpenXR VR session", pending);
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "gameplay");
    strcpy_s(setInfo.localizedActionSetName, "Gameplay");
    XrActionSet gameSet{};
    success(xrCreateActionSet(instance, &setInfo, &gameSet), "create gameplay action set");
    auto action = [&](const char* name, XrActionType type, std::span<const XrPath> subactions = {}) {
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(actionInfo.actionName, name);
        strcpy_s(actionInfo.localizedActionName, name);
        actionInfo.actionType = type;
        actionInfo.countSubactionPaths = static_cast<uint32_t>(subactions.size());
        actionInfo.subactionPaths = subactions.data();
        XrAction result{};
        success(xrCreateAction(gameSet, &actionInfo, &result), name);
        return result;
    };
    auto path = [&](const char* text) {
        XrPath result{};
        success(xrStringToPath(instance, text, &result), text);
        return result;
    };
    const bool tunedShipwright = mode == L"shipwright-tuned";
    const bool shipwright = mode == L"shipwright" || tunedShipwright;
    const XrPath hands[] = {path("/user/hand/left"), path("/user/hand/right")};
    auto move = shipwright ? action("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, hands) :
                            action("move", XR_ACTION_TYPE_VECTOR2F_INPUT);
    const XrPath moveHand = shipwright ? hands[0] : XR_NULL_PATH;
    auto turn = action("turn", XR_ACTION_TYPE_FLOAT_INPUT);
    auto moveY = action("move_y", XR_ACTION_TYPE_FLOAT_INPUT);
    auto other = action("other", XR_ACTION_TYPE_VECTOR2F_INPUT);
    auto grip = action("grip", XR_ACTION_TYPE_POSE_INPUT);
    auto mixed = action("mixed", XR_ACTION_TYPE_VECTOR2F_INPUT);
    auto leftStick = path("/user/hand/left/input/thumbstick");
    auto rightStick = path("/user/hand/right/input/thumbstick");
    std::vector<XrActionSuggestedBinding> bindings = {
        {move, leftStick},
        {turn, path("/user/hand/right/input/thumbstick/x")},
        {moveY, path("/user/hand/left/input/thumbstick/y")},
        {other, rightStick},
        {grip, path("/user/hand/left/input/grip/pose")},
        {mixed, leftStick}, {mixed, rightStick}
    };
    if (shipwright) bindings.push_back({move, rightStick});
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestion.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggestion.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggestion.suggestedBindings = bindings.data();
    success(xrSuggestInteractionProfileBindings(instance, &suggestion), "apply original Touch bindings");
    success(xrSuggestInteractionProfileBindings(instance, &suggestion), "replace Touch bindings like the Dishonored mod");
    if (audiobook) {
        require(readCalls().menuBindings == 1, "bind play/pause to left Menu");
        require(readCalls().bindings == bindings.size() + 1, "preserve game bindings with only the audiobook active");
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &gameSet;
        success(xrAttachSessionActionSets(session, &attach), "attach game and audiobook sets");
        require(readCalls().attached == 2, "attach the audiobook even with swing motion disabled");
        XrActiveActionSet active{gameSet, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        const auto endFrame = load<PFN_xrEndFrame>(loader, "xrEndFrame");
        const auto mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, L"Local\\StrideXR-Audiobook-v3");
        require(mapping != nullptr, "open the audiobook player's shared mapping");
        auto shared = static_cast<DWORD*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4160));
        require(shared != nullptr, "view the audiobook player's shared mapping");
        auto frame = [&] {
            setInput(&input);
            require(xrSyncActions(session, &sync) == (input.focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED), "sync Menu input");
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const auto layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.layerCount = 1;
            end.layers = &layer;
            success(endFrame(session, &end), "submit game and optional subtitle layers");
        };
        frame();
        require(readCalls().layers == 2, "submit the view-relative caption above the original game layer");
        require((shared[3] & 2) != 0 && shared[4] == 0, "report active Menu input without toggling at launch");
        input.menuDown = input.menuChanged = true;
        frame();
        require(shared[4] == 1, "one toggle on the first press");
        frame();
        require(shared[4] == 1, "no repeat while held");
        input.focused = false;
        frame();
        input.focused = true;
        frame();
        require(shared[4] == 1, "no repeat after refocusing with Menu held");
        input.menuDown = false;
        frame();
        input.menuDown = true;
        frame();
        require(shared[4] == 2, "second press toggles again");
        reinterpret_cast<wchar_t*>(shared + 8)[0] = L' ';
        shared[6] = 1;
        ++shared[1];
        frame();
        require(readCalls().layers == 2, "keep the empty subtitle box between cues");
        shared[7] = 50;
        frame();
        require(readCalls().layers == 2, "submit the subtitle layer while fading");
        shared[7] = 100;
        frame();
        require(readCalls().layers == 1, "hide the fully faded subtitle layer");
        input.menuDown = false;
        frame();
        input.menuDown = true;
        frame();
        require(shared[4] == 3, "keep Menu input available with hidden subtitles");
        shared[7] = 0;
        frame();
        require(readCalls().layers == 2, "show subtitles again on resume");
        shared[0] = 0;
        ++shared[1];
        input.menuDown = false;
        frame();
        require(readCalls().layers == 1, "hide the subtitle layer when the desktop player is disarmed");
        input.menuDown = true;
        frame();
        require(shared[4] == 3, "ignore Menu when the desktop player is disarmed");
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        success(xrDestroySession(session), "destroy audiobook session");
        success(xrDestroyInstance(instance), "destroy audiobook instance");
        std::printf("PASS (%u checks)\n", checks);
        return 0;
    }
    const bool enabled = mode != L"disabled" && mode != L"profile-canceled";
    const bool useLayer = enabled && !input.rejectAttach && !input.rejectExtras;
    require(readCalls().bindings == std::size(bindings) + (enabled && !input.rejectExtras ? 4 : 0),
            "preserve all original bindings and merge exactly four private bindings when supported");
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &gameSet;
    success(xrAttachSessionActionSets(session, &attach), "attach action sets");
    require(readCalls().attached == (useLayer ? 2u : 1u), "attach the app set with or without the private set");
    XrActiveActionSet active{gameSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    auto frame = [&] {
        setInput(&input);
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        success(xrWaitFrame(session, &wait, &state), "wait frame");
        require(xrSyncActions(session, &sync) == (input.focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED), "sync action sets");
        require(readCalls().synced == (useLayer ? 2u : 1u), "private input shares the app's sync only when enabled");
    };
    auto vector = [&](XrAction actionHandle, XrPath hand = XR_NULL_PATH) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = actionHandle;
        info.subactionPath = hand;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        success(xrGetActionStateVector2f(session, &info, &state), "read vector action");
        return state;
    };
    if (mode.starts_with(L"profile-")) {
        input.left = input.right = true;
        float pace = 0.0f;
        for (int i = 0; i < 120; ++i) {
            input.leftZ += .01f;
            input.rightZ -= .01f;
            frame();
            pace = vector(move).currentState.y;
        }
        const float expected = mode == L"profile-assigned" ? .44f : mode == L"profile-global" ? .62f :
                               mode == L"profile-capture" ? .51f : 0.0f;
        require(closeEnough(pace, expected), "use the assigned, fallback, or pending profile's own settings");
        for (int i = 0; i < 19; ++i) frame();
        require(vector(move).currentState.y == 0.0f, "load per-profile stop timing");
        success(xrDestroySession(session), "destroy OpenXR session");
        success(xrDestroyInstance(instance), "destroy OpenXR instance");
        std::printf("PASS (%u checks, selected profile pace %.2f)\n", checks, pace);
        return 0;
    }
    auto scalar = [&](XrAction actionHandle) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = actionHandle;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        success(xrGetActionStateFloat(session, &info, &state), "read float action");
        return state;
    };
    if (mode == L"reverse") {
        input.leftX = -.30f;
        input.rightX = .30f;
        input.inverted = true;
        for (int i = 0; i < 35; ++i) {
            input.leftZ = input.rightZ = .006f * float(i);
            frame();
            require(vector(move).currentState.y == 0, "raising or moving inverted hands in one direction does not reverse");
        }
        float peak = 0;
        for (int i = 0; i < 110; ++i) {
            input.leftZ = input.rightZ = .20f + .055f * std::sin(float(i) * .17f);
            frame();
            const auto state = vector(move);
            peak = std::min(peak, state.currentState.y);
            require(state.currentState.y <= 0 && state.currentState.x == 0, "the gesture emits only backward stick");
        }
        require(peak < -.38f && scalar(moveY).currentState < 0, "reverse reaches both stick action types");
        input.stickY = .45f;
        frame();
        require(closeEnough(vector(move).currentState.y, .45f), "physical stick overrides reverse");
        input.stickY = 0;
        input.tracked = false;
        frame();
        require(vector(move).currentState.y == 0, "reverse clears on tracking loss");
        input.tracked = true;
        input.inverted = false;
        for (int i = 0; i < 110; ++i) {
            input.leftZ = .055f * std::sin(float(i) * .17f);
            frame();
            require(vector(move).currentState.y == 0, "upright controllers still require the thumbrest gate");
        }
        success(xrDestroySession(session), "destroy reverse test session");
        success(xrDestroyInstance(instance), "destroy reverse test instance");
        std::printf("PASS (%u checks, peak reverse %.2f)\n", checks, peak);
        return 0;
    }
    auto swing = [&](int frames, float amplitude = .11f) {
        float peak = 0;
        for (int i = 0; i < frames; ++i) {
            input.leftZ = std::sin(i * .13f) * amplitude;
            input.rightZ = -input.leftZ;
            frame();
            auto state = vector(move, moveHand);
            const auto repeat = vector(move, moveHand);
            require(state.currentState.y == repeat.currentState.y &&
                    state.changedSinceLastSync == repeat.changedSinceLastSync &&
                    state.lastChangeTime == repeat.lastChangeTime, "action state stays fixed within a sync");
            peak = std::max(peak, state.currentState.y);
            if (state.currentState.y > .2f) {
                require(state.lastChangeTime > 0, "synthetic movement reports a change time");
                require(closeEnough(scalar(moveY).currentState, state.currentState.y), "split left axis gets the same output");
            }
            require(closeEnough(scalar(turn).currentState, .32f), "right turn is untouched");
            require(closeEnough(vector(other).currentState.y, .1f), "right vector is untouched");
            require(closeEnough(vector(mixed).currentState.y, .1f), "ambiguous aggregate is untouched");
            if (shipwright) {
                require(closeEnough(vector(move, hands[1]).currentState.y, .1f), "shared thumbstick right hand is untouched");
                require(closeEnough(vector(move).currentState.y, .1f), "shared thumbstick aggregate is untouched");
            }
        }
        return peak;
    };
    require(swing(35) == 0, "unarmed swings do not move");
    input.right = true;
    require((swing(65) > .38f) == (mode == L"either"), "one thumb only works in either-thumb mode");
    input.left = true;
    for (int i = 0; i < 80; ++i) frame();
    require(vector(move, moveHand).currentState.y == 0, "resting thumbs and still hands do not move");
    if (tunedShipwright) {
        // Shipwright uses a 15% VR dead zone, then scales by 127 and
        // clamps its adjusted N64 stick to 60 after subtracting 7.
        auto gameStick = [](float value) {
            return value > .15f ? std::clamp(static_cast<int>(value * 127.0f) - 7, 0, 60) : 0;
        };
        auto measure = [&](float amplitude) {
            input.left = false;
            input.leftZ = input.rightZ = 0.0f;
            frame();
            require(vector(move, moveHand).currentState.y == 0, "lifting a thumb stops locomotion");
            input.left = true;
            return gameStick(swing(160, amplitude));
        };
        const int jitter = measure(.009f);
        const int gentle = measure(.028f);
        const int medium = measure(.060f);
        const int large = measure(.120f);
        const int fast = measure(.265f);
        const int strongest = measure(.300f);
        require(jitter == 0, "tiny controller jitter stays below the activation threshold");
        require(gentle >= 12 && gentle <= 14, "small swings yield a slow walk just above the game dead zone");
        require(medium > gentle && large > medium && large < 60, "gradual increase before maximum speed");
        require(fast > large && fast < 60 && strongest > fast, "top speed requires a stronger swing");

        input.left = false;
        input.leftZ = input.rightZ = 0.0f;
        frame();
        input.left = true;
        auto moveHands = [&](float metersPerSecond) {
            input.leftZ += metersPerSecond / 90.0f;
            input.rightZ = -input.leftZ;
            frame();
            return vector(move, moveHand).currentState.y;
        };
        float walking = 0.0f;
        for (int i = 0; i < 80; ++i) walking = moveHands(0.65f);
        require(walking > .20f && walking < .35f, "start a walk at a repeatable arm speed");
        for (int i = 0; i < 18; ++i) {
            require(moveHands(0.0f) == walking, "hold the exact output through a 200 ms turnaround");
        }
        for (int i = 0; i < 20; ++i) {
            require(closeEnough(moveHands(-0.65f), walking), "reversal resumes at the same walking speed");
        }
        for (int i = 0; i < 44; ++i) {
            require(closeEnough(moveHands(0.0f), walking), "hold speed until the half-second limit");
        }
        for (int i = 45; i <= 75; ++i) {
            const float fade = std::clamp((i / 90.0f - .50f) / .33f, 0.0f, 1.0f);
            require(closeEnough(moveHands(0.0f), walking * (1.0f - fade)), "fade linearly to zero over 330 ms");
        }
        require(moveHands(0.0f) == 0.0f, "remain stopped until a new swing");
        require(moveHands(0.65f) == 0.0f, "a single motion sample does not restart walking");
        for (int i = 0; i < 80; ++i) walking = moveHands(0.65f);
        require(walking > .20f, "resume walking after standing still");
        require(moveHands(0.0f) == walking, "retain speed at the start of a pause");
        input.left = false;
        require(moveHands(0.0f) == 0.0f, "releasing a thumbrest cancels a held speed immediately");

        input.left = true;
        float highPace = 0.0f;
        for (int i = 0; i < 100; ++i) highPace = moveHands(1.3f);
        const float slowerTarget = std::lerp(.151f, .53f, (.55f - .20f) / (3.00f - .20f));
        const float midpoint = (highPace + slowerTarget) / 2.0f;
        int framesToMidpoint = 0;
        for (int i = 1; i <= 120; ++i) {
            if (moveHands(.55f) <= midpoint) {
                framesToMidpoint = i;
                break;
            }
        }
        std::printf("Time to halfway to a slower pace: %.3f seconds\n", framesToMidpoint / 90.0);
        require(framesToMidpoint >= 49 && framesToMidpoint <= 61, "reduce pace sooner after easing off");

        input.left = false;
        frame();
        input.left = true;
        float topPace = 0.0f;
        for (int i = 0; i < 26; ++i) topPace = moveHands(3.3f);
        require(topPace > .40f && topPace < .50f, "a strong swing builds gradually near the top of the range");
        for (int i = 0; i < 40; ++i) topPace = moveHands(3.3f);
        require(closeEnough(topPace, .53f), "sustained strong swings still reach full speed");
        for (int i = 0; i < 65; ++i) moveHands(0.0f);
        require(moveHands(.55f) < .25f, "resuming during a fade does not reuse the earlier top speed");
        input.left = false;

        // Both hands reverse at the same time in this replay. Their speed
        // naturally drops toward zero before they change direction.
        auto pendulum = [&](int framesPerCycle, float amplitude) {
            float slowest = 1.0f;
            float fastest = 0.0f;
            for (int i = 0; i < framesPerCycle * 4; ++i) {
                const float phase = 6.283185307f * static_cast<float>(i % framesPerCycle) / framesPerCycle;
                input.leftZ = std::sin(phase) * amplitude;
                input.rightZ = -input.leftZ;
                frame();
                const float pace = vector(move, moveHand).currentState.y;
                if (i >= framesPerCycle * 2) {
                    slowest = std::min(slowest, pace);
                    fastest = std::max(fastest, pace);
                }
            }
            return std::pair{slowest, fastest};
        };
        input.leftZ = input.rightZ = 0.0f;
        frame();
        input.left = true;
        const auto [normalMin, normalMax] = pendulum(90, .14f);
        std::printf("Steady gait at 1 Hz: %.3f to %.3f stick\n", normalMin, normalMax);
        require(normalMax > .22f && normalMax - normalMin < .012f, "steady pace through natural swing turnarounds");
        const auto [gentleMin, gentleMax] = pendulum(90, .06f);
        require(gentleMin >= .151f && gentleMax < normalMin - .04f, "sustained gentler swings reduce the pace");
        const auto [slowMin, slowMax] = pendulum(180, .14f);
        std::printf("Steady gait at 0.5 Hz: %.3f to %.3f stick\n", slowMin, slowMax);
        require(slowMax > .175f && slowMax - slowMin < .012f, "steady pace at a slower cadence");

        success(xrDestroySession(session), "destroy OpenXR session");
        success(xrDestroyInstance(instance), "destroy OpenXR instance");
        std::printf("PASS (%u checks, game stick out of 60: %d / %d / %d / %d / %d / %d)\n",
                    checks, jitter, gentle, medium, large, fast, strongest);
        return 0;
    }
    const float peak = swing(80);
    require(useLayer ? peak > .55f && peak <= 1.0f : peak == 0, "expected motion from both-thumb arm swing");
    if (useLayer) {
        for (int i = 0; i < 80; ++i) {
            const float translation = static_cast<float>(i) * .017f;
            input.leftZ += .017f;
            input.rightZ += .017f;
            input.headZ = translation + .017f;
            frame();
        }
        require(vector(move, moveHand).currentState.y == 0, "common headset and arm translation does not drive movement");
        input.headZ = 0;
        require(swing(80) > .55f, "restart arm swing");
        input.stickX = .5f;
        input.stickY = -.45f;
        frame();
        auto physical = vector(move, moveHand);
        require(closeEnough(physical.currentState.x, .5f) && closeEnough(physical.currentState.y, -.45f), "manual stick overrides");
        input.stickX = input.stickY = 0;
        require(swing(80) > .55f, "resume arm swing");
        input.left = input.right = false;
        frame();
        auto release = vector(move, moveHand);
        require(release.currentState.y == 0 && release.changedSinceLastSync, "release sends a zero in the same sync");
        frame();
        require(vector(move, moveHand).currentState.y == 0 && !vector(move, moveHand).changedSinceLastSync, "release change is reported once");
        input.left = input.right = true;
        require(swing(80) > .55f, "re-arm after release");
        input.leftZ += 2.0f;
        input.rightZ += .008f;
        frame();
        require(vector(move, moveHand).currentState.y > .38f, "one wild position sample does not zero a valid swing");
        input.leftOccluded = true;
        for (int i = 0; i < 80; ++i) {
            input.rightZ = .11f * std::sin(float(i) * .13f);
            frame();
        }
        require(vector(move, moveHand).currentState.y > .38f, "one tracked hand sustains a swing");
        for (int i = 0; i < 75; ++i) {
            input.rightZ += .055f;
            frame();
        }
        require(vector(move, moveHand).currentState.y > .85f, "consistent fast position samples reach the top of the swing range");
        input.tracked = false;
        input.hasVelocity = true;
        input.controllerSpeed = 5.5f;
        for (int i = 0; i < 85; ++i) frame();
        require(vector(move, moveHand).currentState.y > .85f, "valid velocity sustains fast swing without position tracking");
        input.hasVelocity = false;
        frame();
        require(vector(move, moveHand).currentState.y > .38f, "brief tracking loss keeps the last pace");
        for (int i = 0; i < 58; ++i) frame();
        const auto fading = vector(move, moveHand).currentState.y;
        require(fading > 0 && fading < .85f, "extended tracking loss fades the last pace");
        for (int i = 0; i < 25; ++i) frame();
        require(vector(move, moveHand).currentState.y == 0, "extended tracking loss comes to a stop");
        input.tracked = true;
        input.leftOccluded = false;
        require(swing(80) > .55f, "re-arm after tracking recovery");
        input.focused = false;
        frame();
        require(!vector(move, moveHand).isActive && vector(move, moveHand).currentState.y == 0, "focus loss passes through inactive zero");
        input.focused = true;
        frame();
        require(vector(move, moveHand).currentState.y == 0, "refocus starts from rest");
    }
    success(xrDestroySession(session), "destroy OpenXR session");
    success(xrDestroyInstance(instance), "destroy OpenXR instance");
    std::printf("PASS (%u checks, peak %.2f)\n", checks, peak);
}
