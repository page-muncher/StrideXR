#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <array>
#include <vector>

class TestScene {
public:
    static constexpr int EyeWidth = 1024, EyeHeight = 1024;
    void initialize(ID3D11Device* device);
    void draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
              const std::array<XrView, 2>& views, const std::array<XrSpaceLocation, 2>& hands,
              XrVector3f travel, float originY, bool reverse);

private:
    struct Vertex { DirectX::XMFLOAT3 position, color; };
    void box(float x, float y, float z, float sx, float sy, float sz, DirectX::XMFLOAT3 color);
    std::vector<Vertex> mesh;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> geometry, matrix;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
};
