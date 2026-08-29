#include "test_util.hpp"
#include "admission_fabric/engine.hpp"
#include "admission_fabric/decision.hpp"
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/fairness.hpp"
#include "admission_fabric/observation.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/quota.hpp"
#include "admission_fabric/reservation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/slo.hpp"

using namespace admission_fabric;

static AdmissionPolicy default_policy() {
    AdmissionPolicy p;
    p.name = "default";
    p.safety_headroom_ratio = 0.10;
    p.overcommit = OvercommitMode::None;
    p.max_snapshot_age = Seconds(5);
    p.require_reservation = true;
    p.max_reservation_lifetime = Seconds(120);
    p.default_admission_ttl = Seconds(30);
    return p;
}

static PredictionProfile model_profile(ModelId m, ModelRevision rev) {
    PredictionProfile prof;
    prof.model_id = m;
    prof.revision = rev;
    prof.backend = "cuda";
    prof.dtype = "fp16";
    prof.quantization = "none";
    prof.fixed_base_memory = ByteCount{1} << 30;
    prof.per_token_kv_bytes = 1024;
    prof.per_token_tensor_bytes = 1024;
    prof.per_request_overhead = ByteCount{4} << 20;
    prof.workspace_bytes = ByteCount{32} << 20;
    prof.staging_bytes = ByteCount{64} << 20;
    prof.pinned_staging_bytes = ByteCount{16} << 20;
    prof.prefill_tokens_per_sec = 1000.0;
    prof.decode_tokens_per_sec = 50.0;
    prof.execution_slots_required = 1;
    prof.sequence_slots_required = 1;
    return prof;
}

static AdmissionDescriptor make_desc(ModelId m, ModelRevision rev) {
    AdmissionDescriptor d;
    d.admission_request_id = AdmissionRequestId(1000);
    d.request_id = RequestId(2000);
    d.attempt_id = AttemptId(1);
    d.tenant_id = TenantId(3);
    d.model_id = m;
    d.model_revision = rev;
    d.policy_generation = PolicyGeneration(1);
    d.workload_type = WorkloadType::Inference;
    d.input_tokens = 1000;
    d.expected_output_tokens = 1000;
    d.max_output_tokens = 1000;
    d.backend = "cuda";
    d.dtype = "fp16";
    d.latency_class = LatencyClass::BestEffort;
    d.requires_reservation = true;
    return d;
}

static AdmissionFabric make_engine(ByteCount vram, const Capabilities& caps) {
    static std::uint64_t counter = 0;
    AdmissionFabric fabric;
    AdmissionPolicy pol = default_policy();
    AF_CHECK(fabric.install_policy(pol).ok());
    AF_CHECK(fabric.register_profile(model_profile(ModelId(7), ModelRevision(1))));
    ResourceSnapshot snap;
    snap.generation = ResourceSnapshotGeneration(++counter);
    snap.timestamp = SystemClock::now();
    snap.agent_id = AgentId(1);
    snap.boot_id = AgentBootId(1);
    snap.node_name = "node-a";
    snap.device_index = 0;
    snap.device_name = "rtx5090";
    snap.total.set(ResourceId::AcceleratorMemory, vram);
    snap.total.set(ResourceId::HostMemory, ByteCount{4} << 30);
    snap.total.set(ResourceId::PinnedHostMemory, ByteCount{1} << 30);
    snap.total.set(ResourceId::ExecutionSlots, 64);
    snap.total.set(ResourceId::SequenceSlots, 256);
    snap.capabilities = caps;
    snap.healthy = true;
    for (auto& p : snap.pressure) p = PressureLevel::Low;
    for (auto& p : snap.provenance) p = Provenance::Measured;
    fabric.set_snapshot(snap);
    return fabric;
}

static Capabilities cuda_caps() {
    Capabilities c;
    c.add(Capabilities::backend("cuda"));
    c.add(Capabilities::dtype("fp16"));
    c.add(Capabilities::compute_capability("12.0"));
    return c;
}

