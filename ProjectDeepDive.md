# Project Deep Dive & Study Guide

This is the document to **study from**. It assumes you are new to the operations domain and explains every concept from first principles, why each decision was made, exactly what data moves where and who sends it, and how all of it maps to the Trade Operations Engineer role. Read it top to bottom once, then use the FAQ (Section 14) and Glossary (Section 16) to drill.

---

## Table of contents

1. How to use this document
2. The big picture: four planes
3. Host & VM topology — and why
4. Ansible from scratch (and how ours is wired)
5. The roles, parameter by parameter
6. systemd from scratch (and our units)
7. FIX protocol from scratch (and our two scripts)
8. Tick-to-trade and the metrics
9. Monitoring: Netdata and what's on the dashboard
10. The complete data-flow map (who sends what, where)
11. The two scenarios, in depth
12. Directory structure, annotated
13. Mapping to the job description
14. FAQ (frequently asked questions)
15. Honest limitations & how bare metal differs
16. Glossary

---

## 1. How to use this document

The project simulates the daily reality of keeping a low-latency trading server healthy and fast. There are four "planes" (groups of responsibility). If you can explain each plane, why each setting exists, and what would be different on real hardware, you understand the whole project. Every time you see a setting, ask yourself three questions: **What does it do? Why does it matter for trading? What changes on bare metal?**

---

## 2. The big picture: four planes

Everything runs on one Ubuntu virtual machine called the **trading node**. We split the work into four planes:

- **Control plane — Ansible.** This is *how we make changes*. Instead of typing commands by hand, we describe the desired state in code (playbooks and roles) and Ansible makes the machine match that description. This is "Infrastructure as Code." It is repeatable, reviewable, and version-controlled in Git.
- **Data plane — the FIX apps.** This is *the trading workload itself*: a trading engine that sends orders and a mock exchange that fills them, talking the FIX protocol over TCP. This is the thing whose latency we care about.
- **Monitoring plane — Netdata.** This is *how we see what's happening*: CPU, memory, network, interrupts, and our custom tick-to-trade latency, all on a live dashboard.
- **Readiness plane — BOD checks.** This is *how we know we're safe to trade*: a script that runs before the market opens and verifies the machine is healthy, gating the open.

A real trade-ops engineer lives in all four planes every day: automating changes (control), keeping the trading apps fast (data), watching dashboards and alerts (monitoring), and running pre-open checks (readiness).

---

## 3. Host & VM topology — and why

### The physical layout

```
Windows laptop (host)
  └── VirtualBox (hypervisor)
        └── Ubuntu Server VM  =  the "trading node"   (4 vCPU, 3 GB RAM, 25 GB disk)
              ├── Ansible (control node, co-located)
              ├── Trading Engine + Mock Exchange (FIX)
              ├── Netdata agent
              └── BOD checks (systemd timer)
```

### Decision: VM, not dual-boot bare metal

We considered dual-booting Ubuntu directly on the laptop to get "real" bare-metal kernel access. We chose a VM instead because:

- **Risk.** Repartitioning a live Windows disk risks an unbootable machine and a lost evening. The reward was small.
- **The numbers don't matter here anyway.** The *value* of the kernel-tuning work is the *workflow and knowledge*, which are identical in a VM. Bare metal's only real gain was RAM headroom and "authentic" latency figures — and we explicitly treat the figures as a methodology demo, not production numbers.
- **A stable demo.** Sharing a VM window from the familiar Windows desktop is far safer for a live screen-share than rebooting into a freshly-installed Linux.

### Decision: disable Hyper-V / WSL2 on Windows

VirtualBox needs exclusive access to the CPU's hardware-virtualization feature (Intel VT-x). Windows features Hyper-V and WSL2 also claim VT-x exclusively. If both are on, VirtualBox runs in a crippled slow mode or fails. So we disabled `hypervisorlaunchtype`, `VirtualMachinePlatform`, and `HypervisorPlatform`. (Hyper-V itself wasn't installed on this Windows edition, which is fine.)

### Decision: Docker-in-VM was dropped in favor of systemd services

The original plan used Docker containers for the FIX apps. We switched to plain **systemd services** because: systemd is explicitly in the job's required skills; it lets Ansible *deploy* them, BOD *check* them (`systemctl is-active`), and gives a free **high-availability** story (`Restart=on-failure`). One decision satisfied three JD lines.

### Decision: control node co-located with the managed node

In a real shop, the Ansible control node is a separate box that manages many remote servers over SSH. Here, to fit in 3 GB of RAM, Ansible runs *on* the trading node and manages it over SSH to `127.0.0.1`. **The honest framing:** the playbooks, roles, inventory, SSH transport, and idempotency are exactly what you'd use against remote colocation servers — in production you simply change the inventory to point at the colo hosts. Nothing about the automation is fake; only the network distance is collapsed.

### Note on the environment

This VM happens to run a very new Ubuntu development build (kernel 7.0, Python 3.14). That occasionally surfaced edge cases (e.g., a package not yet built for the release, and `-updates`/`-security` repo metadata timestamped slightly in the future). Those are normal "bleeding-edge distro" quirks; the project works around them.

---

## 4. Ansible from scratch (and how ours is wired)

### Core concepts

- **Control node:** the machine that runs Ansible.
- **Managed node / target / host:** the machine being configured. Reached over SSH; needs only Python and SSH (no agent).
- **Inventory:** a list of hosts, grouped. Ours defines a group `trading_nodes` with one host `trading-node`.
- **Module:** a unit of work Ansible knows how to do idempotently (e.g., `apt`, `sysctl`, `systemd`, `template`, `copy`, `command`).
- **Task:** one invocation of a module with parameters, plus a human-readable name.
- **Playbook:** an ordered list of plays; a play maps a group of hosts to a list of roles/tasks.
- **Role:** a reusable bundle of tasks, variables, files, templates, and handlers, in a standard folder layout. Roles keep things organized and reusable.
- **Handler:** a special task that only runs when "notified" by another task that *changed* something (e.g., reload systemd only if a unit file changed). Handlers run at the end of the play.
- **Idempotency:** running the playbook twice produces no changes the second time. Ansible reports `changed` only when it actually altered state. This is the heart of safe automation — you can re-run anytime.
- **Check mode (`--check`) and diff (`--diff`):** a dry run that *previews* changes without making them, showing a diff of files it would write. This is how a professional validates before touching production.
- **`become`:** privilege escalation (run as root via sudo).
- **`ansible-lint`:** a linter that enforces best practices (naming, idempotency hints, schema). Ours passes at the strict `production` profile.

### Our wiring

