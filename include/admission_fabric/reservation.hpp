#pragma once

// Admission Fabric - atomic capacity reservation.
//
// Reservations are the proof-of-authority that an admission decision is still
// valid. A reservation is a first-class, identity-bearing claim against
// governed capacity. Multi-resource reservations are atomic: either the whole
// vector is reserved or nothing is. Rollback returns accounting to the exact
// pre-attempt state. Double-commit, double-release, reservation-after-rejection
// and admission-without-reservation are all rejected without corrupting
// accounting.

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

enum class OvercommitMode : std::uint8_t {
    None = 0,           // never reserve beyond total capacity
    Bounded = 1,        // reserve beyond total up to an explicit, exposed amount
    PredictionAware = 2 // allow bounded overcommit predicated on confidence of prediction
};
inline const char* overcommit_mode_name(OvercommitMode m) {
    switch (m) { case OvercommitMode::None: return "none"; case OvercommitMode::Bounded: return "bounded";
        case OvercommitMode::PredictionAware: return "prediction_aware"; }
    return "unknown";
}

enum class ReservationStatus : std::uint8_t {
    Reserved = 0,    // provisional
    Committed = 1,   // tied to an admitted request
    Released = 2,    // resources returned
    Expired = 3,     // TTL elapsed without commit
    RolledBack = 4   // provisional cancelled
};
inline const char* reservation_status_name(ReservationStatus s) {
    switch (s) { case ReservationStatus::Reserved: return "reserved"; case ReservationStatus::Committed: return "committed";
        case ReservationStatus::Released: return "released"; case ReservationStatus::Expired: return "expired";
        case ReservationStatus::RolledBack: return "rolled_back"; }
    return "unknown";
}

struct ResourceReservation {
    ReservationId id;
    TenantId owner;
    ResourceVector demand;        // reserved resource vector
    TimePoint created_at;
    TimePoint expires_at;
    std::optional<AdmissionRequestId> admission_request_id;
    std::optional<AdmissionGeneration> admission_generation;
    ReservationStatus status{ReservationStatus::Reserved};
    std::uint64_t generation{0};
};

// Bounded, atomic reservation manager against a governed (and optionally
// bounded-overcommit) capacity envelope.
class ReservationManager {
public:
    void set_total(const ResourceVector& total) { total_ = total; }
    [[nodiscard]] const ResourceVector& total() const { return total_; }

