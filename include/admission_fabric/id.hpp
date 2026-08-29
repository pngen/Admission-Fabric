#pragma once

// Admission Fabric - strong typed identity.
//
// Every identity in the system is a strongly typed wrapper around an
// underlying integer. The template tag makes distinct identity domains
// mutually incompatible at compile time: an AdmissionRequestId can never be
// passed where a TenantId (or a ReservationId) is required without an
// explicit conversion. This is the foundation of the authority envelope:
// stale values cannot be made to collide with fresh ones through mere
// assignment or string coercion.

#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>

namespace admission_fabric {

template <typename Tag, typename Rep = std::uint64_t>
class Id {
public:
    using rep_type = Rep;
    using tag_type = Tag;

    constexpr Id() noexcept = default;
    constexpr explicit Id(Rep value) noexcept : value_(value) {}
    constexpr Id(const Id&) noexcept = default;
    constexpr Id& operator=(const Id&) noexcept = default;

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == Rep{0}; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != Rep{0}; }

    // next() yields an ID whose value is one greater, saturating at the
    // maximum representable value so a saturated generation cannot wrap to 0
    // and resurrect stale authority.
    [[nodiscard]] constexpr Id next() const noexcept {
        return Id(value_ == (std::numeric_limits<Rep>::max)() ? value_ : static_cast<Rep>(value_ + Rep{1}));
    }

    [[nodiscard]] std::string str() const {
        std::ostringstream o;
        o << value_;
        return o.str();
    }

    friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Id a, Id b) noexcept { return !(a == b); }
    friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator<=(Id a, Id b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>(Id a, Id b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator>=(Id a, Id b) noexcept { return a.value_ >= b.value_; }

private:
    Rep value_{};
};

// Identity tags. Each distinct identity domain has its own tag.
struct AdmissionRequestIdTag {};
struct RequestIdTag {};
struct AttemptIdTag {};
struct TenantIdTag {};
struct ModelIdTag {};
struct ModelRevisionTag {};
struct AdapterIdTag {};
struct PolicyGenerationTag {};
struct AdmissionGenerationTag {};
struct CoordinatorEpochTag {};
struct ReservationIdTag {};
struct ResourceSnapshotGenerationTag {};
struct SchedulerRequestIdTag {};
struct CorrelationIdTag {};
struct AgentIdTag {};
struct WorkerIdTag {};
struct AgentBootIdTag {};
struct ObservationIdTag {};
struct ProtocolVersionTag {};
struct CapacityGenerationTag {};

// Public identity aliases.
using AdmissionRequestId = Id<AdmissionRequestIdTag>;
using RequestId          = Id<RequestIdTag>;
using AttemptId          = Id<AttemptIdTag>;
using TenantId           = Id<TenantIdTag>;
using ModelId            = Id<ModelIdTag>;
using ModelRevision      = Id<ModelRevisionTag>;
using AdapterId          = Id<AdapterIdTag>;
using PolicyGeneration   = Id<PolicyGenerationTag>;
using AdmissionGeneration = Id<AdmissionGenerationTag>;
using CoordinatorEpoch   = Id<CoordinatorEpochTag>;
using ReservationId      = Id<ReservationIdTag>;
using ResourceSnapshotGeneration = Id<ResourceSnapshotGenerationTag>;
using SchedulerRequestId = Id<SchedulerRequestIdTag>;
using CorrelationId      = Id<CorrelationIdTag>;
using AgentId            = Id<AgentIdTag>;
using WorkerId           = Id<WorkerIdTag>;
using AgentBootId        = Id<AgentBootIdTag>;
using ObservationId      = Id<ObservationIdTag>;
using ProtocolVersion    = Id<ProtocolVersionTag>;
using CapacityGeneration = Id<CapacityGenerationTag>;

} // namespace admission_fabric

namespace std {
template <typename Tag, typename Rep>
struct hash<admission_fabric::Id<Tag, Rep>> {
    size_t operator()(const admission_fabric::Id<Tag, Rep>& id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};
} // namespace std
