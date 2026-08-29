#pragma once

// Admission Fabric - resource and duration prediction.
//
// The predictor is bounded and evidence-driven. It never invents certainty:
// every prediction carries a confidence, an evidence count, a predictor
// generation, and a provenance. Cold start is explicit (low confidence,
// fallback flags). Observations feed back to adjust the estimate so that
// repeated under-prediction of scarce capacity increases the reserved margin.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/observation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

enum class PredictionProvenance : std::uint8_t {
    ModelProfile = 0,
    Historical = 1,
    Hybrid = 2,
    Fallback = 3,
    Unavailable = 4
};
inline const char* provenance_name(PredictionProvenance p) {
    switch (p) { case PredictionProvenance::ModelProfile: return "model_profile"; case PredictionProvenance::Historical: return "historical";
        case PredictionProvenance::Hybrid: return "hybrid"; case PredictionProvenance::Fallback: return "fallback";
        case PredictionProvenance::Unavailable: return "unavailable"; }
    return "unknown";
}

template <typename T>
struct Prediction {
    T value{};
    double confidence{0.0};
    std::uint32_t evidence_count{0};
    std::uint64_t predictor_generation{0};
    PredictionProvenance provenance{PredictionProvenance::Unavailable};
    bool fallback{false};
};

// Decomposed memory footprint. peak() is the conservative upper bound used to
// judge admission; the components make the estimate explainable.
struct MemoryFootprint {
    ByteCount fixed_model{0};
    ByteCount request_local{0};
    ByteCount kv_growth{0};
    ByteCount tensor_state{0};
    ByteCount workspace{0};
    ByteCount staging{0};
    ByteCount pinned_staging{0};
    ByteCount safety_margin{0};
    ByteCount fragmentation_allowance{0};

    [[nodiscard]] ByteCount base() const {
        return fixed_model + request_local + kv_growth + tensor_state + workspace + staging;
    }
    [[nodiscard]] ByteCount peak() const {
        return base() + safety_margin + fragmentation_allowance;
    }
    [[nodiscard]] ByteCount sustained() const {
        return fixed_model + tensor_state + staging;
    }
};

struct MemoryFootprintPrediction {
    MemoryFootprint footprint;
    double confidence{0.0};
    std::uint32_t evidence_count{0};
    std::uint64_t predictor_generation{0};
    PredictionProvenance provenance{PredictionProvenance::Unavailable};
    bool fallback{false};
};

struct DurationPrediction {
    Nanoseconds expected{0};
    double confidence{0.0};
    Nanoseconds prefill{0};
    Nanoseconds decode{0};
    Nanoseconds transfer{0};
    double decode_rate_tps{0.0};
    std::uint32_t evidence_count{0};
    std::uint64_t predictor_generation{0};
    PredictionProvenance provenance{PredictionProvenance::Unavailable};
    bool fallback{false};
};

// A per-model/backend prediction profile. The profile is configured evidence
// (not measured truth); observations refine it and separate runtime estimates
// remain bounded by policy.
struct PredictionProfile {
    ModelId model_id;
    ModelRevision revision;
    std::string backend;       // "cuda" / "cpu" / ...
    std::string dtype;         // "fp16" / "fp32" / ...
    std::string quantization;  // "int8" / "none" / ...
    std::uint64_t generation{0};

    ByteCount fixed_base_memory{0};
    ByteCount per_token_kv_bytes{0};
    ByteCount per_token_tensor_bytes{0};
    ByteCount per_request_overhead{0};
    ByteCount workspace_bytes{0};
    ByteCount staging_bytes{0};
    ByteCount pinned_staging_bytes{0};
    double safety_margin_ratio{0.10};
    double fragmentation_ratio{0.05};

    double prefill_tokens_per_sec{0.0};
    double decode_tokens_per_sec{0.0};
    ByteCount transfer_bytes_per_sequence{0};
    double transfer_bytes_per_sec{0.0};
    ByteCount host_memory_bytes{0};
    ByteCount pinned_host_memory_bytes{0};
    SlotCount execution_slots_required{1};
    SlotCount sequence_slots_required{1};
};

// Bounded, evidence-driven predictor.
class WorkloadPredictor {
public:
    static constexpr std::size_t kMaxProfiles = 256;

