#pragma once

#include "NewPipelineProtocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace wicked_newpipeline
{
// Consume at most one latest snapshot per tick, preserving fractional time.
// Long stalls never create a backlog of obsolete snapshots to catch up on.
inline bool ConsumeControlPublishInterval(float& accumulator, float interval)
{
    if (accumulator < interval)
        return false;
    accumulator = std::fmod(accumulator, interval);
    return true;
}

// Owned by the publication worker. Repeated acknowledgements recover a lost
// unreliable status packet, but only a new selection gates video publication.
struct StreamStatusPublication
{
    std::optional<RemoteStreamStatus> announced;
    std::optional<RemoteStreamStatus> attempted;
    uint64_t next_attempt_usec = 0;

    static bool SameSelection(const RemoteStreamStatus& a, const RemoteStreamStatus& b)
    {
        return a.code == b.code && a.selection == b.selection;
    }

    bool ShouldSend(const RemoteStreamStatus& latest, uint64_t now_usec) const
    {
        return !attempted || !SameSelection(*attempted, latest) ||
            now_usec >= next_attempt_usec;
    }

    void RecordAttempt(const RemoteStreamStatus& status, bool sent, uint64_t now_usec)
    {
        attempted = status;
        if (sent)
            announced = status;
        next_attempt_usec = now_usec + (sent ? 200'000u : 100'000u);
    }

    bool CanPublish(const RemoteStreamStatus& latest, const RemoteStreamSelection& frame) const
    {
        return latest.code == RemoteStreamStatusCode::Selected &&
            latest.selection == frame && announced &&
            SameSelection(*announced, latest);
    }
};

// Small rolling window, updated on the owning thread. Sort only for the debug
// panel; no allocation or sorting is performed in the transport hot path.
struct PacingSamples
{
    std::array<uint64_t, 120> samples = {};
    size_t count = 0;
    size_t cursor = 0;
    uint64_t previous_usec = 0;

    void Add(uint64_t usec)
    {
        samples[cursor] = usec;
        cursor = (cursor + 1u) % samples.size();
        count = std::min(count + 1u, samples.size());
    }

    void Mark(uint64_t now_usec)
    {
        if (previous_usec != 0 && now_usec >= previous_usec)
            Add(now_usec - previous_usec);
        previous_usec = now_usec;
    }

    std::string SummaryMilliseconds() const
    {
        if (count == 0)
            return "n/a";
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.begin() + count);
        const auto percentile = [&](size_t percent) {
            return std::to_string(sorted[(count * percent + 99u) / 100u - 1u] / 1000u);
        };
        return percentile(50) + "/" + percentile(95) + "/" + percentile(99);
    }
};
} // namespace wicked_newpipeline
