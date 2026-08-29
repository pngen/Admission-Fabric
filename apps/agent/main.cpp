#include "admission_fabric/agent.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    using namespace admission_fabric;
    // usage: agent <host> <port> <agent_id> <boot_id> <vram_bytes> [device]
    if (argc < 6) { std::fprintf(stderr, "usage: agent host port agent_id boot_id vram_bytes\n"); return 2; }
    std::string host = argv[1];
    std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    AgentId id(std::strtoull(argv[3], nullptr, 10));
    AgentBootId boot(std::strtoull(argv[4], nullptr, 10));
    ByteCount vram = std::strtoull(argv[5], nullptr, 10);
    std::string device = (argc >= 7) ? argv[6] : "cuda";

    Capabilities caps;
    caps.add(Capabilities::backend("cuda"));
    caps.add(Capabilities::dtype("fp16"));
    ResourceVector inv;
    inv.set(ResourceId::AcceleratorMemory, vram);
    inv.set(ResourceId::HostMemory, ByteCount{4} << 30);
    inv.set(ResourceId::PinnedHostMemory, ByteCount{1} << 30);
    inv.set(ResourceId::ExecutionSlots, 64);

    Agent agent(id, boot, caps, inv, device, 0);
    // Retry connect until the coordinator is up.
    Status s;
    for (int attempt = 0; attempt < 200; ++attempt) {
        s = agent.connect(host, port);
        if (s.ok()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!s.ok()) { std::fprintf(stderr, "connect failed: %s\n", s.message().c_str()); return 1; }
    s = agent.register_self();
    if (!s.ok()) { std::fprintf(stderr, "register failed: %s\n", s.message().c_str()); return 1; }
    ResourceSnapshot snap;
    snap.generation = ResourceSnapshotGeneration(boot.value() * 1000 + 1);
    snap.timestamp = SystemClock::now();
    snap.agent_id = id;
    snap.boot_id = boot;
    snap.node_name = device;
    snap.total.set(ResourceId::AcceleratorMemory, vram);
    snap.total.set(ResourceId::HostMemory, ByteCount{4} << 30);
    snap.total.set(ResourceId::PinnedHostMemory, ByteCount{1} << 30);
    snap.total.set(ResourceId::ExecutionSlots, 64);
    snap.capabilities = caps;
    for (auto& p : snap.pressure) p = PressureLevel::Low;
    for (auto& p : snap.provenance) p = Provenance::Measured;
    s = agent.send_capacity(snap);
    if (!s.ok()) { std::fprintf(stderr, "capacity failed: %s\n", s.message().c_str()); return 1; }
    std::printf("REGISTERED %llu\n", static_cast<unsigned long long>(id.value()));
    std::fflush(stdout);
    Status runres = agent.run();
    (void)runres;
    return 0;
}
