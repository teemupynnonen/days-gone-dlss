// Drives the ray-tracing capture as the add-on does, on the real GPU: G-buffer draws are captured
// with stream output, traced through D3D12 and must land on exactly the pixels and depths the
// rasteriser produced, with the sky left out and alpha-tested holes left open. Skipped (77) without
// DXR 1.1.
#include "ray_tracing.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace days_gone_dlss::upscaler
{
void log(std::string_view message) { std::cout << message << '\n'; }
}

namespace
{
using Microsoft::WRL::ComPtr;
using namespace days_gone_dlss;

constexpr UINT Size = 64;
constexpr int Skipped = 77;

void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT result) { expect(SUCCEEDED(result), "Direct3D operation failed."); }

bool rayQueriesSupported(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> d3d12;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))
        && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12)))
        && SUCCEEDED(d3d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)))
        && options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
}

ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, DXGI_FORMAT format, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = Size;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = format;
    description.Usage = usage;
    description.BindFlags = bind;
    description.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
    ComPtr<ID3D11Texture2D> result;
    check(device->CreateTexture2D(&description, nullptr, &result));
    return result;
}

int run()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
    if (!rayQueriesSupported(device.Get()))
    {
        std::cout << "Skipped: this GPU does not support DXR 1.1.\n";
        return Skipped;
    }

    // A tilted triangle covering the centre but not the corners, in UE4's view space (x right, y up,
    // z forward), drawn through its reversed-Z infinite projection with a sub-pixel TAA jitter.
    const auto compile = [](const char* source, const char* profile = "vs_5_0")
    {
        ComPtr<ID3DBlob> code, errors;
        const auto compiled = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &errors);
        if (FAILED(compiled) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
        check(compiled);
        return code;
    };
    const auto code = compile(R"(
cbuffer View : register(b0) { row_major float4x4 ClipFromWorld; };
float4 main(uint vertex : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position
{
    const float3 corners[3] = { float3(-300, -250, 400), float3(0, 350, 600), float3(350, -200, 500) };
    uv = corners[vertex].xy;
    return mul(float4(corners[vertex], 1), ClipFromWorld);
})");
    ComPtr<ID3D11VertexShader> vertexShader;
    check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertexShader));
    ray_tracing::vertexShaderCreated(code->GetBufferPointer(), code->GetBufferSize(), vertexShader.Get());
    // The game draws its sky at infinity, reversed-Z depth 0, where no world position exists.
    const auto skyCode = compile(R"(
float4 main(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3 : -1, vertex == 2 ? -3 : 1, 0, 1);
})");
    ComPtr<ID3D11VertexShader> skyShader;
    check(device->CreateVertexShader(skyCode->GetBufferPointer(), skyCode->GetBufferSize(), nullptr, &skyShader));
    ray_tracing::vertexShaderCreated(skyCode->GetBufferPointer(), skyCode->GetBufferSize(), skyShader.Get());
    // Bend also draws a finite sky dome, tested against the far plane without writing depth.
    const auto domeCode = compile(R"(
float4 main(uint vertex : SV_VertexID) : SV_Position
{
    return float4(float2(vertex == 1 ? 3 : -1, vertex == 2 ? -3 : 1) * 5000, 10, 5000);
})");
    ComPtr<ID3D11VertexShader> domeShader;
    check(device->CreateVertexShader(domeCode->GetBufferPointer(), domeCode->GetBufferSize(), nullptr, &domeShader));
    ray_tracing::vertexShaderCreated(domeCode->GetBufferPointer(), domeCode->GetBufferSize(), domeShader.Get());
    constexpr float jitter[2]{0.37f * 2 / Size, -0.21f * 2 / Size};
    constexpr float clipFromWorld[16]{1.5f, 0, 0, 0, 0, 1.5f, 0, 0, jitter[0], jitter[1], 0, 1, 0, 0, 10, 0};
    constexpr std::int32_t rect[4]{0, 0, Size, Size};
    D3D11_BUFFER_DESC viewDescription{};
    viewDescription.ByteWidth = sizeof(clipFromWorld);
    viewDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA viewData{clipFromWorld};
    ComPtr<ID3D11Buffer> view;
    check(device->CreateBuffer(&viewDescription, &viewData, &view));

    // A G-buffer of two targets, as Bend binds for static geometry (moving objects add a third, for
    // velocity), and a reversed-Z depth buffer the comparison reads.
    ComPtr<ID3D11RenderTargetView> gbuffer[2];
    for (auto& target : gbuffer)
        check(device->CreateRenderTargetView(texture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET).Get(), nullptr, &target));
    const auto depth = texture(device.Get(), DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC depthView{DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D};
    ComPtr<ID3D11DepthStencilView> depthTarget;
    check(device->CreateDepthStencilView(depth.Get(), &depthView, &depthTarget));
    D3D11_DEPTH_STENCIL_DESC depthTest{TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_GREATER};
    ComPtr<ID3D11DepthStencilState> depthState, skyState;
    check(device->CreateDepthStencilState(&depthTest, &depthState));
    depthTest = {TRUE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_GREATER_EQUAL};
    check(device->CreateDepthStencilState(&depthTest, &skyState));
    D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterState;
    check(device->CreateRasterizerState(&raster, &rasterState));
    const auto backBuffer = texture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    const auto staging = texture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING);

    // One frame as the game renders it: the view command, the scene's draws, then present. Each draw
    // is reported to the capture first, as ReShade's draw event does.
    ID3D11RenderTargetView* targets[]{gbuffer[0].Get(), gbuffer[1].Get()};
    const auto draw = [&](ID3D11VertexShader* shader, ID3D11DepthStencilState* state, UINT vertices)
    {
        context->VSSetShader(shader, nullptr, 0);
        context->OMSetDepthStencilState(state, 0);
        ray_tracing::draw(context.Get(), vertices, 1, 0, 0, 0, false);
        context->Draw(vertices, 0);
    };
    const auto frame = [&](auto&& scene)
    {
        constexpr float clear[4]{};
        ray_tracing::beginView(clipFromWorld, rect, rect, {}, true);
        context->ClearDepthStencilView(depthTarget.Get(), D3D11_CLEAR_DEPTH, 0, 0);
        const D3D11_VIEWPORT viewport{0, 0, Size, Size, 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterState.Get());
        context->OMSetRenderTargets(2, targets, depthTarget.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetConstantBuffers(0, 1, view.GetAddressOf());
        context->PSSetShader(nullptr, nullptr, 0);
        scene();
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ComPtr<ID3D11RenderTargetView> presented;
        check(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &presented));
        context->ClearRenderTargetView(presented.Get(), clear);
        ray_tracing::present(context.Get(), backBuffer.Get());
        context->CopyResource(staging.Get(), backBuffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<std::uint32_t> pixels(Size * Size);
        for (UINT y = 0; y < Size; ++y)
            std::memcpy(&pixels[y * Size], static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, Size * 4);
        context->Unmap(staging.Get(), 0);
        const auto state = ray_tracing::status();
        if (!state.failure.empty()) throw std::runtime_error(state.failure);
        return pixels;
    };
    const auto channel = [](std::uint32_t pixel, int index) { return static_cast<int>(pixel >> (index * 8) & 0xff); };
    // Green where the traced depth matches the rasterised one, dark where both see the sky.
    const auto compare = [&](const std::vector<std::uint32_t>& pixels, const char* scene)
    {
        unsigned agreeing{}, sky{}, disagreeing{};
        for (const auto pixel : pixels)
        {
            const int red = channel(pixel, 0), green = channel(pixel, 1), blue = channel(pixel, 2);
            if (green > red && green > blue) ++agreeing;
            else if (red < 32 && green < 32 && blue < 32) ++sky;
            else ++disagreeing;
        }
        std::cout << scene << " depth comparison: " << agreeing << " agree, " << sky << " sky, " << disagreeing << " disagree.\n";
        return std::array<unsigned, 3>{agreeing, sky, disagreeing};
    };
    const auto skyAndTriangle = [&]
    {
        draw(skyShader.Get(), depthState.Get(), 3);
        draw(domeShader.Get(), skyState.Get(), 3);
        draw(vertexShader.Get(), depthState.Get(), 3);
    };

    ray_tracing::select(ray_tracing::View::Geometry);
    frame(skyAndTriangle); // Starts the D3D11 and D3D12 resources; capture begins with the next frame.
    auto pixels = frame(skyAndTriangle);
    const auto state = ray_tracing::status();
    expect(state.basePassDraws == 3 && state.capturedDraws == 2 && state.triangles == 2, "The geometry is captured and the sky dome left out.");
    expect(pixels[0] == 0xff000000, "A ray past the triangle misses the sky at infinity.");
    const auto centre = pixels[Size / 2 * Size + Size / 2];
    expect(channel(centre, 0) + channel(centre, 1) + channel(centre, 2) > 0, "A ray through the centre hits the triangle.");

    ray_tracing::select(ray_tracing::View::DepthComparison);
    const auto [agreeing, sky, disagreeing] = compare(frame(skyAndTriangle), "Triangle");
    expect(agreeing > Size * Size / 8 && sky > Size * Size / 8, "The triangle and the sky are both in view.");
    expect(disagreeing == 0, "Every traced pixel matches the rasterised coverage and depth.");

    // A quad cut out by a checkerboard in front of an opaque background. As in Bend's renderer, its
    // depth prepass alpha-tests, scaling the opacity by a per-vertex fade, and its G-buffer draw
    // matches that depth without discarding. The first triangle is faded out. Like Bend's foliage,
    // both draws narrow the viewport's depth range and scale clip z to match. Like a chain-link
    // fence, the material tiles its texture: the mesh's coordinates span 1/32 and the alpha test
    // scales them by 32 (and offsets them), which would fill every texel of a bake over 0 to 1.
    const auto backgroundCode = compile(R"(
float4 main(uint vertex : SV_VertexID) : SV_Position
{
    return float4(float2(vertex == 1 ? 3 : -1, vertex == 2 ? -3 : 1) * 800, 10, 800);
})");
    const auto quadCode = compile(R"(
cbuffer View : register(b0) { row_major float4x4 ClipFromWorld; };
void main(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0, out float fade : COLOR0)
{
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
    float2 corner = corners[vertex];
    uv = corner / 32;
    fade = vertex < 3 ? 0.25 : 0.9;
    position = mul(float4(lerp(-250, 250, corner.x), lerp(200, -200, corner.y), 450, 1), ClipFromWorld);
    position.z /= 0.08;
})");
    const auto alphaCode = compile(R"(
cbuffer Material : register(b0) { float4 Tiling; };
Texture2D<float4> Opacity : register(t0);
SamplerState Point : register(s0);
void main(float4 position : SV_Position, float2 uv : TEXCOORD0, float fade : COLOR0)
{
    clip(Opacity.Sample(Point, uv * Tiling.xy + Tiling.zw).a * fade - 0.3333);
})", "ps_5_0");
    D3D11_BUFFER_DESC tilingDescription{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    const float tilingValues[4]{32, 32, 0.25f, 0.25f};
    const D3D11_SUBRESOURCE_DATA tilingData{tilingValues};
    ComPtr<ID3D11Buffer> tiling;
    check(device->CreateBuffer(&tilingDescription, &tilingData, &tiling));
    ComPtr<ID3D11VertexShader> backgroundShader, quadShader;
    ComPtr<ID3D11PixelShader> alphaShader;
    check(device->CreateVertexShader(backgroundCode->GetBufferPointer(), backgroundCode->GetBufferSize(), nullptr, &backgroundShader));
    check(device->CreateVertexShader(quadCode->GetBufferPointer(), quadCode->GetBufferSize(), nullptr, &quadShader));
    check(device->CreatePixelShader(alphaCode->GetBufferPointer(), alphaCode->GetBufferSize(), nullptr, &alphaShader));
    ray_tracing::vertexShaderCreated(backgroundCode->GetBufferPointer(), backgroundCode->GetBufferSize(), backgroundShader.Get());
    ray_tracing::vertexShaderCreated(quadCode->GetBufferPointer(), quadCode->GetBufferSize(), quadShader.Get());
    ray_tracing::pixelShaderCreated(alphaCode->GetBufferPointer(), alphaCode->GetBufferSize(), alphaShader.Get());
    std::vector<std::uint32_t> checker(64 * 64);
    for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x)
            checker[y * 64 + x] = (x / 4 + y / 4) % 2 ? 0xffffffff : 0x00ffffff;
    D3D11_TEXTURE2D_DESC checkerDescription{64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA checkerData{checker.data(), 64 * 4};
    ComPtr<ID3D11Texture2D> checkerTexture;
    ComPtr<ID3D11ShaderResourceView> checkerView;
    check(device->CreateTexture2D(&checkerDescription, &checkerData, &checkerTexture));
    check(device->CreateShaderResourceView(checkerTexture.Get(), nullptr, &checkerView));
    D3D11_SAMPLER_DESC point{D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP};
    point.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> pointSampler;
    check(device->CreateSamplerState(&point, &pointSampler));
    depthTest = {TRUE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_EQUAL};
    ComPtr<ID3D11DepthStencilState> equalState;
    check(device->CreateDepthStencilState(&depthTest, &equalState));
    const D3D11_VIEWPORT viewport{0, 0, Size, Size, 0, 1}, narrowed{0, 0, Size, Size, 0, 0.08f};
    const auto cutOut = [&]
    {
        draw(backgroundShader.Get(), depthState.Get(), 3);
        context->RSSetViewports(1, &narrowed);
        context->OMSetRenderTargets(0, nullptr, depthTarget.Get());
        context->PSSetShader(alphaShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, checkerView.GetAddressOf());
        context->PSSetSamplers(0, 1, pointSampler.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, tiling.GetAddressOf());
        draw(quadShader.Get(), depthState.Get(), 6);
        context->OMSetRenderTargets(2, targets, depthTarget.Get());
        context->PSSetShader(nullptr, nullptr, 0);
        draw(quadShader.Get(), equalState.Get(), 6);
        context->RSSetViewports(1, &viewport);
    };
    const auto [cutAgreeing, cutSky, cutDisagreeing] = compare(frame(cutOut), "Cut-out quad");
    const auto cutState = ray_tracing::status();
    expect(cutState.maskedDraws == 1 && cutState.bakedMaterials == 1 && cutState.capturedDraws == 2,
        "The alpha-tested quad is captured once, from its depth prepass, with its material baked.");
    expect(cutAgreeing > Size * Size * 9 / 10 && !cutSky, "The quad and the background fill the view.");
    expect(cutDisagreeing <= Size * Size / 100, "Rays pass through the quad's holes where the rasteriser does.");

    // Static geometry the view drew stays in the sun rays' scene after the view stops drawing it: the
    // shadow caster view, which traces casters and all cached geometry, still hits the triangle in
    // front of a background drawn alone.
    ray_tracing::select(ray_tracing::View::ShadowCasters);
    const auto nearer = [&](const std::vector<std::uint32_t>& pixels)
    {
        return std::count_if(pixels.begin(), pixels.end(), [&](std::uint32_t pixel)
        {
            return channel(pixel, 0) > channel(pixel, 1) && channel(pixel, 0) > channel(pixel, 2);
        });
    };
    const auto backgroundAlone = [&] { draw(backgroundShader.Get(), depthState.Get(), 3); };
    const auto cachedTriangle = nearer(frame(backgroundAlone));
    std::cout << "Cached triangle: " << cachedTriangle << " pixels nearer than the background.\n";
    expect(cachedTriangle > Size * Size / 8 && ray_tracing::status().tracedCachedDraws > 0,
        "The triangle the view drew in earlier frames is still traced from the geometry cache.");

    // Without the off-screen geometry the caches do no work: they are emptied, and the triangle the
    // view draws is not cached. Back on, it is cached again, for the check below.
    ray_tracing::Features onScreenOnly{};
    onScreenOnly.offscreenGeometry = false;
    ray_tracing::enable(onScreenOnly);
    const auto triangleAlone = [&] { draw(vertexShader.Get(), depthState.Get(), 3); };
    frame(triangleAlone);
    const auto onScreenState = ray_tracing::status();
    expect(!onScreenState.cachedDraws && !onScreenState.newCacheEntries, "Without the off-screen geometry nothing is cached.");
    ray_tracing::enable({});
    frame(triangleAlone);
    expect(ray_tracing::status().newCacheEntries > 0, "With the off-screen geometry back on, the triangle is cached again.");

    // An object that starts moving leaves the cache, or its copies would shadow where it was: once
    // drawn with a velocity target, the triangle is gone from the sun rays' scene when not drawn.
    const auto velocity = texture(device.Get(), DXGI_FORMAT_R16G16_UNORM, D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11RenderTargetView> velocityTarget;
    check(device->CreateRenderTargetView(velocity.Get(), nullptr, &velocityTarget));
    ID3D11RenderTargetView* movingTargets[]{gbuffer[0].Get(), gbuffer[1].Get(), velocityTarget.Get()};
    frame([&]
    {
        draw(backgroundShader.Get(), depthState.Get(), 3);
        context->OMSetRenderTargets(3, movingTargets, depthTarget.Get());
        draw(vertexShader.Get(), depthState.Get(), 3);
        context->OMSetRenderTargets(2, targets, depthTarget.Get());
    });
    expect(!nearer(frame(backgroundAlone)) && !ray_tracing::status().tracedCachedDraws,
        "The triangle drawn with velocity is no longer traced from the geometry cache.");

    // Bend places some static meshes, such as fallen branches, by per-draw constants: the same draw
    // with the same static constants at two places, which the object key cannot tell apart. The cache
    // holds one of them, so it must not stand in for the view's draws: both stay where the view drew
    // them.
    const auto copyCode = compile(R"(
cbuffer View : register(b0) { row_major float4x4 ClipFromWorld; };
cbuffer Placement : register(b1) { float4 Offset; };
cbuffer Mesh : register(b2) { float4 Extent; };
float4 main(uint vertex : SV_VertexID) : SV_Position
{
    float2 corner = float2(vertex == 1, vertex == 2) * Extent.xy;
    return mul(float4(Offset.x + corner.x, corner.y - 100, 500, 1), ClipFromWorld);
})");
    ComPtr<ID3D11VertexShader> copyShader;
    check(device->CreateVertexShader(copyCode->GetBufferPointer(), copyCode->GetBufferSize(), nullptr, &copyShader));
    ray_tracing::vertexShaderCreated(copyCode->GetBufferPointer(), copyCode->GetBufferSize(), copyShader.Get());
    const float extent[4]{200, 200};
    D3D11_BUFFER_DESC constantDescription{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    const D3D11_SUBRESOURCE_DATA extentData{extent};
    ComPtr<ID3D11Buffer> mesh, placement;
    check(device->CreateBuffer(&constantDescription, &extentData, &mesh));
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&constantDescription, nullptr, &placement));
    const auto copyAt = [&](float x)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(placement.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        const float offset[4]{x};
        std::memcpy(mapped.pData, offset, sizeof(offset));
        context->Unmap(placement.Get(), 0);
        ID3D11Buffer* constants[]{placement.Get(), mesh.Get()};
        context->VSSetConstantBuffers(1, 2, constants);
        draw(copyShader.Get(), depthState.Get(), 3);
    };
    const auto twoCopies = [&]
    {
        copyAt(-300);
        copyAt(100);
    };
    ray_tracing::select(ray_tracing::View::DepthComparison);
    frame(twoCopies);
    const auto copies = compare(frame(twoCopies), "Two copies of a mesh");
    expect(copies[0] > Size * Size / 16 && copies[2] == 0, "Both copies of a mesh placed by per-draw constants are traced where the view drew them.");

    // Ambient occlusion: a floor 100 cm below the eye meets a wall 150 cm to its left. The G-buffer
    // carries Bend's octahedral normal, and stand-ins for Bend's occlusion passes carry their DXBC
    // checksums: the mask pass reads the normal, the game's occlusion is fully visible with marker
    // bytes where Bend keeps its depth, and the blur copies. A lighting draw then reads the target.
    const auto aoSceneCode = compile(R"(
cbuffer View : register(b0) { row_major float4x4 ClipFromWorld; };
void main(uint vertex : SV_VertexID, out float4 position : SV_Position, out nointerpolation uint normal : NORMAL0)
{
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
    float2 corner = corners[vertex % 6];
    float3 world = vertex < 6 ? float3(lerp(-150, 400, corner.x), -100, lerp(100, 900, corner.y))
                              : float3(-150, lerp(-100, 300, corner.x), lerp(100, 900, corner.y));
    normal = vertex < 6 ? 0xffe00 : 0x803ff; // +y and +x
    position = mul(float4(world, 1), ClipFromWorld);
})");
    const auto aoGBufferCode = compile(R"(
void main(float4 position : SV_Position, nointerpolation uint normal : NORMAL0, out float4 color : SV_Target0, out uint packed : SV_Target1)
{
    color = 1;
    packed = normal;
})", "ps_5_0");
    const auto lightingCode = compile(R"(
Texture2D<uint2> Ambient : register(t13);
uint2 main(float4 position : SV_Position) : SV_Target
{
    return Ambient.Load(int3(position.xy, 0));
})", "ps_5_0");
    ComPtr<ID3D11VertexShader> aoSceneShader;
    ComPtr<ID3D11PixelShader> aoGBufferShader, lightingShader;
    check(device->CreateVertexShader(aoSceneCode->GetBufferPointer(), aoSceneCode->GetBufferSize(), nullptr, &aoSceneShader));
    check(device->CreatePixelShader(aoGBufferCode->GetBufferPointer(), aoGBufferCode->GetBufferSize(), nullptr, &aoGBufferShader));
    check(device->CreatePixelShader(lightingCode->GetBufferPointer(), lightingCode->GetBufferSize(), nullptr, &lightingShader));
    ray_tracing::vertexShaderCreated(aoSceneCode->GetBufferPointer(), aoSceneCode->GetBufferSize(), aoSceneShader.Get());
    const auto standIn = [&](const char* source, const std::array<std::uint8_t, 16>& checksum)
    {
        const auto code = compile(source, "cs_5_0");
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        std::vector<std::uint8_t> claimed(static_cast<const std::uint8_t*>(code->GetBufferPointer()),
            static_cast<const std::uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
        std::copy(checksum.begin(), checksum.end(), claimed.begin() + 4);
        ray_tracing::computeShaderCreated(claimed.data(), claimed.size(), shader.Get());
        return shader;
    };
    const auto maskShader = standIn(R"(
Texture2D<uint> Normals : register(t0);
RWTexture2D<uint> Mask : register(u0);
[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) { Mask[pixel] = Normals[pixel] & 255; })",
        upscaler::dxbcHash("421145d93a7c3dc04afeb4ef62946097"));
    const auto blurShader = standIn(R"(
Texture2D<uint2> Raw : register(t0);
RWTexture2D<uint2> Blurred : register(u0);
[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) { Blurred[pixel] = Raw[pixel]; })",
        upscaler::dxbcHash("73260a675a394e71f15c5d62bc699101"));
    const auto gameAmbientCode = compile(R"(
RWTexture2D<uint2> Ambient : register(u0);
[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) { Ambient[pixel] = uint2(0x5affffff, 0xa5ffffff); })", "cs_5_0");
    ComPtr<ID3D11ComputeShader> gameAmbientShader;
    check(device->CreateComputeShader(gameAmbientCode->GetBufferPointer(), gameAmbientCode->GetBufferSize(), nullptr, &gameAmbientShader));

    constexpr UINT bindAll = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    const auto normals = texture(device.Get(), DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    const auto mask = texture(device.Get(), DXGI_FORMAT_R32_UINT, bindAll), raw = texture(device.Get(), DXGI_FORMAT_R32G32_UINT, bindAll);
    const auto ambient = texture(device.Get(), DXGI_FORMAT_R32G32_UINT, bindAll);
    const auto lit = texture(device.Get(), DXGI_FORMAT_R32G32_UINT, D3D11_BIND_RENDER_TARGET);
    const auto litStaging = texture(device.Get(), DXGI_FORMAT_R32G32_UINT, 0, D3D11_USAGE_STAGING);
    ComPtr<ID3D11RenderTargetView> normalsTarget, litTarget;
    ComPtr<ID3D11ShaderResourceView> normalsView, rawView, ambientView;
    ComPtr<ID3D11UnorderedAccessView> maskAccess, rawAccess, ambientAccess;
    check(device->CreateRenderTargetView(normals.Get(), nullptr, &normalsTarget));
    check(device->CreateRenderTargetView(lit.Get(), nullptr, &litTarget));
    check(device->CreateShaderResourceView(normals.Get(), nullptr, &normalsView));
    check(device->CreateShaderResourceView(raw.Get(), nullptr, &rawView));
    check(device->CreateShaderResourceView(ambient.Get(), nullptr, &ambientView));
    check(device->CreateUnorderedAccessView(mask.Get(), nullptr, &maskAccess));
    check(device->CreateUnorderedAccessView(raw.Get(), nullptr, &rawAccess));
    check(device->CreateUnorderedAccessView(ambient.Get(), nullptr, &ambientAccess));
    const auto pass = [&](ID3D11ComputeShader* shader, ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output)
    {
        context->CSSetShader(shader, nullptr, 0);
        context->CSSetShaderResources(0, 1, &input);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        ray_tracing::dispatch(context.Get());
        context->Dispatch(Size / 8, Size / 8, 1);
        ID3D11ShaderResourceView* noInput{};
        ID3D11UnorderedAccessView* noOutput{};
        context->CSSetShaderResources(0, 1, &noInput);
        context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    };
    const auto creaseScene = [&]
    {
        ID3D11RenderTargetView* aoTargets[]{gbuffer[0].Get(), normalsTarget.Get()};
        context->OMSetRenderTargets(2, aoTargets, depthTarget.Get());
        context->PSSetShader(aoGBufferShader.Get(), nullptr, 0);
        draw(aoSceneShader.Get(), depthState.Get(), 12);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        pass(maskShader.Get(), normalsView.Get(), maskAccess.Get());
        pass(gameAmbientShader.Get(), nullptr, rawAccess.Get());
        pass(blurShader.Get(), rawView.Get(), ambientAccess.Get());
        context->OMSetRenderTargets(1, litTarget.GetAddressOf(), nullptr);
        context->PSSetShader(lightingShader.Get(), nullptr, 0);
        context->PSSetShaderResources(13, 1, ambientView.GetAddressOf());
        draw(skyShader.Get(), nullptr, 3);
        ID3D11ShaderResourceView* noInput{};
        context->PSSetShaderResources(13, 1, &noInput);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    };
    // The floor's mean visibility as Bend's lighting reads it (the corners on the +y side, slots 1, 3,
    // 4 and 6) within 15 cm of the crease and from 250 cm away, found by casting each pixel's ray.
    const auto floorVisibility = [&]
    {
        context->CopyResource(litStaging.Get(), lit.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(litStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        double crease{}, open{};
        unsigned creasePixels{}, openPixels{};
        bool markersKept = true;
        for (UINT y = 0; y < Size; ++y)
            for (UINT x = 0; x < Size; ++x)
            {
                const auto* value = static_cast<const std::uint32_t*>(mapped.pData) + y * mapped.RowPitch / 4 + x * 2;
                markersKept = markersKept && value[0] >> 24 == 0x5a && value[1] >> 24 == 0xa5;
                const float slopeX = ((x + 0.5f) / Size * 2 - 1 - jitter[0]) / 1.5f, slopeY = (1 - (y + 0.5f) / Size * 2 - jitter[1]) / 1.5f;
                const float distance = slopeY < 0 ? -100 / slopeY : -1, fromWall = distance * slopeX + 150;
                if (distance < 100 || distance > 900 || fromWall < 0 || fromWall > 550) continue;
                const auto slot = [&](int index) { return (value[index / 4] >> (index % 4 * 6) & 63) / 63.0; };
                const double visibility = (slot(1) + slot(3) + slot(4) + slot(6)) / 4;
                if (fromWall < 15)
                {
                    crease += visibility;
                    ++creasePixels;
                }
                else if (fromWall > 250)
                {
                    open += visibility;
                    ++openPixels;
                }
            }
        context->Unmap(litStaging.Get(), 0);
        expect(markersKept, "The traced occlusion keeps the bytes where Bend stores its depth.");
        expect(creasePixels > 10 && openPixels > 10, "The crease and the open floor are both in view.");
        std::cout << "Ambient occlusion: visibility " << crease / creasePixels << " at the crease, " << open / openPixels << " on open floor.\n";
        return std::pair{crease / creasePixels, open / openPixels};
    };

    ray_tracing::select(ray_tracing::View::Off);
    ray_tracing::enable({false, true});
    frame(creaseScene);
    const auto [crease, open] = floorVisibility();
    expect(ray_tracing::status().ambientInjected, "The traced occlusion replaces the game's.");
    expect(open > 0.95, "Open floor stays unoccluded.");
    // At the crease the wall fills half the floor's sky.
    expect(crease < 0.65, "The floor is occluded where it meets the wall.");

    // With half the rays, each pixel takes two of its corners from its neighbour.
    ray_tracing::Features halfRays{};
    halfRays.ambientOcclusion = halfRays.halfAmbientRays = true;
    ray_tracing::enable(halfRays);
    frame(creaseScene);
    const auto [halfCrease, halfOpen] = floorVisibility();
    expect(halfOpen > 0.95 && halfCrease < 0.65, "With half the rays, open floor stays unoccluded and the crease occluded.");
    ray_tracing::enable({});
    frame(creaseScene); // Releases the resources; the game's own occlusion is left alone.
    const auto [gameCrease, gameOpen] = floorVisibility();
    expect(gameCrease == 1 && gameOpen == 1, "Without the feature the lighting reads the game's occlusion.");

    ray_tracing::select(ray_tracing::View::Off);
    frame(skyAndTriangle); // Releases the resources.
    ray_tracing::release();
    return 0;
}
}

int main()
{
    try
    {
        const auto result = run();
        if (!result) std::cout << "Ray-tracing capture checks passed.\n";
        return result;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
