# Project Deep Dive & Study Guide

The document to **study from**. It explains the current codebase — a C++20
lock-free trading engine on an isolated, bare-metal Linux box, deployed and
tuned with Ansible — from first principles: what each piece does, why each
decision was made, exactly how latency is measured, the real numbers we got,
and the problems we hit and fixed along the way.

Companion docs: [`README.md`](README.md) (quickstart) and
[`INTERVIEW_NOTES.md`](INTERVIEW_NOTES.md) (every interview topic → file map).

---

## Table of contents

1. How to use this document
2. The big picture: four planes
3. Hardware & the isolated-core topology
4. The trading engine: C++ architecture (the 3-hop pipeline)
5. The C++ design, header by header
6. **Measurement: every rdtsc and every published stat**
7. **A/B benchmarks — microbench + live, with data**
8. **War stories — the problems we hit and fixed (with data)**
9. Kernel tuning, knob by knob
10. Ansible automation (control plane)
11. systemd units — RT, capabilities, affinity
12. FIX protocol and our two binaries
13. BOD readiness checks
14. The two demo scenarios
15. Data-flow map
16. Mapping to the job description
17. Honest limitations & what production adds
18. Glossary

---

## 1. How to use this document

The project mirrors the daily reality of keeping a low-latency trading server
healthy and fast. There are four "planes" (groups of responsibility). Every
time you meet a setting, ask three questions: **What does it do? Why does it
matter for trading? What would production do differently?** Section 6 (rdtsc +
stats) and Section 8 (war stories) are the two you can least afford to hand-wave.

---

## 2. The big picture: four planes

- **Control plane — Ansible.** *How we make changes.* Desired state is described
  in playbooks/roles; Ansible makes the machine match. Infrastructure as Code:
  repeatable, reviewable, version-controlled.
- **Data plane — the C++ engine + mock exchange.** *The trading workload.* A
  lock-free engine that turns market ticks into FIX orders and a mock exchange
  that fills them. This is the thing whose latency we obsess over.
- **Monitoring plane — Netdata.** *How we see what's happening:* per-core CPU,
  interrupts, context switches, plus the engine's own latency metrics.
- **Readiness plane — BOD checks.** *How we know we're safe to trade:* a
  pre-open script that verifies the box is tuned and healthy, gating the open.

---

## 3. Hardware & the isolated-core topology

### The physical box

Bare-metal Ubuntu 22.04 on an **AMD Ryzen 7 7730U** (Zen 3): 8 physical cores,
16 logical CPUs (SMT/hyper-threading). The SMT sibling pairs are
`(0,1) (2,3) … (8,9) (10,11) (12,13) (14,15)` — i.e. logical CPUs `8` and `9`
are the two threads of physical core 4, and so on. 14 GB RAM, invariant TSC
(`constant_tsc` + `nonstop_tsc`).

### The core plan

```
 Housekeeping (cpu 0-7)              Isolated (cpu 8,10,12,14 = phys cores 4-7)
  ├ OS, systemd, Ansible              ├ market-data thread  -> cpu 8
  ├ engine: logger + metrics-http     ├ strategy thread     -> cpu 10
  ├ netdata, sshd                     ├ gateway thread      -> cpu 12
  └ everything not pinned             └ mock-exchange        -> cpu 14
                                      SMT siblings 9,11,13,15 = OFFLINE
```

Why this shape:
- **One hot thread per *physical* core.** We isolate all of cores 4-7 and take
  the odd SMT siblings (9/11/13/15) **offline**, so each hot thread owns a whole
  core with no hyper-thread contention (two SMT threads share execution units,
  L1/L2 — a sibling running anything steals from the hot thread).
- **Every isolated core does real work.** This is an in-person interview, so
  there's no video call competing for CPU — so instead of reserving cores
  idle, the pipeline is a 3-hop chain plus the exchange, exercising
  multithreading and inter-core communication end to end.
- **Non-hot threads stay on housekeeping.** The logger and metrics-http threads
  are *not* latency-critical and deliberately run on cores 0-7 — a talking
  point in itself ("only the hot path gets a dedicated core").

The isolation is delivered two ways: **GRUB kernel command line** (needs a
reboot) for `isolcpus`/`nohz_full`/`rcu_nocbs`/`hugepages`, and a **systemd
oneshot** (`trade-ops-cpu.service`) that offlines siblings and sets the governor
at every boot (reversible — disable the unit to revert). Details in §9.

---

## 4. The trading engine: C++ architecture (the 3-hop pipeline)

