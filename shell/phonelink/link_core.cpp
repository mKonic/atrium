#include "link_core.hpp"

#include "crypto.hpp"

#include <cstdio>

namespace atrium::phonelink {

namespace {

constexpr std::string_view kPairLabel = "atrium-link pair v1";
constexpr std::string_view kProofLabel = "atrium-link proof v1";
constexpr std::string_view kSessionLabel = "atrium-link session v1";
constexpr std::size_t kNonceSize = 16;
// Nonce labels of the sealed frames, by sender.
constexpr std::uint32_t kPcLabel = 1, kPhoneLabel = 2;

void put16(std::string& s, std::uint16_t v) {
    s += char(v >> 8);
    s += char(v);
}

void put32(std::string& s, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        s += char(v >> shift);
}

std::uint32_t get(std::string_view s, std::size_t at, int bytes) {
    std::uint32_t v = 0;
    for (int i = 0; i < bytes; i++)
        v = v << 8 | std::uint8_t(s[at + i]);
    return v;
}

} // namespace

std::string transcript(std::string_view pcId, std::string_view phoneId, std::string_view pcPub, std::string_view phonePub) {
    std::string t(kPairLabel);
    t.append(pcId).append(phoneId).append(pcPub).append(phonePub);
    return sha256(t);
}

std::string pairCode(std::string_view transcript) {
    char code[8];
    std::snprintf(code, sizeof code, "%06u", get(transcript, 0, 4) % 1'000'000u);
    return code;
}

std::string helloBody(Role role, const Identity& me) {
    std::string b;
    put16(b, kVersion);
    b += char(role);
    b += me.id;
    b += me.name;
    return b;
}

Link::Link(Role role, Identity me, KeyLookup keys, Random random)
    : role_(role), me_(std::move(me)), keys_(std::move(keys)), random_(std::move(random)) {}

std::vector<Event> Link::start() {
    std::vector<Event> out;
    send(Type::Hello, helloBody(role_, me_), out);
    return out;
}

void Link::send(Type type, std::string_view body, std::vector<Event>& out) {
    std::string plain;
    plain += char(type);
    plain += body;
    std::string frame;
    if (phase_ != Phase::Ready) {
        put32(frame, std::uint32_t(plain.size()));
        frame += plain;
    } else {
        put32(frame, std::uint32_t(plain.size() + kTag));
        const std::uint32_t label = role_ == Role::Pc ? kPcLabel : kPhoneLabel;
        frame += seal(sendKey_, nonce(label, sent_++), frame, plain);
    }
    out.push_back({Event::Kind::Send, std::move(frame), {}, type});
}

void Link::close(std::string why, std::vector<Event>& out) {
    if (phase_ == Phase::Closed)
        return;
    phase_ = Phase::Closed;
    out.push_back({Event::Kind::Close, {}, std::move(why), {}});
}

std::vector<Event> Link::received(std::string_view bytes) {
    std::vector<Event> out;
    if (phase_ == Phase::Closed)
        return out;
    buffer_.append(bytes);
    while (phase_ != Phase::Closed && buffer_.size() >= 4) {
        const std::uint32_t len = get(buffer_, 0, 4);
        if (len == 0 || len > kMaxFrame) {
            close("bad frame", out);
            break;
        }
        if (buffer_.size() < 4 + std::size_t(len))
            break;
        std::string plain;
        if (phase_ == Phase::Ready) {
            const std::uint32_t label = role_ == Role::Pc ? kPhoneLabel : kPcLabel;
            auto opened = open(recvKey_, nonce(label, got_++), std::string_view(buffer_).substr(0, 4),
                               std::string_view(buffer_).substr(4, len));
            if (!opened || opened->empty()) {
                close("a frame failed authentication", out);
                break;
            }
            plain = std::move(*opened);
        } else {
            plain = buffer_.substr(4, len);
        }
        buffer_.erase(0, 4 + std::size_t(len));
        handle(Type(std::uint8_t(plain[0])), std::string_view(plain).substr(1), out);
    }
    return out;
}

void Link::handle(Type type, std::string_view body, std::vector<Event>& out) {
    const bool pc = role_ == Role::Pc;
    switch (phase_) {
    case Phase::Hello: {
        if (type != Type::Hello || body.size() < 3 + kIdSize) {
            close("expected HELLO", out);
            return;
        }
        if (get(body, 0, 2) != kVersion) {
            close("version " + std::to_string(get(body, 0, 2)) + " is not " + std::to_string(kVersion), out);
            return;
        }
        if (Role(std::uint8_t(body[2])) == role_) {
            close("the other end is the same kind", out);
            return;
        }
        peer_.id = std::string(body.substr(3, kIdSize));
        peer_.name = std::string(body.substr(3 + kIdSize));
        if (!pc) {
            phase_ = Phase::Auth;  // or pairing, whichever the PC starts
            return;
        }
        if (auto k = keys_(peer_.id)) {
            key_ = std::move(*k);
            phase_ = Phase::Auth;
            startAuth(out);
        } else if (pairing_) {
            phase_ = Phase::Pairing;
            ownPriv_ = random_(32);
            pcPub_ = x25519Public(ownPriv_);
            send(Type::PairCommit, sha256(pcPub_), out);
        } else {
            close("not paired", out);
        }
        return;
    }
    case Phase::Pairing:
        if (!pc && type == Type::PairCommit && body.size() == 32) {
            commit_ = std::string(body);
            ownPriv_ = random_(32);
            phonePub_ = x25519Public(ownPriv_);
            send(Type::PairKey, phonePub_, out);
            return;
        }
        if (pc && type == Type::PairKey && body.size() == 32) {
            phonePub_ = std::string(body);
            send(Type::PairReveal, pcPub_, out);
        } else if (!pc && type == Type::PairReveal && body.size() == 32) {
            pcPub_ = std::string(body);
            if (!same(sha256(pcPub_), commit_)) {
                close("the PC's key doesn't match its commitment", out);
                return;
            }
        } else if (pc && type == Type::Refused) {
            close("the phone isn't pairing now", out);
            return;
        } else {
            close("unexpected message while pairing", out);
            return;
        }
        secret_ = x25519(ownPriv_, pc ? phonePub_ : pcPub_);
        if (secret_.empty()) {
            close("bad key", out);
            return;
        }
        transcript_ = pc ? transcript(me_.id, peer_.id, pcPub_, phonePub_) : transcript(peer_.id, me_.id, pcPub_, phonePub_);
        phase_ = Phase::Confirm;
        out.push_back({Event::Kind::PairCode, {}, pairCode(transcript_), {}});
        return;
    case Phase::Confirm:
        if (type == Type::PairAccept) {
            acceptedThere_ = true;
            if (acceptedHere_)
                pairDone(out);
        } else if (type == Type::PairReject) {
            close("pairing rejected on the other end", out);
        } else {
            close("unexpected message while confirming", out);
        }
        return;
    case Phase::Auth:
        if (!pc && type == Type::PairCommit) {
            if (!pairing_) {
                send(Type::Refused, {}, out);
                close("a pairing came while not pairing", out);
                return;
            }
            phase_ = Phase::Pairing;
            handle(type, body, out);
            return;
        }
        if (type == Type::Unknown) {
            close("the other end doesn't know us", out);
            return;
        }
        if (type == Type::Auth && body.size() == kNonceSize) {
            if (pc) {
                if (!phoneNonce_.empty()) {
                    close("AUTH twice", out);
                    return;
                }
                phoneNonce_ = std::string(body);
                return;  // its PROOF follows
            }
            // Just paired: the key is ours already, maybe not yet stored.
            if (key_.empty()) {
                auto k = keys_(peer_.id);
                if (!k) {
                    send(Type::Unknown, {}, out);
                    close("an unknown PC", out);
                    return;
                }
                key_ = std::move(*k);
            }
            pcNonce_ = std::string(body);
            phoneNonce_ = random_(kNonceSize);
            send(Type::Auth, phoneNonce_, out);
            sendProof(out);
            return;
        }
        if (type == Type::Proof && !phoneNonce_.empty() && !pcNonce_.empty()) {
            if (!checkProof(body)) {
                close("wrong proof: the other end has another key", out);
                return;
            }
            proofChecked_ = true;
            if (!proofSent_)
                sendProof(out);
            deriveSession();
            phase_ = Phase::Ready;
            out.push_back({Event::Kind::Ready, {}, {}, {}});
            return;
        }
        close("unexpected message while authenticating", out);
        return;
    case Phase::Ready:
        if (std::uint8_t(type) < std::uint8_t(Type::AudioStart)) {
            close("a handshake message after the handshake", out);
            return;
        }
        out.push_back({Event::Kind::Message, std::string(body), {}, type});
        return;
    case Phase::Closed:
        return;
    }
}

void Link::startAuth(std::vector<Event>& out) {
    pcNonce_ = random_(kNonceSize);
    send(Type::Auth, pcNonce_, out);
}

std::string Link::proof(Role who) const {
    std::string d(kProofLabel);
    d += char(who);
    d += pcNonce_;
    d += phoneNonce_;
    d += role_ == Role::Pc ? me_.id : peer_.id;
    d += role_ == Role::Pc ? peer_.id : me_.id;
    return hmac(key_, d);
}

void Link::sendProof(std::vector<Event>& out) {
    proofSent_ = true;
    send(Type::Proof, proof(role_), out);
}

bool Link::checkProof(std::string_view p) const {
    return same(p, proof(role_ == Role::Pc ? Role::Phone : Role::Pc));
}

void Link::deriveSession() {
    const std::string keys = hkdf(key_, pcNonce_ + phoneNonce_, kSessionLabel, 96);
    const std::string toPhone = keys.substr(0, 32), toPc = keys.substr(32, 32);
    sendKey_ = role_ == Role::Pc ? toPhone : toPc;
    recvKey_ = role_ == Role::Pc ? toPc : toPhone;
    udp_ = keys.substr(64, 32);
}

std::vector<Event> Link::accept() {
    std::vector<Event> out;
    if (phase_ != Phase::Confirm || acceptedHere_)
        return out;
    acceptedHere_ = true;
    send(Type::PairAccept, {}, out);
    if (acceptedThere_)
        pairDone(out);
    return out;
}

std::vector<Event> Link::reject() {
    std::vector<Event> out;
    if (phase_ != Phase::Confirm)
        return out;
    send(Type::PairReject, {}, out);
    close("pairing rejected", out);
    return out;
}

void Link::pairDone(std::vector<Event>& out) {
    key_ = hkdf(secret_, transcript_, kPairLabel, 32);
    out.push_back({Event::Kind::Paired, key_, {}, {}});
    secret_.clear();
    ownPriv_.clear();
    phase_ = Phase::Auth;
    if (role_ == Role::Pc)
        startAuth(out);
}

std::vector<Event> Link::message(Type type, std::string_view body) {
    std::vector<Event> out;
    if (phase_ == Phase::Ready)
        send(type, body, out);
    return out;
}

// --- datagrams ---------------------------------------------------------------

std::string packAudio(std::string_view key, const AudioPacket& p) {
    std::string d;
    d += char(kAudioMagic);
    d += char(p.flags);
    put16(d, std::uint16_t(p.frames()));
    put32(d, p.seq);
    put32(d, p.timestamp);
    d += seal(key, nonce(kAudioLabel, p.seq), d, p.pcm);
    return d;
}

std::optional<AudioPacket> unpackAudio(std::string_view key, std::string_view d) {
    if (d.size() < kAudioHeader + kTag || std::uint8_t(d[0]) != kAudioMagic)
        return std::nullopt;
    AudioPacket p;
    p.flags = std::uint8_t(d[1]);
    const std::uint32_t frames = get(d, 2, 2);
    p.seq = get(d, 4, 4);
    p.timestamp = get(d, 8, 4);
    if (d.size() != kAudioHeader + frames * kFrameBytes + kTag)
        return std::nullopt;
    auto pcm = open(key, nonce(kAudioLabel, p.seq), d.substr(0, kAudioHeader), d.substr(kAudioHeader));
    if (!pcm)
        return std::nullopt;
    p.pcm = std::move(*pcm);
    return p;
}

std::string packNack(std::string_view key, std::uint32_t counter, const std::vector<std::uint32_t>& seqs) {
    std::string d, body;
    d += char(kNackMagic);
    d += '\0';
    put16(d, std::uint16_t(seqs.size()));
    put32(d, counter);
    for (const std::uint32_t s : seqs)
        put32(body, s);
    d += seal(key, nonce(kNackLabel, counter), d, body);
    return d;
}

std::optional<std::vector<std::uint32_t>> unpackNack(std::string_view key, std::string_view d, std::uint32_t* counter) {
    if (d.size() < 8 + kTag || std::uint8_t(d[0]) != kNackMagic)
        return std::nullopt;
    const std::uint32_t count = get(d, 2, 2), c = get(d, 4, 4);
    if (d.size() != 8 + count * 4 + kTag)
        return std::nullopt;
    auto body = open(key, nonce(kNackLabel, c), d.substr(0, 8), d.substr(8));
    if (!body)
        return std::nullopt;
    std::vector<std::uint32_t> seqs;
    for (std::uint32_t i = 0; i < count; i++)
        seqs.push_back(get(*body, i * 4, 4));
    if (counter)
        *counter = c;
    return seqs;
}

} // namespace atrium::phonelink
