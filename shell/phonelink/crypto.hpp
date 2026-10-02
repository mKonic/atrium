#pragma once
// The few primitives the phone link uses, over OpenSSL's libcrypto. The
// phone's module does the same with the JDK's providers; both test suites
// pin the same known answers.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace atrium::phonelink {

using Nonce = std::array<std::uint8_t, 12>;

inline constexpr std::size_t kTag = 16;  // AES-GCM's

std::string sha256(std::string_view data);
std::string hmac(std::string_view key, std::string_view data);
// RFC 5869, SHA-256.
std::string hkdf(std::string_view ikm, std::string_view salt, std::string_view info, std::size_t length);

// X25519. A private key is 32 random bytes; RFC 7748 clamps it.
std::string x25519Public(std::string_view priv);
// Empty when the peer's key is all-zero output (a low-order point).
std::string x25519(std::string_view priv, std::string_view peerPublic);

// AES-256-GCM: ciphertext followed by the 16-byte tag.
std::string seal(std::string_view key, const Nonce& nonce, std::string_view aad, std::string_view plain);
std::optional<std::string> open(std::string_view key, const Nonce& nonce, std::string_view aad, std::string_view sealed);

// Bytes from the system's CSPRNG.
std::string random(std::size_t n);

// A 12-byte nonce: a 4-byte label, then a counter (big endian).
Nonce nonce(std::uint32_t label, std::uint64_t counter);

// Constant-time comparison of two equally long strings.
bool same(std::string_view a, std::string_view b);

} // namespace atrium::phonelink
