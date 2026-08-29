#include "test_util.hpp"
#include "admission_fabric/engine.hpp"
#include "admission_fabric/persistence.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"

#include <filesystem>
#include <fstream>

using namespace admission_fabric;

static AdmissionPolicy pol() {
    AdmissionPolicy p;
    p.name = "recovery";
    p.max_snapshot_age = Seconds(5);
    p.require_reservation = true;
    p.max_reservation_lifetime = Seconds(120);
    return p;
}

static AdmissionDescriptor desc(AdmissionRequestId id) {
    AdmissionDescriptor d;
    d.admission_request_id = id;
    d.request_id = RequestId(id.value() * 2 + 1);
    d.attempt_id = AttemptId(1);
    d.tenant_id = TenantId(1);
    d.model_id = ModelId(7);
    d.model_revision = ModelRevision(1);
    d.policy_generation = PolicyGeneration(1);
    d.input_tokens = 1000;
    d.expected_output_tokens = 1000;
    d.max_output_tokens = 1000;
    d.backend = "cuda";
    d.dtype = "fp16";
    d.latency_class = LatencyClass::BestEffort;
    return d;
}

static PredictionProfile prof() {
    PredictionProfile p;
    p.model_id = ModelId(7); p.revision = ModelRevision(1);
    p.backend = "cuda"; p.dtype = "fp16"; p.quantization = "none";
    p.fixed_base_memory = ByteCount{1} << 30;
    p.per_token_kv_bytes = 1024;
    p.per_token_tensor_bytes = 1024;
    p.per_request_overhead = ByteCount{4} << 20;
    p.workspace_bytes = ByteCount{32} << 20;
    p.staging_bytes = ByteCount{64} << 20;
    p.pinned_staging_bytes = ByteCount{16} << 20;
    p.prefill_tokens_per_sec = 1000.0;
    p.decode_tokens_per_sec = 50.0;
    p.execution_slots_required = 1;
    p.sequence_slots_required = 1;
    return p;
}

static void setup(AdmissionFabric& f) {
    AF_CHECK(f.install_policy(pol()).ok());
    AF_CHECK(f.register_profile(prof()));
    ResourceSnapshot s;
    s.generation = ResourceSnapshotGeneration(1);
    s.timestamp = SystemClock::now();
    s.agent_id = AgentId(1);
    s.boot_id = AgentBootId(1);
    s.node_name = "node-a";
    s.total.set(ResourceId::AcceleratorMemory, ByteCount{8} << 30);
    s.total.set(ResourceId::HostMemory, ByteCount{4} << 30);
    s.total.set(ResourceId::PinnedHostMemory, ByteCount{1} << 30);
    s.total.set(ResourceId::ExecutionSlots, 64);
    s.total.set(ResourceId::SequenceSlots, 256);
    s.capabilities.add(Capabilities::backend("cuda"));
    s.capabilities.add(Capabilities::dtype("fp16"));
    for (auto& p : s.pressure) p = PressureLevel::Low;
    for (auto& p : s.provenance) p = Provenance::Measured;
    f.set_snapshot(s);
}

AF_TEST(persistence_roundtrip_recovery) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / "af_state_recovery.bin";
    std::filesystem::remove(p);

    // Build authoritative state with one committed and one reserved request.
    AdmissionFabric f;
    setup(f);
    auto d1 = desc(AdmissionRequestId(100)); d1.request_id = RequestId(1001);
    auto r1 = f.evaluate(d1);
    AF_CHECK(r1.has_value());
    AF_CHECK(r1.value().is_admit());
    AF_CHECK(f.commit_admission(AdmissionRequestId(100), r1.value().admission_generation).ok());
    auto d2 = desc(AdmissionRequestId(200)); d2.request_id = RequestId(2001);
    auto r2 = f.evaluate(d2);
    AF_CHECK(r2.has_value());
    AF_CHECK(r2.value().is_admit());
    // r2 stays reserved (provisional).

    StateStore store;
    PersistenceConfig cfg; cfg.path = p;
    Status s = store.save(cfg, f);
    AF_CHECK(s.ok());

    // Recover into a fresh engine.
    AdmissionFabric g;
    Status ls = store.load(cfg, g);
    AF_CHECK(ls.ok());
    AF_CHECK(g.request(AdmissionRequestId(100)) != nullptr);
    AF_CHECK(g.request(AdmissionRequestId(100))->lifecycle == RequestLifecycle::Admitted);
    AF_CHECK(g.request(AdmissionRequestId(200)) != nullptr);
    AF_CHECK(g.request(AdmissionRequestId(200))->lifecycle == RequestLifecycle::Reserved);
    AF_CHECK(g.policies().generation_count() == 1);
    AF_CHECK(g.reservations().verify_accounting());
    // Request 100 is committed (used), request 200 reserved.
    AF_CHECK(!g.reservations().used().is_zero());
    AF_CHECK(!g.reservations().reserved().is_zero());
    std::filesystem::remove(p);
}

AF_TEST(persistence_rejects_corruption) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / "af_state_corrupt.bin";
    std::filesystem::remove(p);
    AdmissionFabric f;
    setup(f);
    auto d1 = desc(AdmissionRequestId(300)); d1.request_id = RequestId(3001);
    auto r1 = f.evaluate(d1);
    AF_CHECK(r1.has_value());
    StateStore store;
    PersistenceConfig cfg; cfg.path = p;
    AF_CHECK(store.save(cfg, f).ok());

    // Corrupt a byte in the middle.
    {
        std::vector<std::uint8_t> bytes;
        { std::ifstream is(p, std::ios::binary); bytes.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>()); }
        AF_CHECK(bytes.size() > 32);
        bytes[bytes.size() / 2] ^= 0xFF;
        { std::ofstream os(p, std::ios::binary | std::ios::trunc); os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); os.close(); }
    }
    AdmissionFabric g;
    Status ls = store.load(cfg, g);
    AF_CHECK(!ls.ok());
    std::filesystem::remove(p);
}

AF_TEST_MAIN("admission-fabric-recovery");
