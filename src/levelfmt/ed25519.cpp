// Ed25519 (RFC 8032) from Monocypher 4.0.3 (third_party/monocypher: audited, constant time); this
// file only fits it to crypto.h.
#include <cstring>
#include <vector>

#include "crypto.h"
#include "monocypher-ed25519.h"

namespace h2::levelfmt::crypto {

void Ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32])
{
    uint8_t s[32], secret[64];
    memcpy(s, seed, 32);  // (key_pair wipes the seed it is given)
    crypto_ed25519_key_pair(secret, pub, s);
    crypto_wipe(secret, sizeof secret);
}

void Ed25519Sign(uint8_t sig[64], const uint8_t* msg, size_t size, const uint8_t seed[32], const uint8_t pub[32])
{
    // the secret key with the public half made from the seed again: a public key given with another seed
    // would make Monocypher's signature give the secret away
    uint8_t s[32], secret[64], made[32];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(secret, made, s);
    (void)pub;
    crypto_ed25519_sign(sig, secret, msg, size);
    crypto_wipe(secret, sizeof secret);
}

bool Ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t size, const uint8_t pub[32])
{
    return crypto_ed25519_check(sig, pub, msg, size) == 0;  // (S >= L refused too)
}

bool Ed25519SelfTest()
{
    struct Vector {
        const char *seed, *pub, *msg, *sig;
    };
    // RFC 8032 section 7.1, tests 1-3
    static const Vector vectors[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const Vector& v : vectors) {
        uint8_t seed[32], pub[32], want[64], sig[64], made[32];
        std::vector<uint8_t> msg(strlen(v.msg) / 2);
        if (!FromHex(v.seed, seed, 32) || !FromHex(v.pub, pub, 32) || !FromHex(v.sig, want, 64) ||
            !FromHex(v.msg, msg.data(), msg.size()))
            return false;
        Ed25519PublicKey(seed, made);
        if (memcmp(made, pub, 32) != 0)
            return false;
        Ed25519Sign(sig, msg.data(), msg.size(), seed, pub);
        if (memcmp(sig, want, 64) != 0 || !Ed25519Verify(sig, msg.data(), msg.size(), pub))
            return false;
        sig[10] ^= 1;  // a changed signature
        if (Ed25519Verify(sig, msg.data(), msg.size(), pub))
            return false;
    }
    return true;
}

} // namespace h2::levelfmt::crypto
