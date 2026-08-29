#include "test_util.hpp"
#include "admission_fabric/reservation.hpp"
#include <cstdio>
using namespace admission_fabric;
static std::uint64_t gs = 0xBEEF;
static std::uint64_t rnd() { std::uint64_t x=gs; x^=x<<13; x^=x>>7; x^=x<<17; gs=x; return x; }
AF_TEST(property_reservation_accounting_to_zero) {
    std::printf("  seed=%llu\n", (unsigned long long)gs);
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 10000000);
    ReservationManager m; m.set_total(total);
    for (std::uint64_t i=1;i<=4000;++i) {
        ResourceVector d; d.set(ResourceId::AcceleratorMemory, (rnd() % 500) + 1);
        ReservationId id(i);
        auto r = m.reserve(id, TenantId(0), d, Seconds(100));
        if (r.has_value()) {
            if (rnd()%2==0) { Status c = m.commit(id, AdmissionRequestId(i), AdmissionGeneration(i)); (void)c; }
        }
        if (i % 7 == 0) { for (auto& [rid, rr] : m.reservations()) { if (rr.status==ReservationStatus::Committed || rr.status==ReservationStatus::Reserved) { Status s=m.release(rid); (void)s; break; } } }
        AF_CHECK(m.verify_accounting());
    }
    // Release everything -> exact zero.
    for (auto& [id, r] : m.reservations()) { Status s=m.release(id); (void)s; }
    AF_CHECK(m.reserved().is_zero());
    AF_CHECK(m.used().is_zero());
    AF_CHECK(m.verify_accounting());
}
AF_TEST_MAIN("admission-fabric-property-reservation");
