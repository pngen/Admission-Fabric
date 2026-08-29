#pragma once

// Admission Fabric - versioned admission policy.
//
// A policy is an immutable, validated, serializable object identified by a
// generation. Replacing policy is atomic: a new generation is created and the
// previous one is retained so every historical decision can explain which
// policy governed it. Policies never silently change retroactively.

#include <map>
#include <optional>
#include <string>
#include <vector>
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/reservation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

// A partial override that can be attached to a workload class, model, backend
// or tenant. Applied on top of a base policy.
struct PolicyOverride {
    std::string key;                       // canonical selector, e.g. "model=7", "tenant=3", "backend=cuda"
    std::optional<double> safety_headroom_ratio;
    std::optional<OvercommitMode> overcommit_mode;
    std::optional<ResourceVector> overcommit_allowance;
    std::optional<double> min_predictor_confidence;
    std::optional<bool> admit_slo_risk;
    std::optional<bool> require_reservation;
    std::optional<double> small_request_boost;
};

struct AdmissionPolicy {
    PolicyGeneration generation;
    std::string name;

    // --- safety / headroom -------------------------------------------------
    double safety_headroom_ratio{0.10};        // fraction of available capacity kept reserved
    ResourceVector absolute_safety_margin;     // additional per-dimension absolute margin

    // --- overcommit --------------------------------------------------------
    OvercommitMode overcommit{OvercommitMode::None};
    ResourceVector overcommit_allowance;       // exposed bounded allowance (only with Bounded / PredictionAware)
    double overcommit_confidence_min{0.85};    // confidence required to use PredictionAware overcommit

    // --- predictor ---------------------------------------------------------
    double min_predictor_confidence{0.20};     // below this, predictions are not trusted for capacity

    // --- snapshot tolerance ------------------------------------------------
    Nanoseconds max_snapshot_age{Seconds(5)};
    bool accept_stale_snapshot{false};

    // --- quota -------------------------------------------------------------
    bool quota_fail_closed{true};

    // --- SLO ---------------------------------------------------------------
    bool admit_slo_feasible_with_risk{true};

    // --- reservation -------------------------------------------------------
    bool require_reservation{true};
    Nanoseconds max_reservation_lifetime{Seconds(120)};
    Nanoseconds default_admission_ttl{Seconds(30)};

    // --- deferral ----------------------------------------------------------
    std::uint32_t max_deferrals{2};
    Nanoseconds defer_min_backoff{Seconds(1)};
    Nanoseconds defer_max_wait{Seconds(60)};

    // --- fairness ----------------------------------------------------------
    double small_request_priority_boost{0.0};  // helps small requests compete with a huge queued one
    double large_request_starvation_boost{0.0};// helps a long-waiting large request avoid starvation
    std::uint32_t fairness_window{256};

    // --- bounded overrides -------------------------------------------------
    std::vector<PolicyOverride> overrides;
};

// Deterministic, versioned policy engine.
class PolicyEngine {
public:
    static constexpr std::size_t kMaxGenerations = 128;

    // Validate a policy against the shape rules. Returns ok() iff valid.
    [[nodiscard]] Status validate(const AdmissionPolicy& p) const {
        if (!(p.safety_headroom_ratio >= 0.0 && p.safety_headroom_ratio < 1.0))
            return Status::failure(ErrorCode::PolicyValidationError, "safety_headroom_ratio out of range");
        if (!(p.min_predictor_confidence >= 0.0 && p.min_predictor_confidence <= 1.0))
            return Status::failure(ErrorCode::PolicyValidationError, "min_predictor_confidence out of range");
        if (!(p.overcommit_confidence_min >= 0.0 && p.overcommit_confidence_min <= 1.0))
            return Status::failure(ErrorCode::PolicyValidationError, "overcommit_confidence_min out of range");
        if (p.overcommit == OvercommitMode::None && p.overcommit_allowance.any_nonzero())
            return Status::failure(ErrorCode::PolicyValidationError, "overcommit_allowance set but overcommit mode is None");
        if (p.max_snapshot_age.count() < 0)
            return Status::failure(ErrorCode::PolicyValidationError, "max_snapshot_age negative");
        if (p.max_reservation_lifetime.count() <= 0)
            return Status::failure(ErrorCode::PolicyValidationError, "max_reservation_lifetime non-positive");
        if (p.default_admission_ttl.count() <= 0)
            return Status::failure(ErrorCode::PolicyValidationError, "default_admission_ttl non-positive");
        if (p.max_deferrals > 1024)
            return Status::failure(ErrorCode::PolicyValidationError, "max_deferrals absurd");
        for (const auto& o : p.overrides) {
            if (o.safety_headroom_ratio.has_value() && !(*o.safety_headroom_ratio >= 0.0 && *o.safety_headroom_ratio < 1.0))
                return Status::failure(ErrorCode::PolicyValidationError, "override safety_headroom_ratio out of range");
        }
        return Status::success();
    }

