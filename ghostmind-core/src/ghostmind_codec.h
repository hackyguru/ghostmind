#pragma once
// Hand-written Borsh for the Ghostmind program (ghostmind-program/src/main.rs), plus
// the seeds of every account it derives and LGO amount formatting. No wallet,
// network or crypto-library code here, so tests/harness can pin the bytes
// offline and the impl header stays plain C++.

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ghostmind {

using Bytes32 = std::array<uint8_t, 32>;
using u128 = unsigned __int128;

constexpr u128 kLgo = 1000000000;                 // atomic units per LGO
constexpr u128 kMinTicketFee = 10000000;          // 0.01 LGO
constexpr u128 kTicketFeeBps = 200;               // 2% of the listed price
constexpr uint64_t kReportsToFlag = 3;
constexpr size_t kMaxName = 60, kMaxBio = 1000, kMaxSpecialties = 200, kMaxJurisdiction = 60,
                 kMaxReason = 300, kMaxReview = 600;

enum Status : uint8_t { Listed = 0, Suspended = 1, Flagged = 2 };

/// An X25519 key pair (ghostmind_chat.h makes and uses them).
struct KeyPair {
    Bytes32 pk{}, sk{};
};

// ── records ──────────────────────────────────────────────────────────────

struct Profile {
    std::string name, bio, specialties;
    u128 price = 0;
    std::vector<uint8_t> receive;   // npk ‖ vpk ‖ identifier
    Bytes32 chatKey{};
};

struct Credential {
    Bytes32 verifier{}, licenceHash{};
    std::string jurisdiction;
    uint64_t expires = 0, time = 0;
};

struct Therapist {
    Bytes32 account{};
    Profile profile;
    uint8_t status = Listed;
    std::string reason;
    std::optional<Credential> credential;
    uint64_t tickets = 0, ratingSum = 0, ratingCount = 0, reports = 0, reviews = 0, registered = 0, updated = 0;
};

struct Verifier {
    Bytes32 account{};
    std::string name;
    Bytes32 chatKey{};
    bool active = false;
};

struct Ticket {
    Bytes32 therapist{};
    uint64_t issued = 0;
    bool used = false;
};

struct Review {
    Bytes32 ticket{};
    uint8_t score = 0;
    std::string text;
    bool report = false;
    uint64_t time = 0;
};

// ── Borsh primitives ─────────────────────────────────────────────────────

struct Writer {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void boolean(bool b) { v.push_back(b ? 1 : 0); }
    void u32(uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff)); }
    void u64(uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff)); }
    void u128v(u128 x) { for (int i = 0; i < 16; ++i) { v.push_back(uint8_t(x & 0xff)); x >>= 8; } }
    void b32(const Bytes32& b) { v.insert(v.end(), b.begin(), b.end()); }
    void str(const std::string& s) { u32(uint32_t(s.size())); v.insert(v.end(), s.begin(), s.end()); }
    void bytes(const std::vector<uint8_t>& b) { u32(uint32_t(b.size())); v.insert(v.end(), b.begin(), b.end()); }
};

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t len) : p(data), n(len) {}
    Reader(const std::vector<uint8_t>& d) : p(d.data()), n(d.size()) {}
    uint64_t u(int bytes)
    {
        if (at + size_t(bytes) > n) { ok = false; return 0; }
        uint64_t x = 0;
        for (int i = bytes - 1; i >= 0; --i) x = (x << 8) | p[at + size_t(i)];
        at += size_t(bytes);
        return x;
    }
    uint8_t u8() { return uint8_t(u(1)); }
    bool boolean() { return u(1) != 0; }
    uint64_t u64() { return u(8); }
    u128 u128v()
    {
        if (at + 16 > n) { ok = false; return 0; }
        u128 x = 0;
        for (int i = 15; i >= 0; --i) x = (x << 8) | p[at + size_t(i)];
        at += 16;
        return x;
    }
    Bytes32 b32()
    {
        Bytes32 b{};
        if (at + 32 > n) { ok = false; return b; }
        std::memcpy(b.data(), p + at, 32);
        at += 32;
        return b;
    }
    std::string str()
    {
        const uint64_t len = u(4);
        if (!ok || at + len > n) { ok = false; return ""; }
        std::string s(reinterpret_cast<const char*>(p + at), size_t(len));
        at += size_t(len);
        return s;
    }
    std::vector<uint8_t> bytes()
    {
        const uint64_t len = u(4);
        if (!ok || at + len > n) { ok = false; return {}; }
        std::vector<uint8_t> b(p + at, p + at + len);
        at += size_t(len);
        return b;
    }
};

