#include "test_util.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/units.hpp"
#include "admission_fabric/checked.hpp"
#include "admission_fabric/resource.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/version.hpp"
#include "admission_fabric/backends/cpu_backend.hpp"

using namespace admission_fabric;

AF_TEST(strong_id_is_typed) {
    AdmissionRequestId a(7);
    AdmissionGeneration g(7);
    // Distinct domains are distinct types even for equal underlying values.
    static_assert(!std::is_convertible_v<AdmissionRequestId, AdmissionGeneration>);
    AF_CHECK(a.value() == 7);
    AF_CHECK(a != AdmissionRequestId(8));
    AF_CHECK(AdmissionRequestId(7) == AdmissionRequestId(7));
    AF_CHECK(a.str() == "7");
    AF_CHECK(!AdmissionRequestId().is_nil() == false);
}

AF_TEST(id_next_saturates) {
    AdmissionGeneration g(1);
    AF_CHECK(g.next().value() == 2);
    constexpr auto m = (std::numeric_limits<std::uint64_t>::max)();
    AdmissionGeneration near(m);
    AF_CHECK(near.next().value() == m);  // saturates, never wraps to zero
}

AF_TEST(checked_arithmetic) {
    std::uint64_t o = 0;
    AF_CHECK(checked_add(3, 4, o) && o == 7);
    AF_CHECK(!checked_add((std::numeric_limits<std::uint64_t>::max)(), 1, o));
    AF_CHECK(checked_sub(10, 3, o) && o == 7);
    AF_CHECK(!checked_sub(2, 3, o));
    AF_CHECK(checked_mul(5, 6, o) && o == 30);
    AF_CHECK(!checked_mul((std::numeric_limits<std::uint64_t>::max)(), 2, o));
}

AF_TEST(resource_vector_arithmetic) {
    ResourceVector a, b;
    a.set(ResourceId::AcceleratorMemory, 100);
    b.set(ResourceId::AcceleratorMemory, 50);
    b.set(ResourceId::HostMemory, 20);
    ResourceVector s;
    AF_CHECK(a.try_add(b, s));
    AF_CHECK(s.get(ResourceId::AcceleratorMemory) == 150);
    AF_CHECK(s.get(ResourceId::HostMemory) == 20);
    ResourceVector d;
    AF_CHECK(s.try_sub(b, d));
    AF_CHECK(d.get(ResourceId::AcceleratorMemory) == 100);
    AF_CHECK(d.get(ResourceId::HostMemory) == 0);
    // overflow
    ResourceVector big;
    big.set(ResourceId::AcceleratorMemory, (std::numeric_limits<std::uint64_t>::max)());
    AF_CHECK(!a.try_add(big, s));
    // fits
    ResourceVector limit;
    limit.set(ResourceId::AcceleratorMemory, 100);
    limit.set(ResourceId::HostMemory, 20);
    ResourceVector req;
    req.set(ResourceId::AcceleratorMemory, 90);
    AF_CHECK(req.fits_in(limit));
    ResourceVector req2;
    req2.set(ResourceId::HostMemory, 25);
    AF_CHECK(!req2.fits_in(limit));
}

AF_TEST(resource_account_coherent) {
    ResourceVector total;
    total.set(ResourceId::AcceleratorMemory, 1000);
    ResourceVector demand;
    demand.set(ResourceId::AcceleratorMemory, 400);
    ResourceAccount acct;
    acct.set_total(total);
    AF_CHECK(acct.reserve(demand));
    AF_CHECK(acct.coherent());
    AF_CHECK(acct.headroom().get(ResourceId::AcceleratorMemory) == 600);
    ResourceVector demand2;
    demand2.set(ResourceId::AcceleratorMemory, 700);
    AF_CHECK(!acct.reserve(demand2));  // exceeds available
    AF_CHECK(acct.commit(demand));       // provisional -> committed
    AF_CHECK(acct.release(demand));      // committed work released
    AF_CHECK(acct.headroom().get(ResourceId::AcceleratorMemory) == 1000);  // fully returned to zero
    AF_CHECK(acct.coherent());
}

AF_TEST(capabilities_required_subset) {
    Capabilities avail;
    avail.add(Capabilities::backend("cuda"));
    avail.add(Capabilities::compute_capability("12.0"));
    avail.add(Capabilities::dtype("fp16"));
    Capabilities required;
    required.add(Capabilities::backend("cuda"));
    required.add(Capabilities::dtype("fp16"));
    AF_CHECK(avail.contains_all(required));
    required.add(Capabilities::quantization("int8"));
    AF_CHECK(!avail.contains_all(required));
}

AF_TEST(version_present) {
    AF_CHECK(std::string(version_string()) == "1.0.0");
    AF_CHECK(runtime_version() != nullptr);
}

AF_TEST_MAIN("admission-fabric-smoke");
