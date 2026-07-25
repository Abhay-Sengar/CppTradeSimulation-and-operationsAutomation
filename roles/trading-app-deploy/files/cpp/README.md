# cpp/ — C++20 trading engine + mock exchange

A 3-hop, lock-free trading pipeline built to surface the "HFT Specific" section
of the interview topic list. See `../../../../INTERVIEW_NOTES.md` for the full
topic→file map.

```
market-data (cpu8) --ring1--> strategy (cpu10) --ring2--> gateway (cpu12)
                                                              |  FIX/TCP
     rdtsc t0 rides the whole pipeline                        v  loopback:9001
     +----------------- tick_to_trade ------------------> exchange (cpu14)

gateway --ring3--> logger (housekeeping)     metrics-http :8000 (housekeeping)
```

## Layout

| Path | What |
|---|---|
| `include/spsc.hpp` | SPSC lock-free ring (alignas(64), acquire/release, `_mm_pause`) |
| `include/tsc.hpp` | rdtsc + CLOCK_MONOTONIC_RAW calibration, invariant-TSC check |
| `include/hist.hpp` | HDR histogram (sub-buckets/octave), lock-free relaxed record |
| `include/pool.hpp` `bump.hpp` `huge.hpp` | object pool, arena, huge-page/mlock arena |
| `include/strategy.hpp` | CRTP vs virtual dispatch (runtime-selectable) |
| `include/fix.hpp` | FIX 4.2 encode/parse + packed-binary discipline demo |
| `include/flatmap.hpp` | open-addressing map vs `std::unordered_map` |
| `include/affinity.hpp` `net.hpp` | pin+SCHED_FIFO RAII; TCP_NODELAY/SO_BUSY_POLL |
| `src/engine_main.cpp` `src/exchange_main.cpp` | the two binaries |
| `bench/microbench.cpp` | dependency-free rdtsc A/B (dispatch/alloc/map) |
| `tests/*.cpp` | GTest unit tests (optional) |

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# optional: -DBUILD_TESTS=ON (libgtest-dev), -DBUILD_GBENCH=ON (libbenchmark-dev),
#           -DENABLE_LTO=ON
# sanitisers (mutually exclusive):
cmake -S . -B build-asan -DSANITIZE=address && cmake --build build-asan -j
cmake -S . -B build-tsan -DSANITIZE=thread  && cmake --build build-tsan -j
```

Plain g++ (no cmake) also works:
```bash
g++ -std=c++20 -O3 -march=native -fno-rtti -Wall -Wextra -Wconversion -Wshadow \
    -Werror -Iinclude src/engine_main.cpp -o trading_engine -lpthread
```

## Run locally (no isolation needed — pins to whatever cores exist)

```bash
./build/mock_exchange --pin=off &
./build/trading_engine --huge=off --pin=on --rt=off --rate=2000
curl -s http://127.0.0.1:8000/metrics      # p50/p99/p99.9 for both histograms
```

Under systemd (via the Ansible role) the flags come from
`roles/trading-app-deploy/defaults/main.yml` and pinning/RT/hugepages are on.

## Runtime A/B flags (no rebuild)

| Flag | Effect |
|---|---|
| `--dispatch=crtp\|virtual` | strategy call: inlined CRTP vs vtable |
| `--alloc=pool\|malloc` | per-order scratch: object pool vs `operator new` |
| `--huge=on\|off` | hot state on 2M huge pages vs regular heap |
| `--pin=on\|off` `--rt=on\|off` | affinity + SCHED_FIFO |
| `--rate=N` | synthetic ticks/sec (default 1000) |

## Microbench (clean per-op numbers)

```bash
taskset -c 10 ./build/microbench
# dispatch crtp vs virtual   ~0.68 vs 1.36 ns   (2.0x)
# alloc    pool vs malloc    ~4.4  vs 17.7 ns   (4.0x)
# map      flat vs unordered ~1.4  vs 2.4  ns   (1.7x)
```
