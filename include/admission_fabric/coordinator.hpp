#pragma once

// Admission Fabric - coordinator.
//
// The coordinator is the authority: it accepts capacity agents over framed TCP,
// validates their authority (epoch + boot identity), tracks live capacity,
// evaluates admission requests against live agents, and persists authoritative
// state. When an agent dies it fences that agent's capacity generation and rolls
// the coordinator epoch so that stale messages cannot resurrect capacity or
// admit work.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include "admission_fabric/engine.hpp"
#include "admission_fabric/net.hpp"
#include "admission_fabric/protocol.hpp"

namespace admission_fabric {

class Coordinator {
public:
    Coordinator();
    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    [[nodiscard]] Status bind(const std::string& host, std::uint16_t port);
    // Blocking accept loop; runs until stop().
    [[nodiscard]] Status run();
    void stop();
    [[nodiscard]] bool running() const { return running_; }

    [[nodiscard]] CoordinatorEpoch epoch() const { return epoch_; }
    [[nodiscard]] AdmissionFabric& engine() { return engine_; }
    [[nodiscard]] const AdmissionFabric& engine() const { return engine_; }
    [[nodiscard]] std::size_t agent_count() const;
    [[nodiscard]] std::uint16_t port() const { return listener_.local_port(); }

    // In-process helper: perform one blocking accept + handle (used by tests).
    [[nodiscard]] Status accept_and_handle_once();

private:
    struct AgentConn {
        AgentId id;
        AgentBootId boot;
        ResourceSnapshot snapshot;
        NetSocket sock;
        bool registered{false};
        bool alive{false};
        std::uint64_t capacity_generation{0};
    };

    AdmissionFabric engine_;
    NetSocket listener_;
    std::atomic<bool> running_{false};
    CoordinatorEpoch epoch_{1};
    mutable std::mutex mu_;
    std::map<AgentId, std::shared_ptr<AgentConn>> agents_;
    std::thread accept_thread_;

    void dispatch(NetSocket conn);
    void handle_agent(AgentConn& ctx);
    void handle_client(NetSocket conn);
    void handle_client_frame(NetSocket conn, FrameHeader hdr, std::vector<std::uint8_t> payload);
    [[nodiscard]] const ResourceSnapshot* choose_snapshot(const AdmissionDescriptor& d) const;
    void on_agent_loss(const AgentId& id);
    [[nodiscard]] CoordinatorEpoch next_epoch_();
};

} // namespace admission_fabric
