#!/usr/bin/env python3
"""FIX 4.2 initiator (trading engine) with tick-to-trade Prometheus metrics."""
import socket
import time
import itertools
import datetime
import simplefix
from prometheus_client import start_http_server, Histogram, Gauge, Counter

EXCHANGE_HOST, EXCHANGE_PORT = "127.0.0.1", 9001
METRICS_PORT = 8000
SENDER, TARGET = "TRADER", "EXCHANGE"
ORDER_INTERVAL_S = 0.1   # ~10 orders/sec — gentle, keeps the dashboard live

T2T = Histogram(
    "engine_tick_to_trade_seconds",
    "Tick-to-trade latency (signal-in to execution-report-in)",
    buckets=(5e-5, 1e-4, 2.5e-4, 5e-4, 1e-3, 2.5e-3, 5e-3, 1e-2, 2.5e-2, 5e-2),
)
T2T_US = Gauge("engine_t2t_microseconds", "Most recent tick-to-trade latency (microseconds)")
ORDERS = Counter("engine_orders_total", "Total orders sent")
FILLS = Counter("engine_fills_total", "Total fills received")

SYMBOLS = ["NIFTY", "BANKNIFTY", "RELIANCE", "TCS", "INFY", "HDFCBANK"]
SIGNALS = []
for i in range(300):
    SIGNALS.append((
        SYMBOLS[i % len(SYMBOLS)],
        "1" if i % 2 == 0 else "2",          # 1=Buy 2=Sell
        str(50 * (1 + (i % 5))),             # qty
        str(round(100 + (i % 50) * 0.5, 2)),  # price
    ))


def ts():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]


def gv(msg, tag, default=""):
    v = msg.get(tag)
    if v is None:
        return default
    return v.decode() if isinstance(v, bytes) else str(v)


def main():
    start_http_server(METRICS_PORT)
    print(f"[engine] metrics on :{METRICS_PORT}/metrics", flush=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect((EXCHANGE_HOST, EXCHANGE_PORT))
    parser = simplefix.FixParser()
    seq = 1

    logon = simplefix.FixMessage()
    for t, v in [(8, "FIX.4.2"), (35, "A"), (49, SENDER), (56, TARGET),
                 (34, seq), (52, ts()), (98, 0), (108, 30)]:
        logon.append_pair(t, v)
    seq += 1
    sock.sendall(logon.encode())

    sock.settimeout(5)
    while True:
        data = sock.recv(4096)
        if not data:
            raise RuntimeError("exchange closed during logon")
        parser.append_buffer(data)
        m = parser.get_message()
        if m and gv(m, 35) == "A":
            print("[engine] logon acknowledged", flush=True)
            break

    sock.settimeout(2)
    oid = itertools.count(1)
    for sym, side, qty, px in itertools.cycle(SIGNALS):
        nos = simplefix.FixMessage()
        for t, v in [(8, "FIX.4.2"), (35, "D"), (49, SENDER), (56, TARGET),
                     (34, seq), (52, ts()), (11, str(next(oid))),
                     (55, sym), (54, side), (38, qty), (40, "2"), (44, px)]:
            nos.append_pair(t, v)
        seq += 1

        t0 = time.perf_counter()
        sock.sendall(nos.encode())
        ORDERS.inc()

        got = False
        while not got:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                break
            if not data:
                raise RuntimeError("exchange closed")
            parser.append_buffer(data)
            while True:
                er = parser.get_message()
                if er is None:
                    break
                if gv(er, 35) == "8":
                    dt = time.perf_counter() - t0
                    T2T.observe(dt)
                    T2T_US.set(dt * 1e6)
                    FILLS.inc()
                    got = True
        time.sleep(ORDER_INTERVAL_S)


if __name__ == "__main__":
    main()
