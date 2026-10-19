// kcreplay - replay a .kcrec packet recording into a live KenshiCoop peer.
//
//   kcreplay <file> --as join [--host 127.0.0.1] [--port 27800] [--peer N]
//                   [--speed 1.0] [--skip-types 29-36] [--linger 2] [--wait 10]
//   kcreplay <file> --as host [--port 27800] [--peer N] [--speed 1.0]
//                   [--skip-types 29-36] [--linger 0] [--wait 120]
//
// --as join: connect to a host, send the recorded HELLO (or a fresh one), wait
//   for WELCOME, then send what the recorded join sent - from a HOST-side
//   recording the DIR_IN records of peer N after that peer was welcomed, from a
//   JOIN-side recording its DIR_OUT records after its WELCOME.
// --as host: listen, answer HELLO with WELCOME (playerId from the recording)
//   and TIME_PING with TIME_PONG, then send what the recorded host sent - from
//   a JOIN-side recording the DIR_IN records (peer 0) after its WELCOME, from a
//   HOST-side recording the DIR_OUT records to peer N or broadcast.
// Relative timing (scaled by --speed; 0 = as fast as possible), channel and
// reliability are preserved. Everything the other side sends is drained and
// counted. C++03 / VC++2010.

#include <enet/enet.h> // first: pulls winsock2 before windows.h
#include "kcrec.h"

#include <windows.h>

using namespace kcrec;
using namespace coop;