    void set_overcommit_mode(OvercommitMode mode) { overcommit_mode_ = mode; }
    [[nodiscard]] OvercommitMode overcommit_mode() const { return overcommit_mode_; }
    // Explicit bounded overcommit allowance (beyond total). Exposed so the
    // caller can see exactly how much overcommit is permitted and never treat
    // the allowance as physically free.
    void set_overcommit_allowance(const ResourceVector& allowance) { overcommit_allowance_ = allowance; }
    [[nodiscard]] const ResourceVector& overcommit_allowance() const { return overcommit_allowance_; }

private:
    // A resource dimension with a total capacity of zero is treated as
    // UNCONSTRAINED (the snapshot simply did not govern it). This keeps the
    // accounting coherent while never falsely reporting bounded scarcity.
    static constexpr std::uint64_t kUnbounded = (std::numeric_limits<std::uint64_t>::max)() / 4;
    [[nodiscard]] std::uint64_t dim_ceiling(ResourceId id) const {
        std::uint64_t t = total_.get(id);
        if (t == 0) return kUnbounded;
        std::uint64_t allowance = 0;
        if (overcommit_mode_ != OvercommitMode::None) allowance = overcommit_allowance_.get(id);
        return sat_add(t, allowance);
    }
public:
    // Total reserve-able per dimension = total + overcommit allowance (if mode
    // allows it); unconstrained dimensions report a very large ceiling.
    [[nodiscard]] ResourceVector ceiling() const {
        ResourceVector c;
        for (std::uint8_t i = 0; i < kResourceCount; ++i) {
            ResourceId id = static_cast<ResourceId>(i);
            c.set(id, dim_ceiling(id));
        }
        return c;
    }
    // How much of the ceiling is physically beyond total (i.e., overcommitted
    // headroom that the policy has explicitly permitted).
    [[nodiscard]] ResourceVector overcommit_headroom() const {
        if (overcommit_mode_ == OvercommitMode::None) return ResourceVector();
        return overcommit_allowance_;
    }
    // Current reserved (provisional + committed) as a vector.
    [[nodiscard]] ResourceVector reserved() const { return reserved_sum_; }
    [[nodiscard]] ResourceVector used() const { return used_sum_; }
    [[nodiscard]] bool overcommitted() const {
        // Overcommit means committed+reserved exceeds PHYSICAL capacity (not the
        // overcommit-ceiling): the extra is permitted but must be exposed.
        ResourceVector sum = reserved_sum_.sat_add(used_sum_);
        for (std::uint8_t i = 0; i < kResourceCount; ++i) {
            ResourceId id = static_cast<ResourceId>(i);
            if (total_.get(id) == 0) continue;              // unconstrained
            if (sum.get(id) > total_.get(id)) return true;
        }
        return false;
    }
    [[nodiscard]] ResourceVector availability() const {
        ResourceVector a;
        for (std::uint8_t i = 0; i < kResourceCount; ++i) {
            ResourceId id = static_cast<ResourceId>(i);
            a.set(id, sat_sub(dim_ceiling(id), sat_add(reserved_sum_.get(id), used_sum_.get(id))));
        }
        return a;
    }

    // Atomic multi-resource reservation. Returns the reservation id on success,
    // or an error describing the limiting dimension. Nothing mutates on failure.
    [[nodiscard]] Result<ReservationId> reserve(const ReservationId& id, const TenantId& owner,
                                                const ResourceVector& demand, Nanoseconds ttl) {
        // Identity collision.
        if (reservations_.count(id) != 0)
            return Result<ReservationId>::fail(ErrorCode::ReservationIdentityCollision, "reservation id already exists");
        // No reservation of zero capacity (avoid degenerate accounting).
        if (demand.is_zero())
            return Result<ReservationId>::fail(ErrorCode::InvalidArgument, "reservation demand is all-zero");
        // Atomic feasibility check: the entire vector must fit available capacity.
        ResourceVector next = reserved_sum_;
        if (!next.try_add(demand, next))
            return Result<ReservationId>::fail(ErrorCode::ReservationOverflow, "reserved+demand overflow");
        ResourceVector used_plus_reserved;
        if (!used_sum_.try_add(next, used_plus_reserved))
            return Result<ReservationId>::fail(ErrorCode::ReservationOverflow, "used+reserved overflow");
        for (std::uint8_t di = 0; di < kResourceCount; ++di) {
            ResourceId dim = static_cast<ResourceId>(di);
            if (used_plus_reserved.get(dim) > dim_ceiling(dim)) {
                return Result<ReservationId>::fail(ErrorCode::InsufficientCapacity,
                    std::string("insufficient ") + resource_name(dim));
            }
        }
        // Commit the change atomically.
        reserved_sum_ = next;
        TimePoint now = SystemClock::now();
        ResourceReservation r;
        r.id = id;
        r.owner = owner;
        r.demand = demand;
        r.created_at = now;
        r.expires_at = now + std::chrono::duration_cast<SystemClock::duration>(ttl);
        r.status = ReservationStatus::Reserved;
        r.generation = next_generation_();
        reservations_.emplace(id, r);
        return Result<ReservationId>::ok(id);
    }

