#!/usr/bin/env bash
# Inject / clear artificial latency on the FIX loopback path to demo T2T impact.
set -euo pipefail
IFACE="${IFACE:-lo}"
DELAY="${DELAY:-5ms}"
JITTER="${JITTER:-1ms}"

case "${1:-}" in
  on)
    sudo tc qdisc replace dev "$IFACE" root netem delay "$DELAY" "$JITTER" distribution normal
    echo "Injected ${DELAY} ± ${JITTER} on ${IFACE}. Round-trip adds ~2x — watch T2T on the dashboard."
    ;;
  off)
    sudo tc qdisc del dev "$IFACE" root 2>/dev/null || true
    echo "Cleared netem on ${IFACE}. T2T returns to baseline."
    ;;
  status)
    tc qdisc show dev "$IFACE"
    ;;
  *)
    echo "usage: $0 {on|off|status}   (env overrides: IFACE, DELAY, JITTER)"
    exit 1
    ;;
esac