namespace {

// Must equal NetLink.cpp's CH_COUNT / channel ids: a connection negotiates
// min(both sides') channel count, and enet_peer_send refuses a packet whose
// channel id is >= that count - recorded CH_BULK traffic would vanish.
const int        CH_COUNT      = 3;
const enet_uint8 CH_RELIABLE   = 0;
const enet_uint8 CH_UNRELIABLE = 1;

volatile LONG g_stop = 0;

BOOL WINAPI onCtrl(DWORD) { InterlockedExchange(&g_stop, 1); return TRUE; }

double nowSec() {
    static LARGE_INTEGER f = { 0 };
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// Same clock NetLink stamps TIME_PONG with (coop::wallClockMs, no skew):
// local time of day in ms.
u32 wallClockMs() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return (u32)((((u32)st.wHour * 60u + st.wMinute) * 60u + st.wSecond) * 1000u + st.wMilliseconds);
}

struct Options {
    const char* file;
    bool        asHost;
    bool        roleSet;
    const char* hostIp;
    int         port;
    int         peer;       // -1 = from the recording
    double      speed;
    bool        skip[256];
    double      linger;     // <0 = role default
    double      wait;       // <0 = role default
};

int usage() {
    std::fprintf(stderr,
        "usage: kcreplay <file.kcrec> --as join [--host IP] [--port P] [--peer N]\n"
        "                [--speed F] [--skip-types 29-36] [--linger S] [--wait S]\n"
        "       kcreplay <file.kcrec> --as host [--port P] [--peer N] [--speed F]\n"
        "                [--skip-types 29-36] [--linger S] [--wait S]\n"
        "  --speed 0 = no pacing; --linger: seconds to stay after the stream ends\n"
        "  (join default 2; host default 0 = until the join disconnects);\n"
        "  --wait: connect/handshake timeout (join 10 s, host 120 s)\n");
    return 2;
}

// ---- selection: which records this side re-sends ----------------------------------
struct Plan {
    std::vector<size_t> stream;   // record indices, in file order
    int                 helloIdx; // recorded HELLO to send (join role), -1 = fresh
    u32                 playerId; // WELCOME playerId (host role)
    int                 peer;     // recorded peer whose traffic is replayed
};

bool welcomeId(const Recording& rec, const Record& r, u32* id) {
    WelcomePacket w;
    if (rec.typeOf(r) != PKT_WELCOME || !readPacket(rec.data(r), r.len, &w)) return false;
    *id = w.playerId;
    return true;
}

bool buildPlan(const Recording& rec, const Options& o, Plan* p, std::string* err) {
    p->stream.clear();
    p->helloIdx = -1;
    p->playerId = 1;
    p->peer = o.peer;
    const bool hostRec = rec.header.isHost != 0;
    const std::vector<Record>& rs = rec.records;

    // Where the replayed peer's session starts: its (first) WELCOME.
    size_t start = 0;
    bool   found = false;
    int    lastHello = -1;
    for (size_t i = 0; i < rs.size() && !found; ++i) {
        const Record& r = rs[i];
        const u8 t = rec.typeOf(r);
        u32 id = 0;
        if (hostRec) {
            // Host side: the HELLO arrives as peer 0 (not yet welcomed) and is
            // answered by a WELCOME OUT to the newly assigned id.
            if (r.dir == DIR_IN && t == PKT_HELLO) lastHello = (int)i;
            if (r.dir == DIR_OUT && welcomeId(rec, r, &id) && (o.peer < 0 || id == (u32)o.peer)) {
                found = true; start = i + 1;
                p->peer = (int)id; p->playerId = id; p->helloIdx = lastHello;
            }
        } else {
            if (r.dir == DIR_OUT && t == PKT_HELLO) lastHello = (int)i;
            if (r.dir == DIR_IN && welcomeId(rec, r, &id)) {
                found = true; start = i + 1;
                p->peer = 0; p->playerId = id; p->helloIdx = lastHello;
            }
        }
    }
    if (!found) {
        if (hostRec && o.peer < 0) { *err = "no WELCOME in the recording: pass --peer N"; return false; }
        if (!hostRec) p->peer = 0;
        start = 0; // recording began mid-session: replay everything
        std::fprintf(stderr, "kcreplay: no WELCOME for the selected peer in the recording; "
                             "replaying from the first record\n");
    }

    // join role: what the recorded JOIN sent; host role: what the HOST sent.
    for (size_t i = start; i < rs.size(); ++i) {
        const Record& r = rs[i];
        if (r.len == 0) continue;
        const u8 t = rec.typeOf(r);
        if (t == PKT_HELLO || t == PKT_WELCOME || o.skip[t]) continue;
        bool take;
        if (!o.asHost) take = hostRec ? (r.dir == DIR_IN && r.peerId == (u8)p->peer)
                                      : (r.dir == DIR_OUT);
        else           take = hostRec ? (r.dir == DIR_OUT && (r.peerId == (u8)p->peer || r.peerId == PEER_ANY))
                                      : (r.dir == DIR_IN);
        // The live host answers clock probes itself; a recorded pong would
        // feed the join a stale clock sample.
        if (o.asHost && t == PKT_TIME_PONG) take = false;
        if (take) p->stream.push_back(i);
    }
    return true;
}

// ---- runtime ------------------------------------------------------------------------
struct Stats {
    unsigned recvTotal;
    unsigned recvByType[256];
    unsigned sent;
    double   maxLateMs;
    double   sumLateMs;
};

void sendRaw(ENetPeer* peer, enet_uint8 ch, const void* data, size_t len, bool reliable) {
    ENetPacket* p = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (p && enet_peer_send(peer, ch, p) < 0) enet_packet_destroy(p);
}

// Host role: the replies a NetLink host owes a join (PONG; WELCOME is handled
// in the handshake). Join role: nothing - the host's traffic is only counted.
void onReceive(const ENetEvent& ev, bool asHost, Stats* st) {
    const u8 t = packetType(ev.packet->data, (unsigned)ev.packet->dataLength);
    ++st->recvTotal;
    ++st->recvByType[t];
    if (asHost && t == PKT_TIME_PING) {
        TimePingPacket tp;
        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &tp)) {
            TimePongPacket po;
            po.type = (u8)PKT_TIME_PONG;
            po.nonce = tp.nonce;
            po.echoWallMs = tp.senderWallMs;
            po.responderWallMs = wallClockMs();
            sendRaw(ev.peer, CH_UNRELIABLE, &po, sizeof(po), false);
        }
    }
}

void printRecv(const Stats& st) {
    std::printf("kcreplay: received %u packets:", st.recvTotal);
    for (int t = 0; t < 256; ++t)
        if (st.recvByType[t]) std::printf(" %s=%u", typeName((u8)t), st.recvByType[t]);
    std::printf("\n");
}

// Replay-clock time (ms) a record is due; never before the stream start.
double dueMs(const Record& r, double t0, double speed) {
    if (speed <= 0.0) return 0.0;
    const double d = ((double)r.tMs - t0) / speed;
    return d > 0.0 ? d : 0.0;
}

