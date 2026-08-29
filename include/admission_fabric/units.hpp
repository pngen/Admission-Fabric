#pragma once

// Admission Fabric - dimensional units.
//
// Resource magnitudes are carried in explicit integer units rather than raw
// numbers so that byte counts, token counts, sequence counts and durations
// cannot be silently interchanged. All quantities are bounded to avoid
// arithmetic overflow (see checked.hpp); these are the canonical unit types.

#include <chrono>
#include <cstdint>
#include <string>
#include <sstream>

namespace admission_fabric {

// Byte count: 1 unit = 1 byte.
using ByteCount = std::uint64_t;
// Token count: 1 unit = 1 token (bounded by token budget policy).
using TokenCount = std::uint64_t;
// Sequence count: number of concurrent sequences in a batch.
using SequenceCount = std::uint32_t;
// Slot counts.
using SlotCount = std::uint32_t;
// A grid/tensor dimension.
using DimIndex = std::uint64_t;
// Transfer bandwidth, bytes per second.
using BandwidthBytesPerSecond = std::uint64_t;

// Seconds as a double, used for derived rates (decode tokens/sec).
using TokensPerSecond = double;

// Durations use std::chrono for an unambiguous unit.
using Nanoseconds = std::chrono::nanoseconds;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using TimePoint = SystemClock::time_point;
using SteadyTimePoint = SteadyClock::time_point;

// Human / machine formatting helpers.
inline std::string format_bytes(ByteCount bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    std::ostringstream o;
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    o.setf(std::ios::fixed);
    o.precision(i == 0 ? 0 : 1);
    o << v << ' ' << kUnits[i];
    return o.str();
}

inline Nanoseconds as_nanoseconds(Seconds s) { return std::chrono::duration_cast<Nanoseconds>(s); }

} // namespace admission_fabric
