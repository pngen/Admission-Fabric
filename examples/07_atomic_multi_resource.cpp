#include "common.hpp"
using namespace admission_fabric;
int main() {
    ResourceVector total;
    total.set(ResourceId::AcceleratorMemory, 1000);
    total.set(ResourceId::PinnedHostMemory, 500);
    total.set(ResourceId::ExecutionSlots, 8);
    ReservationManager m; m.set_total(total);
    ResourceVector demand;
    demand.set(ResourceId::AcceleratorMemory, 400);
    demand.set(ResourceId::PinnedHostMemory, 400);
    demand.set(ResourceId::ExecutionSlots, 4);
    auto r = m.reserve(ReservationId(1), TenantId(0), demand, Seconds(30));
    std::printf("EXAMPLE atomic-multi-resource: %s\n", r.has_value() ? "reserved" : "failed");
    if (!r.has_value()) return 1;
    if (!m.verify_accounting()) return 1;
    return 0;
}
