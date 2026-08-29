#pragma once

// Admission Fabric - canonical serialization.
//
// No raw C++ structs are ever written to disk or the wire. Every structure is
// encoded field-by-field with explicit length prefixes, bounded counts, enum
// range checks, and a CRC32 checksum. Readers reject truncation, unknown
// versions, impossible counts, trailing bytes, duplicate identities and
// overflow. Round-trips are property-tested.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "admission_fabric/error.hpp"

namespace admission_fabric {

// CRC32 (IEEE 802.3, reflected) over a byte range.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept;
inline std::uint32_t crc32(const std::vector<std::uint8_t>& v) {
    return crc32(v.data(), v.size());
}

// A grow-only byte writer with a running CRC (CRC computed over the final
// payload before the trailer is appended).
class ByteWriter {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v & 0xFF)); u8(static_cast<std::uint8_t>((v >> 8) & 0xFF)); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f64(double v) { std::uint64_t bits = 0; std::memcpy(&bits, &v, sizeof(bits)); u64(bits); }
    void flag(bool b) { u8(b ? 1 : 0); }
    void bytes(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void raw(const std::string& s) { bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
    // length-prefixed (u32 count) bounded string
    void string(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s);
    }
    // length-prefixed (u32 count) vector of uint64
    void u64vec(const std::vector<std::uint64_t>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (auto x : v) u64(x);
    }
    // length-prefixed vector of strings
    void stringvec(const std::vector<std::string>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (auto& s : v) string(s);
    }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const { return buf_; }
    [[nodiscard]] std::size_t size() const { return buf_.size(); }
    [[nodiscard]] std::uint32_t checksum() const { return crc32(buf_.data(), buf_.size()); }

    // Build a framed buffer: [payload][magic][version][len][crc].
    static std::vector<std::uint8_t> frame(std::uint32_t magic, std::uint32_t version, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> out = payload;
        ByteWriter w;
        w.u32(magic);
        w.u32(version);
        w.u32(static_cast<std::uint32_t>(payload.size()));
        w.u32(crc32(payload.data(), payload.size()));
        out.insert(out.end(), w.data().begin(), w.data().end());
        return out;
    }

private:
    std::vector<std::uint8_t> buf_;
};

// A bounds-checking reader over a byte buffer.
class ByteReader {
public:
    explicit ByteReader(const std::uint8_t* data, std::size_t len) : data_(data), len_(len) {}
    explicit ByteReader(const std::vector<std::uint8_t>& v) : data_(v.data()), len_(v.size()) {}

    [[nodiscard]] bool ok() const { return !failed_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] std::size_t remaining() const { return pos_ < len_ ? len_ - pos_ : 0; }
    [[nodiscard]] std::size_t consumed() const { return pos_; }

    // Primitive reads. On failure they set failed_ and return 0.
    std::uint8_t u8() { need(1); if (failed_) return 0; return data_[pos_++]; }
    std::uint16_t u16() { std::uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= static_cast<std::uint16_t>(u8()) << (8 * i); return v; }
    std::uint32_t u32() { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i); return v; }
    std::uint64_t u64() { std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i); return v; }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double f64() { std::uint64_t bits = u64(); double v = 0; std::memcpy(&v, &bits, sizeof(v)); return v; }
    bool flag() { return u8() != 0; }

    bool bytes(std::uint8_t* out, std::size_t n) {
        need(n);
        if (failed_) return false;
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    // length-prefixed string (bounded)
    bool string(std::string& out) {
        std::uint32_t n = u32();
        if (failed_ || n > kMaxLen) { fail("length-prefixed string too large"); return false; }
        need(n);
        if (failed_) return false;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return true;
    }
    // length-prefixed vector of uint64 (bounded count)
    bool u64vec(std::vector<std::uint64_t>& out) {
        std::uint32_t n = u32();
        if (failed_ || n > kMaxCount) { fail("count too large"); return false; }
        out.clear();
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!failed_) out.push_back(u64());
            else return false;
        }
        return !failed_;
    }
    bool stringvec(std::vector<std::string>& out) {
        std::uint32_t n = u32();
        if (failed_ || n > kMaxCount) { fail("count too large"); return false; }
        out.clear();
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string s;
            if (!string(s)) return false;
            out.push_back(std::move(s));
        }
        return !failed_;
    }

    // Validate a trailer: returns true if magic/version/len/crc all match.
    bool verify_trailer(std::uint32_t magic, std::uint32_t version) {
        std::uint32_t m = u32(), ver = u32(), len = u32(), crc = u32();
        if (failed_) return false;
        if (m != magic) { error_ = "bad magic"; failed_ = true; return false; }
        if (ver != version) { error_ = "bad version"; failed_ = true; return false; }
        if (len != pos_ - 16) { error_ = "length mismatch"; failed_ = true; return false; }
        // CRC over everything before the trailer.
        std::uint32_t actual = crc32(data_, pos_ - 16);
        if (actual != crc) { error_ = "checksum mismatch"; failed_ = true; return false; }
        return true;
    }

    // Parse a framed payload produced by ByteWriter::frame.
    static bool parse_frame(const std::vector<std::uint8_t>& framed, std::uint32_t magic, std::uint32_t version,
                            std::vector<std::uint8_t>& payload, std::string& err) {
        if (framed.size() < 16) { err = "frame too small"; return false; }
        payload.assign(framed.begin(), framed.end() - 16);
        ByteReader tr(framed.data() + framed.size() - 16, 16);
        std::uint32_t m = tr.u32(), ver = tr.u32(), len = tr.u32(), crc = tr.u32();
        if (m != magic) { err = "bad magic"; return false; }
        if (ver != version) { err = "bad version"; return false; }
        if (len != payload.size()) { err = "length mismatch"; return false; }
        if (crc32(payload.data(), payload.size()) != crc) { err = "checksum mismatch"; return false; }
        return true;
    }

    static constexpr std::uint32_t kMaxLen = 64 * 1024 * 1024;
    static constexpr std::uint32_t kMaxCount = 1u << 20;

private:
    const std::uint8_t* data_;
    std::size_t len_;
    std::size_t pos_{0};
    bool failed_{false};
    std::string error_;

    void need(std::size_t n) {
        if (failed_) return;
        if (n > len_ - pos_) { fail("unexpected end of buffer"); }
    }
    void fail(const std::string& e) { if (!failed_) { failed_ = true; error_ = e; } }
};

} // namespace admission_fabric
