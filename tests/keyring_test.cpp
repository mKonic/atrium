#include "keyring_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::keyring;

namespace {

// Cheap scrypt: the tests make many.
constexpr KdfParams kFast{1 << 10, 8, 1};

Bytes bytes(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

Item item(const std::string& label, Attributes a, const std::string& secret) {
    Item i;
    i.label = label;
    i.attributes = std::move(a);
    i.secret = bytes(secret);
    return i;
}

} // namespace

TEST(Keyring, RoundTripsThroughItsFile) {
    Collection c = Collection::create("Login", "hunter2", 100, kFast);
    EXPECT_FALSE(c.locked());
    const uint64_t a = c.put(item("Chrome Safe Storage", {{"application", "chrome"}}, "k3y"), false, 101);
    const uint64_t b = c.put(item("mail", {{"server", "imap.x"}, {"user", "me"}}, "pw"), false, 102);
    EXPECT_NE(a, b);
    const auto text = c.serialize();
    ASSERT_TRUE(text);
    // Nothing said in the clear: the secret, the label, the attribute values.
    for (const char* s : {"k3y", "Chrome Safe Storage", "chrome", "imap.x"})
        EXPECT_EQ(text->find(s), std::string::npos) << s;

    std::string why;
    auto back = Collection::parse(*text, &why);
    ASSERT_TRUE(back) << why;
    EXPECT_TRUE(back->locked());
    EXPECT_EQ(back->label, "Login");
    EXPECT_EQ(back->item(a), nullptr);  // locked: no secrets
    EXPECT_FALSE(back->unlock("hunter3"));
    EXPECT_TRUE(back->locked());
    ASSERT_TRUE(back->unlock("hunter2"));
    ASSERT_NE(back->item(a), nullptr);
    EXPECT_EQ(back->item(a)->secret, bytes("k3y"));
    EXPECT_EQ(back->item(b)->attributes.at("user"), "me");
    EXPECT_EQ(back->item(a)->created, 101);
}

TEST(Keyring, FindsItemsWhileLocked) {
    Collection c = Collection::create("Login", "pw", 1, kFast);
    const uint64_t a = c.put(item("x", {{"application", "chrome"}, {"xdg:schema", "chrome_libsecret_os_crypt_password_v2"}}, "s"), false, 1);
    c.put(item("y", {{"application", "code-oss"}}, "t"), false, 1);
    auto locked = Collection::parse(*c.serialize(), nullptr);
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->search({{"application", "chrome"}}), std::vector<uint64_t>{a});
    EXPECT_TRUE(locked->search({{"application", "firefox"}}).empty());
    EXPECT_TRUE(locked->search({{"application", "chrome"}, {"other", "1"}}).empty());
    EXPECT_EQ(locked->search({}).size(), 2u);  // everything
}

TEST(Keyring, CreateItemReplacesTheSameAttributes) {
    Collection c = Collection::create("Login", "pw", 1, kFast);
    const uint64_t a = c.put(item("v1", {{"k", "v"}}, "one"), true, 1);
    const uint64_t b = c.put(item("v2", {{"k", "v"}}, "two"), true, 2);
    EXPECT_EQ(a, b);
    EXPECT_EQ(c.item(a)->secret, bytes("two"));
    EXPECT_EQ(c.item(a)->label, "v2");
    const uint64_t d = c.put(item("v3", {{"k", "v"}}, "three"), false, 3);
    EXPECT_NE(d, a);  // without replace: another
    EXPECT_TRUE(c.remove(a, 4));
    EXPECT_EQ(c.item(a), nullptr);
    EXPECT_EQ(c.search({{"k", "v"}}), std::vector<uint64_t>{d});
}

TEST(Keyring, NewPasswordOpensItOldDoesNot) {
    Collection c = Collection::create("Login", "old", 1, kFast);
    const uint64_t a = c.put(item("x", {{"k", "v"}}, "s"), false, 1);
    ASSERT_TRUE(c.set_password("new"));
    auto back = Collection::parse(*c.serialize(), nullptr);
    EXPECT_FALSE(back->unlock("old"));
    ASSERT_TRUE(back->unlock("new"));
    EXPECT_EQ(back->item(a)->secret, bytes("s"));
    EXPECT_EQ(back->search({{"k", "v"}}), std::vector<uint64_t>{a});  // the index follows the new salt
}

