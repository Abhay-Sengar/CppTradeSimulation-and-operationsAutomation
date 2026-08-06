# Operator Runbook

How to run, observe, demo, and recover the trading stack on the bare-metal node.
Command-first. For *why* any of it works, see
[`ProjectDeepDive.md`](ProjectDeepDive.md).

> **Sudo note:** privileged commands must be run in a *real terminal* (they
> prompt for your password). Everything read-only works without sudo.

---

## 0. Quick reference — what's deployed and where

| Thing | Value |
|---|---|
| Dashboard (Netdata) | `http://127.0.0.1:19999` |
| Engine metrics | `http://127.0.0.1:8000/metrics` |
| Mock exchange (FIX) | `127.0.0.1:9001` (loopback only) |
| Binaries | `/opt/trade-ops/app/bin/{trading_engine,mock_exchange}` |
| C++ source (built on node) | `/opt/trade-ops/src/` |
| Engine log | `/tmp/trading-engine.log` |
| BOD script / config | `/usr/local/bin/bod_check.sh` · `/etc/trade-ops/bod.conf` |
| systemd services | `mock-exchange` · `trading-engine` · `netdata` · `trade-ops-cpu` |
| systemd timer | `bod-check.timer` (Mon–Fri 08:45 IST) |
| Isolated cores | `8` md · `9` strategy · `10` gateway · `11` exchange |
| Repo | `/home/cdot/Desktop/HFT/CppTradeSimulation-and-operationsAutomation` |

---

## 1. Reach the dashboard & metrics

```bash
xdg-open http://127.0.0.1:19999            # Netdata dashboard (or open in a browser)
curl -s localhost:8000/metrics             # engine metrics (Prometheus text)
# just the latency lines:
curl -s localhost:8000/metrics | grep -E "_t2t_|_rtt_|orders_total|fills_total"
# live-updating view (great for a demo):
watch -n 0.5 'curl -s localhost:8000/metrics | grep -E "_t2t_p50_ns|_t2t_p99_ns|_rtt_p50_ns|orders_total"'
```

> Metric names are `engine_t2t_*` (engine tick-to-trade) and `engine_rtt_*`
> (round trip). **If you still see `engine_pipeline_*` / `engine_tick_to_trade_*`,
> the renamed build isn't deployed yet — redeploy** (§8).

---

## 2. Start / stop / restart / status

```bash
# status of everything
systemctl status trading-engine mock-exchange netdata --no-pager
systemctl is-active trading-engine mock-exchange netdata bod-check.timer trade-ops-cpu

# start / stop / restart (start the exchange FIRST — the engine Requires it)
sudo systemctl start  mock-exchange trading-engine
sudo systemctl stop   trading-engine mock-exchange
sudo systemctl restart trading-engine        # picks up a fresh histogram

# logs
journalctl -u trading-engine -n 50 --no-pager    # service stderr (startup line, restarts)
journalctl -u mock-exchange  -n 20 --no-pager
tail -f /tmp/trading-engine.log                   # the engine's own event log (ring3)
```

The engine's startup line in `journalctl` shows the effective config, e.g.
`dispatch=crtp alloc=pool huge=on(on_pages=1) pin=1 rt=1 rate=2000 tsc=2.611GHz`.

---

## 3. Beginning-of-Day (BOD) readiness checks

```bash
# run it now (reads /etc/trade-ops/bod.conf)
sudo /usr/local/bin/bod_check.sh ; echo "exit=$?"
#   exit 0 = READY, 1 = READY WITH WARNINGS, 2 = NOT READY

# the scheduled run (pre-open) and when it next fires
systemctl list-timers bod-check.timer --no-pager
sudo systemctl start bod-check.service            # trigger the timed run by hand
journalctl -u bod-check.service -n 30 --no-pager  # see the last result
```

**What PASS looks like now:** isolation active, SMT siblings offline, invariant
TSC, hugepages reserved *and in use*, THP off, governor performance, services
active, exchange reachable → **15 pass, 0 fail, READY** (2 warnings are unused
wired NICs).

---

## 4. Demo — self-heal (high availability)

```bash
# kill the engine abnormally; systemd Restart=on-failure brings it back in ~2s
sudo systemctl kill -s SIGKILL trading-engine
watch -n0.5 'systemctl show trading-engine -p NRestarts -p ActiveState'
# NRestarts increments; ActiveState returns to active; it re-logs on to the exchange
```

**Observe:** in `journalctl -u trading-engine` you'll see it restart and print a
new logon-acknowledged line; `curl :8000/metrics` resumes counting.

---

## 5. Demo — readiness gate (NOT READY)

```bash
sudo systemctl stop trading-engine                # CLEAN stop = not a failure, stays down
sudo /usr/local/bin/bod_check.sh ; echo "exit=$?" # -> FAIL trading-engine, exit 2 = NOT READY
sudo systemctl start trading-engine               # bring it back
sudo /usr/local/bin/bod_check.sh ; echo "exit=$?" # -> READY again
```

This is the point of `SuccessExitStatus=1`: warnings don't block, a real failure
(exit 2) does — a clean signal for "do not open."

---

## 6. Demo — latency injection (troubleshooting)

```bash
sudo ./scripts/latency_demo.sh on      # netem: +5ms±1ms on loopback
#   watch engine_rtt_p50_ns jump ~10ms on :8000 / the dashboard
sudo ./scripts/latency_demo.sh status
sudo ./scripts/latency_demo.sh off     # restore baseline
```

