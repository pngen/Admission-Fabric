#pragma once
// Shared helpers for Admission Fabric examples.
#include "admission_fabric/engine.hpp"
#include "admission_fabric/decision.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/slo.hpp"
#include <cstdio>
#include <string>

namespace afex {

inline admission_fabric::AdmissionPolicy policy() {
    admission_fabric::AdmissionPolicy p;
    p.name = "example";
    p.max_snapshot_age = std::chrono::seconds(60);
    p.require_reservation = true;
    p.safety_headroom_ratio = 0.05;
    return p;
}

inline admission_fabric::PredictionProfile profile(admission_fabric::ModelId m, admission_fabric::ModelRevision rev) {
    admission_fabric::PredictionProfile pr;
    pr.model_id = m; pr.revision = rev;
    pr.backend = "cuda"; pr.dtype = "fp16"; pr.quantization = "none";
    pr.fixed_base_memory = admission_fabric::ByteCount{1} << 30;
    pr.per_token_kv_bytes = 512;
    pr.per_token_tensor_bytes = 512;
    pr.per_request_overhead = admission_fabric::ByteCount{4} << 20;
    pr.workspace_bytes = admission_fabric::ByteCount{16} << 20;
    pr.staging_bytes = admission_fabric::ByteCount{32} << 20;
    pr.pinned_staging_bytes = admission_fabric::ByteCount{8} << 20;
    pr.prefill_tokens_per_sec = 2000.0;
    pr.decode_tokens_per_sec = 80.0;
    pr.execution_slots_required = 1;
    pr.sequence_slots_required = 1;
    return pr;
}

inline admission_fabric::AdmissionFabric engine(admission_fabric::ByteCount vram) {
    admission_fabric::AdmissionFabric f;
    auto sp = f.install_policy(policy()); (void)sp;
    bool rp = f.register_profile(profile(admission_fabric::ModelId(7), admission_fabric::ModelRevision(1)));
    (void)rp;
    admission_fabric::ResourceSnapshot s;
    s.generation = admission_fabric::ResourceSnapshotGeneration(1);
    s.timestamp = admission_fabric::SystemClock::now();
    s.agent_id = admission_fabric::AgentId(1);
    s.boot_id = admission_fabric::AgentBootId(1);
    s.node_name = "example-node";
    s.total.set(admission_fabric::ResourceId::AcceleratorMemory, vram);
    s.total.set(admission_fabric::ResourceId::HostMemory, admission_fabric::ByteCount{8} << 30);
    s.total.set(admission_fabric::ResourceId::PinnedHostMemory, admission_fabric::ByteCount{2} << 30);
    s.total.set(admission_fabric::ResourceId::ExecutionSlots, 64);
    s.total.set(admission_fabric::ResourceId::SequenceSlots, 256);
    s.capabilities.add(admission_fabric::Capabilities::backend("cuda"));
    s.capabilities.add(admission_fabric::Capabilities::dtype("fp16"));
    s.capabilities.add(admission_fabric::Capabilities::compute_capability("12.0"));
    for (auto& p : s.pressure) p = admission_fabric::PressureLevel::Low;
    for (auto& p : s.provenance) p = admission_fabric::Provenance::Measured;
    f.set_snapshot(s);
    return f;
}

inline admission_fabric::AdmissionDescriptor desc(admission_fabric::AdmissionRequestId id,
                                                  admission_fabric::TokenCount in,
                                                  admission_fabric::TokenCount out,
                                                  admission_fabric::TenantId tenant = admission_fabric::TenantId(1)) {
    admission_fabric::AdmissionDescriptor d;
    d.admission_request_id = id;
    d.request_id = admission_fabric::RequestId(id.value() * 10 + 7);
    d.attempt_id = admission_fabric::AttemptId(1);
    d.tenant_id = tenant;
    d.model_id = admission_fabric::ModelId(7);
    d.model_revision = admission_fabric::ModelRevision(1);
    d.policy_generation = admission_fabric::PolicyGeneration(1);
    d.input_tokens = in;
    d.expected_output_tokens = out;
    d.max_output_tokens = out;
    d.backend = "cuda";
    d.dtype = "fp16";
    d.latency_class = admission_fabric::LatencyClass::BestEffort;
    return d;
}

} // namespace afex
