#include "audiobook_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr wchar_t MappingName[] = L"Local\\StrideXR-Audiobook-v3";
constexpr wchar_t MutexName[] = L"Local\\StrideXR-Audiobook-v3-Lock";
constexpr wchar_t LabMapping[] = L"Local\\StrideXR-Audiobook-v3-Lab";
constexpr wchar_t LabMutex[] = L"Local\\StrideXR-Audiobook-v3-Lab-Lock";
constexpr int Width = 1200;
constexpr int RowHeight = 48;
constexpr int MaxCurrentRows = 6;
constexpr int ContextRows = 2;
constexpr int Padding = 8;
// The first spoken row is centered on the quad's anchor. Additional rows extend below it.
constexpr int Height = (2 * (MaxCurrentRows + ContextRows) - 1) * RowHeight + 2 * Padding;
constexpr int CurrentTop = (Height - RowHeight) / 2;
}

void AudiobookOverlay::initialize(XrInstance instance, PFN_xrGetInstanceProcAddr next,
                                  const XrSessionCreateInfo& info) {
    auto load = [&](auto& proc, const char* name) {
        PFN_xrVoidFunction function{};
        if (XR_SUCCEEDED(next(instance, name, &function))) proc = reinterpret_cast<decltype(+proc)>(function);
    };
    load(endFrame_, "xrEndFrame");
    for (auto chain = reinterpret_cast<const XrBaseInStructure*>(info.next); chain; chain = chain->next) {
        if (chain->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) continue;
        device_ = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(chain)->device;
        break;
    }
    if (!device_) return;
    device_->GetImmediateContext(&context_);
    PFN_xrGetSystemProperties properties{};
    load(properties, "xrGetSystemProperties");
    XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
    if (properties && XR_SUCCEEDED(properties(instance, info.systemId, &system)))
        maxLayers_ = system.graphicsProperties.maxLayerCount;
    load(createSpace_, "xrCreateReferenceSpace");
    load(formats_, "xrEnumerateSwapchainFormats");
    load(createSwapchain_, "xrCreateSwapchain");
    load(enumerateImages_, "xrEnumerateSwapchainImages");
    load(acquire_, "xrAcquireSwapchainImage");
    load(wait_, "xrWaitSwapchainImage");
    load(release_, "xrReleaseSwapchainImage");
    failed_ = !context_ || !maxLayers_ || !createSpace_ || !formats_ || !createSwapchain_ ||
              !enumerateImages_ || !acquire_ || !wait_ || !release_ ||
              system.graphicsProperties.maxSwapchainImageWidth < Width ||
              system.graphicsProperties.maxSwapchainImageHeight < Height;
}

void AudiobookOverlay::input(bool active, bool down, bool changed) {
    if (active && down && !menuDown_ && changed) ++pendingPresses_;
    // Preserve the last pressed state through focus loss. A held button must not toggle twice.
    if (active) menuDown_ = down;
    inputActive_ = active;
}

bool AudiobookOverlay::connect(DWORD now, bool lab) {
    if (shared_) return true;
    if (lastConnect_ && now - lastConnect_ < 500) return false;
    lastConnect_ = now;
    mapping_.reset(OpenFileMappingW(FILE_MAP_WRITE, FALSE, lab ? LabMapping : MappingName));
    if (!mapping_) return false;
    mutex_.reset(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lab ? LabMutex : MutexName));
    if (mutex_) shared_.reset(static_cast<Shared*>(MapViewOfFile(mapping_.get(), FILE_MAP_WRITE, 0, 0, sizeof(Shared))));
    if (shared_) return true;
    mutex_.reset();
    mapping_.reset();
    return false;
}

