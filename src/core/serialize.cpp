#include "admission_fabric/serialize.hpp"

namespace admission_fabric {

namespace {
const std::uint32_t* crc_table() {
    static std::uint32_t table[256];
    static const bool init = [] {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    return table;
}
} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept {
    if (data == nullptr || len == 0) return 0xFFFFFFFFu;
    const std::uint32_t* t = crc_table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) crc = t[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

} // namespace admission_fabric
