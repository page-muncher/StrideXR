#include "test_scene.h"
#include "tracking_math.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace {
constexpr char Shader[] = R"(
cbuffer View : register(b0) { row_major float4x4 viewProjection; };
struct Pixel { float4 pos : SV_POSITION; float3 rgb : COLOR; };
Pixel vs(float3 pos : POSITION, float3 rgb : COLOR) {
    Pixel p;
    p.pos = mul(float4(pos, 1), viewProjection);
    p.rgb = rgb;
    return p;
}
float4 ps(Pixel p) : SV_TARGET { return float4(p.rgb, 1); }
)";

void checkScene(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("Could not initialize or draw the VR test scene.");
}
}

void TestScene::initialize(ID3D11Device* device) {
    ComPtr<ID3DBlob> vs, ps;
    checkScene(D3DCompile(Shader, sizeof(Shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, nullptr));
    checkScene(D3DCompile(Shader, sizeof(Shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, nullptr));
    checkScene(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
    checkScene(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
    const D3D11_INPUT_ELEMENT_DESC inputs[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    checkScene(device->CreateInputLayout(inputs, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &layout));
    D3D11_BUFFER_DESC buffer{sizeof(Vertex) * 16384, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
    checkScene(device->CreateBuffer(&buffer, nullptr, &geometry));
    buffer = {sizeof(XMFLOAT4X4), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    checkScene(device->CreateBuffer(&buffer, nullptr, &matrix));
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = 2 * EyeWidth;
    texture.Height = EyeHeight;
    texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthTexture;
    checkScene(device->CreateTexture2D(&texture, nullptr, &depthTexture));
    checkScene(device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depth));
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    checkScene(device->CreateRasterizerState(&rasterDesc, &raster));
    mesh.reserve(16384);
}

void TestScene::box(float x, float y, float z, float sx, float sy, float sz, XMFLOAT3 color) {
    const XMFLOAT3 points[] = {
        {x-sx,y-sy,z-sz}, {x+sx,y-sy,z-sz}, {x-sx,y+sy,z-sz}, {x+sx,y+sy,z-sz},
        {x-sx,y-sy,z+sz}, {x+sx,y-sy,z+sz}, {x-sx,y+sy,z+sz}, {x+sx,y+sy,z+sz}
    };
    constexpr int indices[][6] = {{0,1,2,2,1,3}, {4,6,5,5,6,7}, {0,2,4,4,2,6},
                                  {1,5,3,3,5,7}, {2,3,6,6,3,7}, {0,4,1,1,4,5}};
    for (size_t face = 0; face < std::size(indices); ++face) {
        const float light = face == 4 ? 1.0f : .64f + .05f * static_cast<float>(face);
        for (auto point : indices[face]) mesh.push_back({points[point], {light*color.x,light*color.y,light*color.z}});
    }
}

void TestScene::draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                     const std::array<XrView, 2>& views, const std::array<XrSpaceLocation, 2>& hands,
                     XrVector3f travel, float originY, bool reverse) {
    mesh.clear();
    const float floor = originY - 1.55f;
    const float gridX = std::remainder(travel.x, 1.0f);
    const float gridZ = std::remainder(travel.z, 1.0f);
    box(0, floor - .035f, 0, 20, .03f, 20, {.021f,.029f,.037f});
    for (int i = -20; i <= 20; ++i) {
        box(float(i)-gridX, floor, 0, .009f, .003f, 20, {.07f,.13f,.16f});
        box(0, floor, float(i)-gridZ, 20, .003f, .009f, {.07f,.13f,.16f});
    }
    const float postsX = std::remainder(travel.x, 6.0f);
    const float postsZ = std::remainder(travel.z, 3.0f);
    for (int row = -6; row <= 6; ++row) for (int col = -2; col <= 2; ++col) {
        for (float side : {-1.0f, 1.0f}) {
            const float x = side * 1.25f + float(col) * 6 - postsX;
            const float z = float(row) * 3 - postsZ;
            box(x, floor + .34f, z, .055f, .34f, .055f, {.038f,.17f,.18f});
            box(x, floor + .69f, z, .065f, .035f, .065f, {.16f,.73f,.67f});
        }
    }
    for (size_t i = 0; i < hands.size(); ++i) {
        constexpr auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((hands[i].locationFlags & valid) != valid) continue;
        const auto& pose = hands[i].pose;
        const auto start = mesh.size();
        const auto handColor = reverse ? XMFLOAT3{.93f,.48f,.15f} :
            i == 0 ? XMFLOAT3{.13f,.65f,.92f} : XMFLOAT3{.23f,.83f,.58f};
        box(0, 0, 0, .036f, .040f, .075f, handColor);
        box(0, .051f, -.016f, .032f, .012f, .039f, {.80f,.92f,.98f});
        for (size_t j = start; j < mesh.size(); ++j) {
            auto& p = mesh[j].position;
            auto rotated = rotateVector(pose.orientation, {p.x,p.y,p.z});
            p = {rotated.x + pose.position.x, rotated.y + pose.position.y, rotated.z + pose.position.z};
        }
    }
    const float clear[]{.009f,.014f,.021f,1};
    context->ClearRenderTargetView(target, clear);
    context->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    context->OMSetRenderTargets(1, &target, depth.Get());
    context->RSSetState(raster.Get());
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto* vertices = geometry.Get();
    constexpr UINT stride = sizeof(Vertex), offset = 0;
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    D3D11_MAPPED_SUBRESOURCE data{};
    checkScene(context->Map(vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &data));
    std::memcpy(data.pData, mesh.data(), mesh.size()*sizeof(Vertex));
    context->Unmap(vertices, 0);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    auto* uniform = matrix.Get();
    context->VSSetConstantBuffers(0, 1, &uniform);
    for (size_t eye = 0; eye < views.size(); ++eye) {
        const auto& view = views[eye];
        const auto q = view.pose.orientation;
        const auto p = view.pose.position;
        auto camera = XMMatrixRotationQuaternion(XMVectorSet(q.x,q.y,q.z,q.w)) * XMMatrixTranslation(p.x,p.y,p.z);
        constexpr float nearPlane = .045f;
        auto projection = XMMatrixPerspectiveOffCenterRH(std::tan(view.fov.angleLeft) * nearPlane,
            std::tan(view.fov.angleRight) * nearPlane, std::tan(view.fov.angleDown) * nearPlane,
            std::tan(view.fov.angleUp) * nearPlane, nearPlane, 60.0f);
        XMFLOAT4X4 transform;
        XMStoreFloat4x4(&transform, XMMatrixInverse(nullptr, camera) * projection);
        context->UpdateSubresource(uniform, 0, nullptr, &transform, 0, 0);
        const D3D11_VIEWPORT viewport{float(eye * EyeWidth), 0, float(EyeWidth), float(EyeHeight), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->Draw(static_cast<UINT>(mesh.size()), 0);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
}
