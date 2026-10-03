#pragma once
// The link between atrium and a phone on the same network, without Qt: the
// wire format, pairing, authentication and the audio datagrams. The phone's
// module (atrium-clipsync-ksu, Link.java) implements the same thing in Java;
// change one, change both. Both test suites pin the same golden bytes.
//
// The phone listens on TCP and announces itself as _atrium-link._tcp over
// mDNS; atrium connects. Every message is a frame:
//   u32 length (big endian, of what follows), u8 type, body
// Once both ends are authenticated, a frame is
//   u32 length, AES-256-GCM(u8 type, body)
// keyed per direction, the nonce counting the frames sent that way, the
// length bytes as associated data.
//
// HELLO        u16 version, u8 role (0 PC, 1 phone), 16-byte id, name (UTF-8)
// PAIR_COMMIT  SHA-256 of the PC's one-off X25519 public key
// PAIR_KEY     the phone's one-off X25519 public key
// PAIR_REVEAL  the PC's one-off X25519 public key
// PAIR_ACCEPT  nothing: this end's user confirmed the code
// PAIR_REJECT  nothing
// AUTH         16-byte nonce
// PROOF        32-byte HMAC
// UNKNOWN      nothing: no key for the other end (pairing needed), then close
// REFUSED      nothing: the phone isn't taking pairings now, then close
// (sealed from here on)
// AUDIO_START  u16 UDP port to send to, u16 frames per packet
// AUDIO_STOP   nothing
// AUDIO_STATE  u8 state (AudioState), reason (UTF-8)
// MEDIA        (phone) what its media session plays: u8 status (MediaStatus),
//              u8 actions (MediaActions), u32 duration ms, u32 position ms
//              when sent, then title, artist, album, app as u16 length +
//              UTF-8 each, then the cover art (JPEG; may be empty)
// MEDIA_COMMAND (PC) u8 command (MediaCommand), u32 position ms (Seek's)
//
// Both send HELLO. Then the PC goes on with what it knows:
// - It has a key for the phone's id: AUTH. The phone answers AUTH and PROOF
//   when it has a key for the PC's id, else UNKNOWN. The PC checks the
//   proof and sends its own; each end is ready once the other's checks.
// - It doesn't, and the user asked to pair: numeric comparison, the way
//   Bluetooth pairs. COMMIT, KEY, REVEAL (the commitment keeps a man in the
//   middle from fitting his keys to a code: he gets one guess in a million).
//   Both show the same 6-digit code, from the transcript; both users
//   accept; both store the key and the PC carries on with AUTH.
//
// Transcript T = SHA-256("atrium-link pair v1" | pc id | phone id | pc key | phone key)
// code         = T[0..4] as a big-endian u32, mod 1 000 000
// paired key K = HKDF(X25519 secret, salt T, "atrium-link pair v1", 32)
// PROOF        = HMAC(K, "atrium-link proof v1" | role | pc nonce | phone nonce | pc id | phone id)
// session keys = HKDF(K, salt pc nonce | phone nonce, "atrium-link session v1", 96):
//                PC to phone frames, phone to PC frames, the datagrams.
//
// Audio goes over UDP, phone to PC, one datagram per packet:
//   u8 0xA7, u8 flags, u16 frames, u32 seq, u32 timestamp, sealed PCM
// The first 12 bytes are the associated data; the nonce is
// (kAudioLabel, seq). PCM is 48 kHz stereo s16le. The timestamp counts
// frames, silent ones the phone didn't send included; seq counts
// datagrams. A resent packet is the same bytes again.
// The phone sends from the same port number as its TCP one, and the PC
// sends it a byte from the port it named in AUDIO_START first, and again
// every 10 s: a firewall on the PC lets the audio in as replies on that
// flow (ufw drops UDP nobody asked for).
// The PC asks for lost ones with
//   u8 0xA8, u8 0, u16 count, u32 counter, sealed u32 seq * count
// the nonce (kNackLabel, counter).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium::phonelink {

inline constexpr std::string_view kServiceType = "_atrium-link._tcp";
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kIdSize = 16;
inline constexpr std::uint32_t kMaxFrame = 16u << 20;
inline constexpr std::uint32_t kRate = 48000;
inline constexpr std::uint32_t kChannels = 2;
inline constexpr std::uint32_t kFrameBytes = 4;  // one stereo s16 frame
inline constexpr std::uint16_t kPacketFrames = 240;  // 5 ms: 960 bytes, under the MTU

enum class Role : std::uint8_t { Pc = 0, Phone = 1 };

enum class Type : std::uint8_t {
    Hello = 1,
    PairCommit = 2,
    PairKey = 3,
    PairReveal = 4,
    PairAccept = 5,
    PairReject = 6,
    Auth = 7,
    Proof = 8,
    Unknown = 9,
    Refused = 10,
    AudioStart = 16,
    AudioStop = 17,
    AudioState = 18,
    Media = 19,
    MediaCommand = 20,
};

enum class AudioState : std::uint8_t { Stopped = 0, Streaming = 1, Failed = 2 };

struct Identity {
    std::string id;  // kIdSize bytes
    std::string name;
};

