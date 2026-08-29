#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{6} << 30);
    // Ten small requests from tenant 2, then one huge request from tenant 1.
    for (int i = 1; i <= 8; ++i) { auto rr = f.evaluate(afex::desc(AdmissionRequestId(100 + i), 100, 100, TenantId(2))); (void)rr; }
    auto big = f.evaluate(afex::desc(AdmissionRequestId(200), 4096, 4096, TenantId(1)));
    std::printf("EXAMPLE fairness: huge-request outcome=%s reason=%s\n", outcome_name(big.value().outcome), reason_code_name(big.value().reason));
    return 0;
}