TEST(Keyring, LockForgetsAndUnlockBrings) {
    Collection c = Collection::create("Login", "pw", 1, kFast);
    const uint64_t a = c.put(item("x", {{"k", "v"}}, "s"), false, 1);
    c.lock();
    EXPECT_TRUE(c.locked());
    EXPECT_EQ(c.item(a), nullptr);
    EXPECT_EQ(c.put(item("y", {}, "z"), false, 2), 0u);  // locked: nothing added
    EXPECT_FALSE(c.serialize());
    ASSERT_TRUE(c.unlock("pw"));
    EXPECT_EQ(c.item(a)->secret, bytes("s"));
}

TEST(Keyring, DamagedFilesRefused) {
    std::string why;
    EXPECT_FALSE(Collection::parse("{}", &why));
    EXPECT_FALSE(Collection::parse("not json", &why));
    Collection c = Collection::create("Login", "pw", 1, kFast);
    c.put(item("x", {}, "s"), false, 1);
    std::string text = *c.serialize();
    // A flipped byte in the sealed items: the tag catches it.
    const auto at = text.find("\"data\"", text.find("\"items\"")) + 10;
    text[at] = text[at] == 'A' ? 'B' : 'A';
    auto back = Collection::parse(text, &why);
    ASSERT_TRUE(back);
    EXPECT_FALSE(back->unlock("pw"));
}

TEST(Keyring, PlainSessionPassesSecretsThrough) {
    Bytes out;
    auto s = Session::open("plain", {}, &out);
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->plain());
    EXPECT_TRUE(out.empty());
    Bytes params, value;
    s->encrypt(bytes("secret"), &params, &value);
    EXPECT_TRUE(params.empty());
    EXPECT_EQ(value, bytes("secret"));
    EXPECT_FALSE(Session::open("rot13", {}, &out));
}

TEST(Keyring, DhSessionAgreesWithTheClient) {
    Bytes client_private;
    const Bytes client_public = Session::test_client_keys(&client_private);
    Bytes server_public;
    auto s = Session::open("dh-ietf1024-sha256-aes128-cbc-pkcs7", client_public, &server_public);
    ASSERT_TRUE(s);
    EXPECT_FALSE(s->plain());
    EXPECT_EQ(server_public.size(), 128u);
    const auto key = Session::test_client_aes_key(client_private, server_public);
    ASSERT_TRUE(key);

    // Server to client.
    Bytes iv, value;
    s->encrypt(bytes("Chrome's key"), &iv, &value);
    EXPECT_EQ(iv.size(), 16u);
    EXPECT_EQ(value.size() % 16, 0u);
    bool ok = false;
    EXPECT_EQ(aes128_cbc(*key, iv, value, false, &ok), bytes("Chrome's key"));
    EXPECT_TRUE(ok);

    // Client to server.
    const Bytes civ = random_bytes(16);
    const Bytes sent = aes128_cbc(*key, civ, bytes("new password"), true, &ok);
    EXPECT_EQ(s->decrypt(civ, sent), bytes("new password"));
    Bytes wrong = sent;
    wrong.back() ^= 1;
    EXPECT_NE(s->decrypt(civ, wrong), std::optional<Bytes>(bytes("new password")));
}

TEST(Keyring, DhRefusesWeakPublicKeys) {
    Bytes out;
    EXPECT_FALSE(Session::open("dh-ietf1024-sha256-aes128-cbc-pkcs7", {}, &out));
    EXPECT_FALSE(Session::open("dh-ietf1024-sha256-aes128-cbc-pkcs7", {1}, &out));
    EXPECT_FALSE(Session::open("dh-ietf1024-sha256-aes128-cbc-pkcs7", Bytes(129, 0xff), &out));
}