void AudiobookOverlay::readPlayer(DWORD now, const SubtitleStyle* labStyle) {
    const auto presses = pendingPresses_.exchange(0);
    if (!connect(now, labStyle != nullptr)) return;
    const bool changed = labStyle && (publishedLines_ != labStyle->lines || publishedLayout_ != labStyle->layout);
    const auto locked = WaitForSingleObject(mutex_.get(), 0);
    if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED) {
        pendingPresses_.fetch_add(presses);
        return;
    }
    now = GetTickCount();
    if (now - shared_->playerTick >= 1800) {
        ReleaseMutex(mutex_.get());
        caption_.clear();
        playerReady_ = false;
        fade_ = 0;
        haveRevision_ = false;
        shared_.reset();
        mutex_.reset();
        mapping_.reset();
        return;
    }
    shared_->gameTick = static_cast<LONG>(now);
    shared_->flags = 1 | (inputActive_ ? 2 : 0) | (!device_ ? 4 : 0) | (failed_ ? 8 : 0) | (labStyle ? 16 : 0);
    if (changed) {
        shared_->labLines = publishedLines_ = labStyle->lines;
        shared_->labLayout = publishedLayout_ = labStyle->layout;
    }
    playerReady_ = shared_->ready != 0;
    fade_ = playerReady_ ? static_cast<int>(std::clamp(shared_->fade, 0L, 100L)) : 0;
    if (playerReady_) shared_->presses += presses;
    if (!haveRevision_ || revision_ != shared_->revision) {
        haveRevision_ = true;
        revision_ = shared_->revision;
        const auto length = shared_->ready ? std::clamp(shared_->length, 0L, 2048L) : 0;
        caption_.assign(shared_->caption, static_cast<size_t>(length));
        style_ = shared_->style;
        style_.clamp();
    }
    ReleaseMutex(mutex_.get());
}

bool AudiobookOverlay::create(XrSession session) {
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    reference.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(createSpace_(session, &reference, &space_))) return false;

    uint32_t count = 0;
    if (XR_FAILED(formats_(session, 0, &count, nullptr)) || !count) return false;
    std::vector<int64_t> supported(count);
    if (XR_FAILED(formats_(session, count, &count, supported.data()))) return false;
    constexpr DXGI_FORMAT preferences[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                           DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    auto format = std::ranges::find_if(preferences, [&](auto candidate) {
        return std::ranges::find(supported, candidate) != supported.end();
    });
    if (format == std::end(preferences)) return false;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = *format;
    info.width = Width;
    info.height = Height;
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    if (XR_FAILED(createSwapchain_(session, &info, &swapchain_))) return false;
    if (XR_FAILED(enumerateImages_(swapchain_, 0, &count, nullptr)) || !count) return false;
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    return XR_SUCCEEDED(enumerateImages_(swapchain_, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())));
}

