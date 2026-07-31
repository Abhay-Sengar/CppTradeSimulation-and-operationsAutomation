// exchange_main.cpp — a minimal FIX 4.2 acceptor (mock exchange).
//
// Behaviour matches the original Python mock_exchange.py: ack the logon, and
// fill every NewOrderSingle immediately with an ExecutionReport. C++ rewrite so
// the whole round trip is native and the connection handler can be pinned to an
// isolated core (cpu 14).
//
// Threads: main = acceptor; each connection is served by a handler thread pinned
// to the exchange core. One client at a time (the engine).
#include "affinity.hpp"
#include "config.hpp"
#include "fix.hpp"
#include "net.hpp"
#include "types.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace ts;

namespace {
volatile std::sig_atomic_t g_run = 1;
int g_listen_fd = -1;

void on_signal(int) {
    g_run = 0;
    if (g_listen_fd >= 0) ::shutdown(g_listen_fd, SHUT_RDWR);
}

struct Cfg {
    int  cpu = 14;
    bool pin = false;
    bool rt = false;
    int  rt_prio = 80;
};

Cfg parse(int argc, char** argv) {
    Cfg c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--cpu=", 0) == 0) c.cpu = std::atoi(a.c_str() + 6);
        else if (a == "--pin=on") c.pin = true;
        else if (a == "--pin=off") c.pin = false;
        else if (a == "--rt=on") c.rt = true;
        else if (a.rfind("--rtprio=", 0) == 0) c.rt_prio = std::atoi(a.c_str() + 9);
    }
    return c;
}

// Compose and send one message. Body starts with 35 (required first field),
// then session tags, then the caller's body tags.
template <class BodyFn>
bool send_msg(int fd, fix::Builder& b, u64& seq, std::string_view mtype, BodyFn body) {
    char tsbuf[24];
    const u32 tslen = net::utc_timestamp(tsbuf);
    b.reset();
    b.add(35, mtype);
    b.add(49, "EXCHANGE");
    b.add(56, "TRADER");
    b.add(34, static_cast<i64>(seq++));
    b.add(52, std::string_view(tsbuf, tslen));
    body(b);
    char out[512];
    const u32 n = b.finalize(out);
    return net::send_all(fd, out, n);
}

void handle(int fd, const Cfg& c) {
    if (c.pin) {
        if (pin_to_cpu(c.cpu) && affinity_is(c.cpu)) {
            if (c.rt && !set_realtime(c.rt_prio))
                std::fprintf(stderr, "[exchange] SCHED_FIFO denied (need CAP_SYS_NICE)\n");
        } else {
            std::fprintf(stderr, "[exchange] pin to cpu %d failed\n", c.cpu);
        }
    }
    net::set_nodelay(fd);
    net::try_busy_poll(fd);

    fix::Framer framer;
    fix::Builder b;
    u64 seq = 1;
    char rbuf[4096];

    while (g_run) {
        const ssize_t r = ::recv(fd, rbuf, sizeof(rbuf), 0);
        if (r <= 0) break;
        framer.append(rbuf, static_cast<std::size_t>(r));

        while (auto msg = framer.next()) {
            const fix::View v(*msg);
            const std::string_view mtype = v.get(35);

            if (mtype == "A") {  // logon -> ack
                (void)send_msg(fd, b, seq, "A", [](fix::Builder& m) {
                    m.add(98, 0);
                    m.add(108, 30);
                });
                std::fprintf(stderr, "[exchange] logon ack\n");
            } else if (mtype == "D") {  // NewOrderSingle -> instant fill
                const std::string_view clordid = v.get(11);
                const std::string_view sym = v.get(55);
                const std::string_view side = v.get(54);
                const std::string_view qty = v.get(38);
                const std::string_view px = v.get(44);
                (void)send_msg(fd, b, seq, "8", [&](fix::Builder& m) {
                    m.add(37, static_cast<i64>(seq));         // OrderID
                    m.add(11, clordid);                        // echo ClOrdID
                    m.add(17, static_cast<i64>(seq));          // ExecID
                    m.add(150, "2");                            // ExecType=Filled
                    m.add(39, "2");                             // OrdStatus=Filled
                    m.add(55, sym.empty() ? std::string_view("NIFTY") : sym);
                    m.add(54, side.empty() ? std::string_view("1") : side);
                    m.add(38, qty.empty() ? std::string_view("1") : qty);
                    m.add(32, qty.empty() ? std::string_view("1") : qty);  // LastQty
                    m.add(31, px.empty() ? std::string_view("0") : px);    // LastPx
                });
            } else if (mtype == "1") {  // TestRequest -> Heartbeat
                (void)send_msg(fd, b, seq, "0", [](fix::Builder&) {});
            }
        }
    }
    ::close(fd);
    std::fprintf(stderr, "[exchange] client disconnected\n");
}

}  // namespace

int main(int argc, char** argv) {
    const Cfg c = parse(argc, argv);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    g_listen_fd = net::listen_tcp("127.0.0.1", kExchangePort);
    if (g_listen_fd < 0) {
        std::fprintf(stderr, "[exchange] listen on 127.0.0.1:%u failed\n", kExchangePort);
        return 1;
    }
    std::fprintf(stderr, "[exchange] listening on 127.0.0.1:%u (cpu=%d pin=%d rt=%d)\n",
                 kExchangePort, c.cpu, c.pin, c.rt);

    while (g_run) {
        const int fd = ::accept(g_listen_fd, nullptr, nullptr);
        if (fd < 0) break;
        std::fprintf(stderr, "[exchange] client connected\n");
        std::thread t(handle, fd, std::cref(c));
        t.join();  // one client (the engine) at a time
    }
    ::close(g_listen_fd);
    return 0;
}
