#pragma once

// Admission Fabric - field-by-field codecs for the domain structures.
//
// These codecs are used by both the persistence layer and the wire protocol so
// that on-disk and on-the-wire encoding are canonical and consistent. Decoders
// are defensive: they reject unknown versions, impossible counts, truncation
// and enum range violations.

#include <cstdint>
#include <optional>
#include <string>
#include "admission_fabric/decision.hpp"
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/observation.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/quota.hpp"
#include "admission_fabric/reservation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/serialize.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

// --- primitive helpers -----------------------------------------------------
template <typename Tag, typename Rep>
void encode_id(ByteWriter& w, const Id<Tag, Rep>& id) { w.u64(id.value()); }
template <typename Tag, typename Rep>
bool decode_id(ByteReader& r, Id<Tag, Rep>& id) { id = Id<Tag, Rep>(r.u64()); return r.ok(); }

void encode_resource_vector(ByteWriter& w, const ResourceVector& v);
bool decode_resource_vector(ByteReader& r, ResourceVector& v);
void encode_capabilities(ByteWriter& w, const Capabilities& c);
bool decode_capabilities(ByteReader& r, Capabilities& c);
void encode_timepoint(ByteWriter& w, TimePoint t);
bool decode_timepoint(ByteReader& r, TimePoint& t);

// --- domain structures -----------------------------------------------------
Status encode_descriptor(ByteWriter& w, const AdmissionDescriptor& d);
Status decode_descriptor(ByteReader& r, AdmissionDescriptor& d);

Status encode_snapshot(ByteWriter& w, const ResourceSnapshot& s);
Status decode_snapshot(ByteReader& r, ResourceSnapshot& s);

Status encode_reservation(ByteWriter& w, const ResourceReservation& r);
Status decode_reservation(ByteReader& r, ResourceReservation& res);

Status encode_policy(ByteWriter& w, const AdmissionPolicy& p);
Status decode_policy(ByteReader& r, AdmissionPolicy& p);

Status encode_quota(ByteWriter& w, const QuotaSnapshot& q);
Status decode_quota(ByteReader& r, QuotaSnapshot& q);

Status encode_observation(ByteWriter& w, const Observation& o);
Status decode_observation(ByteReader& r, Observation& o);

// AdmissionDecision is the largest structure; encode the fields that matter for
// authority + explanation + recovery.
Status encode_decision(ByteWriter& w, const AdmissionDecision& d);
Status decode_decision(ByteReader& r, AdmissionDecision& d);

Status encode_prediction_profile(ByteWriter& w, const PredictionProfile& p);
Status decode_prediction_profile(ByteReader& r, PredictionProfile& p);

} // namespace admission_fabric