    // Ties a provisional reservation to a committed admission. Returns false if
    // the reservation is not in Reserved state or already expired.
    [[nodiscard]] Status commit(const ReservationId& id, const AdmissionRequestId& ar,
                                const AdmissionGeneration& gen) {
        auto it = reservations_.find(id);
        if (it == reservations_.end())
            return Status::failure(ErrorCode::StaleReservation, "unknown reservation id");
        auto& r = it->second;
        if (r.status == ReservationStatus::Committed)
            return Status::failure(ErrorCode::DoubleReservation, "reservation already committed");
        if (r.status != ReservationStatus::Reserved)
            return Status::failure(ErrorCode::OperationNotPermitted, "reservation not in reserved state");
        if (SystemClock::now() > r.expires_at) {
            // Expire it and free resources before reporting.
            expire_unsafe(it);
            return Status::failure(ErrorCode::ReservationExpired, "reservation expired before commit");
        }
        // reserved -> used.
        ResourceVector r2, u2;
        if (!reserved_sum_.try_sub(r.demand, r2))
            return Status::failure(ErrorCode::ReservationRollbackError, "reserved-commit underflow");
        if (!used_sum_.try_add(r.demand, u2))
            return Status::failure(ErrorCode::ReservationOverflow, "used-commit overflow");
        reserved_sum_ = r2;
        used_sum_ = u2;
        r.status = ReservationStatus::Committed;
        r.admission_request_id = ar;
        r.admission_generation = gen;
        return Status::success();
    }

    // Roll back a provisional reservation to the exact pre-attempt state.
    [[nodiscard]] Status rollback(const ReservationId& id) {
        auto it = reservations_.find(id);
        if (it == reservations_.end())
            return Status::failure(ErrorCode::StaleReservation, "unknown reservation id");
        auto& r = it->second;
        if (r.status == ReservationStatus::RolledBack || r.status == ReservationStatus::Released)
            return Status::success();  // idempotent rollback, no corruption
        if (r.status != ReservationStatus::Reserved)
            return Status::failure(ErrorCode::OperationNotPermitted, "only provisional reservations can be rolled back");
        ResourceVector r2;
        if (!reserved_sum_.try_sub(r.demand, r2))
            return Status::failure(ErrorCode::ReservationRollbackError, "rollback underflow");
        reserved_sum_ = r2;
        r.status = ReservationStatus::RolledBack;
        return Status::success();
    }

    // Release a committed (or expired) reservation. Idempotent: releasing an
    // already-released reservation is a no-op success and never corrupts
    // accounting.
    [[nodiscard]] Status release(const ReservationId& id) {
        auto it = reservations_.find(id);
        if (it == reservations_.end())
            return Status::failure(ErrorCode::StaleReservation, "unknown reservation id");
        auto& r = it->second;
        if (r.status == ReservationStatus::Released || r.status == ReservationStatus::RolledBack)
            return Status::success();  // idempotent
        if (r.status == ReservationStatus::Reserved) {
            // A provisional reservation released without commit must roll back
            // its reserved capacity, then mark released.
            ResourceVector r2;
            if (!reserved_sum_.try_sub(r.demand, r2))
                return Status::failure(ErrorCode::ReservationRollbackError, "release-rollback underflow");
            reserved_sum_ = r2;
        } else if (r.status == ReservationStatus::Committed) {
            // Free used capacity.
            ResourceVector u2;
            if (!used_sum_.try_sub(r.demand, u2))
                return Status::failure(ErrorCode::DoubleRelease, "used-release underflow (already released)");
            used_sum_ = u2;
        }
        r.status = ReservationStatus::Released;
        return Status::success();
    }