- **`ansible.cfg`** sets defaults so we don't repeat flags: the inventory path, the roles path, `host_key_checking = False` (don't prompt on first SSH), YAML-formatted output, `become = True` with sudo, and quieting deprecation noise.
- **`inventory/hosts.yml`** declares `trading-node` reached at `127.0.0.1:22` as user `trader` over SSH.
- **`playbooks/site.yml`** is the single entry point. It targets `trading_nodes`, escalates to root, and runs the roles **in order**:
  1. `kernel-tuning` (set up the fast substrate)
  2. `monitoring-agent` (so the dashboard exists)
  3. `trading-app-deploy` (the workload + its metrics)
  4. `bod-checks` (readiness, which depends on the services above existing)

Order matters: BOD's config lists the trading services, so those services should be deployed before BOD's config references them.

A few professional touches worth noting: every task is named with a capitalized, human sentence; we use real modules (not raw shell) wherever possible so Ansible can report change state correctly; tasks that can't run meaningfully in check mode are guarded with `when: not ansible_check_mode`; read-only fact-gathering is marked `check_mode: false` so it always runs.

---

## 5. The roles, parameter by parameter

### 5.1 `kernel-tuning` — the low-latency substrate

This role makes the kernel behave deterministically. There are two delivery mechanisms:

- **sysctl** values: live kernel knobs written to `/etc/sysctl.d/99-trade-ops.conf` and applied immediately.
- **GRUB kernel command line** (`/etc/default/grub` → `update-grub`): parameters that can only be set at boot, so they require a **reboot** to take effect.

**Boot-time CPU isolation (GRUB cmdline):**

- `isolcpus=2` — removes CPU core 2 from the kernel's normal load-balancer. The scheduler will not automatically place ordinary tasks on it, so it stays quiet and available for our latency-critical thread. (Explicit pinning with `taskset`/`CPUAffinity` can still place a chosen thread there — isolation stops *automatic* placement, not *deliberate* placement.)
- `nohz_full=2` — makes core 2 "tickless": the kernel stops sending the periodic ~1000/sec timer interrupt to that core, *as long as only one runnable task is on it*. Fewer interruptions = less jitter. (This is why we pin exactly one thread there.)
- `rcu_nocbs=2` — offloads "read-copy-update" callback processing off core 2 onto housekeeping cores, removing another source of background work.
- `transparent_hugepage=never` — disables Transparent Huge Pages. THP's background compaction can cause sudden latency spikes; trading systems prefer *explicit* hugepages instead (see below).

Together, the first three quiet a single core; the fourth removes a memory-management jitter source.

**Memory & scheduler sysctls:**

- `vm.nr_hugepages = 64` — pre-reserves 64 × 2 MB = 128 MB of "huge pages." Normal memory pages are 4 KB; huge pages are 2 MB, so the CPU's address-translation cache (the TLB) covers far more memory with fewer entries → fewer expensive TLB misses on large working sets. Reserving them at a fixed count avoids fragmentation later. **Reserved ≠ consumed:** explicit hugepages are opt-in — an app must request them via `mmap(MAP_HUGETLB)`, `SHM_HUGETLB`, or a `hugetlbfs` mount. A plain Python process never does, so `grep Huge /proc/meminfo` shows `HugePages_Free: 64` (the whole pool idle). This is intentional — the same "provision the production substrate, consumer out of scope" pattern as the governor and IRQ-pinning steps. Wiring a real consumer would mean a C++ engine or an explicit `MAP_HUGETLB` allocation. (Note this is the opposite of THP, which the kernel hands out *automatically* — and which we deliberately disabled below to avoid compaction stalls.)
- `vm.swappiness = 10` — strongly discourage swapping to disk (swapping a trading process is catastrophic for latency).
- `vm.stat_interval = 120` — update VM statistics less often, reducing periodic background work.
- `kernel.numa_balancing = 0` and `kernel.timer_migration = 0` (best-effort) — disable automatic NUMA page migration and timer migration, both of which can move work around unpredictably. Marked best-effort because they may not exist on every kernel.

**Network sysctls (low-latency):**

- `net.core.busy_poll = 50` and `net.core.busy_read = 50` — let sockets *busy-poll* the NIC for up to 50 µs instead of sleeping and waiting for an interrupt; trades a little CPU for lower receive latency.
- `net.core.rmem_max` / `net.core.wmem_max = 16 MB` — raise the maximum socket buffer sizes so bursts of market data aren't dropped.
- `net.core.netdev_max_backlog = 5000` — a deeper queue for incoming packets before the stack processes them.
- `net.ipv4.tcp_fastopen = 3` — enable TCP Fast Open for both client and server, shaving a round trip on connection setup.

**Two settings that are deliberately not active here, and why:**

- **CPU governor → performance.** On bare metal you'd force the CPU to its top clock and disable frequency scaling, so there's no "wake-up from low power" penalty. Inside VirtualBox the guest doesn't control CPU frequency, so `/sys/.../scaling_governor` doesn't exist. The role detects this and *prints a clear note that it's skipping — a bare-metal/BIOS step.* This honesty is a feature: it shows you know the difference.
- **irqbalance disabled.** `irqbalance` is a daemon that periodically moves hardware interrupt handling across cores; that can land a NIC interrupt on your isolated trading core and cause jitter. The role disables it *if present*. On this minimal server it wasn't installed, so the task safely skips.

**IRQ affinity pinning (best-effort, computed mask):**

Disabling irqbalance only *stops* interrupts being reshuffled; it doesn't *move* the existing device IRQs off the isolated core. To actually move them you write a CPU mask to each `/proc/irq/<N>/smp_affinity`. The role does this:

- It computes a **housekeeping mask** from facts — all cores except the isolated one. On the 4-vCPU box isolating core 2 that's cores {0, 1, 3} → 2⁰ + 2¹ + 2³ = 11 = `0xb`. The mask is built with a `range | difference | map('pow', 2) | sum` expression (deliberately *not* Jinja bit-shifts — Ansible's templating chokes on `<<`), so it follows `kernel_tuning_isolated_cpus` automatically and stays portable to bare metal. The inputs are guarded with `| default(...)` so the template still renders an integer during `--check`/lint, when facts may not be populated.
- It writes that mask to every IRQ whose `smp_affinity` is writable, capturing the value **before and after** so it only reports `changed` when something actually moved, and uses `failed_when: false` so a VM that rejects the write never fails the play — the same honest-skip pattern as the governor and irqbalance tasks.

A subtlety worth owning: `smp_affinity` is a *request*, not a command. The kernel applies the **intersection** of what you ask for and what each IRQ actually permits, then reports back the **effective** mask. On this VM the request `0xb` settled to `0xa` (cores 1 and 3) for the device IRQs — core 0 wasn't accepted for those lines, while the special timer/cascade IRQs 0 and 2 stay at `0xf` (all cores). The role reads the value back and reports the *effective* mask rather than assuming the write stuck. Either way every device interrupt now lands on a housekeeping core, off the isolated core 2 — which is the point. On bare metal with a multi-queue NIC the same code pins each RX/TX queue's IRQ to a chosen housekeeping core for real.

