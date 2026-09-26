#include "clipsync_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::clipsync;

namespace {

Clip text(std::string s, std::int64_t time) {
    return {std::string(kText), std::move(s), time};
}

// One end of a connection and what it did to its own clipboard.
struct End {
    Session session;
    std::vector<std::string> clipboard;  // SetClipboard, in order
    std::vector<std::string> history;    // AddHistory, in order
    bool closed = false;
    std::string outbox;

    End(std::string name, std::vector<Clip> recent, std::uint32_t max = kMaxClip)
        : session(std::move(name), std::move(recent), max) {}

    void apply(const std::vector<Action>& actions) {
        for (const Action& a : actions) {
            switch (a.kind) {
            case Action::Kind::Send: outbox += a.bytes; break;
            case Action::Kind::SetClipboard: clipboard.push_back(a.clip.data); break;
            case Action::Kind::AddHistory: history.push_back(a.clip.data); break;
            case Action::Kind::Close: closed = true; break;
            }
        }
    }
};

// Delivers until both are quiet.
void pump(End& a, End& b) {
    while (!a.outbox.empty() || !b.outbox.empty()) {
        std::string ab = std::exchange(a.outbox, {}), ba = std::exchange(b.outbox, {});
        if (!ab.empty())
            b.apply(b.session.received(ab));
        if (!ba.empty())
            a.apply(a.session.received(ba));
    }
}

void connect(End& a, End& b) {
    a.apply(a.session.start());
    b.apply(b.session.start());
    pump(a, b);
}

} // namespace

TEST(ClipSync, FramesSurviveAnySplit) {
    const std::string bytes = helloFrame("pc", 1234) + clipFrame(text("héllo", 42), Live) + syncedFrame();
    Reader r;
    std::vector<Message> got;
    for (const char c : bytes) {
        r.feed(std::string_view(&c, 1));
        while (auto m = r.next())
            got.push_back(*m);
    }
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].type, Type::Hello);
    EXPECT_EQ(got[0].name, "pc");
    EXPECT_EQ(got[0].maxClip, 1234u);
    EXPECT_EQ(got[0].version, kVersion);
    EXPECT_EQ(got[1].type, Type::Clip);
    EXPECT_EQ(got[1].clip.data, "héllo");
    EXPECT_EQ(got[1].clip.mime, kText);
    EXPECT_EQ(got[1].clip.time, 42);
    EXPECT_EQ(got[1].flags, Live);
    EXPECT_EQ(got[2].type, Type::Synced);
    EXPECT_FALSE(r.broken());
}

TEST(ClipSync, BadFramesBreakTheReader) {
    Reader big(100);
    big.feed(clipFrame(text(std::string(100 + 0x20000, 'x'), 1), Live));
    EXPECT_FALSE(big.next());
    EXPECT_TRUE(big.broken());

    Reader empty;
    empty.feed(std::string("\0\0\0\0", 4));
    EXPECT_FALSE(empty.next());
    EXPECT_TRUE(empty.broken());

    // A clip whose mime runs past its end.
    Reader shortClip;
    std::string f = clipFrame(text("", 1), Live);
    f[4 + 1 + 9] = char(0xff);
    shortClip.feed(f);
    EXPECT_FALSE(shortClip.next());
    EXPECT_TRUE(shortClip.broken());
}

TEST(ClipSync, UnknownMessagesAreSkipped) {
    std::string future = std::string("\0\0\0\3", 4) + char(99) + "ab";
    Reader r;
    r.feed(future + syncedFrame());
    auto m = r.next();
    ASSERT_TRUE(m);
    EXPECT_EQ(m->type, Type::Synced);
}

