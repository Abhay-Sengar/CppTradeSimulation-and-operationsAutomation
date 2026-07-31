// net.hpp — small blocking-socket helpers shared by the engine and exchange.
//
// Interview topics (email §"Networking"): TCP_NODELAY (disable Nagle so a small
// order isn't buffered waiting for more data — critical for latency),
// SO_BUSY_POLL (have the kernel busy-poll the NIC queue instead of sleeping —
// trades CPU for lower/steadier wakeup latency). Socket buffer sizing is left
// at the tuned sysctl defaults from the kernel-tuning role.
#pragma once
#include "types.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ts::net {

inline void set_nodelay(int fd) noexcept {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Best-effort: needs CAP_NET_ADMIN / privilege, silently ignored otherwise.
inline void try_busy_poll(int fd) noexcept {
#ifdef SO_BUSY_POLL
    int usec = 50;
    ::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec));
#endif
    (void)fd;
}

inline void set_nonblocking(int fd) noexcept {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

[[nodiscard]] inline int connect_tcp(const char* ip, u16 port) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    set_nodelay(fd);
    return fd;
}

[[nodiscard]] inline int listen_tcp(const char* ip, u16 port) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Send the whole buffer, handling partial writes and EINTR. Returns false on a
// hard error / closed peer.
[[nodiscard]] inline bool send_all(int fd, const char* p, std::size_t n) noexcept {
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r > 0) {
            sent += static_cast<std::size_t>(r);
        } else if (r < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// FIX UTC timestamp "YYYYMMDD-HH:MM:SS.mmm" into out (>=24 bytes). Off the hot
// path (used only in logon / exchange replies), so gmtime_r+snprintf is fine.
[[nodiscard]] inline u32 utc_timestamp(char* out) noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    tm g{};
    ::gmtime_r(&ts.tv_sec, &g);
    const int ms = static_cast<int>(ts.tv_nsec / 1'000'000);
    const int n = std::snprintf(out, 24, "%04d%02d%02d-%02d:%02d:%02d.%03d",
                                g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                                g.tm_hour, g.tm_min, g.tm_sec, ms);
    return static_cast<u32>(n);
}

}  // namespace ts::net
