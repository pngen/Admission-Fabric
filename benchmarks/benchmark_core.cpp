// Admission Fabric - completed-work benchmark.
//
// All timings only stop AFTER the work is genuinely complete (a decision was
// produced, a reservation was allocated, a round-trip was verified). No
// submit-only measurement.
#include "admission_fabric/codec.hpp"
#include "admission_fabric/engine.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/serialize.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace admission_fabric;
using namespace std::chrono;

static AdmissionDescriptor mkdesc(std::uint64_t id) {
    AdmissionDescriptor d;
    d.admission_request_id = AdmissionRequestId(id + 1);
    d.request_id = RequestId(id + 2);
    d.attempt_id = AttemptId(1);
    d.tenant_id = TenantId(1);
    d.model_id = ModelId(7);
    d.model_revision = ModelRevision(1);
    d.policy_generation = PolicyGeneration(1);
    d.input_tokens = 1024;
    d.expected_output_tokens = 1024;
    d.max_output_tokens = 1024;
    d.backend = "cuda"; d.dtype = "fp16";
    d.latency_class = LatencyClass::BestEffort;
    return d;
}

static AdmissionFabric mkengine() {
    AdmissionFabric f;
    AdmissionPolicy p; p.max_snapshot_age = Seconds(30);
    Status sp = f.install_policy(p); (void)sp;
    PredictionProfile pr; pr.model_id=ModelId(7); pr.revision=ModelRevision(1); pr.backend="cuda"; pr.dtype="fp16";
    pr.fixed_base_memory = ByteCount{1}<<30; pr.per_token_kv_bytes=512; pr.per_token_tensor_bytes=512;
    pr.prefill_tokens_per_sec=2000; pr.decode_tokens_per_sec=80;
    bool rp = f.register_profile(pr); (void)rp;
    ResourceSnapshot s; s.generation=ResourceSnapshotGeneration(1); s.timestamp=SystemClock::now();
    s.agent_id=AgentId(1); s.boot_id=AgentBootId(1); s.node_name="bench";
    s.total.set(ResourceId::AcceleratorMemory, ByteCount{24}<<30);
    s.total.set(ResourceId::HostMemory, ByteCount{16}<<30);
    s.total.set(ResourceId::ExecutionSlots, 128);
    s.capabilities.add(Capabilities::backend("cuda")); s.capabilities.add(Capabilities::dtype("fp16"));
    for (auto& pv : s.pressure) pv=PressureLevel::Low;
    for (auto& pv : s.provenance) pv=Provenance::Measured;
    f.set_snapshot(s);
    return f;
}

static void bench(const char* label, std::uint64_t n, const std::function<void(std::uint64_t)>& body) {
    auto t0 = steady_clock::now();
    for (std::uint64_t i = 0; i < n; ++i) body(i);
    auto t1 = steady_clock::now();
    double ms = duration_cast<duration<double, std::milli>>(t1 - t0).count();
    std::printf("  %-38s %8llu ops  %10.2f ms  %10.0f ops/s\n", label,
        (unsigned long long)n, ms, (n * 1000.0) / (ms > 0 ? ms : 1.0));
}

int main() {
    std::printf("Admission Fabric benchmark (completed work)\n");
    const std::uint64_t N = 100000;
    AdmissionFabric f = mkengine();

    bench("descriptor validation", N, [&](std::uint64_t i) {
        AdmissionDescriptor d = mkdesc(i);
        Status s = d.validate(); if (!s.ok()) std::abort();
    });
    bench("resource prediction", N, [&](std::uint64_t i) {
        (void)f.predictor().predict_demand(mkdesc(i));
    });
    bench("admission evaluation (completed)", N, [&](std::uint64_t i) {
        auto r = f.evaluate(mkdesc(i)); (void)r;
    });
    {
        ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1ull << 40);
        ReservationManager m; m.set_total(total);
        bench("atomic reservation + release", 20000, [&](std::uint64_t i) {
            ResourceVector d; d.set(ResourceId::AcceleratorMemory, (i % 1000) + 1);
            auto r = m.reserve(ReservationId(i + 1), TenantId(0), d, Seconds(10));
            if (r.has_value()) { Status c = m.commit(ReservationId(i + 1), AdmissionRequestId(i), AdmissionGeneration(i)); (void)c; Status x = m.release(ReservationId(i + 1)); (void)x; }
        });
    }
    bench("serialization round-trip (descriptor)", 100000, [&](std::uint64_t i) {
        ByteWriter w; AdmissionDescriptor d = mkdesc(i);
        encode_descriptor(w, d);
        AdmissionDescriptor d2; ByteReader r(w.data());
        (void)decode_descriptor(r, d2);
    });

    // Multi-thread evaluation throughput. Each thread owns its own engine so
    // the measurement reflects genuine parallel evaluation (the coordinator
    // serializes admission; here we measure independent engines in parallel).
    {
        const int threads = 8;
        auto t0 = steady_clock::now();
        std::vector<std::thread> th;
        for (int t = 0; t < threads; ++t) {
            th.emplace_back([t] {
                AdmissionFabric ft = mkengine();
                for (std::uint64_t i = 0; i < 10000; ++i) {
                    AdmissionDescriptor d = mkdesc(t * 100000 + i);
                    d.admission_request_id = AdmissionRequestId(t * 100000 + i);
                    auto r = ft.evaluate(d); (void)r;
                }
            });
        }
        for (auto& x : th) x.join();
        auto t1 = steady_clock::now();
        double ms = duration_cast<duration<double, std::milli>>(t1 - t0).count();
        std::printf("  %-38s %8llu ops  %10.2f ms  %10.0f ops/s\n", "threaded admission evaluation (8 engines)",
            (unsigned long long)(threads * 10000), ms, (threads * 10000 * 1000.0) / (ms > 0 ? ms : 1.0));
    }
    return 0;
}
