#include "admission_fabric/codec.hpp"

#include <cstdint>

namespace admission_fabric {

static void enc_opt_res(ByteWriter& w, const std::optional<ResourceVector>& o) {
    w.flag(o.has_value());
    if (o.has_value()) encode_resource_vector(w, *o);
}
static bool dec_opt_res(ByteReader& r, std::optional<ResourceVector>& o) {
    if (!r.flag()) { o.reset(); return true; }
    ResourceVector v;
    if (!decode_resource_vector(r, v)) return false;
    o = v;
    return r.ok();
}
void encode_resource_vector(ByteWriter& w, const ResourceVector& v) {
    for (std::uint8_t i = 0; i < kResourceCount; ++i) w.u64(v.get(static_cast<ResourceId>(i)));
}
bool decode_resource_vector(ByteReader& r, ResourceVector& v) {
    for (std::uint8_t i = 0; i < kResourceCount; ++i) v.set(static_cast<ResourceId>(i), r.u64());
    return r.ok();
}

void encode_capabilities(ByteWriter& w, const Capabilities& c) {
    std::vector<std::string> toks(c.tokens().begin(), c.tokens().end());
    w.stringvec(toks);
}
bool decode_capabilities(ByteReader& r, Capabilities& c) {
    std::vector<std::string> toks;
    if (!r.stringvec(toks)) return false;
    for (auto& t : toks) c.add(t);
    return r.ok();
}

void encode_timepoint(ByteWriter& w, TimePoint t) {
    w.i64(static_cast<std::int64_t>(std::chrono::duration_cast<Nanoseconds>(t.time_since_epoch()).count()));
}
bool decode_timepoint(ByteReader& r, TimePoint& t) {
    std::int64_t ns = r.i64();
    t = TimePoint(std::chrono::duration_cast<SystemClock::duration>(Nanoseconds(ns)));
    return r.ok();
}

Status encode_descriptor(ByteWriter& w, const AdmissionDescriptor& d) {
    w.u8(static_cast<std::uint8_t>(d.workload_type));
    encode_id(w, d.admission_request_id);
    encode_id(w, d.request_id);
    encode_id(w, d.attempt_id);
    encode_id(w, d.tenant_id);
    encode_id(w, d.model_id);
    encode_id(w, d.model_revision);
    w.flag(d.adapter_id.has_value());
    if (d.adapter_id.has_value()) encode_id(w, *d.adapter_id);
    encode_id(w, d.policy_generation);
    w.u64(d.input_tokens);
    w.u64(d.expected_output_tokens);
    w.u64(d.max_output_tokens);
    w.flag(d.batchable);
    w.u8(d.priority);
    w.u8(static_cast<std::uint8_t>(d.latency_class));
    w.flag(d.deadline.has_value());
    if (d.deadline.has_value()) w.i64(static_cast<std::int64_t>(d.deadline->count()));
    w.flag(d.requested_slo.has_value());
    if (d.requested_slo.has_value()) w.f64(*d.requested_slo);
    enc_opt_res(w, d.expected_demand);
    w.u8(static_cast<std::uint8_t>(d.preemption));
    w.flag(d.degradation_permitted);
    w.flag(d.requires_reservation);
    w.i64(static_cast<std::int64_t>(d.admission_ttl.count()));
    w.string(d.backend);
    w.string(d.dtype);
    w.string(d.quantization);
    w.string(d.workload_class);
    w.stringvec(d.policy_tags);
    return Status::success();
}

Status decode_descriptor(ByteReader& r, AdmissionDescriptor& d) {
    d.workload_type = static_cast<WorkloadType>(r.u8());
    if (!decode_id(r, d.admission_request_id)) return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor request id");
    if (!decode_id(r, d.request_id)) return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor request id");
    if (!decode_id(r, d.attempt_id)) return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor attempt id");
    if (!decode_id(r, d.tenant_id)) return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor tenant id");
    if (!decode_id(r, d.model_id)) return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor model id");
    if (!decode_id(r, d.model_revision)) return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor revision");
    if (r.flag()) { AdapterId a; if (!decode_id(r, a)) return Status::failure(ErrorCode::PersistenceCorrupt, "adapter id"); d.adapter_id = a; }
    if (!decode_id(r, d.policy_generation)) return Status::failure(ErrorCode::PersistenceCorrupt, "policy generation");
    d.input_tokens = r.u64();
    d.expected_output_tokens = r.u64();
    d.max_output_tokens = r.u64();
    d.batchable = r.flag();
    d.priority = r.u8();
    d.latency_class = static_cast<LatencyClass>(r.u8());
    if (r.flag()) d.deadline = Nanoseconds(r.i64());
    if (r.flag()) d.requested_slo = r.f64();
    if (!dec_opt_res(r, d.expected_demand)) return Status::failure(ErrorCode::PersistenceCorrupt, "expected demand");
    d.preemption = static_cast<PreemptionPermission>(r.u8());
    d.degradation_permitted = r.flag();
    d.requires_reservation = r.flag();
    d.admission_ttl = Nanoseconds(r.i64());
    if (!r.string(d.backend) || !r.string(d.dtype) || !r.string(d.quantization) || !r.string(d.workload_class))
        return Status::failure(ErrorCode::PersistenceCorrupt, "descriptor strings");
    if (!r.stringvec(d.policy_tags)) return Status::failure(ErrorCode::PersistenceCorrupt, "policy tags");
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_snapshot(ByteWriter& w, const ResourceSnapshot& s) {
    w.u64(s.generation.value());
    encode_timepoint(w, s.timestamp);
    w.u64(s.agent_id.value());
    w.u64(s.boot_id.value());
    w.string(s.node_name);
    w.u32(s.device_index);
    w.string(s.device_name);
    encode_resource_vector(w, s.total);
    encode_resource_vector(w, s.used);
    encode_resource_vector(w, s.reserved);
    encode_resource_vector(w, s.reclaimable);
    encode_capabilities(w, s.capabilities);
    w.flag(s.healthy);
    w.stringvec(s.model_residency_hints);
    w.stringvec(s.transfer_path_hints);
    for (auto p : s.pressure) w.u8(static_cast<std::uint8_t>(p));
    for (auto p : s.provenance) w.u8(static_cast<std::uint8_t>(p));
    return Status::success();
}

Status decode_snapshot(ByteReader& r, ResourceSnapshot& s) {
    s.generation = ResourceSnapshotGeneration(r.u64());
    if (!decode_timepoint(r, s.timestamp)) return Status::failure(ErrorCode::PersistenceCorrupt, "snapshot ts");
    s.agent_id = AgentId(r.u64());
    s.boot_id = AgentBootId(r.u64());
    if (!r.string(s.node_name)) return Status::failure(ErrorCode::PersistenceCorrupt, "node name");
    s.device_index = r.u32();
    if (!r.string(s.device_name)) return Status::failure(ErrorCode::PersistenceCorrupt, "device name");
    if (!decode_resource_vector(r, s.total)) return Status::failure(ErrorCode::PersistenceCorrupt, "total");
    if (!decode_resource_vector(r, s.used)) return Status::failure(ErrorCode::PersistenceCorrupt, "used");
    if (!decode_resource_vector(r, s.reserved)) return Status::failure(ErrorCode::PersistenceCorrupt, "reserved");
    if (!decode_resource_vector(r, s.reclaimable)) return Status::failure(ErrorCode::PersistenceCorrupt, "reclaimable");
    if (!decode_capabilities(r, s.capabilities)) return Status::failure(ErrorCode::PersistenceCorrupt, "caps");
    s.healthy = r.flag();
    if (!r.stringvec(s.model_residency_hints) || !r.stringvec(s.transfer_path_hints))
        return Status::failure(ErrorCode::PersistenceCorrupt, "hints");
    for (auto& p : s.pressure) p = static_cast<PressureLevel>(r.u8());
    for (auto& p : s.provenance) p = static_cast<Provenance>(r.u8());
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_reservation(ByteWriter& w, const ResourceReservation& r) {
    w.u64(r.id.value());
    w.u64(r.owner.value());
    encode_resource_vector(w, r.demand);
    encode_timepoint(w, r.created_at);
    encode_timepoint(w, r.expires_at);
    w.flag(r.admission_request_id.has_value());
    if (r.admission_request_id.has_value()) w.u64(r.admission_request_id->value());
    w.flag(r.admission_generation.has_value());
    if (r.admission_generation.has_value()) w.u64(r.admission_generation->value());
    w.u8(static_cast<std::uint8_t>(r.status));
    w.u64(r.generation);
    return Status::success();
}

Status decode_reservation(ByteReader& r, ResourceReservation& o) {
    o.id = ReservationId(r.u64());
    o.owner = TenantId(r.u64());
    if (!decode_resource_vector(r, o.demand)) return Status::failure(ErrorCode::PersistenceCorrupt, "res demand");
    if (!decode_timepoint(r, o.created_at)) return Status::failure(ErrorCode::PersistenceCorrupt, "res created");
    if (!decode_timepoint(r, o.expires_at)) return Status::failure(ErrorCode::PersistenceCorrupt, "res expires");
    if (r.flag()) o.admission_request_id = AdmissionRequestId(r.u64());
    if (r.flag()) o.admission_generation = AdmissionGeneration(r.u64());
    o.status = static_cast<ReservationStatus>(r.u8());
    o.generation = r.u64();
    // Validate enum range.
    if (o.status > ReservationStatus::RolledBack) return Status::failure(ErrorCode::PersistenceCorrupt, "bad reservation status");
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_policy(ByteWriter& w, const AdmissionPolicy& p) {
    w.u64(p.generation.value());
    w.string(p.name);
    w.f64(p.safety_headroom_ratio);
    encode_resource_vector(w, p.absolute_safety_margin);
    w.u8(static_cast<std::uint8_t>(p.overcommit));
    encode_resource_vector(w, p.overcommit_allowance);
    w.f64(p.overcommit_confidence_min);
    w.f64(p.min_predictor_confidence);
    w.i64(static_cast<std::int64_t>(p.max_snapshot_age.count()));
    w.flag(p.accept_stale_snapshot);
    w.flag(p.quota_fail_closed);
    w.flag(p.admit_slo_feasible_with_risk);
    w.flag(p.require_reservation);
    w.i64(static_cast<std::int64_t>(p.max_reservation_lifetime.count()));
    w.i64(static_cast<std::int64_t>(p.default_admission_ttl.count()));
    w.u32(p.max_deferrals);
    w.i64(static_cast<std::int64_t>(p.defer_min_backoff.count()));
    w.i64(static_cast<std::int64_t>(p.defer_max_wait.count()));
    w.f64(p.small_request_priority_boost);
    w.f64(p.large_request_starvation_boost);
    w.u32(p.fairness_window);
    w.u32(static_cast<std::uint32_t>(p.overrides.size()));
    for (auto& o : p.overrides) {
        w.string(o.key);
        w.flag(o.safety_headroom_ratio.has_value()); if (o.safety_headroom_ratio) w.f64(*o.safety_headroom_ratio);
        w.flag(o.overcommit_mode.has_value()); if (o.overcommit_mode) w.u8(static_cast<std::uint8_t>(*o.overcommit_mode));
        w.flag(o.overcommit_allowance.has_value()); if (o.overcommit_allowance) encode_resource_vector(w, *o.overcommit_allowance);
        w.flag(o.min_predictor_confidence.has_value()); if (o.min_predictor_confidence) w.f64(*o.min_predictor_confidence);
        w.flag(o.admit_slo_risk.has_value()); if (o.admit_slo_risk) w.flag(*o.admit_slo_risk);
        w.flag(o.require_reservation.has_value()); if (o.require_reservation) w.flag(*o.require_reservation);
        w.flag(o.small_request_boost.has_value()); if (o.small_request_boost) w.f64(*o.small_request_boost);
    }
    return Status::success();
}

Status decode_policy(ByteReader& r, AdmissionPolicy& p) {
    p.generation = PolicyGeneration(r.u64());
    if (!r.string(p.name)) return Status::failure(ErrorCode::PersistenceCorrupt, "policy name");
    p.safety_headroom_ratio = r.f64();
    if (!decode_resource_vector(r, p.absolute_safety_margin)) return Status::failure(ErrorCode::PersistenceCorrupt, "policy margin");
    p.overcommit = static_cast<OvercommitMode>(r.u8());
    if (!decode_resource_vector(r, p.overcommit_allowance)) return Status::failure(ErrorCode::PersistenceCorrupt, "policy oc");
    p.overcommit_confidence_min = r.f64();
    p.min_predictor_confidence = r.f64();
    p.max_snapshot_age = Nanoseconds(r.i64());
    p.accept_stale_snapshot = r.flag();
    p.quota_fail_closed = r.flag();
    p.admit_slo_feasible_with_risk = r.flag();
    p.require_reservation = r.flag();
    p.max_reservation_lifetime = Nanoseconds(r.i64());
    p.default_admission_ttl = Nanoseconds(r.i64());
    p.max_deferrals = r.u32();
    p.defer_min_backoff = Nanoseconds(r.i64());
    p.defer_max_wait = Nanoseconds(r.i64());
    p.small_request_priority_boost = r.f64();
    p.large_request_starvation_boost = r.f64();
    p.fairness_window = r.u32();
    std::uint32_t n = r.u32();
    if (n > (1u << 16)) return Status::failure(ErrorCode::PersistenceCorrupt, "policy override count");
    for (std::uint32_t i = 0; i < n; ++i) {
        PolicyOverride o;
        if (!r.string(o.key)) return Status::failure(ErrorCode::PersistenceCorrupt, "override key");
        if (r.flag()) o.safety_headroom_ratio = r.f64();
        if (r.flag()) o.overcommit_mode = static_cast<OvercommitMode>(r.u8());
        if (r.flag()) { ResourceVector v; if (!decode_resource_vector(r, v)) return Status::failure(ErrorCode::PersistenceCorrupt, "override rv"); o.overcommit_allowance = v; }
        if (r.flag()) o.min_predictor_confidence = r.f64();
        if (r.flag()) o.admit_slo_risk = r.flag();
        if (r.flag()) o.require_reservation = r.flag();
        if (r.flag()) o.small_request_boost = r.f64();
        p.overrides.push_back(std::move(o));
    }
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_quota(ByteWriter& w, const QuotaSnapshot& q) {
    w.flag(q.present_);
    w.u64(q.generation);
    w.string(q.source_authority);
    encode_timepoint(w, q.timestamp);
    w.flag(q.stale);
    encode_resource_vector(w, q.limits);
    encode_resource_vector(w, q.used);
    encode_resource_vector(w, q.reserved);
    w.u64(q.token_budget);
    w.u32(q.max_concurrent_requests);
    w.u32(q.max_sequences);
    w.i64(static_cast<std::int64_t>(q.compute_time_allowance.count()));
    return Status::success();
}

Status decode_quota(ByteReader& r, QuotaSnapshot& q) {
    q.present_ = r.flag();
    q.generation = r.u64();
    if (!r.string(q.source_authority)) return Status::failure(ErrorCode::PersistenceCorrupt, "quota source");
    if (!decode_timepoint(r, q.timestamp)) return Status::failure(ErrorCode::PersistenceCorrupt, "quota ts");
    q.stale = r.flag();
    if (!decode_resource_vector(r, q.limits) || !decode_resource_vector(r, q.used) || !decode_resource_vector(r, q.reserved))
        return Status::failure(ErrorCode::PersistenceCorrupt, "quota vectors");
    q.token_budget = r.u64();
    q.max_concurrent_requests = r.u32();
    q.max_sequences = r.u32();
    q.compute_time_allowance = Nanoseconds(r.i64());
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_observation(ByteWriter& w, const Observation& o) {
    w.u64(o.id.value());
    w.u64(o.admission_request_id.value());
    w.u64(o.tenant_id.value());
    w.u64(o.model_id.value());
    w.u64(o.model_revision.value());
    w.string(o.backend);
    w.string(o.dtype);
    w.string(o.quantization);
    w.u8(static_cast<std::uint8_t>(o.workload_type));
    w.u64(o.peak_vram);
    w.u64(o.host_memory);
    w.u64(o.pinned_memory);
    w.u64(o.kv_growth);
    w.u64(o.transfer_bytes);
    w.i64(static_cast<std::int64_t>(o.execution_duration.count()));
    w.u64(o.input_tokens);
    w.u64(o.output_tokens);
    encode_timepoint(w, o.completed_at);
    w.u8(static_cast<std::uint8_t>(o.kind));
    return Status::success();
}

Status decode_observation(ByteReader& r, Observation& o) {
    o.id = ObservationId(r.u64());
    o.admission_request_id = AdmissionRequestId(r.u64());
    o.tenant_id = TenantId(r.u64());
    o.model_id = ModelId(r.u64());
    o.model_revision = ModelRevision(r.u64());
    if (!r.string(o.backend) || !r.string(o.dtype) || !r.string(o.quantization))
        return Status::failure(ErrorCode::PersistenceCorrupt, "obs strings");
    o.workload_type = static_cast<WorkloadType>(r.u8());
    o.peak_vram = r.u64(); o.host_memory = r.u64(); o.pinned_memory = r.u64();
    o.kv_growth = r.u64(); o.transfer_bytes = r.u64();
    o.execution_duration = Nanoseconds(r.i64());
    o.input_tokens = r.u64(); o.output_tokens = r.u64();
    if (!decode_timepoint(r, o.completed_at)) return Status::failure(ErrorCode::PersistenceCorrupt, "obs ts");
    o.kind = static_cast<ObservationKind>(r.u8());
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_decision(ByteWriter& w, const AdmissionDecision& d) {
    w.u64(d.admission_request_id.value());
    w.u64(d.request_id.value());
    w.u64(d.attempt_id.value());
    w.u64(d.tenant_id.value());
    w.u64(d.model_id.value());
    w.u64(d.model_revision.value());
    w.u8(static_cast<std::uint8_t>(d.outcome));
    w.i32(static_cast<std::int32_t>(d.reason));
    w.u64(d.policy_generation.value());
    w.u64(d.snapshot_generation.value());
    w.u64(d.admission_generation.value());
    w.u64(d.epoch.value());
    w.f64(d.confidence);
    encode_timepoint(w, d.decision_time);
    w.flag(d.reservation_id.has_value());
    if (d.reservation_id.has_value()) w.u64(d.reservation_id->value());
    encode_resource_vector(w, d.predicted_requirement.demand());
    w.string(d.explanation);
    return Status::success();
}

Status decode_decision(ByteReader& r, AdmissionDecision& d) {
    d.admission_request_id = AdmissionRequestId(r.u64());
    d.request_id = RequestId(r.u64());
    d.attempt_id = AttemptId(r.u64());
    d.tenant_id = TenantId(r.u64());
    d.model_id = ModelId(r.u64());
    d.model_revision = ModelRevision(r.u64());
    d.outcome = static_cast<AdmissionOutcome>(r.u8());
    d.reason = static_cast<ReasonCode>(r.i32());
    d.policy_generation = PolicyGeneration(r.u64());
    d.snapshot_generation = ResourceSnapshotGeneration(r.u64());
    d.admission_generation = AdmissionGeneration(r.u64());
    d.epoch = CoordinatorEpoch(r.u64());
    d.confidence = r.f64();
    if (!decode_timepoint(r, d.decision_time)) return Status::failure(ErrorCode::PersistenceCorrupt, "decision ts");
    if (r.flag()) d.reservation_id = ReservationId(r.u64());
    ResourceVector rv;
    if (!decode_resource_vector(r, rv)) return Status::failure(ErrorCode::PersistenceCorrupt, "decision req");
    d.predicted_requirement = ResourceRequirement(rv);
    if (!r.string(d.explanation)) return Status::failure(ErrorCode::PersistenceCorrupt, "decision expl");
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

Status encode_prediction_profile(ByteWriter& w, const PredictionProfile& p) {
    w.u64(p.model_id.value());
    w.u64(p.revision.value());
    w.string(p.backend);
    w.string(p.dtype);
    w.string(p.quantization);
    w.u64(p.generation);
    w.u64(p.fixed_base_memory);
    w.u64(p.per_token_kv_bytes);
    w.u64(p.per_token_tensor_bytes);
    w.u64(p.per_request_overhead);
    w.u64(p.workspace_bytes);
    w.u64(p.staging_bytes);
    w.u64(p.pinned_staging_bytes);
    w.f64(p.safety_margin_ratio);
    w.f64(p.fragmentation_ratio);
    w.f64(p.prefill_tokens_per_sec);
    w.f64(p.decode_tokens_per_sec);
    w.u64(p.transfer_bytes_per_sequence);
    w.f64(p.transfer_bytes_per_sec);
    w.u64(p.host_memory_bytes);
    w.u64(p.pinned_host_memory_bytes);
    w.u32(p.execution_slots_required);
    w.u32(p.sequence_slots_required);
    return Status::success();
}

Status decode_prediction_profile(ByteReader& r, PredictionProfile& p) {
    p.model_id = ModelId(r.u64());
    p.revision = ModelRevision(r.u64());
    if (!r.string(p.backend) || !r.string(p.dtype) || !r.string(p.quantization))
        return Status::failure(ErrorCode::PersistenceCorrupt, "profile strings");
    p.generation = r.u64();
    p.fixed_base_memory = r.u64();
    p.per_token_kv_bytes = r.u64();
    p.per_token_tensor_bytes = r.u64();
    p.per_request_overhead = r.u64();
    p.workspace_bytes = r.u64();
    p.staging_bytes = r.u64();
    p.pinned_staging_bytes = r.u64();
    p.safety_margin_ratio = r.f64();
    p.fragmentation_ratio = r.f64();
    p.prefill_tokens_per_sec = r.f64();
    p.decode_tokens_per_sec = r.f64();
    p.transfer_bytes_per_sequence = r.u64();
    p.transfer_bytes_per_sec = r.f64();
    p.host_memory_bytes = r.u64();
    p.pinned_host_memory_bytes = r.u64();
    p.execution_slots_required = r.u32();
    p.sequence_slots_required = r.u32();
    return r.ok() ? Status::success() : Status::failure(ErrorCode::PersistenceCorrupt, r.error());
}

} // namespace admission_fabric