// What the link wants done.
struct Event {
    enum class Kind {
        Send,      // bytes: put on the wire
        PairCode,  // text: the 6-digit code to show; accept() or reject() follows
        Paired,    // bytes: the key to store for the peer (peer())
        Ready,     // authenticated: message() and datagrams work
        Message,   // type, bytes: an application message
        Close,     // text: why; drop the connection
    } kind;
    std::string bytes;
    std::string text;
    Type type{};
};

// One connection's rules, fed bytes, answering with events. It never touches
// a socket, so both ends are testable anywhere.
class Link {
public:
    using KeyLookup = std::function<std::optional<std::string>(const std::string& peerId)>;
    using Random = std::function<std::string(std::size_t)>;

    // `keys`: the key stored for a peer's id, if any. `pair`: the PC asks to
    // pair when it has no key; the phone takes a pairing only then. `random`
    // is the CSPRNG; tests pass a fixed one.
    Link(Role role, Identity me, KeyLookup keys, Random random);

    void setPairing(bool on) { pairing_ = on; }
    std::vector<Event> start();
    std::vector<Event> received(std::string_view bytes);
    // The user's answer to a PairCode.
    std::vector<Event> accept();
    std::vector<Event> reject();
    // An application message; only once ready.
    std::vector<Event> message(Type type, std::string_view body);

    bool ready() const { return phase_ == Phase::Ready; }
    const Identity& peer() const { return peer_; }
    // The datagrams' key, once ready.
    const std::string& datagramKey() const { return udp_; }

private:
    enum class Phase { Hello, Pairing, Confirm, Auth, Ready, Closed };

    void handle(Type type, std::string_view body, std::vector<Event>& out);
    void send(Type type, std::string_view body, std::vector<Event>& out);
    void close(std::string why, std::vector<Event>& out);
    void startAuth(std::vector<Event>& out);
    void sendProof(std::vector<Event>& out);
    bool checkProof(std::string_view proof) const;
    std::string proof(Role who) const;
    void deriveSession();
    void pairDone(std::vector<Event>& out);

    Role role_;
    Identity me_, peer_;
    KeyLookup keys_;
    Random random_;
    bool pairing_ = false;
    Phase phase_ = Phase::Hello;
    std::string buffer_;

    // Pairing.
    std::string ownPriv_, commit_, pcPub_, phonePub_, secret_, transcript_;
    bool acceptedHere_ = false, acceptedThere_ = false;

    // Authentication.
    std::string key_, pcNonce_, phoneNonce_;
    bool proofSent_ = false, proofChecked_ = false;
    std::string sendKey_, recvKey_, udp_;
    std::uint64_t sent_ = 0, got_ = 0;
};

// Pairing's pieces, for the tests on both ends.
std::string transcript(std::string_view pcId, std::string_view phoneId, std::string_view pcPub, std::string_view phonePub);
std::string pairCode(std::string_view transcript);

std::string helloBody(Role role, const Identity& me);

// --- media -------------------------------------------------------------------

enum class MediaStatus : std::uint8_t { None = 0, Playing = 1, Paused = 2, Stopped = 3 };

enum MediaActions : std::uint8_t {
    CanPlay = 1,
    CanPause = 2,
    CanNext = 4,
    CanPrevious = 8,
    CanSeek = 16,
};

enum class MediaCommand : std::uint8_t { Play = 1, Pause = 2, PlayPause = 3, Next = 4, Previous = 5, Stop = 6, Seek = 7 };

struct Media {
    MediaStatus status = MediaStatus::None;
    std::uint8_t actions = 0;
    std::uint32_t duration = 0, position = 0;  // ms
    std::string title, artist, album, app, art;

    bool operator==(const Media&) const = default;
};

std::string packMedia(const Media& m);
std::optional<Media> unpackMedia(std::string_view body);
std::string packMediaCommand(MediaCommand c, std::uint32_t position = 0);
std::optional<std::pair<MediaCommand, std::uint32_t>> unpackMediaCommand(std::string_view body);

// --- datagrams ---------------------------------------------------------------

inline constexpr std::uint8_t kAudioMagic = 0xA7, kNackMagic = 0xA8;
inline constexpr std::uint32_t kAudioLabel = 3, kNackLabel = 4;
inline constexpr std::size_t kAudioHeader = 12;

enum AudioFlags : std::uint8_t {
    Discontinuity = 1,  // the first after a gap the phone made (silence, a restart)
};

struct AudioPacket {
    std::uint8_t flags = 0;
    std::uint32_t seq = 0;
    std::uint32_t timestamp = 0;
    std::string pcm;  // frames * kFrameBytes

    std::uint32_t frames() const { return std::uint32_t(pcm.size() / kFrameBytes); }
};

std::string packAudio(std::string_view key, const AudioPacket& p);
std::optional<AudioPacket> unpackAudio(std::string_view key, std::string_view datagram);

std::string packNack(std::string_view key, std::uint32_t counter, const std::vector<std::uint32_t>& seqs);
// The counter goes in `counter` (callers drop repeats).
std::optional<std::vector<std::uint32_t>> unpackNack(std::string_view key, std::string_view datagram,
                                                     std::uint32_t* counter = nullptr);

} // namespace atrium::phonelink
