#include "crypto.hpp"
#include "jitter.hpp"
#include "link_core.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <deque>
#include <map>

using namespace atrium::phonelink;

namespace {

std::string unhex(std::string_view h) {
    std::string out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out += char(std::stoi(std::string(h.substr(i, 2)), nullptr, 16));
    return out;
}

std::string hex(std::string_view s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (const char c : s) {
        out += d[std::uint8_t(c) >> 4];
        out += d[std::uint8_t(c) & 15];
    }
    return out;
}

// --- crypto: known answers (also pinned by the module's tests) ---------------

TEST(PhonelinkCrypto, KnownAnswers) {
    // RFC 4231 test case 2.
    EXPECT_EQ(hex(hmac("Jefe", "what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 5869 test case 1.
    EXPECT_EQ(hex(hkdf(std::string(22, '\x0b'), unhex("000102030405060708090a0b0c"), unhex("f0f1f2f3f4f5f6f7f8f9"), 42)),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    // RFC 7748 section 6.1.
    const std::string alice = unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const std::string bob = unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    EXPECT_EQ(hex(x25519Public(alice)), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    EXPECT_EQ(hex(x25519(alice, x25519Public(bob))), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    EXPECT_EQ(x25519(alice, std::string(32, '\0')), "");  // low order: refused
    // AES-256-GCM, from Python's cryptography.
    std::string key;
    for (int i = 0; i < 32; i++)
        key += char(i);
    EXPECT_EQ(hex(seal(key, nonce(3, 7), "hdr", "atrium")), "c2f9205df6b4b89af3a5ad4667a19cbe3ab32b87170d");
    EXPECT_EQ(hex(seal(key, nonce(1, 0), "", "")), "197b1d24ed7acc2678859df922c3b5cb");
    EXPECT_EQ(open(key, nonce(3, 7), "hdr", unhex("c2f9205df6b4b89af3a5ad4667a19cbe3ab32b87170d")), "atrium");
    EXPECT_FALSE(open(key, nonce(3, 8), "hdr", unhex("c2f9205df6b4b89af3a5ad4667a19cbe3ab32b87170d")));
    EXPECT_FALSE(open(key, nonce(3, 7), "hdR", unhex("c2f9205df6b4b89af3a5ad4667a19cbe3ab32b87170d")));
}

// --- the link ----------------------------------------------------------------

// Deterministic "random": a counter's bytes, different per end.
Link::Random counting(char base) {
    return [base, n = 0](std::size_t size) mutable {
        std::string s;
        for (std::size_t i = 0; i < size; i++)
            s += char(base + n++);
        return s;
    };
}

struct End {
    std::map<std::string, std::string> keys;  // by peer id
    Link link;
    std::string outbox, code, closed;
    std::vector<std::pair<Type, std::string>> messages;
    bool ready = false;

    End(Role role, std::string id, std::string name, char base)
        : link(role, {std::move(id), std::move(name)},
               [this](const std::string& peer) -> std::optional<std::string> {
                   auto it = keys.find(peer);
                   return it == keys.end() ? std::nullopt : std::optional(it->second);
               },
               counting(base)) {}

    void apply(const std::vector<Event>& events) {
        for (const Event& e : events) {
            switch (e.kind) {
            case Event::Kind::Send: outbox += e.bytes; break;
            case Event::Kind::PairCode: code = e.text; break;
            case Event::Kind::Paired: keys[link.peer().id] = e.bytes; break;
            case Event::Kind::Ready: ready = true; break;
            case Event::Kind::Message: messages.emplace_back(e.type, e.bytes); break;
            case Event::Kind::Close: closed = e.text; break;
            }
        }
    }
};

void pump(End& a, End& b) {
    for (int i = 0; i < 100 && (!a.outbox.empty() || !b.outbox.empty()); i++) {
        std::string ab = std::exchange(a.outbox, {}), ba = std::exchange(b.outbox, {});
        if (!ab.empty() && b.closed.empty())
            b.apply(b.link.received(ab));
        if (!ba.empty() && a.closed.empty())
            a.apply(a.link.received(ba));
    }
}

const std::string kPcId(16, 'P'), kPhoneId(16, 'F');

struct Pair {
    End pc{Role::Pc, kPcId, "desk", 1};
    End phone{Role::Phone, kPhoneId, "NX789J", 101};

    void connect() {
        pc.apply(pc.link.start());
        phone.apply(phone.link.start());
        pump(pc, phone);
    }
};

TEST(PhonelinkLink, PairsThenAuthenticates) {
    Pair p;
    p.pc.link.setPairing(true);
    p.phone.link.setPairing(true);
    p.connect();
    ASSERT_EQ(p.pc.code.size(), 6u);
    EXPECT_EQ(p.pc.code, p.phone.code);
    EXPECT_FALSE(p.pc.ready);
    // Golden, from Python's cryptography; the module's tests pin it too.
    // PC key bytes 1..32, phone key bytes 101..132, ids "P"*16 and "F"*16.
    EXPECT_EQ(p.pc.code, "077564");

    // One side accepting isn't enough.
    p.phone.apply(p.phone.link.accept());
    pump(p.pc, p.phone);
    EXPECT_TRUE(p.pc.keys.empty());
    p.pc.apply(p.pc.link.accept());
    pump(p.pc, p.phone);
    ASSERT_EQ(p.pc.keys.size(), 1u);
    EXPECT_EQ(p.pc.keys[kPhoneId], p.phone.keys[kPcId]);
    EXPECT_TRUE(p.pc.ready);
    EXPECT_TRUE(p.phone.ready);
    EXPECT_EQ(p.pc.link.peer().name, "NX789J");
    EXPECT_EQ(p.phone.link.peer().name, "desk");
    EXPECT_EQ(p.pc.link.datagramKey(), p.phone.link.datagramKey());

    // Sealed messages both ways.
    p.pc.apply(p.pc.link.message(Type::AudioStart, "\x13\x88\x00\xf0"));
    p.phone.apply(p.phone.link.message(Type::AudioState, std::string("\x01", 1) + "ok"));
    pump(p.pc, p.phone);
    ASSERT_EQ(p.phone.messages.size(), 1u);
    EXPECT_EQ(p.phone.messages[0].first, Type::AudioStart);
    EXPECT_EQ(p.phone.messages[0].second, "\x13\x88\x00\xf0");
    ASSERT_EQ(p.pc.messages.size(), 1u);
    EXPECT_EQ(p.pc.messages[0].second, std::string("\x01", 1) + "ok");
}

TEST(PhonelinkLink, KnownPeersSkipPairing) {
    Pair p;
    p.pc.keys[kPhoneId] = std::string(32, 'k');
    p.phone.keys[kPcId] = std::string(32, 'k');
    p.connect();
    EXPECT_TRUE(p.pc.code.empty());
    EXPECT_TRUE(p.pc.ready);
    EXPECT_TRUE(p.phone.ready);
    EXPECT_TRUE(p.pc.closed.empty());
    // Fresh nonces each time: another connection gets other session keys.
    Pair q;
    q.pc.keys = p.pc.keys;
    q.phone.keys = p.phone.keys;
    q.pc.link = Link(Role::Pc, {kPcId, "desk"}, [&](const std::string& id) -> std::optional<std::string> {
        return q.pc.keys.contains(id) ? std::optional(q.pc.keys[id]) : std::nullopt;
    }, counting(50));
    q.connect();
    ASSERT_TRUE(q.pc.ready);
    EXPECT_NE(q.pc.link.datagramKey(), p.pc.link.datagramKey());
}

TEST(PhonelinkLink, WrongKeyIsRefused) {
    Pair p;
    p.pc.keys[kPhoneId] = std::string(32, 'k');
    p.phone.keys[kPcId] = std::string(32, 'x');
    p.connect();
    EXPECT_FALSE(p.pc.ready);
    EXPECT_FALSE(p.phone.ready);
    EXPECT_NE(p.pc.closed, "");
}

TEST(PhonelinkLink, PhoneForgotThePc) {
    Pair p;
    p.pc.keys[kPhoneId] = std::string(32, 'k');
    p.connect();
    EXPECT_EQ(p.pc.closed, "the other end doesn't know us");
    EXPECT_EQ(p.phone.closed, "an unknown PC");
}

TEST(PhonelinkLink, NoPairingUnlessAsked) {
    {
        Pair p;  // the PC wasn't asked to pair
        p.phone.link.setPairing(true);
        p.connect();
        EXPECT_EQ(p.pc.closed, "not paired");
    }
    {
        Pair p;  // the phone isn't taking pairings
        p.pc.link.setPairing(true);
        p.connect();
        EXPECT_EQ(p.pc.closed, "the phone isn't pairing now");
        EXPECT_TRUE(p.phone.code.empty());
    }
}

TEST(PhonelinkLink, RejectEndsIt) {
    Pair p;
    p.pc.link.setPairing(true);
    p.phone.link.setPairing(true);
    p.connect();
    p.pc.apply(p.pc.link.accept());
    p.phone.apply(p.phone.link.reject());
    pump(p.pc, p.phone);
    EXPECT_TRUE(p.pc.keys.empty());
    EXPECT_TRUE(p.phone.keys.empty());
    EXPECT_EQ(p.pc.closed, "pairing rejected on the other end");
}

TEST(PhonelinkLink, RevealMustMatchCommitment) {
    Pair p;
    p.pc.link.setPairing(true);
    p.phone.link.setPairing(true);
    p.pc.apply(p.pc.link.start());
    p.phone.apply(p.phone.link.start());
    // Deliver up to the REVEAL, then swap its key for another.
    std::string reveal;
    for (int i = 0; i < 10 && reveal.empty(); i++) {
        std::string ab = std::exchange(p.pc.outbox, {}), ba = std::exchange(p.phone.outbox, {});
        if (ab.size() > 4 && Type(ab[4]) == Type::PairReveal)
            reveal = ab;
        else if (!ab.empty())
            p.phone.apply(p.phone.link.received(ab));
        if (!ba.empty())
            p.pc.apply(p.pc.link.received(ba));
    }
    ASSERT_EQ(reveal.size(), 4u + 1 + 32);
    reveal[10] ^= 1;
    p.phone.apply(p.phone.link.received(reveal));
    EXPECT_EQ(p.phone.closed, "the PC's key doesn't match its commitment");
    EXPECT_TRUE(p.phone.code.empty());
}

TEST(PhonelinkLink, TamperedFrameCloses) {
    Pair p;
    p.pc.keys[kPhoneId] = std::string(32, 'k');
    p.phone.keys[kPcId] = std::string(32, 'k');
    p.connect();
    ASSERT_TRUE(p.phone.ready);
    std::string f;
    for (const Event& e : p.pc.link.message(Type::AudioStop, ""))
        f += e.bytes;
    f.back() ^= 1;
    p.phone.apply(p.phone.link.received(f));
    EXPECT_EQ(p.phone.closed, "a frame failed authentication");
    EXPECT_TRUE(p.phone.messages.empty());
}

TEST(PhonelinkLink, ReplayedFrameCloses) {
    Pair p;
    p.pc.keys[kPhoneId] = std::string(32, 'k');
    p.phone.keys[kPcId] = std::string(32, 'k');
    p.connect();
    std::string f;
    for (const Event& e : p.pc.link.message(Type::AudioStop, ""))
        f += e.bytes;
    p.phone.apply(p.phone.link.received(f));
    EXPECT_EQ(p.phone.messages.size(), 1u);
    p.phone.apply(p.phone.link.received(f));
    EXPECT_EQ(p.phone.closed, "a frame failed authentication");
}

TEST(PhonelinkLink, FramesSplitAnywhere) {
    Pair p;
    p.pc.keys[kPhoneId] = std::string(32, 'k');
    p.phone.keys[kPcId] = std::string(32, 'k');
    p.pc.apply(p.pc.link.start());
    p.phone.apply(p.phone.link.start());
    for (int i = 0; i < 20; i++) {
        std::string ab = std::exchange(p.pc.outbox, {}), ba = std::exchange(p.phone.outbox, {});
        for (const char c : ab)
            p.phone.apply(p.phone.link.received(std::string(1, c)));
        for (const char c : ba)
            p.pc.apply(p.pc.link.received(std::string(1, c)));
    }
    EXPECT_TRUE(p.pc.ready);
    EXPECT_TRUE(p.phone.ready);
}

TEST(PhonelinkLink, BadHelloCloses) {
    End pc(Role::Pc, kPcId, "desk", 1);
    pc.apply(pc.link.received(std::string("\x00\x00\x00\x02\x05x", 6)));
    EXPECT_EQ(pc.closed, "expected HELLO");
    End other(Role::Pc, kPcId, "desk", 1);
    std::string hello;
    for (const Event& e : End(Role::Pc, kPhoneId, "another pc", 9).link.start())
        hello += e.bytes;
    other.apply(other.link.received(hello));
    EXPECT_EQ(other.closed, "the other end is the same kind");
    End huge(Role::Pc, kPcId, "desk", 1);
    huge.apply(huge.link.received(std::string("\x7f\xff\xff\xff", 4)));
    EXPECT_EQ(huge.closed, "bad frame");
}

// --- datagrams ---------------------------------------------------------------

TEST(PhonelinkDatagram, AudioRoundTrip) {
    const std::string key(32, 'a');
    AudioPacket p{Discontinuity, 0xfffffffe, 123456, std::string(kPacketFrames * kFrameBytes, '\x11')};
    const std::string d = packAudio(key, p);
    EXPECT_EQ(d.size(), kAudioHeader + 960 + kTag);
    EXPECT_LE(d.size() + 28, 1500u);  // IPv4 + UDP: one Ethernet frame
    auto q = unpackAudio(key, d);
    ASSERT_TRUE(q);
    EXPECT_EQ(q->flags, Discontinuity);
    EXPECT_EQ(q->seq, 0xfffffffeu);
    EXPECT_EQ(q->timestamp, 123456u);
    EXPECT_EQ(q->pcm, p.pcm);
    // Golden, from Python's cryptography; the module pins the same bytes.
    EXPECT_EQ(hex(packAudio(key, {0, 1, 2, "abcd"})), "a700000100000001000000021edb81fd9ad1b92580210f28316e1da2e88bc4f5");
    EXPECT_EQ(hex(packNack(key, 9, {1, 5})), "a800000200000009b156f015fa5daa2ffe650a6aac7c4c23b7a068e6c4e009bb");
    std::string bad = d;
    bad[1] = 0;  // the header is authenticated too
    EXPECT_FALSE(unpackAudio(key, bad));
    EXPECT_FALSE(unpackAudio(std::string(32, 'b'), d));
    EXPECT_FALSE(unpackAudio(key, d.substr(0, d.size() - 1)));
}

TEST(PhonelinkMedia, RoundTrips) {
    Media m{MediaStatus::Playing, CanPause | CanNext | CanSeek, 215000, 61000, "Title", "Artist", "Album", "app",
            std::string("\xff\xd8jpeg", 6)};
    const std::string b = packMedia(m);
    // Golden; the module pins the same bytes.
    EXPECT_EQ(hex(packMedia({MediaStatus::Paused, CanPlay, 1, 2, "t", "", "al", "x", "ART"})),
              "0201000000010000000200017400000002616c000178415254");
    EXPECT_EQ(unpackMedia(b), m);
    EXPECT_FALSE(unpackMedia(b.substr(0, 12)));  // cut inside a string
    EXPECT_FALSE(unpackMedia(std::string(1, '\x09') + b.substr(1)));
    EXPECT_EQ(hex(packMediaCommand(MediaCommand::Seek, 70000)), "0700011170");
    EXPECT_EQ(unpackMediaCommand(packMediaCommand(MediaCommand::Seek, 70000)),
              (std::pair{MediaCommand::Seek, std::uint32_t(70000)}));
    EXPECT_FALSE(unpackMediaCommand(std::string("\x08\0\0\0\0", 5)));
    EXPECT_FALSE(unpackMediaCommand("\x01"));
}

TEST(PhonelinkDatagram, NackRoundTrip) {
    const std::string key(32, 'a');
    std::uint32_t counter = 0;
    auto seqs = unpackNack(key, packNack(key, 9, {1, 5, 0xffffffff}), &counter);
    ASSERT_TRUE(seqs);
    EXPECT_EQ(*seqs, (std::vector<std::uint32_t>{1, 5, 0xffffffff}));
    EXPECT_EQ(counter, 9u);
    // An audio datagram is no nack, and the other way around.
    EXPECT_FALSE(unpackNack(key, packAudio(key, {0, 1, 2, "abcd"})));
    EXPECT_FALSE(unpackAudio(key, packNack(key, 1, {1})));
}

// --- jitter buffer -----------------------------------------------------------

// A packet whose frames are its frame indices (left) and their negation (right).
AudioPacket tone(std::uint32_t seq, std::uint32_t ts, std::uint8_t flags = 0, std::uint32_t frames = kPacketFrames) {
    AudioPacket p{flags, seq, ts, {}};
    for (std::uint32_t i = 0; i < frames; i++) {
        const std::int16_t l = std::int16_t((ts + i) % 30000 + 1), r = std::int16_t(-l);
        p.pcm.append(reinterpret_cast<const char*>(&l), 2);
        p.pcm.append(reinterpret_cast<const char*>(&r), 2);
    }
    return p;
}

std::vector<std::int16_t> pull(JitterBuffer& j, std::uint32_t frames) {
    std::vector<std::int16_t> out(frames * kChannels, 7777);
    j.pull(out.data(), frames);
    return out;
}

constexpr std::uint32_t kTarget = 4 * kPacketFrames;

TEST(PhonelinkJitter, PlaysInOrderAfterTheTarget) {
    JitterBuffer j(kTarget);
    for (std::uint32_t i = 0; i < 8; i++)
        j.push(tone(i, i * kPacketFrames), 0);
    // First `target` frames are the wait, then the stream from frame 0.
    auto wait = pull(j, kTarget);
    EXPECT_TRUE(std::all_of(wait.begin(), wait.end(), [](std::int16_t s) { return s == 0; }));
    auto out = pull(j, 8 * kPacketFrames - kTarget);
    for (std::uint32_t i = 0; i < out.size() / 2; i++) {
        ASSERT_EQ(out[i * 2], std::int16_t(i + 1)) << i;
        ASSERT_EQ(out[i * 2 + 1], std::int16_t(-(int(i) + 1))) << i;
    }
    EXPECT_EQ(j.stats().concealed, 0u);
}

TEST(PhonelinkJitter, ReordersAndDropsDuplicates) {
    JitterBuffer j(kTarget);
    for (const std::uint32_t i : {0u, 2u, 1u, 3u, 3u, 5u, 4u})
        j.push(tone(i, i * kPacketFrames), 0);
    pull(j, kTarget);
    auto out = pull(j, 6 * kPacketFrames);
    for (std::uint32_t i = 0; i < out.size() / 2; i++)
        ASSERT_EQ(out[i * 2], std::int16_t(i + 1)) << i;
    EXPECT_EQ(j.stats().duplicates, 1u);
    EXPECT_EQ(j.stats().concealed, 0u);
}

TEST(PhonelinkJitter, AsksForLossesAndTakesTheResend) {
    JitterBuffer j(kTarget);
    j.push(tone(0, 0), 0);
    j.push(tone(1, kPacketFrames), 5'000);
    j.push(tone(4, 4 * kPacketFrames), 20'000);  // 2 and 3 lost
    EXPECT_EQ(j.nacks(20'000), (std::vector<std::uint32_t>{2, 3}));
    EXPECT_TRUE(j.nacks(25'000).empty());                              // too soon to ask again
    EXPECT_EQ(j.nacks(36'000), (std::vector<std::uint32_t>{2, 3}));  // and again
    j.push(tone(3, 3 * kPacketFrames), 37'000);
    j.push(tone(2, 2 * kPacketFrames), 38'000);
    EXPECT_TRUE(j.nacks(60'000).empty());
    pull(j, kTarget);
    auto out = pull(j, 5 * kPacketFrames);
    for (std::uint32_t i = 0; i < out.size() / 2; i++)
        ASSERT_EQ(out[i * 2], std::int16_t(i + 1)) << i;
    EXPECT_EQ(j.stats().recovered, 2u);
    EXPECT_EQ(j.stats().concealed, 0u);
}

TEST(PhonelinkJitter, GivesUpOnOldLosses) {
    JitterBuffer j(kTarget);
    j.push(tone(0, 0), 0);
    j.push(tone(2, 2 * kPacketFrames), 0);
    EXPECT_EQ(j.nacks(0).size(), 1u);
    // Past the target's worth of time a resend can't play: no more asks.
    EXPECT_TRUE(j.nacks(std::uint64_t(kTarget) * 1'000'000 / kRate + 1).empty());
}

TEST(PhonelinkJitter, ConcealsWhatNeverCame) {
    JitterBuffer j(kTarget);
    for (const std::uint32_t i : {0u, 1u, 3u, 4u, 5u})
        j.push(tone(i, i * kPacketFrames), 0);
    pull(j, kTarget);
    auto out = pull(j, 5 * kPacketFrames);
    // Packet 1 repeated, fading: its first frame at full level, then quieter.
    const std::uint32_t gap = 2 * kPacketFrames;
    EXPECT_EQ(out[gap * 2], std::int16_t(kPacketFrames + 1));
    EXPECT_LT(std::abs(out[(gap + kPacketFrames - 1) * 2]), std::abs(out[(gap + 1) * 2]));
    EXPECT_EQ(out[3 * kPacketFrames * 2], std::int16_t(3 * kPacketFrames + 1));  // back on track
    EXPECT_EQ(j.stats().concealed, kPacketFrames);
}

TEST(PhonelinkJitter, DropsLatePackets) {
    JitterBuffer j(kTarget);
    for (std::uint32_t i = 0; i < 6; i++)
        if (i != 1)
            j.push(tone(i, i * kPacketFrames), 0);
    pull(j, kTarget + 3 * kPacketFrames);
    j.push(tone(1, kPacketFrames), 0);
    EXPECT_EQ(j.stats().late, 1u);
}

TEST(PhonelinkJitter, ResyncsAfterSilence) {
    JitterBuffer j(kTarget);
    for (std::uint32_t i = 0; i < 4; i++)
        j.push(tone(i, i * kPacketFrames), 0);
    pull(j, kTarget + 10 * kPacketFrames);  // played out, then quiet
    EXPECT_FALSE(j.playing());
    // The phone comes back far ahead in time, flagged; seq goes on.
    j.push(tone(4, 1000 * kPacketFrames, Discontinuity), 0);
    EXPECT_TRUE(j.playing());
    EXPECT_EQ(j.level(), std::int64_t(kTarget) + kPacketFrames);
    pull(j, kTarget);
    auto out = pull(j, kPacketFrames);
    EXPECT_EQ(out[0], std::int16_t(1000 * kPacketFrames % 30000 + 1));
    EXPECT_EQ(j.stats().resyncs, 2u);
}

TEST(PhonelinkJitter, CatchesUpOnAPile) {
    JitterBuffer j(kTarget);
    for (std::uint32_t i = 0; i < 40; i++)
        j.push(tone(i, i * kPacketFrames), 0);
    EXPECT_LE(j.level(), std::int64_t(kTarget) * 8);
    EXPECT_GT(j.stats().skipped, 0u);
}

TEST(PhonelinkJitter, WrapsAround) {
    JitterBuffer j(kTarget);
    const std::uint32_t seq0 = 0xfffffffd, ts0 = 0xffffffff - 2 * kPacketFrames + 1;
    for (std::uint32_t i = 0; i < 6; i++)
        j.push(tone(seq0 + i, ts0 + i * kPacketFrames), 0);
    EXPECT_TRUE(j.nacks(0).empty());
    EXPECT_EQ(j.level(), std::int64_t(kTarget) + 6 * kPacketFrames);
    pull(j, kTarget + 6 * kPacketFrames);
    EXPECT_EQ(j.stats().concealed, 0u);
    EXPECT_EQ(j.stats().late, 0u);
}

TEST(PhonelinkDrift, HoldsTheTargetAgainstASkewedClock) {
    // The phone sends 0.03% faster than the card plays; the controller has
    // to settle near rate 1.0003 with the level near the target.
    JitterBuffer j(kTarget);
    Drift d;
    const double skew = 1.0003;
    double sent = 0, rate = 1;  // rate: stream frames per card frame
    std::uint32_t seq = 0;
    const std::uint32_t period = 1024;
    std::vector<std::int16_t> out(period * 2 * 2);
    for (std::uint32_t step = 0; step < 60 * kRate / period; step++) {  // a minute
        // Network side: everything up to now, in packets.
        sent += period * skew;
        while (seq * double(kPacketFrames) + kPacketFrames <= sent) {
            j.push(tone(seq, seq * kPacketFrames), 0);
            seq++;
        }
        // Card side: `period` card frames are period*rate stream frames.
        const auto frames = std::uint32_t(std::lround(period * rate));
        out.resize(frames * 2);
        j.pull(out.data(), frames);
        rate = d.update(j.level(), kTarget, period);
    }
    EXPECT_NEAR(d.rate(), skew, 0.0002);
    EXPECT_NEAR(double(j.level()), double(kTarget), kPacketFrames * 2.0);
    EXPECT_EQ(j.stats().skipped, 0u);
}

} // namespace
