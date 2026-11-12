#pragma once
// The encrypted conversation layer: what goes on a Logos delivery topic.
//
//   inbox   /ghostmind-inbox/1/<first 16 hex of the host's chat key>/proto
//           0x01 ‖ crypto_box_seal(host chat key, hello JSON)
//           A sealed box names no sender: the host learns a fresh X25519 key
//           and a conversation id, nothing else.
//   conv    /ghostmind-conv/1/<conversation id>/proto
//           (Content topics are exactly /app/version/name/encoding; a fifth
//           segment makes delivery read the first as a shard generation.)
//           0x02 ‖ nonce(24) ‖ crypto_box_easy_afternm(shared key, message JSON)
//           The shared key is X25519(host chat key, client conversation key),
//           so both sides derive it and nobody else can.
//
// Pure libsodium, no module code, so the harness can run the two ends of a
// conversation in one process.

#include <cstdint>
#include <string>
#include <vector>

#include <sodium.h>

#include "ghostmind_codec.h"

namespace ghostmind::chat {

constexpr uint8_t kHello = 0x01;
constexpr uint8_t kMessage = 0x02;

using ghostmind::KeyPair;

inline KeyPair newKeyPair()
{
    KeyPair k;
    crypto_box_keypair(k.pk.data(), k.sk.data());
    return k;
}

inline KeyPair keyPairFromSecret(const Bytes32& sk)
{
    KeyPair k;
    k.sk = sk;
    crypto_scalarmult_base(k.pk.data(), sk.data());
    return k;
}

inline std::string randomHex(size_t bytes)
{
    std::vector<uint8_t> raw(bytes);
    randombytes_buf(raw.data(), raw.size());
    return hex(raw);
}

inline Bytes32 randomBytes32()
{
    Bytes32 b{};
    randombytes_buf(b.data(), b.size());
    return b;
}

inline std::string inboxTopic(const Bytes32& hostChatKey)
{
    return "/ghostmind-inbox/1/" + hex(hostChatKey).substr(0, 16) + "/proto";
}

inline std::string convTopic(const std::string& convId)
{
    return "/ghostmind-conv/1/" + convId + "/proto";
}

/// Seal `plaintext` to the host's chat key. Only the host can open it, and
/// it can't tell who sealed it.
inline std::vector<uint8_t> sealHello(const Bytes32& hostChatKey, const std::string& plaintext)
{
    std::vector<uint8_t> out(1 + crypto_box_SEALBYTES + plaintext.size());
    out[0] = kHello;
    crypto_box_seal(out.data() + 1, reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size(),
                    hostChatKey.data());
    return out;
}

inline bool openHello(const KeyPair& host, const std::vector<uint8_t>& wire, std::string& plaintext)
{
    if (wire.size() < 1 + crypto_box_SEALBYTES || wire[0] != kHello) return false;
    std::vector<uint8_t> out(wire.size() - 1 - crypto_box_SEALBYTES);
    if (crypto_box_seal_open(out.data(), wire.data() + 1, wire.size() - 1, host.pk.data(), host.sk.data()) != 0)
        return false;
    plaintext.assign(out.begin(), out.end());
    return true;
}

/// X25519 shared key between my secret and their public key; symmetric.
inline bool sharedKey(const Bytes32& theirPk, const Bytes32& mySk, Bytes32& out)
{
    return crypto_box_beforenm(out.data(), theirPk.data(), mySk.data()) == 0;
}

inline std::vector<uint8_t> encrypt(const Bytes32& key, const std::string& plaintext)
{
    std::vector<uint8_t> out(1 + crypto_box_NONCEBYTES + crypto_box_MACBYTES + plaintext.size());
    out[0] = kMessage;
    uint8_t* nonce = out.data() + 1;
    randombytes_buf(nonce, crypto_box_NONCEBYTES);
    crypto_box_easy_afternm(out.data() + 1 + crypto_box_NONCEBYTES, reinterpret_cast<const uint8_t*>(plaintext.data()),
                            plaintext.size(), nonce, key.data());
    return out;
}

inline bool decrypt(const Bytes32& key, const std::vector<uint8_t>& wire, std::string& plaintext)
{
    if (wire.size() < 1 + crypto_box_NONCEBYTES + crypto_box_MACBYTES || wire[0] != kMessage) return false;
    const uint8_t* nonce = wire.data() + 1;
    const uint8_t* ct = wire.data() + 1 + crypto_box_NONCEBYTES;
    const size_t ctLen = wire.size() - 1 - crypto_box_NONCEBYTES;
    std::vector<uint8_t> out(ctLen - crypto_box_MACBYTES);
    if (crypto_box_open_easy_afternm(out.data(), ct, ctLen, nonce, key.data()) != 0) return false;
    plaintext.assign(out.begin(), out.end());
    return true;
}

} // namespace ghostmind::chat
