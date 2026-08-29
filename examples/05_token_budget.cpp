#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    QuotaSnapshot q(true); q.timestamp = SystemClock::now(); q.token_budget = 5000;
    f.set_quota(q);
    AdmissionDescriptor d = afex::desc(AdmissionRequestId(5), 1000, 1000);
    d.max_output_tokens = 2000;           // 1000+1000=2000 <= 5000
    auto r = f.evaluate(d);
    std::printf("EXAMPLE token-budget: outcome=%s reason=%s\n", outcome_name(r.value().outcome), reason_code_name(r.value().reason));
    return r.value().is_admit() ? 0 : 1;
}
