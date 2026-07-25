// engine_main.cpp — the trading engine: a 3-hop, lock-free pipeline across
// isolated cores, with runtime A/B toggles and a Prometheus metrics endpoint.
//
//   market-data (cpu 8) --ring1--> strategy (cpu 10) --ring2--> gateway (cpu 12)
//        |                                                          |  FIX/TCP
//        | rdtsc t0 rides the whole way                            v  loopback
//        +------------------ tick-to-trade ------------------> exchange (cpu 14)
//
//   gateway --ring3--> logger (housekeeping)      metrics-http (housekeeping)
//
// Two latencies are measured (both start at the market-data rdtsc stamp):
//   * pipeline      : tick -> gateway about to send  (in-process, sub-us; this
//                     is where the CRTP/alloc/huge A/B deltas are visible)
//   * tick_to_trade : tick -> ExecutionReport in      (full, dominated by the
//                     loopback TCP round trip — the kernel-bypass motivation)
//
// Runtime flags (no rebuild needed for the demo):
//   --dispatch=crtp|virtual   --alloc=pool|malloc   --huge=on|off
//   --pin=on|off  --rt=on|off  --rate=<ticks/sec>
#include "affinity.hpp"
#include "config.hpp"
#include "fix.hpp"
#include "hist.hpp"
#include "huge.hpp"
#include "messages.hpp"
#include "net.hpp"
#include "pool.hpp"
#include "spsc.hpp"
#include "strategy.hpp"
#include "tsc.hpp"
#include "types.hpp"
#include "util.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <thread>

using namespace ts;

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
struct Cfg {
    bool dispatch_crtp = true;   // --dispatch=crtp|virtual
    bool use_pool = true;        // --alloc=pool|malloc
    bool huge = true;            // --huge=on|off
    bool pin = false;            // --pin=on|off  (off in dev, on under systemd)
    bool rt = false;             // --rt=on|off   (SCHED_FIFO)
    u32  rate = 1000;            // --rate=<ticks/sec>
    int  md_cpu = 8, strat_cpu = 10, gw_cpu = 12, rt_prio = 80;
    const char* exch_ip = "127.0.0.1";
    const char* log_path = "/tmp/trading-engine.log";
};

Cfg parse(int argc, char** argv) {
    Cfg c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dispatch=virtual") c.dispatch_crtp = false;
        else if (a == "--dispatch=crtp") c.dispatch_crtp = true;
        else if (a == "--alloc=malloc") c.use_pool = false;
        else if (a == "--alloc=pool") c.use_pool = true;
        else if (a == "--huge=off") c.huge = false;
        else if (a == "--huge=on") c.huge = true;
        else if (a == "--pin=on") c.pin = true;
        else if (a == "--rt=on") c.rt = true;
        else if (a.rfind("--rate=", 0) == 0) c.rate = static_cast<u32>(std::atoi(a.c_str() + 7));
        else if (a.rfind("--md-cpu=", 0) == 0) c.md_cpu = std::atoi(a.c_str() + 9);
        else if (a.rfind("--strat-cpu=", 0) == 0) c.strat_cpu = std::atoi(a.c_str() + 12);
        else if (a.rfind("--gw-cpu=", 0) == 0) c.gw_cpu = std::atoi(a.c_str() + 9);
        else if (a.rfind("--exch-ip=", 0) == 0) c.exch_ip = argv[i] + 10;
        else if (a.rfind("--log=", 0) == 0) c.log_path = argv[i] + 6;
    }
    return c;
}

// All hot state in one struct so it can be placed on huge pages in one shot.
struct HotState {
    SpscRing<Tick, ring_capacity()>      ring1;  // md -> strategy
    SpscRing<Order, ring_capacity()>     ring2;  // strategy -> gateway
    SpscRing<LogEvent, ring_capacity()>  ring3;  // gateway -> logger
    Histogram pipeline;
    Histogram t2t;
    ObjectPool<OrderCtx, 4096> pool;
    u64 pending[pending_capacity()];             // clordid & MASK -> t0_tsc
    std::atomic<u64> orders{0};
    std::atomic<u64> fills{0};
};
constexpr u64 kPendingMask = pending_capacity() - 1;

// Try to pin + go real-time on the calling thread; log what actually happened.
void setup_core(const char* name, const Cfg& c, int cpu) {
    if (!c.pin) return;
    if (pin_to_cpu(cpu) && affinity_is(cpu)) {
        if (c.rt && !set_realtime(c.rt_prio))
            std::fprintf(stderr, "[%s] SCHED_FIFO denied (need CAP_SYS_NICE)\n", name);
    } else {
        std::fprintf(stderr, "[%s] pin to cpu %d failed\n", name, cpu);
    }
}

