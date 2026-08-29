#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    AdmissionDescriptor d = afex::desc(AdmissionRequestId(3), 1000, 1000);
    d.backend = "tpu"; d.dtype = "bf16";   // capability the node does not have
    auto r = f.evaluate(d);
    std::printf("EXAMPLE hard-rejection: reason=%s expl=%s\n", reason_code_name(r.value().reason), r.value().explanation.c_str());
    return r.value().is_reject() ? 0 : 1;
}
