#include "keyring_core.hpp"

#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <memory>

using json = nlohmann::json;

namespace atrium::keyring {

namespace {

// --- small helpers -------------------------------------------------------------------

std::string to_hex(const uint8_t* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += digits[d[i] >> 4];
        s += digits[d[i] & 15];
    }
    return s;
}

std::string b64(const Bytes& b) {
    std::string out(4 * ((b.size() + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), b.data(), int(b.size()));
    out.resize(size_t(n));
    return out;
}

std::optional<Bytes> unb64(const std::string& s) {
    if (s.size() % 4)
        return std::nullopt;
    Bytes out(s.size() / 4 * 3);
    const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(s.data()), int(s.size()));
    if (n < 0)
        return std::nullopt;
    size_t len = size_t(n);
    // EVP_DecodeBlock counts the padding's bytes too.
    if (!s.empty() && s.back() == '=')
        --len;
    if (s.size() > 1 && s[s.size() - 2] == '=')
        --len;
    out.resize(len);
    return out;
}

void wipe(Bytes& b) {
    OPENSSL_cleanse(b.data(), b.size());
    b.clear();
}

std::optional<Bytes> scrypt(const std::string& password, const Bytes& salt, const KdfParams& k) {
    Bytes key(32);
    if (!EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), k.n, k.r, k.p,
                        size_t(1) << 30, key.data(), key.size()))
        return std::nullopt;
    return key;
}

constexpr size_t kTag = 16;

// AES-256-GCM: the ciphertext with its tag after it.
Bytes seal(const Bytes& key, const Bytes& nonce, const Bytes& plain) {
    Bytes out(plain.size() + kTag);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, m = 0;
    EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce.data());
    EVP_EncryptUpdate(c.get(), out.data(), &n, plain.data(), int(plain.size()));
    EVP_EncryptFinal_ex(c.get(), out.data() + n, &m);
    EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, int(kTag), out.data() + plain.size());
    return out;
}

std::optional<Bytes> open_sealed(const Bytes& key, const Bytes& nonce, const Bytes& sealed) {
    if (sealed.size() < kTag || key.size() != 32 || nonce.size() != 12)
        return std::nullopt;
    const size_t len = sealed.size() - kTag;
    Bytes out(len);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, m = 0;
    EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, key.data(), nonce.data());
    EVP_DecryptUpdate(c.get(), out.data(), &n, sealed.data(), int(len));
    EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, int(kTag), const_cast<uint8_t*>(sealed.data() + len));
    if (EVP_DecryptFinal_ex(c.get(), out.data() + n, &m) <= 0) {
        wipe(out);
        return std::nullopt;
    }
    return out;
}

json item_json(const Item& i) {
    return {{"id", i.id},           {"label", i.label},      {"attributes", i.attributes},
            {"secret", b64(i.secret)}, {"content_type", i.content_type}, {"created", i.created},
            {"modified", i.modified}};
}

std::optional<Item> item_from(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    Item i;
    i.id = j.value("id", uint64_t(0));
    i.label = j.value("label", "");
    if (j.contains("attributes") && j["attributes"].is_object())
        for (auto& [k, v] : j["attributes"].items())
            if (v.is_string())
                i.attributes[k] = v;
    auto secret = unb64(j.value("secret", ""));
    if (!secret || !i.id)
        return std::nullopt;
    i.secret = std::move(*secret);
    i.content_type = j.value("content_type", "text/plain");
    i.created = j.value("created", int64_t(0));
    i.modified = j.value("modified", int64_t(0));
    return i;
}

} // namespace

Bytes random_bytes(size_t n) {
    Bytes b(n);
    RAND_bytes(b.data(), int(n));
    return b;
}

// --- Collection ---------------------------------------------------------------------

Collection Collection::create(const std::string& label, const std::string& password, int64_t now, KdfParams kdf) {
    Collection c;
    c.label = label;
    c.created = c.modified = now;
    c.kdf_ = kdf;
    c.salt_ = random_bytes(16);
    c.key_ = random_bytes(32);
    c.set_password(password);
    return c;
}

