// kcrecdump - print a .kcrec packet recording as JSON lines.
//
//   kcrecdump <file.kcrec> [--peer N] [--dir in|out] [--types 4,5,29-36]
//
// Line 1 is the file header ({"k":"header",...}); every following line is one
// record: {"t":ms,"dir":"in|out","peer":n,"ch":n,"rel":0|1,"type":n,"name":"..",
// "len":n, ...decoded fields}. A per-type summary goes to stderr so stdout stays
// pipeable into jq / python. C++03 / VC++2010.

#include "kcrec.h"

#include <float.h>
#include <ctime>

using namespace kcrec;
using namespace coop;

namespace {

// ---- JSON output helpers --------------------------------------------------------
void jstr(const char* s, size_t n) {
    std::putchar('"');
    for (size_t i = 0; i < n && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { std::putchar('\\'); std::putchar(c); }
        else if (c < 0x20)        std::printf("\\u%04x", (unsigned)c);
        else                      std::putchar(c);
    }
    std::putchar('"');
}
void jstr(const char* s) { jstr(s, (size_t)-1); }

// Floats: 3 decimals (positions are metres-scale world units); NaN/inf would
// make the line invalid JSON, so they print as null.
void jf(f32 v) {
    if (!_finite((double)v)) std::printf("null");
    else                     std::printf("%.3f", (double)v);
}

void jhand(u32 index, u32 serial) { std::printf("\"%u,%u\"", index, serial); }

const char* eventName(u8 e) {
    static const char* const names[] = {
        "NONE", "KNOCKOUT", "DEATH", "REVIVE", "AMPUTATE", "CRUSH", "PICKUP_BODY",
        "DROP_BODY", "ENTER_FURNITURE", "EXIT_FURNITURE", "RECRUIT", "SQUAD_MOVE"
    };
    return e < sizeof(names) / sizeof(names[0]) ? names[e] : "UNKNOWN";
}

// ---- per-type decoders (each appends ,"field":value pairs) ----------------------
void decodeBatch(const u8* d, u32 len) {
    EntityBatchHeader h;
    if (!readPacket(d, len, &h)) { std::printf(",\"bad\":\"short header\""); return; }
    std::printf(",\"owner\":%u,\"sendMs\":%u,\"epoch\":%u,\"count\":%u",
                h.ownerId, h.sendMs, h.epoch, (unsigned)h.count);
    const unsigned avail = (unsigned)((len - sizeof(h)) / sizeof(EntityState));
    const unsigned n = h.count < avail ? h.count : avail;
    if (n < h.count) std::printf(",\"bad\":\"%u of %u entities present\"", n, (unsigned)h.count);
    std::printf(",\"entities\":[");
    for (unsigned i = 0; i < n; ++i) {
        EntityState e;
        std::memcpy(&e, d + sizeof(h) + i * sizeof(EntityState), sizeof(e));
        if (i) std::putchar(',');
        std::printf("{\"hand\":");     jhand(e.hIndex, e.hSerial);
        std::printf(",\"x\":");        jf(e.x);
        std::printf(",\"y\":");        jf(e.y);
        std::printf(",\"z\":");        jf(e.z);
        std::printf(",\"heading\":");  jf(e.heading);
        std::printf(",\"bs\":%u,\"down\":%d,\"task\":%u,\"rawTask\":%u",
                    (unsigned)e.bodyState, bodyIsDown(e.bodyState) ? 1 : 0,
                    (unsigned)e.task, (unsigned)e.rawTask);
        std::printf(",\"cSpeed\":");   jf(e.cSpeed);
        std::printf(",\"cMotion\":["); jf(e.cMotionX); std::putchar(','); jf(e.cMotionY);
        std::putchar(',');             jf(e.cMotionZ);
        std::printf("],\"cMoving\":%u,\"subj\":", (unsigned)e.cMoving);
        jhand(e.sIndex, e.sSerial);
        std::printf(",\"htype\":%u,\"hcont\":\"%u,%u\"}", e.hType, e.hContainer, e.hContainerSerial);
    }
    std::putchar(']');
}

void decodeEvent(const u8* d, u32 len) {
    EventPacket e;
    if (!readPacket(d, len, &e)) { std::printf(",\"bad\":\"short\""); return; }
    std::printf(",\"event\":%u,\"ev\":\"%s\",\"owner\":%u,\"eventId\":%u,\"subject\":",
                (unsigned)e.event, eventName(e.event), e.ownerId, e.eventId);
    jhand(e.sIndex, e.sSerial);
    std::printf(",\"actor\":");
    jhand(e.aIndex, e.aSerial);
    std::printf(",\"arg\":");
    jf(e.arg);
}

void decode(u8 type, const u8* d, u32 len) {
    switch (type) {
    case PKT_HELLO: {
        HelloPacket h;
        if (!readPacket(d, len, &h)) break;
        std::printf(",\"version\":%u,\"identity\":", (unsigned)h.version);
        unsigned n = h.nameLen;
        if (sizeof(h) + n > len) n = len - (unsigned)sizeof(h);
        jstr((const char*)d + sizeof(h), n);
        break;
    }
    case PKT_WELCOME: {
        WelcomePacket w;
        if (readPacket(d, len, &w))
            std::printf(",\"version\":%u,\"playerId\":%u", (unsigned)w.version, w.playerId);
        break;
    }
    case PKT_ENTITY_BATCH: decodeBatch(d, len); break;
    case PKT_EVENT:        decodeEvent(d, len); break;
    case PKT_TIME_PING: {
        TimePingPacket p;
        if (readPacket(d, len, &p))
            std::printf(",\"nonce\":%u,\"senderWallMs\":%u", p.nonce, p.senderWallMs);
        break;
    }
    case PKT_TIME_PONG: {
        TimePongPacket p;
        if (readPacket(d, len, &p))
            std::printf(",\"nonce\":%u,\"echoWallMs\":%u,\"responderWallMs\":%u",
                        p.nonce, p.echoWallMs, p.responderWallMs);
        break;
    }
    case PKT_PEER_STATE: {
        PeerStatePacket p;
        if (readPacket(d, len, &p))
            std::printf(",\"peerId\":%u,\"present\":%u", p.peerId, (unsigned)p.present);
        break;
    }
    case PKT_ROSTER: {
        RosterPacket p;
        if (!readPacket(d, len, &p)) break;
        std::printf(",\"playerId\":%u,\"steamId\":\"%llu\",\"pname\":", p.playerId, p.steamId);
        jstr(p.name, sizeof(p.name));
        break;
    }
    case PKT_CAM_HINT: {
        CamHintPacket p;
        if (!readPacket(d, len, &p)) break;
        std::printf(",\"owner\":%u,\"cam\":[", p.ownerId);
        jf(p.x); std::putchar(','); jf(p.y); std::putchar(','); jf(p.z);
        std::putchar(']');
        break;
    }
    case PKT_LEAVE:
        break;
    default:
        // Every other packet starts {u8 type; u32 ownerId; ...} (Wire.h).
        if (len >= 5) std::printf(",\"owner\":%u", rdU32(d + 1));
        break;
    }
}

int usage() {
    std::fprintf(stderr,
        "usage: kcrecdump <file.kcrec> [--peer N] [--dir in|out] [--types 4,5,29-36]\n"
        "  prints the header then one JSON line per record (filters AND together)\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const char* path = 0;
    int  peer = -1;      // -1 = any
    int  dir  = -1;      // -1 = both
    bool types[256];
    bool typeFilter = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--peer") && i + 1 < argc) {
            peer = std::atoi(argv[++i]);
        } else if (!std::strcmp(a, "--dir") && i + 1 < argc) {
            const char* v = argv[++i];
            if (!std::strcmp(v, "in")) dir = DIR_IN;
            else if (!std::strcmp(v, "out")) dir = DIR_OUT;
            else return usage();
        } else if (!std::strcmp(a, "--types") && i + 1 < argc) {
            if (!parseTypeList(argv[++i], types)) return usage();
            typeFilter = true;
        } else if (a[0] == '-' && a[1] == '-') {
            return usage();
        } else if (!path) {
            path = a;
        } else {
            return usage();
        }
    }
    if (!path) return usage();

