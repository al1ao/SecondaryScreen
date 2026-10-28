#include "encode/gpu_nv12_converter.h"
#include <algorithm>
#include <d3dcompiler.h>
#include "util/gpu.h"
#include "bgra_to_nv12_source.h"
namespace pds {
namespace {
bool CompileShader(const char* entry, const char* profile, ComPtr<ID3DBlob>& code) {
    ComPtr<ID3DBlob> errors;
    const auto hr = D3DCompile(kNv12Shader, sizeof(kNv12Shader) - 1, "bgra_to_nv12.hlsl", nullptr, nullptr,
        entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) && errors) LOG_ERROR(static_cast<const char*>(errors->GetBufferPointer()));
    return LOG_HR(hr, "D3DCompile NV12 shader");
}
}
bool GpuNv12Converter::Initialize(ID3D11Device* device, uint32_t width, uint32_t height) {
    if (!device || width < 2 || height < 2 || width % 2 != 0 || height % 2 != 0) return false;
    device_ = device; width_ = width; height_ = height;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    ComPtr<ID3DBlob> vs, y, uv;
    if (!CompileShader("Fullscreen", "vs_5_0", vs) || !CompileShader("Luma", "ps_5_0", y) ||
        !CompileShader("Chroma", "ps_5_0", uv) ||
        !LOG_HR(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex_.ReleaseAndGetAddressOf()), "NV12 vertex shader") ||
        !LOG_HR(device_->CreatePixelShader(y->GetBufferPointer(), y->GetBufferSize(), nullptr, luma_.ReleaseAndGetAddressOf()), "NV12 luma shader") ||
        !LOG_HR(device_->CreatePixelShader(uv->GetBufferPointer(), uv->GetBufferSize(), nullptr, chroma_.ReleaseAndGetAddressOf()), "NV12 chroma shader")) return false;
    D3D11_SAMPLER_DESC sampling{};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    if (!LOG_HR(device_->CreateSamplerState(&sampling, sampler_.ReleaseAndGetAddressOf()), "NV12 sampler")) return false;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    if (!LOG_HR(device_->CreateRasterizerState(&raster, rasterizer_.ReleaseAndGetAddressOf()), "NV12 rasterizer")) return false;
    // Validate plane RTV support during backend probing, so unsupported GPUs fall back before HELLO_ACK.
    D3D11_TEXTURE2D_DESC input_desc{};
    input_desc.Width = width; input_desc.Height = height; input_desc.MipLevels = 1; input_desc.ArraySize = 1;
    input_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; input_desc.SampleDesc.Count = 1;
    input_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> test, converted;
    return LOG_HR(device_->CreateTexture2D(&input_desc, nullptr, test.GetAddressOf()), "NV12 probe source") && Convert(test.Get(), converted);
}
bool GpuNv12Converter::Convert(ID3D11Texture2D* input, ComPtr<ID3D11Texture2D>& output) {
    if (!input || !device_) return false;
    D3D11_TEXTURE2D_DESC source{};
    input->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || source.Width == 0 || source.Height == 0) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_; desc.Height = height_; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> converted;
    if (!LOG_HR(device_->CreateTexture2D(&desc, nullptr, converted.GetAddressOf()), "NV12 output texture")) return false;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> y, uv;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_R8_UNORM;
    if (!LOG_HR(device_->CreateShaderResourceView(input, nullptr, srv.GetAddressOf()), "BGRA shader resource") ||
        !LOG_HR(device_->CreateRenderTargetView(converted.Get(), &view, y.GetAddressOf()), "NV12 Y plane RTV")) return false;
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (!LOG_HR(device_->CreateRenderTargetView(converted.Get(), &view, uv.GetAddressOf()), "NV12 UV plane RTV")) return false;
    GpuContextLock lock(context_.Get());
    constexpr float black_y[]{16.0f / 255.0f, 0, 0, 0};
    constexpr float neutral_uv[]{128.0f / 255.0f, 128.0f / 255.0f, 0, 0};
    context_->ClearRenderTargetView(y.Get(), black_y);
    context_->ClearRenderTargetView(uv.Get(), neutral_uv);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context_->OMSetDepthStencilState(nullptr, 0);
    ID3D11ShaderResourceView* resources[]{srv.Get()};
    ID3D11SamplerState* samplers[]{sampler_.Get()};
    context_->PSSetShaderResources(0, 1, resources);
    context_->PSSetSamplers(0, 1, samplers);
    const float scale = (std::min)(static_cast<float>(width_) / static_cast<float>(source.Width),
        static_cast<float>(height_) / static_cast<float>(source.Height));
    // Align the letterbox rectangle to chroma samples; both planes describe the same image.
    const auto rw = (std::max)(2u, static_cast<uint32_t>(static_cast<float>(source.Width) * scale) & ~1u);
    const auto rh = (std::max)(2u, static_cast<uint32_t>(static_cast<float>(source.Height) * scale) & ~1u);
    const auto x = ((width_ - rw) / 2) & ~1u, top = ((height_ - rh) / 2) & ~1u;
    D3D11_VIEWPORT viewport{static_cast<float>(x), static_cast<float>(top), static_cast<float>(rw), static_cast<float>(rh), 0, 1};
    ID3D11RenderTargetView* targets[]{y.Get()};
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->PSSetShader(luma_.Get(), nullptr, 0);
    context_->Draw(3, 0);
    targets[0] = uv.Get();
    viewport.TopLeftX /= 2; viewport.TopLeftY /= 2; viewport.Width /= 2; viewport.Height /= 2;
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->PSSetShader(chroma_.Get(), nullptr, 0);
    context_->Draw(3, 0);
    targets[0] = nullptr; resources[0] = nullptr;
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->PSSetShaderResources(0, 1, resources);
    context_->Flush();
    output = std::move(converted);
    return true;
}
}