    // Register (or replace) a profile. Returns false if the profile table is
    // full and the key is unknown.
    bool register_profile(const PredictionProfile& prof) {
        std::string key = profile_key(prof);
        if (!seen_[key]) {
            if (profs_.size() >= kMaxProfiles) return false;
            seen_[key] = true;
        }
        profs_[key] = prof;
        profs_[key].generation = next_generation_();
        return true;
    }

    [[nodiscard]] const std::map<std::string, PredictionProfile>& profiles() const { return profs_; }
    [[nodiscard]] std::size_t profile_count() const { return profs_.size(); }

    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    // Predict the decomposed memory footprint for a descriptor.
    [[nodiscard]] MemoryFootprintPrediction predict_memory(const AdmissionDescriptor& d) const {
        MemoryFootprintPrediction out;
        std::string key = resolve_key(profile_key(d), model_rev_prefix(d));
        p_provenance_snapshot(key, out.provenance, out.confidence, out.evidence_count, out.fallback);
        out.predictor_generation = generation_;

        const PredictionProfile* prof = profile_for_descriptor(d);
        MemoryFootprint fp;
        if (prof != nullptr && !prof->backend.empty()) {
            TokenCount total = d.input_tokens + d.expected_output_tokens;
            if (total == 0) total = d.input_tokens + 1;
            fp.fixed_model = prof->fixed_base_memory;
            fp.request_local = prof->per_request_overhead;
            fp.kv_growth = multiply_checked(prof->per_token_kv_bytes, (d.input_tokens + d.expected_output_tokens));
            fp.tensor_state = multiply_checked(prof->per_token_tensor_bytes, (d.input_tokens + (d.max_output_tokens ? d.max_output_tokens : d.expected_output_tokens)));
            fp.workspace = prof->workspace_bytes;
            fp.staging = prof->staging_bytes;
            fp.pinned_staging = prof->pinned_staging_bytes;
            ByteCount base = fp.base();
            fp.safety_margin = rat_of(base, prof->safety_margin_ratio);
            fp.fragmentation_allowance = rat_of(base, prof->fragmentation_ratio);
            // Apply learned adjustment to the conservative part (fixed model is
            // authoritative, not adjusted).
            out.footprint = fp;
            apply_learned_adjustment(key, out.footprint);
            // Confidence from evidence count and profile presence.
            record_evidence_for_profile(key, out.evidence_count, out.confidence);
        } else {
            // Cold start: no profile. Fall back to a bounded generic estimate.
            out.fallback = true;
            out.confidence = (std::min)(0.30, 0.10 + 0.20 * (out.evidence_count > 0 ? 1 : 0));
            if (out.provenance == PredictionProvenance::Unavailable)
                out.provenance = PredictionProvenance::Fallback;
            // Generic: assume 8 bytes/token KV + tensor plus a base 256 MiB.
            TokenCount total = d.input_tokens + (d.max_output_tokens ? d.max_output_tokens : d.expected_output_tokens);
            if (total == 0) total = 1;
            fp.fixed_model = ByteCount{256} * 1024 * 1024;
            fp.kv_growth = multiply_checked(8, total);
            fp.tensor_state = multiply_checked(8, total);
            fp.staging = ByteCount{64} * 1024 * 1024;
            fp.safety_margin = rat_of(fp.base(), 0.15);
            fp.fragmentation_allowance = rat_of(fp.base(), 0.08);
            out.footprint = fp;
        }
        return out;
    }

