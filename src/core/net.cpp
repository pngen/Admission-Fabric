#include "admission_fabric/net.hpp"

#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif

namespace admission_fabric {

#ifdef _WIN32

static bool g_winsock_init = false;
static Status init_winsock_once() {
    if (g_winsock_init) return Status::success();
    WSADATA d;
    int rc = WSAStartup(MAKEWORD(2, 2), &d);
    if (rc != 0) return Status::failure(ErrorCode::SocketError, "WSAStartup failed");
    g_winsock_init = true;
    return Status::success();
}

Status NetSocket::ensure_winsock() { return init_winsock_once(); }

NetSocket::NetSocket() : sock_(reinterpret_cast<void*>(INVALID_SOCKET)) {}
NetSocket::~NetSocket() { close(); }
NetSocket::NetSocket(NetSocket&& other) noexcept : sock_(other.sock_), listener_(other.listener_), peer_closed_(other.peer_closed_) {
    other.sock_ = reinterpret_cast<void*>(INVALID_SOCKET);
    other.listener_ = false;
}
NetSocket& NetSocket::operator=(NetSocket&& other) noexcept {
    if (this != &other) {
        close();
        sock_ = other.sock_;
        listener_ = other.listener_;
        peer_closed_ = other.peer_closed_;
        other.sock_ = reinterpret_cast<void*>(INVALID_SOCKET);
        other.listener_ = false;
        other.peer_closed_ = false;
    }
    return *this;
}
void NetSocket::close() {
    SOCKET s = static_cast<SOCKET>(reinterpret_cast<std::uintptr_t>(sock_));
    if (s != INVALID_SOCKET) { closesocket(s); }
    sock_ = reinterpret_cast<void*>(INVALID_SOCKET);
    listener_ = false;
}

Status NetSocket::connect(const std::string& host, std::uint16_t port) {
    Status w = init_winsock_once();
    if (!w.ok()) return w;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Status::failure(ErrorCode::SocketError, "socket create failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    InetPtonA(AF_INET, host.c_str(), &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(s);
        return Status::failure(ErrorCode::SocketError, "connect failed (" + std::to_string(err) + ")");
    }
    set_socket(static_cast<std::uintptr_t>(s));
    return Status::success();
}

Status NetSocket::bind_listen(const std::string& host, std::uint16_t port) {
    Status w = init_winsock_once();
    if (!w.ok()) return w;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Status::failure(ErrorCode::SocketError, "socket create failed");
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty()) addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else InetPtonA(AF_INET, host.c_str(), &addr.sin_addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(s);
        return Status::failure(ErrorCode::SocketError, "bind failed (" + std::to_string(err) + ")");
    }
    if (::listen(s, SOMAXCONN) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(s);
        return Status::failure(ErrorCode::SocketError, "listen failed (" + std::to_string(err) + ")");
    }
    set_socket(static_cast<std::uintptr_t>(s));
    listener_ = true;
    return Status::success();
}

Status NetSocket::accept(NetSocket& out) const {
    SOCKET s = static_cast<SOCKET>(reinterpret_cast<std::uintptr_t>(sock_));
    SOCKET c = ::accept(s, nullptr, nullptr);
    if (c == INVALID_SOCKET) return Status::failure(ErrorCode::SocketError, "accept failed");
    out.close();
    out.set_socket(static_cast<std::uintptr_t>(c));
    out.listener_ = false;
    out.peer_closed_ = false;
    return Status::success();
}

std::uint16_t NetSocket::local_port() const {
    SOCKET s = static_cast<SOCKET>(reinterpret_cast<std::uintptr_t>(sock_));
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) return 0;
    return ntohs(addr.sin_port);
}

Status NetSocket::send_all(const std::uint8_t* data, std::size_t len) {
    SOCKET s = static_cast<SOCKET>(reinterpret_cast<std::uintptr_t>(sock_));
    if (s == INVALID_SOCKET) { peer_closed_ = true; return Status::failure(ErrorCode::SocketError, "invalid socket"); }
    std::size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, reinterpret_cast<const char*>(data + sent), static_cast<int>((len - sent) & 0x7FFFFFFF), 0);
        if (n == SOCKET_ERROR) { peer_closed_ = true; return Status::failure(ErrorCode::SocketError, "send failed"); }
        if (n == 0) { peer_closed_ = true; return Status::failure(ErrorCode::ConnectionReset, "send returned 0"); }
        sent += static_cast<std::size_t>(n);
    }
    return Status::success();
}

