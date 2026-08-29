#pragma once

// Admission Fabric - wire protocol.
//
// Messages between coordinator and agents are framed TCP payloads carrying a
// typed body. The authority envelope carries the generations that prove a
// message is not stale; the coordinator validates them in strict order. No raw
// C++ structs cross the wire.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "admission_fabric/decision.hpp"
#include "admission_fabric/descriptor.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/observation.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/serialize.hpp"

namespace admission_fabric {

enum class WireType : std::uint16_t {
    RegisterAgent = 1,
    AgentRegistered = 2,
    CapacityUpdate = 3,
    CapacityAccepted = 4,
    EvaluateRequest = 5,
    EvaluateResponse = 6,
    CommitRequest = 7,
    CommitResponse = 8,
    ReleaseRequest = 9,
    ReleaseResponse = 10,
    SnapshotRequest = 11,
    SnapshotResponse = 12,
    PolicyInstall = 13,
    PolicyAck = 14,
    ObservationSubmit = 15,
    ObservationAck = 16,
    AgentLost = 17,
    Heartbeat = 18,
    Shutdown = 19,
    ErrorResponse = 20
};

inline const char* wire_type_name(WireType t) {
    switch (t) {
        case WireType::RegisterAgent: return "RegisterAgent";
        case WireType::AgentRegistered: return "AgentRegistered";
        case WireType::CapacityUpdate: return "CapacityUpdate";
        case WireType::CapacityAccepted: return "CapacityAccepted";
        case WireType::EvaluateRequest: return "EvaluateRequest";
        case WireType::EvaluateResponse: return "EvaluateResponse";
        case WireType::CommitRequest: return "CommitRequest";
        case WireType::CommitResponse: return "CommitResponse";
        case WireType::ReleaseRequest: return "ReleaseRequest";
        case WireType::ReleaseResponse: return "ReleaseResponse";
        case WireType::SnapshotRequest: return "SnapshotRequest";
        case WireType::SnapshotResponse: return "SnapshotResponse";
        case WireType::PolicyInstall: return "PolicyInstall";
        case WireType::PolicyAck: return "PolicyAck";
        case WireType::ObservationSubmit: return "ObservationSubmit";
        case WireType::ObservationAck: return "ObservationAck";
        case WireType::AgentLost: return "AgentLost";
        case WireType::Heartbeat: return "Heartbeat";
        case WireType::Shutdown: return "Shutdown";
        case WireType::ErrorResponse: return "ErrorResponse";
    }
    return "Unknown";
}

// Authority envelope: the generations proving a message is not stale.
struct AuthorityEnvelope {
    CoordinatorEpoch epoch;
    AgentId agent_id;
    AgentBootId boot_id;
    std::optional<AdmissionRequestId> admission_request_id;
    std::optional<AdmissionGeneration> admission_generation;
    std::optional<ReservationId> reservation_id;
    std::optional<ResourceSnapshotGeneration> snapshot_generation;
};

// A decoded wire message.
struct WireMessage {
    WireType type{WireType::Heartbeat};
    std::uint64_t correlation{0};
    AuthorityEnvelope authority;
    std::vector<std::uint8_t> payload;
};

// --- message body codecs ---------------------------------------------------
void encode_register(ByteWriter& w, const AgentId& agent, const AgentBootId& boot, const Capabilities& caps,
                     const ResourceVector& inventory, const std::string& device_name, std::uint32_t device_index);
Status decode_register(ByteReader& r, AgentId& agent, AgentBootId& boot, Capabilities& caps,
                       ResourceVector& inventory, std::string& device_name, std::uint32_t& device_index);

void encode_capacity_update(ByteWriter& w, const ResourceSnapshot& snap);
Status decode_capacity_update(ByteReader& r, ResourceSnapshot& snap);

void encode_evaluate_request(ByteWriter& w, const AdmissionDescriptor& d);
Status decode_evaluate_request(ByteReader& r, AdmissionDescriptor& d);

void encode_evaluate_response(ByteWriter& w, const AdmissionDecision& dec, std::int32_t error_code);
Status decode_evaluate_response(ByteReader& r, AdmissionDecision& dec, std::int32_t& error_code);

void encode_commit_request(ByteWriter& w, const AdmissionRequestId& id, const AdmissionGeneration& gen);
Status decode_commit_request(ByteReader& r, AdmissionRequestId& id, AdmissionGeneration& gen);

void encode_release_request(ByteWriter& w, const ReservationId& id);
Status decode_release_request(ByteReader& r, ReservationId& id);

void encode_observation_submit(ByteWriter& w, const Observation& o);
Status decode_observation_submit(ByteReader& r, Observation& o);

void encode_error_response(ByteWriter& w, ErrorCode code, const std::string& msg);
Status decode_error_response(ByteReader& r, ErrorCode& code, std::string& msg);

} // namespace admission_fabric
