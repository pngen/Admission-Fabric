#include "admission_fabric/engine.hpp"
#include "admission_fabric/decision.hpp"
#include "admission_fabric/observation.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/prediction.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/slo.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace admission_fabric;

static std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

static AdmissionPolicy default_policy() {
    AdmissionPolicy p;
    p.name = "default";
    p.max_snapshot_age = Seconds(30);
    return p;
}

static AdmissionFabric make_local_engine() {
    AdmissionFabric f;
    Status sp = f.install_policy(default_policy());
    (void)sp;
    ResourceSnapshot s;
    s.generation = ResourceSnapshotGeneration(1);
    s.timestamp = SystemClock::now();
    s.agent_id = AgentId(1);
    s.boot_id = AgentBootId(1);
    s.node_name = "local";
    s.total.set(ResourceId::AcceleratorMemory, ByteCount{24} << 30);
    s.total.set(ResourceId::HostMemory, ByteCount{16} << 30);
    s.total.set(ResourceId::PinnedHostMemory, ByteCount{2} << 30);
    s.total.set(ResourceId::ExecutionSlots, 128);
    s.total.set(ResourceId::SequenceSlots, 512);
    s.capabilities.add(Capabilities::backend("cuda"));
    s.capabilities.add(Capabilities::dtype("fp16"));
    s.capabilities.add(Capabilities::compute_capability("12.0"));
    for (auto& p : s.pressure) p = PressureLevel::Low;
    for (auto& p : s.provenance) p = Provenance::Measured;
    f.set_snapshot(s);
    return f;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: admission-fabricctl <command> [options]\n");
        std::printf("commands: evaluate admit release list-requests list-reservations show-capacity\n");
        std::printf("          show-policy validate-policy explain observe benchmark validate\n");
        return 2;
    }
    std::string cmd = argv[1];
    std::map<std::string, std::string> kv;
    for (int i = 2; i + 1 < argc; i += 2) kv[argv[i]] = argv[i + 1];
    auto get = [&](const std::string& k, const std::string& def = std::string()) -> std::string {
        auto it = kv.find(k); return it == kv.end() ? def : it->second;
    };
    auto get_ull = [&](const std::string& k, std::uint64_t def) -> std::uint64_t {
        auto it = kv.find(k); return it == kv.end() ? def : std::strtoull(it->second.c_str(), nullptr, 10);
    };
    AdmissionFabric f = make_local_engine();

    if (cmd == "evaluate") {
        AdmissionDescriptor d;
        d.admission_request_id = AdmissionRequestId(get_ull("--request-id", 1));
        d.request_id = RequestId(get_ull("--request-id", 1) + 100000);
        d.attempt_id = AttemptId(1);
        d.tenant_id = TenantId(get_ull("--tenant", 1));
        d.model_id = ModelId(get_ull("--model", 7));
        d.model_revision = ModelRevision(get_ull("--revision", 1));
        d.policy_generation = PolicyGeneration(1);
        d.input_tokens = get_ull("--input-tokens", 1024);
        d.expected_output_tokens = get_ull("--output-tokens", 1024);
        d.max_output_tokens = get_ull("--max-output-tokens", d.expected_output_tokens);
        d.backend = get("--backend", "cuda");
        d.dtype = get("--dtype", "fp16");
        d.latency_class = LatencyClass::BestEffort;
        if (kv.count("--deadline-ms")) d.deadline = Milliseconds(get_ull("--deadline-ms", 0));
        auto r = f.evaluate(d);
        std::printf("{\n");
        if (r.has_value()) {
            const AdmissionDecision& dec = r.value();
            std::printf("  \"admission_request\": %s,\n", dec.admission_request_id.str().c_str());
            std::printf("  \"outcome\": %s,\n", json_str(outcome_name(dec.outcome)).c_str());
            std::printf("  \"reason\": %s,\n", json_str(reason_code_name(dec.reason)).c_str());
            std::printf("  \"confidence\": %.3f,\n", dec.confidence);
            std::printf("  \"predicted_peak_vram_bytes\": %llu,\n", static_cast<unsigned long long>(dec.memory_prediction.footprint.peak()));
            std::printf("  \"predicted_duration_ms\": %lld,\n", static_cast<long long>(dec.duration_prediction.expected.count() / 1000000));
            std::printf("  \"slo\": %s,\n", json_str(slo_verdict_name(dec.slo.verdict)).c_str());
            if (dec.reservation_id.has_value()) std::printf("  \"reservation\": %s,\n", dec.reservation_id->str().c_str());
            std::printf("  \"explanation\": %s\n", json_str(dec.explanation).c_str());
        } else {
            std::printf("  \"error\": %s\n", json_str(r.message()).c_str());
        }
        std::printf("}\n");
        return r.has_value() ? 0 : 1;
    }
    if (cmd == "admit") {
        AdmissionRequestId id(get_ull("--request-id", 0));
        const RequestRecord* rec = f.request(id);
        if (!rec || !rec->last_decision.has_value()) { std::printf("{\"error\": \"no decision\"}\n"); return 1; }
        Status s = f.commit_admission(id, rec->last_decision->admission_generation);
        std::printf("{\"admit\": %s}\n", s.ok() ? "true" : "false");
        return s.ok() ? 0 : 1;
    }
    if (cmd == "release") {
        Status s = f.release_reservation(ReservationId(get_ull("--reservation-id", 0)));
        std::printf("{\"release\": %s}\n", s.ok() ? "true" : "false");
        return s.ok() ? 0 : 1;
    }
    if (cmd == "show-capacity") {
        std::printf("{\"generation\": %llu, \"total_vram\": %llu, \"available_vram\": %llu, \"reserved_vram\": %llu, \"used_vram\": %llu}\n",
            static_cast<unsigned long long>(f.snapshot().generation.value()),
            static_cast<unsigned long long>(f.snapshot().total.get(ResourceId::AcceleratorMemory)),
            static_cast<unsigned long long>(f.reservations().availability().get(ResourceId::AcceleratorMemory)),
            static_cast<unsigned long long>(f.reservations().reserved().get(ResourceId::AcceleratorMemory)),
            static_cast<unsigned long long>(f.reservations().used().get(ResourceId::AcceleratorMemory)));
        return 0;
    }
    if (cmd == "list-requests") {
        std::printf("{\"requests\": [\n");
        bool first = true;
        for (auto& [id, rec] : f.requests()) { if (!first) std::printf(",\n"); first = false;
            std::printf("  {\"id\": %s, \"lifecycle\": %s}", id.str().c_str(), json_str(lifecycle_name(rec.lifecycle)).c_str()); }
        std::printf("\n]}\n");
        return 0;
    }
    if (cmd == "list-reservations") {
        std::printf("{\"reservations\": [\n");
        bool first = true;
        for (auto& [id, r] : f.reservations().reservations()) { if (!first) std::printf(",\n"); first = false;
            std::printf("  {\"id\": %s, \"status\": %s, \"vram\": %llu}", id.str().c_str(), json_str(reservation_status_name(r.status)).c_str(),
                static_cast<unsigned long long>(r.demand.get(ResourceId::AcceleratorMemory))); }
        std::printf("\n]}\n");
        return 0;
    }
    if (cmd == "show-policy") {
        const AdmissionPolicy* p = f.policy();
        if (!p) { std::printf("{\"error\": \"no policy\"}\n"); return 1; }
        std::printf("{\"generation\": %llu, \"name\": %s, \"safety_headroom_ratio\": %.3f}\n",
            static_cast<unsigned long long>(p->generation.value()), json_str(p->name).c_str(), p->safety_headroom_ratio);
        return 0;
    }
    if (cmd == "validate-policy") {
        Status s = f.install_policy(default_policy());
        std::printf("{\"valid\": %s, \"message\": %s}\n", s.ok() ? "true" : "false", json_str(s.message()).c_str());
        return s.ok() ? 0 : 1;
    }
    if (cmd == "explain") {
        const RequestRecord* rec = f.request(AdmissionRequestId(get_ull("--request-id", 0)));
        if (!rec || !rec->last_decision.has_value()) { std::printf("{\"error\": \"no decision\"}\n"); return 1; }
        std::printf("%s\n", f.explain(*rec->last_decision).c_str());
        return 0;
    }
    if (cmd == "observe") {
        Observation o;
        o.id = ObservationId(get_ull("--observation-id", 1));
        o.admission_request_id = AdmissionRequestId(get_ull("--request-id", 1));
        o.model_id = ModelId(get_ull("--model", 7));
        o.model_revision = ModelRevision(get_ull("--revision", 1));
        o.backend = get("--backend", "cuda");
        o.peak_vram = get_ull("--peak-vram", 0);
        o.execution_duration = Milliseconds(get_ull("--duration-ms", 1));
        o.input_tokens = get_ull("--input-tokens", 1024);
        o.output_tokens = get_ull("--output-tokens", 1024);
        Status s = o.validate();
        if (!s.ok()) { std::printf("{\"error\": %s}\n", json_str(s.message()).c_str()); return 1; }
        f.observe(o);
        std::printf("{\"observed\": true}\n");
        return 0;
    }
    if (cmd == "benchmark") {
        std::uint64_t n = get_ull("--iterations", 10000);
        auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < n; ++i) {
            AdmissionDescriptor d;
            d.admission_request_id = AdmissionRequestId(i + 1);
            d.request_id = RequestId(i + 1);
            d.attempt_id = AttemptId(1);
            d.tenant_id = TenantId(1);
            d.model_id = ModelId(7);
            d.model_revision = ModelRevision(1);
            d.policy_generation = PolicyGeneration(1);
            d.input_tokens = 1000;
            d.expected_output_tokens = 1000;
            d.max_output_tokens = 1000;
            d.backend = "cuda"; d.dtype = "fp16";
            d.latency_class = LatencyClass::BestEffort;
            auto rr = f.evaluate(d);
            (void)rr;
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
        std::printf("{\"iterations\": %llu, \"elapsed_ms\": %.2f, \"ops_per_sec\": %.0f}\n",
            static_cast<unsigned long long>(n), ms, (n * 1000.0) / (ms > 0 ? ms : 1.0));
        return 0;
    }
    if (cmd == "validate" || cmd == "defer" || cmd == "show-request" || cmd == "show-reservation" || cmd == "show-agents") {
        std::printf("{\"note\": %s}\n", json_str("command available via library API").c_str());
        return 0;
    }
    std::printf("{\"error\": \"unknown command\"}\n");
    return 2;
}
