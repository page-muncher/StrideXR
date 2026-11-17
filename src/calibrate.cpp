#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>

#include "calibration.h"
#include "audiobook_overlay.h"
#include "gesture.h"
#include "settings_io.h"
#include "test_scene.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string_view>

namespace {
using Microsoft::WRL::ComPtr;
constexpr int Width = 960;
constexpr int Height = 600;
constexpr wchar_t ResultFile[] = L"Result.ini";
constexpr std::array<const wchar_t*, 3> Paces{L"Slow", L"Medium", L"Fast"};
constexpr XrSpaceLocationFlags Tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

void check(XrResult result, const char* operation) {
    if (XR_FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ").");
}

void checkGraphics(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ").");
}

#define XR_FUNCTIONS(X) \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) X(xrGetInstanceProcAddr) \
    X(xrGetInstanceProperties) X(xrGetSystem) X(xrCreateSession) X(xrDestroySession) X(xrPollEvent) X(xrBeginSession) \
    X(xrEndSession) X(xrCreateReferenceSpace) X(xrLocateSpace) X(xrCreateActionSet) X(xrCreateAction) \
    X(xrStringToPath) X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) \
    X(xrGetActionStateBoolean) X(xrCreateActionSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) X(xrGetActionStateVector2f)

struct OpenXR {
    HMODULE loader;
#define DECLARE(name) PFN_##name name{};
    XR_FUNCTIONS(DECLARE)
#undef DECLARE
    explicit OpenXR(const std::filesystem::path& dll) :
        loader(LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!loader) throw std::runtime_error("Could not load the bundled OpenXR loader.");
#define LOAD(name) name = reinterpret_cast<PFN_##name>(GetProcAddress(loader, #name)); \
        if (!name) throw std::runtime_error("The OpenXR loader is missing " #name ".");
        XR_FUNCTIONS(LOAD)
#undef LOAD
    }
    ~OpenXR() { FreeLibrary(loader); }
};
#undef XR_FUNCTIONS

