#pragma once

// Admission Fabric - CPU / generic capacity backend.
//
// A CPU-only backend for tests, examples and CI portability. It models a node's
// resource inventory and produces a completed-work Observation that can be fed
// back into the bounded predictor, exactly as the CUDA backend produces real
// measured observations. The core library never requires a CUDA toolkit; this
// backend is always available.

#include <cstdint>
#include "admission_fabric/observation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

class CpuBackend {
public:
    void set_inventory(const ResourceVector& total) { inventory_ = total; }
    [[nodiscard]] const ResourceVector& inventory() const { return inventory_; }

    // Model a completed CPU workload and return a valid observation reflecting
    // its measured resource footprint and duration.
    [[nodiscard]] Observation run_workload(ModelId model, ModelRevision rev, TokenCount input,
                                           TokenCount output, ByteCount work_bytes) const {
        Observation o;
        o.id = ObservationId(static_cast<std::uint64_t>(model.value()) << 32 | rev.value());
        o.admission_request_id = AdmissionRequestId(0);
        o.tenant_id = TenantId(0);
        o.model_id = model;
        o.model_revision = rev;
        o.backend = "cpu";
        o.dtype = "fp32";
        o.quantization = "none";
        o.workload_type = WorkloadType::Inference;
        o.peak_vram = 0;                       // CPU has no accelerator memory
        o.host_memory = (work_bytes / 2) + (ByteCount{1} << 20);
        o.pinned_memory = work_bytes / 16;
        o.kv_growth = (input + output) * 8;
        o.transfer_bytes = work_bytes;
        // ~40 output tokens/sec modeled on CPU.
        o.execution_duration = Nanoseconds(static_cast<std::int64_t>(
            static_cast<double>(output) / 40.0 * 1e9));
        o.input_tokens = input;
        o.output_tokens = output;
        o.completed_at = SystemClock::now();
        o.kind = ObservationKind::Completed;
        return o;
    }

private:
    ResourceVector inventory_;
};

} // namespace admission_fabric