bool Collection::set_password(const std::string& password) {
    if (!key_)
        return false;
    salt_ = random_bytes(16);
    auto kek = scrypt(password, salt_, kdf_);
    if (!kek)
        return false;
    key_nonce_ = random_bytes(12);
    wrapped_key_ = seal(*kek, key_nonce_, *key_);
    wipe(*kek);
    reindex();  // the salt is the index's too
    return true;
}

bool Collection::unlock(const std::string& password) {
    if (key_)
        return true;
    auto kek = scrypt(password, salt_, kdf_);
    if (!kek)
        return false;
    auto key = open_sealed(*kek, key_nonce_, wrapped_key_);
    wipe(*kek);
    if (!key)
        return false;
    std::vector<Item> items;
    if (!items_sealed_.empty()) {
        auto plain = open_sealed(*key, items_nonce_, items_sealed_);
        if (!plain)
            return false;
        const json j = json::parse(plain->begin(), plain->end(), nullptr, false);
        wipe(*plain);
        if (!j.is_array())
            return false;
        for (const json& e : j)
            if (auto i = item_from(e))
                items.push_back(std::move(*i));
    }
    key_ = std::move(key);
    items_ = std::move(items);
    for (const Item& i : items_)
        next_id_ = std::max(next_id_, i.id + 1);
    return true;
}

void Collection::lock() {
    if (!key_)
        return;
    // What's in memory goes; the file's sealed copy is current (serialize
    // keeps it so).
    if (auto text = serialize()) {
        const json j = json::parse(*text);
        items_sealed_ = *unb64(j["items"]["data"].get<std::string>());
        items_nonce_ = *unb64(j["items"]["nonce"].get<std::string>());
    }
    for (Item& i : items_)
        wipe(i.secret);
    items_.clear();
    wipe(*key_);
    key_.reset();
}