**Preemption-model detection (read-only knowledge demonstration):**

Installing a PREEMPT_RT kernel is a separate kernel build and a reboot into a new kernel — outside the scope of a role. Instead the role *detects and reports* the running preemption model (from `/sys/kernel/debug/sched/preempt`, falling back to parsing `/proc/version`). This is pure read-only reporting (`changed_when: false`, and `check_mode: false` so it runs even in `--check`), letting you speak to PREEMPT_RT vs PREEMPT_DYNAMIC from your own box's real output rather than theory. The next bare-metal step is to boot a PREEMPT_RT kernel or set `preempt=full` via PREEMPT_DYNAMIC.

> **The honest-demonstration triad is now four.** Governor, irqbalance, IRQ pinning, and preemption-model detection each prove you know the real bare-metal step while being straight about what the VM does and doesn't allow — exactly the nuance that separates "I understand what the kernel did" from "I copied some commands."

**Why the reboot is manual:** changing the GRUB command line needs a reboot to take effect, and because Ansible is running *on* the machine it's rebooting, it can't reboot itself mid-play. So the role notifies a "reboot required" message and you reboot by hand, then re-run. On a real remote fleet, the control node is separate, so `ansible.builtin.reboot` would reboot the target and wait for it to return.

**Proof it worked:** after reboot, `cat /proc/cmdline` shows `isolcpus=2 nohz_full=2 rcu_nocbs=2 transparent_hugepage=never`; `/sys/devices/system/cpu/isolated` shows `2`; `grep HugePages_Total /proc/meminfo` shows `64`; THP shows `[never]`. For the IRQ pin, `for i in /proc/irq/[0-9]*; do echo "$(basename $i): $(cat $i/smp_affinity)"; done` shows the device IRQs reading `a` (cores 1, 3) while the timer/cascade IRQs 0 and 2 stay at `f`.

### 5.2 `monitoring-agent` — Netdata

- **Why Netdata over Prometheus+Grafana?** With only 3 GB of RAM, Netdata's ~200 MB footprint and near-zero configuration beat the multi-gigabyte Prometheus+Grafana stack. Netdata auto-discovers system metrics at 1-second resolution and ships hundreds of pre-built alerts.
- **Why the static build?** The distro had no `netdata` package (bleeding-edge release), so we used Netdata's official installer with `--static-only`: a self-contained build under `/opt/netdata` that works on any distribution. Telemetry disabled, auto-updates off.
- **Binding:** we template `/opt/netdata/etc/netdata/netdata.conf` with `[web] bind to = 0.0.0.0:19999`. This matters because VirtualBox's NAT port-forward delivers to the guest's NAT interface, not loopback — so Netdata must listen on all interfaces, not just `127.0.0.1`, to be reachable from the host browser.
- **Scraping the engine:** we drop `/opt/netdata/etc/netdata/go.d/prometheus.conf` telling Netdata's Prometheus collector to scrape `http://127.0.0.1:8000/metrics` every second. That's how our custom tick-to-trade metrics appear on the dashboard.
- **Per-core CPU charts:** Netdata's `proc` plugin ships per-core utilization (`cpu.cpuN`) **off by default** in current versions (`per cpu core utilization = no`). The template enables it via a `[plugin:proc:/proc/stat]` section, so the dashboard exposes one chart per core (`cpu0`–`cpu3`) under **System → CPU**. This matters here specifically: it's what makes the isolated core (`cpu2`) visible as its own line, distinct from the housekeeping cores — the visual proof that `isolcpus=2` + `CPUAffinity=2` work end to end. Without the toggle you only get the aggregate `system.cpu` and the isolation is invisible.

### 5.3 `trading-app-deploy` — the FIX workload