// ── instruction encoders (enum Instruction, variant index first) ─────────

inline void putProfile(Writer& w, const Profile& p)
{
    w.str(p.name);
    w.str(p.bio);
    w.str(p.specialties);
    w.u128v(p.price);
    w.bytes(p.receive);
    w.b32(p.chatKey);
}

inline std::vector<uint8_t> encodeInit()
{
    Writer w;
    w.u8(0);
    return w.v;
}

inline std::vector<uint8_t> encodeSetVerifier(const Bytes32& verifier, const std::string& name, const Bytes32& chatKey, bool active)
{
    Writer w;
    w.u8(1);
    w.b32(verifier);
    w.str(name);
    w.b32(chatKey);
    w.boolean(active);
    return w.v;
}

inline std::vector<uint8_t> encodeRegister(uint64_t index, const Profile& p, uint64_t time)
{
    Writer w;
    w.u8(2);
    w.u64(index);
    putProfile(w, p);
    w.u64(time);
    return w.v;
}

inline std::vector<uint8_t> encodeUpdate(const Profile& p, uint64_t time)
{
    Writer w;
    w.u8(3);
    putProfile(w, p);
    w.u64(time);
    return w.v;
}

inline std::vector<uint8_t> encodeVerify(const Bytes32& therapist, const Bytes32& licenceHash, const std::string& jurisdiction,
                                         uint64_t expires, uint64_t time)
{
    Writer w;
    w.u8(4);
    w.b32(therapist);
    w.b32(licenceHash);
    w.str(jurisdiction);
    w.u64(expires);
    w.u64(time);
    return w.v;
}

inline std::vector<uint8_t> encodeRevoke(const Bytes32& therapist)
{
    Writer w;
    w.u8(5);
    w.b32(therapist);
    return w.v;
}

inline std::vector<uint8_t> encodeSuspend(const Bytes32& therapist, const std::string& reason)
{
    Writer w;
    w.u8(6);
    w.b32(therapist);
    w.str(reason);
    return w.v;
}

inline std::vector<uint8_t> encodeReinstate(const Bytes32& therapist)
{
    Writer w;
    w.u8(7);
    w.b32(therapist);
    return w.v;
}

inline std::vector<uint8_t> encodeIssueTicket(const Bytes32& hash, u128 fee, uint64_t time)
{
    Writer w;
    w.u8(8);
    w.b32(hash);
    w.u128v(fee);
    w.u64(time);
    return w.v;
}

inline std::vector<uint8_t> encodeRate(const Bytes32& therapist, const Bytes32& secret, uint64_t index, uint8_t score,
                                       const std::string& text, bool report, uint64_t time)
{
    Writer w;
    w.u8(9);
    w.b32(therapist);
    w.b32(secret);
    w.u64(index);
    w.u8(score);
    w.str(text);
    w.boolean(report);
    w.u64(time);
    return w.v;
}

// ── record decoders ──────────────────────────────────────────────────────

inline bool readProfile(Reader& r, Profile& p)
{
    p.name = r.str();
    p.bio = r.str();
    p.specialties = r.str();
    p.price = r.u128v();
    p.receive = r.bytes();
    p.chatKey = r.b32();
    return r.ok;
}

