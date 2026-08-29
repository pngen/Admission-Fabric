#include "common.hpp"
#include "admission_fabric/backends/cuda_backend.hpp"
using namespace admission_fabric;
int main() {
    CudaBackend be;
    int devs = 0;
    if (!CudaBackend::device_count(devs).ok() || devs < 1) { std::printf("no CUDA device\n"); return 2; }
    if (!be.init(0).ok()) { std::printf("cuda init failed\n"); return 2; }
    AdmissionFabric f = afex::engine(ByteCount{2} << 30);   // 2 GiB governed VRAM
    auto r1 = f.evaluate(afex::desc(AdmissionRequestId(1), 2000, 2000));
    if (!r1.value().is_admit()) return 1;
    void* ptr = nullptr;
    if (!be.allocate(ptr, static_cast<std::size_t>(r1.value().predicted_requirement.demand().get(ResourceId::AcceleratorMemory))).ok()) return 1;
    if (!be.run_and_verify_add(1u << 20).ok()) return 1;
    if (!be.free(ptr).ok()) return 1;
    auto r2 = f.evaluate(afex::desc(AdmissionRequestId(2), 2000, 2000));
    std::printf("EXAMPLE cuda-governed: r1=%s r2=%s\n", reason_code_name(r1.value().reason), reason_code_name(r2.value().reason));
    return r2.value().is_defer() ? 0 : 1;
}