bool AudiobookOverlay::draw(unsigned opacity) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = Width;
    info.bmiHeader.biHeight = -Height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return false;
    auto dc = CreateCompatibleDC(nullptr);
    if (!dc) { DeleteObject(bitmap); return false; }
    auto old = SelectObject(dc, bitmap);
    auto mask = static_cast<uint32_t*>(bits);
    std::fill_n(mask, Width * Height, 0u);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);
    auto font = CreateFontW(-39, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (font) SelectObject(dc, font);
    std::array<std::wstring_view, MaxCurrentRows + 2 * ContextRows> rows{};
    int count = 0, firstCurrent = -1, currentCount = 0;
    size_t start = 0;
    while (start <= caption_.size() && count < static_cast<int>(rows.size())) {
        auto end = caption_.find(L'\n', start);
        if (end == std::wstring::npos) end = caption_.size();
        auto row = std::wstring_view(caption_).substr(start, end - start);
        if (row.empty() || row.front() != L'\x01') {
            if (firstCurrent < 0) firstCurrent = count;
            ++currentCount;
        }
        rows[count++] = row;
        start = end + 1;
    }
    if (firstCurrent < 0) firstCurrent = count;
    const int currentHeight = std::clamp(currentCount, 1, style_.lines) * RowHeight;
    const int boxBottom = CurrentTop + currentHeight + Padding;
    for (int i = 0; i < count; ++i) {
        auto row = rows[i];
        if (row.empty()) continue;
        const bool dim = row.front() == L'\x01';
        if (dim) row.remove_prefix(1);
        int y = CurrentTop + (i - firstCurrent) * RowHeight;
        if (i < firstCurrent) y -= Padding;
        else if (dim) y += Padding;
        else if (y >= CurrentTop + currentHeight) continue;
        RECT area{36, y, Width - 36, y + RowHeight};
        SetTextColor(dc, dim ? RGB(140, 140, 140) : RGB(255, 255, 255));
        DrawTextW(dc, row.data(), int(row.size()), &area,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    GdiFlush();
    pixels_.assign(Width * Height, 0u);
    const int firstRow = std::max(0, CurrentTop - firstCurrent * RowHeight - Padding);
    const int lastRow = std::min(Height, std::max(boxBottom, CurrentTop + (count - firstCurrent) * RowHeight + Padding));
    for (int y = firstRow; y < lastRow; ++y) {
        const bool box = y >= CurrentTop - Padding && y < boxBottom;
        for (int x = 0; x < Width; ++x) {
            const int i = y * Width + x;
            const unsigned text = mask[i] & 0xff;
            if (!box && !text) continue;
            const unsigned color = box ? 18 + (237 * text) / 255 : 230;
            const unsigned alpha = (box ? 195 + 60 * text / 255 : text) * opacity / 100;
            // Grayscale RGBA and BGRA have the same byte order.
            pixels_[i] = (alpha << 24) | (color << 16) | (color << 8) | color;
        }
    }
    SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return true;
}

bool AudiobookOverlay::upload() {
    if (!acquired_) {
        XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(acquire_(swapchain_, &info, &imageIndex_))) { failed_ = true; return false; }
        acquired_ = true;
    }
    XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    info.timeout = 0;
    const auto result = wait_(swapchain_, &info);
    if (result == XR_TIMEOUT_EXPIRED) return false;
    if (XR_FAILED(result)) { failed_ = true; return false; }
    const bool validImage = imageIndex_ < images_.size() && images_[imageIndex_].texture;
    if (validImage) context_->UpdateSubresource(images_[imageIndex_].texture, 0, nullptr, pixels_.data(), Width * 4, 0);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    failed_ = XR_FAILED(release_(swapchain_, &release)) || !validImage;
    acquired_ = false;
    return !failed_;
}

const XrCompositionLayerQuad* AudiobookOverlay::compose(XrSession session) {
    if (caption_.empty() || fade_ == 100 || !device_ || failed_) {
        return nullptr;
    }
    if (!swapchain_ && !create(session)) {
        failed_ = true;
        return nullptr;
    }
    const int opacity = (style_.opacity * (100 - fade_) + 50) / 100;
    if (!uploaded_ || caption_ != uploadedCaption_ ||
        opacity != uploadedStyle_.opacity || style_.lines != uploadedStyle_.lines) {
        if (draw(static_cast<unsigned>(opacity)) && upload()) {
            uploadedCaption_ = caption_;
            uploadedStyle_ = style_;
            uploadedStyle_.opacity = opacity;
            uploaded_ = true;
        }
        if (!uploaded_ || failed_) return nullptr;
    }
    auto& quad = quad_;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    quad.space = space_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain_;
    quad.subImage.imageRect.extent = {Width, Height};
    const auto tilt = std::atan2(style_.y * .01f, 1.65f);
    quad.pose.orientation = {std::sin(tilt / 2), 0, 0, std::cos(tilt / 2)};
    quad.pose.position = {style_.x * .01f, style_.y * .01f, -1.65f};
    quad.size = {0.96f, 0.96f * Height / Width};
    return &quad;
}

const XrCompositionLayerQuad* AudiobookOverlay::preview(XrSession session, std::wstring_view caption, SubtitleStyle style, bool visible) {
    readPlayer(GetTickCount(), &style);
    if (!visible) return nullptr;
    if (!playerReady_) caption_ = caption;
    style_ = style;
    style_.clamp();
    return compose(session);
}

XrResult AudiobookOverlay::endFrame(XrSession session, const XrFrameEndInfo* frame) {
    if (!endFrame_) return XR_ERROR_FUNCTION_UNSUPPORTED;
    readPlayer(GetTickCount());
    if (!frame || !frame->layerCount || !frame->layers || frame->layerCount >= maxLayers_)
        return endFrame_(session, frame);
    const auto* quad = compose(session);
    if (!quad) return endFrame_(session, frame);
    std::vector<const XrCompositionLayerBaseHeader*> layers(frame->layers, frame->layers + frame->layerCount);
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(quad));
    auto augmented = *frame;
    augmented.layerCount = static_cast<uint32_t>(layers.size());
    augmented.layers = layers.data();
    const auto result = endFrame_(session, &augmented);
    if (result == XR_ERROR_LAYER_INVALID || result == XR_ERROR_LAYER_LIMIT_EXCEEDED ||
        result == XR_ERROR_SWAPCHAIN_RECT_INVALID || result == XR_ERROR_POSE_INVALID) failed_ = true;
    return result;
}
