#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    auto r = f.evaluate(afex::desc(AdmissionRequestId(1), 1000, 1000));
    if (!r.has_value()) return 1;
    const AdmissionDecision& d = r.value();
    std::printf("EXAMPLE basic-admit: outcome=%s reason=%s reservation=%s\n",
        outcome_name(d.outcome), reason_code_name(d.reason),
        d.reservation_id.has_value() ? d.reservation_id->str().c_str() : "none");
    return d.is_admit() ? 0 : 1;
}
