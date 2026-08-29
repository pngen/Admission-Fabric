#include "admission_fabric/coordinator.hpp"
#include "admission_fabric/policy.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    using namespace admission_fabric;
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    if (argc >= 2) port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    if (argc >= 3) host = argv[2];

    Coordinator c;
    Status s = c.bind(host, port);
    if (!s.ok()) { std::fprintf(stderr, "bind failed: %s\n", s.message().c_str()); return 1; }
    // Install a sane default policy so evaluations are authoritative.
    AdmissionPolicy p;
    p.name = "coordinator-default";
    p.max_snapshot_age = Seconds(30);
    Status sp = c.engine().install_policy(p);
    (void)sp;
    std::printf("LISTENING %s %u\n", host.c_str(), port);
    std::fflush(stdout);
    Status r = c.run();
    if (!r.ok()) std::fprintf(stderr, "coordinator exited: %s\n", r.message().c_str());
    return 0;
}
