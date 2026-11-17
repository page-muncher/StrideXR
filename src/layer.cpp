#include <windows.h>
#include <openxr/openxr_loader_negotiation.h>

#include "audiobook_overlay.h"
#include "gesture.h"
#include "settings_io.h"
#include "tracking_math.h"

#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

#ifdef STRIDEXR_X86
constexpr char LayerName[] = "XR_APILAYER_STRIDEXR_locomotion_x86";
#else
constexpr char LayerName[] = "XR_APILAYER_STRIDEXR_locomotion";
#endif
constexpr wchar_t DefaultGames[] = L"Dishonored2.exe";
constexpr int Left = 0;
constexpr int Right = 1;

bool matchesGame(const std::wstring& games, const std::wstring& exe) {
    size_t start = 0;
    while (start < games.size()) {
        const size_t end = games.find(L';', start);
        auto game = games.substr(start, end - start);
        auto first = game.find_first_not_of(L" \t");
        if (first != std::wstring::npos) {
            game = game.substr(first, game.find_last_not_of(L" \t") - first + 1);
            if (game == L"*" || _wcsicmp(game.c_str(), exe.c_str()) == 0) return true;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring selectProfile(const std::filesystem::path& ini, const std::wstring& process,
                           const std::wstring& exe, std::wstring& capture) {
    const auto ids = iniText(ini, L"Profiles", L"Ids");
    std::wstring basenameMatch;
    size_t start = 0;
    while (start < ids.size()) {
        const auto end = ids.find(L';', start);
        const auto id = ids.substr(start, end - start);
        const auto section = L"Profile:" + id;
        const auto count = std::min(256u, GetPrivateProfileIntW(section.c_str(), L"TargetCount", 0, ini.c_str()));
        for (unsigned index = 0; index < count; ++index) {
            const auto key = L"Target" + std::to_wstring(index);
            const auto target = iniText(ini, section, key.c_str());
            if (_wcsicmp(target.c_str(), process.c_str()) == 0) return id;
            if (basenameMatch.empty() && _wcsicmp(target.c_str(), exe.c_str()) == 0) basenameMatch = id;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (!basenameMatch.empty()) return basenameMatch;

    const auto pending = iniText(ini, L"Profiles", L"Capture");
    if (!pending.empty() && !iniText(ini, L"Profile:" + pending, L"Name").empty()) {
        if (auto event = OpenEventW(SYNCHRONIZE, FALSE, (L"Local\\StrideXR-" + pending).c_str())) {
            CloseHandle(event);
            capture = pending;
            return pending;
        }
    }
    if (GetPrivateProfileIntW(L"Profiles", L"Global", 0, ini.c_str())) {
        return iniText(ini, L"Profiles", L"Default");
    }
    return {};
}

struct Settings {
    bool enabled = false;
    bool audiobook = false;
    bool reverseWaggle = true;
    ThumbGate gate = ThumbGate::Both;
    MotionSettings motion;
    std::filesystem::path statusFile;
    std::filesystem::path launchFile;
    std::filesystem::path debugFile;
    std::wstring debugEvent;
    std::wstring executable;
    std::wstring capture;
    std::wstring profileName;

    Settings() {
        std::wstring local(32768, L'\0');
        std::wstring process(32768, L'\0');
        auto localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), DWORD(local.size()));
        auto processLength = GetModuleFileNameW(nullptr, process.data(), DWORD(process.size()));
        if (!localLength || localLength >= local.size() || !processLength || processLength >= process.size()) return;
        local.resize(localLength);
        process.resize(processLength);
        executable = process;
        auto exe = std::filesystem::path(process).filename();
        audiobook = _wcsicmp(exe.c_str(), L"FalloutNV.exe") == 0;
        auto directory = std::filesystem::path(local) / L"StrideXR";
        statusFile = directory / (exe.native() + L".status");
        auto launches = directory / L"OpenXR";
        std::error_code error;
        std::filesystem::create_directories(launches, error);
        launchFile = launches / (std::to_wstring(GetCurrentProcessId()) + L".xr");
        const auto debugId = iniText(directory / L"DebugCapture.ini", L"Capture", L"Id");
        if (debugId.size() == 32 && debugId.find_first_not_of(L"0123456789abcdef") == std::wstring::npos) {
            debugEvent = L"Local\\StrideXR-Debug-" + debugId;
            debugFile = directory / L"Diagnostics" / debugId / (L"Layer-" + std::to_wstring(GetCurrentProcessId()) + L".log");
        }

        auto ini = directory / L"Profiles.ini";
        std::wstring section;
        if (!iniText(ini, L"Profiles", L"Ids").empty()) {
            const auto id = selectProfile(ini, process, exe.native(), capture);
            if (id.empty()) return;
            section = L"Profile:" + id;
            profileName = iniText(ini, section, L"Name");
            if (profileName.empty()) return;
        } else {
            ini = directory / L"StrideXR.ini";
            section = L"StrideXR";
            if (!GetPrivateProfileIntW(section.c_str(), L"Enabled", 1, ini.c_str()) ||
                !matchesGame(iniText(ini, section, L"Games", DefaultGames), exe.native())) return;
        }
        enabled = true;
        gate = readThumbGate(ini, section);
        reverseWaggle = GetPrivateProfileIntW(section.c_str(), L"ReverseWaggle", 1, ini.c_str()) != 0;
        motion = readMotionSettings(ini, section);
    }
};

struct Instance {
    XrInstance handle;
    PFN_xrGetInstanceProcAddr next;
    Settings settings;
    XrActionSet set = XR_NULL_HANDLE;
    std::array<XrAction, 2> touch{};
    std::array<XrAction, 2> pose{};
    std::array<XrPath, 4> profiles{};
    std::array<bool, 4> augmented{};
    std::array<XrActionSuggestedBinding, 4> bindings{};
    XrActionSuggestedBinding menu{};
    std::string runtime;
    std::string application;
    PFN_xrWaitFrame wait{};
    PFN_xrSyncActions sync{};
    PFN_xrGetActionStateBoolean getBoolean{};
    PFN_xrGetActionStateVector2f getVector{};
    PFN_xrGetActionStateFloat getFloat{};
    PFN_xrLocateSpace locate{};
    PFN_xrEnumerateBoundSourcesForAction enumerate{};
    PFN_xrPathToString pathToString{};

    template<class F>
    F proc(const char* name) const {
        PFN_xrVoidFunction function{};
        if (XR_FAILED(next(handle, name, &function))) return nullptr;
        return reinterpret_cast<F>(function);
    }

    XrPath path(const char* name) const {
        XrPath result = XR_NULL_PATH;
        proc<PFN_xrStringToPath>("xrStringToPath")(handle, name, &result);
        return result;
    }

    void trace(std::string_view message) const {
        if (settings.debugFile.empty()) return;
        auto event = OpenEventW(SYNCHRONIZE, FALSE, settings.debugEvent.c_str());
        if (!event) return;
        CloseHandle(event);
        SYSTEMTIME time{};
        GetSystemTime(&time);
        char timestamp[30]{};
        sprintf_s(timestamp, "%02u:%02u:%02u.%03u UTC", unsigned(time.wHour), unsigned(time.wMinute),
                  unsigned(time.wSecond), unsigned(time.wMilliseconds));
        std::ofstream file(settings.debugFile, std::ios::app);
        file << timestamp << ' ' << message << '\n';
    }

    void status(const char* phase, const std::array<bool, 2>& available = {},
                const std::array<bool, 2>& touching = {}, const std::array<bool, 2>& tracked = {},
                bool movement = false, float output = 0.0f) const {
        if (!settings.enabled) return;
        std::ostringstream snapshot;
        snapshot << "Runtime: " << runtime << '\n' << "State: " << phase << '\n';
        if (!settings.profileName.empty()) snapshot << "Profile: " << utf8(settings.profileName) << '\n';
        for (int hand = Left; hand <= Right; ++hand) {
            snapshot << (hand == Left ? "Left thumbrest: " : "Right thumbrest: ")
                 << (!available[hand] ? "unavailable" : touching[hand] ? "touched" : "released")
                 << "   |   Pose: " << (tracked[hand] ? "tracked" : "idle or unavailable") << '\n';
        }
        snapshot << "Left-stick action: " << (movement ? "detected" : "waiting") << '\n'
             << "Swing output: " << static_cast<int>(output * 100) << "%\n";
        const auto text = snapshot.str();
        std::ofstream file(settings.statusFile, std::ios::trunc);
        file << text;
        trace(text);
    }

    void recordLaunch(const char* stage) const {
        if (settings.launchFile.empty()) return;
        std::ofstream file(settings.launchFile, std::ios::trunc | std::ios::binary);
        file << utf8(settings.executable) << '\n' << stage << '\n' << utf8(settings.capture) << '\n'
             << runtime << '\n' << application << '\n';
    }

    void initialize(const XrInstanceCreateInfo& instanceInfo) {
        application.assign(instanceInfo.applicationInfo.applicationName,
                           strnlen_s(instanceInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE));
        std::replace(application.begin(), application.end(), '\n', ' ');
        std::replace(application.begin(), application.end(), '\r', ' ');
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (auto getProperties = proc<PFN_xrGetInstanceProperties>("xrGetInstanceProperties");
            getProperties && XR_SUCCEEDED(getProperties(handle, &properties))) runtime = properties.runtimeName;
        recordLaunch("OpenXR detected");
        if (!settings.debugFile.empty()) {
            trace(std::string(sizeof(void*) == 4 ? "x86" : "x64") + " layer entered " + utf8(settings.executable) +
                  " | application=" + application + " | runtime=" + runtime +
                  " | profile=" + (settings.enabled ? utf8(settings.profileName) : "unassigned") +
                  " | x64 disable switch present=" + (GetEnvironmentVariableW(L"DISABLE_STRIDEXR", nullptr, 0) ? "yes" : "no"));
        }
        if (!settings.enabled && !settings.audiobook) return;

        sync = proc<PFN_xrSyncActions>("xrSyncActions");
        getBoolean = proc<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
        if (settings.enabled) {
            wait = proc<PFN_xrWaitFrame>("xrWaitFrame");
            getVector = proc<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
            getFloat = proc<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
            locate = proc<PFN_xrLocateSpace>("xrLocateSpace");
            enumerate = proc<PFN_xrEnumerateBoundSourcesForAction>("xrEnumerateBoundSourcesForAction");
            pathToString = proc<PFN_xrPathToString>("xrPathToString");
            if (!wait || !getVector || !getFloat || !locate || !enumerate || !pathToString) {
                status("The OpenXR runtime is missing input or tracking functions");
                settings.enabled = false;
            }
        }
        if (!sync || !getBoolean || (!settings.enabled && !settings.audiobook)) {
            status("The OpenXR runtime is missing input or tracking functions");
            return;
        }

        XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(info.actionSetName, "stridexr_locomotion");
        strcpy_s(info.localizedActionSetName, "StrideXR locomotion");
        if (XR_FAILED(proc<PFN_xrCreateActionSet>("xrCreateActionSet")(handle, &info, &set))) {
            status("Could not create StrideXR actions");
            return;
        }

        const auto create = proc<PFN_xrCreateAction>("xrCreateAction");
        auto make = [&](const char* name, const char* label, XrActionType type, XrAction& action) {
            XrActionCreateInfo createInfo{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(createInfo.actionName, name);
            strcpy_s(createInfo.localizedActionName, label);
            createInfo.actionType = type;
            return XR_SUCCEEDED(create(set, &createInfo, &action));
        };
        if (settings.enabled &&
            (!make("rest_l", "StrideXR left thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT, touch[Left]) ||
             !make("rest_r", "StrideXR right thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT, touch[Right]) ||
             !make("grip_l", "StrideXR left grip pose", XR_ACTION_TYPE_POSE_INPUT, pose[Left]) ||
             !make("grip_r", "StrideXR right grip pose", XR_ACTION_TYPE_POSE_INPUT, pose[Right]))) {
            status("Could not create StrideXR actions");
            settings.enabled = false;
        }
        if (settings.audiobook && make("book_play_pause", "Audiobook play/pause", XR_ACTION_TYPE_BOOLEAN_INPUT, menu.action))
            menu.binding = path("/user/hand/left/input/menu/click");
        if (!settings.enabled && !menu.binding) {
            proc<PFN_xrDestroyActionSet>("xrDestroyActionSet")(set);
            set = XR_NULL_HANDLE;
            status("Could not create StrideXR actions");
            return;
        }

        profiles = {path("/interaction_profiles/oculus/touch_controller"),
                    path("/interaction_profiles/meta/touch_plus_controller"),
                    path("/interaction_profiles/facebook/touch_controller_pro"),
                    path("/interaction_profiles/meta/touch_controller_plus")};
        bindings = {{{touch[Left], path("/user/hand/left/input/thumbrest/touch")},
                     {touch[Right], path("/user/hand/right/input/thumbrest/touch")},
                     {pose[Left], path("/user/hand/left/input/grip/pose")},
                     {pose[Right], path("/user/hand/right/input/grip/pose")}}};
        status("Waiting for the game's Touch controller bindings");
    }
};

struct ActionKey {
    XrAction action;
    XrPath subaction;
    bool operator==(const ActionKey&) const = default;
};

struct ActionHash {
    size_t operator()(const ActionKey& key) const {
        return std::hash<XrAction>{}(key.action) ^ (std::hash<XrPath>{}(key.subaction) << 1);
    }
};

struct ActionHistory {
    uint64_t sample = 0;
    XrVector2f value{};
    XrTime lastChange = 0;
    bool synthetic = false;
    bool changed = false;
    bool report = false;
};

struct Session {
    std::shared_ptr<Instance> instance;
    std::mutex mutex;
    bool attached = false;
    bool stereo = false;
    AudiobookOverlay audiobook;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace head = XR_NULL_HANDLE;
    std::array<XrSpace, 2> hands{};
    XrTime frameTime = 0;
    XrDuration framePeriod = 0;
    ULONGLONG lastFrame = 0;
    XrTime previousTime = 0;
    XrTime stateTime = 0;
    ArmGesture gesture;
    float output = 0.0f;
    std::array<bool, 2> available{};
    std::array<bool, 2> touching{};
    std::array<ULONGLONG, 2> lastConfirmedTouch{};
    std::array<bool, 2> tracked{};
    bool movementFound = false;
    float deliveredSinceStatus = 0.0f;
    ULONGLONG lastStatus = 0;
    uint64_t sample = 0;
    std::unordered_map<ActionKey, ActionHistory, ActionHash> history;

    template<class F>
    F proc(const char* name) const { return instance->proc<F>(name); }

    void resetMotion() {
        previousTime = 0;
        gesture.reset();
        output = 0.0f;
    }

    void createSpaces(XrSession handle) {
        const auto createReference = proc<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
        const auto createAction = proc<PFN_xrCreateActionSpace>("xrCreateActionSpace");
        XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        reference.poseInReferenceSpace.orientation.w = 1.0f;
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (XR_FAILED(createReference(handle, &reference, &local))) local = XR_NULL_HANDLE;
        reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (XR_FAILED(createReference(handle, &reference, &head))) head = XR_NULL_HANDLE;
        for (int hand = Left; hand <= Right; ++hand) {
            XrActionSpaceCreateInfo action{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            action.action = instance->pose[hand];
            action.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(createAction(handle, &action, &hands[hand]))) hands[hand] = XR_NULL_HANDLE;
        }
    }

    void updateMotion(XrSession handle);
    bool isLeftStick(XrSession handle, const XrActionStateGetInfo& info, bool scalar) const;
    const ActionHistory* adjust(XrSession handle, const XrActionStateGetInfo& info,
                                XrVector2f native, XrTime nativeChange, bool active, bool scalar);
};

void Session::updateMotion(XrSession handle) {
    const auto getBoolean = instance->getBoolean;
    const auto locate = instance->locate;
    const auto now = GetTickCount64();
    for (int hand = Left; hand <= Right; ++hand) {
        XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO};
        query.action = instance->touch[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        available[hand] = XR_SUCCEEDED(getBoolean(handle, &query, &state)) && state.isActive;
        touching[hand] = available[hand] && state.currentState;
        if (available[hand]) lastConfirmedTouch[hand] = touching[hand] ? now : 0;
    }
    tracked = {};
    if (!local || !head || frameTime <= 0 || GetTickCount64() - lastFrame > 140) {
        resetMotion();
        return;
    }

    XrSpaceVelocity headVelocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation view{XR_TYPE_SPACE_LOCATION, &headVelocity};
    constexpr auto positionTracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if (XR_FAILED(locate(head, local, frameTime, &view)) ||
        (view.locationFlags & positionTracked) != positionTracked ||
        !(view.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        resetMotion();
        return;
    }
    std::array<XrSpaceLocation, 2> grips{};
    std::array<XrSpaceVelocity, 2> velocities{};
    auto armedTouch = touching;
    for (int hand = Left; hand <= Right; ++hand) {
        velocities[hand].type = XR_TYPE_SPACE_VELOCITY;
        grips[hand] = {XR_TYPE_SPACE_LOCATION, &velocities[hand]};
        if (!hands[hand] || XR_FAILED(locate(hands[hand], local, frameTime, &grips[hand]))) {
            grips[hand].locationFlags = 0;
            velocities[hand].velocityFlags = 0;
        }
        tracked[hand] = (grips[hand].locationFlags & positionTracked) == positionTracked;
        // If the runtime also stops reporting touch during occlusion, bridge only the missing input.
        // An explicit release always clears the gate on the same sync.
        if (!available[hand] && !tracked[hand] && lastConfirmedTouch[hand]) {
            const auto& motion = instance->settings.motion;
            const auto grace = static_cast<ULONGLONG>(1000 * (motion.stopDelay + motion.stopFade)) + 20;
            armedTouch[hand] = now - lastConfirmedTouch[hand] <= grace;
        }
    }
    if (frameTime == previousTime) {
        if (gesture.reversePose() ? !tracked[Left] || !tracked[Right] :
            !thumbsArmed(instance->settings.gate, armedTouch)) resetMotion();
        return;
    }
    const float dt = previousTime ? static_cast<float>(frameTime - previousTime) * 1e-9f : 0.0f;
    output = gesture.update(instance->settings.motion, instance->settings.gate, instance->settings.reverseWaggle,
                            dt, view.pose, grips, armedTouch, nullptr, &headVelocity, &velocities);
    previousTime = frameTime;
}

bool Session::isLeftStick(XrSession handle, const XrActionStateGetInfo& info, bool scalar) const {
    auto enumerate = instance->enumerate;
    auto pathToString = instance->pathToString;
    XrBoundSourcesForActionEnumerateInfo query{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
    query.action = info.action;
    std::array<XrPath, 16> sources{};
    uint32_t count = 0;
    if (XR_FAILED(enumerate(handle, &query, uint32_t(sources.size()), &count, sources.data())) ||
        count > sources.size()) return false;

    char subaction[XR_MAX_PATH_LENGTH]{};
    uint32_t length = 0;
    if (info.subactionPath && XR_FAILED(pathToString(instance->handle, info.subactionPath,
                                                     sizeof(subaction), &length, subaction))) return false;
    if (info.subactionPath && std::string_view(subaction) != "/user/hand/left") return false;

    bool found = false;
    for (uint32_t index = 0; index < count; ++index) {
        char path[XR_MAX_PATH_LENGTH]{};
        if (XR_FAILED(pathToString(instance->handle, sources[index], sizeof(path), &length, path))) return false;
        std::string_view source(path);
        if (info.subactionPath && !source.starts_with("/user/hand/left/")) continue;
        if (source != (scalar ? "/user/hand/left/input/thumbstick/y" : "/user/hand/left/input/thumbstick")) {
            return false;
        }
        found = true;
    }
    return found;
}

const ActionHistory* Session::adjust(XrSession handle, const XrActionStateGetInfo& info, XrVector2f native,
                                     XrTime nativeChange, bool active, bool scalar) {
    const ActionKey key{info.action, info.subactionPath};
    if (!active) {
        history.erase(key);
        return nullptr;
    }
    auto prior = history.find(key);
    if (prior != history.end() && prior->second.sample == sample) {
        return prior->second.report ? &prior->second : nullptr;
    }
    if (!isLeftStick(handle, info, scalar)) {
        history.erase(key);
        return nullptr;
    }
    movementFound = true;
    // A deliberately deflected physical stick takes priority over arm swing.
    const bool synthetic = output != 0.0f && std::hypot(native.x, native.y) < 0.18f;
    if (synthetic && std::abs(output) > std::abs(deliveredSinceStatus)) deliveredSinceStatus = output;
    const XrVector2f value = synthetic ? XrVector2f{0.0f, output} : native;
    const ActionHistory old = prior != history.end() ? prior->second :
                             ActionHistory{0, native, nativeChange, false, false, false};
    const bool changed = value.x != old.value.x || value.y != old.value.y;
    const bool report = synthetic || old.synthetic || (old.report && nativeChange < old.lastChange);
    const XrTime changedAt = changed ? std::max(old.lastChange, std::max(stateTime, nativeChange)) : old.lastChange;
    auto& result = history[key];
    result = {sample, value, report ? changedAt : nativeChange, synthetic, changed, report};
    return report ? &result : nullptr;
}

std::mutex stateMutex;
std::unordered_map<XrInstance, std::shared_ptr<Instance>> instances;
std::unordered_map<XrSession, std::shared_ptr<Session>> sessions;

template<class Map, class Handle>
auto find(const Map& map, Handle handle) -> typename Map::mapped_type {
    std::scoped_lock lock(stateMutex);
    const auto entry = map.find(handle);
    return entry != map.end() ? entry->second : nullptr;
}

XrResult XRAPI_CALL destroyInstance(XrInstance handle) {
    auto instance = find(instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto result = instance->proc<PFN_xrDestroyInstance>("xrDestroyInstance")(handle);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(stateMutex);
        std::erase_if(sessions, [&](const auto& pair) { return pair.second->instance == instance; });
        instances.erase(handle);
    }
    return result;
}

XrResult XRAPI_CALL createSession(XrInstance handle, const XrSessionCreateInfo* info, XrSession* sessionHandle) {
    auto instance = find(instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    auto session = std::make_shared<Session>();
    const auto result = instance->proc<PFN_xrCreateSession>("xrCreateSession")(handle, info, sessionHandle);
    if (XR_SUCCEEDED(result)) {
        session->instance = instance;
        if (instance->settings.audiobook && info) session->audiobook.initialize(handle, instance->next, *info);
        std::scoped_lock lock(stateMutex);
        sessions[*sessionHandle] = std::move(session);
    }
    if (!instance->settings.debugFile.empty())
        instance->trace("xrCreateSession returned " + std::to_string(result));
    return result;
}

XrResult XRAPI_CALL destroySession(XrSession handle) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto result = session->proc<PFN_xrDestroySession>("xrDestroySession")(handle);
    if (XR_SUCCEEDED(result)) {
        session->instance->status("OpenXR session closed");
        std::scoped_lock lock(stateMutex);
        sessions.erase(handle);
    }
    return result;
}

XrResult XRAPI_CALL beginSession(XrSession handle, const XrSessionBeginInfo* info) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto result = session->proc<PFN_xrBeginSession>("xrBeginSession")(handle, info);
    if (!session->instance->settings.debugFile.empty())
        session->instance->trace("xrBeginSession returned " + std::to_string(result));
    if (result == XR_SUCCESS && info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO) {
        session->stereo = true;
        session->instance->recordLaunch("OpenXR VR session");
    }
    return result;
}

XrResult XRAPI_CALL suggest(XrInstance handle, const XrInteractionProfileSuggestedBinding* info) {
    auto instance = find(instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto next = instance->proc<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
    if (info && info->type == XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING && info->interactionProfile &&
        info->suggestedBindings && info->countSuggestedBindings) {
        auto profile = std::find(instance->profiles.begin(), instance->profiles.end(), info->interactionProfile);
        if (profile != instance->profiles.end()) {
            std::vector<XrActionSuggestedBinding> bindings(info->suggestedBindings,
                                                           info->suggestedBindings + info->countSuggestedBindings);
            if (instance->settings.enabled)
                bindings.insert(bindings.end(), instance->bindings.begin(), instance->bindings.end());
            if (instance->menu.binding) bindings.push_back(instance->menu);
            auto copy = *info;
            copy.suggestedBindings = bindings.data();
            copy.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
            auto result = next(handle, &copy);
            if (XR_FAILED(result) && instance->menu.binding && instance->settings.enabled) {
                --copy.countSuggestedBindings;
                result = next(handle, &copy);
            }
            if (!instance->settings.debugFile.empty())
                instance->trace("xrSuggestInteractionProfileBindings with StrideXR returned " + std::to_string(result));
            const auto index = static_cast<size_t>(profile - instance->profiles.begin());
            if (XR_SUCCEEDED(result)) {
                instance->augmented[index] = true;
                instance->status("Touch bindings accepted. Waiting for the game session");
                return result;
            }
            const auto original = next(handle, info);
            if (XR_SUCCEEDED(original)) {
                instance->augmented[index] = false;
                instance->status("Runtime accepted game bindings only");
            }
            return original;
        }
    }
    return next(handle, info);
}

XrResult XRAPI_CALL attach(XrSession handle, const XrSessionActionSetsAttachInfo* info) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto next = session->proc<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
    const auto& augmented = session->instance->augmented;
    if (info && info->type == XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO &&
        info->actionSets && info->countActionSets && std::ranges::any_of(augmented, [](bool v) { return v; })) {
        std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
        sets.push_back(session->instance->set);
        auto copy = *info;
        copy.actionSets = sets.data();
        copy.countActionSets = static_cast<uint32_t>(sets.size());
        const auto result = next(handle, &copy);
        if (!session->instance->settings.debugFile.empty())
            session->instance->trace("xrAttachSessionActionSets with StrideXR returned " + std::to_string(result));
        if (XR_SUCCEEDED(result)) {
            std::scoped_lock lock(session->mutex);
            if (session->instance->settings.enabled) session->createSpaces(handle);
            session->attached = true;
            session->instance->status("Attached. Waiting for input focus");
            return result;
        }
    }
    session->instance->status("No compatible Touch bindings could be attached");
    return next(handle, info);
}

XrResult XRAPI_CALL waitFrame(XrSession handle, const XrFrameWaitInfo* info, XrFrameState* state) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    // The downstream wait may block while the input thread is still consuming the previous frame.
    const auto result = session->instance->wait(handle, info, state);
    if (XR_SUCCEEDED(result) && state) {
        std::scoped_lock lock(session->mutex);
        session->frameTime = state->predictedDisplayTime;
        session->framePeriod = state->predictedDisplayPeriod;
        session->lastFrame = GetTickCount64();
    }
    return result;
}

XrResult XRAPI_CALL sync(XrSession handle, const XrActionsSyncInfo* info) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    const auto& instance = *session->instance;
    const auto next = instance.sync;
    std::scoped_lock lock(session->mutex);
    if (!session->attached || !info || info->type != XR_TYPE_ACTIONS_SYNC_INFO ||
        (info->countActiveActionSets && !info->activeActionSets) ||
        (!info->countActiveActionSets && !instance.menu.action)) {
        session->audiobook.input(false, false, false);
        session->resetMotion();
        ++session->sample;
        return next(handle, info);
    }

    std::vector<XrActiveActionSet> sets;
    if (info->countActiveActionSets)
        sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    sets.push_back({instance.set, XR_NULL_PATH});
    auto copy = *info;
    copy.activeActionSets = sets.data();
    copy.countActiveActionSets = static_cast<uint32_t>(sets.size());
    auto result = next(handle, &copy);
    ++session->sample;
    session->stateTime = std::max(session->stateTime, session->frameTime - std::max<XrDuration>(0, session->framePeriod));
    if (instance.menu.action && session->stereo) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = instance.menu.action;
        XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool active = result == XR_SUCCESS && instance.getBoolean(handle, &get, &button) == XR_SUCCESS && button.isActive;
        session->audiobook.input(active, button.currentState, button.changedSinceLastSync);
    }
    if (result == XR_SUCCESS) {
        if (instance.settings.enabled && info->countActiveActionSets) session->updateMotion(handle);
        else session->resetMotion();
    } else {
        session->resetMotion();
        session->lastConfirmedTouch = {};
        session->available = session->touching = session->tracked = {};
        if (XR_FAILED(result)) result = next(handle, info);
    }

    const ULONGLONG now = GetTickCount64();
    if (now - session->lastStatus >= 250) {
        session->lastStatus = now;
        session->instance->status(result == XR_SUCCESS ? "Reading controller input" : "Waiting for input focus",
                                  session->available, session->touching, session->tracked,
                                  session->movementFound, session->output);
        if (!session->instance->settings.debugFile.empty())
            session->instance->trace("Peak signed stick value returned to game since last report: " +
                                     std::to_string(session->deliveredSinceStatus));
        session->deliveredSinceStatus = 0.0f;
    }
    return result;
}

XrResult XRAPI_CALL endFrame(XrSession handle, const XrFrameEndInfo* info) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (session->stereo) return session->audiobook.endFrame(handle, info);
    return session->proc<PFN_xrEndFrame>("xrEndFrame")(handle, info);
}

XrResult XRAPI_CALL getVector(XrSession handle, const XrActionStateGetInfo* info, XrActionStateVector2f* state) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    std::scoped_lock lock(session->mutex);
    const auto result = session->instance->getVector(handle, info, state);
    if (result == XR_SUCCESS && session->attached && info && state) {
        if (auto change = session->adjust(handle, *info, state->currentState, state->lastChangeTime, state->isActive, false)) {
            state->currentState = change->value;
            state->changedSinceLastSync = change->changed;
            state->lastChangeTime = change->lastChange;
        }
    }
    return result;
}

XrResult XRAPI_CALL getFloat(XrSession handle, const XrActionStateGetInfo* info, XrActionStateFloat* state) {
    auto session = find(sessions, handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    std::scoped_lock lock(session->mutex);
    const auto result = session->instance->getFloat(handle, info, state);
    if (result == XR_SUCCESS && session->attached && info && state) {
        if (auto change = session->adjust(handle, *info, {0.0f, state->currentState},
                                          state->lastChangeTime, state->isActive, true)) {
            state->currentState = change->value.y;
            state->changedSinceLastSync = change->changed;
            state->lastChangeTime = change->lastChange;
        }
    }
    return result;
}

XrResult XRAPI_CALL getProc(XrInstance handle, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    auto instance = find(instances, handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    const auto result = instance->next(handle, name, function);
    if (XR_FAILED(result)) return result;

#define HOOK(api, fn) if (std::string_view(name) == #api) *function = reinterpret_cast<PFN_xrVoidFunction>(fn)
    HOOK(xrDestroyInstance, destroyInstance);
    HOOK(xrCreateSession, createSession);
    HOOK(xrDestroySession, destroySession);
    HOOK(xrBeginSession, beginSession);
    if (instance->settings.audiobook) {
        HOOK(xrEndFrame, endFrame);
    }
    if (instance->set) {
        HOOK(xrSuggestInteractionProfileBindings, suggest);
        HOOK(xrAttachSessionActionSets, attach);
        HOOK(xrSyncActions, sync);
    }
    if (instance->set && instance->settings.enabled) {
        HOOK(xrWaitFrame, waitFrame);
        HOOK(xrGetActionStateVector2f, getVector);
        HOOK(xrGetActionStateFloat, getFloat);
    }
#undef HOOK
    return result;
}

XrResult XRAPI_CALL createInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                  XrInstance* handle) {
    if (!info || !layerInfo || !layerInfo->nextInfo || !handle) return XR_ERROR_INITIALIZATION_FAILED;
    auto instance = std::make_shared<Instance>();
    auto downstream = *layerInfo;
    downstream.nextInfo = layerInfo->nextInfo->next;
    const auto result = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &downstream, handle);
    if (XR_FAILED(result)) return result;
    instance->handle = *handle;
    instance->next = layerInfo->nextInfo->nextGetInstanceProcAddr;
    instance->initialize(*info);
    std::scoped_lock lock(stateMutex);
    instances[*handle] = std::move(instance);
    return result;
}

} // namespace

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char* name, XrNegotiateApiLayerRequest* request) {
    if (!loader || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize < sizeof(*loader) ||
        !request || request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || request->structSize < sizeof(*request) ||
        !name || std::string_view(name) != LayerName ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->minApiVersion > XR_CURRENT_API_VERSION || XR_VERSION_MAJOR(loader->maxApiVersion) < 1) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = std::min<XrVersion>(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = getProc;
    request->createApiLayerInstance = createInstance;
    return XR_SUCCESS;
}
