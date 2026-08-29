#include "test_util.hpp"
#include "admission_fabric/backends/cuda_backend.hpp"
#include "admission_fabric/engine.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"

#include <cstdio>

using namespace admission_fabric;

static AdmissionPolicy oc_policy() {
    AdmissionPolicy p;
    p.name = "cuda-governed";
    p.max_snapshot_age = Seconds(60);
    p.require_reservation = true;
    p.safety_headroom_ratio = 0.05;
    return p;
}

static PredictionProfile oc_profile() {
    PredictionProfile pr;
    pr.model_id = ModelId(9); pr.revision = ModelRevision(1);
    pr.backend = "cuda"; pr.dtype = "fp16"; pr.quantization = "none";
    pr.fixed_base_memory = ByteCount{1} << 30;
    pr.per_token_kv_bytes = 512;
    pr.per_token_tensor_bytes = 512;
    pr.per_request_overhead = ByteCount{4} << 20;
    pr.workspace_bytes = ByteCount{16} << 20;
    pr.staging_bytes = ByteCount{32} << 20;
    pr.pinned_staging_bytes = ByteCount{8} << 20;
    pr.prefill_tokens_per_sec = 2000.0;
    pr.decode_tokens_per_sec = 80.0;
    return pr;
}

static AdmissionDescriptor oc_desc(AdmissionRequestId id, TokenCount tok) {
    AdmissionDescriptor d;
    d.admission_request_id = id;
    d.request_id = RequestId(id.value() + 1000);
    d.attempt_id = AttemptId(1);
    d.tenant_id = TenantId(1);
    d.model_id = ModelId(9);
    d.model_revision = ModelRevision(1);
    d.policy_generation = PolicyGeneration(1);
    d.input_tokens = tok;
    d.expected_output_tokens = tok;
    d.max_output_tokens = tok;
    d.backend = "cuda";
    d.dtype = "fp16";
    d.latency_class = LatencyClass::BestEffort;
    return d;
}

static AdmissionFabric make_governed_engine(ByteCount vram) {
    AdmissionFabric f;
    Status sp = f.install_policy(oc_policy());
    (void)sp;
    AF_CHECK(f.register_profile(oc_profile()));
    ResourceSnapshot s;
    s.generation = ResourceSnapshotGeneration(1);
    s.timestamp = SystemClock::now();
    s.agent_id = AgentId(1);
    s.boot_id = AgentBootId(1);
    s.node_name = "rtx5090";
    s.total.set(ResourceId::AcceleratorMemory, vram);
    s.total.set(ResourceId::HostMemory, ByteCount{8} << 30);
    s.total.set(ResourceId::PinnedHostMemory, ByteCount{2} << 30);
    s.total.set(ResourceId::ExecutionSlots, 64);
    s.capabilities.add(Capabilities::backend("cuda"));
    s.capabilities.add(Capabilities::dtype("fp16"));
    s.capabilities.add(Capabilities::compute_capability("12.0"));
    for (auto& p : s.pressure) p = PressureLevel::Low;
    for (auto& p : s.provenance) p = Provenance::Measured;
    f.set_snapshot(s);
    return f;
}

AF_TEST(cuda_governed_admission) {
    CudaBackend be;
    int devs = 0;
    AF_CHECK(CudaBackend::device_count(devs).ok());
    AF_CHECK(devs >= 1);
    AF_CHECK(be.init(0).ok());
    std::string name; int major = 0, minor = 0; std::size_t total = 0;
    AF_CHECK(be.properties(name, major, minor, total).ok());
    std::printf("  CUDA device: %s (cc %d.%d, %zu bytes)\n", name.c_str(), major, minor, total);
    AF_CHECK(total > 0);

    const ByteCount governed = ByteCount{2} << 30;  // 2 GiB governed
    AF_CHECK(be.set_governed_capacity(governed).ok());

    AdmissionFabric f = make_governed_engine(governed);

    // R1: footprint ~1.1 GiB -> ADMIT.
    AdmissionDescriptor d1 = oc_desc(AdmissionRequestId(1), 2000);
    auto r1 = f.evaluate(d1);
    AF_CHECK(r1.has_value());
    AF_CHECK(r1.value().is_admit());
    AF_CHECK(r1.value().reservation_id.has_value());
    ByteCount reserve1 = r1.value().predicted_requirement.demand().get(ResourceId::AcceleratorMemory);
    std::printf("  R1 reserved %zu bytes\n", static_cast<std::size_t>(reserve1));

    // Actually allocate real CUDA memory for the reserved capacity and run work.
    void* ptr = nullptr;
    AF_CHECK(be.allocate(ptr, static_cast<std::size_t>(reserve1)).ok());
    AF_CHECK(be.run_and_verify_add(1u << 20).ok());
    AF_CHECK(be.free(ptr).ok());

    // R2: another ~1.1 GiB request -> should DEFER (2.2 GiB > 2.0 GiB governed).
    AdmissionDescriptor d2 = oc_desc(AdmissionRequestId(2), 2000);
    auto r2 = f.evaluate(d2);
    AF_CHECK(r2.has_value());
    AF_CHECK(r2.value().is_defer() || r2.value().is_reject());

    // Release R1 reservation, then R2 must now be admissible.
    AF_CHECK(f.release_reservation(*r1.value().reservation_id).ok());
    auto r3 = f.evaluate(d2);
    AF_CHECK(r3.has_value());
    if (!r3.value().is_admit())
        std::printf("  R2 after release: reason=%s expl=%s\n", reason_code_name(r3.value().reason), r3.value().explanation.c_str());
    AF_CHECK(r3.value().is_admit());
    AF_CHECK(r3.value().reservation_id.has_value());

    // Execute the second workload for real and verify, then release.
    void* ptr2 = nullptr;
    ByteCount reserve2 = r3.value().predicted_requirement.demand().get(ResourceId::AcceleratorMemory);
    AF_CHECK(be.allocate(ptr2, static_cast<std::size_t>(reserve2)).ok());
    AF_CHECK(be.run_and_verify_add(2u << 20).ok());
    AF_CHECK(be.free(ptr2).ok());
    AF_CHECK(f.release_reservation(*r3.value().reservation_id).ok());

    // Exact accounting-to-zero.
    AF_CHECK(f.reservations().verify_accounting());
    AF_CHECK(f.reservations().reserved().is_zero());
    AF_CHECK(f.reservations().used().is_zero());
}

AF_TEST_MAIN("admission-fabric-cuda-governed");
