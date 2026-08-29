#include "admission_fabric/engine.hpp"
#include "admission_fabric/codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

namespace admission_fabric {

AdmissionFabric::AdmissionFabric() = default;

Status AdmissionFabric::install_policy(const AdmissionPolicy& policy) {
    Status s = policy_.install(policy);
    if (!s.ok()) return s;
    if (const AdmissionPolicy* p = policy_.latest()) {
        reservations_.set_overcommit_mode(p->overcommit);
        if (p->overcommit == OvercommitMode::None) reservations_.set_overcommit_allowance(ResourceVector());
        else reservations_.set_overcommit_allowance(p->overcommit_allowance);
    }
    return Status::success();
}

bool AdmissionFabric::register_profile(const PredictionProfile& profile) { return predictor_.register_profile(profile); }

void AdmissionFabric::set_snapshot(const ResourceSnapshot& snapshot) {
    snapshot_ = snapshot;
    has_snapshot_ = true;
    reservations_.set_total(snapshot_.total);
}

void AdmissionFabric::set_quota(const QuotaSnapshot& quota) { quota_ = quota; }

AdmissionGeneration AdmissionFabric::next_admission_generation_() {
    AdmissionGeneration g = admission_generation_;
    admission_generation_ = admission_generation_.next();
    return g;
}

ReservationId AdmissionFabric::next_reservation_id_() {
    ReservationId id(reservation_counter_);
    if (reservation_counter_ == (std::numeric_limits<std::uint64_t>::max)()) reservation_counter_ = 1;
    else ++reservation_counter_;
    return id;
}

AdmissionDecision AdmissionFabric::build_decision_(const AdmissionDescriptor& d, ReasonCode reason) const {
    AdmissionDecision dec;
    dec.admission_request_id = d.admission_request_id;
    dec.request_id = d.request_id;
    dec.attempt_id = d.attempt_id;
    dec.tenant_id = d.tenant_id;
    dec.model_id = d.model_id;
    dec.model_revision = d.model_revision;
    dec.reason = reason;
    dec.outcome = primary_outcome_of(reason);
    dec.policy_generation = d.policy_generation;
    dec.snapshot_generation = snapshot_.generation;
    dec.admission_generation = AdmissionGeneration(0);
    dec.epoch = CoordinatorEpoch(0);
    dec.decision_time = SystemClock::now();
    return dec;
}

const AdmissionPolicy* AdmissionFabric::policy() const {
    return policy_.latest();
}

const RequestRecord* AdmissionFabric::request(const AdmissionRequestId& id) const {
    auto it = requests_.find(id);
    return it == requests_.end() ? nullptr : &it->second;
}

Status AdmissionFabric::transition_(const AdmissionRequestId& id, RequestLifecycle from, RequestLifecycle to,
                                    const AdmissionDescriptor& d) {
    auto it = requests_.find(id);
    if (it == requests_.end()) {
        RequestRecord rec;
        rec.id = id;
        rec.descriptor = d;
        rec.created_at = SystemClock::now();
        rec.updated_at = rec.created_at;
        rec.lifecycle = from;
        it = requests_.emplace(id, std::move(rec)).first;
    }
    if (it->second.lifecycle == from) {
        it->second.lifecycle = to;
        it->second.updated_at = SystemClock::now();
        return Status::success();
    }
    return Status::failure(ErrorCode::OperationNotPermitted, "illegal lifecycle transition");
}

ReasonCode AdmissionFabric::classify_capability_(const AdmissionDescriptor& d) const {
    if (d.backend.empty()) return ReasonCode::Admit;
    Capabilities required;
    required.add(Capabilities::backend(d.backend));
    if (!d.dtype.empty()) required.add(Capabilities::dtype(d.dtype));
    if (!d.quantization.empty()) required.add(Capabilities::quantization(d.quantization));
    Capabilities avail = snapshot_.capabilities;
    if (avail.contains_all(required)) return ReasonCode::Admit;
    return ReasonCode::RejectCapability;
}

ReasonCode AdmissionFabric::classify_quota_(const AdmissionDescriptor& d, const ResourceVector& demand,
                                            ResourceVector& committed_remaining, std::string& what) const {
    if (!quota_.present_) { committed_remaining = ResourceVector(); return ReasonCode::Admit; }
    ResourceVector rem = quota_.remaining();
    bool violate_resource = false;
    ResourceId limiting = ResourceId::AcceleratorMemory;
    for (std::size_t i = 0; i < kResourceCount; ++i) {
        ResourceId id = static_cast<ResourceId>(i);
        if (!quota_.constrained(id)) continue;
        if (demand.get(id) > rem.get(id)) { violate_resource = true; limiting = id; break; }
    }
    TokenCount tokens = d.input_tokens + d.expected_output_tokens;
    if (quota_.token_budget != 0 && tokens > quota_.token_budget) {
        what = "token_budget"; committed_remaining = rem; return ReasonCode::RejectQuota;
    }
    if (violate_resource) {
        what = std::string(resource_name(limiting)); committed_remaining = rem; return ReasonCode::DeferQuota;
    }
    committed_remaining = rem.sat_sub(demand);
    return ReasonCode::Admit;
}

SloFeasibility AdmissionFabric::evaluate_slo_(const AdmissionDescriptor& d, const DurationPrediction& dur) const {
    SloFeasibility s;
    s.predicted_execution_duration = dur.expected;
    s.predicted_transfer_delay = dur.transfer;
    s.predicted_reservation_delay = Milliseconds(1);
    auto pressure = snapshot_.pressure[static_cast<std::size_t>(ResourceId::AcceleratorMemory)];
    switch (pressure) {
        case PressureLevel::Critical: s.predicted_queue_delay = Milliseconds(200); break;
        case PressureLevel::High: s.predicted_queue_delay = Milliseconds(80); break;
        case PressureLevel::Normal: s.predicted_queue_delay = Milliseconds(20); break;
        case PressureLevel::Low: s.predicted_queue_delay = Milliseconds(5); break;
        default: s.predicted_queue_delay = Milliseconds(0); break;
    }
    s.uncertainty = 1.0 - std::clamp(dur.confidence, 0.0, 1.0);
    auto total = s.predicted_total();
    if (!d.deadline.has_value()) {
        if (d.latency_class == LatencyClass::BestEffort) { s.verdict = SloVerdict::Feasible; }
        else if (d.latency_class == LatencyClass::FixedDeadline) { s.verdict = SloVerdict::Unknown; }
        else { s.verdict = SloVerdict::FeasibleWithRisk; }
        s.explanation = "no absolute deadline enforced";
        return s;
    }
    auto budget = *d.deadline;
    auto remaining = budget - total;
    s.deadline_remaining = remaining;
    if (remaining.count() <= 0) {
        s.verdict = SloVerdict::Infeasible;
        s.explanation = "predicted time exceeds deadline";
    } else if (static_cast<double>(remaining.count()) / static_cast<double>(budget.count()) < 0.10) {
        s.verdict = SloVerdict::FeasibleWithRisk;
        s.explanation = "deadline met but with thin margin";
    } else {
        s.verdict = SloVerdict::Feasible;
        s.explanation = "deadline comfortably met";
    }
    return s;
}

Result<AdmissionDecision> AdmissionFabric::evaluate(const AdmissionDescriptor& d) {
    metrics_.evaluations++;
    // Local helper to finalize a decision for a request (store + lifecycle).
    auto emit = [&](RequestLifecycle to_life, AdmissionDecision& dec, bool admit_metric) {
        requests_[d.admission_request_id].last_decision = dec;
        // Guarantee the record is at least in Evaluating so the final
        // transition is always legal, even for the early malformed path.
        Status s1 = transition_(d.admission_request_id, RequestLifecycle::Proposed, RequestLifecycle::Evaluating, d);
        Status s2 = transition_(d.admission_request_id, RequestLifecycle::Evaluating, to_life, d);
        (void)s1; (void)s2;
        if (admit_metric) { metrics_.admissions++; }
        metrics_.count_reason(dec.reason);
    };

    // 1. Validate.
    Status v = d.validate();
    if (!v.ok()) {
        auto dec = build_decision_(d, ReasonCode::RejectMalformed);
        dec.explanation = v.message();
        emit(RequestLifecycle::Rejected, dec, false);
        metrics_.rejections++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }

    // 1b. Duplicate / already-active request id guard.
    {
        auto it = requests_.find(d.admission_request_id);
        if (it != requests_.end() && it->second.lifecycle != RequestLifecycle::Proposed
            && it->second.lifecycle != RequestLifecycle::Deferred
            && it->second.lifecycle != RequestLifecycle::Evaluating) {
            auto dec = build_decision_(d, ReasonCode::RejectMalformed);
            dec.explanation = "duplicate admission request id (already " + std::string(lifecycle_name(it->second.lifecycle)) + ")";
            emit(RequestLifecycle::Rejected, dec, false);
            metrics_.rejections++;
            return Result<AdmissionDecision>::ok(std::move(dec));
        }
    }

    // 2. Lifecycle entry.
    Status s0 = transition_(d.admission_request_id, RequestLifecycle::Proposed, RequestLifecycle::Evaluating, d);
    (void)s0;

    // 3. Policy.
    const AdmissionPolicy* base = policy_.latest();
    if (base == nullptr) {
        auto dec = build_decision_(d, ReasonCode::RejectPolicy);
        dec.explanation = "no policy installed";
        emit(RequestLifecycle::Rejected, dec, false);
        metrics_.rejections++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }
    AdmissionPolicy pol = policy_.effective(d, *base);
    reservations_.set_overcommit_mode(pol.overcommit);
    if (pol.overcommit == OvercommitMode::None) reservations_.set_overcommit_allowance(ResourceVector());
    else reservations_.set_overcommit_allowance(pol.overcommit_allowance);

    // 4. Predict.
    auto mem = predictor_.predict_memory(d);
    auto dur = predictor_.predict_duration(d);
    auto demand = predictor_.predict_demand(d);

    // 5. Capability.
    if (classify_capability_(d) == ReasonCode::RejectCapability) {
        auto dec = build_decision_(d, ReasonCode::RejectCapability);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.memory_prediction = mem; dec.duration_prediction = dur;
        dec.explanation = "required capability not present on available capacity";
        emit(RequestLifecycle::Rejected, dec, false);
        metrics_.rejections++; metrics_.capability_failures++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }

    // 6. Snapshot staleness / availability.
    if (has_snapshot_) {
        auto now = SystemClock::now();
        if (snapshot_.is_stale_as_of(now, pol.max_snapshot_age) && !pol.accept_stale_snapshot) {
            auto dec = build_decision_(d, ReasonCode::RejectStale);
            dec.predicted_requirement = ResourceRequirement(demand);
            dec.explanation = "resource snapshot is stale beyond policy tolerance";
            emit(RequestLifecycle::Rejected, dec, false);
            metrics_.rejections++; metrics_.stale_authority_rejections++;
            return Result<AdmissionDecision>::ok(std::move(dec));
        }
        if (snapshot_.has_unavailable_dimension()) {
            auto dec = build_decision_(d, ReasonCode::DeferCapacity);
            dec.predicted_requirement = ResourceRequirement(demand);
            dec.explanation = "some capacity dimension unavailable; deferring";
            emit(RequestLifecycle::Deferred, dec, false);
            metrics_.deferrals++;
            return Result<AdmissionDecision>::ok(std::move(dec));
        }
    } else {
        if (pol.require_reservation) {
            auto dec = build_decision_(d, ReasonCode::DeferCapacity);
            dec.predicted_requirement = ResourceRequirement(demand);
            dec.explanation = "no capacity snapshot; deferring until capacity known";
            emit(RequestLifecycle::Deferred, dec, false);
            metrics_.deferrals++;
            return Result<AdmissionDecision>::ok(std::move(dec));
        }
    }

    // 7. Quota.
    QuotaEvaluation qe;
    std::string quota_what;
    ResourceVector committed_remaining;
    ReasonCode qc = classify_quota_(d, demand, committed_remaining, quota_what);
    if (qc == ReasonCode::RejectQuota) {
        auto dec = build_decision_(d, qc);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.memory_prediction = mem; dec.duration_prediction = dur;
        dec.quota = qe; dec.quota.passed = false; dec.quota.limiting_name = quota_what; dec.quota.token_violation = true;
        dec.explanation = "request violates hard quota limit: " + quota_what;
        emit(RequestLifecycle::Rejected, dec, false);
        metrics_.rejections++; metrics_.quota_failures++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }
    if (qc == ReasonCode::DeferQuota) {
        auto dec = build_decision_(d, qc);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.memory_prediction = mem; dec.duration_prediction = dur;
        dec.quota = qe; dec.quota.passed = false; dec.quota.limiting_name = quota_what;
        dec.explanation = "quota headroom insufficient for: " + quota_what;
        dec.defer_hint = DeferHint{qc, Seconds(5), "quota " + quota_what + " headroom", "", "release quota capacity then retry"};
        emit(RequestLifecycle::Deferred, dec, false);
        metrics_.deferrals++; metrics_.quota_failures++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }
    qe.passed = true;

    // 8. SLO.
    SloFeasibility slo = evaluate_slo_(d, dur);
    if (slo.verdict == SloVerdict::Infeasible) {
        auto dec = build_decision_(d, ReasonCode::RejectInfeasible);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.duration_prediction = dur; dec.slo = slo;
        dec.explanation = "SLO infeasible: " + slo.explanation;
        emit(RequestLifecycle::Rejected, dec, false);
        metrics_.rejections++; metrics_.slo_infeasibility++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }
    if (slo.verdict == SloVerdict::Unknown) {
        auto dec = build_decision_(d, ReasonCode::DeferSlo);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.duration_prediction = dur; dec.slo = slo;
        dec.explanation = "SLO unknown for fixed-deadline class without budget; deferring";
        dec.defer_hint = DeferHint{ReasonCode::DeferSlo, Milliseconds(100), "", "", "supply a deadline budget"};
        emit(RequestLifecycle::Deferred, dec, false);
        metrics_.deferrals++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }

    // 9. Capacity with safety headroom.
    ResourceVector avail = reservations_.availability();
    ResourceVector margin;
    for (std::size_t i = 0; i < kResourceCount; ++i) {
        ResourceId id = static_cast<ResourceId>(i);
        std::uint64_t a = avail.get(id);
        std::uint64_t abs = pol.absolute_safety_margin.get(id);
        std::uint64_t ratio_part = static_cast<std::uint64_t>(static_cast<double>(a) * pol.safety_headroom_ratio);
        margin.set(id, sat_add(ratio_part, abs));
    }
    ResourceVector effective = avail.sat_sub(margin);
    bool fits = demand.fits_in(effective);

    std::uint32_t pending_small = 0;
    for (auto& [rid, rec] : requests_) {
        if (rec.lifecycle == RequestLifecycle::Deferred && rec.descriptor.expected_output_tokens < 512) ++pending_small;
    }
    double capacity_fraction = 0.0;
    if (avail.get(ResourceId::AcceleratorMemory) > 0 && demand.get(ResourceId::AcceleratorMemory) > 0) {
        capacity_fraction = static_cast<double>(demand.get(ResourceId::AcceleratorMemory))
            / static_cast<double>(avail.get(ResourceId::AcceleratorMemory));
    }
    bool large_starving = fairness_.starvation_age(d.tenant_id).count() > 10e9;
    bool defer_for_fairness = fairness_.should_defer_large(d, pol, pending_small, capacity_fraction, large_starving);

    if (!fits) {
        auto dec = build_decision_(d, ReasonCode::DeferCapacity);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.memory_prediction = mem; dec.duration_prediction = dur; dec.slo = slo;
        std::string limited;
        for (std::size_t i = 0; i < kResourceCount; ++i) {
            ResourceId id = static_cast<ResourceId>(i);
            if (demand.get(id) > effective.get(id)) { limited = resource_name(id); break; }
        }
        dec.explanation = "insufficient headroom (limiting: " + (limited.empty() ? std::string("capacity") : limited) + ")";
        dec.defer_hint = DeferHint{ReasonCode::DeferCapacity, Seconds(3), "[" + (limited.empty() ? std::string("capacity") : limited) + "] +headroom", "", "wait or release capacity"};
        emit(RequestLifecycle::Deferred, dec, false);
        metrics_.deferrals++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }
    if (defer_for_fairness) {
        auto dec = build_decision_(d, ReasonCode::DeferCapacity);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.memory_prediction = mem; dec.duration_prediction = dur; dec.slo = slo;
        dec.explanation = "deferred by fairness to protect smaller pending requests";
        dec.defer_hint = DeferHint{ReasonCode::DeferCapacity, Milliseconds(50), "", "", "smaller requests ahead"};
        emit(RequestLifecycle::Deferred, dec, false);
        metrics_.deferrals++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }

    // 10. Atomic reservation.
    ReservationId rid = next_reservation_id_();
    auto rr = reservations_.reserve(rid, d.tenant_id, demand, pol.max_reservation_lifetime);
    if (!rr.has_value()) {
        auto dec = build_decision_(d, ReasonCode::DeferCapacity);
        dec.predicted_requirement = ResourceRequirement(demand);
        dec.memory_prediction = mem; dec.duration_prediction = dur; dec.slo = slo;
        dec.explanation = std::string("reservation failed: ") + rr.error().message();
        dec.defer_hint = DeferHint{ReasonCode::DeferCapacity, Seconds(3), "", "", rr.error().message()};
        emit(RequestLifecycle::Deferred, dec, false);
        metrics_.deferrals++;
        return Result<AdmissionDecision>::ok(std::move(dec));
    }

    // Admit.
    AdmissionGeneration gen = next_admission_generation_();
    auto dec = build_decision_(d, ReasonCode::AdmitWithReservation);
    dec.predicted_requirement = ResourceRequirement(demand);
    dec.memory_prediction = mem; dec.duration_prediction = dur; dec.slo = slo; dec.quota = qe;
    dec.reservation_id = rid;
    dec.admission_generation = gen;
    dec.confidence = mem.confidence;
    bool risky = (mem.confidence < pol.min_predictor_confidence) || slo.verdict == SloVerdict::FeasibleWithRisk;
    dec.reason = risky ? ReasonCode::AdmitWithRisk : ReasonCode::AdmitWithReservation;
    dec.outcome = primary_outcome_of(dec.reason);
    dec.explanation = risky ? "admitted with reservation; confidence/SLO margin below strict threshold"
                            : "admitted with reservation; capacity, quota and SLO all feasible";
    emit(RequestLifecycle::Reserved, dec, true);
    metrics_.active_reservations = reservations_.reservation_count();
    return Result<AdmissionDecision>::ok(std::move(dec));
}

Status AdmissionFabric::commit_admission(const AdmissionRequestId& request_id, const AdmissionGeneration& generation) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) return Status::failure(ErrorCode::StaleAdmissionGeneration, "unknown request id");
    auto& rec = it->second;
    if (!rec.last_decision.has_value()) return Status::failure(ErrorCode::OperationNotPermitted, "no decision to commit");
    auto& dec = *rec.last_decision;
    if (dec.admission_generation != generation)
        return Status::failure(ErrorCode::StaleAdmissionGeneration, "stale admission generation");
    if (!rec.last_decision->reservation_id.has_value())
        return Status::failure(ErrorCode::AdmissionWithoutReservation, "decision has no reservation");
    Status s = reservations_.commit(*rec.last_decision->reservation_id, request_id, generation);
    if (!s.ok()) return s;
    rec.lifecycle = RequestLifecycle::Admitted;
    rec.updated_at = SystemClock::now();
    return Status::success();
}

