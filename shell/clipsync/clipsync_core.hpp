#pragma once
// Clipboard sync with a phone over Bluetooth, without Qt: the wire format and
// the rules both ends follow. The phone's module (atrium-clipsync-ksu) implements the same
// thing in Java; change one, change both.
//
// The phone listens on an RFCOMM channel under kServiceUuid; atrium connects
// when the phone's Bluetooth connection comes up. Every message is a frame:
//   u32 length (big endian, of what follows), u8 type, body
// HELLO   u16 version, u32 biggest clip it takes (bytes), name (UTF-8)
// CLIP    i64 time (ms since the epoch), u8 flags, u16 mime length, mime, data
// SYNCED  nothing: the sender has sent its history
//
// On connecting both send HELLO. Once the other's HELLO is in (its limit
// decides which clips fit), each sends its kHistory newest clips (oldest
// first, flag History) and SYNCED. With both lists in, each end adds the
// other's clips it lacks to its history, and the newest clip of all becomes
// both clipboards. From then on every new copy on either end goes over as a
// CLIP flagged Live and becomes the other's clipboard.
//
// Text is always "text/plain;charset=utf-8" on the wire. A clip never goes
// over when it is bigger than the smaller of the two ends' limits.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::clipsync {

inline constexpr std::string_view kServiceUuid = "7a1e5c0d-5ca1-4c1b-9d2e-a7c1195c0de1";
inline constexpr std::uint16_t kVersion = 1;
inline constexpr int kHistory = 5;
inline constexpr std::uint32_t kMaxClip = 10u << 20;  // what this end takes
inline constexpr std::string_view kText = "text/plain;charset=utf-8";

enum class Type : std::uint8_t { Hello = 1, Clip = 2, Synced = 3 };
enum Flags : std::uint8_t { History = 1, Live = 2 };

struct Clip {
    std::string mime;
    std::string data;
    std::int64_t time = 0;  // ms since the epoch; 0 when not known

    // FNV-1a over mime, a zero byte, data: the same on both ends.
    std::uint64_t hash() const;
};

struct Message {
    Type type{};
    // Hello
    std::uint16_t version = 0;
    std::uint32_t maxClip = 0;
    std::string name;
    // Clip
    std::uint8_t flags = 0;
    Clip clip;
};

std::string helloFrame(std::string_view name, std::uint32_t maxClip = kMaxClip);
std::string clipFrame(const Clip& c, std::uint8_t flags);
std::string syncedFrame();

// Cuts a byte stream into messages. A frame bigger than `limit` or one that
// doesn't parse breaks the reader for good: the connection is to be dropped.
class Reader {
public:
    explicit Reader(std::uint32_t limit = kMaxClip) : limit_(limit) {}
    void feed(std::string_view bytes) { buffer_.append(bytes); }
    std::optional<Message> next();
    bool broken() const { return broken_; }

private:
    std::string buffer_;
    std::uint32_t limit_;
    bool broken_ = false;
};

// What the session wants done.
struct Action {
    enum class Kind { Send, SetClipboard, AddHistory, Close } kind;
    std::string bytes;  // Send: a frame
    Clip clip;          // SetClipboard, AddHistory
};

// One connection's rules, fed events, answering with actions. It never
// touches the clipboard itself, so the same rules are testable anywhere.
class Session {
public:
    // `recent`: this end's newest clips, newest first (more than kHistory is fine).
    Session(std::string name, std::vector<Clip> recent, std::uint32_t maxClip = kMaxClip);

    // What to send on connecting.
    std::vector<Action> start();
    // Bytes from the peer.
    std::vector<Action> received(std::string_view bytes);
    // The user copied something here. Clips this end put in the clipboard or
    // history itself (SetClipboard, AddHistory) must not come back as copies.
    std::vector<Action> copied(const Clip& c);

    bool synced() const { return phase_ == Phase::Live; }
    const std::string& peerName() const { return peerName_; }

private:
    enum class Phase { Hello, History, Live };

    std::vector<Action> handle(Message m);
    std::vector<Action> merge();
    std::uint32_t limit() const;

    std::string name_, peerName_;
    std::vector<Clip> recent_;      // ours, newest first
    std::optional<Clip> current_;  // what our clipboard holds
    std::vector<Clip> theirs_;      // their history, as it came (oldest first)
    std::uint32_t maxClip_, peerMax_ = 0;
    Phase phase_ = Phase::Hello;
    bool helloSeen_ = false;
    Reader reader_;
    // The clip the peer's clipboard holds, as far as we know: a copy of it
    // here is not news.
    std::optional<std::uint64_t> peerCurrent_;
    // Copies made before the history exchange finished, sent after it.
    std::vector<Clip> early_;
};

} // namespace atrium::clipsync
