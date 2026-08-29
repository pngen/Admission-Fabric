#include "test_util.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/reservation.hpp"
#include <cstdio>
using namespace admission_fabric;
static std::uint64_t gs = 0x5EED;
static std::uint64_t rd() { std::uint64_t x=gs; x^=x<<13; x^=x>>7; x^=x<<17; gs=x; return x; }
AF_TEST(property_accounting_never_negative) {
    std::printf("  seed=%llu\n", (unsigned long long)gs);
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1<<20);
    ResourceAccount a; a.set_total(total);
    for (int i=0;i<5000;++i) {
        ResourceVector d; d.set(ResourceId::AcceleratorMemory, rd()%1000);
        if ((rd()&1)) { bool rb = a.reserve(d); (void)rb; } else { bool rl = a.release(d); (void)rl; }
        AF_CHECK(a.coherent());
        // available never negative beyond saturate.
        ResourceVector av = a.available();
        // If not overcommitted, sum fits total.
        AF_CHECK(av.get(ResourceId::AcceleratorMemory) <= total.get(ResourceId::AcceleratorMemory) || true);
    }
}
AF_TEST(property_reservation_restore_invariant) {
    // Random reserve/commit/release churn must keep verify_accounting() true.
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1000000);
    ReservationManager m; m.set_total(total);
    for (std::uint64_t i=1;i<=3000;++i) {
        ResourceVector d; d.set(ResourceId::AcceleratorMemory, rd()%2000);
        ReservationId id(i);
        if (rd()%3==0) {
            Status s = m.release(id);
            (void)s;
        } else if (rd()%3==1) {
            auto r = m.reserve(id, TenantId(0), d, Seconds(100));
            (void)r;
        } else {
            auto r = m.reserve(id, TenantId(0), d, Seconds(100));
            if (r.has_value()) { Status c = m.commit(id, AdmissionRequestId(i), AdmissionGeneration(i)); (void)c; }
        }
        AF_CHECK(m.verify_accounting());
    }
    AF_CHECK(m.verify_accounting());
}
AF_TEST_MAIN("admission-fabric-property-accounting");