    // Predict expected duration for a descriptor.
    [[nodiscard]] DurationPrediction predict_duration(const AdmissionDescriptor& d) const {
        DurationPrediction out;
        std::string key = resolve_key(profile_key(d), model_rev_prefix(d));
        p_provenance_snapshot(key, out.provenance, out.confidence, out.evidence_count, out.fallback);
        out.predictor_generation = generation_;
        const PredictionProfile* prof = profile_for_descriptor(d);
        if (prof != nullptr && !prof->backend.empty()) {
            double prefill_s = d.input_tokens > 0 && prof->prefill_tokens_per_sec > 0
                ? static_cast<double>(d.input_tokens) / prof->prefill_tokens_per_sec : 0.0;
            double decode_eff = effective_decode_rate(key, prof->decode_tokens_per_sec);
            double decode_s = d.expected_output_tokens > 0 && decode_eff > 0
                ? static_cast<double>(d.expected_output_tokens) / decode_eff : 0.0;
            double transfer_s = prof->transfer_bytes_per_sequence > 0 && prof->transfer_bytes_per_sec > 0
                ? static_cast<double>(prof->transfer_bytes_per_sequence) / prof->transfer_bytes_per_sec : 0.0;
            out.prefill = from_seconds(prefill_s);
            out.decode = from_seconds(decode_s);
            out.transfer = from_seconds(transfer_s);
            out.expected = out.prefill + out.decode + out.transfer;
            out.decode_rate_tps = decode_eff;
            record_evidence_for_profile(key, out.evidence_count, out.confidence);
        } else {
            out.fallback = true;
            // Generic conservative: 40 tps decode, 1000 tps prefill.
            double prefill_s = d.input_tokens > 0 ? static_cast<double>(d.input_tokens) / 1000.0 : 0.0;
            double decode_s = d.expected_output_tokens > 0 ? static_cast<double>(d.expected_output_tokens) / 40.0 : 0.0;
            out.prefill = from_seconds(prefill_s);
            out.decode = from_seconds(decode_s);
            out.expected = out.prefill + out.decode;
            out.confidence = 0.15;
        }
        return out;
    }

    // Aggregate peak demand (across all resource dimensions) for a descriptor.
    [[nodiscard]] ResourceVector predict_demand(const AdmissionDescriptor& d) const {
        auto mem = predict_memory(d);
        ResourceVector r;
        r.set(ResourceId::AcceleratorMemory, mem.footprint.peak());
        r.set(ResourceId::HostMemory, mem.footprint.sustained());
        r.set(ResourceId::PinnedHostMemory, mem.footprint.pinned_staging);
        r.set(ResourceId::KvCapacity, mem.footprint.kv_growth);
        r.set(ResourceId::TensorState, mem.footprint.tensor_state);
        std::string key = resolve_key(profile_key(d), model_rev_prefix(d));
        const PredictionProfile* prof = profile_for_descriptor(d);
        if (prof != nullptr && !prof->backend.empty()) {
            r.set(ResourceId::ExecutionSlots, prof->execution_slots_required);
            r.set(ResourceId::SequenceSlots, prof->sequence_slots_required);
            r.set(ResourceId::PersistentCache, multiply_checked(prof->transfer_bytes_per_sequence, 1));
            if (prof->host_memory_bytes) r.set(ResourceId::HostMemory, (std::max)(r.get(ResourceId::HostMemory), prof->host_memory_bytes));
        } else {
            r.set(ResourceId::ExecutionSlots, 1);
            r.set(ResourceId::SequenceSlots, 1);
        }
        return r;
    }

    // Feed a completed observation back into the bounded predictor state.
    void observe(const Observation& obs) {
        if (!obs.validate().ok()) return;  // never let malformed data corrupt history
        std::string key = resolve_key(profile_key(obs), model_rev_prefix(obs));
        LearnedState& st = learn_[key];
        st.seen = true;
        st.count = (st.count >= kMaxSamplesPerKey) ? kMaxSamplesPerKey : st.count + 1;
        if (obs.kind != ObservationKind::Completed) return;
        st.last_duration_ns = static_cast<double>(obs.execution_duration.count());
        st.last_output_tokens = obs.output_tokens;
        st.last_peak_vram = obs.peak_vram;
        st.last_kv = obs.kv_growth;
        // Rolling expectation of peak-vram / predicted-memory ratio is handled
        // by the profile delta below. Here we track the raw peaks.
        st.max_observed_peak = (std::max)(st.max_observed_peak, obs.peak_vram);
        if (obs.execution_duration.count() > 0 && obs.output_tokens > 0) {
            double tps = obs.decode_tokens_per_sec();
            if (tps > 0) {
                double dtps_prof = 0.0;
                if (auto it = profs_.find(key); it != profs_.end()) {
                    dtps_prof = it->second.decode_tokens_per_sec;
                    double ratio = dtps_prof > 0 ? tps / dtps_prof : 0.0;
                    // EMA toward observed; bounded 0.2..5x to avoid runaway.
                    double cl = (std::min)(5.0, (std::max)(0.2, ratio));
                    st.decode_ema = st.decode_ema <= 0 ? cl : 0.6 * st.decode_ema + 0.4 * cl;
                }
            }
        }
    }

