#include "clipsync_core.hpp"

#include <algorithm>
#include <unordered_set>

namespace atrium::clipsync {

namespace {

constexpr std::uint32_t kOverhead = 64;  // a CLIP frame's own fields, besides mime and data

void put16(std::string& s, std::uint16_t v) {
    s += char(v >> 8);
    s += char(v);
}

void put32(std::string& s, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        s += char(v >> shift);
}

void put64(std::string& s, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
        s += char(v >> shift);
}

std::uint64_t get(std::string_view s, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; i++)
        v = v << 8 | std::uint8_t(s[at + i]);
    return v;
}

std::string frame(Type type, const std::string& body) {
    std::string out;
    put32(out, std::uint32_t(body.size() + 1));
    out += char(type);
    out += body;
    return out;
}

// Newer first; the hash breaks ties so both ends pick the same one.
bool newer(const Clip& a, const Clip& b) {
    return a.time != b.time ? a.time > b.time : a.hash() > b.hash();
}

} // namespace

std::uint64_t Clip::hash() const {
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= std::uint8_t(c);
            h *= 1099511628211ull;
        }
    };
    mix(mime);
    mix(std::string_view("\0", 1));
    mix(data);
    return h;
}

std::string helloFrame(std::string_view name, std::uint32_t maxClip) {
    std::string body;
    put16(body, kVersion);
    put32(body, maxClip);
    body += name;
    return frame(Type::Hello, body);
}

std::string clipFrame(const Clip& c, std::uint8_t flags) {
    std::string body;
    put64(body, std::uint64_t(c.time));
    body += char(flags);
    put16(body, std::uint16_t(c.mime.size()));
    body += c.mime;
    body += c.data;
    return frame(Type::Clip, body);
}

std::string syncedFrame() {
    return frame(Type::Synced, {});
}

std::optional<Message> Reader::next() {
    if (broken_ || buffer_.size() < 4)
        return std::nullopt;
    const std::uint32_t length = std::uint32_t(get(buffer_, 0, 4));
    if (length == 0 || length > std::uint64_t(limit_) + kOverhead + 0xffff) {
        broken_ = true;
        return std::nullopt;
    }
    if (buffer_.size() < 4 + std::size_t(length))
        return std::nullopt;
    const std::string_view body = std::string_view(buffer_).substr(5, length - 1);
    Message m;
    m.type = Type(std::uint8_t(buffer_[4]));
    bool ok = true;
    switch (m.type) {
    case Type::Hello:
        ok = body.size() >= 6;
        if (ok) {
            m.version = std::uint16_t(get(body, 0, 2));
            m.maxClip = std::uint32_t(get(body, 2, 4));
            m.name = std::string(body.substr(6));
        }
        break;
    case Type::Clip: {
        ok = body.size() >= 11;
        if (!ok)
            break;
        m.clip.time = std::int64_t(get(body, 0, 8));
        m.flags = std::uint8_t(body[8]);
        const std::size_t mime = get(body, 9, 2);
        ok = body.size() >= 11 + mime;
        if (ok) {
            m.clip.mime = std::string(body.substr(11, mime));
            m.clip.data = std::string(body.substr(11 + mime));
        }
        break;
    }
    case Type::Synced:
        break;
    default:
        // A newer peer's message this end doesn't know: skipped.
        buffer_.erase(0, 4 + length);
        return next();
    }
    if (!ok) {
        broken_ = true;
        return std::nullopt;
    }
    buffer_.erase(0, 4 + length);
    return m;
}

Session::Session(std::string name, std::vector<Clip> recent, std::uint32_t maxClip)
    : name_(std::move(name)), recent_(std::move(recent)), maxClip_(maxClip), reader_(maxClip) {
    if (!recent_.empty())
        current_ = recent_.front();
}

std::uint32_t Session::limit() const {
    return std::min(maxClip_, peerMax_);
}

std::vector<Action> Session::start() {
    return {{Action::Kind::Send, helloFrame(name_, maxClip_), {}}};
}

