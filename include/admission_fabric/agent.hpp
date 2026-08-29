#pragma once

// Admission Fabric - capacity agent.
//
// An agent represents an execution node / capacity provider. It connects to the
// coordinator over framed TCP, registers with a stable AgentId and a fresh
// AgentBootId, and continuously reports its resource snapshot. The coordinator
// is the authority; the agent never makes admission decisions.

#include <cstdint>
#include <string>
#include "admission_fabric/net.hpp"
#include "admission_fabric/resource.hpp"

namespace admission_fabric {

class Agent {
public:
    Agent(AgentId id, AgentBootId boot, Capabilities caps, const ResourceVector& inventory,
          std::string device_name, std::uint32_t device_index);
    ~Agent();

    [[nodiscard]] Status connect(const std::string& host, std::uint16_t port);
    [[nodiscard]] Status register_self();
    [[nodiscard]] Status send_capacity(const ResourceSnapshot& snap);
    // Blocking loop: respond to heartbeats until shutdown/disconnect. Returns on exit.
    [[nodiscard]] Status run();
    void stop();

    [[nodiscard]] CoordinatorEpoch epoch() const { return epoch_; }
    [[nodiscard]] bool connected() const { return sock_.valid(); }

private:
    AgentId id_;
    AgentBootId boot_;
    Capabilities caps_;
    ResourceVector inventory_;
    std::string device_name_;
    std::uint32_t device_index_{0};
    NetSocket sock_;
    CoordinatorEpoch epoch_{1};
    bool running_{false};
};

} // namespace admission_fabric
