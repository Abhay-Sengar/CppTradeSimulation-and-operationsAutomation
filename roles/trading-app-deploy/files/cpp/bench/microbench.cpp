// microbench.cpp — standalone rdtsc micro-benchmarks for the three A/B pairs.
//
// No external dependency (not even Google Benchmark) so it builds with plain
// g++ and reproduces the headline numbers at nanosecond resolution:
//   * CRTP vs virtual dispatch
//   * object pool vs malloc/new
//   * flat (open-addressing) map vs std::unordered_map
//
// These isolate the per-operation cost cleanly — the live engine histogram is
// better for the *tail* (malloc jitter, isolation, hugepages), this is better
// for the *mean* of a single cheap operation the histogram can't separate from
// system noise. Run it on a quiet core — `scripts/ab_sweep.sh` stops the engine
// first and runs it on a P-core, a housekeeping E-core and an isolated E-core,
// because on a hybrid CPU the per-op ratios differ per microarchitecture.
//
// Interview topics (email §"Profiling & Measurement"): benchmark hygiene —
// warm-up iterations, take the MIN across trials (least contaminated by
// interrupts), do_not_optimize barriers so the optimiser can't delete the work.
#include "flatmap.hpp"
#include "messages.hpp"
#include "pool.hpp"
#include "strategy.hpp"
#include "tsc.hpp"
#include "types.hpp"

#include <cstdio>
#include <new>
#include <unordered_map>

using namespace ts;

namespace {

// Prevent the optimiser from eliding work whose result is otherwise unused.
volatile u64 g_sink = 0;
template <class T>
inline void escape(T* p) noexcept { asm volatile("" : : "g"(p) : "memory"); }

constexpr std::size_t kIters = 2'000'000;
constexpr int kTrials = 21;

// Run `body(i)` kIters times per trial, kTrials trials; return best (min) ns/op.
//
// `body` RETURNS the value to keep alive, and we accumulate it into a plain local
// register. Do NOT write to a `volatile` inside the timed loop: a volatile
// read-modify-write (`g_sink = g_sink + x`) compiles to a real load+store every
// iteration and creates a loop-carried dependency through store-to-load
// forwarding. That chain becomes the FLOOR of the measurement, and on this hybrid
// CPU the floor differs per core type — ~6 cycles on the P-cores (Golden Cove)
// vs ~2 cycles on the E-cores (Gracemont). For the cheapest experiment (dispatch,
// well under a nanosecond) the floor exceeded the signal and the P-core reported
// crtp == virtual == 1.60 ns, a bogus 1.00x. Accumulating in a register costs one
// cycle of add latency and touches the volatile sink once per bench() call,
// outside the timed region. See ProjectDeepDive.md §8.
template <class Body>
double bench(const TscClock& clk, Body body) {
    u64 acc = 0;
    // warm-up
    for (u64 i = 0; i < 50'000; ++i) acc += body(i);
    u64 best = ~u64{0};
    for (int t = 0; t < kTrials; ++t) {
        const u64 s = rdtsc_serialized();
        for (u64 i = 0; i < kIters; ++i) acc += body(i);
        const u64 e = rdtsc_serialized();
        const u64 d = e - s;
        if (d < best) best = d;
    }
    g_sink = g_sink + acc;  // one volatile store, after all timing
    return static_cast<double>(clk.ticks_to_ns(best)) / static_cast<double>(kIters);
}

}  // namespace

int main() {
    const TscClock clk = TscClock::calibrate(100);
    std::printf("microbench @ %.3f GHz (min ns/op over %d trials x %zu iters)\n\n",
                clk.ghz(), kTrials, kIters);

    const Tick base{0, 0, 0, 10000, 10010};

    // ---- dispatch: CRTP (inlined) vs virtual (vtable) ----------------------
    AltStrategy crtp;
    AltStrategyV virt_obj;
    // volatile pointer barrier: stops the compiler from proving the dynamic type
    // and speculatively de-virtualising the call (which would hide the cost).
    IStrategy* volatile vp = &virt_obj;

    const double ns_crtp = bench(clk, [&](u64 i) {
        Tick t = base;
        t.bid += static_cast<i64>(i & 63);
        const Order o = crtp.on_tick(t, i);
        return static_cast<u64>(o.px);
    });
    const double ns_virt = bench(clk, [&](u64 i) {
        Tick t = base;
        t.bid += static_cast<i64>(i & 63);
        IStrategy* p = vp;
        const Order o = p->on_tick(t, i);
        return static_cast<u64>(o.px);
    });

    // ---- allocation: object pool vs new/delete -----------------------------
    ObjectPool<OrderCtx, 1024> pool;
    const double ns_pool = bench(clk, [&](u64 i) {
        OrderCtx* c = pool.acquire();
        if (c) { c->clordid = i; escape(c); pool.release(c); }
        return reinterpret_cast<u64>(c);
    });
    const double ns_malloc = bench(clk, [&](u64 i) {
        OrderCtx* c = static_cast<OrderCtx*>(::operator new(sizeof(OrderCtx), std::nothrow));
        if (c) { c->clordid = i; escape(c); ::operator delete(c); }
        return reinterpret_cast<u64>(c);
    });

    // ---- map: flat open-addressing vs std::unordered_map -------------------
    FlatMap<u64, 4096> flat;
    for (u64 i = 0; i < 2000; ++i) flat.put(i, i * 3);
    const double ns_flat = bench(clk, [&](u64 i) {
        u64* v = flat.get(i & 1999);
        return v ? *v : 0;
    });
    std::unordered_map<u64, u64> umap;
    for (u64 i = 0; i < 2000; ++i) umap.emplace(i, i * 3);
    const double ns_umap = bench(clk, [&](u64 i) {
        auto it = umap.find(i & 1999);
        return it != umap.end() ? it->second : u64{0};
    });

    std::printf("%-28s %8s %8s   %s\n", "experiment", "A(ns)", "B(ns)", "ratio B/A");
    std::printf("%-28s %8.2f %8.2f   %.2fx\n", "dispatch  crtp vs virtual",
                ns_crtp, ns_virt, ns_virt / ns_crtp);
    std::printf("%-28s %8.2f %8.2f   %.2fx\n", "alloc     pool vs malloc",
                ns_pool, ns_malloc, ns_malloc / ns_pool);
    std::printf("%-28s %8.2f %8.2f   %.2fx\n", "map       flat vs unordered",
                ns_flat, ns_umap, ns_umap / ns_flat);
    std::printf("\n(sink=%llu — ignore)\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