**Observe:** `engine_rtt_*` spikes (round trip crosses loopback); `engine_t2t_*`
(the engine compute path) barely moves. Measured: `rtt` p50 11.2 µs → **11.47 ms**
(1024×), `t2t` p50 466 → **490 ns** (+5%, and partly just the cumulative
histogram re-mixing). Don't oversell it as "zero change" — the honest version is
stronger: the path you own is ~1000× less sensitive than the path you don't.

---

## 7. What to observe, and where

| Want to see… | Command / place |
|---|---|
| Hot cores pegged, housekeeping idle | Netdata → **System → CPU** → per-core `cpu8/9/10/11` ~100%, `cpu0-7` low |
| Thread → core pinning | `for t in /proc/$(pgrep -x trading_engine)/task/*; do echo "$(cat $t/comm) $(taskset -cp $(basename $t))"; done` |
| Isolation is live | `cat /proc/cmdline` (isolcpus…), `lscpu -e` (1/3 offline) |
| Hugepages in use | `grep HugePages_ /proc/meminfo` (Free < Total) |
| Engine latency (t2t/rtt) | `curl -s localhost:8000/metrics \| grep _ns` |
| Order flow rate | `curl … \| grep orders_total` (watch it climb) |
| Syscalls on the hot path | `sudo strace -c -p $(pgrep -x trading_engine) -f` (Ctrl-C after a few s) |
| Per-thread CPU% | `top -H -p $(pgrep -x trading_engine)` |

---

## 8. Change engine behaviour (A/B) and redeploy

The systemd unit's flags come from `roles/trading-app-deploy/defaults/main.yml`
(`trading_dispatch`, `trading_alloc`, `trading_huge`, `trading_rate`, …).

**Option A — via Ansible (the "production" way):** edit the defaults, then:
```bash
ansible-playbook -i inventory/local.yml playbooks/site.yml --tags app -K
```
This rebuilds, reinstalls, and restarts the services.

**Option B — run a variant by hand (quick A/B):** stop the service, run the
binary with flags, then restart. (Run as root for SCHED_FIFO + hugepages.)
```bash
sudo systemctl stop trading-engine
sudo /opt/trade-ops/app/bin/trading_engine \
     --dispatch=virtual --alloc=malloc --huge=on --pin=on --rt=on \
     --md-cpu=8 --strat-cpu=9 --gw-cpu=10 --rate=2000
#   ^ compare engine_t2t_p99_ns vs the crtp/pool baseline, then Ctrl-C
sudo systemctl start trading-engine
```

**Clean per-op A/B (microbench):** run on a FREE core (not a busy isolated one):
```bash
taskset -c 6 /opt/trade-ops/src/build/microbench    # housekeeping E-core
taskset -c 2 /opt/trade-ops/src/build/microbench    # P-core — different ratios, see ProjectDeepDive §7
```

**Full attribution sweep (what each knob is worth):** stops the engine, runs the
microbench on a P-core / housekeeping E-core / isolated E-core, then one engine
variant per flag, then restarts the service. ~5 min:
```bash
sudo ./scripts/ab_sweep.sh                              # 45s dwell, rate=2000
sudo RATE=50000 OUT=/tmp/ab_hirate.txt ./scripts/ab_sweep.sh 30
```
Use the **50k** rate for anything you intend to quote: at rate=2000 the hot path
runs at a 0.1% duty cycle, every tick lands on cold caches, and the per-op deltas
sit below the run-to-run noise floor (ProjectDeepDive §7).

---

## 9. Re-tune / re-deploy from scratch

```bash
# full converge (idempotent): kernel tuning, netdata, build+deploy, BOD
ansible-playbook -i inventory/local.yml playbooks/site.yml -K
# just one layer:
ansible-playbook -i inventory/local.yml playbooks/site.yml --tags kernel -K
ansible-playbook -i inventory/local.yml playbooks/site.yml --tags app    -K
# dry run (preview): add --check --diff  (note: build-dependent tasks may show
# errors in check mode; that's expected)
```

---

## 10. Rollback (undo the isolation)

Only the GRUB cmdline is boot-persistent; everything else is reversible at
runtime.

```bash
# revert the kernel cmdline
sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT=.*/GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"/' /etc/default/grub
sudo update-grub
# undo the runtime CPU tuning (siblings back online, governor back)
sudo systemctl disable --now trade-ops-cpu.service
for c in 1 3; do echo 1 | sudo tee /sys/devices/system/cpu/cpu$c/online; done
sudo reboot     # to fully clear isolcpus/nohz_full/hugepages
```

At the GRUB menu you can also press `e` and delete the params to boot once
unmodified.

---

## 11. Known issues / housekeeping

- **Engine log grows unbounded.** `/tmp/trading-engine.log` gets one line per
  order (~5 GB/day at rate=2000). Truncate it when needed:
  `sudo truncate -s0 /tmp/trading-engine.log`. (A production logger would rotate
  or rate-limit; noted in remaining tasks.)
- **Netdata (apt) has no go.d plugin**, so the engine's Prometheus metrics don't
  appear as netdata charts — read them from `:8000` directly. Per-core CPU
  charts (the key visual) work.
- **Renamed metrics** (`t2t`/`rtt`) require a redeploy (§8) to go live.
