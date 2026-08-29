#include "test_util.hpp"
#include "admission_fabric/agent.hpp"
#include "admission_fabric/codec.hpp"
#include "admission_fabric/coordinator.hpp"
#include "admission_fabric/decision.hpp"
#include "admission_fabric/net.hpp"
#include "admission_fabric/policy.hpp"
#include "admission_fabric/protocol.hpp"
#include "admission_fabric/resource.hpp"

#include <chrono>
#include <thread>

using namespace admission_fabric;

// A tiny raw client helper that sends a typed frame and awaits a matching reply.
struct Client {
    NetSocket sock;
    std::uint64_t corr{1};

    Status connect(std::uint16_t port) { return sock.connect("127.0.0.1", port); }

    Status send(WireType t, const std::vector<std::uint8_t>& payload) {
        return admission_fabric::send_frame(sock, static_cast<std::uint16_t>(t), corr, payload);
    }
    Status recv(WireType& t, std::vector<std::uint8_t>& payload) {
        std::uint16_t type; std::uint64_t c;
        Status s = admission_fabric::recv_frame(sock, type, c, payload);
        if (!s.ok()) return s;
        t = static_cast<WireType>(type);
        return Status::success();
    }
    Status request(WireType t, const std::vector<std::uint8_t>& payload, WireType expect, std::vector<std::uint8_t>& out) {
        Status s1 = send(t, payload);
        if (!s1.ok()) return s1;
        WireType rt; std::vector<std::uint8_t> p;
        Status s2 = recv(rt, p);
        if (!s2.ok()) return s2;
        if (rt != expect) return Status::failure(ErrorCode::UnknownMessage, "unexpected reply type");
        out = std::move(p);
        return Status::success();
    }
};

static AdmissionDescriptor mkdesc(AdmissionRequestId id, TenantId tenant) {
    AdmissionDescriptor d;
    d.admission_request_id = id;
    d.request_id = RequestId(id.value() * 10 + 7);
    d.attempt_id = AttemptId(1);
    d.tenant_id = tenant;
    d.model_id = ModelId(7);
    d.model_revision = ModelRevision(1);
    d.policy_generation = PolicyGeneration(1);
    d.input_tokens = 1000;
    d.expected_output_tokens = 1000;
    d.max_output_tokens = 1000;
    d.backend = "cuda";
    d.dtype = "fp16";
    d.latency_class = LatencyClass::BestEffort;
    return d;
}

static ResourceSnapshot mk_snap(AgentId agent, AgentBootId boot, std::uint64_t gen, ByteCount vram) {
    ResourceSnapshot s;
    s.generation = ResourceSnapshotGeneration(gen);
    s.timestamp = SystemClock::now();
    s.agent_id = agent;
    s.boot_id = boot;
    s.node_name = "node";
    s.total.set(ResourceId::AcceleratorMemory, vram);
    s.total.set(ResourceId::HostMemory, ByteCount{4} << 30);
    s.total.set(ResourceId::PinnedHostMemory, ByteCount{1} << 30);
    s.total.set(ResourceId::ExecutionSlots, 64);
    s.capabilities.add(Capabilities::backend("cuda"));
    s.capabilities.add(Capabilities::dtype("fp16"));
    for (auto& p : s.pressure) p = PressureLevel::Low;
    for (auto& p : s.provenance) p = Provenance::Measured;
    return s;
}

