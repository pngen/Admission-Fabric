#include "common.hpp"
using namespace admission_fabric;
int main() {
    AdmissionFabric f = afex::engine(ByteCount{8} << 30);
    PolicyGeneration g1 = f.policy()->generation;
    AdmissionPolicy p = afex::policy(); p.name = "stricter"; p.safety_headroom_ratio = 0.5;
    if (!f.install_policy(p).ok()) return 1;
    PolicyGeneration g2 = f.policy()->generation;
    std::printf("EXAMPLE policy-replacement: %llu -> %llu (versioned=%s)\n",
        (unsigned long long)g1.value(), (unsigned long long)g2.value(), g2.value() > g1.value() ? "yes" : "no");
    return g2.value() > g1.value() ? 0 : 1;
}
