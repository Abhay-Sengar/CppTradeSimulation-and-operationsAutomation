# CppTradeSimulation & Operations Automation

A low-latency **C++20 trading engine** — a lock-free, 3-hop pipeline pinned to
isolated CPU cores on bare-metal Linux — together with the **Ansible automation**
that tunes the box, deploys the workload, monitors it, and gates the market open
with Beginning-of-Day checks. Built to exercise the HFT C++ and Linux
low-latency skill set end to end.

> **Honest framing:** the *engine, isolation, measurement, and automation are
> real and run on bare metal* (Intel Core i7-1255U, isolated cores, invariant TSC,
> explicit huge pages, `SCHED_FIFO`). The transport is loopback TCP (not a real
> exchange link), so the round-trip number includes the mock exchange + kernel —
> the number the engine *owns* is the in-process tick-to-trade (`t2t`). See
> [`ProjectDeepDive.md`](ProjectDeepDive.md) §6 and §17.

---

## Headline numbers (bare metal, isolated cores, `performance` governor)

**Engine tick-to-trade** (`t2t` — tick generated → order ready to send, 100%
in-process), at 50k ticks/s on the isolated E-cores:

| p50 | p99 | p999 | max |
|---|---|---|---|
| **474 ns** | **700 ns** | **2.67 µs** | 20.4 µs |

**Round trip** (`rtt` — tick → fill, includes the mock exchange + kernel TCP):
p50 10.9 µs · p99 13.5 µs · p999 16.8 µs. The ~23× gap is the kernel/TCP cost —
the live motivation for kernel bypass.

**Microbenchmark A/B** (why the banned constructs are banned) — P-core, quiet box:

| dispatch CRTP vs `virtual` | alloc pool vs `malloc` | map flat vs `unordered_map` |
|---|---|---|
| 0.25 → 0.58 ns (**2.32×**) | 1.95 → 10.41 ns (**5.33×**) | 0.75 → 2.24 ns (**2.99×**) |

### Hardware migration: same code, two machines

This project was built on an AMD Ryzen 7 7730U and later moved to a hybrid Intel
i7-1255U (see [`ProjectDeepDive.md`](ProjectDeepDive.md) §3a). Both columns are
fully tuned, so the comparison is fair:

| `t2t` | Ryzen 7 7730U (Zen 3) | i7-1255U (isolated E-cores) |
|---|---|---|
| p50 | 588 ns | **474 ns** (−19%) |
| p99 | 1.58 µs | **700 ns** (−56%) |
| p999 | 2.0 µs | **2.67 µs** (+34%) |
| max | 2.5 µs | 20.4 µs (worse) |

The median and p99 improved — four hot threads inside one E-core L2 cluster make
the ring hand-offs cheaper than crossing the Ryzen's cache hierarchy. The **far
tail regressed**: this is a 15 W laptop part sharing a power and thermal budget,
where the Ryzen held a 2.5 µs max. Honest summary: *better typical latency, worse
worst case.*

### What each tuning knob is actually worth

Measured with [`scripts/ab_sweep.sh`](scripts/ab_sweep.sh) — one runtime flag
flipped per run, no rebuild, 1.5 M orders per variant at 50k ticks/s:

| knob turned off | p99 | max | mean | verdict |
|---|---|---|---|---|
| CRTP → `virtual` | +1% | +6% | **0%** | **invisible at engine level** |
| pool → `malloc` | +27% | +75% | +0.6% | tail only |
| hugepages off | +29% | +67% | −0.6% | tail only |
| `SCHED_FIFO` off | +18% | +93% | +1.4% | tail only |
| **pinning off** | **10×** | **154×** | **7.6×** | **the one that matters** |

Two things this says that the numbers alone don't: the per-op knobs (pool,
hugepages, FIFO) buy **predictability, not throughput** — they barely move the
mean and heavily move the max. And CRTP's 0.33 ns win is real in the microbench
yet unmeasurable in a 470 ns pipeline; it is 0.1% of the path. The instrument has
to match the question.

---

## Architecture

```
┌── LAPTOP (bare metal, Ubuntu 22.04, Intel i7-1255U hybrid) ────────────────┐
│  Housekeeping cpu 0,2,4-7          Isolated cpu 8-11 (E-cores, one L2)      │
│   ├ OS / systemd / netdata          ├ market-data ─ring1─▶ strategy        │
│   ├ engine: logger + metrics-http   │      (cpu8)          (cpu9)           │
│   └ Ansible (control plane)         │                         │ ring2       │
│                                     │                         ▼             │
│   P-core HT siblings 1,3 OFFLINE    ├ mock-exchange ◀─FIX/TCP─ gateway      │
│                                     └   (cpu11)     :9001     (cpu10)        │
│   engine :8000/metrics  ──▶ netdata :19999                                  │
└────────────────────────────────────────────────────────────────────────────┘
```