    Recording rec;
    std::string err;
    if (!load(path, &rec, &err)) {
        std::fprintf(stderr, "kcrecdump: %s: %s\n", path, err.c_str());
        return 1;
    }

    // ---- header line ----
    const FileHeader& h = rec.header;
    char utc[40] = "";
    {
        time_t secs = (time_t)(h.startWallMs / 1000ULL);
        struct tm* g = std::gmtime(&secs);
        if (g) {
            _snprintf(utc, sizeof(utc) - 1, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                      g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour,
                      g->tm_min, g->tm_sec, (unsigned)(h.startWallMs % 1000ULL));
        }
    }
    u32 lastT = 0; // the writer stamps under one lock, but be robust to hand-made files
    for (size_t i = 0; i < rec.records.size(); ++i)
        if (rec.records[i].tMs > lastT) lastT = rec.records[i].tMs;
    std::printf("{\"k\":\"header\",\"file\":");
    jstr(path);
    std::printf(",\"headerSize\":%u,\"protocol\":%u,\"toolProtocol\":%u,\"role\":\"%s\","
                "\"startWallMs\":%llu,\"startUtc\":\"%s\",\"version\":",
                h.headerSize, h.protocol, (unsigned)PROTOCOL_VERSION,
                h.isHost ? "host" : "join", h.startWallMs, utc);
    jstr(h.version, sizeof(h.version));
    std::printf(",\"note\":");
    jstr(h.note, sizeof(h.note));
    std::printf(",\"records\":%u,\"durationMs\":%u,\"truncated\":%d}\n",
                (unsigned)rec.records.size(), lastT, rec.truncated ? 1 : 0);
    if (h.protocol != PROTOCOL_VERSION)
        std::fprintf(stderr, "kcrecdump: warning: recorded protocol %u, tool built for %u "
                             "(struct decodes may be wrong)\n", h.protocol, (unsigned)PROTOCOL_VERSION);

