#!/usr/bin/env python3
"""KenshiCoop devtools dashboard server (Python 3.12, standard library only).

Bridges the plugin's UDP telemetry to a browser page:

  game instances --UDP JSON--> 127.0.0.1:27850 (--udp)  --SSE--> GET /events
  browser POST /cmd {"instance":"role:pid","cmd":"..."} --UDP text--> 127.0.0.1:<cmdPort>
  instance replies (to our dedicated command socket)    --SSE--> GET /events

Protocol: docs/DEVTOOLS.md. Usage: tools/devkit/dashboard/README.md.
"""

import argparse
import json
import os
import queue
import socket
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX_PATH = os.path.join(HERE, "index.html")

STALE_AFTER_S = 5.0      # an instance silent this long is shown stale
SNAPSHOT_EVERY_S = 1.0   # instances snapshot cadence on the SSE stream
CLIENT_QUEUE_MAX = 4000  # per-browser backlog; a stuck tab drops, it never stalls the UDP thread


# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

class Hub:
    """Instances table + SSE fan-out. Every method is thread-safe."""

    def __init__(self, log_path):
        self._lock = threading.Lock()
        self._instances = {}   # "role:pid" -> {"role","pid","cmdPort","global","lastSeen","count"}
        self._clients = set()  # queue.Queue per connected /events stream
        self._log = open(log_path, "a", encoding="utf-8") if log_path else None

    # -- instances ----------------------------------------------------------

    def note_datagram(self, obj, now):
        """Track the sending instance. Only 'global' carries cmdPort/version."""
        role = obj.get("role")
        pid = obj.get("pid")
        if role is None or pid is None:
            return
        key = "%s:%s" % (role, pid)
        with self._lock:
            inst = self._instances.get(key)
            if inst is None:
                inst = {"id": key, "role": role, "pid": pid, "cmdPort": None,
                        "global": None, "lastSeen": now, "count": 0}
                self._instances[key] = inst
            inst["lastSeen"] = now
            inst["count"] += 1
            if obj.get("k") == "global":
                inst["global"] = obj
                port = obj.get("cmdPort")
                if isinstance(port, int) and 0 < port < 65536:
                    inst["cmdPort"] = port

    def cmd_port(self, key):
        with self._lock:
            inst = self._instances.get(key)
            return None if inst is None else inst["cmdPort"]

    def snapshot(self, now):
        with self._lock:
            out = []
            for inst in self._instances.values():
                age = now - inst["lastSeen"]
                out.append({"id": inst["id"], "role": inst["role"], "pid": inst["pid"],
                            "cmdPort": inst["cmdPort"], "global": inst["global"],
                            "age": round(age, 1), "stale": age > STALE_AFTER_S,
                            "count": inst["count"]})
        out.sort(key=lambda i: (i["role"] != "host", str(i["pid"])))
        return {"k": "instances", "list": out}

    # -- SSE fan-out ----------------------------------------------------------

    def add_client(self):
        q = queue.Queue(maxsize=CLIENT_QUEUE_MAX)
        with self._lock:
            self._clients.add(q)
        return q

    def remove_client(self, q):
        with self._lock:
            self._clients.discard(q)

    def publish(self, line):
        """line: one JSON text (no newline). Sent as one SSE 'data:' frame."""
        with self._lock:
            clients = list(self._clients)
        for q in clients:
            try:
                q.put_nowait(line)
            except queue.Full:
                pass  # this tab is not draining; drop rather than block telemetry

    def log(self, line):
        if self._log is None:
            return
        with self._lock:
            self._log.write(line + "\n")
            self._log.flush()


# ---------------------------------------------------------------------------
# UDP: telemetry in, commands out / replies in
# ---------------------------------------------------------------------------

def parse_datagram(data):
    try:
        obj = json.loads(data.decode("utf-8", errors="replace"))
    except ValueError:
        return None
    return obj if isinstance(obj, dict) else None


def telemetry_loop(sock, hub):
    while True:
        try:
            data, _addr = sock.recvfrom(65535)
        except ConnectionResetError:
            continue  # Windows reports ICMP port-unreachable on UDP recv; harmless
        except OSError:
            return
        obj = parse_datagram(data)
        if obj is None:
            continue
        now = time.time()
        obj["_rx"] = int(now * 1000)  # receive wall clock: aligns host/join timelines in the page
        hub.note_datagram(obj, now)
        line = json.dumps(obj, separators=(",", ":"))
        hub.log(line)
        hub.publish(line)


def reply_loop(sock, hub):
    while True:
        try:
            data, addr = sock.recvfrom(65535)
        except ConnectionResetError:
            # An instance whose cmdPort is closed: tell the page instead of silently losing it.
            line = json.dumps({"k": "reply", "ok": 0, "cmd": "?",
                               "msg": "command port unreachable (instance gone?)",
                               "_rx": int(time.time() * 1000)}, separators=(",", ":"))
            hub.log(line)
            hub.publish(line)
            continue
        except OSError:
            return
        obj = parse_datagram(data)
        if obj is None:
            obj = {"k": "reply", "ok": 0, "cmd": "?",
                   "msg": "non-JSON reply from %s:%d: %r" % (addr[0], addr[1], data[:200])}
        obj["_rx"] = int(time.time() * 1000)
        line = json.dumps(obj, separators=(",", ":"))
        hub.log(line)
        hub.publish(line)