```
market-data (cpu8) --ring1--> strategy (cpu10) --ring2--> gateway (cpu12)
     |                                                        |  FIX/TCP
     | rdtsc t0 stamped here, rides the whole pipeline        v  loopback :9001
     +------------------- round trip (rtt) ------------> mock-exchange (cpu14)
                                                             |
gateway --ring3--> logger (housekeeping)   metrics-http :8000 (housekeeping)
```

Six threads in the engine process:

| Thread | Core | Sched | Job |
|---|---|---|---|
| market-data | 8 | FIFO | generate synthetic top-of-book ticks, stamp `rdtsc`, push ring1 |
| strategy | 10 | FIFO | pop ring1, decide side/price, encode FIX, push ring2 |
| gateway | 12 | FIFO | pop ring2, send FIX, match ExecutionReports, record latency, push log events to ring3 |
| logger | 0-7 | OTHER | pop ring3, write to file (off the hot path) |
| metrics-http | 0-7 | OTHER | serve Prometheus text on `:8000/metrics` |
| main | 0-7 | OTHER | startup, TSC calibration, signal handling, join |

The three hot threads communicate through **SPSC (single-producer /
single-consumer) lock-free ring buffers** — no locks, no syscalls, no
allocation on the hot path. The mock exchange is a separate process pinned to
cpu 14.

**Ring payloads** (`messages.hpp`), all trivially-copyable POD:
- `ring1` `Tick{ t0_tsc, sym_id, seq, bid, ask }`
- `ring2` `Order{ t0_tsc, clordid, sym_id, side, px, qty, fix_len, fix[120] }`
- `ring3` `LogEvent{ tsc, type, len, msg[54] }`

The `t0_tsc` stamped at tick generation **rides the whole pipeline**, which is
what makes end-to-end latency measurable (§6).

**Runtime A/B toggles** (CLI flags, no rebuild): `--dispatch=crtp|virtual`,
`--alloc=pool|malloc`, `--huge=on|off`, `--pin=on|off`, `--rt=on|off`,
`--rate=<ticks/sec>`.

---

## 5. The C++ design, header by header

All under `roles/trading-app-deploy/files/cpp/`. Each header is heavily
commented; this is the map. (See `INTERVIEW_NOTES.md` for the topic→file table.)

| File | What it implements | Key ideas |
|---|---|---|
| `spsc.hpp` | SPSC lock-free ring | pow-2 capacity + bitmask wrap; `alignas(64)` head/tail on separate cache lines (false-sharing fix); acquire/release only; cached opposite-index to avoid cross-core loads |
| `util.hpp` | `cpu_relax()` | `_mm_pause` in spin loops (power, SMT yield, avoids memory-order-violation flush) |
| `tsc.hpp` | rdtsc + calibration | `rdtsc_fast` (hot path) vs `rdtsc_serialized` (fenced); calibrate ns/tick vs `CLOCK_MONOTONIC_RAW`; invariant-TSC check |
| `hist.hpp` | HDR histogram | sub-buckets per octave via `__builtin_clzll` (~1.5% rel. error); lock-free relaxed atomic buckets |
| `pool.hpp` | object pool | intrusive free-list (union), placement new + explicit dtor; O(1), no allocator |
| `bump.hpp` | arena allocator | pointer-bump + alignment, `reset()` frees all |
| `huge.hpp` | huge-page arena + mlock | `mmap(MAP_HUGETLB\|MAP_POPULATE)`, `madvise`, `mlockall`; falls back to normal pages |
| `strategy.hpp` | CRTP vs virtual dispatch | one shared `decide()`; both flavours selectable at startup |
| `fix.hpp` | FIX 4.2 encode/parse | fixed-buffer builder (no alloc), `string_view`/`from_chars` parse, `Framer` for TCP; plus a `#pragma pack` binary-wire discipline demo |
| `flatmap.hpp` | open-addressing map | linear probing vs `std::unordered_map`; the "flat vs node-based" artifact |
| `affinity.hpp` | pin + SCHED_FIFO | `pthread_setaffinity_np`, `set_realtime`, RAII `AffinityGuard` |
| `net.hpp` | socket helpers | `TCP_NODELAY`, `SO_BUSY_POLL`, `send_all`, FIX timestamps |
| `types.hpp` `messages.hpp` `config.hpp` | strong int64 money types, ring PODs, constexpr/consteval config | `enum class`, `[[nodiscard]]`, `operator<=>`, `constinit` |