inline std::optional<Therapist> decodeTherapist(const std::vector<uint8_t>& raw)
{
    Reader r(raw);
    Therapist t;
    t.account = r.b32();
    if (!readProfile(r, t.profile)) return std::nullopt;
    t.status = r.u8();
    t.reason = r.str();
    if (r.boolean()) {
        Credential c;
        c.verifier = r.b32();
        c.licenceHash = r.b32();
        c.jurisdiction = r.str();
        c.expires = r.u64();
        c.time = r.u64();
        t.credential = c;
    }
    t.tickets = r.u64();
    t.ratingSum = r.u64();
    t.ratingCount = r.u64();
    t.reports = r.u64();
    t.reviews = r.u64();
    t.registered = r.u64();
    t.updated = r.u64();
    if (!r.ok || r.at != raw.size()) return std::nullopt;
    return t;
}

inline std::vector<uint8_t> encodeTherapist(const Therapist& t)
{
    Writer w;
    w.b32(t.account);
    putProfile(w, t.profile);
    w.u8(t.status);
    w.str(t.reason);
    w.boolean(t.credential.has_value());
    if (t.credential) {
        w.b32(t.credential->verifier);
        w.b32(t.credential->licenceHash);
        w.str(t.credential->jurisdiction);
        w.u64(t.credential->expires);
        w.u64(t.credential->time);
    }
    w.u64(t.tickets);
    w.u64(t.ratingSum);
    w.u64(t.ratingCount);
    w.u64(t.reports);
    w.u64(t.reviews);
    w.u64(t.registered);
    w.u64(t.updated);
    return w.v;
}

inline std::optional<std::vector<Verifier>> decodeVerifiers(const std::vector<uint8_t>& raw)
{
    std::vector<Verifier> out;
    if (raw.empty()) return out;
    Reader r(raw);
    const uint64_t n = r.u(4);
    for (uint64_t i = 0; i < n && r.ok; ++i) {
        Verifier v;
        v.account = r.b32();
        v.name = r.str();
        v.chatKey = r.b32();
        v.active = r.boolean();
        out.push_back(v);
    }
    if (!r.ok || r.at != raw.size()) return std::nullopt;
    return out;
}

inline std::optional<Ticket> decodeTicket(const std::vector<uint8_t>& raw)
{
    Reader r(raw);
    Ticket t;
    t.therapist = r.b32();
    t.issued = r.u64();
    t.used = r.boolean();
    if (!r.ok || r.at != raw.size()) return std::nullopt;
    return t;
}

inline std::optional<Review> decodeReview(const std::vector<uint8_t>& raw)
{
    Reader r(raw);
    Review v;
    v.ticket = r.b32();
    v.score = r.u8();
    v.text = r.str();
    v.report = r.boolean();
    v.time = r.u64();
    if (!r.ok || r.at != raw.size()) return std::nullopt;
    return v;
}

// ── seeds (32 bytes each, as the program derives them) ───────────────────

// ── SHA-256 (FIPS 180-4), so this header needs no crypto library ─────────

inline Bytes32 sha256(const uint8_t* data, size_t len)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = uint64_t(len) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(uint8_t((bits >> (8 * i)) & 0xff));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(msg[off + 4 * i]) << 24 | uint32_t(msg[off + 4 * i + 1]) << 16 |
                   uint32_t(msg[off + 4 * i + 2]) << 8 | uint32_t(msg[off + 4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    Bytes32 out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[size_t(4 * i + j)] = uint8_t((h[i] >> (24 - 8 * j)) & 0xff);
    return out;
}

inline Bytes32 sha256(const std::vector<uint8_t>& data) { return sha256(data.data(), data.size()); }

inline Bytes32 sha256(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> all;
    for (const auto& p : parts) all.insert(all.end(), p.begin(), p.end());
    return sha256(all);
}

inline std::vector<uint8_t> bytesOf(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }
inline std::vector<uint8_t> bytesOf(const Bytes32& b) { return std::vector<uint8_t>(b.begin(), b.end()); }
inline std::vector<uint8_t> leBytes(uint64_t x)
{
    std::vector<uint8_t> v;
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff));
    return v;
}

