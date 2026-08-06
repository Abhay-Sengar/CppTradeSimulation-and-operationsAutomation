#!/usr/bin/env bash
# ab_sweep.sh — measure what each tuning decision is actually worth, on THIS box.
#
# Stops the deployed trading-engine service, runs the binary once per variant with
# a single knob flipped, samples :8000/metrics after a fixed dwell, and restarts
# the service. Also runs the per-op microbenchmark on a P-core, an E-core, and an
# isolated core while the box is quiet.
#
# Everything here is a RUNTIME flag — no rebuild between variants, so the binary
# under test is byte-identical across the sweep. The mock exchange keeps running
# throughout (only the engine is swapped), so the rtt path is unchanged too.
#
# Usage:  sudo ./scripts/ab_sweep.sh [dwell_seconds]     (default 45)
set -uo pipefail

DWELL="${1:-45}"
RATE="${RATE:-2000}"      # ticks/sec; raise it to keep the hot path cache-warm
BIN=/opt/trade-ops/app/bin/trading_engine
MICROBENCH=/opt/trade-ops/src/build/microbench
OUT="${OUT:-/tmp/ab_results.txt}"
COMMON="--rate=${RATE} --md-cpu=8 --strat-cpu=9 --gw-cpu=10"

[ "$(id -u)" -eq 0 ] || { echo "must run as root (SCHED_FIFO + hugepages)"; exit 1; }
[ -x "$BIN" ] || { echo "missing $BIN — deploy first"; exit 1; }

echo "=== ab_sweep: dwell=${DWELL}s per variant, output -> $OUT ==="
: > "$OUT"
{
  echo "# ab_sweep $(date -Is)"
  echo "# kernel: $(uname -r)"
  echo "# cmdline: $(cat /proc/cmdline)"
  echo "# governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  echo "# rate: ${RATE}/s  dwell: ${DWELL}s"
  echo "# online: $(cat /sys/devices/system/cpu/online)"
} >> "$OUT"

echo "--- stopping trading-engine (mock-exchange stays up) ---"
systemctl stop trading-engine
sleep 2

# --- microbench on a quiet box: P-core, housekeeping E-core, isolated E-core ---
if [ -x "$MICROBENCH" ]; then
  for spec in "2:P-core" "6:E-core-housekeeping" "8:E-core-isolated"; do
    cpu="${spec%%:*}"; label="${spec##*:}"
    echo "--- microbench on cpu${cpu} (${label}) ---"
    { echo; echo "### microbench cpu${cpu} ${label}"; } >> "$OUT"
    taskset -c "$cpu" "$MICROBENCH" >> "$OUT" 2>&1
  done
else
  echo "!! $MICROBENCH not found — skipping microbench" | tee -a "$OUT"
fi

# --- one variant per knob --------------------------------------------------
run() {
  local label="$1"; shift
  echo "--- ${label}: $* ---"
  # shellcheck disable=SC2086
  "$BIN" $COMMON "$@" > "/tmp/ab_${label}.log" 2>&1 &
  local pid=$!
  sleep "$DWELL"
  {
    echo
    echo "### ${label} :: $*"
    curl -s --max-time 5 localhost:8000/metrics | grep -vE '^#'
  } >> "$OUT"
  kill -TERM "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  sleep 3          # let :8000 and the FIX session close before the next variant
}

#     label      dispatch          alloc           huge        pin/rt
run baseline  --dispatch=crtp    --alloc=pool   --huge=on  --pin=on  --rt=on
run virtual   --dispatch=virtual --alloc=pool   --huge=on  --pin=on  --rt=on
run malloc    --dispatch=crtp    --alloc=malloc --huge=on  --pin=on  --rt=on
run nohuge    --dispatch=crtp    --alloc=pool   --huge=off --pin=on  --rt=on
run nort      --dispatch=crtp    --alloc=pool   --huge=on  --pin=on  --rt=off
run nopin     --dispatch=crtp    --alloc=pool   --huge=on  --pin=off --rt=off
# All three per-op knobs flipped at once — the largest signal available if the
# individual deltas are below the run-to-run noise floor.
run worstcase --dispatch=virtual --alloc=malloc --huge=off --pin=on  --rt=on

echo "--- restarting trading-engine ---"
systemctl start trading-engine

echo
echo "=== done. results in $OUT ==="