std::vector<Action> Session::received(std::string_view bytes) {
    reader_.feed(bytes);
    std::vector<Action> out;
    while (auto m = reader_.next()) {
        auto more = handle(std::move(*m));
        out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        if (!out.empty() && out.back().kind == Action::Kind::Close)
            return out;
    }
    if (reader_.broken())
        out.push_back({Action::Kind::Close, {}, {}});
    return out;
}

std::vector<Action> Session::handle(Message m) {
    std::vector<Action> out;
    switch (m.type) {
    case Type::Hello: {
        if (helloSeen_ || m.version < 1)
            return {{Action::Kind::Close, {}, {}}};
        helloSeen_ = true;
        peerName_ = m.name;
        peerMax_ = m.maxClip;
        phase_ = Phase::History;
        // Our history, as much as fits, oldest first.
        std::vector<Clip> send;
        for (const Clip& c : recent_) {
            if (int(send.size()) == kHistory)
                break;
            if (c.data.size() <= limit())
                send.push_back(c);
        }
        recent_ = send;
        for (auto it = send.rbegin(); it != send.rend(); ++it)
            out.push_back({Action::Kind::Send, clipFrame(*it, History), {}});
        out.push_back({Action::Kind::Send, syncedFrame(), {}});
        break;
    }
    case Type::Clip:
        if (phase_ == Phase::Hello)
            return {{Action::Kind::Close, {}, {}}};
        if (m.clip.data.size() > maxClip_)
            break;
        if (phase_ == Phase::History) {
            theirs_.push_back(std::move(m.clip));
            break;
        }
        // Live: theirs is the newest now, here too.
        peerCurrent_ = m.clip.hash();
        out.push_back({Action::Kind::SetClipboard, {}, std::move(m.clip)});
        break;
    case Type::Synced:
        if (phase_ != Phase::History)
            return {{Action::Kind::Close, {}, {}}};
        out = merge();
        break;
    }
    return out;
}

std::vector<Action> Session::merge() {
    std::vector<Action> out;
    phase_ = Phase::Live;
    std::unordered_set<std::uint64_t> seen;
    for (const Clip& c : recent_)
        seen.insert(c.hash());

    // Their newest replaces our clipboard only when it is newer than what
    // ours holds (which may be a clip too big to have gone over).
    const Clip* theirs = nullptr;
    for (const Clip& c : theirs_)
        if (!theirs || newer(c, *theirs))
            theirs = &c;
    const Clip* ours = recent_.empty() ? nullptr : &recent_.front();
    const bool take = theirs && (!current_ || (newer(*theirs, *current_) && theirs->hash() != current_->hash()));

    // Theirs that we lack, oldest first, into our history; the one taken
    // goes into the clipboard instead (which records it too).
    std::vector<const Clip*> missing;
    for (const Clip& c : theirs_)
        if (seen.insert(c.hash()).second && !(take && &c == theirs))
            missing.push_back(&c);
    std::ranges::sort(missing, [](const Clip* a, const Clip* b) { return newer(*b, *a); });
    for (const Clip* c : missing)
        out.push_back({Action::Kind::AddHistory, {}, *c});
    if (take)
        out.push_back({Action::Kind::SetClipboard, {}, *theirs});

    // They take ours by the same rule.
    if (ours && (!theirs || newer(*ours, *theirs)))
        peerCurrent_ = ours->hash();
    else if (theirs)
        peerCurrent_ = theirs->hash();
    theirs_.clear();

    // Copies made while the histories went over.
    std::vector<Clip> early = std::move(early_);
    early_.clear();
    for (const Clip& c : early) {
        auto more = copied(c);
        out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    return out;
}

std::vector<Action> Session::copied(const Clip& c) {
    if (phase_ != Phase::Live) {
        early_.push_back(c);
        return {};
    }
    const std::uint64_t h = c.hash();
    if (peerCurrent_ == h || c.data.size() > limit())
        return {};
    peerCurrent_ = h;
    return {{Action::Kind::Send, clipFrame(c, Live), {}}};
}

} // namespace atrium::clipsync