inline Bytes32 tagSeed(const char* tag)
{
    Bytes32 s{};
    std::memcpy(s.data(), tag, std::strlen(tag));
    return s;
}
inline Bytes32 councilSeed() { return tagSeed("haven/council"); }
inline Bytes32 verifiersSeed() { return tagSeed("haven/verifiers"); }
inline Bytes32 indexSeed() { return tagSeed("haven/index"); }
inline Bytes32 treasurySeed() { return tagSeed("haven/treasury"); }
inline Bytes32 listingSeed(uint64_t n)
{
    Bytes32 s = tagSeed("haven/listing/");
    for (int i = 0; i < 8; ++i) s[14 + size_t(i)] = uint8_t((n >> (8 * i)) & 0xff);
    return s;
}
inline Bytes32 recordSeed(const Bytes32& therapist) { return sha256({bytesOf("haven/therapist/"), bytesOf(therapist)}); }
inline Bytes32 ticketSeed(const Bytes32& hash) { return sha256({bytesOf("haven/ticket/"), bytesOf(hash)}); }
inline Bytes32 reviewSeed(const Bytes32& therapist, uint64_t n)
{
    return sha256({bytesOf("haven/review/"), bytesOf(therapist), leBytes(n)});
}

/// The fee a ticket costs for a listing at `price`.
inline u128 ticketFee(u128 price)
{
    const u128 pct = price * kTicketFeeBps / 10000;
    return pct > kMinTicketFee ? pct : kMinTicketFee;
}

// ── amounts and encodings ────────────────────────────────────────────────

inline std::string u128ToString(u128 v)
{
    if (!v) return "0";
    std::string s;
    while (v) { s.insert(s.begin(), char('0' + int(v % 10))); v /= 10; }
    return s;
}

/// "1.25" LGO with at most four decimals, trailing zeros trimmed to two.
inline std::string formatLgo(u128 atomic)
{
    std::string frac = u128ToString(atomic % kLgo);
    frac.insert(frac.begin(), 9 - frac.size(), '0');
    frac.resize(4);
    while (frac.size() > 2 && frac.back() == '0') frac.pop_back();
    return u128ToString(atomic / kLgo) + "." + frac;
}

/// "1.5" → 1500000000. Up to nine decimals; anything else is nullopt.
inline std::optional<u128> parseLgo(const std::string& in)
{
    std::string s;
    for (char c : in) if (c != ' ' && c != '_' && c != ',') s.push_back(c);
    if (s.empty()) return std::nullopt;
    const size_t dot = s.find('.');
    const std::string whole = dot == std::string::npos ? s : s.substr(0, dot);
    std::string frac = dot == std::string::npos ? "" : s.substr(dot + 1);
    if (frac.size() > 9 || (whole.empty() && frac.empty())) return std::nullopt;
    u128 v = 0;
    for (char c : whole) { if (c < '0' || c > '9') return std::nullopt; v = v * 10 + u128(c - '0'); }
    frac.resize(9, '0');
    u128 f = 0;
    for (char c : frac) { if (c < '0' || c > '9') return std::nullopt; f = f * 10 + u128(c - '0'); }
    return v * kLgo + f;
}

inline std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
inline std::string hex(const Bytes32& b) { return hex(b.data(), 32); }
inline std::string hex(const std::vector<uint8_t>& v) { return hex(v.data(), v.size()); }

inline bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}
inline bool unhex32(const std::string& s, Bytes32& out) { return unhex(s, out.data(), 32); }
inline std::vector<uint8_t> unhexVec(const std::string& s)
{
    std::vector<uint8_t> v(s.size() / 2);
    if (!unhex(s, v.data(), v.size())) v.clear();
    return v;
}

} // namespace ghostmind