// Encode a NewOrderSingle for `o` into `out`; returns its length.
u32 encode_nos(fix::Builder& b, const Order& o, char* out) noexcept {
    b.reset();
    b.add(35, "D");
    b.add(49, "TRADER");
    b.add(56, "EXCHANGE");
    b.add(34, static_cast<i64>(o.clordid));
    b.add(11, static_cast<i64>(o.clordid));
    b.add(55, kSymbols[static_cast<std::size_t>(o.sym_id)]);
    b.add(54, static_cast<i64>(static_cast<u8>(o.side)));
    b.add(38, o.qty);
    b.add(40, "2");  // OrdType = Limit
    b.add(44, o.px);
    return b.finalize(out);
}

// ---- market-data: paced synthetic tick generator --------------------------
void run_market_data(HotState& hs, const Cfg& c, const TscClock& clk) {
    setup_core("market-data", c, c.md_cpu);
    const double period_ns = 1e9 / static_cast<double>(c.rate);
    const u64 period_ticks = static_cast<u64>(period_ns / clk.ns_per_tick());
    u64 next = rdtsc_fast();
    u32 sym = 0, seq = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        const u64 now = rdtsc_fast();
        if (now < next) { cpu_relax(); continue; }
        next += period_ticks;
        const i64 wiggle = (static_cast<i64>(seq % 20) - 10) * 5;
        const i64 mid = kLimits.base_price + wiggle;
        Tick t{rdtsc_fast(), sym, seq, mid - 5, mid + 5};
        while (!hs.ring1.try_push(t) && g_running.load(std::memory_order_relaxed))
            cpu_relax();
        sym = (sym + 1) % kNumSymbols;
        ++seq;
    }
}

// ---- strategy: templated on the strategy type so CRTP inlines and virtual
//      genuinely dispatches through the vtable. Runs the --alloc A/B. ---------
template <class Strat>
void run_strategy(Strat* strat, HotState& hs, const Cfg& c) {
    setup_core("strategy", c, c.strat_cpu);
    fix::Builder builder;
    char stackbuf[160];
    u64 clordid = 1;
    Tick t;
    while (g_running.load(std::memory_order_relaxed)) {
        if (!hs.ring1.try_pop(t)) { cpu_relax(); continue; }

        // Per-order scratch: pool vs malloc. Encoding into it (then copying the
        // bytes into the Order that gets sent) gives the allocation an
        // observable effect, so -O3 cannot elide the malloc path.
        OrderCtx* ctx = c.use_pool
                            ? hs.pool.acquire()
                            : static_cast<OrderCtx*>(::operator new(sizeof(OrderCtx),
                                                                    std::nothrow));
        Order o = strat->on_tick(t, clordid);
        char* enc = ctx ? ctx->fix_buf : stackbuf;
        const u32 n = encode_nos(builder, o, enc);
        o.fix_len = n;
        std::memcpy(o.fix, enc, n);

        while (!hs.ring2.try_push(o) && g_running.load(std::memory_order_relaxed))
            cpu_relax();

        if (ctx) {
            if (c.use_pool) hs.pool.release(ctx);
            else ::operator delete(ctx);
        }
        ++clordid;
    }
}