// Send the planned stream to 'target' with recorded pacing, servicing ENet in
// between. Returns once the stream is done and the linger rule is met, the
// target disconnects, or Ctrl-C.
void replay(ENetHost* host, ENetPeer* target, const Recording& rec, const Plan& plan,
            const Options& o, double linger, Stats* st) {
    const std::vector<Record>& rs = rec.records;
    const double t0 = plan.stream.empty() ? 0.0 : (double)rs[plan.stream[0]].tMs;
    double spanMs = 0.0;
    for (size_t i = 0; i < plan.stream.size(); ++i)
        if ((double)rs[plan.stream[i]].tMs - t0 > spanMs) spanMs = (double)rs[plan.stream[i]].tMs - t0;
    const double span = spanMs;
    std::printf("kcreplay: replaying %u records spanning %.1f s at speed %.2f\n",
                (unsigned)plan.stream.size(), span / 1000.0, o.speed);
    std::fflush(stdout);

    const double start = nowSec();
    double lastProgress = start;
    double doneAt = -1.0;
    bool   gone = false;
    size_t next = 0;
    while (!g_stop) {
        const double el = (nowSec() - start) * 1000.0; // ms since replay start
        while (next < plan.stream.size()) {
            const Record& r = rs[plan.stream[next]];
            const double due = dueMs(r, t0, o.speed);
            if (due > el) break;
            enet_uint8 ch = r.channel < CH_COUNT ? r.channel : CH_RELIABLE;
            sendRaw(target, ch, rec.data(r), r.len, (r.flags & 1) != 0);
            const double late = el - due;
            if (late > st->maxLateMs) st->maxLateMs = late;
            st->sumLateMs += late;
            ++st->sent;
            ++next;
        }
        const double now = nowSec();
        if (now - lastProgress >= 5.0) {
            lastProgress = now;
            std::printf("kcreplay: %6.1f s  sent %u/%u  received %u\n", now - start,
                        st->sent, (unsigned)plan.stream.size(), st->recvTotal);
            std::fflush(stdout);
        }
        if (next == plan.stream.size() && doneAt < 0.0) {
            doneAt = now;
            std::printf("kcreplay: stream done at %.3f s (recorded span %.3f s)\n",
                        now - start, span / 1000.0);
            std::fflush(stdout);
        }
        if (gone) break;
        if (doneAt >= 0.0 && linger > 0.0 && now - doneAt >= linger) break;

        // Sleep in ENet until the next record is due (<= 5 ms so the progress
        // and stop checks stay responsive).
        enet_uint32 waitMs = 5;
        if (next < plan.stream.size() && o.speed > 0.0) {
            double due = dueMs(rs[plan.stream[next]], t0, o.speed) - (nowSec() - start) * 1000.0;
            if (due < 0.0) due = 0.0;
            if (due < waitMs) waitMs = (enet_uint32)due;
        } else if (next < plan.stream.size()) {
            waitMs = 0;
        }
        ENetEvent ev;
        int rc = enet_host_service(host, &ev, waitMs);
        while (rc > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                onReceive(ev, o.asHost, st);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT && ev.peer == target) {
                gone = true;
                std::printf("kcreplay: peer disconnected\n");
            }
            rc = enet_host_check_events(host, &ev);
        }
        if (rc < 0) { std::fprintf(stderr, "kcreplay: enet_host_service failed\n"); break; }
    }

    const double total = nowSec() - start;
    std::printf("kcreplay: sent %u/%u records in %.3f s (recorded span %.3f s); "
                "lateness avg %.2f ms max %.2f ms\n",
                st->sent, (unsigned)plan.stream.size(), total, span / 1000.0,
                st->sent ? st->sumLateMs / st->sent : 0.0, st->maxLateMs);
    if (!gone) {
        // Graceful close so the other side logs a DISCONNECT, not a timeout.
        enet_peer_disconnect_later(target, 0);
        const double until = nowSec() + 2.0;
        ENetEvent ev;
        while (nowSec() < until) {
            int rc = enet_host_service(host, &ev, 20);
            if (rc < 0) break;
            if (rc == 0) continue;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) { onReceive(ev, o.asHost, st); enet_packet_destroy(ev.packet); }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT && ev.peer == target) break;
        }
    }
    printRecv(*st);
}