Status NetSocket::recv_exact(std::uint8_t* out, std::size_t len) {
    SOCKET s = static_cast<SOCKET>(reinterpret_cast<std::uintptr_t>(sock_));
    if (s == INVALID_SOCKET) { peer_closed_ = true; return Status::failure(ErrorCode::SocketError, "invalid socket"); }
    std::size_t got = 0;
    while (got < len) {
        int n = ::recv(s, reinterpret_cast<char*>(out + got), static_cast<int>((len - got) & 0x7FFFFFFF), 0);
        if (n == 0) { peer_closed_ = true; return Status::failure(ErrorCode::ConnectionReset, "peer closed"); }
        if (n == SOCKET_ERROR) { peer_closed_ = true; return Status::failure(ErrorCode::ConnectionReset, "recv failed"); }
        got += static_cast<std::size_t>(n);
    }
    return Status::success();
}

static void pack_header(const FrameHeader& h, std::uint8_t buf[kFrameHeaderSize]) {
    std::uint32_t magic = h.magic, plen = h.payload_len;
    std::uint16_t ver = h.version, type = h.type;
    std::uint64_t corr = h.correlation;
    std::memcpy(buf + 0, &magic, 4);
    std::memcpy(buf + 4, &ver, 2);
    std::memcpy(buf + 6, &type, 2);
    std::memcpy(buf + 8, &plen, 4);
    std::memcpy(buf + 12, &corr, 8);
}

static Status unpack_header(const std::uint8_t buf[kFrameHeaderSize], FrameHeader& h) {
    std::memcpy(&h.magic, buf + 0, 4);
    std::memcpy(&h.version, buf + 4, 2);
    std::memcpy(&h.type, buf + 6, 2);
    std::memcpy(&h.payload_len, buf + 8, 4);
    std::memcpy(&h.correlation, buf + 12, 8);
    if (h.magic != kProtocolMagic) return Status::failure(ErrorCode::MalformedFrame, "bad magic");
    if (h.version != kProtocolVersion) return Status::failure(ErrorCode::UnsupportedVersion, "unsupported protocol version");
    if (h.payload_len > kMaxFramePayload) return Status::failure(ErrorCode::FrameTooLarge, "frame too large");
    return Status::success();
}

Status NetSocket::send_frame(const FrameHeader& hdr, const std::vector<std::uint8_t>& payload) {
    FrameHeader h = hdr;
    h.magic = kProtocolMagic;
    h.version = kProtocolVersion;
    h.payload_len = static_cast<std::uint32_t>(payload.size());
    std::uint8_t hdrbuf[kFrameHeaderSize];
    pack_header(h, hdrbuf);
    Status s1 = send_all(hdrbuf, kFrameHeaderSize);
    if (!s1.ok()) return s1;
    if (!payload.empty()) {
        Status s2 = send_all(payload.data(), payload.size());
        if (!s2.ok()) return s2;
    }
    return Status::success();
}

Status NetSocket::recv_frame(FrameHeader& hdr, std::vector<std::uint8_t>& payload) {
    std::uint8_t hdrbuf[kFrameHeaderSize];
    Status s = recv_exact(hdrbuf, kFrameHeaderSize);
    if (!s.ok()) return s;
    Status u = unpack_header(hdrbuf, hdr);
    if (!u.ok()) return u;
    payload.assign(hdr.payload_len, 0);
    if (hdr.payload_len > 0) {
        Status s2 = recv_exact(payload.data(), payload.size());
        if (!s2.ok()) return s2;
    }
    return Status::success();
}

#else
// Non-Windows: sockets are not implemented in this build; failure is explicit.
Status NetSocket::ensure_winsock() { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
NetSocket::NetSocket() = default;
NetSocket::~NetSocket() { close(); }
NetSocket::NetSocket(NetSocket&&) noexcept = default;
NetSocket& NetSocket::operator=(NetSocket&&) noexcept = default;
void NetSocket::close() {}
Status NetSocket::connect(const std::string&, std::uint16_t) { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
Status NetSocket::bind_listen(const std::string&, std::uint16_t) { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
Status NetSocket::accept(NetSocket&) const { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
std::uint16_t NetSocket::local_port() const { return 0; }
Status NetSocket::send_all(const std::uint8_t*, std::size_t) { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
Status NetSocket::recv_exact(std::uint8_t*, std::size_t) { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
Status NetSocket::send_frame(const FrameHeader&, const std::vector<std::uint8_t>&) { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
Status NetSocket::recv_frame(FrameHeader&, std::vector<std::uint8_t>&) { return Status::failure(ErrorCode::NotImplemented, "no socket backend"); }
#endif

Status send_frame(NetSocket& s, std::uint16_t type, std::uint64_t correlation, const std::vector<std::uint8_t>& payload) {
    FrameHeader h;
    h.type = type;
    h.correlation = correlation;
    return s.send_frame(h, payload);
}
Status recv_frame(NetSocket& s, std::uint16_t& type, std::uint64_t& correlation, std::vector<std::uint8_t>& payload) {
    FrameHeader h;
    Status st = s.recv_frame(h, payload);
    if (!st.ok()) return st;
    type = h.type;
    correlation = h.correlation;
    return Status::success();
}

} // namespace admission_fabric
