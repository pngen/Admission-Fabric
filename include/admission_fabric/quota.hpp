#pragma once

// Admission Fabric - quota inputs.
//
// Admission Fabric accepts externally supplied quota constraints produced by
// Quota Fabric. It evaluates a request against that envelope; it does not own
// long-term quota accounting. Quota state carries a generation and an
// authority/source identity so stale quota cannot be silently reused.

#include <cstdint>
#include <string>
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

// A point-in-time quota snapshot that the admission engine consults.
struct QuotaSnapshot {
    QuotaSnapshot(){}
    QuotaSnapshot(bool present) : present_(present) {}

    bool present_{false};                 // false => no quota consultant configured (quota trivially passes)
    std::uint64_t generation{0};          // quota generation (monotonic)
    std::string source_authority;         // e.g. "quota-fabric@epoch3"
    TimePoint timestamp;
    bool stale{false};

    // Resource-dimension limits (only dimensions that are constrained are non-zero).
    ResourceVector limits;
    ResourceVector used;
    ResourceVector reserved;

    // Scalar caps (0 => unconstrained).
    TokenCount token_budget{0};
    SlotCount max_concurrent_requests{0};
    SlotCount max_sequences{0};
    Nanoseconds compute_time_allowance{0};

    [[nodiscard]] bool constrained(ResourceId id) const { return limits.get(id) != 0; }
    [[nodiscard]] ResourceVector remaining() const {
        ResourceVector over = limits.sat_sub(used.sat_add(reserved));
        return over;
    }
};

// Result of evaluating a request against a quota snapshot.
struct QuotaEvaluation {
    bool passed{false};
    ResourceVector committed_remaining;   // remaining headroom after this request
    std::optional<ResourceId> limiting_resource;   // the dimension/limit violated, if any
    std::string limiting_name;            // "token_budget" / resource name / "concurrency" etc.
    TokenCount token_remaining{0};
    bool token_violation{false};
    std::string explanation;
};

} // namespace admission_fabric
