#include "common.hpp"
#include "admission_fabric/persistence.hpp"
#include <filesystem>
using namespace admission_fabric;
int main() {
    auto p = std::filesystem::temp_directory_path() / "af_example_state.bin";
    std::filesystem::remove(p);
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    auto r = f.evaluate(afex::desc(AdmissionRequestId(1), 1000, 1000));
    if (!r.has_value()) return 1;
    if (!f.commit_admission(AdmissionRequestId(1), r.value().admission_generation).ok()) return 1;
    StateStore store; PersistenceConfig cfg; cfg.path = p;
    if (!store.save(cfg, f).ok()) return 1;
    AdmissionFabric g;
    if (!store.load(cfg, g).ok()) return 1;
    std::printf("EXAMPLE persistence: recovered lifecycle=%s\n", lifecycle_name(g.request(AdmissionRequestId(1))->lifecycle));
    std::filesystem::remove(p);
    return g.request(AdmissionRequestId(1))->lifecycle == RequestLifecycle::Admitted ? 0 : 1;
}