TEST(ClipSync, WireBytesArePinned) {
    // From an independent encoder; the phone module's test pins the same.
    const std::string bytes = helloFrame("pc", 1234) + clipFrame(text("héllo", 42), Live) + syncedFrame();
    std::string hex;
    for (const char c : bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        hex += digits[std::uint8_t(c) >> 4];
        hex += digits[std::uint8_t(c) & 15];
    }
    EXPECT_EQ(hex, "00000009010001000004d270630000002a02000000000000002a020018746578742f706c61696e3b636861727365743d"
                   "7574662d3868c3a96c6c6f0000000103");
}

TEST(ClipSync, HashIsFnv1a) {
    // The phone module computes the same number (its test pins it too).
    EXPECT_EQ(text("hello", 0).hash(), 0x6cd2490d6771879bull) << std::hex << text("hello", 0).hash();
}

TEST(ClipSync, NewestOfBothBecomesBothClipboards) {
    End pc("pc", {text("p3", 30), text("p2", 20), text("p1", 10)});
    End phone("phone", {text("f2", 25), text("f1", 5)});
    connect(pc, phone);
    EXPECT_TRUE(pc.session.synced());
    EXPECT_TRUE(phone.session.synced());
    EXPECT_EQ(pc.session.peerName(), "phone");
    // p3 is the newest: the PC keeps it, the phone takes it.
    EXPECT_TRUE(pc.clipboard.empty());
    EXPECT_EQ(phone.clipboard, std::vector<std::string>{"p3"});
    // Each gets the other's other clips, oldest first.
    EXPECT_EQ(pc.history, (std::vector<std::string>{"f1", "f2"}));
    EXPECT_EQ(phone.history, (std::vector<std::string>{"p1", "p2"}));
}

TEST(ClipSync, TheOtherSidesNewestWins) {
    End pc("pc", {text("p1", 10)});
    End phone("phone", {text("f1", 50)});
    connect(pc, phone);
    EXPECT_EQ(pc.clipboard, std::vector<std::string>{"f1"});
    EXPECT_TRUE(pc.history.empty());
    EXPECT_TRUE(phone.clipboard.empty());
    EXPECT_EQ(phone.history, std::vector<std::string>{"p1"});
}

TEST(ClipSync, SharedClipsAreNotAddedTwice) {
    End pc("pc", {text("same", 40), text("p1", 10)});
    End phone("phone", {text("same", 45), text("f1", 5)});
    connect(pc, phone);
    EXPECT_EQ(pc.history, std::vector<std::string>{"f1"});
    EXPECT_EQ(phone.history, std::vector<std::string>{"p1"});
    // Both hold "same" already.
    EXPECT_TRUE(pc.clipboard.empty());
    EXPECT_TRUE(phone.clipboard.empty());
}

TEST(ClipSync, NewestInHistoryButNotCurrentIsSetAgain) {
    // The phone copied "a" again recently; the PC has it, but further down.
    End pc("pc", {text("x", 30), text("a", 10)});
    End phone("phone", {text("a", 50)});
    connect(pc, phone);
    EXPECT_EQ(pc.clipboard, std::vector<std::string>{"a"});
    EXPECT_TRUE(pc.history.empty());
    EXPECT_EQ(phone.history, std::vector<std::string>{"x"});
}

TEST(ClipSync, EmptyEnds) {
    End pc("pc", {});
    End phone("phone", {text("f1", 5)});
    connect(pc, phone);
    EXPECT_EQ(pc.clipboard, std::vector<std::string>{"f1"});
    EXPECT_TRUE(phone.clipboard.empty());

    End a("a", {}), b("b", {});
    connect(a, b);
    EXPECT_TRUE(a.session.synced());
    EXPECT_TRUE(a.clipboard.empty() && b.clipboard.empty() && a.history.empty());
}

TEST(ClipSync, OnlyTheNewestFiveGoOver) {
    std::vector<Clip> many;
    for (int i = 9; i >= 0; i--)
        many.push_back(text("p" + std::to_string(i), i + 1));
    End pc("pc", many);
    End phone("phone", {});
    connect(pc, phone);
    EXPECT_EQ(phone.clipboard, std::vector<std::string>{"p9"});
    EXPECT_EQ(phone.history, (std::vector<std::string>{"p5", "p6", "p7", "p8"}));
}

