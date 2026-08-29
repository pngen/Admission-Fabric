#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    QuotaSnapshot q(true);
    q.generation = 1; q.timestamp = SystemClock::now();
    q.limits.set(ResourceId::AcceleratorMemory, ByteCount{4} << 30);
    q.token_budget = 500;                 // request uses 2000 tokens
    f.set_quota(q);
    auto r = f.evaluate(afex::desc(AdmissionRequestId(4), 1000, 1000));
    std::printf("EXAMPLE quota-denial: reason=%s expl=%s\n", reason_code_name(r.value().reason), r.value().explanation.c_str());
    return r.value().is_reject() ? 0 : 1;
}
