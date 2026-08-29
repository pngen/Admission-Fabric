#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    AdmissionDescriptor d = afex::desc(AdmissionRequestId(6), 1000, 100000);
    d.deadline = Milliseconds(20);
    auto r = f.evaluate(d);
    std::printf("EXAMPLE slo-infeasible: reason=%s slo=%s\n", reason_code_name(r.value().reason), slo_verdict_name(r.value().slo.verdict));
    return r.value().is_reject() ? 0 : 1;
}
