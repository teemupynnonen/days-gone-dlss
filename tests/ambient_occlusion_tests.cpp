#include "shader_bytecode.hpp"
#include "ambient_occlusion.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using namespace days_gone_dlss::upscaler;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT result) { expect(SUCCEEDED(result), "Direct3D operation failed."); }

ComPtr<ID3DBlob> compile(const char* source, const char* profile,
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3)
{
    ComPtr<ID3DBlob> result, error;
    const auto status = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile,
        flags, 0, &result, &error);
    if (FAILED(status) && error) std::cerr << static_cast<const char*>(error->GetBufferPointer());
    check(status);
    return result;
}

void render()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    const auto vertex = compile(R"(
float4 main(uint index : SV_VertexID) : SV_Position {
    return float4(index == 2 ? 3 : -1, index == 1 ? 3 : -1, 0, 1);
})", "vs_5_0");
    ComPtr<ID3D11VertexShader> vertexShader;
    check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertexShader));
    expect(readPixelBytecode(vertex->GetBufferPointer(), vertex->GetBufferSize()).words.empty(), "Leave vertex shaders alone.");

    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 8;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    check(device->CreateTexture2D(&description, nullptr, &target));
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&description, nullptr, &staging));
    ComPtr<ID3D11RenderTargetView> view;
    check(device->CreateRenderTargetView(target.Get(), nullptr, &view));
    D3D11_BUFFER_DESC bufferDescription{};
    bufferDescription.ByteWidth = 16;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    check(device->CreateBuffer(&bufferDescription, nullptr, &constants));
    const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    auto renderCount = [&](ID3D11PixelShader* shader, const void* data)
    {
        context->UpdateSubresource(constants.Get(), 0, nullptr, data, 0, 0);
        constexpr float black[4]{};
        context->ClearRenderTargetView(view.Get(), black);
        context->PSSetShader(shader, nullptr, 0);
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        unsigned count{};
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x)
            {
                const auto red = static_cast<const unsigned char*>(mapped.pData)[y * mapped.RowPitch + x * 4];
                count += red != 0;
            }
        context->Unmap(staging.Get(), 0);
        return count;
    };
    // Authored fixture: reproduce interpolated weights just below their intended
    // integers. A weight of zero can reject the only depth-compatible AO sample.
    const auto ambient = compile(R"(
cbuffer Settings { float4 inputWeights; };
float4 main() : SV_Target {
    float4 weights = frac(inputWeights) * 16.0;
    uint4 packed = uint4(weights);
    return all(packed == uint4(1, 3, 3, 9)) ? 1.0 : float4(0, 0, 0, 1);
})", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3);
    expect(makeFullResolutionAmbientOcclusion(ambient->GetBufferPointer(), ambient->GetBufferSize()).bytecode.empty(),
        "Only known lighting shaders may receive the AO correction.");
    std::vector<std::uint32_t> identified(ambient->GetBufferSize() / 4);
    std::memcpy(identified.data(), ambient->GetBufferPointer(), ambient->GetBufferSize());
    // Substitute a supported identity solely to exercise the production rewrite
    // with this authored fixture, without distributing game shader bytecode.
    constexpr std::array<std::uint32_t, 4> identity{0xd6fc4671, 0x0f7a1932, 0x334f9bde, 0x518ed285};
    std::copy(identity.begin(), identity.end(), identified.begin() + 1);
    expect(readPixelBytecode(nullptr, identified.size() * 4).words.empty(), "Reject null input.");
    expect(readPixelBytecode(identified.data(), identified.size() * 4 - 4).words.empty(), "Reject truncated DXBC.");
    auto malformed = identified;
    malformed[7] = 0xffffffff;
    expect(readPixelBytecode(malformed.data(), malformed.size() * 4).words.empty(), "Reject an invalid chunk table.");
    malformed = identified;
    malformed[8] = static_cast<std::uint32_t>(malformed.size() * 4 - 4);
    expect(readPixelBytecode(malformed.data(), malformed.size() * 4).words.empty(), "Reject an out-of-range chunk.");
    const auto corrected = makeFullResolutionAmbientOcclusion(identified.data(), identified.size() * 4);
    if (corrected.bytecode.empty())
    {
        ComPtr<ID3DBlob> assembly;
        check(D3DDisassemble(ambient->GetBufferPointer(), ambient->GetBufferSize(), 0, nullptr, &assembly));
        std::cerr << static_cast<const char*>(assembly->GetBufferPointer());
    }
    expect(!corrected.bytecode.empty(), "Recognize the AO weight conversion.");
    expect(makeFullResolutionAmbientOcclusion(corrected.bytecode.data(), corrected.bytecode.size() * 4).bytecode.empty(),
        "A corrected lighting shader is not patched twice.");
    ComPtr<ID3D11PixelShader> ambientShader, correctedAmbientShader;
    check(device->CreatePixelShader(ambient->GetBufferPointer(), ambient->GetBufferSize(), nullptr, &ambientShader));
    check(device->CreatePixelShader(corrected.bytecode.data(), corrected.bytecode.size() * 4, nullptr, &correctedAmbientShader));
    constexpr float impreciseWeights[]{0.06249999f, 0.18749999f, 0.18749999f, 0.5624999f};
    constexpr float exactWeights[]{0.0625f, 0.1875f, 0.1875f, 0.5625f};
    expect(renderCount(ambientShader.Get(), impreciseWeights) == 0,
        "Truncation loses nominal integer weights after floating-point error.");
    expect(renderCount(correctedAmbientShader.Get(), impreciseWeights) == 64,
        "Rounding retains the intended AO weights despite floating-point error.");
    expect(renderCount(correctedAmbientShader.Get(), exactWeights) == 64,
        "Rounding preserves exact AO weights.");
}
}

int main()
{
    try { render(); std::cout << "Ambient occlusion rendering checks passed.\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