std::string Collection::hash(const std::string& name, const std::string& value) const {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> c(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    const uint8_t zero = 0;
    uint8_t out[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    EVP_DigestInit_ex(c.get(), EVP_sha256(), nullptr);
    EVP_DigestUpdate(c.get(), salt_.data(), salt_.size());
    EVP_DigestUpdate(c.get(), name.data(), name.size());
    EVP_DigestUpdate(c.get(), &zero, 1);
    EVP_DigestUpdate(c.get(), value.data(), value.size());
    EVP_DigestFinal_ex(c.get(), out, &len);
    return to_hex(out, len);
}

void Collection::reindex() {
    if (!key_)
        return;
    index_.clear();
    for (const Item& i : items_) {
        Indexed x{i.id, {}};
        for (const auto& [k, v] : i.attributes)
            x.hashes[k] = hash(k, v);
        index_.push_back(std::move(x));
    }
}

std::vector<uint64_t> Collection::search(const Attributes& match) const {
    std::vector<uint64_t> out;
    for (const Indexed& x : index_) {
        bool all = true;
        for (const auto& [k, v] : match) {
            auto it = x.hashes.find(k);
            if (it == x.hashes.end() || it->second != hash(k, v)) {
                all = false;
                break;
            }
        }
        if (all)
            out.push_back(x.id);
    }
    return out;
}

const Item* Collection::item(uint64_t id) const {
    for (const Item& i : items_)
        if (i.id == id)
            return &i;
    return nullptr;
}

std::vector<uint64_t> Collection::ids() const {
    std::vector<uint64_t> out;
    for (const Indexed& x : index_)
        out.push_back(x.id);
    return out;
}

uint64_t Collection::put(Item item, bool replace, int64_t now) {
    if (!key_)
        return 0;
    if (replace)
        for (Item& i : items_)
            if (i.attributes == item.attributes) {
                i.label = item.label;
                i.secret = std::move(item.secret);
                i.content_type = item.content_type;
                i.modified = now;
                modified = now;
                return i.id;
            }
    item.id = next_id_++;
    item.created = item.created ? item.created : now;
    item.modified = item.modified ? item.modified : now;
    items_.push_back(std::move(item));
    modified = now;
    reindex();
    return items_.back().id;
}

bool Collection::update(const Item& item, int64_t now) {
    for (Item& i : items_)
        if (i.id == item.id) {
            i.label = item.label;
            i.attributes = item.attributes;
            i.secret = item.secret;
            i.content_type = item.content_type;
            i.modified = now;
            modified = now;
            reindex();
            return true;
        }
    return false;
}

bool Collection::remove(uint64_t id, int64_t now) {
    if (!key_)
        return false;
    auto it = std::ranges::find_if(items_, [id](const Item& i) { return i.id == id; });
    if (it == items_.end())
        return false;
    wipe(it->secret);
    items_.erase(it);
    modified = now;
    reindex();
    return true;
}

std::optional<std::string> Collection::serialize() const {
    if (!key_)
        return std::nullopt;
    json items = json::array();
    for (const Item& i : items_)
        items.push_back(item_json(i));
    std::string text = items.dump();
    Bytes plain(text.begin(), text.end());
    OPENSSL_cleanse(text.data(), text.size());
    const Bytes nonce = random_bytes(12);
    const Bytes sealed = seal(*key_, nonce, plain);
    wipe(plain);
    json index = json::array();
    for (const Indexed& x : index_)
        index.push_back({{"id", x.id}, {"hashes", x.hashes}});
    const json j{
        {"format", 1},
        {"label", label},
        {"created", created},
        {"modified", modified},
        {"kdf", {{"name", "scrypt"}, {"n", kdf_.n}, {"r", kdf_.r}, {"p", kdf_.p}, {"salt", b64(salt_)}}},
        {"key", {{"nonce", b64(key_nonce_)}, {"data", b64(wrapped_key_)}}},
        {"index", index},
        {"next_id", next_id_},
        {"items", {{"nonce", b64(nonce)}, {"data", b64(sealed)}}},
    };
    return j.dump(1);
}

std::optional<Collection> Collection::parse(const std::string& text, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<Collection> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("format", 0) != 1)
        return fail("not an atrium keyring");
    try {
        Collection c;
        c.label = j.value("label", "");
        c.created = j.value("created", int64_t(0));
        c.modified = j.value("modified", int64_t(0));
        const json& k = j.at("kdf");
        if (k.value("name", "") != "scrypt")
            return fail("an unknown key derivation");
        c.kdf_ = {k.at("n").get<uint64_t>(), k.at("r").get<uint32_t>(), k.at("p").get<uint32_t>()};
        auto salt = unb64(k.at("salt").get<std::string>());
        auto knonce = unb64(j.at("key").at("nonce").get<std::string>());
        auto kdata = unb64(j.at("key").at("data").get<std::string>());
        auto inonce = unb64(j.at("items").at("nonce").get<std::string>());
        auto idata = unb64(j.at("items").at("data").get<std::string>());
        if (!salt || !knonce || !kdata || !inonce || !idata)
            return fail("damaged");
        c.salt_ = *salt;
        c.key_nonce_ = *knonce;
        c.wrapped_key_ = *kdata;
        c.items_nonce_ = *inonce;
        c.items_sealed_ = *idata;
        c.next_id_ = j.value("next_id", uint64_t(1));
        for (const json& x : j.at("index")) {
            Indexed i{x.at("id").get<uint64_t>(), {}};
            for (auto& [name, h] : x.at("hashes").items())
                i.hashes[name] = h.get<std::string>();
            c.index_.push_back(std::move(i));
        }
        return c;
    } catch (const json::exception&) {
        return fail("damaged");
    }
}

// --- sessions ----------------------------------------------------------------------

namespace {

// RFC 2409, 6.2: the second Oakley group (1024 bits), generator 2.
const char* kPrime =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF";
constexpr size_t kPrimeBytes = 128;

struct Bn {
    BIGNUM* p = BN_new();
    ~Bn() { BN_clear_free(p); }
    Bn() = default;
    Bn(const Bn&) = delete;
    Bn& operator=(const Bn&) = delete;
};

Bytes bn_bytes(const BIGNUM* n, size_t size) {
    Bytes out(size);
    BN_bn2binpad(n, out.data(), int(size));
    return out;
}

// (base^exp mod p) as `size` big-endian bytes.
std::optional<Bytes> mod_exp(const Bytes& base, const Bytes& exp) {
    Bn p, b, e, r;
    BN_hex2bn(&p.p, kPrime);
    BN_bin2bn(base.data(), int(base.size()), b.p);
    BN_bin2bn(exp.data(), int(exp.size()), e.p);
    // A public key of 0, 1 or p-1 (or out of range) gives a known secret.
    Bn one, top;
    BN_one(one.p);
    BN_sub(top.p, p.p, one.p);
    if (BN_cmp(b.p, one.p) <= 0 || BN_cmp(b.p, top.p) >= 0)
        return std::nullopt;
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    if (!BN_mod_exp(r.p, b.p, e.p, p.p, ctx.get()))
        return std::nullopt;
    return bn_bytes(r.p, kPrimeBytes);
}

Bytes public_from(const Bytes& private_key) {
    Bn p, g, e, r;
    BN_hex2bn(&p.p, kPrime);
    BN_set_word(g.p, 2);
    BN_bin2bn(private_key.data(), int(private_key.size()), e.p);
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    BN_mod_exp(r.p, g.p, e.p, p.p, ctx.get());
    return bn_bytes(r.p, kPrimeBytes);
}

// HKDF-SHA256 with no salt and no info, 16 bytes (as gnome-keyring and
// libsecret derive the AES key).
std::optional<Bytes> hkdf16(const Bytes& secret) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> c(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr),
                                                                   EVP_PKEY_CTX_free);
    Bytes out(16);
    size_t len = out.size();
    if (EVP_PKEY_derive_init(c.get()) <= 0 || EVP_PKEY_CTX_set_hkdf_md(c.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_key(c.get(), secret.data(), int(secret.size())) <= 0 ||
        EVP_PKEY_derive(c.get(), out.data(), &len) <= 0)
        return std::nullopt;
    return out;
}

} // namespace