Status AdmissionFabric::release_reservation(const ReservationId& id) {
    Status s = reservations_.release(id);
    if (!s.ok()) return s;
    for (auto& [rid, rec] : requests_) {
        if (rec.last_decision.has_value() && rec.last_decision->reservation_id == id
            && (rec.lifecycle == RequestLifecycle::Admitted || rec.lifecycle == RequestLifecycle::Reserved)) {
            rec.lifecycle = RequestLifecycle::Released;
            rec.updated_at = SystemClock::now();
        }
    }
    metrics_.active_reservations = reservations_.reservation_count();
    return Status::success();
}

Status AdmissionFabric::cancel_request(const AdmissionRequestId& request_id) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) return Status::failure(ErrorCode::StaleAdmissionGeneration, "unknown request id");
    auto& rec = it->second;
    switch (rec.lifecycle) {
        case RequestLifecycle::Proposed:
        case RequestLifecycle::Evaluating:
        case RequestLifecycle::Deferred:
            rec.lifecycle = RequestLifecycle::Cancelled; rec.updated_at = SystemClock::now();
            return Status::success();
        case RequestLifecycle::Reserved:
            if (rec.last_decision.has_value() && rec.last_decision->reservation_id.has_value())
                { Status r = reservations_.rollback(*rec.last_decision->reservation_id); (void)r; }
            rec.lifecycle = RequestLifecycle::Cancelled; rec.updated_at = SystemClock::now();
            return Status::success();
        case RequestLifecycle::Admitted:
            return Status::failure(ErrorCode::OperationNotPermitted, "admitted request must be released, not cancelled");
        default:
            return Status::failure(ErrorCode::OperationNotPermitted, "terminal request cannot mutate");
    }
}