// ---- join role ----------------------------------------------------------------------
int runJoin(const Recording& rec, const Plan& plan, const Options& o) {
    ENetHost* client = enet_host_create(0, 1, CH_COUNT, 0, 0);
    if (!client) { std::fprintf(stderr, "kcreplay: client create failed\n"); return 1; }
    ENetAddress addr;
    if (enet_address_set_host(&addr, o.hostIp) != 0) {
        std::fprintf(stderr, "kcreplay: cannot resolve %s\n", o.hostIp);
        return 1;
    }
    addr.port = (enet_uint16)o.port;
    ENetPeer* peer = enet_host_connect(client, &addr, CH_COUNT, 0);
    if (!peer) { std::fprintf(stderr, "kcreplay: connect failed\n"); return 1; }
    std::printf("kcreplay: join connecting to %s:%d (replaying recorded peer %d)\n",
                o.hostIp, o.port, plan.peer);
    std::fflush(stdout);

    Stats st;
    std::memset(&st, 0, sizeof(st));
    const double wait = o.wait >= 0.0 ? o.wait : 10.0;
    const double deadline = nowSec() + wait;
    bool connected = false, welcomed = false;
    while (!g_stop && !welcomed && nowSec() < deadline) {
        ENetEvent ev;
        int rc = enet_host_service(client, &ev, 20);
        if (rc < 0) break;
        if (rc == 0) continue;
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
            if (plan.helloIdx >= 0) {
                const Record& h = rec.records[plan.helloIdx];
                HelloPacket hp;
                if (readPacket(rec.data(h), h.len, &hp) && hp.version != PROTOCOL_VERSION)
                    std::printf("kcreplay: note: recorded HELLO is protocol v%u, this tool v%u\n",
                                (unsigned)hp.version, (unsigned)PROTOCOL_VERSION);
                sendRaw(peer, CH_RELIABLE, rec.data(h), h.len, true);
                std::printf("kcreplay: connected; sent recorded HELLO (%u bytes)\n", h.len);
            } else {
                HelloPacket hp;
                hp.type = (u8)PKT_HELLO; hp.version = PROTOCOL_VERSION; hp.nameLen = 0;
                sendRaw(peer, CH_RELIABLE, &hp, sizeof(hp), true);
                std::printf("kcreplay: connected; sent fresh HELLO v%u\n", (unsigned)PROTOCOL_VERSION);
            }
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            WelcomePacket w;
            if (packetType(ev.packet->data, (unsigned)ev.packet->dataLength) == PKT_WELCOME &&
                readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &w)) {
                welcomed = true;
                std::printf("kcreplay: WELCOME playerId=%u version=%u%s\n", w.playerId,
                            (unsigned)w.version,
                            w.playerId != (u32)plan.peer ? " (differs from the recorded id: "
                                                           "ownerId fields are replayed verbatim)" : "");
            }
            onReceive(ev, false, &st);
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            std::fprintf(stderr, "kcreplay: host refused/disconnected during handshake "
                                 "(protocol mismatch or session full?)\n");
            enet_host_destroy(client);
            return 1;
        }
    }
    if (!welcomed) {
        std::fprintf(stderr, "kcreplay: %s within %.0f s\n",
                     connected ? "no WELCOME" : "could not connect", wait);
        enet_host_destroy(client);
        return 1;
    }
    std::fflush(stdout);
    replay(client, peer, rec, plan, o, o.linger >= 0.0 ? o.linger : 2.0, &st);
    enet_host_destroy(client);
    return 0;
}

