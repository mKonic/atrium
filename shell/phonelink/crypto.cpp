#include "crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <memory>
#include <stdexcept>

namespace atrium::phonelink {

namespace {

const unsigned char* bytes(std::string_view s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("libcrypto: ") + what);
}

struct PkeyFree {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct PkeyCtxFree {
    void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
};
struct CipherCtxFree {
    void operator()(EVP_CIPHER_CTX* p) const { EVP_CIPHER_CTX_free(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree>;
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxFree>;

Pkey privateKey(std::string_view priv) {
    if (priv.size() != 32)
        fail("X25519 private key is not 32 bytes");
    Pkey k(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, bytes(priv), priv.size()));
    if (!k)
        fail("X25519 private key");
    return k;
}

} // namespace

std::string sha256(std::string_view data) {
    std::string out(SHA256_DIGEST_LENGTH, '\0');
    SHA256(bytes(data), data.size(), reinterpret_cast<unsigned char*>(out.data()));
    return out;
}

std::string hmac(std::string_view key, std::string_view data) {
    std::string out(32, '\0');
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(), key.data(), int(key.size()), bytes(data), data.size(),
              reinterpret_cast<unsigned char*>(out.data()), &len))
        fail("HMAC");
    return out;
}

std::string hkdf(std::string_view ikm, std::string_view salt, std::string_view info, std::size_t length) {
    PkeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
    std::string out(length, '\0');
    std::size_t len = length;
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) <= 0
        || EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), bytes(salt), int(salt.size())) <= 0
        || EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), bytes(ikm), int(ikm.size())) <= 0
        || EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), bytes(info), int(info.size())) <= 0
        || EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len) <= 0 || len != length)
        fail("HKDF");
    return out;
}

std::string x25519Public(std::string_view priv) {
    Pkey k = privateKey(priv);
    std::string out(32, '\0');
    std::size_t len = out.size();
    if (EVP_PKEY_get_raw_public_key(k.get(), reinterpret_cast<unsigned char*>(out.data()), &len) <= 0 || len != 32)
        fail("X25519 public key");
    return out;
}

std::string x25519(std::string_view priv, std::string_view peerPublic) {
    if (peerPublic.size() != 32)
        return {};
    Pkey k = privateKey(priv);
    Pkey peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, bytes(peerPublic), peerPublic.size()));
    if (!peer)
        return {};
    PkeyCtx ctx(EVP_PKEY_CTX_new(k.get(), nullptr));
    std::string out(32, '\0');
    std::size_t len = out.size();
    // Derivation fails on an all-zero result (RFC 7748 section 6.1).
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 || EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) <= 0
        || EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len) <= 0 || len != 32)
        return {};
    return out;
}

std::string seal(std::string_view key, const Nonce& nonce, std::string_view aad, std::string_view plain) {
    if (key.size() != 32)
        fail("AES-256 key is not 32 bytes");
    CipherCtx ctx(EVP_CIPHER_CTX_new());
    std::string out(plain.size() + kTag, '\0');
    auto* o = reinterpret_cast<unsigned char*>(out.data());
    int len = 0, total = 0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, bytes(key), nonce.data()) != 1
        || (!aad.empty() && EVP_EncryptUpdate(ctx.get(), nullptr, &len, bytes(aad), int(aad.size())) != 1))
        fail("AES-GCM init");
    if (!plain.empty() && EVP_EncryptUpdate(ctx.get(), o, &len, bytes(plain), int(plain.size())) != 1)
        fail("AES-GCM");
    total = plain.empty() ? 0 : len;
    if (EVP_EncryptFinal_ex(ctx.get(), o + total, &len) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, int(kTag), o + plain.size()) != 1)
        fail("AES-GCM final");
    return out;
}

std::optional<std::string> open(std::string_view key, const Nonce& nonce, std::string_view aad, std::string_view sealed) {
    if (key.size() != 32 || sealed.size() < kTag)
        return std::nullopt;
    const std::size_t n = sealed.size() - kTag;
    CipherCtx ctx(EVP_CIPHER_CTX_new());
    std::string out(n, '\0');
    auto* o = reinterpret_cast<unsigned char*>(out.data());
    int len = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, bytes(key), nonce.data()) != 1
        || (!aad.empty() && EVP_DecryptUpdate(ctx.get(), nullptr, &len, bytes(aad), int(aad.size())) != 1)
        || (n && EVP_DecryptUpdate(ctx.get(), o, &len, bytes(sealed), int(n)) != 1))
        return std::nullopt;
    std::string tag(sealed.substr(n));
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, int(kTag), tag.data()) != 1
        || EVP_DecryptFinal_ex(ctx.get(), o + (n ? len : 0), &len) != 1)
        return std::nullopt;
    return out;
}

std::string random(std::size_t n) {
    std::string out(n, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), int(n)) != 1)
        fail("RAND_bytes");
    return out;
}

Nonce nonce(std::uint32_t label, std::uint64_t counter) {
    Nonce n{};
    for (int i = 0; i < 4; i++)
        n[i] = std::uint8_t(label >> (24 - 8 * i));
    for (int i = 0; i < 8; i++)
        n[4 + i] = std::uint8_t(counter >> (56 - 8 * i));
    return n;
}

bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace atrium::phonelink
