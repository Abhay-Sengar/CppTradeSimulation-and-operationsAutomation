#!/usr/bin/env python3
"""Minimal FIX 4.2 acceptor (mock exchange): acks logon, fills every NewOrderSingle."""
import socket
import datetime
import simplefix

HOST, PORT = "0.0.0.0", 9001
SENDER, TARGET = "EXCHANGE", "TRADER"


def ts():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]


def gv(msg, tag, default=""):
    v = msg.get(tag)
    if v is None:
        return default
    return v.decode() if isinstance(v, bytes) else str(v)


def base(msgtype, seq):
    m = simplefix.FixMessage()
    m.append_pair(8, "FIX.4.2")
    m.append_pair(35, msgtype)
    m.append_pair(49, SENDER)
    m.append_pair(56, TARGET)
    m.append_pair(34, seq)
    m.append_pair(52, ts())
    return m


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(1)
    print(f"[mock-exchange] listening on {HOST}:{PORT}", flush=True)
    while True:
        conn, addr = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"[mock-exchange] client connected: {addr}", flush=True)
        parser = simplefix.FixParser()
        seq = 1
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                parser.append_buffer(data)
                while True:
                    msg = parser.get_message()
                    if msg is None:
                        break
                    mtype = gv(msg, 35)
                    if mtype == "A":
                        r = base("A", seq); seq += 1
                        r.append_pair(98, 0)
                        r.append_pair(108, 30)
                        conn.sendall(r.encode())
                        print("[mock-exchange] logon ack", flush=True)
                    elif mtype == "D":
                        er = base("8", seq); seq += 1
                        er.append_pair(37, f"EX{seq}")
                        er.append_pair(11, gv(msg, 11))
                        er.append_pair(17, f"EXEC{seq}")
                        er.append_pair(150, "2")      # ExecType = Filled
                        er.append_pair(39, "2")        # OrdStatus = Filled
                        er.append_pair(55, gv(msg, 55, "NIFTY"))
                        er.append_pair(54, gv(msg, 54, "1"))
                        er.append_pair(38, gv(msg, 38, "1"))
                        er.append_pair(32, gv(msg, 38, "1"))   # LastQty
                        er.append_pair(31, gv(msg, 44, "0"))   # LastPx
                        conn.sendall(er.encode())
                    elif mtype == "1":                 # TestRequest -> Heartbeat
                        hb = base("0", seq); seq += 1
                        conn.sendall(hb.encode())
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            conn.close()
            print("[mock-exchange] client disconnected", flush=True)


if __name__ == "__main__":
    main()