AF_TEST(multiprocess_restart_fencing) {
    Coordinator c;
    Status bs = c.bind("127.0.0.1", 0);
    AF_CHECK(bs.ok());
    std::uint16_t port = c.port();
    AF_CHECK(port != 0);
    AdmissionPolicy pol;
    pol.name = "mp";
    pol.max_snapshot_age = Seconds(30);
    AF_CHECK(c.engine().install_policy(pol).ok());

    std::thread coord([&] { Status runres = c.run(); (void)runres; });

    // Agent A + B register.
    Agent a(AgentId(1), AgentBootId(100), [&] { Capabilities x; x.add(Capabilities::backend("cuda")); x.add(Capabilities::dtype("fp16")); return x; }(),
            [&] { ResourceVector v; v.set(ResourceId::AcceleratorMemory, ByteCount{6} << 30); return v; }(), "cuda", 0);
    AF_CHECK(a.connect("127.0.0.1", port).ok());
    AF_CHECK(a.register_self().ok());
    AF_CHECK(a.send_capacity(mk_snap(AgentId(1), AgentBootId(100), 1001, ByteCount{6} << 30)).ok());

    Agent b(AgentId(2), AgentBootId(200), [&] { Capabilities x; x.add(Capabilities::backend("cuda")); x.add(Capabilities::dtype("fp16")); return x; }(),
            [&] { ResourceVector v; v.set(ResourceId::AcceleratorMemory, ByteCount{6} << 30); return v; }(), "cuda", 0);
    AF_CHECK(b.connect("127.0.0.1", port).ok());
    AF_CHECK(b.register_self().ok());
    AF_CHECK(b.send_capacity(mk_snap(AgentId(2), AgentBootId(200), 2001, ByteCount{6} << 30)).ok());
    AF_CHECK(c.agent_count() == 2);
    CoordinatorEpoch epoch_before = c.epoch();

    // Client submits a request that requires a reservation.
    Client client;
    AF_CHECK(client.connect(port).ok());
    AdmissionDescriptor d1 = mkdesc(AdmissionRequestId(1000), TenantId(1));
    ByteWriter w1; encode_evaluate_request(w1, d1);
    std::vector<std::uint8_t> out;
    Status rs = client.request(WireType::EvaluateRequest, w1.data(), WireType::EvaluateResponse, out);
    AF_CHECK(rs.ok());
    AdmissionDecision dec; std::int32_t errc = 99;
    ByteReader rd(out);
    AF_CHECK(decode_evaluate_response(rd, dec, errc).ok());
    AF_CHECK(errc == 0);
    AF_CHECK(dec.is_admit());
    AF_CHECK(dec.reservation_id.has_value());
    ReservationId res = *dec.reservation_id;
    AdmissionGeneration adm_gen = dec.admission_generation;

    // 1. Kill agent A (close socket) -> coordinator must fence + roll epoch.
    a.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    AF_CHECK(c.epoch() != epoch_before);   // epoch rolled

    // 2. Restart the same logical agent A with a NEW boot id.
    Agent a2(AgentId(1), AgentBootId(300), [&] { Capabilities x; x.add(Capabilities::backend("cuda")); x.add(Capabilities::dtype("fp16")); return x; }(),
             [&] { ResourceVector v; v.set(ResourceId::AcceleratorMemory, ByteCount{6} << 30); return v; }(), "cuda", 0);
    AF_CHECK(a2.connect("127.0.0.1", port).ok());
    AF_CHECK(a2.register_self().ok());
    AF_CHECK(a2.send_capacity(mk_snap(AgentId(1), AgentBootId(300), 3001, ByteCount{6} << 30)).ok());

    // 3. Stale boot identity: a NEW connection claiming the OLD boot (100) for a
    //    now-live agent must be rejected.
    {
        NetSocket stale;
        AF_CHECK(stale.connect("127.0.0.1", port).ok());
        ByteWriter w;
        encode_register(w, AgentId(1), AgentBootId(100),
            [&] { Capabilities x; x.add(Capabilities::backend("cuda")); return x; }(),
            [&] { ResourceVector v; v.set(ResourceId::AcceleratorMemory, ByteCount{1} << 30); return v; }(), "cuda", 0);
        AF_CHECK(admission_fabric::send_frame(stale, static_cast<std::uint16_t>(WireType::RegisterAgent), 7, w.data()).ok());
        std::uint16_t type; std::uint64_t c4; std::vector<std::uint8_t> p;
        AF_CHECK(admission_fabric::recv_frame(stale, type, c4, p).ok());
        AF_CHECK(static_cast<WireType>(type) == WireType::ErrorResponse);
        ErrorCode code; std::string msg;
        ByteReader er(p);
        AF_CHECK(decode_error_response(er, code, msg).ok());
        AF_CHECK(code == ErrorCode::AgentBootMismatch || code == ErrorCode::AgentAlreadyRegistered);
    }

    // 4. Stale coordinator epoch: a capacity update carrying the pre-restart epoch
    //    on the restarted agent's connection must be rejected.
    {
        ByteWriter w;
        w.u64(epoch_before.value());       // stale epoch
        ResourceSnapshot s = mk_snap(AgentId(1), AgentBootId(300), 3002, ByteCount{6} << 30);
        encode_capacity_update(w, s);
        // Use a fresh raw connection that registers first, then sends the stale update.
        NetSocket raw;
        AF_CHECK(raw.connect("127.0.0.1", port).ok());
        ByteWriter rw;
        encode_register(rw, AgentId(3), AgentBootId(400), [&]{ Capabilities x; x.add(Capabilities::backend("cuda")); return x; }(),
            [&]{ ResourceVector v; v.set(ResourceId::AcceleratorMemory, ByteCount{1} << 30); return v; }(), "cuda", 0);
        AF_CHECK(admission_fabric::send_frame(raw, static_cast<std::uint16_t>(WireType::RegisterAgent), 8, rw.data()).ok());
        std::uint16_t t; std::uint64_t cc; std::vector<std::uint8_t> p;
        AF_CHECK(admission_fabric::recv_frame(raw, t, cc, p).ok());
        AF_CHECK(static_cast<WireType>(t) == WireType::AgentRegistered);
        // Now the stale capacity update with the OLD epoch.
        AF_CHECK(admission_fabric::send_frame(raw, static_cast<std::uint16_t>(WireType::CapacityUpdate), 9, w.data()).ok());
        AF_CHECK(admission_fabric::recv_frame(raw, t, cc, p).ok());
        AF_CHECK(static_cast<WireType>(t) == WireType::ErrorResponse);
        ErrorCode code; std::string msg;
        ByteReader er(p);
        AF_CHECK(decode_error_response(er, code, msg).ok());
        AF_CHECK(code == ErrorCode::StaleEpoch);
    }

    // 5. Commit with a stale admission generation must be rejected.
    {
        ByteWriter w;
        encode_commit_request(w, AdmissionRequestId(1000), adm_gen.next());
        std::vector<std::uint8_t> out2;
        Status r = client.request(WireType::CommitRequest, w.data(), WireType::CommitResponse, out2);
        AF_CHECK(r.ok());
        ByteReader cr(out2);
        std::int32_t ec = cr.i32();
        AF_CHECK(ec == static_cast<std::int32_t>(ErrorCode::StaleAdmissionGeneration));
    }

    // 6. Release with a stale / unknown reservation authority rejected.
    {
        ByteWriter w;
        encode_release_request(w, ReservationId(999999));
        std::vector<std::uint8_t> out2;
        Status r = client.request(WireType::ReleaseRequest, w.data(), WireType::ReleaseResponse, out2);
        AF_CHECK(r.ok());
        ByteReader cr(out2);
        std::int32_t ec = cr.i32();
        AF_CHECK(ec == static_cast<std::int32_t>(ErrorCode::StaleReservation));
    }

    // 7. Fresh authoritative admission succeeds.
    AdmissionDescriptor d2 = mkdesc(AdmissionRequestId(2000), TenantId(2));
    ByteWriter w2; encode_evaluate_request(w2, d2);
    std::vector<std::uint8_t> out2;
    Status rs2 = client.request(WireType::EvaluateRequest, w2.data(), WireType::EvaluateResponse, out2);
    AF_CHECK(rs2.ok());
    AdmissionDecision dec2; std::int32_t ec2;
    ByteReader rd2(out2);
    AF_CHECK(decode_evaluate_response(rd2, dec2, ec2).ok());
    AF_CHECK(ec2 == 0);
    AF_CHECK(dec2.is_admit());
    AF_CHECK(dec2.reservation_id.has_value());

    // 8. Release the fresh reservation and assert exact accounting-to-zero.
    {
        ByteWriter w;
        encode_release_request(w, *dec2.reservation_id);
        std::vector<std::uint8_t> out3;
        Status r = client.request(WireType::ReleaseRequest, w.data(), WireType::ReleaseResponse, out3);
        AF_CHECK(r.ok());
        ByteReader cr(out3);
        std::int32_t ec = cr.i32();
        AF_CHECK(ec == static_cast<std::int32_t>(ErrorCode::Ok));
    }
    // In-process authoritative check: the engine accounting is empty for the fresh reservation.
    AF_CHECK(c.engine().reservations().verify_accounting());

    // Cleanup.
    b.stop();
    a2.stop();
    c.stop();
    coord.join();
}

AF_TEST_MAIN("admission-fabric-restart");