Build: `CMakeLists.txt`, C++20, `-O3 -march=native -fno-rtti -Wall -Wextra
-Wconversion -Wshadow -Werror`; separate ASan+UBSan and TSan presets; a
dependency-free `microbench` target plus optional GTest/Google-Benchmark ones.

---

## 6. Measurement: every rdtsc and every published stat

This is the section to be able to recite cold.

### 6.1 rdtsc — what it is and the two variants

`rdtsc` reads the CPU's 64-bit **timestamp counter** in a few cycles — far
cheaper than `clock_gettime` (a vDSO call or syscall). We use it because we
timestamp *every* tick on the hot path and can't afford a syscall there.

Two variants (`tsc.hpp`):
- **`rdtsc_fast()`** = bare `__rdtsc()`. Unfenced, ~a few cycles. Used on the
  **hot path** (every tick), where a few cycles of instruction-reordering skew
  is irrelevant.
- **`rdtsc_serialized()`** = `lfence; __rdtscp(); lfence`. `rdtscp` waits for
  prior instructions to retire; the trailing `lfence` stops later instructions
  hoisting above the read. Used only at **calibration boundaries** and inside
  the **microbench** timing loop, where a precise fence matters.

**Why rdtsc is valid here.** The box has **invariant TSC** (`constant_tsc` +
`nonstop_tsc`): the counter ticks at a fixed nominal rate regardless of
P-state/turbo, doesn't stop in idle, and is synchronised across cores. That last
property is what lets us stamp a tick on cpu 8 and read the counter on cpu 12
and subtract — cross-core rdtsc subtraction is only valid *because* the TSC is
invariant. (`tsc_is_invariant()` checks the flags; `bod_check.sh` gates on it.)

### 6.2 Calibration — turning cycles into nanoseconds

rdtsc counts **cycles**, not nanoseconds. `TscClock::calibrate()` reads the TSC
and `CLOCK_MONOTONIC_RAW` at two points ~100 ms apart and divides to get
`ns_per_tick`. We use `MONOTONIC_RAW` (not `MONOTONIC`) so NTP slewing can't
distort the ratio. On this box it measures ≈0.50 ns/tick ≈ **1.996 GHz** — the
7730U's 2.0 GHz base clock, which is exactly what an invariant TSC counts at.
Calibration runs on a `std::async` worker so its window overlaps the rest of
startup (`std::future` demo).

### 6.3 Where each timestamp is taken

| Point | Thread | Call | Purpose |
|---|---|---|---|
| `t0` = tick generated | market-data (cpu8) | `rdtsc_fast()` → `Tick.t0_tsc` | the stopwatch **start** |
| `t2t` end | gateway (cpu12) | `rdtsc_fast()` just **before `send()`** | order encoded & ready |
| `rtt` end | gateway (cpu12) | `rdtsc_fast()` when **ExecutionReport arrives** | fill received |

The gateway keeps `t0` in a **direct-mapped array** `t0_by_id[clordid & MASK]`
(O(1), no hashing — clordids are monotonic), so when a fill comes back it looks
up the originating tick's timestamp and computes `rtt = now − t0`.

### 6.4 The two latencies — and which one is "the engine"

