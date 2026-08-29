#pragma once

// Admission Fabric - the admission engine.
//
// AdmissionFabric owns the decision boundary: it decides whether new work may
// cross into the expensive execution system, based on a versioned policy, a
// provenance-aware resource snapshot, predicted resource demand / duration /
// memory footprint, quota inputs, capability feasibility, SLO feasibility and
// atomic capacity reservation. It is *not* a scheduler, quota database,
// resource broker, or latency governor.
//
// Admission is modelled as an authoritative state transition. A request moves
// PROPOSED -> EVALUATING -> RESERVED -> ADMITTED -> RELEASED, or is DEFERRED /
// REJECTED / EXPIRED / CANCELLED. Transitions are validated; stale authority
// cannot mutate current state.

#include <map>
#include <optional>
#include <string>
#include "admission_fabric/decision.hpp"
#include "admission_fabric/serialize.hpp"
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/fairness.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/quota.hpp"
#include "admission_fabric/reservation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

// Explicit request lifecycle.
enum class RequestLifecycle : std::uint8_t {
    Unknown = 0,
    Proposed = 1,
    Evaluating = 2,
    Deferred = 3,
    Reserved = 4,
    Admitted = 5,
    Released = 6,
    Rejected = 7,
    Expired = 8,
    Cancelled = 9
};
inline const char* lifecycle_name(RequestLifecycle s) {
    switch (s) { case RequestLifecycle::Unknown: return "unknown"; case RequestLifecycle::Proposed: return "proposed";
        case RequestLifecycle::Evaluating: return "evaluating"; case RequestLifecycle::Deferred: return "deferred";
        case RequestLifecycle::Reserved: return "reserved"; case RequestLifecycle::Admitted: return "admitted";
        case RequestLifecycle::Released: return "released"; case RequestLifecycle::Rejected: return "rejected";
        case RequestLifecycle::Expired: return "expired"; case RequestLifecycle::Cancelled: return "cancelled"; }
    return "unknown";
}

struct RequestRecord {
    AdmissionRequestId id;
    RequestLifecycle lifecycle{RequestLifecycle::Proposed};
    AdmissionDescriptor descriptor;
    std::optional<AdmissionDecision> last_decision;
    TimePoint created_at;
    TimePoint updated_at;
    std::uint64_t evaluation_count{0};
};

// Bounded operational evidence.
struct Metrics {
    std::uint64_t evaluations{0};
    std::uint64_t admissions{0};
    std::uint64_t deferrals{0};
    std::uint64_t rejections{0};
    std::map<int32_t, std::uint64_t> by_reason;
    std::uint64_t active_reservations{0};
    std::uint64_t reservation_leaks{0};
    std::uint64_t stale_authority_rejections{0};
    std::uint64_t quota_failures{0};
    std::uint64_t capability_failures{0};
    std::uint64_t slo_infeasibility{0};

    void count_reason(ReasonCode c) { by_reason[static_cast<int32_t>(c)]++; }
    [[nodiscard]] std::uint64_t reason_count(ReasonCode c) const {
        auto it = by_reason.find(static_cast<int32_t>(c));
        return it == by_reason.end() ? 0 : it->second;
    }
};

class AdmissionFabric {
public:
    AdmissionFabric();

    // --- policy ------------------------------------------------------------
    [[nodiscard]] Status install_policy(const AdmissionPolicy& policy);
    [[nodiscard]] const AdmissionPolicy* policy() const;       // latest
    [[nodiscard]] const PolicyEngine& policies() const { return policy_; }

    // --- prediction --------------------------------------------------------
    [[nodiscard]] bool register_profile(const PredictionProfile& profile);
    [[nodiscard]] WorkloadPredictor& predictor() { return predictor_; }
    [[nodiscard]] const WorkloadPredictor& predictor() const { return predictor_; }

    // --- capacity / snapshot ----------------------------------------------
    void set_snapshot(const ResourceSnapshot& snapshot);
    [[nodiscard]] const ResourceSnapshot& snapshot() const { return snapshot_; }
    [[nodiscard]] bool has_snapshot() const { return has_snapshot_; }

    // --- quota -------------------------------------------------------------
    void set_quota(const QuotaSnapshot& quota);
    [[nodiscard]] const QuotaSnapshot& quota() const { return quota_; }

    // --- reservations ------------------------------------------------------
    [[nodiscard]] const ReservationManager& reservations() const { return reservations_; }

    // --- fairness ----------------------------------------------------------
    [[nodiscard]] FairnessController& fairness() { return fairness_; }

    // --- evaluation --------------------------------------------------------
    // Evaluate a descriptor. Produces a typed decision; for admit outcomes a
    // provisional reservation is created and its id is returned in the decision.
    [[nodiscard]] Result<AdmissionDecision> evaluate(const AdmissionDescriptor& descriptor);

    // --- authoritative transitions ----------------------------------------
    // Commit a provisional reservation into ADMITTED. Rejects stale generation.
    [[nodiscard]] Status commit_admission(const AdmissionRequestId& request_id, const AdmissionGeneration& generation);
    // Release a reservation / admitted request. Idempotent.
    [[nodiscard]] Status release_reservation(const ReservationId& id);
    // Cancel a request (terminal). Valid only from Proposed/Deferred/Evaluating.
    [[nodiscard]] Status cancel_request(const AdmissionRequestId& request_id);

    // --- observation -------------------------------------------------------
    void observe(const Observation& observation);

    // --- durable state -----------------------------------------------------
    // Serialize the engine's authoritative state (policy library, reservation
    // table, requests, quota, snapshot, counters) field-by-field.
    [[nodiscard]] Status save_state(ByteWriter& w) const;
    [[nodiscard]] Status restore_state(ByteReader& r);

    // --- introspection -----------------------------------------------------
    [[nodiscard]] const RequestRecord* request(const AdmissionRequestId& id) const;
    [[nodiscard]] std::size_t request_count() const { return requests_.size(); }
    [[nodiscard]] const std::map<AdmissionRequestId, RequestRecord>& requests() const { return requests_; }
    [[nodiscard]] AdmissionGeneration admission_generation() const { return admission_generation_; }
    [[nodiscard]] const Metrics& metrics() const { return metrics_; }

    // --- explanation -------------------------------------------------------
    [[nodiscard]] std::string explain(const AdmissionDecision& d) const;

private:
    PolicyEngine policy_;
    WorkloadPredictor predictor_;
    ReservationManager reservations_;
    ResourceSnapshot snapshot_;
    bool has_snapshot_{false};
    QuotaSnapshot quota_;
    FairnessController fairness_;
    Metrics metrics_;
    std::map<AdmissionRequestId, RequestRecord> requests_;
    AdmissionGeneration admission_generation_{1};
    std::uint64_t reservation_counter_{1};

    [[nodiscard]] AdmissionGeneration next_admission_generation_();
    [[nodiscard]] ReservationId next_reservation_id_();

    [[nodiscard]] AdmissionDecision build_decision_(const AdmissionDescriptor& d, ReasonCode reason) const;
    [[nodiscard]] ReasonCode classify_capability_(const AdmissionDescriptor& d) const;
    [[nodiscard]] ReasonCode classify_quota_(const AdmissionDescriptor& d, const ResourceVector& demand, ResourceVector& committed_remaining, std::string& what) const;
    [[nodiscard]] SloFeasibility evaluate_slo_(const AdmissionDescriptor& d, const DurationPrediction& dur) const;

    [[nodiscard]] Status transition_(const AdmissionRequestId& id, RequestLifecycle from, RequestLifecycle to, const AdmissionDescriptor& d);
};

} // namespace admission_fabric
