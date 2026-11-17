#include "runtime.h"
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include <cmath>

namespace {

struct Action {
    std::string name;
    XrActionType type;
    XrActionSet set;
};

struct Space {
    XrReferenceSpaceType reference{};
    XrAction action{};
};

Input input;
Calls calls{};
XrTime time = 1'000'000'000;
std::vector<std::string> paths;
std::vector<Action*> actions;
std::vector<Space*> spaces;
std::vector<XrActionSuggestedBinding> bindings;
uintptr_t nextHandle = 0x1000;
bool calibration = false;
bool scene = false;
bool audiobook = false;
bool readyEvent = false;
int calibrationFrame = 0;
XrSession calibrationSession{};
ID3D11Device* calibrationDevice{};
std::unordered_map<XrSwapchain, Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;

template<class Handle>
Handle newHandle() {
    ++nextHandle;
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(nextHandle);
    else return (uint64_t{1} << 40) | nextHandle; // Exercise 64-bit handle values in a 32-bit process.
}

XrResult XRAPI_CALL createInstance(const XrInstanceCreateInfo* info, XrInstance* handle) {
    calibration = strcmp(info->applicationInfo.applicationName, "StrideXR Calibration") == 0;
    scene = strcmp(info->applicationInfo.applicationName, "StrideXR Test Scene") == 0;
    audiobook = strcmp(info->applicationInfo.applicationName, "StrideXR audiobook test") == 0;
    calibrationFrame = 0;
    *handle = newHandle<XrInstance>();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroyInstance(XrInstance) {
    for (auto action : actions) delete action;
    for (auto space : spaces) delete space;
    actions.clear();
    spaces.clear();
    bindings.clear();
    paths.clear();
    calls = {};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL extensions(const char*, uint32_t cap, uint32_t* count, XrExtensionProperties* properties) {
    *count = 1;
    if (cap) {
        strcpy_s(properties[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        properties[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL instanceProperties(XrInstance, XrInstanceProperties* properties) {
    strcpy_s(properties->runtimeName, "StrideXR simulated runtime");
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL stringToPath(XrInstance, const char* value, XrPath* path) {
    auto found = std::ranges::find(paths, value);
    if (found == paths.end()) {
        paths.push_back(value);
        *path = paths.size();
    } else {
        *path = 1 + (found - paths.begin());
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL pathToString(XrInstance, XrPath path, uint32_t cap, uint32_t* count, char* out) {
    if (!path || path > paths.size()) return XR_ERROR_PATH_INVALID;
    const auto& value = paths[static_cast<size_t>(path - 1)];
    *count = static_cast<uint32_t>(value.size() + 1);
    if (!cap) return XR_SUCCESS;
    if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    strcpy_s(out, cap, value.c_str());
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* set) {
    *set = newHandle<XrActionSet>();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySet(XrActionSet) { return XR_SUCCESS; }
XrResult XRAPI_CALL createAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    auto value = new Action{info->actionName, info->actionType, set};
    actions.push_back(value);
    *action = reinterpret_cast<XrAction>(value);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL suggest(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    calls.menuBindings = 0;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        auto action = reinterpret_cast<Action*>(info->suggestedBindings[i].action);
        if (input.rejectExtras && (action->name == "rest_l" || action->name == "rest_r")) return XR_ERROR_PATH_UNSUPPORTED;
        if (action->name == "book_play_pause" &&
            paths.at(size_t(info->suggestedBindings[i].binding - 1)) == "/user/hand/left/input/menu/click")
            ++calls.menuBindings;
    }
    calls.bindings = info->countSuggestedBindings;
    bindings.assign(info->suggestedBindings, info->suggestedBindings + calls.bindings);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSession(XrInstance, const XrSessionCreateInfo* info, XrSession* session) {
    *session = newHandle<XrSession>();
    if (calibration || audiobook || scene) {
        const auto* binding = static_cast<const XrGraphicsBindingD3D11KHR*>(info->next);
        if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        calibrationDevice = binding->device;
        calibrationSession = *session;
        readyEvent = true;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL beginSession(XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL endSession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL destroySession(XrSession) {
    textures.clear();
    calibrationDevice = nullptr;
    for (auto space : spaces) delete space;
    spaces.clear();
    calls.attached = calls.synced = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL attach(XrSession, const XrSessionActionSetsAttachInfo* info) {
    if (input.rejectAttach && info->countActionSets > 1) return XR_ERROR_RUNTIME_FAILURE;
    calls.attached = info->countActionSets;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createReference(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    auto value = new Space{info->referenceSpaceType, XR_NULL_HANDLE};
    spaces.push_back(value);
    *space = reinterpret_cast<XrSpace>(value);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSpace(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    auto value = new Space{{}, info->action};
    spaces.push_back(value);
    *space = reinterpret_cast<XrSpace>(value);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL waitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
#ifdef _WIN64
    if (calibration) {
        ++calibrationFrame;
        if (calibrationFrame > 1500) return XR_ERROR_RUNTIME_FAILURE;
        const float amplitude = calibrationFrame < 425 ? 0.07f : calibrationFrame < 850 ? 0.15f : 0.23f;
        input.left = input.right = true;
        input.leftZ = amplitude * std::sin(float(calibrationFrame) * 6.283185307f / 90.0f);
        input.rightZ = -input.leftZ;
    }
    if (scene) {
        ++calibrationFrame;
        if (calibrationFrame > 115) return XR_ERROR_RUNTIME_FAILURE;
        input.inverted = true;
        input.leftX = -.28f;
        input.rightX = .28f;
        input.leftZ = input.rightZ = .05f * std::sin(float(calibrationFrame) * .20f);
    }
#endif
    time += 11'111'111;
    state->predictedDisplayTime = time;
    state->predictedDisplayPeriod = 11'111'111;
    state->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSpace space, XrSpace, XrTime, XrSpaceLocation* location) {
    auto value = reinterpret_cast<Space*>(space);
    location->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    location->pose.orientation.w = 1.0f;
    location->pose.position = {input.headX, 1.7f, input.headZ};
    if (value->action) {
        auto action = reinterpret_cast<Action*>(value->action);
        const bool left = action->name == "grip_l";
        location->pose.position = {left ? input.leftX : input.rightX, input.inverted ? 1.68f : 1.0f, left ? input.leftZ : input.rightZ};
        if (input.inverted) location->pose.orientation = {0, 0, 1, 0};
        if (left && input.leftOccluded) location->locationFlags = 0;
        if (input.hasVelocity && location->next) {
            auto* velocity = static_cast<XrSpaceVelocity*>(location->next);
            velocity->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
            velocity->linearVelocity = {0, 0, -input.controllerSpeed};
        }
    }
    if ((input.tracked && location->locationFlags) || !value->action)
        location->locationFlags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL sync(XrSession, const XrActionsSyncInfo* info) {
    calls.synced = info->countActiveActionSets;
    return input.focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}
XrResult XRAPI_CALL getBool(XrSession, const XrActionStateGetInfo* info, XrActionStateBoolean* state) {
    auto action = reinterpret_cast<Action*>(info->action);
    if (action->name == "book_play_pause") {
        state->isActive = input.focused && calls.synced > 1 && calls.menuBindings == 1;
        state->currentState = input.menuDown;
        state->changedSinceLastSync = input.menuChanged;
        return XR_SUCCESS;
    }
#ifdef _WIN64
    if (scene) {
        state->isActive = XR_TRUE;
        state->currentState = (action->name == "advance" && (calibrationFrame == 30 || calibrationFrame == 50 || calibrationFrame == 78)) ||
            (action->name == "back" && calibrationFrame == 106) || (action->name == "book_y" && calibrationFrame == 12);
        return XR_SUCCESS;
    }
    if (calibration) {
        state->isActive = XR_TRUE;
        state->currentState = action->name == "advance" ?
            calibrationFrame == 5 || calibrationFrame == 430 || calibrationFrame == 855 || calibrationFrame == 1280 :
            action->name != "back";
        return XR_SUCCESS;
    }
#endif
    state->isActive = input.focused && calls.synced > 1;
    state->currentState = action->name == "rest_l" ? input.left : input.right;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL getVector(XrSession, const XrActionStateGetInfo* info, XrActionStateVector2f* state) {
    auto action = reinterpret_cast<Action*>(info->action);
    state->isActive = input.focused;
    if (scene && action->name == "edit_panel") {
        state->currentState = {calibrationFrame < 30 ? 1.0f : 0.0f, 1.0f};
        return XR_SUCCESS;
    }
    const bool left = info->subactionPath && paths.at(static_cast<size_t>(info->subactionPath - 1)) == "/user/hand/left";
    const bool move = action->name == "move" || (action->name == "thumbstick" && left);
    state->currentState = move ? XrVector2f{input.stickX, input.stickY} : XrVector2f{0.1f, 0.1f};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL getFloat(XrSession, const XrActionStateGetInfo* info, XrActionStateFloat* state) {
    auto action = reinterpret_cast<Action*>(info->action);
    state->isActive = input.focused;
    state->currentState = action->name == "move_y" ? input.stickY : 0.32f;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerate(XrSession, const XrBoundSourcesForActionEnumerateInfo* info,
                              uint32_t capacity, uint32_t* count, XrPath* sources) {
    *count = 0;
    for (const auto& binding : bindings) {
        if (binding.action != info->action) continue;
        if (capacity && *count >= capacity) return XR_ERROR_SIZE_INSUFFICIENT;
        if (capacity) sources[*count] = binding.binding;
        ++*count;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL getSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* system) {
    *system = 1;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL getSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
    properties->graphicsProperties = {4096, 4096, 16};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL graphicsRequirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* output) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)) ||
        FAILED(adapter->GetDesc1(&desc))) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    output->adapterLuid = desc.AdapterLuid;
    output->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL pollEvent(XrInstance, XrEventDataBuffer* data) {
    if (!readyEvent) return XR_EVENT_UNAVAILABLE;
    readyEvent = false;
    XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    event.session = calibrationSession;
    event.state = XR_SESSION_STATE_READY;
    memcpy(data, &event, sizeof(event));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerateFormats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats) {
    *count = 1;
    if (capacity) formats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = info->width;
    texture.Height = info->height;
    texture.ArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = static_cast<DXGI_FORMAT>(info->format);
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> image;
    if (!calibrationDevice || FAILED(calibrationDevice->CreateTexture2D(&texture, nullptr, &image)))
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *swapchain = newHandle<XrSwapchain>();
    textures.emplace(*swapchain, std::move(image));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL swapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = 1;
    if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)->texture = textures.at(swapchain).Get();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquireImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) { *index = 0; return XR_SUCCESS; }
XrResult XRAPI_CALL waitImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL releaseImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL beginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL locateViews(XrSession, const XrViewLocateInfo*, XrViewState* state,
                                uint32_t capacity, uint32_t* count, XrView* views) {
    state->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    *count = 2;
    if (capacity < 2) return capacity == 0 ? XR_SUCCESS : XR_ERROR_SIZE_INSUFFICIENT;
    for (unsigned i = 0; i < 2; ++i) {
        views[i].pose = {{0, 0, 0, 1}, {i == 0 ? -.032f : .032f, 1.7f, 0}};
        views[i].fov = {-.83f, .83f, .83f, -.83f};
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL endFrame(XrSession, const XrFrameEndInfo* info) {
    calls.layers = info->layerCount;
    if (audiobook && (info->layerCount == 1 || info->layerCount == 2)) {
        if (info->layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) return XR_ERROR_LAYER_INVALID;
        if (info->layerCount == 1) return XR_SUCCESS;
        if (info->layers[1]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) return XR_ERROR_LAYER_INVALID;
        const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
        if (reinterpret_cast<Space*>(quad->space)->reference != XR_REFERENCE_SPACE_TYPE_VIEW ||
            quad->eyeVisibility != XR_EYE_VISIBILITY_BOTH || quad->pose.position.y <= 0 ||
            !quad->subImage.swapchain || std::abs(quad->pose.position.x - .25f) > .001f ||
            std::abs(quad->pose.position.y - .45f) > .001f) return XR_ERROR_LAYER_INVALID;
        return XR_SUCCESS;
    }
    if (scene) {
        if (info->layerCount != 3 || info->layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION ||
            info->layers[1]->type != XR_TYPE_COMPOSITION_LAYER_QUAD ||
            info->layers[2]->type != XR_TYPE_COMPOSITION_LAYER_QUAD) return XR_ERROR_LAYER_INVALID;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
        return projection->viewCount == 2 && projection->views[1].subImage.imageRect.offset.x > 0
            ? XR_SUCCESS : XR_ERROR_LAYER_INVALID;
    }
    return info->layerCount == 1 && info->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD
        ? XR_SUCCESS : XR_ERROR_LAYER_INVALID;
}

XrResult XRAPI_CALL getProc(XrInstance, const char* name, PFN_xrVoidFunction* out) {
#define FN(api, fn) if (strcmp(name, #api) == 0) { *out = reinterpret_cast<PFN_xrVoidFunction>(fn); return XR_SUCCESS; }
    FN(xrGetInstanceProcAddr, getProc)
    FN(xrCreateInstance, createInstance)
    FN(xrDestroyInstance, destroyInstance)
    FN(xrEnumerateInstanceExtensionProperties, extensions)
    FN(xrGetInstanceProperties, instanceProperties)
    FN(xrStringToPath, stringToPath)
    FN(xrPathToString, pathToString)
    FN(xrCreateActionSet, createSet)
    FN(xrDestroyActionSet, destroySet)
    FN(xrCreateAction, createAction)
    FN(xrSuggestInteractionProfileBindings, suggest)
    FN(xrCreateSession, createSession)
    FN(xrBeginSession, beginSession)
    FN(xrEndSession, endSession)
    FN(xrDestroySession, destroySession)
    FN(xrAttachSessionActionSets, attach)
    FN(xrCreateReferenceSpace, createReference)
    FN(xrCreateActionSpace, createSpace)
    FN(xrWaitFrame, waitFrame)
    FN(xrLocateSpace, locate)
    FN(xrSyncActions, sync)
    FN(xrGetActionStateBoolean, getBool)
    FN(xrGetActionStateVector2f, getVector)
    FN(xrGetActionStateFloat, getFloat)
    FN(xrEnumerateBoundSourcesForAction, enumerate)
    FN(xrGetSystem, getSystem)
    FN(xrGetSystemProperties, getSystemProperties)
    FN(xrGetD3D11GraphicsRequirementsKHR, graphicsRequirements)
    FN(xrPollEvent, pollEvent)
    FN(xrEnumerateSwapchainFormats, enumerateFormats)
    FN(xrCreateSwapchain, createSwapchain)
    FN(xrEnumerateSwapchainImages, swapchainImages)
    FN(xrAcquireSwapchainImage, acquireImage)
    FN(xrWaitSwapchainImage, waitImage)
    FN(xrReleaseSwapchainImage, releaseImage)
    FN(xrBeginFrame, beginFrame)
    FN(xrLocateViews, locateViews)
    FN(xrEndFrame, endFrame)
#undef FN
    *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
} // namespace

extern "C" __declspec(dllexport) void TestSetInput(const Input* value) { input = *value; }
extern "C" __declspec(dllexport) Calls TestReadCalls() { return calls; }
extern "C" XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest* request) {
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    request->getInstanceProcAddr = getProc;
    return XR_SUCCESS;
}
