#pragma once

#include <algorithm>
#include <chrono>

namespace runtime_timing
{
    struct SamplingDemand
    {
        bool latencySensitive = false;
        bool displaySynchronized = false;
        bool periodic = false;
        int idleRate = 20;
    };

    struct RadarDemand
    {
        bool localOverlay = false;
        bool foreground = true;
    };

    constexpr int dataSamplingRate(
        int displayRefreshRate,
        const SamplingDemand demand)
    {
        if (demand.latencySensitive) {
            return 240;
        }
        if (demand.displaySynchronized) {
            return std::clamp(displayRefreshRate, 60, 240);
        }
        if (demand.periodic) {
            return 60;
        }
        return std::clamp(demand.idleRate, 1, 60);
    }

    constexpr int radarSamplingRate(const RadarDemand demand)
    {
        // Local Radar is a foreground overlay, so it needs no background or
        // service-specific sampling rates.
        return demand.localOverlay && demand.foreground ? 20 : 0;
    }

    constexpr std::chrono::milliseconds metadataRefreshInterval(
        const bool latencySensitive)
    {
        return latencySensitive
            ? std::chrono::milliseconds(100)
            : std::chrono::milliseconds(250);
    }

    constexpr std::chrono::nanoseconds intervalForRate(int rate)
    {
        return std::chrono::nanoseconds(
            1'000'000'000LL / std::max(1, rate));
    }
}
