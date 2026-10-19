// PacketRecorder.h - record every ENet packet of a network session to a .kcrec
// file (docs/DEVTOOLS.md). Thread-safe: packet() is called from the NetLink
// thread, arm()/disarm() from the main thread.
//
// File format (little endian, packed):
//   header  : char magic[8] = "KCREC01\0"; u32 headerSize; u32 protocol;
//             u8 isHost; u8 pad[3]; u64 startWallMs (UTC ms since 1970);
//             char version[32]; char note[64]
//   records : u32 tMs (since start); u8 dir (0 = received, 1 = sent);
//             u8 peerId (host: the join's player id, 255 = broadcast/unknown;
//             join: 0 = host); u8 channel; u8 flags (bit0 reliable);
//             u32 len; u8 data[len]
// tools/devkit/replay reads it (kcrecdump / kcreplay).

#ifndef KENSHICOOP_PACKET_RECORDER_H
#define KENSHICOOP_PACKET_RECORDER_H

namespace coop {
namespace rec {

const unsigned char DIR_IN  = 0;
const unsigned char DIR_OUT = 1;
const unsigned char PEER_ANY = 255;

// Main thread: sessions started while armed are recorded into dir
// (created if missing) as YYYYMMDD_HHMMSS_<host|join>.kcrec.
void arm(const char* dir, const char* version);
void disarm();
bool armed();

// NetLink thread: session edges. begin() opens a new file when armed.
void sessionBegin(bool isHost, const char* note);
void sessionEnd();

// Main thread (command channel): start/stop now, independent of arm().
bool startNow(const char* dir, bool isHost, const char* version, const char* note);
void stopNow();

// Any thread. Cheap no-op unless a file is open.
bool active();
void packet(unsigned char dir, unsigned int peerId, unsigned char channel,
            unsigned int flags, const void* data, unsigned int len);

// Path of the file being written (empty when none). Main thread.
const char* currentPath();

} // namespace rec
} // namespace coop

#endif // KENSHICOOP_PACKET_RECORDER_H
