#include "pass_timer.hpp"

#include <stdexcept>

namespace days_gone_dlss::ray_tracing
{
PassTimer::PassTimer(ID3D11Device* device) : device(device)
{
    for (auto& frame : frames) frame.disjoint = query(D3D11_QUERY_TIMESTAMP_DISJOINT);
}

Microsoft::WRL::ComPtr<ID3D11Query> PassTimer::query(D3D11_QUERY type) const
{
    Microsoft::WRL::ComPtr<ID3D11Query> result;
    const D3D11_QUERY_DESC description{type, 0};
    if (FAILED(device->CreateQuery(&description, result.GetAddressOf()))) throw std::runtime_error("Creating a GPU timer query failed.");
    return result;
}

void PassTimer::begin(ID3D11DeviceContext* context, Pass pass)
{
    auto& frame = frames[current];
    if (!std::exchange(frame.open, true))
    {
        context->Begin(frame.disjoint.Get());
        frame.used = 0;
    }
    if (frame.used == frame.runs.size()) frame.runs.push_back({pass, query(D3D11_QUERY_TIMESTAMP), query(D3D11_QUERY_TIMESTAMP)});

    auto& run = frame.runs[frame.used];
    run.pass = pass;
    context->End(run.start.Get());
    running[static_cast<std::size_t>(pass)] = frame.used++;
}

void PassTimer::end(ID3D11DeviceContext* context, Pass pass)
{
    auto& index = running[static_cast<std::size_t>(pass)];
    if (!index) return;

    context->End(frames[current].runs[*index].end.Get());
    index.reset();
}

std::optional<PassTimer::Milliseconds> PassTimer::read(ID3D11DeviceContext* context, Frame& frame) const
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (context->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return std::nullopt;

    frame.pending = false;
    // The clock changed speed during the frame (power state): its times mean nothing.
    if (disjoint.Disjoint || !disjoint.Frequency) return std::nullopt;

    Milliseconds result{};
    for (std::size_t i = 0; i < frame.used; ++i)
    {
        const auto& run = frame.runs[i];
        UINT64 start{}, end{};
        if (context->GetData(run.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context->GetData(run.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || end < start)
            continue;

        result[static_cast<std::size_t>(run.pass)] += static_cast<float>(static_cast<double>(end - start) * 1000.0 / static_cast<double>(disjoint.Frequency));
    }
    return result;
}

std::optional<PassTimer::Milliseconds> PassTimer::endFrame(ID3D11DeviceContext* context)
{
    auto& frame = frames[current];
    if (std::exchange(frame.open, false))
    {
        // A pass left open (an exception mid-pass) ends with the frame.
        for (auto& index : running)
            if (index) end(context, static_cast<Pass>(&index - running.data()));
        context->End(frame.disjoint.Get());
        frame.pending = true;
        current = (current + 1) % FrameCount;
    }

    // Oldest first, so the newest finished frame is the one returned. The frame about to be recorded
    // into is given up if the GPU has still not finished it.
    std::optional<Milliseconds> newest;
    for (std::size_t i = 0; i < FrameCount; ++i)
    {
        auto& candidate = frames[(current + i) % FrameCount];
        if (!candidate.pending) continue;

        if (auto times = read(context, candidate)) newest = times;
        else if (&candidate == &frames[current]) candidate.pending = false;
    }
    return newest;
}
}
