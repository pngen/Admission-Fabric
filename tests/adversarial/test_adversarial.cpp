#include "test_util.hpp"
#include "admission_fabric/engine.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"
using namespace admission_fabric;
static AdmissionPolicy ap() { AdmissionPolicy p; p.require_reservation=true; p.max_snapshot_age=Seconds(5); return p; }
static AdmissionFabric eng(ByteCount vram) {
    AdmissionFabric f;
    Status s = f.install_policy(ap()); (void)s;
    if (vram > 0) {
        ResourceSnapshot snap;
        snap.generation=ResourceSnapshotGeneration(1); snap.timestamp=SystemClock::now();
        snap.agent_id=AgentId(1); snap.boot_id=AgentBootId(1); snap.node_name="n";
        snap.total.set(ResourceId::AcceleratorMemory, vram);
        snap.total.set(ResourceId::HostMemory, ByteCount{4}<<30);
        snap.total.set(ResourceId::ExecutionSlots, 64);
        snap.capabilities.add(Capabilities::backend("cuda"));
        for (auto& p : snap.pressure) p=PressureLevel::Low;
        for (auto& p : snap.provenance) p=Provenance::Measured;
        f.set_snapshot(snap);
    }
    return f;
}
static AdmissionDescriptor adv(AdmissionRequestId id, TokenCount tok) {
    AdmissionDescriptor d;
    d.admission_request_id=id; d.request_id=RequestId(id.value()); d.attempt_id=AttemptId(1);
    d.tenant_id=TenantId(1); d.model_id=ModelId(7); d.model_revision=ModelRevision(1);
    d.policy_generation=PolicyGeneration(1);
    d.input_tokens=tok; d.expected_output_tokens=tok; d.max_output_tokens=tok;
    d.backend="cuda"; d.dtype="fp16"; d.latency_class=LatencyClass::BestEffort;
    return d;
}
AF_TEST(adversarial_zero_capacity) {
    AdmissionFabric f = eng(0);
    auto r = f.evaluate(adv(AdmissionRequestId(1), 100));
    AF_CHECK(r.has_value());
    AF_CHECK(!r.value().is_admit());
}
AF_TEST(adversarial_nearly_full_and_oversize) {
    AdmissionFabric f = eng(ByteCount{8} << 30);
    // Tiny flood.
    for (std::uint64_t i=1;i<=50;++i) {
        auto r = f.evaluate(adv(AdmissionRequestId(i), 10));
        AF_CHECK(r.has_value());
    }
    // Enormous request.
    auto rbig = f.evaluate(adv(AdmissionRequestId(9999), 1u << 30));
    AF_CHECK(rbig.has_value());
    AF_CHECK(rbig.value().is_defer() || rbig.value().is_reject());
}
AF_TEST(adversarial_duplicate_and_malformed_ids) {
    AdmissionFabric f = eng(ByteCount{8} << 30);
    AdmissionDescriptor d = adv(AdmissionRequestId(100), 1000);
    d.request_id = RequestId(0); // malformed
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().is_reject());
}
AF_TEST(adversarial_expired_deadline) {
    AdmissionFabric f = eng(ByteCount{8} << 30);
    AdmissionDescriptor d = adv(AdmissionRequestId(200), 100000);
    d.deadline = Milliseconds(1);  // infeasible
    auto r = f.evaluate(d);
    AF_CHECK(r.has_value());
    AF_CHECK(r.value().is_reject());
}
AF_TEST_MAIN("admission-fabric-adversarial");
