#include "common.hpp"
using namespace admission_fabric;
int main() {
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 500);
    ReservationManager m; m.set_total(total);
    ResourceVector d; d.set(ResourceId::AcceleratorMemory, 300);
    auto r = m.reserve(ReservationId(1), TenantId(0), d, Milliseconds(0));  // instant TTL
    if (!r.has_value()) return 1;
    m.expire_stale(SystemClock::now() + Milliseconds(1));
    std::printf("EXAMPLE reservation-expiry: status=%s reserved=%llu\n",
        reservation_status_name(m.find(ReservationId(1))->status),
        (unsigned long long)m.reserved().get(ResourceId::AcceleratorMemory));
    return m.reserved().get(ResourceId::AcceleratorMemory) == 0 ? 0 : 1;
}
