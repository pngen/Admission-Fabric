#include "admission_fabric/agent.hpp"
#include "admission_fabric/codec.hpp"
#include "admission_fabric/protocol.hpp"

namespace admission_fabric {

Agent::Agent(AgentId id, AgentBootId boot, Capabilities caps, const ResourceVector& inventory,
             std::string device_name, std::uint32_t device_index)
    : id_(id), boot_(boot), caps_(std::move(caps)), inventory_(inventory),
      device_name_(std::move(device_name)), device_index_(device_index) {}
Agent::~Agent() { stop(); }

Status Agent::connect(const std::string& host, std::uint16_t port) {
    Status w = NetSocket::ensure_winsock();
    if (!w.ok()) return w;
    return sock_.connect(host, port);
}

Status Agent::register_self() {
    ByteWriter w;
    encode_register(w, id_, boot_, caps_, inventory_, device_name_, device_index_);
    Status s = admission_fabric::send_frame(sock_, static_cast<std::uint16_t>(WireType::RegisterAgent), 0, w.data());
    if (!s.ok()) return s;
    FrameHeader hdr;
    std::vector<std::uint8_t> payload;
    Status r = sock_.recv_frame(hdr, payload);
    if (!r.ok()) return r;
    if (hdr.type == static_cast<std::uint16_t>(WireType::ErrorResponse)) {
        ErrorCode code; std::string msg;
        ByteReader br(payload);
        decode_error_response(br, code, msg);
        return Status::failure(code, msg);
    }
    if (hdr.type != static_cast<std::uint16_t>(WireType::AgentRegistered))
        return Status::failure(ErrorCode::UnknownMessage, "register not acknowledged");
    ByteReader br(payload);
    epoch_ = CoordinatorEpoch(br.u64());
    return Status::success();
}

Status Agent::send_capacity(const ResourceSnapshot& snap) {
    ByteWriter w;
    w.u64(epoch_.value());
    encode_capacity_update(w, snap);
    Status s2 = admission_fabric::send_frame(sock_, static_cast<std::uint16_t>(WireType::CapacityUpdate), 0, w.data());
    if (!s2.ok()) return s2;
    FrameHeader hdr;
    std::vector<std::uint8_t> payload;
    Status r = sock_.recv_frame(hdr, payload);
    if (!r.ok()) return r;
    if (hdr.type == static_cast<std::uint16_t>(WireType::ErrorResponse)) {
        ErrorCode code; std::string msg;
        ByteReader br(payload);
        decode_error_response(br, code, msg);
        return Status::failure(code, msg);
    }
    if (hdr.type != static_cast<std::uint16_t>(WireType::CapacityAccepted))
        return Status::failure(ErrorCode::UnknownMessage, "capacity not accepted");
    return Status::success();
}

Status Agent::run() {
    running_ = true;
    while (running_ && sock_.valid()) {
        FrameHeader hdr;
        std::vector<std::uint8_t> payload;
        Status s = sock_.recv_frame(hdr, payload);
        if (!s.ok()) break;
        if (hdr.type == static_cast<std::uint16_t>(WireType::Heartbeat)) {
            ByteWriter w; w.u64(epoch_.value());
            admission_fabric::send_frame(sock_, static_cast<std::uint16_t>(WireType::Heartbeat), hdr.correlation, w.data());
        } else if (hdr.type == static_cast<std::uint16_t>(WireType::Shutdown)) {
            break;
        } else if (hdr.type == static_cast<std::uint16_t>(WireType::ErrorResponse)) {
            // The coordinator rejected something (e.g., stale epoch); surface it.
            ErrorCode code; std::string msg;
            ByteReader br(payload);
            if (decode_error_response(br, code, msg).ok()) {
                // Re-connect after a coordinator epoch roll.
                // This agent has gone stale; caller should restart with a fresh boot id.
            }
        }
    }
    running_ = false;
    return Status::success();
}

void Agent::stop() {
    running_ = false;
    sock_.close();
}

} // namespace admission_fabric
