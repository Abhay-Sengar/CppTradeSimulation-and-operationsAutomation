#!/usr/bin/env bash
# Beginning-of-Day (BOD) readiness checks for the trading node.
set -uo pipefail

CONF=/etc/trade-ops/bod.conf
[ -r "$CONF" ] && . "$CONF"

DISK_WARN_PCT="${DISK_WARN_PCT:-85}"
MEM_WARN_PCT="${MEM_WARN_PCT:-90}"
CLOCK_OFFSET_MAX_MS="${CLOCK_OFFSET_MAX_MS:-50}"
EXCHANGE_HOST="${EXCHANGE_HOST:-127.0.0.1}"
EXCHANGE_PORT="${EXCHANGE_PORT:-9001}"
SERVICES="${SERVICES:-ssh}"
ISOLATED_CPUS="${ISOLATED_CPUS:-8-15}"
OFFLINE_SIBLINGS="${OFFLINE_SIBLINGS:-9 11 13 15}"

PASS=0; WARN=0; FAIL=0
RESULTS=()
c_green=$'\e[32m'; c_yellow=$'\e[33m'; c_red=$'\e[31m'; c_reset=$'\e[0m'

record() { # status message...
  local s="$1"; shift
  case "$s" in
    PASS) PASS=$((PASS+1));;
    WARN) WARN=$((WARN+1));;
    FAIL) FAIL=$((FAIL+1));;
  esac
  RESULTS+=("$s|$*")
}

check_disk() {
  local issue=0 pct mount p
  while read -r pct mount; do
    p="${pct%\%}"
    if [ "${p:-0}" -ge "$DISK_WARN_PCT" ]; then
      record WARN "Disk ${mount} at ${p}% (>= ${DISK_WARN_PCT}%)"; issue=1
    fi
  done < <(df -x tmpfs -x devtmpfs --output=pcent,target | tail -n +2)
  [ "$issue" -eq 0 ] && record PASS "Disk usage OK on all mounts (< ${DISK_WARN_PCT}%)"
}

check_memory() {
  local up; up=$(free | awk '/^Mem:/ {printf "%.0f", $3/$2*100}')
  if [ "$up" -ge "$MEM_WARN_PCT" ]; then record WARN "Memory at ${up}% (>= ${MEM_WARN_PCT}%)"
  else record PASS "Memory at ${up}%"; fi
}

check_clock() {
  if ! command -v chronyc >/dev/null 2>&1; then
    record WARN "chrony not installed — time sync unverified"; return; fi
  local off leap ms
  off=$(chronyc tracking 2>/dev/null | awk -F': ' '/Last offset/ {print $2}' | awk '{print $1}')
  leap=$(chronyc tracking 2>/dev/null | awk -F': ' '/Leap status/ {print $2}')
  if [ -z "$off" ]; then record FAIL "Unable to read chrony tracking"; return; fi
  ms=$(awk -v o="$off" 'BEGIN{o=o<0?-o:o; printf "%.3f", o*1000}')
  if awk -v m="$ms" -v t="$CLOCK_OFFSET_MAX_MS" 'BEGIN{exit !(m<=t)}'; then
    record PASS "Clock offset ${ms} ms (<= ${CLOCK_OFFSET_MAX_MS} ms), leap: ${leap}"
  else
    record FAIL "Clock offset ${ms} ms (> ${CLOCK_OFFSET_MAX_MS} ms)"
  fi
}

check_isolation() {
  if grep -q "isolcpus=" /proc/cmdline; then
    local v; v=$(sed -n 's/.*isolcpus=\([0-9,-]*\).*/\1/p' /proc/cmdline)
    record PASS "CPU isolation active (isolcpus=${v})"
  else
    record FAIL "CPU isolation NOT active in /proc/cmdline"
  fi
}

check_hugepages() {
  local t f
  t=$(awk '/HugePages_Total/ {print $2}' /proc/meminfo)
  f=$(awk '/HugePages_Free/  {print $2}' /proc/meminfo)
  if [ "${t:-0}" -gt 0 ]; then
    # Free < Total means the engine has actually mapped its arena onto huge
    # pages — the visible proof that MAP_HUGETLB is in use, not just reserved.
    if [ "${f:-0}" -lt "${t:-0}" ]; then
      record PASS "Hugepages reserved ${t}, in use $((t - f)) (engine mapped its arena)"
    else
      record WARN "Hugepages reserved ${t} but none in use (engine not running on huge pages)"
    fi
  else
    record WARN "No hugepages reserved"
  fi
}