void AdmissionFabric::observe(const Observation& observation) { predictor_.observe(observation); }

std::string AdmissionFabric::explain(const AdmissionDecision& d) const {
    std::ostringstream o;
    o << "admission_request=" << d.admission_request_id.str()
      << " tenant=" << d.tenant_id.str() << " model=" << d.model_id.str()
      << " outcome=" << outcome_name(d.outcome) << " reason=" << reason_code_name(d.reason) << "\n";
    o << "  policy_generation=" << d.policy_generation.str()
      << " snapshot_generation=" << d.snapshot_generation.str()
      << " admission_generation=" << d.admission_generation.str() << "\n";
    o << "  confidence=" << d.confidence
      << " predicted_peak_vram=" << format_bytes(d.memory_prediction.footprint.peak()) << "\n";
    o << "  kv_growth=" << format_bytes(d.memory_prediction.footprint.kv_growth)
      << " tensor_state=" << format_bytes(d.memory_prediction.footprint.tensor_state) << "\n";
    o << "  predicted_duration_ms=" << (d.duration_prediction.expected.count() / 1000000)
      << " slo=" << slo_verdict_name(d.slo.verdict) << "\n";
    if (!d.explanation.empty()) o << "  " << d.explanation << "\n";
    return o.str();
}

Status AdmissionFabric::save_state(ByteWriter& w) const {
    // Policy library.
    const auto& allp = policy_.all();
    w.u32(static_cast<std::uint32_t>(allp.size()));
    for (const auto& [gen, p] : allp) encode_policy(w, p);
    // Predictor profiles.
    const auto& profs = predictor_.profiles();
    w.u32(static_cast<std::uint32_t>(profs.size()));
    for (const auto& [key, prof] : profs) encode_prediction_profile(w, prof);
    (void)0;
    // Reservation table.
    w.u32(static_cast<std::uint32_t>(reservations_.reservation_count()));
    for (const auto& [id, r] : reservations_.reservations()) encode_reservation(w, r);
    encode_resource_vector(w, reservations_.reserved());
    encode_resource_vector(w, reservations_.used());
    w.u8(static_cast<std::uint8_t>(reservations_.overcommit_mode()));
    encode_resource_vector(w, reservations_.overcommit_allowance());
    // Requests.
    w.u32(static_cast<std::uint32_t>(requests_.size()));
    for (const auto& [id, rec] : requests_) {
        w.u64(id.value());
        w.u8(static_cast<std::uint8_t>(rec.lifecycle));
        Status s = encode_descriptor(w, rec.descriptor);
        if (!s.ok()) return s;
        w.flag(rec.last_decision.has_value());
        if (rec.last_decision.has_value()) {
            Status ds = encode_decision(w, *rec.last_decision);
            if (!ds.ok()) return ds;
        }
    }
    // Quota + counters + snapshot.
    Status qs = encode_quota(w, quota_);
    if (!qs.ok()) return qs;
    w.u64(admission_generation_.value());
    w.u64(reservation_counter_);
    w.flag(has_snapshot_);
    if (has_snapshot_) {
        Status ss = encode_snapshot(w, snapshot_);
        if (!ss.ok()) return ss;
    }
    return Status::success();
}

