#include "test_util.hpp"
#include "admission_fabric/reservation.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
using namespace admission_fabric;
AF_TEST(concurrency_reservation_churn) {
    ResourceVector total; total.set(ResourceId::AcceleratorMemory, 1000000);
    ReservationManager m; m.set_total(total);
    std::mutex mu;
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> next_id{1};
    const int kThreads = 8;
    const std::uint64_t kPerThread = 2000;
    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&]{
            for (std::uint64_t i = 0; i < kPerThread; ++i) {
                std::uint64_t idv = next_id.fetch_add(1);
                ResourceVector d; d.set(ResourceId::AcceleratorMemory, (idv % 500) + 1);
                std::lock_guard<std::mutex> lk(mu);
                auto r = m.reserve(ReservationId(idv), TenantId(0), d, Seconds(100));
                if (r.has_value()) {
                    Status c = m.commit(ReservationId(idv), AdmissionRequestId(idv), AdmissionGeneration(idv));
                    if (c.ok()) committed++;
                    else failed++;
                } else failed++;
            }
        });
    }
    for (auto& th : ths) th.join();
    // Some reservations are still committed (used); release them all and assert zero.
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [id, r] : m.reservations()) {
        Status s = m.release(id); (void)s;
    }
    AF_CHECK(m.verify_accounting());
    AF_CHECK(m.used().is_zero());
    AF_CHECK(m.verify_accounting());
    std::printf("  committed=%llu failed=%llu reservations=%llu\n", (unsigned long long)committed.load(), (unsigned long long)failed.load(), (unsigned long long)m.reservation_count());
}
AF_TEST_MAIN("admission-fabric-concurrency");