def snapshot_loop(hub):
    while True:
        time.sleep(SNAPSHOT_EVERY_S)
        hub.publish(json.dumps(hub.snapshot(time.time()), separators=(",", ":")))


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def make_handler(hub, cmd_sock):

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "KenshiCoopDashboard/1"

        def log_message(self, fmt, *args):
            pass  # SSE + polling would flood the console

        def _send(self, status, body, ctype):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, status, obj):
            self._send(status, json.dumps(obj).encode("utf-8"), "application/json")

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path in ("/", "/index.html"):
                try:
                    with open(INDEX_PATH, "rb") as f:
                        body = f.read()
                except OSError as e:
                    self._send(HTTPStatus.INTERNAL_SERVER_ERROR, str(e).encode(), "text/plain")
                    return
                self._send(HTTPStatus.OK, body, "text/html; charset=utf-8")
            elif path == "/events":
                self._events()
            elif path == "/instances":
                self._json(HTTPStatus.OK, hub.snapshot(time.time()))
            else:
                self._send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")

        def _events(self):
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self.close_connection = True
            q = hub.add_client()
            try:
                # Immediate snapshot so the page lists instances without waiting a tick.
                first = json.dumps(hub.snapshot(time.time()), separators=(",", ":"))
                self.wfile.write(("data: %s\n\n" % first).encode("utf-8"))
                self.wfile.flush()
                while True:
                    try:
                        line = q.get(timeout=10)
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                        continue
                    # Batch whatever is queued into one write: 20 Hz x hands x instances.
                    parts = [line]
                    while len(parts) < 500:
                        try:
                            parts.append(q.get_nowait())
                        except queue.Empty:
                            break
                    self.wfile.write("".join("data: %s\n\n" % p for p in parts).encode("utf-8"))
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass
            finally:
                hub.remove_client(q)

        def do_POST(self):
            if self.path.split("?", 1)[0] != "/cmd":
                self._send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")
                return
            try:
                n = int(self.headers.get("Content-Length") or 0)
                req = json.loads(self.rfile.read(n).decode("utf-8")) if n else {}
            except ValueError:
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "bad JSON"})
                return
            key = req.get("instance") if isinstance(req, dict) else None
            cmd = req.get("cmd") if isinstance(req, dict) else None
            if not isinstance(key, str) or not isinstance(cmd, str) or not cmd.strip():
                self._json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "need instance and cmd"})
                return
            cmd = cmd.strip()
            port = hub.cmd_port(key)
            if port is None:
                self._json(HTTPStatus.NOT_FOUND,
                           {"ok": False, "error": "unknown instance or no cmdPort yet: " + key})
                return
            try:
                cmd_sock.sendto(cmd.encode("utf-8"), ("127.0.0.1", port))
            except OSError as e:
                self._json(HTTPStatus.BAD_GATEWAY, {"ok": False, "error": str(e)})
                return
            sent = {"k": "sent", "instance": key, "cmd": cmd, "port": port,
                    "_rx": int(time.time() * 1000)}
            line = json.dumps(sent, separators=(",", ":"))
            hub.log(line)
            hub.publish(line)
            self._json(HTTPStatus.OK, {"ok": True, "port": port})

    return Handler


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="KenshiCoop devtools dashboard")
    ap.add_argument("--udp", type=int, default=27850, help="telemetry UDP port on 127.0.0.1 (default 27850)")
    ap.add_argument("--http", type=int, default=8765, help="HTTP port on 127.0.0.1 (default 8765)")
    ap.add_argument("--log", metavar="FILE.jsonl", help="append every datagram/reply/sent command as JSON lines")
    args = ap.parse_args()

    hub = Hub(args.log)

    tel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tel.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    tel.bind(("127.0.0.1", args.udp))

    # Dedicated command socket: instances reply to the command's source address,
    # so replies come back here and never mix with telemetry.
    cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cmd.bind(("127.0.0.1", 0))

    for target, a in ((telemetry_loop, (tel, hub)), (reply_loop, (cmd, hub)), (snapshot_loop, (hub,))):
        threading.Thread(target=target, args=a, daemon=True).start()

    httpd = ThreadingHTTPServer(("127.0.0.1", args.http), make_handler(hub, cmd))
    httpd.daemon_threads = True
    print("dashboard: http://127.0.0.1:%d/  telemetry udp 127.0.0.1:%d  cmd socket :%d%s"
          % (args.http, args.udp, cmd.getsockname()[1],
             ("  log " + args.log) if args.log else ""), flush=True)
    try:
        httpd.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        tel.close()
        cmd.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
