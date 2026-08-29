#pragma once

// Admission Fabric - admission decision.
//
// Admission evaluation returns a typed result, never a bare Boolean. A decision
// carries a primary outcome (admit / defer / reject), a detailed reason code,
// the governing policy and resource-snapshot generations, the predicted
// requirements and duration, SLO and quota results, the reservation that
// backs the admission, confidence, and a human-readable explanation.

#include <optional>
#include <string>
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/quota.hpp"
#include "admission_fabric/slo.hpp"
#include "admission_fabric/units.hpp"
#include "admission_fabric/error.hpp"

namespace admission_fabric {

enum class AdmissionOutcome : std::uint8_t {
    Admit = 0,
    Defer = 1,
    Reject = 2
};
inline const char* outcome_name(AdmissionOutcome o) {
    switch (o) { case AdmissionOutcome::Admit: return "admit"; case AdmissionOutcome::Defer: return "defer";
        case AdmissionOutcome::Reject: return "reject"; }
    return "unknown";
}

// Detailed reason code. The primary outcome is the coarse classification; the
// reason code is the explainable sub-outcome.
enum class ReasonCode : std::int32_t {
    Admit = 0,
    AdmitWithReservation = 1,
    AdmitWithRisk = 2,
    DeferCapacity = 3,
    DeferQuota = 4,
    DeferSlo = 5,
    DeferCapability = 6,
    RejectInfeasible = 7,
    RejectPolicy = 8,
    RejectCapability = 9,
    RejectQuota = 10,
    RejectExpired = 11,
    RejectMalformed = 12,
    RejectUnsupported = 13,
    RejectStale = 14
};
inline const char* reason_code_name(ReasonCode c) {
    switch (c) {
        case ReasonCode::Admit: return "ADMIT";
        case ReasonCode::AdmitWithReservation: return "ADMIT_WITH_RESERVATION";
        case ReasonCode::AdmitWithRisk: return "ADMIT_WITH_RISK";
        case ReasonCode::DeferCapacity: return "DEFER_CAPACITY";
        case ReasonCode::DeferQuota: return "DEFER_QUOTA";
        case ReasonCode::DeferSlo: return "DEFER_SLO";
        case ReasonCode::DeferCapability: return "DEFER_CAPABILITY";
        case ReasonCode::RejectInfeasible: return "REJECT_INFEASIBLE";
        case ReasonCode::RejectPolicy: return "REJECT_POLICY";
        case ReasonCode::RejectCapability: return "REJECT_CAPABILITY";
        case ReasonCode::RejectQuota: return "REJECT_QUOTA";
        case ReasonCode::RejectExpired: return "REJECT_EXPIRED";
        case ReasonCode::RejectMalformed: return "REJECT_MALFORMED";
        case ReasonCode::RejectUnsupported: return "REJECT_UNSUPPORTED";
        case ReasonCode::RejectStale: return "REJECT_STALE";
    }
    return "UNKNOWN";
}

[[nodiscard]] inline AdmissionOutcome primary_outcome_of(ReasonCode c) {
    switch (c) {
        case ReasonCode::Admit:
        case ReasonCode::AdmitWithReservation:
        case ReasonCode::AdmitWithRisk:
            return AdmissionOutcome::Admit;
        case ReasonCode::DeferCapacity:
        case ReasonCode::DeferQuota:
        case ReasonCode::DeferSlo:
        case ReasonCode::DeferCapability:
            return AdmissionOutcome::Defer;
        default:
            return AdmissionOutcome::Reject;
    }
}

// A deferred/retry hint: what would have to change for re-admission.
struct DeferHint {
    ReasonCode reason{ReasonCode::DeferCapacity};
    Nanoseconds earliest_reevaluation{0};
    std::string required_resource_change;   // e.g. "accelerator_memory: +512MiB"
    std::string quota_change_required;
    std::string explanation;
};

struct AdmissionDecision {
    // Identity.
    AdmissionRequestId admission_request_id;
    RequestId request_id;
    AttemptId attempt_id;
    TenantId tenant_id;
    ModelId model_id;
    ModelRevision model_revision;

    AdmissionOutcome outcome{AdmissionOutcome::Reject};
    ReasonCode reason{ReasonCode::RejectInfeasible};

    // Authority / generations that governed this decision.
    PolicyGeneration policy_generation;
    ResourceSnapshotGeneration snapshot_generation;
    AdmissionGeneration admission_generation;
    CoordinatorEpoch epoch;

    // Predicted envelope.
    ResourceRequirement predicted_requirement;
    MemoryFootprintPrediction memory_prediction;
    DurationPrediction duration_prediction;
    SloFeasibility slo;
    QuotaEvaluation quota;

    // Reservation backing the admission (set only when admitted / reserved).
    std::optional<ReservationId> reservation_id;

    // Confidence (0..1) in the capacity prediction that drove the decision.
    double confidence{0.0};

    // Timestamps.
    TimePoint decision_time;
    std::optional<TimePoint> expires_at;

    // Explanation + optional defer hint.
    std::string explanation;
    std::optional<DeferHint> defer_hint;

    [[nodiscard]] bool is_admit() const { return outcome == AdmissionOutcome::Admit; }
    [[nodiscard]] bool is_defer() const { return outcome == AdmissionOutcome::Defer; }
    [[nodiscard]] bool is_reject() const { return outcome == AdmissionOutcome::Reject; }
};

} // namespace admission_fabric
