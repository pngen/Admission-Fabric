#include "admission_fabric/coordinator.hpp"
#include "admission_fabric/codec.hpp"

#include <algorithm>

namespace admission_fabric {

Coordinator::Coordinator() { NetSocket::ensure_winsock(); }
Coordinator::~Coordinator() { stop(); }

Status Coordinator::bind(const std::string& host, std::uint16_t port) {
    return listener_.bind_listen(host, port);
}

CoordinatorEpoch Coordinator::next_epoch_() { return epoch_.next(); }

void Coordinator::stop() {
    running_ = false;
    listener_.close();
    if (accept_thread_.joinable()) accept_thread_.join();
}

Status Coordinator::run() {
    running_ = true;
    while (running_) {
        NetSocket conn;
        Status s = listener_.accept(conn);
        if (!s.ok()) return s;
        std::thread([this, c = std::move(conn)]() mutable { dispatch(std::move(c)); }).detach();
    }
    return Status::success();
}

Status Coordinator::accept_and_handle_once() {
    NetSocket conn;
    Status s = listener_.accept(conn);
    if (!s.ok()) return s;
    dispatch(std::move(conn));
    return Status::success();
}

std::size_t Coordinator::agent_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return agents_.size();
}

void Coordinator::dispatch(NetSocket conn) {
    FrameHeader hdr;
    std::vector<std::uint8_t> payload;
    Status s = conn.recv_frame(hdr, payload);
    if (!s.ok()) return;
    if (hdr.type == static_cast<std::uint16_t>(WireType::RegisterAgent)) {
        AgentId id; AgentBootId boot; Capabilities caps; ResourceVector inv;
        std::string dev; std::uint32_t idx = 0;
        ByteReader r(payload);
        Status ds = decode_register(r, id, boot, caps, inv, dev, idx);
        if (!ds.ok()) return;
        auto ctx = std::make_shared<AgentConn>();
        ctx->id = id; ctx->boot = boot; ctx->sock = std::move(conn);
        ctx->registered = true; ctx->alive = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = agents_.find(id);
            if (it != agents_.end() && it->second->alive && it->second->boot != boot) {
                // A live agent claiming a different boot identity is stale.
                ByteWriter w; encode_error_response(w, ErrorCode::AgentBootMismatch, "stale or mismatched agent boot identity");
                admission_fabric::send_frame(ctx->sock, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                return;
            }
            agents_[id] = ctx;   // replaces a dead (fenced) agent with a fresh boot
        }
        ByteWriter w;
        w.u64(epoch_.value());
        admission_fabric::send_frame(ctx->sock, static_cast<std::uint16_t>(WireType::AgentRegistered), hdr.correlation, w.data());
        handle_agent(*ctx);
    } else {
        // Client/control connection: re-dispatch the first frame then loop.
        handle_client_frame(std::move(conn), hdr, payload);
    }
}

void Coordinator::handle_agent(AgentConn& ctx) {
    for (;;) {
        FrameHeader hdr;
        std::vector<std::uint8_t> payload;
        Status s = ctx.sock.recv_frame(hdr, payload);
        if (!s.ok()) { on_agent_loss(ctx.id); return; }
        if (hdr.type == static_cast<std::uint16_t>(WireType::Heartbeat)) {
            ByteWriter w; w.u64(epoch_.value());
            admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::Heartbeat), hdr.correlation, w.data());
            continue;
        }
        if (hdr.type == static_cast<std::uint16_t>(WireType::CapacityUpdate)) {
            ByteReader r(payload);
            std::uint64_t epoch_val = r.u64();
            ResourceSnapshot snap;
            Status ds = decode_capacity_update(r, snap);
            if (!ds.ok()) {
                ByteWriter w; encode_error_response(w, ErrorCode::MalformedFrame, "bad capacity update");
                admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                continue;
            }
            // Authority: epoch must be current, boot must match the registered agent.
            if (epoch_val != epoch_.value()) {
                ByteWriter w; encode_error_response(w, ErrorCode::StaleEpoch, "stale coordinator epoch");
                admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                continue;
            }
            if (snap.boot_id != ctx.boot || snap.agent_id != ctx.id) {
                ByteWriter w; encode_error_response(w, ErrorCode::StaleBootIdentity, "stale agent boot identity");
                admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                continue;
            }
            if (ctx.capacity_generation != 0 && snap.generation.value() <= ctx.capacity_generation) {
                ByteWriter w; encode_error_response(w, ErrorCode::StaleSnapshotGeneration, "stale resource snapshot generation");
                admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                continue;
            }
            ctx.snapshot = snap;
            ctx.capacity_generation = snap.generation.value();
            ByteWriter w; w.u64(snap.generation.value());
            admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::CapacityAccepted), hdr.correlation, w.data());
            continue;
        }
        // Unknown message from an agent.
        ByteWriter w; encode_error_response(w, ErrorCode::UnknownMessage, wire_type_name(static_cast<WireType>(hdr.type)));
        admission_fabric::send_frame(ctx.sock, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
    }
}

