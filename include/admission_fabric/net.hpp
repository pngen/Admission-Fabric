#pragma once

// Admission Fabric - framed TCP transport.
//
// A small, correct, fully-buffered TCP abstraction built on Winsock. Frames are
// self-describing: [magic][version][type][payload_len][correlation] followed by
// payload. Send/recv loops are complete (never assume one recv == one message);
// partial frames are carried across calls; malformed/truncated/oversized frames
// are rejected. Winsock startup/cleanup and socket lifetime are handled here.

#include <cstdint>
#include <string>
#include <vector>
#include "admission_fabric/error.hpp"

namespace admission_fabric {

// Frame header layout (field-by-field, no struct packing):
//   u32 magic, u16 version, u16 type, u32 payload_len, u64 correlation
constexpr std::size_t kFrameHeaderSize = 24;
constexpr std::uint32_t kProtocolMagic = 0xAD1FAB00u;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint32_t kMaxFramePayload = 16u * 1024u * 1024u;  // 16 MiB hard cap

struct FrameHeader {
    std::uint32_t magic{kProtocolMagic};
    std::uint16_t version{kProtocolVersion};
    std::uint16_t type{0};
    std::uint32_t payload_len{0};
    std::uint64_t correlation{0};
};

class NetSocket {
public:
    NetSocket();
    ~NetSocket();
    NetSocket(const NetSocket&) = delete;
    NetSocket& operator=(const NetSocket&) = delete;
    NetSocket(NetSocket&& other) noexcept;
    NetSocket& operator=(NetSocket&& other) noexcept;

    // One-time Winsock initialization (idempotent).
    static Status ensure_winsock();

    [[nodiscard]] bool valid() const { return sock_ != nullptr; }
    [[nodiscard]] bool is_listener() const { return listener_; }
    void close();

    [[nodiscard]] Status connect(const std::string& host, std::uint16_t port);
    [[nodiscard]] Status bind_listen(const std::string& host, std::uint16_t port);
    [[nodiscard]] Status accept(NetSocket& out) const;
    [[nodiscard]] std::uint16_t local_port() const;

    // Complete send: may loop. Returns false on closed/rejected socket.
    [[nodiscard]] Status send_all(const std::uint8_t* data, std::size_t len);
    // Complete receive of exactly len bytes (loops over partial reads).
    [[nodiscard]] Status recv_exact(std::uint8_t* out, std::size_t len);

    // Framed helpers on top of send_all/recv_exact.
    [[nodiscard]] Status send_frame(const FrameHeader& hdr, const std::vector<std::uint8_t>& payload);
    [[nodiscard]] Status recv_frame(FrameHeader& hdr, std::vector<std::uint8_t>& payload);

    [[nodiscard]] bool peer_closed() const { return peer_closed_; }

private:
    void* sock_{nullptr};               // SOCKET stored as void* to keep winsock out of header
    // On Windows INVALID_SOCKET is (~0); stored as a non-null void*, and
    // NetSocket()'s ctor sets it to reinterpret_cast<void*>(INVALID_SOCKET).
    bool listener_{false};
    bool peer_closed_{false};
    void set_socket(std::uintptr_t s) { sock_ = reinterpret_cast<void*>(s); }
    [[nodiscard]] std::uintptr_t socket_value() const { return reinterpret_cast<std::uintptr_t>(sock_); }
};

// Convenience: send a frame with a raw payload vector.
Status send_frame(NetSocket& s, std::uint16_t type, std::uint64_t correlation, const std::vector<std::uint8_t>& payload);
[[nodiscard]] Status recv_frame(NetSocket& s, std::uint16_t& type, std::uint64_t& correlation, std::vector<std::uint8_t>& payload);

} // namespace admission_fabric
