#pragma once
// The hashes and signatures of level packages (package.h): SHA-256 / SHA-512 and random bytes from
// Windows (CNG, bcrypt.dll), Ed25519 (RFC 8032) from Monocypher (third_party/monocypher, ed25519.cpp;
// checked against the RFC's vectors and Python's `cryptography` by h2/tests/package_test.py).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace h2::levelfmt::crypto {

using Sha256Digest = std::array<uint8_t, 32>;
using Sha512Digest = std::array<uint8_t, 64>;

Sha256Digest Sha256(const void* data, size_t size);
Sha512Digest Sha512(const void* data, size_t size);
// Random bytes from the system's generator; false when it fails (nothing is made of them then).
bool RandomBytes(void* out, size_t size);

std::string Hex(const uint8_t* data, size_t size);  // lowercase
// Exactly `size` bytes of lowercase hex; false otherwise.
bool FromHex(const std::string& hex, uint8_t* out, size_t size);

// Ed25519: a key is its 32-byte seed (secret) and the 32-byte public key made from it.
void Ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32]);
void Ed25519Sign(uint8_t sig[64], const uint8_t* msg, size_t size, const uint8_t seed[32], const uint8_t pub[32]);
// Also refuses a signature whose S is not below the group's order (no second form of one signature) and a
// public key that is not a point of the curve.
bool Ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t size, const uint8_t pub[32]);
// The RFC 8032 test vectors (levelpack selftest).
bool Ed25519SelfTest();

} // namespace h2::levelfmt::crypto