    // ---- records ----
    unsigned shown = 0;
    unsigned countIn[256], countOut[256];
    for (int i = 0; i < 256; ++i) { countIn[i] = 0; countOut[i] = 0; }
    for (size_t i = 0; i < rec.records.size(); ++i) {
        const Record& r = rec.records[i];
        const u8 type = rec.typeOf(r);
        if (peer >= 0 && r.peerId != (u8)peer) continue;
        if (dir >= 0 && r.dir != (u8)dir) continue;
        if (typeFilter && !types[type]) continue;
        ++shown;
        if (r.dir == DIR_IN) ++countIn[type]; else ++countOut[type];
        std::printf("{\"t\":%u,\"dir\":\"%s\",\"peer\":%u,\"ch\":%u,\"rel\":%u,\"type\":%u,"
                    "\"name\":\"%s\",\"len\":%u",
                    r.tMs, r.dir == DIR_IN ? "in" : "out", (unsigned)r.peerId,
                    (unsigned)r.channel, (unsigned)(r.flags & 1), (unsigned)type,
                    typeName(type), r.len);
        if (r.len > 0) decode(type, rec.data(r), r.len);
        std::printf("}\n");
    }

    // ---- summary (stderr) ----
    std::fprintf(stderr, "kcrecdump: %u of %u records shown, %.1f s%s\n", shown,
                 (unsigned)rec.records.size(), lastT / 1000.0,
                 rec.truncated ? " (file truncated mid-record)" : "");
    for (int t = 0; t < 256; ++t) {
        if (countIn[t] || countOut[t])
            std::fprintf(stderr, "  %3d %-18s in %7u  out %7u\n", t, typeName((u8)t),
                         countIn[t], countOut[t]);
    }
    return 0;
}
