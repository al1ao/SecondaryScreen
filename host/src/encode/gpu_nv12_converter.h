#pragma once
#include <d3d11.h>
#include "util/com_ptr.h"
namespace pds {
class GpuNv12Converter {
public:
    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height);
    bool Convert(ID3D11Texture2D* input, ComPtr<ID3D11Texture2D>& output);
private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> luma_, chroma_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    uint32_t width_ = 0, height_ = 0;
};
}
