#include "test_util.hpp"
#include "admission_fabric/serialize.hpp"
#include <cstdio>
using namespace admission_fabric;
static std::uint64_t gs = 0xF00D;
static std::uint64_t rnd() { std::uint64_t x=gs; x^=x<<13; x^=x>>7; x^=x<<17; gs=x; return x; }
AF_TEST(property_serialization_deterministic_roundtrip) {
    std::printf("  seed=%llu\n", (unsigned long long)gs);
    for (int iter=0; iter<300; ++iter) {
        ByteWriter w;
        int n = static_cast<int>(rnd() % 64);
        for (int i=0;i<n;++i) {
            w.u64(rnd());
            w.string("s"+std::to_string(rnd() % 1000));
            w.f64(static_cast<double>(rnd())/1e9);
            w.flag((rnd()&1)!=0);
        }
        auto framed = ByteWriter::frame(0x11223344u, 1u, w.data());
        std::vector<std::uint8_t> body; std::string err;
        AF_CHECK(ByteReader::parse_frame(framed, 0x11223344u, 1u, body, err));
        AF_CHECK(body.size() == w.data().size());
        // Determinism: re-encode identical buffer -> identical checksum.
        ByteWriter w2; w2.bytes(w.data().data(), w.data().size());
        AF_CHECK(w2.checksum() == w.checksum());
    }
}
AF_TEST_MAIN("admission-fabric-property-serialization");