check_tsc() {
  local flags c n
  flags=$(grep -m1 '^flags' /proc/cpuinfo)
  c=$(echo "$flags" | grep -c constant_tsc)
  n=$(echo "$flags" | grep -c nonstop_tsc)
  if [ "$c" -ge 1 ] && [ "$n" -ge 1 ]; then
    record PASS "Invariant TSC (constant_tsc + nonstop_tsc) — cross-core rdtsc valid"
  else
    record WARN "TSC not fully invariant (constant_tsc=$c nonstop_tsc=$n)"
  fi
}

check_smt_siblings() {
  local off=0 total=0 c st
  for c in $OFFLINE_SIBLINGS; do
    total=$((total + 1))
    st=$(cat "/sys/devices/system/cpu/cpu${c}/online" 2>/dev/null)
    [ "$st" = "0" ] && off=$((off + 1))
  done
  if [ "$off" -eq "$total" ] && [ "$total" -gt 0 ]; then
    record PASS "SMT siblings offline (${OFFLINE_SIBLINGS}) — each hot core is dedicated"
  else
    record WARN "SMT siblings not all offline: ${off}/${total} (${OFFLINE_SIBLINGS})"
  fi
}

check_thp() {
  local thp; thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)
  if echo "$thp" | grep -q '\[never\]'; then record PASS "Transparent Huge Pages disabled"
  else record WARN "THP not disabled: ${thp:-unknown}"; fi
}

check_governor() {
  local f=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  if [ -r "$f" ]; then
    local g; g=$(cat "$f")
    [ "$g" = performance ] && record PASS "CPU governor: performance" \
                           || record WARN "CPU governor: ${g} (expected performance)"
  else
    record WARN "cpufreq not exposed (VM) — governor is a bare-metal/BIOS setting"
  fi
}

check_nic() {
  local dev state
  for dev in $(ls /sys/class/net | grep -v '^lo$'); do
    state=$(cat "/sys/class/net/${dev}/operstate" 2>/dev/null)
    [ "$state" = up ] && record PASS "NIC ${dev} link up" \
                      || record WARN "NIC ${dev} state: ${state}"
  done
}

check_services() {
  local svc
  for svc in $SERVICES; do
    systemctl is-active --quiet "$svc" && record PASS "Service ${svc} active" \
                                       || record FAIL "Service ${svc} NOT active"
  done
}

check_exchange() {
  if command -v nc >/dev/null 2>&1; then
    if nc -z -w2 "$EXCHANGE_HOST" "$EXCHANGE_PORT" 2>/dev/null; then
      record PASS "Exchange gateway reachable ${EXCHANGE_HOST}:${EXCHANGE_PORT}"
    else
      record WARN "Exchange gateway unreachable ${EXCHANGE_HOST}:${EXCHANGE_PORT} (FIX server wired in Step 5)"
    fi
  else
    record WARN "nc not installed — cannot test exchange connectivity"
  fi
}

echo "=== Beginning-of-Day checks  $(date '+%Y-%m-%d %H:%M:%S %Z') ==="
check_disk; check_memory; check_clock; check_isolation; check_smt_siblings
check_tsc; check_hugepages; check_thp; check_governor; check_nic
check_services; check_exchange

echo
printf '%s\n' "${RESULTS[@]}" | column -t -s '|' | \
  sed -e "s/^PASS/${c_green}PASS${c_reset}/" \
      -e "s/^WARN/${c_yellow}WARN${c_reset}/" \
      -e "s/^FAIL/${c_red}FAIL${c_reset}/"
echo
echo "Summary: ${PASS} pass, ${WARN} warn, ${FAIL} fail"

if   [ "$FAIL" -gt 0 ]; then echo "BOD result: NOT READY";            exit 2
elif [ "$WARN" -gt 0 ]; then echo "BOD result: READY WITH WARNINGS";  exit 1
else                         echo "BOD result: READY";                exit 0
fi