void Coordinator::handle_client_frame(NetSocket conn, FrameHeader hdr, std::vector<std::uint8_t> payload) {
    for (;;) {
        switch (static_cast<WireType>(hdr.type)) {
            case WireType::EvaluateRequest: {
                ByteReader r(payload);
                AdmissionDescriptor d;
                Status ds = decode_evaluate_request(r, d);
                if (!ds.ok()) {
                    ByteWriter w; encode_error_response(w, ErrorCode::MalformedFrame, "bad evaluate request");
                    admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                    break;
                }
                const ResourceSnapshot* snap = choose_snapshot(d);
                if (snap == nullptr) {
                    // No live agent with the required capability: engine defers.
                    ResourceSnapshot empty;
                    empty.generation = ResourceSnapshotGeneration(epoch_.value());
                    empty.timestamp = SystemClock::now();
                    for (auto& p : empty.provenance) p = Provenance::Unavailable;
                    engine_.set_snapshot(empty);
                } else {
                    engine_.set_snapshot(*snap);
                }
                auto rr = engine_.evaluate(d);
                ByteWriter w;
                if (rr.has_value()) {
                    encode_evaluate_response(w, rr.value(), 0);
                } else {
                    AdmissionDecision dec;
                    encode_evaluate_response(w, dec, static_cast<std::int32_t>(rr.code()));
                }
                admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::EvaluateResponse), hdr.correlation, w.data());
                break;
            }
            case WireType::CommitRequest: {
                AdmissionRequestId id; AdmissionGeneration gen;
                ByteReader r(payload);
                Status ds = decode_commit_request(r, id, gen);
                if (!ds.ok()) {
                    ByteWriter w; encode_error_response(w, ErrorCode::MalformedFrame, "bad commit request");
                    admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                    break;
                }
                Status cs = engine_.commit_admission(id, gen);
                ByteWriter w; w.i32(static_cast<std::int32_t>(cs.code()));
                admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::CommitResponse), hdr.correlation, w.data());
                break;
            }
            case WireType::ReleaseRequest: {
                ReservationId id;
                ByteReader r(payload);
                Status ds = decode_release_request(r, id);
                if (!ds.ok()) {
                    ByteWriter w; encode_error_response(w, ErrorCode::MalformedFrame, "bad release request");
                    admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                    break;
                }
                Status rs = engine_.release_reservation(id);
                ByteWriter w; w.i32(static_cast<std::int32_t>(rs.code()));
                admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ReleaseResponse), hdr.correlation, w.data());
                break;
            }
            case WireType::ObservationSubmit: {
                Observation o;
                ByteReader r(payload);
                Status ds = decode_observation_submit(r, o);
                if (!ds.ok()) {
                    ByteWriter w; encode_error_response(w, ErrorCode::MalformedFrame, "bad observation");
                    admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                    break;
                }
                engine_.observe(o);
                ByteWriter w; w.i32(0);
                admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ObservationAck), hdr.correlation, w.data());
                break;
            }
            case WireType::Shutdown: {
                ByteWriter w; w.i32(0);
                admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::Shutdown), hdr.correlation, w.data());
                return;
            }
            default: {
                ByteWriter w; encode_error_response(w, ErrorCode::UnknownMessage, wire_type_name(static_cast<WireType>(hdr.type)));
                admission_fabric::send_frame(conn, static_cast<std::uint16_t>(WireType::ErrorResponse), hdr.correlation, w.data());
                break;
            }
        }
        FrameHeader nh;
        Status s = conn.recv_frame(nh, payload);
        if (!s.ok()) return;
        hdr = nh;
    }
}

const ResourceSnapshot* Coordinator::choose_snapshot(const AdmissionDescriptor& d) const {
    std::lock_guard<std::mutex> lk(mu_);
    const ResourceSnapshot* best = nullptr;
    std::uint64_t best_free = 0;
    for (auto& [id, a] : agents_) {
        if (!a->alive) continue;
        Capabilities req;
        if (!d.backend.empty()) req.add(Capabilities::backend(d.backend));
        if (!d.dtype.empty()) req.add(Capabilities::dtype(d.dtype));
        if (!a->snapshot.capabilities.contains_all(req)) continue;
        std::uint64_t free = a->snapshot.headroom().get(ResourceId::AcceleratorMemory);
        if (!best || free > best_free) { best = &a->snapshot; best_free = free; }
    }
    return best;
}

void Coordinator::on_agent_loss(const AgentId& id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = agents_.find(id);
        if (it != agents_.end() && it->second->alive) {
            it->second->alive = false;   // fence this agent's capacity generation
            changed = true;
        }
    }
    if (changed) {
        // Roll coordinator epoch so any pre-loss authority is invalidated.
        epoch_ = epoch_.next();
    }
}

} // namespace admission_fabric
