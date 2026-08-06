# Interview Notes — topic → where it lives → talking point

Study companion for the Open Futures C++ interview. Every bullet from the
recruiter's topic list maps to one of three states:

- ✅ **code-backed** — implemented in this repo; open the file and point at it.
- 🟡 **partial** — related code exists; know the gap and why.
- 📖 **pure study** — not in this codebase (usually DSA or a concept the setup
  can't show); the honest answer is "I know it, here's the summary."

The live pipeline is `market-data (cpu8) → strategy (cpu9) → gateway (cpu10) →
exchange (cpu11)`, three SPSC rings, FIX/TCP loopback, all under
`roles/trading-app-deploy/files/cpp/`.

Measured on this box (Intel Core i7-1255U, isolated E-cores, `performance`
governor, RT throttling disabled — microbench min ns/op):

| A/B | A | B | ratio (P-core) | ratio (E-core) |
|---|---|---|---|---|
| dispatch: CRTP vs virtual | 0.25 ns | 0.58 ns | 2.32× | 1.76× |
| alloc: pool vs malloc | 1.95 ns | 10.41 ns | 5.33× | 5.33× |
| map: flat vs `unordered_map` | 0.75 ns | 2.24 ns | 2.99× | 1.23× |

(A/B columns are the P-core figures. The ratios differ by microarchitecture —
worth saying out loud: these are properties of a core design, not universal
constants. Previous hardware, Ryzen 7 7730U: 2.06× / 4.36× / 1.64×.)

Live latency on the isolated cores (from `:8000/metrics`). **`t2t` is the
engine-only number we optimise** (tick generated → order ready to send, fully
in-process); **`rtt` includes the mock exchange + kernel TCP** and is NOT
engine-only:

| histogram | p50 | p99 | p999 | max |
|---|---|---|---|---|
| `t2t` — engine tick-to-trade (md→gateway) | 474 ns | 700 ns | 2.67 µs | 20.4 µs |
| `rtt` — round trip incl. exchange (md→fill) | 10.9 µs | 13.5 µs | 16.8 µs | 52 µs |

(At 50k ticks/s. Previous hardware, Ryzen 7 7730U: `t2t` 588 ns / 1.58 µs /
2.0 µs / 2.5 µs — better p50 and p99 here, worse far tail; see
`ProjectDeepDive.md` §3a.)

The ~0.5µs `t2t` vs ~11µs `rtt` gap is the kernel/TCP stack cost — the live
motivation for kernel bypass. Getting the `t2t` tail down required: isolcpus +
nohz_full + SCHED_FIFO **with RT throttling disabled**
(`kernel.sched_rt_runtime_us=-1`) so the pinned FIFO thread is never stalled,
and keeping the non-hot threads (logger/metrics) on housekeeping cores. Before
that tuning the p99 was ~880 ms (RT throttle stalling the gateway) — a concrete
lesson in why each knob matters.

**Know the attribution, not just the total** (`scripts/ab_sweep.sh`, one flag
flipped per run at 50k ticks/s — full table in `ProjectDeepDive.md` §7):
pinning off = **10× p99 / 402× p999**; `malloc` = +27% p99, +75% max but **+0.6%
mean**; hugepages off = +29% p99; FIFO off = +18% p99; CRTP→virtual =
**unmeasurable** (+1%, inside the noise). The lesson to lead with: the per-op
knobs buy *predictability*, not throughput — and a 0.5 ns vtable cost is 0.1% of
a 470 ns path, so the microbench can see it and the engine histogram cannot.

---

## HFT-Specific — Core C++ Language (C++20)

| Topic | State | Where / talking point |
|---|---|---|
| Move semantics, RVO/NRVO, copy elision | 🟡 | `Order`/`Tick` are trivially-copyable PODs moved by value through rings; RVO applies to `decide()` returning `Order` (`strategy.hpp`). Know: RVO is mandatory in C++17 for prvalues; NRVO (named) is allowed but not guaranteed. |
| CRTP, `if constexpr`, variadic templates | ✅ | `strategy.hpp` (CRTP `StrategyCRTP<Derived>`), `pool.hpp`/`bump.hpp` (variadic `acquire(Args&&...)` + perfect forwarding). |
| `consteval` / `constinit` | ✅ | `config.hpp`: `consteval ring_capacity()`, `constinit kLimits`; `types.hpp`: `consteval price_scale()`. |
| Memory model: `std::atomic`, `memory_order` — explain *why* each | ✅ | `spsc.hpp`: acquire/release (publish data with the index). `hist.hpp`: relaxed (independent counters, no cross-bucket invariant). `engine_main.cpp`: relaxed stop flag. **Why:** acquire/release gives the one happens-before edge the ring needs; seq_cst would add a full barrier for nothing; relaxed is fine when there's no data dependency to order. |
| POD / trivially-copyable discipline | ✅ | `messages.hpp` + `fix.hpp`: `static_assert(is_trivially_copyable_v<>)`, `#pragma pack(1)` on `BinOrder`, `sizeof` asserts. |
| `[[nodiscard]]`, `noexcept`, `explicit`, `override`, `final` | ✅ | `types.hpp` (explicit ctor, nodiscard), `strategy.hpp` (override/final), hot-path fns are `noexcept`. **Why noexcept:** lets the compiler omit unwinding tables and enables move in containers. |
| Rule of 0/3/5, explicit copy/move delete | ✅ | `spsc.hpp` and `pool.hpp` delete copy/move (own atomics / raw storage). Most PODs follow Rule of 0. |
| `enum class` vs unscoped | ✅ | `types.hpp` `Side : u8`, `messages.hpp` `LogType : u8` — scoped, fixed underlying type, no implicit int. |
| `string_view`, `span`, `from_chars` | ✅ | `fix.hpp`: parse with `string_view` + `from_chars` (no alloc, no locale); `checksum(std::span<const char>)`. |

## Lock-Free Data Structures

| Topic | State | Where / talking point |
|---|---|---|
| SPSC ring from scratch (pow2, bitmask, alignas(64) head/tail, acquire/release) | ✅ | `spsc.hpp` — the centrepiece. |
| SPSC vs MPSC vs MPMC — why MPMC is last resort | 📖/✅ | SPSC needs **no CAS** (one writer per index) — plain load/store. MPSC/MPMC need CAS loops → contention + ABA. Our pipeline is a chain of SPSC hops by construction. |
| False sharing, fix with `alignas(64)` | ✅ | `spsc.hpp`: head and tail on separate 64-byte lines so producer/consumer don't invalidate each other's cache line. |
| `_mm_pause()` in spin loops | ✅ | `util.hpp` `cpu_relax()`. **Why:** de-pipelines the spin (power), yields SMT slot, and dodges the memory-order-violation pipeline flush on wake. |

## Memory & Allocation

| Topic | State | Where / talking point |
|---|---|---|
| Object pool with free-list | ✅ | `pool.hpp` — intrusive free-list threaded through the free slots (union), placement new + explicit dtor. |
| Arena / bump allocator | ✅ | `bump.hpp` — pointer-bump + alignment, `reset()` frees everything at once. |
| Why malloc/new banned on hot path (50ns–50µs jitter) | ✅ | microbench shows pool 4.4 ns vs malloc 17.7 ns *mean*; the real killer is the **tail** (arena refill, lock, page fault). Hot path pre-allocates. |
| `mmap`+`MAP_POPULATE`, `madvise(WILLNEED)`, `mlockall` | ✅ | `huge.hpp`. Pre-fault at startup so no minor faults on the hot path; lock pages so nothing swaps. |
| Huge pages: why THP worse than explicit | ✅ | `huge.hpp` uses `MAP_HUGETLB`; role sets `transparent_hugepage=never`. **Why:** THP is assembled/split lazily by khugepaged at unpredictable times → jitter; explicit 2M pages are reserved up front. |

## CPU & Cache Architecture

| Topic | State | Talking point |
|---|---|---|
| L1/L2/L3/DRAM latency (~1.5/5/15/80 ns) | 📖 | Know the ladder. Hot working set (ring slots + pool slot + pending entry) is a few cache lines → stays in L1. |
| Cache line = 64B; working set fits L1 | ✅ | `alignas(64)` throughout; PODs kept small. |
| Store-forwarding stalls | 📖 | A load that overlaps a recent store to the same address but different size/alignment can't forward from the store buffer → stall. Avoid by not aliasing narrow/wide stores. |
| Branch misprediction (~15–20 cyc), `[[likely]]`/`__builtin_expect` | ✅ | `pool.hpp` marks the drained/null path `[[unlikely]]`. |
| NUMA: NIC+core+memory same node | 📖 | Single-socket laptop → `numactl --hardware` shows 1 node, so N/A here. On a 2-socket colo box: pin the NIC IRQ, the hot thread, and its memory to the same node or pay the cross-node penalty. |

## Performance-Sensitive Design Patterns

| Topic | State | Where / talking point |
|---|---|---|
| Why virtual dispatch banned on hot path | ✅ | `strategy.hpp`; microbench 2.0×. vtable load + indirect branch can't inline, may mispredict, pollutes i-cache. |
| CRTP as virtual replacement | ✅ | `strategy.hpp` `StrategyCRTP<Derived>` — resolved at compile time, fully inlined. |
| Why `std::function` / `shared_ptr` / `unordered_map` banned | ✅/📖 | `std::function` = heap + indirect call; `shared_ptr` = atomic refcount churn; `unordered_map` = node-per-element alloc + pointer chase (see `flatmap.hpp`). |
| Flat (open-addressing) vs node-based map | ✅ | `flatmap.hpp` (linear probing, one contiguous array); microbench 1.7× vs `unordered_map`. Live pending-order store is an even simpler direct-mapped array (`engine_main.cpp`) since clordids are monotonic. |
| `int64_t` for money, never `double` | ✅ | `types.hpp` `Price`/`Qty`. **Why:** 0.10 has no exact binary representation → rounding error accumulates; scaled integers are exact. |

## Profiling & Measurement

| Topic | State | Where / talking point |
|---|---|---|
| `rdtsc` + calibration vs `CLOCK_MONOTONIC_RAW` | ✅ | `tsc.hpp`. TSC counts cycles; calibrate ns/tick against MONOTONIC_RAW (no NTP slew). Invariant TSC → cross-core subtraction valid. |
| `perf stat`, `perf record`, flamegraphs | 📖 | `perf stat ./trading_engine` for IPC/cache-misses/branch-misses; `perf record`+`perf script`+FlameGraph for a flamegraph. Works on bare metal with real isolated cores. |
| HDR histogram vs averages; p99/p99.9 | ✅ | `hist.hpp` — sub-buckets per octave (`__builtin_clzll`), fixed relative error. Mean hides the tail; in trading the tail is the risk. |
| `strace -c` to prove zero syscalls on hot path | ✅/🟡 | md/strategy stages are syscall-free; the **gateway's TCP send/recv are the only syscalls** — which is exactly the motivation for kernel bypass. Be precise about this. |
| Benchmark hygiene | ✅ | `microbench.cpp`: warm-up, min-over-trials, `do_not_optimize` barriers, run on isolated core, production flags. |

## Compiler & Build

| Topic | State | Where / talking point |
|---|---|---|
| `-O3 -Ofast -march=native -flto -fno-exceptions -fno-rtti` | ✅ | `CMakeLists.txt`. `-fno-rtti` on (we use CRTP not dynamic_cast). `-march=native` = alderlake (hybrid: resolves to the ISA the P- and E-cores share, so no AVX-512). `-flto` optional. **Exceptions kept ON** — hot path is `noexcept`/alloc-free by discipline; `-fno-exceptions` program-wide fights `<future>`/`<thread>` for no hot-path gain. |
| Why `-Ofast` safe when money is int64 | ✅ | `-Ofast` enables `-ffast-math`, which only changes **floating-point** semantics. All money math is `int64_t`, so there's no FP on the hot path to break. |
| PGO two-phase | 📖 | `-fprofile-generate` → run a representative load → `-fprofile-use`. Feeds real branch/layout data back to the optimiser. Left out to keep the build one-step. |
| ASan+UBSan vs TSan — why not combined | ✅ | `CMakeLists.txt` presets. Both hook shadow memory / the allocator and can't coexist. ASan+UBSan: use-after-free/OOB/UB. TSan: data races. Ship neither in `-O3`. |

## Linux Systems

| Topic | State | Where / talking point |
|---|---|---|
| `isolcpus`, `nohz_full`, `rcu_nocbs` | ✅ | `roles/kernel-tuning/defaults/main.yml` cmdline. isolcpus keeps the scheduler off 8-11; nohz_full stops the tick when 1 runnable task; rcu_nocbs offloads RCU callbacks to housekeeping. |
| `SCHED_FIFO` + `pthread_setaffinity_np` in code | ✅ | `affinity.hpp` (`pin_to_cpu`, `set_realtime`, `AffinityGuard` RAII). Pin FIRST, verify, THEN go FIFO (safety). |
| governor performance, disable turbo, stop irqbalance | ✅ | `trade-ops-cpu-tuning.sh` (governor + optional boost off), role stops irqbalance, `irqaffinity=0-7` on cmdline. |
| `/proc/cpuinfo` `constant_tsc` / `nonstop_tsc` | ✅ | `tsc.hpp` `tsc_is_invariant()`; `bod_check.sh` `check_tsc`. |
| SMT sibling offlining | ✅ | `trade-ops-cpu-tuning.sh` offlines 1/3 — the HT siblings of the housekeeping P-cores. The hot cores are E-cores with no sibling, so the step moves to where SMT contention actually exists; see the hybrid-topology note in `ProjectDeepDive.md`. |

## Networking

| Topic | State | Talking point |
|---|---|---|
| `TCP_NODELAY`, `SO_BUSY_POLL`, socket buffers | ✅ | `net.hpp`. NODELAY disables Nagle (don't buffer a small order); SO_BUSY_POLL busy-polls the NIC queue; buffers tuned via sysctl in kernel-tuning. |
| UDP multicast, IGMP join | 📖 | Exchanges fan out market data via UDP multicast; receivers `IP_ADD_MEMBERSHIP` (IGMP join). Our transport is TCP-loopback FIX, so this is concept-only. |
| Kernel bypass: ExaNIC / Solarflare ef_vi | 📖 | Map NIC RX/TX rings into user space, poll in userland → skip the kernel network stack and its syscall/copy/IRQ overhead. Our `rtt` vs `t2t` gap (~43µs vs ~0.6µs) is precisely the kernel cost this removes. |

## Build & Tooling

| Topic | State | Where |
|---|---|---|
| CMake C++20, `CXX_EXTENSIONS OFF`, ccache, LTO | ✅ | `CMakeLists.txt`. |
| `-Wall -Wextra -Wconversion -Wshadow -Werror` | ✅ | `CMakeLists.txt` — the whole tree builds clean under these. |
| GTest + Google Benchmark | ✅ | `tests/test_spsc.cpp`, `tests/test_hist.cpp` (GTest, `-DBUILD_TESTS=ON` + libgtest-dev); `bench/bench_gbench.cpp` (`-DBUILD_GBENCH=ON` + libbenchmark-dev). The dependency-free `microbench` is the demo one. |

---

## General C++ (topics 1–5, 7, 8) — quick map

| Topic | State | Note |
|---|---|---|
| OOP: classes/encapsulation/polymorphism | ✅ | `strategy.hpp` (both static & dynamic polymorphism). |
| Virtual functions, vtable internals, pure virtual, abstract | ✅ | `IStrategy` (pure virtual, abstract). vtable = per-class table of fn pointers; each object holds a vptr. |
| Ctor/dtor order in inheritance | 📖 | Base ctor first, members in declaration order, then derived; dtor reverse. |
| Multiple inheritance, diamond, virtual inheritance | 📖 | **Pure study** — no natural fit here. Diamond = duplicated base; `virtual` inheritance shares one base subobject. |
| `explicit`, `friend`, `mutable`, `static` | 🟡 | `explicit` in `types.hpp`. friend/mutable are study. |
| Operator overloading | ✅ | `types.hpp` (`+`, `-`, `<=>`, `==` on strong types). |
| Pointers vs refs, `const` correctness | ✅ | Throughout (const refs, const methods). |
| `static`/`extern`/`volatile`/`inline` | 🟡 | `inline` everywhere (header fns); `volatile` sink in microbench (and know: volatile ≠ atomic, it's for MMIO/signals). |
| Stack vs heap, memory layout (code/data/BSS/stack/heap) | 📖 | Know the segments; hot state is one mmap'd arena (`huge.hpp`). |
| `auto`/`decltype`, lambdas, structured bindings, range-for | ✅ | lambdas for thread bodies (`engine_main.cpp`), range-for/structured bindings in loops. |
| UB: signed overflow, null deref, strict aliasing | ✅ | `fix.hpp` explains strict-aliasing (memcpy not reinterpret_cast). Signed overflow is UB → money uses defined int64 ops. |
| Casts: static/dynamic/reinterpret/const | 🟡 | static_cast throughout; **no dynamic_cast** (we build `-fno-rtti` — that *is* the answer). |
| Smart pointers: unique/shared/weak — ownership | 🟡 | `unique_ptr<HugeArena>` in `engine_main.cpp`. shared/weak: **study** — shared = refcounted shared ownership (atomic count), weak = non-owning break-cycle handle. Banned on hot path. |
| RAII | ✅ | `AffinityGuard` (`affinity.hpp`), `HugeArena` dtor unmaps, file/socket closes. |
| Leaks / dangling / double-free | 📖 | Caught by ASan build. Pool avoids per-op alloc entirely. |
| Placement new | ✅ | `pool.hpp`, and placing `HotState` in the arena (`engine_main.cpp`). |
| STL containers / iterators / algorithms / complexity | 📖 | **Study** — the hot path deliberately avoids node-based STL. Know: `vector` contiguous O(1) amortized push; `map` = RB-tree O(log n); `unordered_map` = hash chaining O(1) avg; iterator categories. |
| `std::move`/`forward`/`exchange` | 🟡 | Perfect forwarding in pool/bump `acquire`/`make`. |
| Concurrency: `thread`/`jthread`/`mutex`/`lock_guard` | ✅ | `engine_main.cpp` threads; metrics thread uses a plain mutex deliberately (non-hot-path contrast). |
| `atomic` load/store/CAS, ordering | ✅ | `spsc.hpp`, `hist.hpp` (CAS loop for max). |
| `condition_variable` wait/notify | 🟡 | **Design choice:** hot threads can't block on a CV, so shutdown uses an atomic flag polled each loop + join. Know the wait/notify + predicate-loop pattern for the non-hot case. |
| Race/deadlock/livelock/starvation | 📖 | Lock-free hot path sidesteps deadlock; know the definitions + lock-ordering fix. |
| `thread_local` | 🟡 | Per-thread stack buffers in the loops; know TLS semantics. |
| `future`/`promise`/`async` | 🟡 | `std::async`+`std::future` for TSC calibration (`engine_main.cpp`). promise: study. |
| System design: ring producer-consumer, Meyer's singleton, memory pool, LRU, async logger | ✅/📖 | Ring ✅ (`spsc.hpp`); memory pool ✅ (`pool.hpp`); async off-thread logger ✅ (ring3 + logger thread). Meyer's singleton (`static` local, thread-safe since C++11) + LRU (list + hashmap) = **study**. |
| Debugging: gdb, valgrind/ASan, perf/gprof, CMake, compiler vs linker errors | ✅/📖 | ASan preset ✅; perf ✅; gdb/valgrind/gprof = study. Compiler error = syntax/type in one TU; linker error = missing/duplicate symbol across TUs. |

## DSA (topic 6) — 📖 pure study, not in this codebase

Two pointers, sliding window, prefix sum; sort complexity/stability (quick/merge/heap);
linked-list reversal + Floyd's cycle; BST/AVL/RB concept, tree traversals, heap ops;
graph BFS/DFS/Dijkstra/topo-sort, Union-Find; hashing (chaining vs open addressing —
see `flatmap.hpp` for the open-addressing side); DP memo vs tabulation (LIS, knapsack,
coin change, edit distance); Big-O for every container/algorithm. **This is the biggest
study block and unrelated to the setup — budget Wed–Fri for it.**
