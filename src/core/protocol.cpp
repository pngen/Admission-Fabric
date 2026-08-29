#include "admission_fabric/protocol.hpp"
#include "admission_fabric/codec.hpp"

namespace admission_fabric {

void encode_register(ByteWriter& w, const AgentId& agent, const AgentBootId& boot, const Capabilities& caps,
                     const ResourceVector& inventory, const std::string& device_name, std::uint32_t device_index) {
    encode_id(w, agent);
    encode_id(w, boot);
    encode_capabilities(w, caps);
    encode_resource_vector(w, inventory);
    w.string(device_name);
    w.u32(device_index);
}

Status decode_register(ByteReader& r, AgentId& agent, AgentBootId& boot, Capabilities& caps,
                       ResourceVector& inventory, std::string& device_name, std::uint32_t& device_index) {
    if (!decode_id(r, agent) || !decode_id(r, boot)) return Status::failure(ErrorCode::MalformedFrame, "register id");
    if (!decode_capabilities(r, caps)) return Status::failure(ErrorCode::MalformedFrame, "register caps");
    if (!decode_resource_vector(r, inventory)) return Status::failure(ErrorCode::MalformedFrame, "register inventory");
    if (!r.string(device_name)) return Status::failure(ErrorCode::MalformedFrame, "register device");
    device_index = r.u32();
    return r.ok() ? Status::success() : Status::failure(ErrorCode::MalformedFrame, r.error());
}

void encode_capacity_update(ByteWriter& w, const ResourceSnapshot& snap) { encode_snapshot(w, snap); }
Status decode_capacity_update(ByteReader& r, ResourceSnapshot& snap) { return decode_snapshot(r, snap); }

void encode_evaluate_request(ByteWriter& w, const AdmissionDescriptor& d) { encode_descriptor(w, d); }
Status decode_evaluate_request(ByteReader& r, AdmissionDescriptor& d) { return decode_descriptor(r, d); }

void encode_evaluate_response(ByteWriter& w, const AdmissionDecision& dec, std::int32_t error_code) {
    encode_decision(w, dec);
    w.i32(error_code);
}
Status decode_evaluate_response(ByteReader& r, AdmissionDecision& dec, std::int32_t& error_code) {
    Status s = decode_decision(r, dec);
    if (!s.ok()) return s;
    error_code = r.i32();
    return r.ok() ? Status::success() : Status::failure(ErrorCode::MalformedFrame, r.error());
}

void encode_commit_request(ByteWriter& w, const AdmissionRequestId& id, const AdmissionGeneration& gen) {
    encode_id(w, id);
    encode_id(w, gen);
}
Status decode_commit_request(ByteReader& r, AdmissionRequestId& id, AdmissionGeneration& gen) {
    if (!decode_id(r, id) || !decode_id(r, gen)) return Status::failure(ErrorCode::MalformedFrame, "commit id");
    return r.ok() ? Status::success() : Status::failure(ErrorCode::MalformedFrame, r.error());
}

void encode_release_request(ByteWriter& w, const ReservationId& id) { encode_id(w, id); }
Status decode_release_request(ByteReader& r, ReservationId& id) {
    if (!decode_id(r, id)) return Status::failure(ErrorCode::MalformedFrame, "release id");
    return r.ok() ? Status::success() : Status::failure(ErrorCode::MalformedFrame, r.error());
}

void encode_observation_submit(ByteWriter& w, const Observation& o) { encode_observation(w, o); }
Status decode_observation_submit(ByteReader& r, Observation& o) { return decode_observation(r, o); }

void encode_error_response(ByteWriter& w, ErrorCode code, const std::string& msg) {
    w.i32(static_cast<std::int32_t>(code));
    w.string(msg);
}
Status decode_error_response(ByteReader& r, ErrorCode& code, std::string& msg) {
    code = static_cast<ErrorCode>(r.i32());
    if (!r.string(msg)) return Status::failure(ErrorCode::MalformedFrame, "error msg");
    return r.ok() ? Status::success() : Status::failure(ErrorCode::MalformedFrame, r.error());
}

} // namespace admission_fabric