AF_TEST(engine_admits_and_accounts_to_zero) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(11);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    const AdmissionDecision& dec = r.value();
    AF_CHECK(dec.is_admit());
    AF_CHECK(dec.reason == ReasonCode::AdmitWithReservation || dec.reason == ReasonCode::AdmitWithRisk);
    AF_CHECK(dec.reservation_id.has_value());
    AF_CHECK(dec.memory_prediction.footprint.kv_growth > 0);
    AF_CHECK(dec.duration_prediction.expected.count() > 0);
    Status cs = f.commit_admission(d.admission_request_id, dec.admission_generation);
    AF_CHECK(cs.ok());
    AF_CHECK(f.request(d.admission_request_id)->lifecycle == RequestLifecycle::Admitted);
    Status rs = f.release_reservation(*dec.reservation_id);
    AF_CHECK(rs.ok());
    AF_CHECK(f.request(d.admission_request_id)->lifecycle == RequestLifecycle::Released);
    AF_CHECK(f.reservations().verify_accounting());
    AF_CHECK(f.reservations().reserved().is_zero());
    AF_CHECK(f.reservations().used().is_zero());
}

AF_TEST(engine_defers_on_insufficient_capacity) {
    AdmissionFabric f = make_engine(ByteCount{1100} << 20, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(12);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().is_defer() || r.value().is_reject());
}

AF_TEST(engine_rejects_capability_mismatch) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(13);
    d.backend = "tpu";
    d.dtype = "bf16";
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().reason == ReasonCode::RejectCapability);
}

AF_TEST(engine_rejects_malformed_descriptor) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(14);
    d.request_id = RequestId(0);
    d.expected_output_tokens = 5;
    d.max_output_tokens = 8;
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().reason == ReasonCode::RejectMalformed);
}

AF_TEST(engine_rejects_quota_token_budget) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    QuotaSnapshot q(true);
    q.generation = 1;
    q.timestamp = SystemClock::now();
    q.limits.set(ResourceId::AcceleratorMemory, ByteCount{4} << 30);
    q.token_budget = 1000;
    f.set_quota(q);
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(15);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().reason == ReasonCode::RejectQuota);
}

AF_TEST(engine_rejects_slo_infeasible) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(16);
    d.expected_output_tokens = 10000;
    d.max_output_tokens = 10000;
    d.deadline = Milliseconds(50);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().reason == ReasonCode::RejectInfeasible);
}

AF_TEST(engine_policy_versioning) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionPolicy p2 = default_policy();
    p2.name = "v2";
    p2.safety_headroom_ratio = 0.5;
    Status s = f.install_policy(p2);
    AF_CHECK(s.ok());
    AF_CHECK(f.policies().generation_count() == 2);
    AF_CHECK(f.policies().latest()->name == "v2");
    AF_CHECK(f.policy() != nullptr);
}

AF_TEST(engine_duplicate_request_rejected) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(17);
    auto r1 = f.evaluate(d);
    AF_CHECK(r1.has_value());
    AF_CHECK(r1.value().is_admit());
    auto r2 = f.evaluate(d);
    AF_CHECK(r2.has_value());
    AF_CHECK(r2.value().is_reject());
}

AF_TEST(engine_cancel_rolls_back_reservation) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(18);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().is_admit());
    AF_CHECK(f.reservations().reserved().any_nonzero());
    Status cs = f.cancel_request(d.admission_request_id);
    AF_CHECK(cs.ok());
    AF_CHECK(f.reservations().reserved().is_zero());
}

AF_TEST(engine_stale_commit_generation_rejected) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(19);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().is_admit());
    AdmissionGeneration wrong = r.value().admission_generation.next();
    Status cs = f.commit_admission(d.admission_request_id, wrong);
    AF_CHECK(!cs.ok());
    AF_CHECK(cs.code() == ErrorCode::StaleAdmissionGeneration);
}

AF_TEST(engine_stale_snapshot_rejected) {
    AdmissionFabric f = make_engine(ByteCount{8} << 30, cuda_caps());
    // Age out the snapshot beyond policy tolerance.
    ResourceSnapshot s = f.snapshot();
    s.timestamp = SystemClock::now() - Seconds(60);
    f.set_snapshot(s);
    AdmissionDescriptor d = make_desc(ModelId(7), ModelRevision(1));
    d.admission_request_id = AdmissionRequestId(20);
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().reason == ReasonCode::RejectStale);
}

AF_TEST_MAIN("admission-fabric-engine");
