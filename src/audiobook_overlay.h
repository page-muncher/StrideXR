#pragma once

#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "subtitle_style.h"

class AudiobookOverlay {
public:
    void initialize(XrInstance instance, PFN_xrGetInstanceProcAddr next, const XrSessionCreateInfo& info);
    void input(bool active, bool down, bool changed);
    XrResult endFrame(XrSession session, const XrFrameEndInfo* frame);
    const XrCompositionLayerQuad* preview(XrSession session, std::wstring_view caption, SubtitleStyle style, bool visible);

private:
    // Matches the fixed-width header in AudiobookLink.cs. Offsets are identical on x86 and x64.
    struct Shared {
        LONG ready, revision, gameTick, flags;
        DWORD presses, playerTick;
        LONG length, fade;
        wchar_t caption[2048];
        SubtitleStyle style;
        LONG labLines, labLayout, reserved2;
    };
    static_assert(sizeof(Shared) == 4160);

    struct Close {
        void operator()(void* handle) const { CloseHandle(handle); }
    };
    struct Unmap {
        void operator()(Shared* address) const { UnmapViewOfFile(address); }
    };
    std::unique_ptr<void, Close> mapping_, mutex_;
    std::unique_ptr<Shared, Unmap> shared_;
    DWORD lastConnect_ = 0;
    LONG revision_ = 0;
    bool haveRevision_ = false;
    bool playerReady_ = false;
    int fade_ = 0;
    int publishedLines_ = 0, publishedLayout_ = -1;
    std::atomic<bool> inputActive_ = false;
    std::atomic<DWORD> pendingPresses_ = 0;
    bool menuDown_ = false;
    bool failed_ = false, acquired_ = false, uploaded_ = false;
    std::wstring caption_;
    std::wstring uploadedCaption_;
    SubtitleStyle style_, uploadedStyle_;
    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    std::vector<uint32_t> pixels_;
    uint32_t imageIndex_ = 0;
    uint32_t maxLayers_ = 0;
    PFN_xrCreateReferenceSpace createSpace_{};
    PFN_xrEnumerateSwapchainFormats formats_{};
    PFN_xrCreateSwapchain createSwapchain_{};
    PFN_xrEnumerateSwapchainImages enumerateImages_{};
    PFN_xrAcquireSwapchainImage acquire_{};
    PFN_xrWaitSwapchainImage wait_{};
    PFN_xrReleaseSwapchainImage release_{};
    PFN_xrEndFrame endFrame_{};

    bool connect(DWORD now, bool lab);
    void readPlayer(DWORD now, const SubtitleStyle* labStyle = nullptr);
    bool create(XrSession session);
    bool draw(unsigned opacity);
    bool upload();
    const XrCompositionLayerQuad* compose(XrSession session);
};