    // Create a new immutable generation of a policy. The policy's generation
    // field is overwritten. Fails if generation history is full.
    [[nodiscard]] Status install(AdmissionPolicy p) {
        Status v = validate(p);
        if (!v.ok()) return v;
        p.generation = next_generation_();
        // Enforce a bounded history: reject if a stale generation would be
        // needed but never evict the currently-active generation.
        if (policies_.size() >= kMaxGenerations && policies_.count(p.generation) == 0)
            return Status::failure(ErrorCode::PolicyConflict, "policy generation history full");
        policies_.erase(p.generation);
        policies_.emplace(p.generation, std::move(p));
        latest_ = p.generation;
        return Status::success();
    }

    // Install a policy under an explicit generation (used for recovery / tests).
    [[nodiscard]] Status install_at_generation(AdmissionPolicy p, PolicyGeneration gen) {
        Status v = validate(p);
        if (!v.ok()) return v;
        p.generation = gen;
        if (policies_.count(gen) != 0)
            return Status::failure(ErrorCode::PolicyConflict, "policy generation already installed");
        policies_.emplace(gen, std::move(p));
        if (gen.value() >= latest_.value()) latest_ = gen;
        return Status::success();
    }

    [[nodiscard]] const AdmissionPolicy* by_generation(PolicyGeneration gen) const {
        auto it = policies_.find(gen);
        return it == policies_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const AdmissionPolicy* latest() const {
        auto it = policies_.find(latest_);
        return it == policies_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] PolicyGeneration latest_generation() const { return latest_; }
    [[nodiscard]] std::size_t generation_count() const { return policies_.size(); }
    [[nodiscard]] const std::map<PolicyGeneration, AdmissionPolicy>& all() const { return policies_; }

    // Resolve the effective policy for a descriptor, applying overrides.
    [[nodiscard]] AdmissionPolicy effective(const AdmissionDescriptor& d, const AdmissionPolicy& base) const {
        AdmissionPolicy p = base;
        for (const auto& o : base.overrides) {
            if (!matches(o.key, d)) continue;
            if (o.safety_headroom_ratio) p.safety_headroom_ratio = *o.safety_headroom_ratio;
            if (o.overcommit_mode) p.overcommit = *o.overcommit_mode;
            if (o.overcommit_allowance) p.overcommit_allowance = *o.overcommit_allowance;
            if (o.min_predictor_confidence) p.min_predictor_confidence = *o.min_predictor_confidence;
            if (o.admit_slo_risk) p.admit_slo_feasible_with_risk = *o.admit_slo_risk;
            if (o.require_reservation) p.require_reservation = *o.require_reservation;
            if (o.small_request_boost) p.small_request_priority_boost = *o.small_request_boost;
        }
        return p;
    }

private:
    std::map<PolicyGeneration, AdmissionPolicy> policies_;
    PolicyGeneration latest_;
    std::uint64_t counter_{1};

    [[nodiscard]] PolicyGeneration next_generation_() {
        std::uint64_t v = counter_;
        if (counter_ == (std::numeric_limits<std::uint64_t>::max)()) counter_ = 1; else ++counter_;
        return PolicyGeneration(v);
    }

    [[nodiscard]] static bool matches(const std::string& key, const AdmissionDescriptor& d) {
        if (key.rfind("model=", 0) == 0) {
            return std::to_string(d.model_id.value()) == key.substr(6);
        }
        if (key.rfind("tenant=", 0) == 0) {
            return std::to_string(d.tenant_id.value()) == key.substr(7);
        }
        if (key.rfind("backend=", 0) == 0) {
            return d.backend == key.substr(8);
        }
        if (key.rfind("class=", 0) == 0) {
            return d.workload_class == key.substr(6);
        }
        return false;
    }
};

} // namespace admission_fabric
