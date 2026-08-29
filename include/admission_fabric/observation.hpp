#pragma once

// Admission Fabric - completed-work observations.
//
// Observations are the feedback signal that lets the bounded predictor improve.
// An observation records actual measured resource consumption and duration for
// a completed workload. Malformed, negative, or wildly out-of-range values are
// rejected and never corrupt prediction history.

#include <string>
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

enum class ObservationKind : std::uint8_t { Completed = 0, Failed = 1, Aborted = 2 };

struct Observation {
    ObservationId id;
    AdmissionRequestId admission_request_id;
    TenantId tenant_id;
    ModelId model_id;
    ModelRevision model_revision;
    std::string backend;
    std::string dtype;
    std::string quantization;
    WorkloadType workload_type{WorkloadType::Inference};

    ByteCount peak_vram{0};
    ByteCount host_memory{0};
    ByteCount pinned_memory{0};
    ByteCount kv_growth{0};
    ByteCount transfer_bytes{0};
    Nanoseconds execution_duration{0};

    TokenCount input_tokens{0};
    TokenCount output_tokens{0};

    TimePoint completed_at;
    ObservationKind kind{ObservationKind::Completed};

    // Reject malformed / absurd / impossible observations.
    [[nodiscard]] Status validate() const {
        if (id.is_nil()) return Status::failure(ErrorCode::InvalidArgument, "observation id nil");
        if (model_id.is_nil()) return Status::failure(ErrorCode::InvalidArgument, "model id nil");
        if (backend.empty()) return Status::failure(ErrorCode::InvalidArgument, "backend empty");
        if (kind == ObservationKind::Completed) {
            if (execution_duration.count() < 0)
                return Status::failure(ErrorCode::InvalidArgument, "negative duration");
            if (input_tokens > (TokenCount{1} << 40) || output_tokens > (TokenCount{1} << 40))
                return Status::failure(ErrorCode::InvalidArgument, "token count absurd");
            if (peak_vram > (ByteCount{1} << 48) || host_memory > (ByteCount{1} << 48)
                || pinned_memory > (ByteCount{1} << 48))
                return Status::failure(ErrorCode::InvalidArgument, "memory value absurd");
        }
        return Status::success();
    }

    // Observed decode rate (output tokens per second).
    [[nodiscard]] double decode_tokens_per_sec() const {
        auto ns = execution_duration.count();
        if (ns <= 0 || output_tokens == 0) return 0.0;
        return static_cast<double>(output_tokens) / (static_cast<double>(ns) / 1e9);
    }
};

} // namespace admission_fabric