class Canvas {
public:
    Canvas() {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Width;
        info.bmiHeader.biHeight = -Height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) throw std::runtime_error("Could not allocate the calibration display.");
        oldBitmap = SelectObject(dc, bitmap);
        for (size_t i = 0; i < fonts.size(); ++i) {
            fonts[i] = CreateFontW(-std::array<int, 3>{48, 32, 24}[i], 0, 0, 0, i == 0 ? FW_BOLD : FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        }
        SetBkMode(dc, TRANSPARENT);
    }
    ~Canvas() {
        SelectObject(dc, oldBitmap);
        for (auto font : fonts) DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void* data() const { return pixels; }

    void drawSceneHud(float stick, bool reverse, int mode, const SubtitleStyle& style) {
        constexpr const wchar_t* modes[] = {L"Position", L"Opacity", L"Box lines", L"Layout"};
        constexpr const wchar_t* layouts[] = {L"Current", L"Previous + current", L"Current + next", L"Both", L"History"};
        fill(0, 0, Width, 210, RGB(17, 24, 31));
        wchar_t value[140]{};
        swprintf_s(value, L"Stick Y %+d%%  •  %s", int(stick * 100), reverse ? L"Reverse pose" : L"Forward swing");
        text(30, 12, value, 1, RGB(230, 246, 247));
        swprintf_s(value, L"%s  |  X %+d  Y %+d  Opacity %d%%  Box lines %d  |  %s", modes[mode],
            style.x, style.y, style.opacity, style.lines, layouts[style.layout]);
        text(30, 63, value, 2, RGB(129, 223, 206));
        text(30, 106, L"Right stick adjusts  •  A changes setting  •  X hides/shows sample", 2, RGB(218, 231, 235));
        text(30, 151, L"Left Menu or Y plays/pauses the book  •  B saves and exits", 2, RGB(218, 231, 235));
        GdiFlush();
    }

    void draw(std::wstring_view title, std::wstring_view instruction, std::wstring_view detail,
              std::wstring_view speeds, float progress, float stick, float distance, bool armed) {
        fill(0, 0, Width, Height, RGB(15, 25, 39));
        fill(34, 21, Width - 34, 192, RGB(24, 39, 57));
        text(57, 36, title, 0, RGB(240, 249, 255));
        text(57, 98, instruction, 1, RGB(212, 232, 245));
        text(57, 150, detail, 2, RGB(143, 206, 211));
        fill(57, 202, Width - 57, 210, RGB(42, 62, 79));
        fill(57, 202, 57 + int((Width - 114) * progress), 210, RGB(87, 219, 183));

        fill(57, 228, 903, 465, RGB(19, 33, 49));
        polygon({{{380, 249}, {580, 249}, {854, 449}, {106, 449}}}, RGB(35, 63, 80));
        polygon({{{377, 249}, {385, 249}, {127, 449}, {106, 449}}}, RGB(87, 219, 183));
        polygon({{{575, 249}, {583, 249}, {854, 449}, {833, 449}}}, RGB(87, 219, 183));
        for (int i = 0; i < 7; ++i) {
            float depth = std::fmod(distance + static_cast<float>(i) / 7.0f, 1.0f);
            depth *= depth;
            int y = 249 + int(195 * depth);
            int span = 95 + int(260 * depth);
            fill(480 - span, y, 480 + span, y + 2 + int(3 * depth), RGB(104, 152, 176));
        }
        text(57, 478, speeds, 2, RGB(212, 232, 245));
        fill(57, 520, 903, 533, RGB(42, 62, 79));
        fill(57, 520, 57 + int(846 * std::clamp(stick, 0.0f, 1.0f)), 533, RGB(87, 219, 183));
        wchar_t label[100]{};
        swprintf_s(label, L"Stick output %d%%    •    %s", int(stick * 100), armed ? L"Thumbrests engaged" : L"Touch the profile’s thumbrests");
        text(57, 542, label, 2, RGB(212, 232, 245));
        GdiFlush();
    }
private:
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ oldBitmap{};
    void* pixels{};
    std::array<HFONT, 3> fonts{};
    void fill(int left, int top, int right, int bottom, COLORREF color) {
        RECT rect{left, top, right, bottom};
        SetDCBrushColor(dc, color);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void polygon(const std::array<POINT, 4>& points, COLORREF color) {
        const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
        SetDCBrushColor(dc, color);
        Polygon(dc, points.data(), int(points.size()));
        SelectObject(dc, brush);
        SelectObject(dc, pen);
    }
    void text(int x, int y, std::wstring_view value, size_t font, COLORREF color) {
        auto old = SelectObject(dc, fonts[font]);
        SetTextColor(dc, color);
        TextOutW(dc, x, y, value.data(), int(value.size()));
        SelectObject(dc, old);
    }
};

class CalibrationApp {
public:
    CalibrationApp(OpenXR& api, std::filesystem::path directory, bool test)
        : xr(api), folder(std::move(directory)), motion(readMotionSettings(folder / L"Input.ini", L"Calibration")),
          gate(readThumbGate(folder / L"Input.ini", L"Calibration")), testMode(test) {
        const auto ini = folder / L"Input.ini";
        reverseEnabled = GetPrivateProfileIntW(L"Calibration", L"ReverseWaggle", 1, ini.c_str()) != 0;
        if (testMode) {
            style.x = static_cast<int>(GetPrivateProfileIntW(L"Subtitles", L"X", 0, ini.c_str()));
            style.y = static_cast<int>(GetPrivateProfileIntW(L"Subtitles", L"Y", 40, ini.c_str()));
            style.opacity = static_cast<int>(GetPrivateProfileIntW(L"Subtitles", L"Opacity", 100, ini.c_str()));
            style.lines = static_cast<int>(GetPrivateProfileIntW(L"Subtitles", L"Lines", 4, ini.c_str()));
            style.layout = static_cast<int>(GetPrivateProfileIntW(L"Subtitles", L"Layout", 0, ini.c_str()));
            style.clamp();
        }
    }

    ~CalibrationApp() {
        if (session) xr.xrDestroySession(session);
        if (instance) xr.xrDestroyInstance(instance);
    }

    void initialize(bool probe) {
        uint32_t count = 0;
        check(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "Enumerating OpenXR extensions");
        std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
        check(xr.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "Reading OpenXR extensions");
        auto supports = [&](const char* name) {
            return std::ranges::any_of(extensions, [&](const auto& item) { return std::string_view(item.extensionName) == name; });
        };
        if (!supports(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) throw std::runtime_error("The active OpenXR runtime does not support Direct3D 11.");
        std::vector<const char*> enabled{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        const bool plus = supports(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
        const bool pro = supports(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
        if (plus) enabled.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
        if (pro) enabled.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, testMode ? "StrideXR Test Scene" : "StrideXR Calibration");
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
        info.enabledExtensionNames = enabled.data();
        check(xr.xrCreateInstance(&info, &instance), "Opening the active OpenXR runtime");
        setupActions(plus, pro);
        if (probe) {
            XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
            check(xr.xrGetInstanceProperties(instance, &properties), "Reading runtime name");
            std::ofstream(folder / L"Probe.txt") << properties.runtimeName << "\nTouch action bindings accepted\n";
            return;
        }
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        check(xr.xrGetSystem(instance, &systemInfo, &system), "Finding the headset. Connect it to the PC before calibrating");
        setupSession();
        setupDisplay();
        if (testMode) setupScene();
    }

    void run() {
        Canvas canvas;
        const ULONGLONG started = GetTickCount64();
        while (!finished && GetFileAttributesW((folder / L"Cancel").c_str()) == INVALID_FILE_ATTRIBUTES) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while (xr.xrPollEvent(instance, &event) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) return;
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    auto state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        check(xr.xrBeginSession(session, &begin), "Starting headset calibration");
                        running = true;
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        xr.xrEndSession(session);
                        return;
                    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) return;
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            if (!running) {
                if (GetTickCount64() - started > 20000) throw std::runtime_error("The headset session did not become ready. Close the game and return to VR.");
                Sleep(20);
                continue;
            }
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            check(xr.xrWaitFrame(session, &wait, &frame), "Waiting for a headset frame");
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            check(xr.xrBeginFrame(session, &begin), "Beginning the calibration frame");
            if (frame.shouldRender) {
                update(frame.predictedDisplayTime);
                if (testMode) { canvas.drawSceneHud(stick, gesture.reversePose(), editMode, style); upload(canvas); }
                else draw(canvas);
            }
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.space = viewSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = swapchain;
            quad.subImage.imageRect.extent = {Width, Height};
            quad.pose.orientation.w = 1.0f;
            quad.pose.position.z = -1.8f;
            quad.size = {1.60f, 1.0f};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frame.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            std::array<const XrCompositionLayerBaseHeader*, 3> layers{};
            if (frame.shouldRender) {
                if (testMode) {
                    if (drawScene(frame.predictedDisplayTime)) layers[end.layerCount++] =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                    quad.subImage.imageRect.extent.height = 210;
                    quad.pose.position = {0, -.43f, -1.55f};
                    quad.size = {1.28f, 1.28f * 210 / Width};
                }
                layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                if (testMode) {
                    const auto* subtitle = subtitlePreview.preview(session, sampleCaption(), style, showSubtitle);
                    if (subtitle) layers[end.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(subtitle);
                }
                end.layers = layers.data();
            }
            check(xr.xrEndFrame(session, &end), "Displaying the calibration frame");
        }
    }

private:
    OpenXR& xr;
    std::filesystem::path folder;
    MotionSettings motion;
    ThumbGate gate;
    ArmGesture gesture;
    SwingSample sample;
    std::array<float, 3> speeds{};
    std::array<XrAction, 2> rest{}, grip{};
    std::array<XrSpace, 2> handSpace{};
    XrAction advance{}, back{}, editor{}, toggleSubtitle{}, bookMenu{}, bookY{};
    XrActionSet actions{};
    XrInstance instance{};
    XrSystemId system{};
    XrSession session{};
    XrSpace viewSpace{}, localSpace{};
    XrSwapchain swapchain{};
    std::vector<XrSwapchainImageD3D11KHR> images;
    DXGI_FORMAT swapchainFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    XrTime lastTime{};
    size_t step = 0;
    float remaining = 0.0f, stick = 0.0f, distance = 0.0f;
    bool running = false, finished = false, armed = false, previousA = false, previousB = false;
    std::wstring message;
    bool testMode, reverseEnabled = true;
    bool showSubtitle = true, previousX = false, haveOrigin = false;
    int editMode = 0;
    float editX = 0, editY = 0, originY = 0;
    XrVector3f travel{};
    std::array<XrSpaceLocation, 2> trackedHands{};
    std::array<XrSpaceVelocity, 2> handVelocities{};
    XrSpaceVelocity headVelocity{XR_TYPE_SPACE_VELOCITY};
    std::optional<XrPosef> trackedHead;
    TestScene scene;
    AudiobookOverlay subtitlePreview;
    SubtitleStyle style;
    std::wstring sampleCaption() const {
        constexpr const wchar_t* rows[] = {
            L"She waited under the awning.", L"The wind died down.",
            L"The rain had finally stopped.",
            L"She opened the door and stepped into the street.",
            L"A light still burned in the window across the road.",
            L"The rest of the city was quiet.", L"She listened for a moment.",
            L"Then she started walking again.",
            L"A tram bell rang at the corner.", L"She looked back one last time."};
        std::wstring text;
        const bool before = style.layout == 1 || style.layout == 3 || style.layout == 4;
        const bool after = style.layout == 2 || style.layout == 3;
        const int first = before ? 0 : 2;
        const int next = 2 + style.lines;
        const int end = next + (after ? 2 : 0);
        for (int i = first; i < end; ++i) {
            if (i != first) text += L'\n';
            if (i < 2 || i >= next) text += L'\x01';
            text += rows[i];
        }
        return text;
    }
    XrSwapchain sceneSwapchain{};
    std::vector<ComPtr<ID3D11RenderTargetView>> sceneTargets;
    std::array<XrCompositionLayerProjectionView, 2> eyes{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    void setupActions(bool plus, bool pro) {
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(set.actionSetName, "calibration");
        strcpy_s(set.localizedActionSetName, "StrideXR calibration");
        check(xr.xrCreateActionSet(instance, &set, &actions), "Creating the calibration action set");
        auto action = [&](const char* name, const char* label, XrActionType type) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(info.actionName, name);
            strcpy_s(info.localizedActionName, label);
            info.actionType = type;
            XrAction handle{};
            check(xr.xrCreateAction(actions, &info, &handle), "Creating a calibration action");
            return handle;
        };
        rest = {action("rest_l", "Left thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT),
                action("rest_r", "Right thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT)};
        grip = {action("grip_l", "Left grip pose", XR_ACTION_TYPE_POSE_INPUT),
                action("grip_r", "Right grip pose", XR_ACTION_TYPE_POSE_INPUT)};
        advance = action("advance", "Next calibration step", XR_ACTION_TYPE_BOOLEAN_INPUT);
        back = action("back", "Repeat calibration step", XR_ACTION_TYPE_BOOLEAN_INPUT);
        auto path = [&](const char* value) {
            XrPath handle{};
            check(xr.xrStringToPath(instance, value, &handle), "Resolving a Touch input path");
            return handle;
        };
        std::vector<XrActionSuggestedBinding> bindings = {
            {rest[0], path("/user/hand/left/input/thumbrest/touch")}, {rest[1], path("/user/hand/right/input/thumbrest/touch")},
            {grip[0], path("/user/hand/left/input/grip/pose")}, {grip[1], path("/user/hand/right/input/grip/pose")},
            {advance, path("/user/hand/right/input/a/click")}, {back, path("/user/hand/right/input/b/click")}
        };
        if (testMode) {
            editor = action("edit_panel", "Adjust subtitle panel", XR_ACTION_TYPE_VECTOR2F_INPUT);
            toggleSubtitle = action("toggle_panel", "Show or hide subtitle preview", XR_ACTION_TYPE_BOOLEAN_INPUT);
            bindings.push_back({editor, path("/user/hand/right/input/thumbstick")});
            bindings.push_back({toggleSubtitle, path("/user/hand/left/input/x/click")});
            bookMenu = action("book_menu", "Play or pause audiobook (Menu)", XR_ACTION_TYPE_BOOLEAN_INPUT);
            bookY = action("book_y", "Play or pause audiobook (Y)", XR_ACTION_TYPE_BOOLEAN_INPUT);
            bindings.push_back({bookY, path("/user/hand/left/input/y/click")});
            bindings.push_back({bookMenu, path("/user/hand/left/input/menu/click")});
        }
        auto suggest = [&](const char* profile) {
            XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            info.interactionProfile = path(profile);
            info.countSuggestedBindings = uint32_t(std::size(bindings));
            info.suggestedBindings = bindings.data();
            auto result = xr.xrSuggestInteractionProfileBindings(instance, &info);
            if (testMode && XR_FAILED(result)) {
                --info.countSuggestedBindings;
                result = xr.xrSuggestInteractionProfileBindings(instance, &info);
            }
            return result;
        };
        bool accepted = XR_SUCCEEDED(suggest("/interaction_profiles/oculus/touch_controller"));
        if (plus) {
            accepted |= XR_SUCCEEDED(suggest("/interaction_profiles/meta/touch_plus_controller"));
            accepted |= XR_SUCCEEDED(suggest("/interaction_profiles/meta/touch_controller_plus"));
        }
        if (pro) accepted |= XR_SUCCEEDED(suggest("/interaction_profiles/facebook/touch_controller_pro"));
        if (!accepted) throw std::runtime_error("The active OpenXR runtime did not accept Touch thumbrest bindings.");
    }

    void setupSession() {
        PFN_xrVoidFunction function{};
        check(xr.xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", &function), "Finding the runtime graphics requirements");
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        check(reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(function)(instance, system, &requirements), "Reading the runtime graphics requirements");
        ComPtr<IDXGIFactory1> factory;
        checkGraphics(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Creating a DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        bool matching = false;
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) == S_OK; ++index) {
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart) { matching = true; break; }
            adapter.Reset();
        }
        if (!matching) throw std::runtime_error("The OpenXR runtime's graphics adapter was not found.");
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level{};
        checkGraphics(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested, UINT(std::size(requested)), D3D11_SDK_VERSION, &device, &level, &context), "Creating the calibration graphics device");
        if (level < requirements.minFeatureLevel) throw std::runtime_error("The graphics device is below the OpenXR runtime's feature level.");
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO, &binding};
        create.systemId = system;
        check(xr.xrCreateSession(instance, &create, &session), "Creating a calibration session. Close any open VR game first");
        if (testMode) subtitlePreview.initialize(instance, xr.xrGetInstanceProcAddr, create);
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.poseInReferenceSpace.orientation.w = 1.0f;
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        check(xr.xrCreateReferenceSpace(session, &reference, &viewSpace), "Creating the headset reference space");
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        check(xr.xrCreateReferenceSpace(session, &reference, &localSpace), "Creating the tracking reference space");
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &actions;
        check(xr.xrAttachSessionActionSets(session, &attach), "Attaching Touch controller actions");
        for (size_t i = 0; i < handSpace.size(); ++i) {
            XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space.action = grip[i];
            space.poseInActionSpace.orientation.w = 1.0f;
            check(xr.xrCreateActionSpace(session, &space, &handSpace[i]), "Creating a controller tracking space");
        }
    }

    void setupScene() {
        scene.initialize(device.Get());
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = swapchainFormat;
        info.width = 2 * TestScene::EyeWidth;
        info.height = TestScene::EyeHeight;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        check(xr.xrCreateSwapchain(session, &info, &sceneSwapchain), "Creating the stereo scene swapchain");
        uint32_t count = 0;
        check(xr.xrEnumerateSwapchainImages(sceneSwapchain, 0, &count, nullptr), "Finding stereo scene images");
        std::vector<XrSwapchainImageD3D11KHR> sceneImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        check(xr.xrEnumerateSwapchainImages(sceneSwapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(sceneImages.data())), "Reading stereo scene images");
        sceneTargets.resize(count);
        for (size_t i = 0; i < sceneTargets.size(); ++i) {
            D3D11_RENDER_TARGET_VIEW_DESC target{};
            target.Format = static_cast<DXGI_FORMAT>(info.format);
            target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            checkGraphics(device->CreateRenderTargetView(sceneImages[i].texture, &target, &sceneTargets[i]),
                "Creating stereo render targets");
        }
        projection.space = localSpace;
        projection.viewCount = uint32_t(eyes.size());
        projection.views = eyes.data();
    }

    bool drawScene(XrTime time) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = time;
        locate.space = localSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        uint32_t count = 0;
        check(xr.xrLocateViews(session, &locate, &state, uint32_t(views.size()), &count, views.data()), "Locating the stereo cameras");
        constexpr auto valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if (count != views.size() || (state.viewStateFlags & valid) != valid) return false;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        check(xr.xrAcquireSwapchainImage(sceneSwapchain, &acquire, &index), "Acquiring the stereo scene image");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        check(xr.xrWaitSwapchainImage(sceneSwapchain, &wait), "Waiting for the stereo scene image");
        scene.draw(context.Get(), sceneTargets.at(index).Get(), views, trackedHands, travel, originY, gesture.reversePose());
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        check(xr.xrReleaseSwapchainImage(sceneSwapchain, &release), "Releasing the stereo scene image");
        for (size_t i = 0; i < eyes.size(); ++i) {
            eyes[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            eyes[i].pose = views[i].pose;
            eyes[i].fov = views[i].fov;
            eyes[i].subImage.swapchain = sceneSwapchain;
            eyes[i].subImage.imageRect = {{int(i) * TestScene::EyeWidth, 0}, {TestScene::EyeWidth, TestScene::EyeHeight}};
        }
        return true;
    }

    void setupDisplay() {
        uint32_t count = 0;
        check(xr.xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "Finding headset texture formats");
        std::vector<int64_t> formats(count);
        check(xr.xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "Reading headset texture formats");
        const auto color = std::ranges::find(formats, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        const auto linear = std::ranges::find(formats, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (color == formats.end() && linear == formats.end()) throw std::runtime_error("The runtime has no supported calibration texture format.");
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format = color != formats.end() ? *color : *linear;
        swapchainFormat = static_cast<DXGI_FORMAT>(create.format);
        create.width = Width;
        create.height = Height;
        create.sampleCount = create.faceCount = create.arraySize = create.mipCount = 1;
        check(xr.xrCreateSwapchain(session, &create, &swapchain), "Creating the headset display texture");
        check(xr.xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), "Finding headset display textures");
        images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        check(xr.xrEnumerateSwapchainImages(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
            "Reading headset display textures");
    }

    bool pressed(XrAction action) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xr.xrGetActionStateBoolean(session, &info, &state)) && state.isActive && state.currentState;
    }

    void tracking(XrTime time) {
        trackedHead.reset();
        trackedHands = {};
        headVelocity = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION, &headVelocity};
        if (XR_SUCCEEDED(xr.xrLocateSpace(viewSpace, localSpace, time, &head)) &&
            (head.locationFlags & Tracked) == Tracked && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            trackedHead = head.pose;
            if (!haveOrigin) { originY = head.pose.position.y; haveOrigin = true; }
        }
        for (size_t i = 0; i < handSpace.size(); ++i) {
            handVelocities[i] = {XR_TYPE_SPACE_VELOCITY};
            trackedHands[i] = {XR_TYPE_SPACE_LOCATION, &handVelocities[i]};
            if (XR_FAILED(xr.xrLocateSpace(handSpace[i], localSpace, time, &trackedHands[i]))) {
                trackedHands[i].locationFlags = 0;
                handVelocities[i].velocityFlags = 0;
            }
        }
    }

    void edit(float dt, bool a, bool b) {
        if (b && !previousB) {
            std::ofstream file(folder / L"SubtitleStyle.txt");
            file << style.x << ',' << style.y << ',' << style.opacity << ',' << style.lines << ',' << style.layout << '\n';
            file.close();
            if (!file) throw std::runtime_error("Could not save the subtitle settings.");
            finished = true;
            return;
        }
        if (a && !previousA) { editMode = (editMode + 1) % 4; editX = editY = 0; }
        const bool play = pressed(bookMenu) || pressed(bookY);
        subtitlePreview.input(true, play, true);
        const bool x = pressed(toggleSubtitle);
        if (x && !previousX) showSubtitle = !showSubtitle;
        previousX = x;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = editor;
        XrActionStateVector2f stickState{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xr.xrGetActionStateVector2f(session, &get, &stickState)) || !stickState.isActive) return;
        auto increment = [&](float value, float speed, float& accumulated) {
            if (std::abs(value) < .22f) { accumulated = 0; return 0; }
            accumulated += value * speed * dt;
            int whole = static_cast<int>(accumulated);
            accumulated -= static_cast<float>(whole);
            return whole;
        };
        if (editMode == 0) {
            style.x += increment(stickState.currentState.x, 57, editX);
            style.y += increment(stickState.currentState.y, 57, editY);
        } else if (editMode == 1) style.opacity += increment(stickState.currentState.y, 64, editY);
        else if (editMode == 2) style.lines += increment(stickState.currentState.y, 6, editY);
        else style.layout += increment(stickState.currentState.y, 4, editY);
        style.clamp();
    }

    void update(XrTime time) {
        XrActiveActionSet active{actions, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (xr.xrSyncActions(session, &sync) != XR_SUCCESS) {
            if (testMode) subtitlePreview.input(false, false, false);
            armed = false;
            lastTime = 0;
            stick = 0;
            gesture.reset();
            if (testMode) tracking(time);
            return;
        }
        const bool a = pressed(advance), b = pressed(back);
        const float dt = lastTime ? float(time - lastTime) * 1e-9f : 0.0f;
        lastTime = time;
        const bool timed = dt > 0 && dt <= .12f;
        if (testMode) {
            edit(timed ? dt : 0, a, b);
        } else if (b && !previousB) {
            if (remaining > 0.0f) remaining = 0.0f;
            else if (step > 0) speeds[--step] = 0.0f;
            message.clear();
            gesture.reset();
        } else if (a && !previousA && remaining <= 0.0f) {
            if (step == speeds.size()) { save(); finished = true; }
            else { remaining = 4.5f; sample.reset(); message.clear(); }
        }
        previousA = a;
        previousB = b;
        const std::array<bool, 2> touching{pressed(rest[0]), pressed(rest[1])};
        armed = thumbsArmed(gate, touching);
        tracking(time);
        if (!trackedHead) { stick = 0; gesture.reset(); return; }
        const auto settings = step == speeds.size() ? calibratedCurve(motion, speeds) : motion;
        std::array<std::optional<float>, 2> deltas{};
        stick = gesture.update(settings, gate, reverseEnabled && (testMode || step == speeds.size()),
                               dt, *trackedHead, trackedHands, touching, &deltas, &headVelocity, &handVelocities);
        if (!timed) return;
        if (testMode) {
            if (const auto front = horizontalForward(trackedHead->orientation)) {
                travel.x += front->x * stick * dt * 2.2f;
                travel.z += front->y * stick * dt * 2.2f;
            }
            return;
        }
        distance = std::fmod(distance + stick * dt * 0.8f, 1.0f);
        if (remaining > 0.0f) {
            sample.update(dt, swingSpeed(dt, deltas), motion.smoothing);
            remaining -= dt;
            if (remaining <= 0.0f) {
                auto value = sample.result();
                if (!value) message = L"No steady swing measured. Touch the thumbrests and try again.";
                else if (step > 0 && *value < speeds[step - 1] + 0.05f)
                    message = L"Swing a little faster than the previous pace, or press B to go back.";
                else if (step == 2 && *value < 0.15f) message = L"The full-speed swing needs to be a little faster.";
                else { speeds[step++] = *value; gesture.reset(); }
            }
        }
    }

    void save() const {
        const auto settings = calibratedCurve(motion, speeds);
        std::ofstream file(folder / ResultFile);
        file << std::fixed << std::setprecision(3)
             << "StartSpeed=" << settings.threshold << '\n' << "SlowSpeed=" << speeds[0] << '\n'
             << "MidSpeed=" << speeds[1] << '\n' << "FullSpeed=" << speeds[2] << '\n';
        file.close();
        if (!file) throw std::runtime_error("Could not save the calibration measurements.");
    }

    void draw(Canvas& canvas) {
        std::wstring title = step == 3 ? L"Preview your calibrated pace" : std::wstring(Paces[step]) + L" swing";
        std::wstring instructions = step == 3 ? L"Swing to preview  •  A saves measurements" :
            remaining > 0.0f ? L"Touch the thumbrests and swing at this pace" : L"Press A to record this pace";
        std::wstring detail = message.empty() ? (step == 0 ? L"The lane shows how much forward stick the profile sends." :
            L"Press B to repeat the previous pace.") : message;
        wchar_t measured[140]{};
        swprintf_s(measured, L"Arm speed (m/s)    Slow %.2f    Medium %.2f    Fast %.2f", speeds[0], speeds[1], speeds[2]);
        canvas.draw(title, instructions, detail, measured, remaining > 0.0f ? 1.0f - remaining / 4.5f : (step == 3 ? 1.0f : 0.0f),
            stick, distance, armed);
        upload(canvas);
    }

    void upload(Canvas& canvas) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        check(xr.xrAcquireSwapchainImage(swapchain, &acquire, &index), "Acquiring a headset texture");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        check(xr.xrWaitSwapchainImage(swapchain, &wait), "Waiting for the headset texture");
        context->UpdateSubresource(images.at(index).texture, 0, nullptr, canvas.data(), Width * 4, 0);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        check(xr.xrReleaseSwapchainImage(swapchain, &release), "Releasing the headset texture");
    }
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) return 2;
    const auto folder = std::filesystem::path(argv[1]);
    try {
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)))) throw std::runtime_error("Could not locate calibration files.");
        OpenXR xr(std::filesystem::path(executable).parent_path() / L"openxr_loader.dll");
        const bool scene = argc == 3 && std::wstring_view(argv[2]) == L"--scene";
        CalibrationApp app(xr, folder, scene);
        const bool probe = argc == 3 && std::wstring_view(argv[2]) == L"--probe";
        app.initialize(probe);
        if (!probe) app.run();
        return 0;
    } catch (const std::exception& error) {
        std::ofstream(folder / L"Error.txt") << error.what() << '\n';
        return 1;
    }
}