Status AdmissionFabric::restore_state(ByteReader& r) {
    // Clear existing runtime state (fresh recovery target).
    std::map<AdmissionRequestId, RequestRecord> empty_requests;
    requests_.swap(empty_requests);

    // Policy library.
    std::uint32_t n = r.u32();
    if (n > PolicyEngine::kMaxGenerations) return Status::failure(ErrorCode::PersistenceInvalidCount, "policy count");
    for (std::uint32_t i = 0; i < n; ++i) {
        AdmissionPolicy p;
        Status s = decode_policy(r, p);
        if (!s.ok()) return s;
        Status is = policy_.install_at_generation(p, p.generation);
        if (!is.ok()) return is;
    }
    // Predictor profiles.
    n = r.u32();
    if (n > WorkloadPredictor::kMaxProfiles) return Status::failure(ErrorCode::PersistenceInvalidCount, "profile count");
    for (std::uint32_t i = 0; i < n; ++i) {
        PredictionProfile p;
        Status s = decode_prediction_profile(r, p);
        if (!s.ok()) return s;
        predictor_.register_profile(p);
    }
    // Reservations.
    n = r.u32();
    if (n > (1u << 20)) return Status::failure(ErrorCode::PersistenceInvalidCount, "reservation count");
    std::vector<ResourceReservation> rsv;
    rsv.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        ResourceReservation rr;
        Status s = decode_reservation(r, rr);
        if (!s.ok()) return s;
        if (rr.status > ReservationStatus::RolledBack) return Status::failure(ErrorCode::PersistenceCorrupt, "bad reservation status");
        rsv.push_back(std::move(rr));
    }
    ResourceVector res, used;
    if (!decode_resource_vector(r, res) || !decode_resource_vector(r, used))
        return Status::failure(ErrorCode::PersistenceCorrupt, "reservation sums");
    std::uint8_t om = r.u8();
    ResourceVector oca;
    if (!decode_resource_vector(r, oca)) return Status::failure(ErrorCode::PersistenceCorrupt, "overcommit allowance");
    Status rs = reservations_.restore(rsv, res, used);
    if (!rs.ok()) return rs;
    reservations_.set_overcommit_mode(static_cast<OvercommitMode>(om));
    reservations_.set_overcommit_allowance(oca);

    // Requests.
    n = r.u32();
    if (n > (1u << 20)) return Status::failure(ErrorCode::PersistenceInvalidCount, "request count");
    for (std::uint32_t i = 0; i < n; ++i) {
        AdmissionRequestId id(r.u64());
        RequestLifecycle lc = static_cast<RequestLifecycle>(r.u8());
        if (lc > RequestLifecycle::Cancelled) return Status::failure(ErrorCode::PersistenceCorrupt, "bad lifecycle");
        AdmissionDescriptor d;
        Status s = decode_descriptor(r, d);
        if (!s.ok()) return s;
        RequestRecord rec;
        rec.id = id;
        rec.lifecycle = lc;
        rec.descriptor = d;
        rec.created_at = SystemClock::now();
        rec.updated_at = rec.created_at;
        if (r.flag()) {
            AdmissionDecision dec;
            Status ds = decode_decision(r, dec);
            if (!ds.ok()) return ds;
            rec.last_decision = dec;
        }
        if (requests_.count(id) != 0) return Status::failure(ErrorCode::PersistenceDuplicateId, "duplicate request id in state");
        requests_.emplace(id, std::move(rec));
    }
    // Quota.
    QuotaSnapshot q;
    Status qs = decode_quota(r, q);
    if (!qs.ok()) return qs;
    quota_ = q;
    // Counters.
    admission_generation_ = AdmissionGeneration(r.u64());
    reservation_counter_ = r.u64();
    has_snapshot_ = r.flag();
    if (has_snapshot_) {
        ResourceSnapshot s;
        Status ss = decode_snapshot(r, s);
        if (!ss.ok()) return ss;
        set_snapshot(s);
    }
    // Rebind reservation overcommit from the latest policy.
    if (const AdmissionPolicy* p = policy_.latest()) {
        reservations_.set_overcommit_mode(p->overcommit);
        if (p->overcommit == OvercommitMode::None) reservations_.set_overcommit_allowance(ResourceVector());
        else reservations_.set_overcommit_allowance(p->overcommit_allowance);
    }
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

} // namespace admission_fabric
