#include "common.hpp"
#include "admission_fabric/persistence.hpp"
#include <filesystem>
using namespace admission_fabric;
int main() {
    // Persistence + recovery (server-side coordinator crash safety).
    auto p = std::filesystem::temp_directory_path() / "af_ex14.bin";
    std::filesystem::remove(p);
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    auto rr = f.evaluate(afex::desc(AdmissionRequestId(7), 1000, 1000)); (void)rr;
    StateStore store; PersistenceConfig cfg; cfg.path = p;
    if (!store.save(cfg, f).ok()) return 1;
    AdmissionFabric g;
    if (!store.load(cfg, g).ok()) return 1;
    std::printf("EXAMPLE persistence-recovery: requests=%zu\n", g.request_count());
    std::filesystem::remove(p);
    return g.request_count() > 0 ? 0 : 1;
}
