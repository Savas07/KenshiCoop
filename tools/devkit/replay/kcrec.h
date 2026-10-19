// kcrec.h - shared .kcrec reader for kcrecdump / kcreplay.
// File format: src/plugin/devtools/PacketRecorder.h (writer: PacketRecorder.cpp).
// The two FileHeader/RecordHeader structs below MUST stay byte-identical to the
// writer's. C++03 / VC++2010.

#ifndef KENSHICOOP_KCREC_H
#define KENSHICOOP_KCREC_H

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "Wire.h" // src/netproto (packet structs, PKT_*, PROTOCOL_VERSION)

namespace kcrec {

using coop::u8;
using coop::u16;
using coop::u32;

const u8 DIR_IN   = 0;
const u8 DIR_OUT  = 1;
const u8 PEER_ANY = 255;

#pragma pack(push, 1)
struct FileHeader {
    char               magic[8];     // "KCREC01\0"
    u32                headerSize;
    u32                protocol;
    u8                 isHost;
    u8                 pad[3];
    unsigned long long startWallMs;  // UTC ms since 1970
    char               version[32];
    char               note[64];
};
struct RecordHeader {
    u32 tMs;
    u8  dir;
    u8  peerId;
    u8  channel;
    u8  flags;   // bit0 = reliable
    u32 len;
};
#pragma pack(pop)

struct Record {
    u32 tMs;
    u8  dir;
    u8  peerId;
    u8  channel;
    u8  flags;
    u32 len;
    size_t off;  // payload offset into Recording::bytes
};

struct Recording {
    FileHeader          header;
    std::vector<u8>     bytes;    // every record payload, back to back
    std::vector<Record> records;
    bool                truncated; // file ended inside a record (crash / still being written)

    const u8* data(const Record& r) const { return r.len ? &bytes[r.off] : 0; }
    u8 typeOf(const Record& r) const { return r.len ? bytes[r.off] : (u8)0; }
};

// Load the whole file. Returns false (and fills err) when it is not a .kcrec.
inline bool load(const char* path, Recording* out, std::string* err) {
    out->records.clear();
    out->bytes.clear();
    out->truncated = false;
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) { *err = "cannot open file"; return false; }
    std::memset(&out->header, 0, sizeof(out->header));
    // headerSize lets a newer writer append header fields: read the fixed
    // prefix, then skip whatever the file says the header occupies.
    if (std::fread(&out->header, sizeof(FileHeader), 1, fp) != 1 ||
        std::memcmp(out->header.magic, "KCREC01", 8) != 0) {
        std::fclose(fp);
        *err = "not a KCREC01 file";
        return false;
    }
    if (out->header.headerSize < sizeof(FileHeader)) {
        std::fclose(fp);
        *err = "bad headerSize";
        return false;
    }
    if (out->header.headerSize > sizeof(FileHeader))
        std::fseek(fp, (long)(out->header.headerSize - sizeof(FileHeader)), SEEK_CUR);
    out->header.version[sizeof(out->header.version) - 1] = '\0';
    out->header.note[sizeof(out->header.note) - 1] = '\0';

    for (;;) {
        RecordHeader rh;
        size_t got = std::fread(&rh, 1, sizeof(rh), fp);
        if (got == 0) break;
        if (got != sizeof(rh) || rh.len > 64u * 1024u * 1024u) { out->truncated = true; break; }
        Record r;
        r.tMs = rh.tMs; r.dir = rh.dir; r.peerId = rh.peerId;
        r.channel = rh.channel; r.flags = rh.flags; r.len = rh.len;
        r.off = out->bytes.size();
        if (rh.len > 0) {
            out->bytes.resize(r.off + rh.len);
            if (std::fread(&out->bytes[r.off], 1, rh.len, fp) != rh.len) {
                out->bytes.resize(r.off);
                out->truncated = true;
                break;
            }
        }
        out->records.push_back(r);
    }
    std::fclose(fp);
    return true;
}

inline const char* typeName(u8 t) {
    static const char* const names[] = {
        "NONE", "HELLO", "WELCOME", "LEAVE", "ENTITY_BATCH", "EVENT", "INV_SNAPSHOT",
        "WORLD_ITEM", "WORLD_ITEM_REMOVE", "WORLD_DROP", "WORLD_PICKUP", "TIME_PING",
        "TIME_PONG", "MEDICAL", "TREATMENT", "SPEED_REQ", "SPEED_SET", "STATS",
        "STEALTH", "SPAWN_REQ", "SPAWN_INFO", "MONEY", "FACTION", "TIME", "DOOR",
        "BUILD_PLACE", "BUILD_STATE", "BUILD_DOOR", "BUILD_REMOVE", "SAVE_REQ",
        "SAVE_BEGIN", "SAVE_FILE", "SAVE_DONE", "SAVE_ACK", "LOAD_GO", "LOAD_REQ",
        "LOAD_NACK", "PROD", "NPC_CENSUS", "INV_XFER", "RESEARCH", "CAM_HINT",
        "COMBAT_HIT", "WORLD_ITEM_CLAIM", "CELL_CLAIM", "INV_XFER_ACK", "MONEY_DELTA",
        "DEED", "FIXTURE", "PEER_STATE", "CHARGEN", "ROSTER"
    };
    return t < sizeof(names) / sizeof(names[0]) ? names[t] : "UNKNOWN";
}

// Parse "4,5,29-36" into a 256-entry membership table. Returns false on junk.
inline bool parseTypeList(const char* s, bool table[256]) {
    for (int i = 0; i < 256; ++i) table[i] = false;
    if (!s || !*s) return false;
    const char* p = s;
    while (*p) {
        char* end = 0;
        long a = std::strtol(p, &end, 10);
        if (end == p || a < 0 || a > 255) return false;
        long b = a;
        p = end;
        if (*p == '-') {
            ++p;
            b = std::strtol(p, &end, 10);
            if (end == p || b < a || b > 255) return false;
            p = end;
        }
        for (long k = a; k <= b; ++k) table[k] = true;
        if (*p == ',') ++p;
        else if (*p) return false;
    }
    return true;
}

// Read a little-endian u32 at an arbitrary (possibly unaligned) offset.
inline u32 rdU32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }

} // namespace kcrec

#endif // KENSHICOOP_KCREC_H