Bytes aes128_cbc(const Bytes& key, const Bytes& iv, const Bytes& data, bool encrypt, bool* ok) {
    *ok = false;
    if (key.size() != 16 || iv.size() != 16)
        return {};
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    Bytes out(data.size() + 16);
    int n = 0, m = 0;
    if (!EVP_CipherInit_ex(c.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv.data(), encrypt ? 1 : 0) ||
        !EVP_CipherUpdate(c.get(), out.data(), &n, data.data(), int(data.size())) ||
        !EVP_CipherFinal_ex(c.get(), out.data() + n, &m))
        return {};
    out.resize(size_t(n + m));
    *ok = true;
    return out;
}

std::optional<Session> Session::open(const std::string& algorithm, const Bytes& input, Bytes* output) {
    output->clear();
    if (algorithm == "plain")
        return Session(std::nullopt);
    if (algorithm != "dh-ietf1024-sha256-aes128-cbc-pkcs7" || input.empty() || input.size() > kPrimeBytes)
        return std::nullopt;
    Bytes priv = random_bytes(kPrimeBytes);
    auto shared = mod_exp(input, priv);
    if (!shared) {
        wipe(priv);
        return std::nullopt;
    }
    *output = public_from(priv);
    wipe(priv);
    auto key = hkdf16(*shared);
    wipe(*shared);
    if (!key)
        return std::nullopt;
    return Session(std::move(*key));
}

void Session::encrypt(const Bytes& secret, Bytes* parameters, Bytes* value) const {
    if (!key_) {
        parameters->clear();
        *value = secret;
        return;
    }
    *parameters = random_bytes(16);
    bool ok = false;
    *value = aes128_cbc(*key_, *parameters, secret, true, &ok);
}

std::optional<Bytes> Session::decrypt(const Bytes& parameters, const Bytes& value) const {
    if (!key_)
        return value;
    bool ok = false;
    Bytes out = aes128_cbc(*key_, parameters, value, false, &ok);
    if (!ok)
        return std::nullopt;
    return out;
}

Bytes Session::test_client_keys(Bytes* private_key) {
    *private_key = random_bytes(kPrimeBytes);
    return public_from(*private_key);
}

std::optional<Bytes> Session::test_client_aes_key(const Bytes& private_key, const Bytes& server_public) {
    auto shared = mod_exp(server_public, private_key);
    if (!shared)
        return std::nullopt;
    return hkdf16(*shared);
}

} // namespace atrium::keyring
