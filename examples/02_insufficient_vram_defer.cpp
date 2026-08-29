#include "common.hpp"
using namespace admission_fabric;
int main() {
    // 1.2 GiB governed VRAM: the ~1.16 GiB request cannot fit with headroom.
    AdmissionFabric f = afex::engine(ByteCount{1200} << 20);
    auto r = f.evaluate(afex::desc(AdmissionRequestId(2), 2000, 2000));
    const AdmissionDecision& d = r.value();
    std::printf("EXAMPLE insufficient-vram: outcome=%s reason=%s expl=%s\n",
        outcome_name(d.outcome), reason_code_name(d.reason), d.explanation.c_str());
    return d.is_defer() ? 0 : 1;
}
