#!/usr/bin/env python3
"""Fake KenshiCoop telemetry: two instances (host + join), two hands each.

Exercises the dashboard without the game. Emits contract datagrams
(docs/DEVTOOLS.md) to 127.0.0.1:<--udp> and answers text commands on each
instance's own cmdPort (ephemeral, announced in its 'global' datagram).

Scenario:
  - "Alice" (hand 1,3332275456) is owned by the host, "Bob" (hand 2,1234567890)
    by the join. Both walk/stop on a loop; the foot distance swings while a
    body moves (gait clip) and is constant while still.
  - On the HOST's driven copy of Bob, every --detach-every seconds the model
    detaches for --detach-for seconds: hk goes null, the mesh (pelvis) freezes,
    the nameplate position keeps moving and the foot distance goes flat. That
    is the "frozen model" bug; with the watchdog on it trips once per episode.
  - Commands mutate the fake: knockdown sets/clears BODY_DOWN, restore ends a
    detach, glide/overlay/watchdog/record flip the global flags, dump and mark
    emit events.
"""

import argparse
import json
import math
import random
import select
import socket
import sys
import time

TICK_S = 0.05          # 20 Hz body telemetry, as the plugin
GLOBAL_EVERY_S = 0.5
BODY_DOWN = 1 << 0
VERSION = "fake-0.57.4"
MESH_TRIP = 15.0       # watchdog threshold mirrored from the dashboard's red-row rule

def r1(v):
    return round(v, 1)


def v3(p):
    return None if p is None else [r1(p[0]), r1(p[1]), r1(p[2])]


