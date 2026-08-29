#include "test_util.hpp"
#include "admission_fabric/reservation.hpp"
using namespace admission_fabric;
AF_TEST(reservation_atomic_and_rollback) {
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1000);
    ReservationManager m; m.set_total(total);
    ResourceVector d1; d1.set(ResourceId::AcceleratorMemory, 400);
    ResourceVector d2; d2.set(ResourceId::AcceleratorMemory, 300);
    auto r1 = m.reserve(ReservationId(1), TenantId(0), d1, Seconds(10));
    AF_CHECK(r1.has_value());
    auto r2 = m.reserve(ReservationId(2), TenantId(0), d2, Seconds(10));
    AF_CHECK(r2.has_value());
    AF_CHECK(m.verify_accounting());
    // A third request that would exceed capacity must NOT partially reserve.
    ResourceVector d3; d3.set(ResourceId::AcceleratorMemory, 500);
    auto r3 = m.reserve(ReservationId(3), TenantId(0), d3, Seconds(10));
    AF_CHECK(!r3.has_value());
    AF_CHECK(m.verify_accounting());
    // Rollback r2 returns exact prior state.
    AF_CHECK(m.rollback(ReservationId(2)).ok());
    AF_CHECK(m.reserved().get(ResourceId::AcceleratorMemory) == 400);
    AF_CHECK(m.verify_accounting());
    // Commit r1 -> used.
    AF_CHECK(m.commit(ReservationId(1), AdmissionRequestId(77), AdmissionGeneration(1)).ok());
    AF_CHECK(m.used().get(ResourceId::AcceleratorMemory) == 400);
    // Double commit rejected.
    AF_CHECK(!m.commit(ReservationId(1), AdmissionRequestId(77), AdmissionGeneration(1)).ok());
}
AF_TEST(reservation_double_release_idempotent) {
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1000);
    ReservationManager m; m.set_total(total);
    ResourceVector d; d.set(ResourceId::AcceleratorMemory, 400);
    auto r = m.reserve(ReservationId(1), TenantId(0), d, Seconds(10));
    AF_CHECK(r.has_value());
    AF_CHECK(m.commit(ReservationId(1), AdmissionRequestId(1), AdmissionGeneration(1)).ok());
    AF_CHECK(m.release(ReservationId(1)).ok());
    AF_CHECK(m.release(ReservationId(1)).ok());   // idempotent
    AF_CHECK(m.used().is_zero());
    AF_CHECK(m.verify_accounting());
}
AF_TEST(reservation_identity_collision) {
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1000);
    ReservationManager m; m.set_total(total);
    ResourceVector d; d.set(ResourceId::AcceleratorMemory, 100);
    AF_CHECK(m.reserve(ReservationId(1), TenantId(0), d, Seconds(10)).has_value());
    AF_CHECK(!m.reserve(ReservationId(1), TenantId(0), d, Seconds(10)).has_value());
}
AF_TEST(reservation_bounded_overcommit) {
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 500);
    ReservationManager m; m.set_total(total);
    ResourceVector allow; allow.set(ResourceId::AcceleratorMemory, 200);
    m.set_overcommit_mode(OvercommitMode::Bounded);
    m.set_overcommit_allowance(allow);
    ResourceVector d; d.set(ResourceId::AcceleratorMemory, 600);
    AF_CHECK(m.reserve(ReservationId(1), TenantId(0), d, Seconds(10)).has_value());
    AF_CHECK(m.overcommitted());
    AF_CHECK(m.availability().get(ResourceId::AcceleratorMemory) == 100);
}
AF_TEST_MAIN("admission-fabric-reservation");
