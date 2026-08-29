// Distributed coordinator + two capacity agents over framed TCP.
// This example shows the in-process coordinator/server topology using real
// sockets; the OS-process coordinator/agent applications are built as
// admission-fabric-coordinator and admission-fabric-agent.
#include "common.hpp"
#include "admission_fabric/agent.hpp"
#include "admission_fabric/coordinator.hpp"
#include "admission_fabric/protocol.hpp"
#include <thread>
using namespace admission_fabric;
int main() {
    Coordinator c;
    if (!c.bind("127.0.0.1", 0).ok()) return 1;
    std::uint16_t port = c.port();
    AdmissionPolicy p = afex::policy(); p.max_snapshot_age = Seconds(30);
    Status sp = c.engine().install_policy(p); (void)sp;
    std::thread t([&] { Status s = c.run(); (void)s; });
    auto caps = [] { Capabilities x; x.add(Capabilities::backend("cuda")); x.add(Capabilities::dtype("fp16")); return x; }();
    ResourceVector inv; inv.set(ResourceId::AcceleratorMemory, ByteCount{8} << 30);
    Agent a(AgentId(1), AgentBootId(1), caps, inv, "cuda", 0);
    if (!a.connect("127.0.0.1", port).ok()) return 1;
    if (!a.register_self().ok()) return 1;
    ResourceSnapshot s;
    s.generation = ResourceSnapshotGeneration(1); s.timestamp = SystemClock::now();
    s.agent_id = AgentId(1); s.boot_id = AgentBootId(1); s.node_name = "cuda";
    s.total.set(ResourceId::AcceleratorMemory, ByteCount{8} << 30);
    s.total.set(ResourceId::ExecutionSlots, 64);
    s.capabilities = caps;
    for (auto& pr : s.pressure) pr = PressureLevel::Low;
    for (auto& pr : s.provenance) pr = Provenance::Measured;
    if (!a.send_capacity(s).ok()) return 1;
    std::printf("EXAMPLE distributed: agents=%zu port=%u\n", c.agent_count(), port);
    a.stop(); c.stop(); t.join();
    return 0;
}
