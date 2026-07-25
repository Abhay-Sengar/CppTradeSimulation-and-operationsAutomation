// bench_gbench.cpp — the same A/B as microbench.cpp, expressed with Google
// Benchmark (email lists "GTest + Google Benchmark"). Optional target
// (BUILD_GBENCH=ON, needs libbenchmark-dev). The standalone microbench.cpp is
// the one used in the demo; this shows the idiomatic framework form.
#include "messages.hpp"
#include "pool.hpp"
#include "strategy.hpp"
#include "types.hpp"

#include <benchmark/benchmark.h>
#include <new>

using namespace ts;

static void BM_PoolAcquireRelease(benchmark::State& st) {
    ObjectPool<OrderCtx, 1024> pool;
    for (auto _ : st) {
        OrderCtx* c = pool.acquire();
        benchmark::DoNotOptimize(c);
        if (c) pool.release(c);
    }
}
BENCHMARK(BM_PoolAcquireRelease);

static void BM_MallocNewDelete(benchmark::State& st) {
    for (auto _ : st) {
        OrderCtx* c = new OrderCtx;
        benchmark::DoNotOptimize(c);
        delete c;
    }
}
BENCHMARK(BM_MallocNewDelete);

static void BM_DispatchCRTP(benchmark::State& st) {
    AltStrategy s;
    const Tick t{0, 0, 0, 10000, 10010};
    u64 i = 0;
    for (auto _ : st) {
        Order o = s.on_tick(t, i++);
        benchmark::DoNotOptimize(o);
    }
}
BENCHMARK(BM_DispatchCRTP);

static void BM_DispatchVirtual(benchmark::State& st) {
    AltStrategyV v;
    IStrategy* p = &v;
    const Tick t{0, 0, 0, 10000, 10010};
    u64 i = 0;
    for (auto _ : st) {
        Order o = p->on_tick(t, i++);
        benchmark::DoNotOptimize(o);
    }
}
BENCHMARK(BM_DispatchVirtual);

BENCHMARK_MAIN();
