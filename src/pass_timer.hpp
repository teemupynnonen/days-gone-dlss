#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace days_gone_dlss::ray_tracing
{
// The ray tracer's own work in Direct3D 11's command stream.
enum class Pass : std::size_t
{
    Prepare, Wait, SunFilter, SunInject, AmbientFilter, BounceFilter, BounceAdd, LightsWait, LightsFilter, LightsAdd, DebugView, Count
};

// Times passes on the GPU: a timestamp before and after each run, read back a few frames later
// without waiting. Runs of the same pass in a frame add up.
class PassTimer
{
public:
    using Milliseconds = std::array<float, static_cast<std::size_t>(Pass::Count)>;

    explicit PassTimer(ID3D11Device* device);

    void begin(ID3D11DeviceContext* context, Pass pass);
    void end(ID3D11DeviceContext* context, Pass pass);
    // Closes this frame's runs. Returns the newest frame the GPU has finished since the last call,
    // if any: per pass, 0 where it did not run.
    std::optional<Milliseconds> endFrame(ID3D11DeviceContext* context);

private:
    struct Run { Pass pass; Microsoft::WRL::ComPtr<ID3D11Query> start, end; };
    struct Frame
    {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        std::vector<Run> runs; // Pooled: the first `used` belong to this frame
        std::size_t used{};
        bool open{}, pending{};
    };

    // Frames the GPU may still be working on, and the one being recorded.
    static constexpr std::size_t FrameCount = 5;

    Microsoft::WRL::ComPtr<ID3D11Query> query(D3D11_QUERY type) const;
    std::optional<Milliseconds> read(ID3D11DeviceContext* context, Frame& frame) const;

    ID3D11Device* device;

    std::array<Frame, FrameCount> frames;
    std::size_t current{};
    std::array<std::optional<std::size_t>, static_cast<std::size_t>(Pass::Count)> running{}; // Each pass's open run
};
}