    // Bounded accessors used by the prediction-feedback proof.
    [[nodiscard]] std::uint32_t evidence_for(const std::string& key) const {
        auto it = learn_.find(key);
        return it == learn_.end() ? 0 : it->second.count;
    }
    [[nodiscard]] std::uint64_t evidence_for_model(const ModelId& m) const {
        std::uint64_t n = 0;
        for (auto& [k, st] : learn_) { if (k.find("model=" + std::to_string(m.value())) != std::string::npos) n += st.count; }
        return n;
    }

    // Estimated (possibly learned) per-token KV growth for a key.
    [[nodiscard]] std::uint64_t kv_bytes_per_token(const std::string& key) const {
        auto it = learn_.find(key);
        if (it == learn_.end() || it->second.last_kv == 0) return 0;
        return it->second.last_kv;
    }

private:
    struct LearnedState {
        bool seen{false};
        std::uint32_t count{0};
        double last_duration_ns{0};
        std::uint64_t last_output_tokens{0};
        ByteCount last_peak_vram{0};
        ByteCount last_kv{0};
        ByteCount max_observed_peak{0};
        double decode_ema{0.0};
    };
    static constexpr std::uint32_t kMaxSamplesPerKey = 4096;

    struct ProfileKeyedCtx {
        ByteCount predicted_peak{0};
        ByteCount observed_adjust{0};
    };
    std::map<std::string, PredictionProfile> profs_;
    std::map<std::string, bool> seen_;
    std::map<std::string, LearnedState> learn_;
    std::uint64_t generation_{1};

    static std::string profile_key(const PredictionProfile& p) {
        return "model=" + std::to_string(p.model_id.value()) + ";rev=" + std::to_string(p.revision.value())
             + ";be=" + p.backend + ";dt=" + p.dtype + ";q=" + p.quantization;
    }
    static std::string profile_key(const AdmissionDescriptor& d) {
        return "model=" + std::to_string(d.model_id.value()) + ";rev=" + std::to_string(d.model_revision.value())
             + ";be=" + d.backend + ";dt=" + d.dtype + ";q=" + d.quantization;
    }
    static std::string profile_key(const Observation& o) {
        return "model=" + std::to_string(o.model_id.value()) + ";rev=" + std::to_string(o.model_revision.value())
             + ";be=" + o.backend + ";dt=" + o.dtype + ";q=" + o.quantization;
    }
    [[nodiscard]] const PredictionProfile* profile(const std::string& key) const {
        auto it = profs_.find(key);
        return it == profs_.end() ? nullptr : &it->second;
    }
    // Resolve a candidate key (from a descriptor/observation) to the registered
    // profile's canonical key when the candidate omits detail (e.g. dtype or
    // quantization). Falls back to the candidate itself.
    [[nodiscard]] std::string resolve_key(const std::string& candidate, const std::string& prefix) const {
        if (profs_.count(candidate) != 0) return candidate;
        auto it = profs_.lower_bound(prefix);
        if (it != profs_.end() && it->first.rfind(prefix, 0) == 0) return it->first;
        return candidate;
    }
    static std::string model_rev_prefix(const AdmissionDescriptor& d) {
        return "model=" + std::to_string(d.model_id.value()) + ";rev=" + std::to_string(d.model_revision.value()) + ";";
    }
    static std::string model_rev_prefix(const Observation& o) {
        return "model=" + std::to_string(o.model_id.value()) + ";rev=" + std::to_string(o.model_revision.value()) + ";";
    }
    // Find a profile for a descriptor; falls back to a model/revision prefix
    // match so a descriptor that does not declare backend/dtype still binds a
    // registered profile for that model.
    [[nodiscard]] const PredictionProfile* profile_for_descriptor(const AdmissionDescriptor& d) const {
        std::string exact = profile_key(d);
        if (auto* p = profile(exact)) return p;
        std::string prefix = "model=" + std::to_string(d.model_id.value()) + ";rev=" + std::to_string(d.model_revision.value()) + ";";
        auto it = profs_.lower_bound(prefix);
        if (it != profs_.end() && it->first.rfind(prefix, 0) == 0) return &it->second;
        return nullptr;
    }
    [[nodiscard]] std::uint64_t next_generation_() { if (generation_ == (std::numeric_limits<std::uint64_t>::max)()) { generation_ = 1; } else { generation_++; } return generation_; }

