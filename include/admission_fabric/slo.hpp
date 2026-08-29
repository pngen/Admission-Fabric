#pragma once

// Admission Fabric - SLO feasibility.
//
// Admission Fabric answers only the pre-admission question: "given current
// state and predicted execution requirements, is this latency obligation
// plausible enough to admit?" It is not the Latency Governor's job of live
// enforcement. SLO feasibility uses predicted duration plus current
// contention and a bounded uncertainty model.

#include <optional>
#include <string>
#include "admission_fabric/units.hpp"
#include "admission_fabric/error.hpp"

namespace admission_fabric {

enum class SloVerdict : std::uint8_t {
    Feasible = 0,
    FeasibleWithRisk = 1,
    Infeasible = 2,
    Unknown = 3
};
inline const char* slo_verdict_name(SloVerdict v) {
    switch (v) { case SloVerdict::Feasible: return "feasible"; case SloVerdict::FeasibleWithRisk: return "feasible_with_risk";
        case SloVerdict::Infeasible: return "infeasible"; case SloVerdict::Unknown: return "unknown"; }
    return "unknown";
}

// Pre-admission SLO feasibility evaluation.
struct SloFeasibility {
    SloVerdict verdict{SloVerdict::Unknown};
    Nanoseconds predicted_queue_delay{0};
    Nanoseconds predicted_transfer_delay{0};
    Nanoseconds predicted_execution_duration{0};
    Nanoseconds predicted_reservation_delay{0};
    // Total predicted time to complete once the work is admitted.
    [[nodiscard]] Nanoseconds predicted_total() const {
        return predicted_queue_delay + predicted_transfer_delay + predicted_execution_duration + predicted_reservation_delay;
    }
    // Remaining slack relative to an optional absolute deadline.
    std::optional<Nanoseconds> deadline_remaining;
    double uncertainty{0.0};              // 0..1 relative uncertainty of the estimate
    std::string explanation;

    [[nodiscard]] bool passes() const {
        return verdict == SloVerdict::Feasible || verdict == SloVerdict::FeasibleWithRisk;
    }
};

} // namespace admission_fabric