// ---- gateway: send NOS, match ExecutionReports, record both histograms -----
void run_gateway(HotState& hs, const Cfg& c, const TscClock& clk, int fd) {
    setup_core("gateway", c, c.gw_cpu);
    fix::Framer framer;
    char rbuf[8192];
    const u64 warmup = kLimits.warmup_orders;

    while (g_running.load(std::memory_order_relaxed)) {
        bool did_work = false;

        Order o;
        if (hs.ring2.try_pop(o)) {
            did_work = true;
            const u64 pdt = rdtsc_fast() - o.t0_tsc;            // tick -> send
            if (hs.orders.load(std::memory_order_relaxed) >= warmup)
                hs.pipeline.record(clk.ticks_to_ns(pdt));
            hs.pending[o.clordid & kPendingMask] = o.t0_tsc;
            (void)net::send_all(fd, o.fix, o.fix_len);
            hs.orders.fetch_add(1, std::memory_order_relaxed);

            LogEvent le{rdtsc_fast(), LogType::OrderSent, 0, {}};
            const int m = std::snprintf(le.msg, sizeof(le.msg), "NOS id=%llu",
                                        static_cast<unsigned long long>(o.clordid));
            le.len = static_cast<u8>(m);
            (void)hs.ring3.try_push(le);
        }

        const ssize_t r = ::recv(fd, rbuf, sizeof(rbuf), 0);
        if (r > 0) {
            did_work = true;
            framer.append(rbuf, static_cast<std::size_t>(r));
            while (auto msg = framer.next()) {
                const fix::View v(*msg);
                if (v.get(35) == "8") {                        // ExecutionReport
                    const u64 id = static_cast<u64>(v.get_int(11));
                    const u64 t0 = hs.pending[id & kPendingMask];
                    const u64 dt = clk.ticks_to_ns(rdtsc_fast() - t0);  // tick -> ER
                    if (hs.fills.load(std::memory_order_relaxed) >= warmup)
                        hs.t2t.record(dt);
                    hs.fills.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else if (r == 0) {
            std::fprintf(stderr, "[gateway] exchange closed connection\n");
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }

        if (!did_work) cpu_relax();
    }
}

// ---- logger: drain ring3 off the hot path, write to file -------------------
void run_logger(HotState& hs, const Cfg& c) {
    std::FILE* f = std::fopen(c.log_path, "w");
    LogEvent le;
    while (g_running.load(std::memory_order_relaxed) || hs.ring3.size_approx() > 0) {
        if (hs.ring3.try_pop(le)) {
            if (f) std::fprintf(f, "%llu %u %.*s\n",
                                static_cast<unsigned long long>(le.tsc),
                                static_cast<unsigned>(le.type), le.len, le.msg);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    if (f) std::fclose(f);
}

// ---- metrics: Prometheus text on :8000/metrics (netdata scrapes this) ------
void append_gauge(std::string& s, const char* name, u64 v) {
    char line[128];
    const int n = std::snprintf(line, sizeof(line),
                                "# TYPE %s gauge\n%s %llu\n",
                                name, name, static_cast<unsigned long long>(v));
    s.append(line, static_cast<std::size_t>(n));
}

std::string build_metrics(const HotState& hs, const TscClock& clk, bool on_huge) {
    const Histogram::Snapshot p = hs.pipeline.snapshot();
    const Histogram::Snapshot q = hs.t2t.snapshot();
    std::string s;
    s.reserve(1024);
    char line[128];
    int n = std::snprintf(line, sizeof(line),
                          "# TYPE engine_orders_total counter\nengine_orders_total %llu\n"
                          "# TYPE engine_fills_total counter\nengine_fills_total %llu\n",
                          static_cast<unsigned long long>(hs.orders.load(std::memory_order_relaxed)),
                          static_cast<unsigned long long>(hs.fills.load(std::memory_order_relaxed)));
    s.append(line, static_cast<std::size_t>(n));
    append_gauge(s, "engine_pipeline_p50_ns", p.p50);
    append_gauge(s, "engine_pipeline_p99_ns", p.p99);
    append_gauge(s, "engine_pipeline_p999_ns", p.p999);
    append_gauge(s, "engine_pipeline_max_ns", p.max);
    append_gauge(s, "engine_pipeline_mean_ns", p.mean);
    append_gauge(s, "engine_tick_to_trade_p50_ns", q.p50);
    append_gauge(s, "engine_tick_to_trade_p99_ns", q.p99);
    append_gauge(s, "engine_tick_to_trade_p999_ns", q.p999);
    append_gauge(s, "engine_tick_to_trade_max_ns", q.max);
    append_gauge(s, "engine_tick_to_trade_mean_ns", q.mean);
    n = std::snprintf(line, sizeof(line),
                      "# TYPE engine_tsc_ghz gauge\nengine_tsc_ghz %.3f\n"
                      "# TYPE engine_hugepages gauge\nengine_hugepages %d\n",
                      clk.ghz(), on_huge ? 1 : 0);
    s.append(line, static_cast<std::size_t>(n));
    return s;
}

void run_metrics(const HotState& hs, const TscClock& clk, bool on_huge) {
    const int lfd = net::listen_tcp("127.0.0.1", kMetricsPort);
    if (lfd < 0) {
        std::fprintf(stderr, "[metrics] listen on :%u failed\n", kMetricsPort);
        return;
    }
    timeval tv{0, 200000};  // 200ms accept timeout so we notice shutdown
    ::setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (g_running.load(std::memory_order_relaxed)) {
        const int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) continue;  // timeout or interrupt -> re-check running
        char req[1024];
        (void)::recv(fd, req, sizeof(req), 0);  // ignore the request line
        const std::string body = build_metrics(hs, clk, on_huge);
        std::string resp = "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
                           "Connection: close\r\nContent-Length: ";
        resp += std::to_string(body.size());
        resp += "\r\n\r\n";
        resp += body;
        (void)net::send_all(fd, resp.data(), resp.size());
        ::close(fd);
    }
    ::close(lfd);
}

// Perform FIX logon on the main thread (blocking) before the gateway loop.
bool do_logon(int fd) {
    fix::Builder b;
    char ts[24];
    const u32 tslen = net::utc_timestamp(ts);
    b.reset();
    b.add(35, "A");
    b.add(49, "TRADER");
    b.add(56, "EXCHANGE");
    b.add(34, 1);
    b.add(52, std::string_view(ts, tslen));
    b.add(98, 0);
    b.add(108, 30);
    char out[256];
    const u32 n = b.finalize(out);
    if (!net::send_all(fd, out, n)) return false;

    fix::Framer framer;
    char rbuf[4096];
    for (int tries = 0; tries < 50 && g_running.load(std::memory_order_relaxed); ++tries) {
        const ssize_t r = ::recv(fd, rbuf, sizeof(rbuf), 0);
        if (r <= 0) return false;
        framer.append(rbuf, static_cast<std::size_t>(r));
        while (auto msg = framer.next()) {
            const fix::View v(*msg);
            if (v.get(35) == "A") return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const Cfg cfg = parse(argc, argv);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Kick off TSC calibration on a worker while we do the rest of startup
    // (email: std::async / std::future). The two ~100ms windows overlap setup.
    std::future<TscClock> clk_fut =
        std::async(std::launch::async, [] { return TscClock::calibrate(100); });

    if (!lock_memory())
        std::fprintf(stderr, "[engine] mlockall denied (need CAP_IPC_LOCK / LimitMEMLOCK)\n");

    // Place all hot state on huge pages if requested/available; else the heap.
    std::unique_ptr<HugeArena> arena;
    HotState* hs = nullptr;
    bool on_huge = false, placed = false;
    if (cfg.huge) {
        arena = std::make_unique<HugeArena>(sizeof(HotState) + (1u << 20), true);
        if (arena->ok()) {
            if (void* m = arena->alloc(sizeof(HotState), 64)) {
                hs = new (m) HotState();
                on_huge = arena->using_hugepages();
                placed = true;
            }
        }
    }
    if (hs == nullptr) hs = new HotState();

    const TscClock clk = clk_fut.get();

    if (!tsc_is_invariant())
        std::fprintf(stderr, "[engine] WARNING: TSC not invariant — timings unreliable\n");
    std::fprintf(stderr,
                 "[engine] dispatch=%s alloc=%s huge=%s(on_pages=%d) pin=%d rt=%d "
                 "rate=%u tsc=%.3fGHz\n",
                 cfg.dispatch_crtp ? "crtp" : "virtual",
                 cfg.use_pool ? "pool" : "malloc", cfg.huge ? "on" : "off",
                 on_huge, cfg.pin, cfg.rt, cfg.rate, clk.ghz());

    const int fd = net::connect_tcp(cfg.exch_ip, kExchangePort);
    if (fd < 0) {
        std::fprintf(stderr, "[engine] cannot connect to exchange %s:%u\n",
                     cfg.exch_ip, kExchangePort);
        return 1;
    }
    if (!do_logon(fd)) {
        std::fprintf(stderr, "[engine] logon failed\n");
        ::close(fd);
        return 1;
    }
    std::fprintf(stderr, "[engine] logon acknowledged; starting pipeline\n");
    net::set_nonblocking(fd);

    // Strategy objects live for the whole run; the loop is templated so CRTP is
    // a direct inlined call and virtual truly goes through the vtable.
    AltStrategy crtp;
    AltStrategyV virt;

    std::thread md_t([&] { run_market_data(*hs, cfg, clk); });
    std::thread st_t([&] {
        if (cfg.dispatch_crtp) run_strategy<AltStrategy>(&crtp, *hs, cfg);
        else run_strategy<IStrategy>(&virt, *hs, cfg);
    });
    std::thread gw_t([&] { run_gateway(*hs, cfg, clk, fd); });
    std::thread lg_t([&] { run_logger(*hs, cfg); });
    std::thread mx_t([&] { run_metrics(*hs, clk, on_huge); });

    md_t.join();
    st_t.join();
    gw_t.join();
    lg_t.join();
    mx_t.join();

    ::close(fd);
    const u64 orders = hs->orders.load();
    const u64 fills = hs->fills.load();
    std::fprintf(stderr, "[engine] shutdown: orders=%llu fills=%llu\n",
                 static_cast<unsigned long long>(orders),
                 static_cast<unsigned long long>(fills));
    if (placed) hs->~HotState();
    else delete hs;
    return 0;
}