This distinction matters (and it's a common interview trap):

- **`t2t` (tick-to-trade)** = `t0` (tick generated) → gateway about to `send()`.
  This spans market-data → ring1 → strategy → ring2 → gateway, **entirely
  inside the engine process, on the isolated cores**. It contains *no* syscall,
  *no* exchange, *no* network. **This is the number we own and optimise** — the
  true tick-to-trade. This is where the CRTP/pool/hugepage A/B deltas live.
- **`rtt` (round trip)** = `t0` → ExecutionReport received back. It includes the
  `send()` syscall, the kernel TCP stack **twice**, the loopback, **and the mock
  exchange's own processing**. So `rtt` is **NOT** engine-only — a big chunk of
  it is the exchange and the kernel network stack, which in real life you don't
  control. It's kept as a diagnostic: the gap between `t2t` (~0.6 µs) and `rtt`
  (~43 µs) *is* the kernel/network cost, and the live motivation for kernel
  bypass.

> The metrics were originally named `pipeline`/`tick_to_trade`; they are now
> `t2t`/`rtt` precisely so the engine-only number is called what it is and the
> round-trip isn't mislabelled "tick-to-trade."

### 6.5 The histogram (`hist.hpp`)

Latency is recorded into an **HDR-style histogram**: each power-of-two octave is
split into 64 linear sub-buckets, indexed via `__builtin_clzll`. That gives a
fixed ~1.5% relative error from nanoseconds to seconds in a ~2240-slot array
(~18 KB). Why not an average? **A mean hides the tail, and in trading the tail
is the risk** — a p99.9 stall is the order that misses the market. The buckets
are `std::atomic<u64>` with **relaxed** increments (gateway records) and relaxed
reads (metrics thread snapshots) — correct because the buckets are independent
counters and percentiles only need an approximate snapshot; we deliberately pay
no lock or acquire/release on the hot path. The first `warmup_orders` (500) are
excluded so cold-cache/TLB-fill outliers don't skew the distribution.

### 6.6 Every published stat (`:8000/metrics`, Prometheus text)

The metrics-http thread serves these (all in nanoseconds unless noted):

| Metric | Type | Meaning |
|---|---|---|
| `engine_orders_total` | counter | orders sent (rate = orders/sec) |
| `engine_fills_total` | counter | ExecutionReports received |
| `engine_t2t_{p50,p99,p999,max,mean}_ns` | gauge | **engine tick-to-trade** (§6.4) — the number we optimise |
| `engine_rtt_{p50,p99,p999,max,mean}_ns` | gauge | round trip incl. exchange + kernel TCP |
| `engine_tsc_ghz` | gauge | calibrated TSC frequency |
| `engine_hugepages` | gauge | 1 if hot state is on huge pages, else 0 |

Percentiles are computed **in-process** from the HDR histogram and exposed as
plain gauges (rather than raw buckets), so any scraper charts them directly. The
same `/metrics` endpoint plugs into Prometheus+Grafana unchanged — that
portability is the point (we don't run a Prometheus *server*; Netdata scrapes
the endpoint, §10).

### 6.7 "Zero syscalls on the hot path" — the honest version

`strace -c` on the engine shows the **market-data and strategy stages are
syscall-free** (pure compute + ring ops). The **gateway necessarily makes
`send`/`recv` syscalls** for TCP — that's unavoidable with the kernel stack, and
is exactly what kernel-bypass NICs (ef_vi/DPDK) remove. Be precise about this in
the interview: the `t2t` compute path is syscall-free; the wire egress is not.

---

## 7. A/B benchmarks — microbench + live, with data

Two complementary tools:
- **`microbench`** (`bench/microbench.cpp`, dependency-free, rdtsc-timed,
  min-over-21-trials) isolates the *per-operation* cost of each design choice at
  nanosecond resolution — where a ~10 ns dispatch delta is measurable.
- **The live engine** `t2t` histogram shows the *tail* effects (allocation
  jitter, isolation, hugepages) that a microbench mean can't.

### Microbench (isolated core, performance governor)

| Experiment | A | B | ratio | Why |
|---|---|---|---|---|
| dispatch: CRTP vs `virtual` | 0.46 ns | 0.94 ns | **2.06×** | vtable load + indirect (un-inlinable) branch |
| alloc: pool vs `malloc` | 2.88 ns | 12.58 ns | **4.36×** | allocator bookkeeping/locks vs O(1) free-list |
| map: flat vs `unordered_map` | 1.02 ns | 1.67 ns | **1.64×** | contiguous probing vs node pointer-chasing |

### Live pipeline (isolated cores, RT, throttling disabled)

| Histogram | p50 | p99 | p999 | max |
|---|---|---|---|---|
| `t2t` (engine tick-to-trade) | **588 ns** | **1.58 µs** | **2.0 µs** | 2.5 µs |
| `rtt` (round trip, incl. exchange) | 43 µs | 59 µs | 67 µs | — |

Reading them together: the microbench proves *why* each banned construct is
banned (mean cost); the live `t2t` proves the whole pipeline holds a
**~2 µs p999** on real hardware; the `rtt`/`t2t` gap (~70×) is the kernel/TCP
tax. Benchmark hygiene throughout: isolated core, performance governor, warm-up
iterations, min-over-trials, `do_not_optimize` compiler barriers, production
build flags.

---

## 8. War stories — the problems we hit and fixed (with data)

Real bring-up problems on the bare-metal box. Each is a good "tell me about a
bug you debugged" answer.

**Q: The pipeline p99 was 884 ms and p999 was 1.78 s — on isolated cores. What?**
> Two compounding causes. (1) The systemd unit pinned the *whole* engine process
> to `CPUAffinity=8 10 12`, so the non-hot **logger/metrics/main threads were
> trapped on the isolated cores** alongside the SCHED_FIFO hot threads. (2) The
> kernel's **RT bandwidth throttling** (default `sched_rt_runtime_us=950000`)
> throttles SCHED_FIFO to 95% — a ~50 ms stall every second — so those trapped
> SCHED_OTHER threads could run. That stall froze the gateway and the backlog
> inflated the tail to hundreds of ms.
>
> **Fix:** widen the unit affinity to `0-7 8 10 12` so isolcpus keeps the
> non-hot threads on housekeeping while the hot threads pin to the isolated
> cores; and set **`kernel.sched_rt_runtime_us=-1`** to disable RT throttling
> (safe because nothing else is scheduled on the isolated cores).
> **Result:** p99 **884 ms → 1.58 µs**, p999 **1.78 s → 2.0 µs** — a ~500,000×
> tail improvement. The single best "why each knob matters" story in the project.

**Q: Why does disabling RT throttling not hang the box?**
> Because the FIFO threads live on isolated cores with nothing else runnable
> there, and housekeeping cores 0-7 are untouched. If a FIFO thread had been
> left on a housekeeping core, `-1` *would* be dangerous — which is exactly why
> the affinity fix and the throttle fix had to land together. (Also why the
> first symptom of getting it wrong was the microbench *hanging* when I pinned it
> to core 10 — the strategy FIFO thread there never yields, so a SCHED_OTHER
> benchmark starves forever.)

**Q: The histogram couldn't tell 770 ns from 800 ns. Why, and the fix?**
> The first histogram had **one bucket per octave** (plain log2), so anything in
> [512, 1024) ns collapsed into a single bucket — the CRTP/alloc A/B deltas were
> invisible. Fixed by making it a proper **HDR histogram with 64 sub-buckets per
> octave** (~1.5% resolution), which resolves 770 vs 800 into distinct buckets.
> Lesson: your instrument's resolution has to beat the effect you're measuring.

**Q: Netdata silently failed to install. How did you find it and fix it?**
> The upstream static kickstart exited 0 but produced **no binary** (a flaky
> remote tarball download); the next task failed with "config dir doesn't
> exist." Diagnosed by checking `/opt/netdata/usr/sbin/netdata` (missing).
> Switched the role to the **Ubuntu apt package** — reliable, standard
> `/etc/netdata` paths. Caveat learned: Ubuntu's netdata 1.33 ships **without
> the `go.d` plugin**, so it can't scrape the engine's Prometheus endpoint into
> the dashboard; the per-core CPU view still works and `t2t` comes from `:8000`.

**Q: GRUB isolation didn't take effect after the first reboot. Why?**
> `/proc/cmdline` still showed `quiet splash` even though `/etc/default/grub`
> was correct — `grub.cfg` had never been regenerated (the `update-grub` handler
> got tangled in an earlier *failed* play, and on the clean re-run the grub task
> was unchanged so the handler didn't fire). Confirmed the sourced value was
> right (no `grub.d` override), ran `update-grub` manually, verified `isolcpus`
> appeared in `grub.cfg`, rebooted. Lesson: verify the *generated* artifact
> (`grub.cfg`), not just the source (`/etc/default/grub`).

**Q: SMT siblings wouldn't offline at boot — `write error: Device or resource busy`.**
> The oneshot ran at `sysinit.target` (`DefaultDependencies=no`) — too early;
> the CPU-hotplug offline write returns EBUSY while the scheduler is still
> migrating kernel threads off the core. It worked fine later at runtime.
> **Fix:** run the unit **late** (`After=basic.target`, default deps) and add a
> **5× retry with backoff** in the script. Offlines cleanly at boot now.

**Q: A couple of Ansible tasks failed on `set -o pipefail`.**
> Ansible's `shell` runs under `/bin/sh` = **dash**, which rejects `pipefail`.
> Fixed by adding `args: executable: /bin/bash` to the two tasks that need it.
> (Also fixed a pre-existing IRQ-mask bug: the old code did `pow(cpu, 2)` instead
> of `2**cpu` and coerced a CPU *range string* through `int()` — replaced the
> whole hex-mask computation by writing `smp_affinity_list` directly, which
> takes `0-7` as-is.)

---

## 9. Kernel tuning, knob by knob

Delivered by the `kernel-tuning` role. Boot-time params (GRUB cmdline, need a
reboot) and runtime params (sysctl + the cpu oneshot).

**GRUB cmdline** (`/proc/cmdline` after reboot):
`isolcpus=8-15 nohz_full=8-15 rcu_nocbs=8-15 irqaffinity=0-7
transparent_hugepage=never default_hugepagesz=2M hugepagesz=2M hugepages=512`

- **`isolcpus=8-15`** — remove logical CPUs 8-15 from the scheduler's automatic
  load-balancer; only explicitly-pinned threads land there.
- **`nohz_full=8-15`** — stop the periodic ~1000 Hz timer tick on those cores
  when only one task runs there (needs `rcu_nocbs`). Fewer interrupts = less
  jitter.
- **`rcu_nocbs=8-15`** — offload RCU callback processing to housekeeping cores.
- **`irqaffinity=0-7`** — route device IRQs to housekeeping cores from boot.
- **`transparent_hugepage=never`** — THP is assembled/split lazily by
  khugepaged at unpredictable times (jitter); use explicit huge pages instead.
- **`hugepages=512` (2 MB each = 1 GB)** — reserved early at boot (most
  reliable), so the engine's `mmap(MAP_HUGETLB)` arena is backed by huge pages.

**Runtime sysctls** (`/etc/sysctl.d/99-trade-ops.conf`):
- **`kernel.sched_rt_runtime_us=-1`** — disable RT throttling so the pinned
  FIFO threads run uninterrupted (see §8; this is the one that killed the
  880 ms tail).
- `vm.swappiness=10` (avoid swap), `vm.nr_hugepages=512` (belt-and-suspenders),
  `net.core.busy_poll/busy_read=50`, `rmem_max/wmem_max=16 MB`,
  `netdev_max_backlog=5000`, `tcp_fastopen=3`.

**The cpu oneshot** (`trade-ops-cpu.service`, every boot, reversible):
offlines SMT siblings 9/11/13/15 (so each hot core is dedicated), sets the
governor to **performance**, and optionally disables turbo (off by default; on
for benchmark-stable numbers). Plus **global `CPUAffinity=0-7`** in
`/etc/systemd/system.conf` so every *other* systemd service defaults off the
isolated cores.

---

## 10. Ansible automation (control plane)

Standard Ansible: an **inventory** (hosts), **playbook** (`site.yml`, runs the
four roles in order), **roles** (reusable task bundles), **handlers**
(change-triggered, e.g. restart-on-config-change), **idempotency** (re-runnable),
`--check`/`--diff` dry runs, `become` (sudo).

Two inventories:
- **`inventory/local.yml`** — the default: run the playbook directly on the
  trading node (`ansible_connection=local`). Used throughout bring-up.
- **`inventory/hosts.yml`** — the *same* playbook from a separate control host
  over SSH (edit the host/user). Shows the "control node manages a separate
  trading node" story without any change to the roles.

The four roles (each tagged for staged rollout — `--tags kernel,app,…`):

| Role | Does |
|---|---|
| `kernel-tuning` | isolation (GRUB), sysctls, cpu oneshot, IRQ affinity, `system.conf` CPUAffinity |
| `monitoring-agent` | install netdata (apt), bind localhost, per-core CPU on |
| `trading-app-deploy` | create `trader` user, **build the C++ via CMake**, install binaries, systemd units, netdata scrape config, start services |
| `bod-checks` | readiness script + config + systemd timer |

The C++ is **built on the target** (`cmake -S … -B build && cmake --build`) and
the binaries installed to `/opt/trade-ops/app/bin/`. Deploying via CMake (not
copying prebuilt binaries) keeps the build reproducible and demonstrates the
CMake toolchain.

---

## 11. systemd units — RT, capabilities, affinity

Both binaries run as unprivileged `trader` via systemd. The interesting
directives:

- **`CPUAffinity=0-7 8 10 12`** (engine) / `0-7 14` (exchange) — the process may
  use housekeeping + its isolated cores; the binary pins each hot thread to its
  exact isolated core internally (`affinity.hpp`), and isolcpus keeps the
  unpinned threads on 0-7. (Overrides the global `CPUAffinity=0-7`.)
- **`AmbientCapabilities=CAP_SYS_NICE CAP_IPC_LOCK`** — lets the non-root process
  set `SCHED_FIFO` (`CAP_SYS_NICE`) and `mlockall` (`CAP_IPC_LOCK`) without
  running as root.
- **`LimitRTPRIO=99`** + **`LimitMEMLOCK=infinity`** — raise the RT-priority and
  locked-memory rlimits so FIFO scheduling and huge-page locking are permitted.
- **`Restart=on-failure` + `RestartSec=2`** — self-heal: a crash/SIGKILL is
  restarted in ~2 s; a clean `systemctl stop` is *not* a failure (so it stays
  down — the BOD "NOT READY" demo). The engine `Requires=` + `After=` the
  exchange.

The code brings threads up as pinned first, verifies placement, *then* elevates
to `SCHED_FIFO` — so a mis-set affinity can never strand a busy FIFO spin on a
housekeeping core.

---

## 12. FIX protocol and our two binaries

**FIX 4.2** is a text protocol: `tag=value` fields separated by SOH (0x01),
over a long-lived TCP session (logon → orders/executions → heartbeats). We
hand-roll a minimal encoder/parser in C++ (`fix.hpp`) rather than pull a library,
so the hot path builds messages into a **fixed stack buffer with no allocation**
and parses with `string_view`+`from_chars`. `Framer` pulls complete messages out
of the TCP byte stream using `BodyLength(9)`. A `#pragma pack(1)` `BinOrder`
struct + `memcpy` (de)serialise demonstrates binary-wire discipline (the ITCH/
OUCH style) with `static_assert(is_trivially_copyable_v<>)`.

Tags used: header `8/9/35/49/56/34/52`; `A` Logon (`98/108`); `D` NewOrderSingle
(`11/55/54/38/40/44`); `8` ExecutionReport (`37/11/17/150/39/32/31`); `0/1`
heartbeat/testrequest; trailer `10` CheckSum.

- **`mock_exchange.cpp`** — acceptor on `127.0.0.1:9001`, connection handler
  pinned to cpu 14; acks logon, fills every NewOrderSingle instantly with an
  ExecutionReport.
- **`engine_main.cpp`** — the pipeline of §4.

Why FIX and not ITCH/OUCH: ITCH/OUCH are Nasdaq binary protocols; FIX is
vendor-neutral and the concepts (sessions, orders, executions) transfer. NSE's
real interface is NNF (encryption-mandated) with TBT/MTBT multicast market data —
a generic FIX PoC is enough to show end-to-end understanding.

---

## 13. BOD readiness checks

`bod-checks` installs `bod_check.sh` + a templated `/etc/trade-ops/bod.conf` +
a systemd timer (`Mon..Fri 08:45 IST`, ~30 min pre-open). Checks: disk, memory,
**clock offset via chrony**, **CPU isolation** (`isolcpus` in `/proc/cmdline`),
**SMT siblings offline**, **invariant TSC**, **hugepages reserved *and in use***
(free < total proves the engine mapped its arena — closing a gap the v1 project
had), THP off, governor, NIC link, services active, exchange reachable. Exit
codes drive alerting: `0`=READY, `1`=READY WITH WARNINGS, `2`=NOT READY (the
unit's `SuccessExitStatus=1` means warnings don't flag it, a real failure does).

Current result: **15 pass, 0 fail, READY** (2 warnings are unused wired NICs).

---

## 14. The two demo scenarios

**A — latency injection (`scripts/latency_demo.sh on`).** `tc qdisc … netem
delay 5ms 1ms` on `lo` adds ~5 ms each direction, so `rtt` on the dashboard
jumps ~10 ms — a known impairment, visible on your own instrumentation. `off`
restores baseline. (Note: this inflates `rtt`, the round trip; `t2t` — the
engine compute path — is unaffected, which itself makes the point about what we
control.)

**B — failover / self-heal.** `systemctl kill -s SIGKILL trading-engine` →
systemd restarts it in ~2 s (`NRestarts` increments), it re-logs on. A clean
`systemctl stop` leaves it down → `bod_check.sh` reports NOT READY (exit 2),
showing the gate would block a market open.

---

## 15. Data-flow map

| # | From | To | Channel | What |
|---|---|---|---|---|
| 1 | Ansible (local, or a remote control host) | trading node | local / SSH 22 | config, C++ build, service control |
| 2 | market-data thread | strategy thread | ring1 (in-process, cpu8→10) | `Tick` |
| 3 | strategy thread | gateway thread | ring2 (in-process, cpu10→12) | `Order` (+ encoded FIX) |
| 4 | gateway | mock-exchange | TCP 9001 loopback | FIX NewOrderSingle |
| 5 | mock-exchange | gateway | TCP 9001 loopback | FIX ExecutionReport |
| 6 | gateway | logger thread | ring3 (in-process → cpu0-7) | `LogEvent` |
| 7 | engine metrics-http | (exposes) | HTTP 8000 localhost | Prometheus text (t2t/rtt/counters) |
| 8 | netdata (trade-metrics.plugin) | engine | HTTP 8000 | scrape → t2t/rtt/orders charts |
| 9 | kernel | netdata | /proc,/sys | per-core CPU, IRQs, ctxsw, net, mem |
| 10 | BOD timer | BOD service | systemd | pre-open readiness run |

Mental model: the **hot path is rings (2,3,6) + one loopback TCP hop (4,5)**;
the `t2t` metric covers the in-process part, `rtt` adds the TCP hop + exchange.

---

## 16. Mapping to the job description

| JD responsibility | Evidence |
|---|---|
| Low-latency infra management | isolcpus/nohz_full/rcu_nocbs, hugepages, RT throttle off, SMT-offline, governor; engine pinned SCHED_FIFO per core |
| C++ (the core skill) | 13-header lock-free C++20 engine (SPSC, HDR histogram, pool/arena/hugepage, CRTP, FIX, affinity) |
| Automation with Ansible | 4 idempotent roles, two inventories, `--check`/`--diff`, tags, handlers |
| Linux kernel tuning | the full §9 knob set, applied and verified |
| Beginning-of-Day checks | `bod-checks` role + timer, 15-pass readiness gate |
| Monitor & alert | netdata per-core + engine `:8000/metrics`; BOD exit codes as a signal |
| Troubleshoot latency, find bottlenecks | the §8 war stories; `perf`/`strace`/microbench methodology |
| High availability | `Restart=on-failure` self-heal demo |
| Python & Bash | Ansible + `bod_check.sh`/`latency_demo.sh`/`trade-ops-cpu-tuning.sh` |

---

## 17. Honest limitations & what production adds

- **`rtt` includes the mock exchange + loopback** — not a real exchange link;
  the number we own is `t2t`.
- **Loopback TCP, not a real NIC** — no kernel-bypass (ef_vi/DPDK/AF_XDP); the
  kernel stack is in the path (that's the `t2t`↔`rtt` gap).
- **Single socket** — no real NUMA (`numactl --hardware` shows one node); on a
  2-socket box you'd co-locate NIC IRQ + core + memory on one node.
- **Not PREEMPT_RT** — kernel is PREEMPT_DYNAMIC (`preempt=full` available); RT
  is a separate build/reboot.
- **Netdata (apt) lacks go.d** — engine metrics come from `:8000` directly.
- **Mock exchange** doesn't enforce sequence numbers or risk checks.

Production adds: real NIC IRQ affinity, kernel bypass, PREEMPT_RT/tickless,
NUMA-aware placement, PTP time sync, redundant paths + sub-ms failover, a
hardened validated FIX engine, PGO/LTO release pipeline.

---

## 18. Glossary

- **Determinism / jitter** — bounded, consistent latency / its variation (the enemy).
- **isolcpus / nohz_full / rcu_nocbs** — boot params that quiet a core: off the load-balancer, tickless, RCU offloaded.
- **SMT / hyper-threading** — two logical CPUs sharing one physical core's execution units; we offline one sibling per hot core.
- **SCHED_FIFO / RT throttling** — a real-time scheduling policy (runs until it yields) / the kernel's 95% cap on RT time, which we disable on isolated cores.
- **Invariant TSC** — `constant_tsc`+`nonstop_tsc`; the TSC ticks at a fixed rate and is cross-core comparable — the precondition for cross-core rdtsc timing.
- **rdtsc / rdtscp / lfence** — read the timestamp counter / serializing variant / load-fence used to stop reordering around the read.
- **t2t (tick-to-trade)** — tick in → order ready; **engine-only**, the number we optimise.
- **rtt (round trip)** — tick → fill received; includes the exchange + kernel TCP.
- **HDR histogram** — high-dynamic-range histogram with sub-buckets per octave for fixed relative error across scales.
- **SPSC ring** — single-producer/single-consumer lock-free queue; no CAS needed.
- **False sharing** — two cores writing different variables on the same cache line; fixed with `alignas(64)`.
- **CRTP** — Curiously Recurring Template Pattern; compile-time polymorphism replacing virtual dispatch on the hot path.
- **Object pool / bump allocator / hugepages / mlockall** — pre-allocated recycled storage / pointer-bump arena / 2 MB pages for TLB coverage / pin pages in RAM.
- **FIX / NewOrderSingle (35=D) / ExecutionReport (35=8)** — text trading protocol / order / fill.
- **Prometheus exposition format vs server** — the text convention we publish (no server run; netdata scrapes it).
- **BOD** — Beginning-of-Day readiness checks gating the market open.
- **Ansible role / handler / idempotency / become** — reusable task bundle / change-triggered task / re-runnable safely / sudo.
- **PREEMPT_RT / DPDK / NUMA / PTP** — fully-preemptible kernel / user-space packet I/O / non-uniform memory / sub-µs time sync.