def hdist(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


class Mover:
    """Owner-side truth of one character: a walk/stop loop around a centre."""

    def __init__(self, cx, cz, radius, phase, walk_s, stop_s, speed):
        self.cx, self.cz, self.radius = cx, cz, radius
        self.phase = phase
        self.walk_s, self.stop_s, self.speed = walk_s, stop_s, speed
        self.angle = phase
        self.moving = False
        self.cur_speed = 0.0

    def step(self, t, dt):
        cycle = self.walk_s + self.stop_s
        self.moving = ((t + self.phase * 3.0) % cycle) < self.walk_s
        self.cur_speed = self.speed if self.moving else 0.0
        if self.moving:
            self.angle += (self.speed / self.radius) * dt

    def pos(self):
        return [self.cx + self.radius * math.cos(self.angle), 1200.0,
                self.cz + self.radius * math.sin(self.angle)]


class Hand:
    def __init__(self, hand, name, owner_role, mover):
        self.hand = hand
        self.name = name
        self.owner_role = owner_role
        self.mover = mover
        self.gait = 0.0


class View:
    """One instance's view of one hand (what its DevBody would contain)."""

    def __init__(self):
        self.np = None
        self.mesh = None
        self.bs = 0
        self.detached_until = 0.0
        self.tripped = False
        self.last_detach = 0.0


class Instance:
    def __init__(self, role, pid, local_id, udp_target):
        self.role = role
        self.pid = pid
        self.local_id = local_id
        self.target = udp_target
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.cmd_port = self.sock.getsockname()[1]
        self.start = time.time()
        self.glide = True
        self.overlay = False
        self.watchdog = True
        self.rec = ""
        self.views = {}

    def t_ms(self, now):
        return int((now - self.start) * 1000)

    def send(self, obj):
        data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        assert len(data) <= 1200, "datagram over contract limit: %d" % len(data)
        try:
            self.sock.sendto(data, self.target)
        except OSError:
            pass  # dashboard not running yet; UDP is fire-and-forget

    def event(self, now, ev, rule, hand, msg, dump=""):
        self.send({"k": "event", "role": self.role, "pid": self.pid, "t": self.t_ms(now),
                   "ev": ev, "rule": rule, "hand": hand, "msg": msg, "dump": dump})


# ---------------------------------------------------------------------------
# Simulation
# ---------------------------------------------------------------------------

def body_datagram(inst, h, view, now):
    m = h.mover
    own = inst.role == h.owner_role
    truth = m.pos()
    down = (view.bs & BODY_DOWN) != 0
    detached = now < view.detached_until

    # Driven copy lags the stream a little; own copy IS the truth.
    if own:
        np = truth
    else:
        lag = 0.25 if m.moving else 0.0
        back = (m.speed / m.radius) * lag if m.moving else 0.0
        np = [m.cx + m.radius * math.cos(m.angle - back), truth[1],
              m.cz + m.radius * math.sin(m.angle - back)]
        np[0] += random.uniform(-0.3, 0.3)
        np[2] += random.uniform(-0.3, 0.3)
    if down:
        np = view.np or np  # a downed body does not travel
    view.np = np

    # Model (pelvis): follows np with a small offset, or freezes while detached.
    if detached:
        if view.mesh is None:
            view.mesh = [np[0], np[1] + 9.0, np[2]]  # pelvis stays where the body was
        mesh = view.mesh
        hk = None
    else:
        mesh = [np[0] + random.uniform(-0.6, 0.6), np[1] + 9.0, np[2] + random.uniform(-0.6, 0.6)]
        view.mesh = None
        hk = [np[0] + random.uniform(-0.2, 0.2), np[1], np[2] + random.uniform(-0.2, 0.2)]

    # Foot distance swings while a gait clip plays; flat while still/sliding/down.
    if m.moving and not detached and not down:
        foot = 1.2 + 3.0 * abs(math.sin(h.gait))
    else:
        foot = 1.3 + random.uniform(-0.05, 0.05)

    if own:
        reg, gap, vlen, cspd = "own", 0.0, 0.0, 0.0
        author = inst.local_id
    else:
        reg = "down" if down else ("glide" if (m.moving and inst.glide) else ("walk" if m.moving else "rest"))
        gap = hdist(truth, np) + random.uniform(0.0, 0.4)
        vlen = m.cur_speed + (random.uniform(-0.5, 0.5) if m.moving else 0.0)
        cspd = m.cur_speed
        author = 0 if h.owner_role == "host" else 1

    task = 4 if m.moving else 65535
    return {"k": "body", "role": inst.role, "pid": inst.pid, "t": inst.t_ms(now),
            "hand": h.hand, "name": h.name, "own": 1 if own else 0, "author": author,
            "np": v3(np), "mesh": v3(mesh), "foot": r1(foot), "hk": v3(hk),
            "bs": view.bs, "task": task, "carrying": 0, "carried": 0,
            "reg": reg, "fresh": 0 if own else 1, "gap": r1(gap), "vlen": r1(vlen),
            "cspd": r1(cspd), "smov": 1 if m.moving else 0,
            "sbs": view.bs if own else 0, "stask": 0 if own else task,
            "glide": 1 if (reg == "glide") else 0, "dest": 0,
            "newest": None if own else v3(truth)}


def global_datagram(inst, now, peers):
    return {"k": "global", "role": inst.role, "pid": inst.pid, "t": inst.t_ms(now),
            "ver": VERSION, "cmdPort": inst.cmd_port, "host": 1 if inst.role == "host" else 0,
            "localId": inst.local_id, "peers": peers, "net": 1, "world": 1,
            "slew": 1.0 if inst.role == "host" else r1(1.0 + 0.05 * math.sin(now / 7.0)),
            "speed": 1.0, "paused": 0, "overlay": 1 if inst.overlay else 0,
            "watchdog": 1 if inst.watchdog else 0, "rec": inst.rec}


def watchdog(inst, hands, now):
    if not inst.watchdog:
        return
    for h in hands:
        v = inst.views[h.hand]
        if now < v.detached_until and v.mesh is not None and v.np is not None:
            if not v.tripped and hdist(v.mesh, v.np) > MESH_TRIP:
                v.tripped = True
                dump = "kc_dumps\\%s_%d_%d_%s.txt" % (inst.role, inst.pid, inst.t_ms(now),
                                                       h.hand.replace(",", "_"))
                inst.event(now, "trip", "mesh_detached", h.hand,
                           "|mesh-np| %.1f > %.0f, hk missing on upright body"
                           % (hdist(v.mesh, v.np), MESH_TRIP), dump)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

HELP = ("ping | help | restore <hand> | endaction <hand> | knockdown <hand> <0|1> | halt <hand> | "
        "glide <on|off> | overlay <on|off> | watchdog <on|off> | dump <hand> | mark <text> | "
        "record <start|stop>")


def handle_command(inst, text, now):
    """Returns (ok, msg). Mirrors the plugin's command set."""
    parts = text.strip().split()
    if not parts:
        return 0, "empty command"
    verb, rest = parts[0].lower(), parts[1:]

    def onoff(name):
        if len(rest) != 1 or rest[0] not in ("on", "off"):
            return None
        return rest[0] == "on"

    def view_of(hand):
        return inst.views.get(hand)

    if verb == "ping":
        return 1, "pong %s pid=%d t=%d" % (inst.role, inst.pid, inst.t_ms(now))
    if verb == "help":
        return 1, HELP
    if verb in ("glide", "overlay", "watchdog"):
        v = onoff(verb)
        if v is None:
            return 0, "usage: %s <on|off>" % verb
        setattr(inst, verb, v)
        return 1, "%s %s" % (verb, "on" if v else "off")
    if verb == "record":
        if rest == ["start"]:
            inst.rec = "kc_records\\%s_%d_%d.kcrec" % (inst.role, inst.pid, int(now))
            return 1, "recording to " + inst.rec
        if rest == ["stop"]:
            was, inst.rec = inst.rec, ""
            return 1, ("stopped " + was) if was else "was not recording"
        return 0, "usage: record <start|stop>"
    if verb == "mark":
        msg = text.strip()[4:].strip()
        inst.event(now, "mark", "mark", "", msg)
        return 1, "marked: " + msg
    if verb in ("restore", "endaction", "halt", "dump", "knockdown"):
        if not rest:
            return 0, "usage: %s <hand>" % verb
        v = view_of(rest[0])
        if v is None:
            return 0, "no such hand " + rest[0]
        if verb == "restore":
            was = now < v.detached_until
            v.detached_until = 0.0
            v.tripped = False
            return 1, "restoreMovement %s (%s)" % (rest[0], "re-attached havok body" if was else "nothing to fix")
        if verb == "endaction":
            return 1, "endAction %s" % rest[0]
        if verb == "halt":
            return 1, "haltMovement %s" % rest[0]
        if verb == "dump":
            dump = "kc_dumps\\%s_%d_%d_%s_manual.txt" % (inst.role, inst.pid, inst.t_ms(now),
                                                          rest[0].replace(",", "_"))
            inst.event(now, "cmd", "dump", rest[0], "manual dump", dump)
            return 1, "dumped " + dump
        # knockdown
        if len(rest) != 2 or rest[1] not in ("0", "1"):
            return 0, "usage: knockdown <hand> <0|1>"
        if rest[1] == "1":
            v.bs |= BODY_DOWN
        else:
            v.bs &= ~BODY_DOWN
        return 1, "knockDown %s %s bs=%d" % (rest[0], rest[1], v.bs)
    return 0, "unknown command '%s' (try help)" % verb


def serve_commands(instances, now):
    for inst in instances:
        while True:
            try:
                data, addr = inst.sock.recvfrom(2048)
            except BlockingIOError:
                break
            except ConnectionResetError:
                continue  # Windows: an earlier telemetry send hit a closed port
            except OSError:
                break
            text = data.decode("utf-8", errors="replace")
            ok, msg = handle_command(inst, text, now)
            reply = {"k": "reply", "role": inst.role, "pid": inst.pid, "cmd": text.strip(),
                     "ok": ok, "msg": msg[:900]}
            inst.sock.sendto(json.dumps(reply, separators=(",", ":")).encode("utf-8"), addr)
            # mark/dump emit their own event; queries change nothing worth logging.
            verb = text.strip().split(" ", 1)[0].lower()
            if ok and verb not in ("ping", "help", "mark", "dump"):
                inst.event(now, "cmd", verb, "", text.strip()[:200])


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="fake KenshiCoop telemetry for the dashboard")
    ap.add_argument("--udp", type=int, default=27850, help="dashboard telemetry port (default 27850)")
    ap.add_argument("--detach-every", type=float, default=20.0, help="seconds between model detaches")
    ap.add_argument("--detach-for", type=float, default=8.0, help="detach duration in seconds")
    ap.add_argument("--duration", type=float, default=0.0, help="stop after N seconds (0 = forever)")
    args = ap.parse_args()

    target = ("127.0.0.1", args.udp)
    host = Instance("host", 41001, 0, target)
    join = Instance("join", 41002, 1, target)
    instances = [host, join]
    for inst in instances:
        inst.sock.setblocking(False)

    hands = [
        Hand("1,3332275456", "Alice", "host", Mover(-4200.0, 3100.0, 60.0, 0.0, 9.0, 4.0, 22.0)),
        Hand("2,1234567890", "Bob", "join", Mover(-4150.0, 3150.0, 45.0, 1.3, 7.0, 3.0, 26.0)),
    ]
    for inst in instances:
        for h in hands:
            inst.views[h.hand] = View()

    bob_on_host = host.views["2,1234567890"]
    print("fake telemetry -> udp %s:%d   host cmdPort %d   join cmdPort %d"
          % (target[0], target[1], host.cmd_port, join.cmd_port), flush=True)

    t0 = time.time()
    last = t0
    next_global = t0
    next_detach = t0 + 5.0
    try:
        while True:
            now = time.time()
            if args.duration and now - t0 > args.duration:
                break
            dt = now - last
            last = now
            t = now - t0

            for h in hands:
                h.mover.step(t, dt)
                if h.mover.moving:
                    h.gait += dt * 2.0 * math.pi * 0.9  # ~0.9 strides/s

            # The bug scenario: host's driven copy of Bob periodically loses its havok body.
            if now >= next_detach:
                bob_on_host.detached_until = now + args.detach_for
                bob_on_host.tripped = False
                next_detach = now + args.detach_every

            for inst in instances:
                for h in hands:
                    inst.send(body_datagram(inst, h, inst.views[h.hand], now))
                watchdog(inst, hands, now)

            if now >= next_global:
                for inst in instances:
                    inst.send(global_datagram(inst, now, 1))
                next_global = now + GLOBAL_EVERY_S

            # Sleep until the next tick but wake for commands.
            timeout = max(0.0, TICK_S - (time.time() - now))
            r, _, _ = select.select([i.sock for i in instances], [], [], timeout)
            if r:
                serve_commands(instances, time.time())
                rem = TICK_S - (time.time() - now)
                if rem > 0:
                    time.sleep(rem)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
