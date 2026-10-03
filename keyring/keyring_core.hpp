#pragma once
// atrium-keyring's storage and crypto, apart from D-Bus so they can be
// tested: a collection of secrets in one file, and the Secret Service's
// session encryption.
//
// A collection's file is JSON:
//   kdf     scrypt's parameters and salt: the key that opens the master key
//           comes from the password (the login password, for "login")
//   key     the master key (random), AES-256-GCM under that
//   index   each item's id and its attributes' values as salted SHA-256, so
//           an app can find its item while the collection is locked (and be
//           asked to unlock it) without the file saying what they are
//   items   labels, attributes and secrets: AES-256-GCM under the master key

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace atrium::keyring {

using Bytes = std::vector<uint8_t>;
using Attributes = std::map<std::string, std::string>;

struct Item {
    uint64_t id = 0;
    std::string label;
    Attributes attributes;
    Bytes secret;
    std::string content_type = "text/plain";
    int64_t created = 0, modified = 0;  // seconds since the epoch
};

struct KdfParams {
    uint64_t n = 1 << 15;  // ~32 MiB, ~0.1 s
    uint32_t r = 8, p = 1;
};

class Collection {
public:
    // A new one, unlocked, opened by `password` from then on.
    static Collection create(const std::string& label, const std::string& password, int64_t now,
                             KdfParams kdf = {});
    // From its file's text, locked; nullopt (and why) if it isn't one.
    static std::optional<Collection> parse(const std::string& text, std::string* error);
    // Its file's text (unlocked only: the items need the key).
    std::optional<std::string> serialize() const;

    bool locked() const { return !key_; }
    // False if `password` isn't the one.
    bool unlock(const std::string& password);
    void lock();
    // Opened by `password` from now on (unlocked only).
    bool set_password(const std::string& password);

    // Items whose attributes include all of `match` (all items for none);
    // works locked too.
    std::vector<uint64_t> search(const Attributes& match) const;
    // Unlocked only (nullptr otherwise, or no such item).
    const Item* item(uint64_t id) const;
    std::vector<uint64_t> ids() const;
    // An item with the same attributes is replaced when `replace` (as
    // CreateItem does); its id either way. 0 when locked.
    uint64_t put(Item item, bool replace, int64_t now);
    bool update(const Item& item, int64_t now);
    bool remove(uint64_t id, int64_t now);

    std::string label;
    int64_t created = 0, modified = 0;

private:
    struct Indexed {
        uint64_t id;
        std::map<std::string, std::string> hashes;  // name: hash of the value
    };
    std::string hash(const std::string& name, const std::string& value) const;
    void reindex();

    KdfParams kdf_;
    Bytes salt_;
    Bytes wrapped_key_, key_nonce_;  // the master key, sealed
    Bytes items_sealed_, items_nonce_;
    std::optional<Bytes> key_;       // the master key, while unlocked
    std::vector<Item> items_;        // while unlocked
    std::vector<Indexed> index_;
    uint64_t next_id_ = 1;
};

// --- the Secret Service's sessions -------------------------------------------------

// "plain", or "dh-ietf1024-sha256-aes128-cbc-pkcs7": a Diffie-Hellman
// exchange (RFC 2409's second Oakley group) whose shared secret, through
// HKDF-SHA256, is an AES-128 key; secrets go over D-Bus as CBC with PKCS#7
// padding and a fresh IV each.
class Session {
public:
    // For an OpenSession: the algorithm's name and the client's input;
    // nullopt for an algorithm that isn't one of these, or bad input.
    // `output` is what the reply carries (our public key, or nothing).
    static std::optional<Session> open(const std::string& algorithm, const Bytes& input, Bytes* output);

    bool plain() const { return !key_; }
    // A secret for the client: parameters (the IV) and value.
    void encrypt(const Bytes& secret, Bytes* parameters, Bytes* value) const;
    // A secret from the client; nullopt if it doesn't decrypt.
    std::optional<Bytes> decrypt(const Bytes& parameters, const Bytes& value) const;

    // The client's half, for tests (and nothing else): a key pair, and the
    // AES key from its private key and the server's public one.
    static Bytes test_client_keys(Bytes* private_key);
    static std::optional<Bytes> test_client_aes_key(const Bytes& private_key, const Bytes& server_public);
    explicit Session(std::optional<Bytes> key) : key_(std::move(key)) {}

private:
    std::optional<Bytes> key_;  // AES-128, for dh
};

// AES-128-CBC with PKCS#7, as the dh session sends secrets (also for tests).
Bytes aes128_cbc(const Bytes& key, const Bytes& iv, const Bytes& data, bool encrypt, bool* ok);

Bytes random_bytes(size_t n);

} // namespace atrium::keyring