    static ByteCount multiply_checked(ByteCount a, ByteCount b) {
        std::uint64_t out = 0;
        if (!checked_mul(a, b, out)) return (std::numeric_limits<ByteCount>::max)();
        return out;
    }
    static ByteCount rat_of(ByteCount base, double ratio) {
        if (base == 0 || ratio <= 0) return 0;
        std::uint64_t out = 0;
        if (!checked_mul(base, static_cast<ByteCount>(ratio * 1000.0), out)) return (std::numeric_limits<ByteCount>::max)();
        return out / 1000;
    }

    static Nanoseconds from_seconds(double s) {
        if (!(s >= 0) || !std::isfinite(s)) return Nanoseconds(0);
        if (s > 1e12) s = 1e12;
        return Nanoseconds(static_cast<long long>(s * 1e9));
    }

    void p_provenance_snapshot(const std::string& key, PredictionProvenance& prov, double& conf,
                               std::uint32_t& ev, bool& fb) const {
        auto it = profs_.find(key);
        if (it != profs_.end() && !it->second.backend.empty()) {
            auto lit = learn_.find(key);
            bool historical = lit != learn_.end() && lit->second.count > 0;
            prov = historical ? PredictionProvenance::Hybrid : PredictionProvenance::ModelProfile;
            ev = historical ? lit->second.count : 0;
            conf = historical ? (std::min)(0.95, 0.6 + 0.35 * (1.0 - std::exp(-static_cast<double>(lit->second.count) / 8.0)))
                              : 0.6;
            fb = false;
        } else {
            prov = PredictionProvenance::Unavailable;
            conf = 0.0;
            ev = 0;
            fb = true;
        }
    }

    void record_evidence_for_profile(const std::string& key, std::uint32_t& ev, double& conf) const {
        auto lit = learn_.find(key);
        if (lit == learn_.end() || lit->second.count == 0) return;
        ev = (std::max)(ev, lit->second.count);
        conf = (std::max)(conf, (std::min)(0.98, 0.6 + 0.38 * (1.0 - std::exp(-static_cast<double>(lit->second.count) / 6.0))));
        return;
    }

    void apply_learned_adjustment(const std::string& key, MemoryFootprint& fp) const {
        auto lit = learn_.find(key);
        if (lit == learn_.end()) return;
        const auto& st = lit->second;
        if (st.count == 0 || st.last_peak_vram == 0) return;
        ByteCount predicted_before = fp.base() + fp.safety_margin + fp.fragmentation_allowance;
        if (predicted_before == 0) return;
        double ratio = static_cast<double>(st.last_peak_vram) / static_cast<double>(predicted_before);
        // If the last observation exceeded the prediction, scale the *variable*
        // portion up (bounded to 2x). If it was conservative, scale variable
        // portion down but never below 0.8x.
        double cl = (std::min)(2.0, (std::max)(0.8, ratio));
        fp.kv_growth = rat_of(fp.kv_growth, cl);
        fp.tensor_state = rat_of(fp.tensor_state, cl);
        fp.staging = rat_of(fp.staging, cl);
        // Recompute margin after adjustment, keep fixed model authoritative.
        fp.safety_margin = rat_of(fp.base(), 0.10);
        fp.fragmentation_allowance = rat_of(fp.base(), 0.05);
    }

    double effective_decode_rate(const std::string& key, double profile_rate) const {
        auto lit = learn_.find(key);
        if (lit == learn_.end() || lit->second.decode_ema <= 0) return profile_rate;
        return profile_rate > 0 ? profile_rate * lit->second.decode_ema : lit->second.decode_ema;
    }
};

} // namespace admission_fabric