TEST(ClipSync, LiveCopiesGoBothWays) {
    End pc("pc", {text("p1", 10)});
    End phone("phone", {text("f1", 5)});
    connect(pc, phone);
    pc.clipboard.clear();
    phone.clipboard.clear();

    pc.apply(pc.session.copied(text("from pc", 100)));
    pump(pc, phone);
    EXPECT_EQ(phone.clipboard, std::vector<std::string>{"from pc"});

    phone.apply(phone.session.copied(text("from phone", 200)));
    pump(pc, phone);
    EXPECT_EQ(pc.clipboard, std::vector<std::string>{"from phone"});
}

TEST(ClipSync, WhatThePeerHoldsIsNotSentBack) {
    End pc("pc", {text("p1", 10)});
    End phone("phone", {});
    connect(pc, phone);
    ASSERT_EQ(phone.clipboard, std::vector<std::string>{"p1"});
    // A platform that reports our own SetClipboard as a copy anyway.
    phone.apply(phone.session.copied(text("p1", 11)));
    EXPECT_TRUE(phone.outbox.empty());

    pc.apply(pc.session.copied(text("b", 20)));
    pump(pc, phone);
    phone.apply(phone.session.copied(text("b", 21)));
    EXPECT_TRUE(phone.outbox.empty());

    // Copying an older clip again is news.
    phone.apply(phone.session.copied(text("p1", 30)));
    pump(pc, phone);
    EXPECT_EQ(pc.clipboard.back(), "p1");
}

TEST(ClipSync, TheSmallerLimitDecides) {
    const std::string big(2000, 'b');
    End pc("pc", {text(big, 50), text("small", 10)}, 1000);
    End phone("phone", {text(big + "!", 60)});  // takes kMaxClip, but the PC takes 1000
    connect(pc, phone);
    // Neither big clip went over, and neither end gave up its own (newer)
    // clipboard for the PC's older one that fits: that only went to history.
    EXPECT_TRUE(pc.clipboard.empty());
    EXPECT_TRUE(phone.clipboard.empty());
    EXPECT_EQ(phone.history, std::vector<std::string>{"small"});

    pc.apply(pc.session.copied(text(big, 70)));
    EXPECT_TRUE(pc.outbox.empty());
    phone.apply(phone.session.copied(text(big, 80)));
    EXPECT_TRUE(phone.outbox.empty());
    phone.apply(phone.session.copied(text("fits", 90)));
    pump(pc, phone);
    EXPECT_EQ(pc.clipboard, std::vector<std::string>{"fits"});
}

TEST(ClipSync, CopiesDuringTheExchangeGoAfterIt) {
    End pc("pc", {text("p1", 10)});
    End phone("phone", {});
    pc.apply(pc.session.start());
    pc.apply(pc.session.copied(text("early", 20)));
    EXPECT_EQ(pc.outbox, helloFrame("pc"));
    phone.apply(phone.session.start());
    pump(pc, phone);
    EXPECT_EQ(phone.clipboard, (std::vector<std::string>{"p1", "early"}));
}

TEST(ClipSync, ProtocolViolationsClose) {
    Session clipFirst("pc", {});
    EXPECT_EQ(clipFirst.received(clipFrame(text("x", 1), Live)).back().kind, Action::Kind::Close);

    Session twoHellos("pc", {});
    twoHellos.received(helloFrame("a"));
    EXPECT_EQ(twoHellos.received(helloFrame("a")).back().kind, Action::Kind::Close);

    Session syncedTwice("pc", {});
    syncedTwice.received(helloFrame("a") + syncedFrame());
    EXPECT_EQ(syncedTwice.received(syncedFrame()).back().kind, Action::Kind::Close);

    Session garbage("pc", {});
    EXPECT_EQ(garbage.received(std::string("\xff\xff\xff\xff", 4)).back().kind, Action::Kind::Close);
}
