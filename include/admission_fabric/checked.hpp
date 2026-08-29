#pragma once

// Admission Fabric - checked arithmetic.
//
// Capacity accounting and reservation logic are the places most likely to
// silently overflow and produce wildly wrong admission decisions. All
// critical arithmetic therefore goes through explicit checked primitives that
// report overflow rather than wrapping. Debug builds assert; shipping builds
// propagate the error code.

#include <cstdint>
#include <limits>
#include <stdexcept>
#include "admission_fabric/error.hpp"

namespace admission_fabric {

// Returns true on success, false on overflow.
[[nodiscard]] inline bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (b > (std::numeric_limits<std::uint64_t>::max)() - a) { return false; }
    out = a + b;
    return true;
}

[[nodiscard]] inline bool checked_sub(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (b > a) { return false; }
    out = a - b;
    return true;
}

[[nodiscard]] inline bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a) { return false; }
    out = a * b;
    return true;
}

// Saturating add/subtract for cases where a bounded clamp is preferable to
// an error (e.g. derived headroom reporting).
[[nodiscard]] inline std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    if (b > (std::numeric_limits<std::uint64_t>::max)() - a) return (std::numeric_limits<std::uint64_t>::max)();
    return a + b;
}
[[nodiscard]] inline std::uint64_t sat_sub(std::uint64_t a, std::uint64_t b) noexcept {
    return a >= b ? a - b : 0;
}

} // namespace admission_fabric