- **Python virtualenv** at `/opt/trade-ops/venv` keeps our dependencies (`simplefix`, `prometheus-client`) isolated from system Python — standard professional practice.
- **Two files** deployed to `/opt/trade-ops/app/`: `mock_exchange.py` and `trading_engine.py` (explained fully in Section 7).
- **Two systemd units:** `mock-exchange.service` and `trading-engine.service`. Both run as the unprivileged `trader` user with `Restart=on-failure` and `RestartSec=2` (self-heal). The engine adds **`CPUAffinity=2`**, pinning it to the isolated core — this is the moment the kernel-tuning work and the workload connect end to end. (systemd, running as root, sets the affinity before dropping to `trader`, so the app needs no special privilege.)
- **Netdata scrape config** is written here too (it depends on the engine's metrics port), notifying a Netdata restart.

**Proof it worked:** `systemctl is-active mock-exchange trading-engine` → `active`; `taskset -cp $(pgrep -f trading_engine.py)` → affinity list `2`; `curl localhost:8000/metrics` shows the engine metrics climbing.

### 5.4 `bod-checks` — readiness gate

- **Config-file pattern:** the script is static (`/usr/local/bin/bod_check.sh`); its tunables (thresholds, service list, exchange host/port) come from a templated `/etc/trade-ops/bod.conf`. Keeping config separate from code is clean and is how real ops scripts are written.
- **What it checks:** disk usage, memory, **clock offset via chrony**, CPU isolation present in `/proc/cmdline`, hugepages reserved, THP disabled, CPU governor, NIC link state, key **systemd services active** (`ssh`, `netdata`, `mock-exchange`, `trading-engine`), and **connectivity to the exchange gateway** (`nc` to `127.0.0.1:9001`).
- **Exit codes drive alerting:** `0` = READY (all pass), `1` = READY WITH WARNINGS, `2` = NOT READY (a real failure). The systemd service sets `SuccessExitStatus=1`, so "warnings" don't mark the unit failed, but a genuine "not ready" (exit 2) does — giving monitoring a clean signal to distinguish "fine to open" from "do not open."
- **The timer:** `bod-check.timer` is set to `Mon..Fri 08:45` in the `Asia/Kolkata` timezone — roughly 30 minutes before the 09:15 IST market open. The role also sets the system timezone and ensures `chrony` (time sync) is running, tying directly to trading clock-sync requirements.

---

## 6. systemd from scratch (and our units)

**systemd** is the init system that starts, stops, supervises, and schedules everything on a modern Linux box. The pieces we use:

- **Unit:** any managed object. We use **service** units (long-running or one-shot programs) and **timer** units (schedulers, like cron but integrated).
- **`Type=simple`** (our FIX services): the process runs in the foreground and systemd considers it started immediately. **`Type=oneshot`** (BOD service): runs to completion once, then exits.
- **`Restart=on-failure` + `RestartSec=2`:** if the process exits abnormally (crash or killed by signal), systemd restarts it after 2 seconds. This is our high-availability/self-heal mechanism. A *clean* `systemctl stop` is **not** a failure, so it does not trigger a restart — which is why we can use `stop` to demonstrate the BOD "NOT READY" path.
- **`CPUAffinity=2`:** pins the service's processes to CPU core 2 (the isolated core).
- **`User=trader`:** drops privileges; the trading apps don't run as root.
- **`SuccessExitStatus=1`:** treat exit code 1 as success (our "warnings" case).
- **`enable` vs `start`:** *enable* means "start at boot"; *start* means "start now." We do both.
- **`daemon-reload`:** tells systemd to re-read unit files after we change them.
- **`journalctl -u <unit>`:** reads a unit's logs. `systemctl list-timers` shows when timers will next fire.

Our units: `mock-exchange.service` (acceptor), `trading-engine.service` (initiator, pinned, depends on the exchange via `After=`/`Requires=`), `bod-check.service` (oneshot) triggered by `bod-check.timer`.

---

## 7. FIX protocol from scratch (and our two scripts)

### What FIX is

**FIX (Financial Information eXchange)** is the lingua franca of electronic trading: a text protocol where each message is a list of `tag=value` pairs separated by a special byte (SOH, ASCII 0x01). Both sides hold a long-lived TCP **session**; they log on, exchange business messages (orders, executions), send heartbeats to prove the link is alive, and log off.

A message looks like (using `|` to show the invisible SOH separator):

```
8=FIX.4.2|9=...|35=D|49=TRADER|56=EXCHANGE|34=12|52=20260531-10:00:00.123|11=42|55=NIFTY|54=1|38=50|40=2|44=120.5|10=...|
```

### The tags we use

**Header (every message):**
- `8` BeginString — protocol version, `FIX.4.2`
- `9` BodyLength — message length (computed automatically by the library)
- `35` MsgType — what kind of message
- `49` SenderCompID / `56` TargetCompID — who's sending / receiving
- `34` MsgSeqNum — sequence number
- `52` SendingTime — UTC timestamp

**Message types (`35`):**
- `A` Logon — establishes the session; carries `98` EncryptMethod (0 = none) and `108` HeartBtInt (heartbeat interval, 30 s)
- `0` Heartbeat / `1` TestRequest — liveness
- `D` NewOrderSingle — a new order, with `11` ClOrdID (client order id), `55` Symbol, `54` Side (1=Buy, 2=Sell), `38` OrderQty, `40` OrdType (2=Limit), `44` Price
- `8` ExecutionReport — the exchange's response, with `37` OrderID, `11` ClOrdID (echoed), `17` ExecID, `150` ExecType (2=Fill), `39` OrdStatus (2=Filled), plus `32` LastQty / `31` LastPx

**Trailer:**
- `10` CheckSum — integrity check (computed automatically)

We use the `simplefix` library, which builds and parses these messages and computes `9` (BodyLength) and `10` (CheckSum) for us automatically on encode.

### `mock_exchange.py` — the FIX acceptor (the "exchange")

Conceptually:
1. Open a TCP socket, set `TCP_NODELAY` (disable Nagle's algorithm so small messages send immediately — important for latency), listen on `:9001`.
2. Accept one client (the engine).
3. Loop: read bytes, feed them to a `simplefix` parser, pull out complete messages.
4. If it's a **Logon (A)** → reply with a Logon.
5. If it's a **NewOrderSingle (D)** → build and send a filled **ExecutionReport (8)**, echoing the client order id and reporting it as fully filled.
6. If it's a **TestRequest (1)** → reply with a Heartbeat.

It is intentionally minimal — it does not enforce sequence numbers or do risk checks; it exists to give the engine something realistic to talk to so we can measure round-trip latency.

### `trading_engine.py` — the FIX initiator (the "engine") + metrics

Conceptually:
1. Start a Prometheus metrics HTTP server on `:8000` (this is what Netdata scrapes).
2. Connect to the exchange (`127.0.0.1:9001`), `TCP_NODELAY` on.
3. Send **Logon (A)**, wait for the Logon acknowledgement.
4. Loop forever over ~300 hardcoded signals (symbol, side, qty, price), one every ~100 ms:
   - Record a high-resolution start time `t0` (`time.perf_counter()`).
   - Build and send a **NewOrderSingle (D)**; increment the `orders` counter.
   - Wait to receive the **ExecutionReport (8)**; record `t1`.
   - The **tick-to-trade** sample is `t1 − t0`: observe it into the histogram, set the gauge (in µs), increment the `fills` counter.

If the connection drops (e.g., the exchange is killed), the engine raises and exits — and systemd restarts it, which re-logs on. That's the self-heal behavior you demo in Section 11.

### Why not ITCH/OUCH?

ITCH (market data) and OUCH (order entry) are **Nasdaq** binary protocols — not used by NSE/BSE. We used generic FIX because it's vendor-neutral and the concepts (sessions, sequence numbers, orders, executions) transfer everywhere. For India accuracy: NSE's native order interface is **NNF** (Non-NEAT Front-end), which now mandates **encrypted** connections (TLS 1.3 + AES-256), and market data is delivered via **TBT/MTBT** (tick-by-tick) multicast feeds. A generic FIX proof-of-concept is sufficient to show you understand the stack end to end.

---

## 8. Tick-to-trade and the metrics

**Tick-to-trade (T2T)** is *the* headline latency metric in trading: the time from a market input ("tick") to your resulting order hitting the wire ("trade"). In this project we measure a close analogue: from the moment the engine acts on a signal to the moment it receives the execution report — a full round trip through the mock exchange.

We expose four metrics from the engine on `:8000/metrics` in Prometheus text format:

- **`engine_tick_to_trade_seconds` (Histogram)** — the *correct* way to track latency. A histogram sorts each sample into buckets (50 µs, 100 µs, 250 µs, …). From the bucket counts you can compute percentiles like p50/p95/**p99** — the tail latency that actually matters in trading. (In PromQL: `histogram_quantile(0.99, ...)`.)
- **`engine_t2t_microseconds` (Gauge)** — the most recent sample in microseconds. A gauge is a single value that goes up and down; this gives a clean, obvious line on the dashboard that spikes when you inject latency.
- **`engine_orders_total` (Counter)** — total orders sent; a counter only goes up, and its *rate* gives orders/second.
- **`engine_fills_total` (Counter)** — total execution reports received.

**Why both a histogram and a gauge?** The histogram is what a real system uses (you care about p99, not the last value). The gauge is for *demo legibility* — it makes the latency injection visually unmistakable. Knowing the difference (and why histograms are right for latency) is a key concept — see the FAQ.

**Important: there is no Prometheus *server* here.** "Prometheus" names two distinct things, and this project uses only one of them. (1) The **Prometheus exposition format** plus the **`prometheus-client` library** — `start_http_server(8000)` in the engine publishes the metrics above as plain text at `:8000/metrics`. That's the *producer*. (2) **Netdata's built-in Prometheus collector** (configured by the `go.d/prometheus.conf` from the `trading-app-deploy` role) reads that endpoint every second — that's the *consumer/scraper* that a standalone Prometheus database would otherwise play. We deliberately did *not* run Prometheus-the-server (it's the heavyweight stack we skipped for RAM). Because the format is the standard, the same `/metrics` endpoint plugs into a real Prometheus + Grafana stack with zero code change — that portability is the point. Verify Netdata is actually ingesting it: `curl -s "http://127.0.0.1:19999/api/v1/allmetrics?format=prometheus" | grep tick_to_trade`.

Baseline observed in this build: ~1,175 µs round trip on loopback inside the VM. Under injected latency (Section 11) it jumps to ~11 ms.

---

## 9. Monitoring: Netdata and what's on the dashboard

Netdata does three things: **collects** metrics (from the kernel via `/proc` and `/sys`, and by scraping our engine's `:8000`), **stores** them in its `dbengine` time-series database, and **serves** a live dashboard on `:19999`.

What's displayed and why it matters for trading:

- **Per-core CPU utilization** — you can see the isolated core (cpu2) behaving differently from the housekeeping cores; under load it should stay dedicated to the engine.
- **Context switches & interrupts** — spikes here mean the CPU is being pulled away from the trading thread; low and steady is the goal of all the isolation work.
- **Network throughput, packets, errors/drops** — drops or errors on the trading path are latency and correctness risks.
- **Memory** — confirms no swapping (catastrophic for latency) and that hugepages are reserved.
- **Disk I/O** — generally off the hot path, but watched for log/storage health.
- **Custom trading metrics** (Prometheus → trading_engine section): `engine_t2t_microseconds` (the latency line), `engine_orders_total` rate (orders/sec), and the `engine_tick_to_trade_seconds` histogram buckets.

---

## 10. The complete data-flow map (who sends what, where)

This is the section to be able to recite. Each row is: **who** sends **what**, to **where**, over **what**.

| # | From | To | Channel / Port | What flows |
|---|---|---|---|---|
| 1 | Ansible control node | trading node OS | SSH (22; host-forward 2222) | Config changes, package installs, file/template deploys, service control |
| 2 | Ansible control node | GitHub | SSH (git) | Push of the Infrastructure-as-Code repo |
| 3 | Trading engine | Mock exchange | TCP 9001 (loopback `lo`) | FIX Logon, NewOrderSingle (35=D), Heartbeats |
| 4 | Mock exchange | Trading engine | TCP 9001 (loopback `lo`) | FIX Logon ack, ExecutionReport (35=8) |
| 5 | Trading engine | (exposes) | HTTP 8000 (`0.0.0.0`) | Prometheus text: T2T histogram, gauge, counters |
| 6 | Netdata agent | Trading engine | HTTP 8000 (loopback) | Scrape request (pulls the metrics in row 5) every 1 s |
| 7 | Linux kernel | Netdata agent | `/proc`, `/sys` (in-kernel) | System metrics: CPU, memory, network, interrupts |
| 8 | Netdata agent | dbengine TSDB | local disk | Writes/reads time series |
| 9 | Netdata agent | (serves) | HTTP 19999 (`0.0.0.0`) | The dashboard UI + chart data |
| 10 | Operator browser | Netdata | host `127.0.0.1:19999` → NAT forward → guest 19999 | Dashboard views |
| 11 | BOD check script | local system | `df`, `free`, `chronyc`, `/proc`, `/sys`, `systemctl`, `nc :9001` | Reads health; emits PASS/WARN/FAIL and an exit code |
| 12 | BOD timer | BOD service | systemd | Triggers the check pre-open (Mon–Fri 08:45 IST) |

Key mental model: **the trading hot path (rows 3–4) is loopback TCP**, which is why injecting `netem` delay on the `lo` interface (Section 11) inflates tick-to-trade — and why it leaves your SSH session (which rides the NAT interface) untouched.

---

## 11. The two scenarios, in depth

### Scenario A — latency injection (`tc/netem`)

**What the tools are:** `tc` (traffic control) configures **qdiscs** (queueing disciplines) on a network interface. `netem` (network emulator) is a qdisc that can add delay, jitter, loss, and reordering — exactly the impairments you'd see on a real network path.

**What we do:** `scripts/latency_demo.sh on` runs `tc qdisc replace dev lo root netem delay 5ms 1ms distribution normal`. Because the FIX round trip is loopback, *each direction* gets ~5 ms added, so the round trip gains ~10 ms.

**What you observe:** `engine_t2t_microseconds` on the dashboard jumps from ~1.2 ms to ~11 ms, and the histogram's mass shifts into the higher buckets. `off` removes the qdisc and it returns to baseline.

**What it proves:** the JD line "troubleshoot network and system latency, identify bottlenecks." You can inject a known impairment and *see it on your own instrumentation* — demonstrating both the monitoring and the diagnostic skill. (Honest note: netem on `lo` also slightly delays Netdata's scrape; harmless at 1-second intervals.)

### Scenario B — failover / self-heal

**Self-heal (HA):** `sudo systemctl kill -s SIGKILL trading-engine` kills the process abnormally. Because the unit has `Restart=on-failure` + `RestartSec=2`, systemd restarts it within ~2 seconds; `systemctl show trading-engine -p NRestarts` increments, and the engine re-logs on automatically. This is the "high availability / minimal downtime" line.

**Readiness gate:** `sudo systemctl stop trading-engine` stops it *cleanly* (not a failure), so systemd leaves it down. Running `bod_check.sh` now reports `trading-engine` as **FAIL** and returns **exit 2 → NOT READY**. This shows the BOD gate would block a market open if something is genuinely broken — and ties back to the `SuccessExitStatus=1` design that lets monitoring distinguish "warnings" from "down."

---

## 12. Directory structure, annotated

```
trade-ops-ansible/
├── ansible.cfg                       # Defaults: inventory, roles_path, become=sudo, YAML output
├── inventory/
│   └── hosts.yml                     # Group 'trading_nodes' -> host 'trading-node' over SSH
├── playbooks/
│   └── site.yml                      # Runs roles in order: kernel-tuning, monitoring-agent,
│                                     #   trading-app-deploy, bod-checks
├── roles/
│   ├── kernel-tuning/
│   │   ├── defaults/main.yml         # isolated CPU(s), hugepage count, sysctl dicts, cmdline params
│   │   ├── handlers/main.yml         # 'Update grub', 'Reboot required'
│   │   └── tasks/main.yml            # sysctls, GRUB cmdline, governor check, irqbalance,
│   │                                 #   IRQ-affinity pin, preempt-model detect, verify
│   ├── monitoring-agent/
│   │   ├── defaults/main.yml         # netdata bind address, conf path
│   │   ├── handlers/main.yml         # 'Restart netdata'
│   │   ├── templates/netdata.conf.j2 # [web] bind to 0.0.0.0:19999
│   │   └── tasks/main.yml            # kickstart static install, bind config, ensure running
│   ├── trading-app-deploy/
│   │   ├── defaults/main.yml         # app dir, venv, user, metrics/exchange ports, engine CPU
│   │   ├── handlers/main.yml         # 'Reload systemd', 'Restart mock exchange/trading engine'
│   │   ├── files/mock_exchange.py    # FIX acceptor
│   │   ├── files/trading_engine.py   # FIX initiator + Prometheus metrics
│   │   ├── templates/mock-exchange.service.j2
│   │   ├── templates/trading-engine.service.j2   # includes CPUAffinity
│   │   └── tasks/main.yml            # venv, pip, deploy, units, netdata scrape, enable/start
│   └── bod-checks/
│       ├── defaults/main.yml         # timezone, schedule, thresholds, service list, exchange host/port
│       ├── handlers/main.yml         # 'Reload systemd'
│       ├── files/bod_check.sh        # the readiness script (PASS/WARN/FAIL, exit 0/1/2)
│       ├── templates/bod.conf.j2     # config consumed by the script
│       ├── templates/bod-check.service.j2   # oneshot, SuccessExitStatus=1
│       └── templates/bod-check.timer.j2     # Mon..Fri 08:45 (IST)
├── scripts/
│   └── latency_demo.sh               # tc/netem on|off|status
├── README.md
└── ProjectDeepDive.md               # this document
```

---

## 13. Mapping to the job description

| JD responsibility | Concrete evidence in this project |
|---|---|
| Low-Latency Infrastructure Management | `kernel-tuning` (CPU isolation, hugepages, THP off, net sysctls); engine pinned to isolated core |
| Automation with Ansible | Four roles, inventory, handlers, idempotent runs, `--check --diff`, `ansible-lint` clean at production |
| Python & Bash scripting | `trading_engine.py`, `mock_exchange.py` (Python); `bod_check.sh`, `latency_demo.sh` (Bash) |
| Linux Kernel Parameter Tuning | sysctls + GRUB cmdline for CPU scheduling, memory, network throughput |
| Daily / Beginning-of-Day checks before open | `bod-checks` role + `bod-check.timer` (pre-open IST) |
| Monitor trading systems & raise alerts | Netdata dashboard + custom T2T metrics; BOD exit codes as a health signal |
| Troubleshoot network/system latency, find bottlenecks | `tc/netem` injection demo observed on the T2T dashboard |
| High availability, minimal downtime | `Restart=on-failure` self-heal demo |
| Coordinate with trading/tech teams | The repo + README + this doc are the artifacts you'd hand a team |

---

## 14. FAQ (frequently asked questions)

These are the questions worth being able to answer cold — each gets at whether the project does what it claims. They double as a self-test: cover the answer and try to reconstruct it.

**What is this project, in one paragraph?**
> A miniature trade-ops environment that mirrors the daily job: an Ansible-managed Ubuntu trading node with low-latency kernel tuning; a simulated FIX order flow with tick-to-trade instrumentation; a live Netdata dashboard; and Beginning-of-Day checks that gate the market open. It includes two live demos — injecting network latency and watching tick-to-trade spike, and killing a service to show systemd self-heal. It runs in a VM, so latency numbers are a methodology demonstration, not production figures.

**Why these kernel parameters?**
> `isolcpus` removes a core from the scheduler's load-balancer; `nohz_full` stops the timer tick on it (when only one task runs there); `rcu_nocbs` offloads RCU callbacks — together they quiet a core for the trading thread. THP off avoids compaction jitter; explicit hugepages cut TLB misses; `busy_poll`/`busy_read` reduce receive latency; raised socket buffers absorb bursts. On bare metal you'd add the performance governor and disable C-states/turbo in BIOS for frequency stability.

**What actually runs on the isolated core (CPU 2)?**
> Only the trading engine, placed there explicitly by `CPUAffinity=2` in its systemd unit. Nothing else of ours is pinned there. The important nuance: `isolcpus` is not a hard partition — it stops the scheduler from *automatically* placing ordinary tasks on core 2, but the core still does a small amount of unavoidable kernel work: the residual ~1/sec timer tick that `nohz_full` can't remove, per-CPU kernel threads (kworkers, migration/stopper threads, ksoftirqd, RCU quiescent-state reporting), and occasional IPIs (TLB shootdowns, scheduler events). You can see what is *schedulable* there with `ps -eo pid,psr,comm | awk '$2==2'` and confirm the engine's pin with `taskset -cp $(pgrep -f trading_engine.py)`. Pushing closer to a truly silent core needs `cpuset` cgroups, IRQ pinning, and a PREEMPT_RT/tickless kernel — bare-metal territory. On top of all that, in a VM the "isolated" core is a vCPU the host schedules onto shared silicon, so isolation is real *to the guest scheduler* only.

**Where do the mock exchange and Netdata run?**
> On the housekeeping cores (0, 1, 3), placed automatically by the normal scheduler — which is exactly the point of isolating core 2. Neither has a `CPUAffinity` set. Keeping the mock exchange off core 2 matters: if both ends of the FIX round trip shared the isolated core they would contend with each other. Confirm with `taskset -cp $(pgrep -f mock_exchange.py)` (not pinned to 2).

**Are the 64 reserved hugepages actually used?**
> No — they are *reserved but not consumed*, and that is an honestly-scoped gap. `vm.nr_hugepages=64` locks a 128 MB pool of 2 MB pages away from normal allocation; `grep Huge /proc/meminfo` shows `HugePages_Free: 64` (equal to Total = nobody is using them). Explicit hugepages are **opt-in**: an app must request them via `mmap(MAP_HUGETLB)`, `SHM_HUGETLB`, or a `hugetlbfs` mount. A plain Python process (or any `malloc`) never touches the pool. This is deliberately the same "demonstrate the production substrate, consumer out of scope" pattern as the governor/IRQ-pinning steps: pre-allocating a pool at boot (to avoid fragmentation) and disabling THP (to avoid compaction stalls) is the real pattern; wiring a consumer would mean a C++ engine or `mmap(MAP_HUGETLB)`. Why it matters: one 2 MB page covers the span of 512 × 4 KB pages with a single TLB entry, so a large working set (order books, market-data buffers) gets far better TLB coverage and fewer page-walk stalls. See [§5.1](#51-kernel-tuning--the-low-latency-substrate).

**Is there a Prometheus server in this project? Where is Prometheus configured?**
> No Prometheus server and no Prometheus database. The project uses two things that share the name: (1) the **Prometheus text exposition format** plus the **`prometheus-client` Python library**, which the engine uses — `start_http_server(8000)` publishes the metrics in that format at `:8000/metrics` (the *producer*); and (2) **Netdata's built-in Prometheus collector**, configured by the `go.d/prometheus.conf` we deploy in `trading-app-deploy`, which scrapes `:8000/metrics` every second (the *consumer/scraper*). Netdata stores the series in its own dbengine TSDB and renders them. So the chain is: engine exposes Prometheus-format metrics → Netdata scrapes → Netdata TSDB → dashboard. The format is the standard, so the same `/metrics` endpoint plugs straight into a real Prometheus+Grafana stack with no code change. See [§8](#8-tick-to-trade-and-the-metrics).

**How would this differ on real colocation bare metal?**
> Pinned physical cores, BIOS C-states/turbo/hyper-threading decisions, a PREEMPT_RT or tickless kernel, real NIC interrupt affinity with irqbalance disabled, possibly kernel-bypass NICs (Solarflare/Onload, DPDK), NUMA-aware placement, and PTP-grade time sync. The Ansible automation and the parameter choices carry over unchanged; only the substrate and the achievable numbers change.

**PREEMPT_RT vs PREEMPT_DYNAMIC?**
> PREEMPT_RT is a kernel build that makes almost all kernel code preemptible (sleeping spinlocks, threaded IRQs) for the lowest latency, at some throughput cost. PREEMPT_DYNAMIC lets you choose none/voluntary/full preemption at boot via `preempt=` without recompiling — but it can't select RT. So DYNAMIC is flexibility across the non-RT models; RT is a different, fully-preemptible build. The role detects and reports the running model rather than installing RT.

**How does irqbalance hurt low latency?**
> It periodically migrates interrupt affinity across CPUs. That can land a NIC interrupt on your isolated trading core, polluting its cache and adding jitter. You disable it and pin IRQs to housekeeping cores instead. Disabling irqbalance only *stops the reshuffling*, though — it doesn't move the existing IRQs. For that you write a CPU mask to each `/proc/irq/<N>/smp_affinity`, which the kernel-tuning role does.

**Which cores are IRQs pinned to, and how is the mask built?**
> A housekeeping mask is computed from the CPU count minus the isolated core — on the 4-vCPU box isolating core 2 that's cores 0, 1, 3, or `0xb`. It is built arithmetically (`range | difference | map('pow',2) | sum`) rather than with bit-shifts, because Ansible's Jinja rejects `<<`, and the inputs are guarded with defaults so it still renders in `--check` mode. That value is written to each writable IRQ's `smp_affinity`. The key nuance: `smp_affinity` is a *request* — the kernel applies the intersection of what was asked and what each IRQ permits, then reports back the *effective* mask. On this VM `0xb` settled to `0xa` (cores 1 and 3) for the device IRQs because core 0 wasn't accepted for those lines; the timer/cascade IRQs 0 and 2 stay on all cores (`0xf`). The role reads back and reports the effective value rather than assuming the write stuck — and either way every device interrupt is off the isolated core 2. On bare metal with a multi-queue NIC the same code pins each queue's IRQ for real.

**What about NUMA?**
> On a multi-socket box you keep the trading thread, its memory, and the NIC's interrupts on the same NUMA node to avoid slow cross-node memory access; you'd use `numactl` and disable automatic NUMA balancing. This laptop is single-socket so it can only be described, but the sysctl `kernel.numa_balancing=0` is in the role.

**Kernel bypass — DPDK vs the kernel stack?**
> DPDK polls the NIC in user space, bypassing the kernel network stack — roughly single-digit microseconds versus tens for the kernel path — but it burns a whole core polling, needs hugepages, core pinning, and a supported NIC, and you lose the kernel's mature tooling and TCP stack. It's worth it only when the NIC is the binding constraint. Deliberately out of scope here.

**What is tick-to-trade and how is it measured?**
> The time from a market input to your order on the wire. The engine measures a round-trip analogue: from when it acts on a signal to when it gets the execution report, using a monotonic clock (`time.perf_counter()`), recorded as a Prometheus histogram so percentiles (p99) can be read, plus a gauge for the latest sample on the dashboard.

**Why a histogram for latency, not an average?**
> Averages hide the tail. In trading the p99/p99.9 is what hurts you. A histogram preserves the distribution so you can compute and alert on percentiles.

**How do I read the four trading_engine charts on the dashboard?**
> 1) `engine_orders_total` is a counter charted as a *rate* (~9 orders/s) — the slope is what matters, not the cumulative value; ~9 rather than 10 because each cycle is the 100 ms sleep *plus* the round-trip wait. 2) `engine_t2t_microseconds` is the gauge — the most-recent T2T, ~0.9 ms baseline on loopback, with transient spikes to several ms that are host-scheduler jitter (or an occasional Python GC pause). 3) `engine_tick_to_trade_seconds` is the histogram as a heatmap — the y-axis is the bucket boundaries, colour is observations/s; the bright band sitting in the 1–2.5 ms buckets is the median, faint cells in higher buckets are the tail (your p99). 4) `engine_tick_to_trade_seconds_count` is the histogram's observation count as a rate, which should track the order rate one-to-one — if it dipped below, orders would be going out without fills coming back. The `python_gc` series (auto-exposed by `prometheus-client`) is worth knowing: a GC pause is one plausible cause of a tail spike — itself an argument for why production engines are C++ with no GC.

**What does the BOD check verify, and what happens if something fails?**
> Disk, memory, clock sync via chrony, CPU isolation, hugepages, THP, governor, NIC link, key services active, and exchange connectivity. It exits 0/1/2 for ready/warnings/not-ready. The systemd service uses `SuccessExitStatus=1` so warnings don't flag the unit, but a real failure (exit 2) does — so alerting can block a market open.

**India-specific: clock sync?**
> SEBI's algo-trading framework (circular CIR/MRD/DP/09/2012) requires the *exchange* to keep its clock synced to within ~1 microsecond precision and ±1 millisecond accuracy of an atomic reference before the open; members maintain audit trails and unique exchange order IDs. NSE colocation provides both NTP and PTP time services. The BOD clock check and chrony usage reflect why time sync is treated as a hard requirement. See also the clock-skew note in [§15](#15-honest-limitations--how-bare-metal-differs).

**Why FIX and not ITCH/OUCH?**
> ITCH/OUCH are Nasdaq protocols, not NSE/BSE. This project uses generic FIX because the concepts transfer and it shows end-to-end stack understanding. NSE's real order interface is NNF (now encryption-mandated), with TBT/MTBT for market data.

**The control node manages itself — isn't that artificial?**
> The control and managed node are co-located only to fit in 3 GB of RAM. The roles, inventory, SSH transport, and idempotency are identical to managing remote colo servers; in production you'd just point the inventory at the colo hosts. Nothing about the automation changes.

---

## 15. Honest limitations & how bare metal differs

- Latency figures are VM methodology demonstrations, not production numbers (host jitter, shared vCPUs).
- CPU governor / C-states / turbo / hyper-threading are BIOS/bare-metal controls not exposed in the VM.
- IRQ pinning is best-effort: `smp_affinity` is a request the kernel can override, and a VM's virtual interrupt controller may strip cores or ignore the write entirely. The role reports the *effective* mask it reads back, not the requested one.
- PREEMPT_RT is not installed — that's a separate kernel build/reboot, out of scope. The role only detects and reports the running preemption model.
- `nohz_full` can't fully eliminate the tick (≈1/sec remains) and is incompatible with the `intel_pstate` driver — on bare metal you fix the frequency instead.
- No kernel bypass (DPDK/XDP) — deliberately out of scope; the kernel network stack is in the path.
- Single VM, single socket — no real NUMA, no redundant network paths, no real exchange link.
- The mock exchange does not enforce FIX sequence numbers or do risk checks — it exists to exercise the round trip.

What a production version adds: bare-metal with pinned cores and BIOS tuning; PREEMPT_RT/tickless kernel; real NIC IRQ affinity; possibly kernel bypass; NUMA-aware placement; PTP time sync; redundant paths and sub-millisecond failover; a hardened, validated FIX engine.

### Operational note: VM clock skew after suspend/resume

A VirtualBox VM that is **saved/suspended and resumed later** comes back with its guest clock frozen at the moment of suspend — so it runs *behind* real (host) wall-clock time by however long it was paused. This produces two confusing symptoms that look like failures but aren't:

1. **The dashboard appears to "stop."** Netdata stamps samples in *guest* time, but the browser's "now" follows *host* time, so everything between the last guest sample and host-now looks like an empty flatline gap. Data is actually flowing fine.
2. **The BOD timer "doesn't fire."** The `Mon..Fri 08:45` `OnCalendar` is evaluated in guest time; if the guest clock hasn't reached that moment yet, the timer is simply still pending (`systemctl list-timers` shows it armed with hours "left").

**Fix:** force chrony to step the clock to real time — `sudo chronyc makestep` — then confirm with `date`. Because the BOD timer has `Persistent=true`, the moment the clock jumps past a missed `08:45`, systemd fires the catch-up run automatically. **Prevent recurrence:** install VirtualBox Guest Additions (its time-sync re-disciplines the clock on resume), or run `chronyc makestep` after resuming. For a clean live demo, don't save-state the VM beforehand, and give the engine ~10 minutes of run time so the chart window is gap-free.

---

## 16. Glossary

- **Determinism** — consistent, bounded response time regardless of load. The goal of all the tuning. Jitter is its enemy.
- **Jitter** — variation in latency; spikes in response time.
- **isolcpus / nohz_full / rcu_nocbs** — boot parameters that quiet a CPU core: remove it from load-balancing, stop its timer tick, and offload RCU callbacks.
- **RCU (read-copy-update)** — a kernel synchronization mechanism whose deferred callbacks we offload off the isolated core.
- **Hugepages / TLB** — 2 MB (vs 4 KB) memory pages; the TLB is the CPU's address-translation cache, and hugepages reduce expensive TLB misses.
- **THP (Transparent Huge Pages)** — automatic hugepages whose background compaction causes jitter; disabled in favor of explicit hugepages.
- **MAP_HUGETLB / hugetlbfs** — the explicit, opt-in ways an app consumes the reserved hugepage pool; without one of them a reserved pool stays idle (`HugePages_Free` unchanged).
- **IRQ** — hardware interrupt; the CPU stops to service one. We keep them off the trading core.
- **smp_affinity** — per-IRQ CPU mask under `/proc/irq/<N>/`; a *request* the kernel intersects with what's allowed, returning an *effective* mask. We write the housekeeping mask to steer device IRQs off the isolated core.
- **Housekeeping cores** — the non-isolated cores that absorb IRQs, RCU callbacks, and kernel threads so the isolated core stays quiet.
- **PREEMPT_DYNAMIC** — kernel feature to choose none/voluntary/full preemption at boot via `preempt=`, without recompiling; distinct from the fully-preemptible PREEMPT_RT build.
- **sysctl** — runtime kernel tunables (e.g., `vm.swappiness`).
- **GRUB cmdline** — boot-time kernel parameters; require a reboot.
- **governor / C-states / P-states** — CPU power/frequency management; deep sleep states add wake-up latency.
- **FIX** — text-based trading protocol of `tag=value` messages over a TCP session.
- **Tick-to-trade (T2T)** — latency from market input to order on the wire; the headline trading metric.
- **NewOrderSingle (35=D) / ExecutionReport (35=8)** — the order and its fill confirmation in FIX.
- **Prometheus metric types** — Counter (only increases), Gauge (up/down), Histogram (bucketed distribution for percentiles).
- **Prometheus exposition format vs Prometheus server** — the format is a plain-text convention for publishing metrics over HTTP (used here via `prometheus-client`); the *server* is a separate scraper+TSDB product (not run here — Netdata's collector scrapes the endpoint instead).
- **chronyc makestep** — forces an immediate one-shot correction of the system clock to NTP time; the fix for large VM clock skew after suspend/resume.
- **qdisc / netem** — Linux traffic-control queueing discipline / network emulator used to inject delay.
- **systemd unit / service / timer** — the init system's managed objects; services run programs, timers schedule them.
- **idempotency** — re-running automation yields no further changes; safe to repeat.
- **Ansible role / playbook / handler / inventory** — reusable task bundle / ordered plays / change-triggered tasks / host list.
- **chrony / NTP / PTP** — time-synchronization daemon / protocols; PTP is sub-microsecond, used in colo.
- **NNF / TBT / MTBT** — NSE's native order-entry front-end / tick-by-tick market data / multicast TBT feed.
- **PREEMPT_RT** — real-time kernel patch making nearly all kernel code preemptible for low latency.
- **DPDK / kernel bypass** — user-space packet I/O that skips the kernel network stack for lower latency.