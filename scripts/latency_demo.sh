#!/usr/bin/env bash
# Inject / clear artificial latency on the FIX loopback path.
#
# The point of the demo is the CONTRAST: rtt (which crosses loopback twice) blows
# up by ~1000x, while t2t (the in-process engine path) barely moves. Watch
# engine_rtt_* -- t2t is the control, not the signal.
set -euo pipefail
IFACE="${IFACE:-lo}"
DELAY="${DELAY:-5ms}"
JITTER="${JITTER:-1ms}"

case "${1:-}" in
  on)
    sudo tc qdisc replace dev "$IFACE" root netem delay "$DELAY" "$JITTER" distribution normal
    echo "Injected ${DELAY} ± ${JITTER} on ${IFACE}. Loopback is crossed twice, so"
    echo "expect engine_rtt_p50_ns to jump to ~2x the delay (~10ms). engine_t2t_p50_ns"
    echo "should stay within a few percent of baseline — that contrast IS the demo."
    ;;
  off)
    sudo tc qdisc del dev "$IFACE" root 2>/dev/null || true
    echo "Cleared netem on ${IFACE}. engine_rtt_* returns to baseline (~11us)."
    ;;
  status)
    tc qdisc show dev "$IFACE"
    ;;
  *)
    echo "usage: $0 {on|off|status}   (env overrides: IFACE, DELAY, JITTER)"
    exit 1
    ;;
esac