// ---- host role ----------------------------------------------------------------------
int runHost(const Recording& rec, const Plan& plan, const Options& o) {
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = (enet_uint16)o.port;
    ENetHost* server = enet_host_create(&addr, 8, CH_COUNT, 0, 0);
    if (!server) { std::fprintf(stderr, "kcreplay: host create failed (port %d in use?)\n", o.port); return 1; }
    std::printf("kcreplay: host listening on port %d (WELCOME playerId=%u)\n", o.port, plan.playerId);
    std::fflush(stdout);

    Stats st;
    std::memset(&st, 0, sizeof(st));
    const double wait = o.wait >= 0.0 ? o.wait : 120.0;
    const double deadline = nowSec() + wait;
    ENetPeer* join = 0;
    while (!g_stop && !join && nowSec() < deadline) {
        ENetEvent ev;
        int rc = enet_host_service(server, &ev, 20);
        if (rc < 0) break;
        if (rc == 0) continue;
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            std::printf("kcreplay: peer connecting (awaiting HELLO)\n");
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            HelloPacket h;
            const unsigned len = (unsigned)ev.packet->dataLength;
            if (packetType(ev.packet->data, len) == PKT_HELLO && readPacket(ev.packet->data, len, &h)) {
                std::printf("kcreplay: HELLO version=%u identity=%.*s\n", (unsigned)h.version,
                            (int)(h.nameLen <= len - sizeof(h) ? h.nameLen : 0),
                            (const char*)ev.packet->data + sizeof(h));
                WelcomePacket w;
                w.type = (u8)PKT_WELCOME; w.version = PROTOCOL_VERSION; w.playerId = plan.playerId;
                sendRaw(ev.peer, CH_RELIABLE, &w, sizeof(w), true);
                std::printf("kcreplay: sent WELCOME playerId=%u\n", plan.playerId);
                join = ev.peer;
            }
            onReceive(ev, true, &st);
            enet_packet_destroy(ev.packet);
        }
    }
    if (!join) {
        std::fprintf(stderr, "kcreplay: no join completed HELLO within %.0f s\n", wait);
        enet_host_destroy(server);
        return 1;
    }
    std::fflush(stdout);
    replay(server, join, rec, plan, o, o.linger >= 0.0 ? o.linger : 0.0, &st);
    enet_host_destroy(server);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    o.file = 0; o.asHost = false; o.roleSet = false; o.hostIp = "127.0.0.1";
    o.port = 27800; o.peer = -1; o.speed = 1.0; o.linger = -1.0; o.wait = -1.0;
    for (int i = 0; i < 256; ++i) o.skip[i] = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasV = i + 1 < argc;
        if (!std::strcmp(a, "--as") && hasV) {
            const char* v = argv[++i];
            if (!std::strcmp(v, "host")) o.asHost = true;
            else if (!std::strcmp(v, "join")) o.asHost = false;
            else return usage();
            o.roleSet = true;
        }
        else if (!std::strcmp(a, "--host") && hasV)  o.hostIp = argv[++i];
        else if (!std::strcmp(a, "--port") && hasV)  o.port = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--peer") && hasV)  o.peer = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--speed") && hasV) o.speed = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--linger") && hasV) o.linger = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--wait") && hasV)  o.wait = std::atof(argv[++i]);
        else if (!std::strcmp(a, "--skip-types") && hasV) {
            if (!parseTypeList(argv[++i], o.skip)) return usage();
        }
        else if (a[0] == '-' && a[1] == '-') return usage();
        else if (!o.file) o.file = a;
        else return usage();
    }
    if (!o.file || !o.roleSet || o.port <= 0 || o.port > 65535 || o.speed < 0.0) return usage();

    Recording rec;
    std::string err;
    if (!load(o.file, &rec, &err)) {
        std::fprintf(stderr, "kcreplay: %s: %s\n", o.file, err.c_str());
        return 1;
    }
    std::printf("kcreplay: %s: %s-side recording, protocol %u, %u records%s\n", o.file,
                rec.header.isHost ? "host" : "join", rec.header.protocol,
                (unsigned)rec.records.size(), rec.truncated ? " (truncated)" : "");
    if (rec.header.protocol != PROTOCOL_VERSION)
        std::printf("kcreplay: note: recording protocol v%u, tool v%u - a live peer will "
                    "decode the payloads as v%u\n", rec.header.protocol,
                    (unsigned)PROTOCOL_VERSION, (unsigned)PROTOCOL_VERSION);
    Plan plan;
    if (!buildPlan(rec, o, &plan, &err)) {
        std::fprintf(stderr, "kcreplay: %s\n", err.c_str());
        return 1;
    }

    if (enet_initialize() != 0) { std::fprintf(stderr, "kcreplay: enet_initialize failed\n"); return 1; }
    SetConsoleCtrlHandler(onCtrl, TRUE);
    const int rc = o.asHost ? runHost(rec, plan, o) : runJoin(rec, plan, o);
    enet_deinitialize();
    return rc;
}