Four planes: **control** (Ansible), **data** (C++ engine + mock exchange),
**monitoring** (netdata), **readiness** (BOD checks). One hot thread per isolated
physical core; SPSC lock-free rings between hops; non-hot threads on housekeeping.
Ansible runs on the node itself (`inventory/local.yml`), or from any separate
control host over SSH (`inventory/hosts.yml`).

### Live core map — what runs on each logical CPU

```mermaid
flowchart TB
  subgraph HK["HOUSEKEEPING · cpu 0,2 (P-cores) + 4-7 (E-cores) · SCHED_OTHER"]
    direction LR
    os["OS · systemd · sshd"]
    nd["netdata :19999"]
    em["engine: main"]
    el["engine: logger"]
    eh["engine: metrics-http :8000"]
    ea["exchange: acceptor"]
  end
  subgraph ISO["ISOLATED · E-cores, shared L2 · SCHED_FIFO · nohz_full"]
    direction LR
    c8["cpu 8<br/>market-data"]
    c9["cpu 9<br/>strategy"]
    c10["cpu 10<br/>gateway"]
    c11["cpu 11<br/>mock-exchange<br/>fix-handler"]
  end
  subgraph OFF["OFFLINE · HT siblings of the housekeeping P-cores"]
    direction LR
    c1["cpu 1"]
    c3["cpu 3"]
  end
  c8 -->|"ring1 · Tick"| c9
  c9 -->|"ring2 · Order+FIX"| c10
  c10 -->|"FIX NewOrderSingle · TCP :9001"| c11
  c11 -->|"ExecutionReport"| c10
  c10 -->|"ring3 · LogEvent"| el
  eh -->|":8000/metrics"| nd
  classDef off fill:#eee,stroke:#bbb,color:#999,stroke-dasharray:4 3;
  class c1,c3 off;
  classDef hot fill:#0b7,stroke:#065,color:#fff;
  class c8,c9,c10,c11 hot;
```

