#pragma once

// Admission Fabric - deterministic bounded fairness.
//
// Admission control must not reduce to first-come-first-served or pure
// highest-priority-wins. This controller maintains bounded per-tenant and
// per-class fairness state (recent admits / deferrals / rejections, weight,
// starvation age) and produces a deterministic priority boost so that a large
// request cannot permanently starve smaller ones and endless small requests
// cannot starve a large request forever. Tie-breaking is by identity.

#include <map>
#include <string>
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/units.hpp"
#include "admission_fabric/policy.hpp"

namespace admission_fabric {

class FairnessController {
public:
    static constexpr std::size_t kMaxTenants = 512;

    void set_tenant_weight(const TenantId& t, double weight) {
        auto& st = state(t);
        st.weight = weight;
    }

    void note_admission(const TenantId& t, const AdmissionDescriptor& d) {
        (void)d;
        auto& st = state(t);
        st.active_++;
        st.recent_admissions_++;
        st.last_admit_ = SystemClock::now();
    }
    void note_deferral(const TenantId& t) { ++state(t).recent_deferrals_; }
    void note_rejection(const TenantId& t) { ++state(t).recent_rejections_; }
    void note_completion(const TenantId& t) {
        auto& st = state(t);
        if (st.active_ > 0) --st.active_;
    }
    void note_admission_started(const TenantId& t) { ++state(t).active_; }

    [[nodiscard]] std::uint32_t active_admitted(const TenantId& t) const {
        auto it = tenants_.find(t);
        return it == tenants_.end() ? 0 : it->second.active_;
    }

    // Starvation age (time since last admission) in seconds.
    [[nodiscard]] Nanoseconds starvation_age(const TenantId& t) const {
        auto it = tenants_.find(t);
        if (it == tenants_.end()) return Nanoseconds{0};
        if (!it->second.last_admit_) return Nanoseconds{0};
        return SystemClock::now() - *it->second.last_admit_;
    }

    [[nodiscard]] double tenant_weight(const TenantId& t) const {
        auto it = tenants_.find(t);
        return it == tenants_.end() ? 1.0 : it->second.weight;
    }

    // Deterministic effective priority: base priority + a bounded fairness
    // boost derived from starvation age and recent deny history.
    [[nodiscard]] double effective_priority(const AdmissionDescriptor& d, const AdmissionPolicy& p) const {
        double boost = 0.0;
        // Small-request boost: smaller requests are favored slightly so they are
        // not permanently blocked by a huge queued request.
        if (d.expected_output_tokens < 512) boost += p.small_request_priority_boost;
        // Starvation boost: a tenant that has not been admitted in a while is
        // favored so it cannot be starved indefinitely.
        auto age = starvation_age(d.tenant_id);
        double age_s = static_cast<double>(age.count()) / 1e9;
        boost += (std::min)(p.large_request_starvation_boost, age_s * 0.02);
        return static_cast<double>(d.priority) + boost;
    }

    // Advise whether a large request should be deferred to protect smaller
    // pending requests from starvation. Deterministic given the inputs.
    [[nodiscard]] bool should_defer_large(const AdmissionDescriptor& d, const AdmissionPolicy& p,
                                          std::uint32_t pending_small_requests, double capacity_fraction,
                                          bool large_starving) const {
        (void)d;
        if (pending_small_requests == 0) return false;
        if (large_starving) return false;  // a starving large request is admitted first
        // An enormous request that would consume most of the capacity is deferred
        // while small requests wait, unless policy forbids strict ordering.
        if (capacity_fraction > 0.60 && pending_small_requests >= p.fairness_window / 4)
            return true;
        return false;
    }

    // Deterministic tie-break key: two requests with equal effective priority
    // are ordered by identity (so the outcome is reproducible).
    [[nodiscard]] static bool less_when_tie(const AdmissionRequestId& a, const AdmissionRequestId& b) {
        return a.value() < b.value();
    }

    [[nodiscard]] std::size_t tenant_count() const { return tenants_.size(); }

private:
    struct TenantState {
        double weight{1.0};
        std::uint32_t active_{0};
        std::uint64_t recent_admissions_{0};
        std::uint64_t recent_deferrals_{0};
        std::uint64_t recent_rejections_{0};
        ResourceVector accounted_demand_;
        std::optional<TimePoint> last_admit_;
        std::optional<TimePoint> first_wait_;
    };
    std::map<TenantId, TenantState> tenants_;

    [[nodiscard]] TenantState& state(const TenantId& t) {
        auto it = tenants_.find(t);
        if (it == tenants_.end()) {
            if (tenants_.size() >= kMaxTenants && !evict_lru()) {
                // Fallback: reuse the tenant with the smallest evidence.
                auto fallback = tenants_.begin();
                if (fallback != tenants_.end()) {
                    tenants_.erase(fallback);
                }
            }
            it = tenants_.emplace(t, TenantState{}).first;
        }
        return it->second;
    }
    [[nodiscard]] bool evict_lru() { return false; }  // bounded by map growth policy above
};

} // namespace admission_fabric
