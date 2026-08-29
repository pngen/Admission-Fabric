#include "test_util.hpp"
#include "admission_fabric/backends/cuda_backend.hpp"
#include "admission_fabric/engine.hpp"
#include "admission_fabric/observation.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"

#include <cstdio>

using namespace admission_fabric;

static AdmissionPolicy p_policy() {
    AdmissionPolicy p;
    p.name = "cuda-pred";
    p.max_snapshot_age = Seconds(60);
    p.require_reservation = true;
    p.safety_headroom_ratio = 0.05;
    return p;
}

static PredictionProfile p_profile() {
    PredictionProfile pr;
    pr.model_id = ModelId(11); pr.revision = ModelRevision(1);
    pr.backend = "cuda"; pr.dtype = "fp16"; pr.quantization = "none";
    pr.fixed_base_memory = ByteCount{256} << 20;
    pr.per_token_kv_bytes = 256;
    pr.per_token_tensor_bytes = 256;
    pr.per_request_overhead = ByteCount{2} << 20;
    pr.workspace_bytes = ByteCount{8} << 20;
    pr.staging_bytes = ByteCount{8} << 20;
    pr.pinned_staging_bytes = ByteCount{4} << 20;
    pr.prefill_tokens_per_sec = 3000.0;
    pr.decode_tokens_per_sec = 120.0;
    return pr;
}

static AdmissionDescriptor p_desc(AdmissionRequestId id, TokenCount tok) {
    AdmissionDescriptor d;
    d.admission_request_id = id;
    d.request_id = RequestId(id.value() + 2000);
    d.attempt_id = AttemptId(1);
    d.tenant_id = TenantId(1);
    d.model_id = ModelId(11);
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

static AdmissionFabric make_pred_engine(ByteCount vram) {
    AdmissionFabric f;
    Status sp = f.install_policy(p_policy());
    (void)sp;
    AF_CHECK(f.register_profile(p_profile()));
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

AF_TEST(cuda_prediction_feedback) {
    CudaBackend be;
    int devs = 0;
    AF_CHECK(CudaBackend::device_count(devs).ok() && devs >= 1);
    AF_CHECK(be.init(0).ok());

    // Build the engine with a LARGER governed capacity so the first evaluation
    // is optimistic (cold-start low confidence but fits).
    AdmissionFabric f = make_pred_engine(ByteCount{6} << 30);

    // Execute several real workloads, capture their actual resource footprint.
    std::vector<ByteCount> actual_peaks;
    std::vector<TokenCount> tokens;
    for (TokenCount tok : {2000u, 4000u, 8000u}) {
        ByteCount bytes = (ByteCount{256} << 20) + (static_cast<ByteCount>(tok) * 256u) * 2 + (ByteCount{8} << 20);
        void* ptr = nullptr;
        AF_CHECK(be.allocate(ptr, static_cast<std::size_t>(bytes)).ok());
        AF_CHECK(be.run_and_verify_add(1u << 20).ok());
        AF_CHECK(be.free(ptr).ok());
        actual_peaks.push_back(bytes);
        tokens.push_back(tok);

        Observation o;
        o.id = ObservationId(tok);
        o.admission_request_id = AdmissionRequestId(tok);
        o.model_id = ModelId(11); o.model_revision = ModelRevision(1);
        o.backend = "cuda"; o.dtype = "fp16"; o.quantization = "none";
        o.peak_vram = bytes;
        o.input_tokens = tok; o.output_tokens = tok;
        o.execution_duration = Milliseconds(50);
        o.completed_at = SystemClock::now();
        o.kind = ObservationKind::Completed;
        f.observe(o);
    }

    // Cold-start confidence vs post-feedback confidence.
    AdmissionDescriptor d = p_desc(AdmissionRequestId(999), 8000);
    double cold_confidence = f.predictor().predict_memory(d).confidence;
    (void)cold_confidence;
    // Evaluate a new workload: predictor should now show evidence + hybrid provenance.
    auto pred = f.predictor().predict_memory(d);
    std::printf("  post-feedback confidence=%.3f evidence=%u provenance=%s\n",
        pred.confidence, pred.evidence_count, provenance_name(pred.provenance));
    AF_CHECK(pred.evidence_count > 0);
    AF_CHECK(pred.confidence > 0.5);

    // Decision at large governed capacity: fits.
    auto r_big = f.evaluate(p_desc(AdmissionRequestId(1000), 8000));
    AF_CHECK(r_big.has_value());
    AF_CHECK(r_big.value().is_admit());
    ByteCount big_peak = r_big.value().memory_prediction.footprint.peak();
    std::printf("  big-governed predicted_peak=%zu\n", static_cast<std::size_t>(big_peak));

    // Shrink the governed capacity below the predicted peak -> the same request
    // must now be deferred (decision changes when governed capacity changes).
    AdmissionFabric g = make_pred_engine(ByteCount{200} << 20);
    auto r_small = g.evaluate(p_desc(AdmissionRequestId(1001), 8000));
    AF_CHECK(r_small.has_value());
    std::printf("  small-governed reason=%s peak=%zu\n", reason_code_name(r_small.value().reason), static_cast<std::size_t>(r_small.value().memory_prediction.footprint.peak()));
    AF_CHECK(r_small.value().is_defer() || r_small.value().is_reject());
    AF_CHECK(r_big.value().is_admit() && !r_small.value().is_admit());
}

AF_TEST_MAIN("admission-fabric-cuda-prediction");
