#pragma once

// Admission Fabric - admission descriptor.
//
// The descriptor expresses what the *work itself* claims about itself before
// admission. It carries identity, workload envelope, token intent, deadline,
// SLO request, and any optional expected-resource hints. It does not silently
// manufacture resource estimates: resource demands are produced by the
// predictor from evidence. Missing information is explicit (optional fields).

#include <optional>
#include <string>
#include <vector>
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

// Top-level workload classification.
enum class WorkloadType : std::uint8_t {
    Inference = 0,
    Prefill = 1,
    Decode = 2,
    Training = 3,
    FineTune = 4,
    Embedding = 5,
    Rerank = 6,
    Other = 7
};
inline const char* workload_type_name(WorkloadType t) {
    switch (t) { case WorkloadType::Inference: return "inference"; case WorkloadType::Prefill: return "prefill";
        case WorkloadType::Decode: return "decode"; case WorkloadType::Training: return "training";
        case WorkloadType::FineTune: return "fine_tune"; case WorkloadType::Embedding: return "embedding";
        case WorkloadType::Rerank: return "rerank"; case WorkloadType::Other: return "other"; }
    return "unknown";
}

// Latency obligation class.
enum class LatencyClass : std::uint8_t {
    BestEffort = 0,
    Interactive = 1,
    Realtime = 2,
    FixedDeadline = 3,
    SoftRealtime = 4
};
inline const char* latency_class_name(LatencyClass c) {
    switch (c) { case LatencyClass::BestEffort: return "best_effort"; case LatencyClass::Interactive: return "interactive";
        case LatencyClass::Realtime: return "realtime"; case LatencyClass::FixedDeadline: return "fixed_deadline";
        case LatencyClass::SoftRealtime: return "soft_realtime"; }
    return "unknown";
}

// Request priority (bounded integer, higher = more urgent).
using Priority = std::uint8_t;

// Retry / deferral permission attached to a request.
struct RetryPolicy {
    std::uint32_t max_attempts{1};       // total attempts allowed
    std::uint32_t max_deferrals{0};      // how many times it may be deferred
    Nanoseconds defer_backoff{Seconds(1)};
    Nanoseconds defer_max_wait{Seconds(60)};
};

// Whether the work tolerates degrade/preempt semantics.
enum class PreemptionPermission : std::uint8_t { NotPermitted = 0, Preemptible = 1, Deferrable = 2 };

// A structured admission descriptor. Fields are optional where the runtime
// must not invent them; a call to validate() enforces that the *mandatory*
// identity and semantic fields are present and well-formed.
struct AdmissionDescriptor {
    // --- identity ----------------------------------------------------------
    AdmissionRequestId admission_request_id;
    RequestId request_id;
    AttemptId attempt_id;
    TenantId tenant_id;
    ModelId model_id;
    ModelRevision model_revision;
    std::optional<AdapterId> adapter_id;

    // --- policy / generation stamp ----------------------------------------
    PolicyGeneration policy_generation;   // which policy was in force when raised

    // --- workload ----------------------------------------------------------
    WorkloadType workload_type{WorkloadType::Inference};
    std::string workload_class;           // arbitrary policy tag class

    // Execution context (optional; empty means not declared by the requester).
    // Used to bind a prediction profile. Never manufactured by the runtime.
    std::string backend;                  // e.g. "cuda" / "cpu"
    std::string dtype;                    // e.g. "fp16"
    std::string quantization;             // e.g. "int8"

    // --- token envelope ----------------------------------------------------
    TokenCount input_tokens{0};
    TokenCount expected_output_tokens{0};
    TokenCount max_output_tokens{0};      // 0 => same as expected (hard cap unknown)
    bool batchable{false};

    // --- priority / latency ------------------------------------------------
    Priority priority{0};
    LatencyClass latency_class{LatencyClass::BestEffort};
    std::optional<Nanoseconds> deadline;  // absolute latency budget (from now)
    std::optional<double> requested_slo;  // e.g. 99th percentile target, seconds

    // --- optional expected resource hints (evidence, not authoritative) ----
    std::optional<ResourceVector> expected_demand;  // optional declared expectation

    // --- permissions -------------------------------------------------------
    PreemptionPermission preemption{PreemptionPermission::NotPermitted};
    bool degradation_permitted{false};

    // --- reservation / lifetime -------------------------------------------
    bool requires_reservation{true};
    Nanoseconds admission_ttl{Seconds(30)};

    // --- classification ----------------------------------------------------
    std::vector<std::string> policy_tags;

    // Validation. Returns ok() iff the descriptor is well-formed for evaluation.
    [[nodiscard]] Status validate() const {
        if (admission_request_id.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing admission_request_id");
        if (request_id.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing request_id");
        if (attempt_id.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing attempt_id");
        if (tenant_id.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing tenant_id");
        if (model_id.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing model_id");
        if (model_revision.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing model_revision");
        if (policy_generation.is_nil()) return Status::failure(ErrorCode::MalformedDescriptor, "missing policy_generation");
        // Token envelope sanity: budget <= max (when max provided), bounded.
        if (max_output_tokens != 0 && expected_output_tokens > max_output_tokens)
            return Status::failure(ErrorCode::MalformedDescriptor, "expected_output_tokens > max_output_tokens");
        if (input_tokens > (TokenCount{1} << 40))
            return Status::failure(ErrorCode::MalformedDescriptor, "input_tokens absurd");
        if (expected_output_tokens > (TokenCount{1} << 40))
            return Status::failure(ErrorCode::MalformedDescriptor, "expected_output_tokens absurd");
        if (max_output_tokens > (TokenCount{1} << 40))
            return Status::failure(ErrorCode::MalformedDescriptor, "max_output_tokens absurd");
        if (deadline.has_value() && deadline->count() <= 0)
            return Status::failure(ErrorCode::MalformedDescriptor, "non-positive deadline");
        if (requested_slo.has_value() && !(*requested_slo > 0.0))
            return Status::failure(ErrorCode::MalformedDescriptor, "non-positive SLO");
        if (priority > 63)
            return Status::failure(ErrorCode::MalformedDescriptor, "priority out of range");
        if (admission_ttl.count() <= 0)
            return Status::failure(ErrorCode::MalformedDescriptor, "non-positive admission_ttl");
        return Status::success();
    }
};

} // namespace admission_fabric