    // Expire any reservations past their TTL. Idempotent; bounded sweep.
    void expire_stale(TimePoint now) {
        for (auto it = reservations_.begin(); it != reservations_.end();) {
            if (it->second.status == ReservationStatus::Reserved && now > it->second.expires_at) {
                // Expire provisional: free reserved capacity.
                ResourceVector r2;
                if (reserved_sum_.try_sub(it->second.demand, r2)) { reserved_sum_ = r2; it->second.status = ReservationStatus::Expired; }
                ++it;
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] bool has(const ReservationId& id) const { return reservations_.count(id) != 0; }
    [[nodiscard]] const ResourceReservation* find(const ReservationId& id) const {
        auto it = reservations_.find(id);
        return it == reservations_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const std::map<ReservationId, ResourceReservation>& reservations() const { return reservations_; }
    [[nodiscard]] std::size_t reservation_count() const { return reservations_.size(); }

    // Restore reservation state after recovery: validates that the provided per-
    // status demand sums exactly match the recorded reserved/used vectors, that
    // there are no duplicate ids, and repopulates the table.
    [[nodiscard]] Status restore(const std::vector<ResourceReservation>& reservations,
                                 const ResourceVector& reserved, const ResourceVector& used) {
        ResourceVector rsum, usum;
        std::map<ReservationId, int> seen;
        for (auto& r : reservations) {
            if (++seen[r.id] > 1) return Status::failure(ErrorCode::PersistenceDuplicateId, "duplicate reservation id");
            ResourceVector n;
            if (r.status == ReservationStatus::Reserved) { if (!rsum.try_add(r.demand, n)) return Status::failure(ErrorCode::PersistenceInconsistentAccounting, "reserved sum overflow"); rsum = n; }
            else if (r.status == ReservationStatus::Committed) { if (!usum.try_add(r.demand, n)) return Status::failure(ErrorCode::PersistenceInconsistentAccounting, "used sum overflow"); usum = n; }
            else if (!r.demand.is_zero()) return Status::failure(ErrorCode::PersistenceInconsistentAccounting, "terminal reservation carries demand");
        }
        if (rsum != reserved) return Status::failure(ErrorCode::PersistenceInconsistentAccounting, "reserved accounting mismatch");
        if (usum != used) return Status::failure(ErrorCode::PersistenceInconsistentAccounting, "used accounting mismatch");
        // Rebuild table.
        std::map<ReservationId, ResourceReservation> rebuilt;
        for (auto& r : reservations) rebuilt.emplace(r.id, r);
        reservations_ = std::move(rebuilt);
        reserved_sum_ = reserved;
        used_sum_ = used;
        return Status::success();
    }

    // Recompute accounting from the reservation table and compare to running
    // totals; detects leaks / double-count / lost updates.
    [[nodiscard]] bool verify_accounting() const {
        ResourceVector rsum, usum;
        for (auto& [id, r] : reservations_) {
            ResourceVector n;
            if (r.status == ReservationStatus::Reserved) {
                if (!rsum.try_add(r.demand, n)) return false; rsum = n;
            } else if (r.status == ReservationStatus::Committed) {
                if (!usum.try_add(r.demand, n)) return false; usum = n;
            }
        }
        if (rsum != reserved_sum_) return false;
        if (usum != used_sum_) return false;
        ResourceVector tot = rsum.sat_add(usum);
        if (!tot.fits_in(ceiling())) return false;
        return true;
    }

private:
    ResourceVector total_;
    ResourceVector overcommit_allowance_;
    ResourceVector reserved_sum_;
    ResourceVector used_sum_;
    OvercommitMode overcommit_mode_{OvercommitMode::None};
    std::map<ReservationId, ResourceReservation> reservations_;
    std::uint64_t generation_{1};

    // Soft ceiling used for the atomic feasibility check: total + allowance
    // when overcommit is enabled, else total.

    [[nodiscard]] std::uint64_t next_generation_() {
        if (generation_ == (std::numeric_limits<std::uint64_t>::max)()) generation_ = 1; else ++generation_;
        return generation_;
    }

    void expire_unsafe(std::map<ReservationId, ResourceReservation>::iterator it) {
        ResourceVector r2;
        if (reserved_sum_.try_sub(it->second.demand, r2)) { reserved_sum_ = r2; it->second.status = ReservationStatus::Expired; }
    }


};

} // namespace admission_fabric