The `t2t` metric covers `cpu8 → cpu9 → cpu10` (in-process); `rtt` adds the
`cpu10 ⇄ cpu11` loopback FIX hop. All four hot cores are E-cores from the *same
L2 cluster*, so the ring hand-offs between hops stay L2-local instead of going
out to L3 — and E-cores have no SMT sibling, so each hot thread owns its core
outright with nothing to offline. The sibling-offlining step instead targets
cpu1/cpu3, the HT siblings of the two P-cores that host housekeeping (the
engine's logger/metrics threads and netdata), so those run on full physical
cores. `cpu0` stays housekeeping deliberately: it is the boot CPU and
`nohz_full` must leave a timekeeping CPU alone.

---

## Repository structure

```
├── ansible.cfg                    # inventory, roles path, become=sudo, YAML output
├── inventory/
│   ├── local.yml                  # run the playbook on the node itself (default)
│   └── hosts.yml                  # or from a separate control host over SSH (edit host/user)
├── playbooks/site.yml             # runs the 4 roles (tagged: kernel/monitoring/app/bod)
├── roles/
│   ├── kernel-tuning/             # isolcpus/nohz/rcu/hugepages, RT-throttle off, SMT-offline oneshot
│   ├── monitoring-agent/          # netdata (apt), localhost bind, per-core CPU
│   ├── trading-app-deploy/        # trader user, CMake build, systemd units (RT/caps/affinity)
│   │   └── files/cpp/             # ← the C++20 engine + mock exchange (see cpp/README.md)
│   └── bod-checks/                # readiness script + config + systemd timer
├── scripts/
│   ├── latency_demo.sh            # tc/netem latency-injection demo
│   └── ab_sweep.sh                # per-knob attribution sweep + microbench (see Headline numbers)
├── INTERVIEW_NOTES.md             # every C++ topic → file map + talking points
├── ProjectDeepDive.md             # full design/measurement/war-stories study guide
└── README.md
```

The C++ lives in [`roles/trading-app-deploy/files/cpp/`](roles/trading-app-deploy/files/cpp/)
— 13 headers + 2 binaries + a microbench; see its
[README](roles/trading-app-deploy/files/cpp/README.md).

---

## Quickstart

**Build & run the engine locally** (no isolation needed — pins to whatever cores exist):

```bash
cd roles/trading-app-deploy/files/cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mock_exchange --pin=off &
./build/trading_engine --huge=off --pin=on --rt=off --rate=2000
curl -s localhost:8000/metrics | grep engine_t2t     # tick-to-trade percentiles
taskset -c 6 ./build/microbench                        # the A/B numbers
```

**Full deploy with tuning** (bare metal; needs sudo, reboot once for GRUB isolation):

```bash
ansible-playbook -i inventory/local.yml playbooks/site.yml --tags kernel -K   # isolation
sudo reboot                                                                    # activate isolcpus/hugepages
ansible-playbook -i inventory/local.yml playbooks/site.yml -K                  # build + deploy + monitor
```

Verify: `cat /proc/cmdline` (isolation params), `lscpu -e` (1/3 offline),
`bod_check.sh` (READY), `curl localhost:8000/metrics`, netdata at
`http://127.0.0.1:19999`.

---

## Runtime A/B toggles (no rebuild)

| flag | effect |
|---|---|
| `--dispatch=crtp\|virtual` | strategy call: inlined CRTP vs vtable |
| `--alloc=pool\|malloc` | per-order scratch: object pool vs `operator new` |
| `--huge=on\|off` | hot state on 2 MB huge pages vs regular heap |
| `--pin=on\|off` `--rt=on\|off` | affinity + `SCHED_FIFO` |
| `--rate=N` | synthetic ticks/sec |

---

## The two live demos

- **Latency injection** — `sudo ./scripts/latency_demo.sh on` adds `netem`
  delay on loopback; measured: `rtt` p50 goes **11.2 µs → 11.47 ms (1024×)**
  while `t2t` p50 moves **466 → 490 ns (+5%)**. The engine path is essentially
  untouched while the network path explodes — that three-orders-of-magnitude
  difference in sensitivity is the point. `off` restores baseline.
- **Failover / self-heal** — `sudo systemctl kill -s SIGKILL trading-engine` →
  systemd restarts it in ~2 s; a clean `systemctl stop` leaves it down and
  `bod_check.sh` returns **NOT READY (exit 2)** — the readiness gate.

---

## Engine metrics (`:8000/metrics`)

`engine_t2t_{p50,p99,p999,max,mean}_ns` (**engine tick-to-trade** — the number we
optimise), `engine_rtt_{…}_ns` (round trip incl. exchange), `engine_orders_total`
/ `engine_fills_total` (counters), `engine_tsc_ghz`, `engine_hugepages`. Standard
Prometheus text — plugs into Prometheus+Grafana unchanged.

---

## Mapping to the role

| Responsibility | Evidence |
|---|---|
| **C++ (core skill)** | 13-header lock-free C++20 engine: SPSC rings, HDR histogram, object pool / arena / huge-page arena, CRTP-vs-virtual, hand-rolled FIX, `pthread` affinity + `SCHED_FIFO` |
| Low-latency infra | isolcpus/nohz_full/rcu_nocbs, explicit hugepages, RT-throttle disabled, SMT-offline, performance governor, per-core pinning |
| Automation (Ansible) | 4 idempotent roles, two inventories, tags, `--check`/`--diff`, handlers |
| Kernel tuning | full boot + sysctl knob set, applied and verified |
| BOD checks | readiness role + timer; 15-pass gate incl. isolation/TSC/hugepages-in-use |
| Monitoring & alerting | netdata per-core + engine `:8000/metrics`; BOD exit codes |
| Latency troubleshooting | `perf`/`strace`/microbench methodology; documented war stories |
| High availability | `Restart=on-failure` self-heal |
| Python & Bash | Ansible + `bod_check.sh` / `latency_demo.sh` / `trade-ops-cpu-tuning.sh` |

---

## Honest limitations

- **Loopback TCP, not a real NIC** — no kernel bypass (ef_vi/DPDK/AF_XDP); the
  `t2t`↔`rtt` gap *is* the kernel-stack cost. The `rtt` number includes the mock
  exchange, which you'd never own in production; the engine's own number is `t2t`.
- **Single socket** — no real NUMA. **Not PREEMPT_RT** (kernel is
  PREEMPT_DYNAMIC). **Netdata (apt) lacks the go.d plugin**, so engine metrics
  are read from `:8000` directly; the per-core CPU dashboard works.
- The mock exchange doesn't enforce FIX sequence numbers or risk checks.

Production adds: real NIC IRQ affinity, kernel bypass, PREEMPT_RT/tickless,
NUMA-aware placement, PTP time sync, redundant paths + sub-ms failover.

---

## Tech stack

C++20 (GCC, CMake) · Ansible · Netdata · systemd · chrony · `tc/netem` ·
`perf`/`strace` · Linux CPU isolation · Git.

**Docs:** [`RUNBOOK.md`](RUNBOOK.md) — start/stop, dashboard, BOD, self-heal, and
demos, command-first · [`ProjectDeepDive.md`](ProjectDeepDive.md) — full design,
rdtsc/metrics methodology, A/B data, debugging war stories ·
[`INTERVIEW_NOTES.md`](INTERVIEW_NOTES.md) — interview-topic → file map.
