#!/usr/bin/env bash
# Applied at every boot by trade-ops-cpu.service (and once immediately by the
# kernel-tuning role). Everything here is RUNTIME and REVERSIBLE — disable the
# service and reboot (or re-online the CPUs / reset the governor) to revert.
#
#   1. Offline the odd SMT siblings so each isolated hot thread owns a full
#      physical core with no hyper-thread contention.
#   2. Set the CPU frequency governor to 'performance' on all online CPUs.
#   3. Optionally disable turbo/boost for benchmark-stable numbers.
set -uo pipefail

CONF=/etc/trade-ops/cpu-tuning.conf
[ -r "$CONF" ] && . "$CONF"

OFFLINE_SIBLINGS="${OFFLINE_SIBLINGS:-9 11 13 15}"
GOVERNOR="${GOVERNOR:-performance}"
DISABLE_TURBO="${DISABLE_TURBO:-0}"

echo "[trade-ops-cpu] offlining SMT siblings: ${OFFLINE_SIBLINGS}"
for c in $OFFLINE_SIBLINGS; do
  f="/sys/devices/system/cpu/cpu${c}/online"
  [ -w "$f" ] || continue
  # Retry: at boot the write can transiently return EBUSY while the scheduler
  # migrates kernel threads off the core. Give it a few tries before giving up.
  for attempt in 1 2 3 4 5; do
    if echo 0 > "$f" 2>/dev/null; then
      echo "  cpu${c} -> offline"
      break
    fi
    [ "$attempt" = 5 ] && echo "  cpu${c} -> STILL BUSY after 5 tries" || sleep 0.5
  done
done

echo "[trade-ops-cpu] setting governor: ${GOVERNOR}"
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo "$GOVERNOR" > "$g" 2>/dev/null || true
done

if [ "$DISABLE_TURBO" = "1" ]; then
  echo "[trade-ops-cpu] disabling turbo/boost"
  [ -w /sys/devices/system/cpu/cpufreq/boost ] && echo 0 > /sys/devices/system/cpu/cpufreq/boost
  [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
fi

exit 0
