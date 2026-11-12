// Ghostmind harness.
//
//   harness codec                         offline: Borsh bytes, seeds, amounts, chat crypto
//   harness <cmd> [args…]                 live: one module instance in $GHOSTMIND_DATA_DIR
//   harness session THERAPIST_DIR CLIENT_DIR [VERIFIER_DIR]
//                                         live: therapist and client in one process, talking
//                                         over the in-process bus, paying and rating on the
//                                         real testnet
//
// Live commands wait for the queued job and print the resulting state.
#include "ghostmind_impl.h"
#include "ghostmind_chat.h"
#include "ghostmind_codec.h"
#include "logos_sdk.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using nlohmann::json;
using namespace ghostmind;

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!ok) ++g_fail;
}

static int codec()
{
    printf("=== codec ===\n");
    Bytes32 a{}, b{};
    for (int i = 0; i < 32; ++i) { a[size_t(i)] = uint8_t(i); b[size_t(i)] = uint8_t(255 - i); }

    // Init = variant 0, nothing else.
    check(hex(encodeInit()) == "00", "Init is one byte");
    // SetVerifier { a, "Board", b, true } = 01 | a | 05000000 Board | b | 01
    check(hex(encodeSetVerifier(a, "Board", b, true)) == "01" + hex(a) + "05000000" + hex(bytesOf("Board")) + hex(b) + "01",
          "SetVerifier bytes");
    // IssueTicket { hash, 0.01 LGO, 7 } = 08 | hash | 80969800 00…(16) | 0700000000000000
    check(hex(encodeIssueTicket(a, kMinTicketFee, 7)) == "08" + hex(a) + "80969800000000000000000000000000" + "0700000000000000",
          "IssueTicket bytes");
    // Rate { t, secret, 2, 5, "ok", false, 9 }
    check(hex(encodeRate(a, b, 2, 5, "ok", false, 9)) ==
              "09" + hex(a) + hex(b) + "0200000000000000" + "05" + "02000000" + hex(bytesOf("ok")) + "00" + "0900000000000000",
          "Rate bytes");

    // A therapist record round-trips through the same layout the program writes.
    Therapist t;
    t.account = a;
    t.profile.name = "Dr. Example";
    t.profile.bio = "Bio";
    t.profile.specialties = "anxiety, grief";
    t.profile.price = 2 * kLgo + kLgo / 2;
    t.profile.receive.assign(1248, 0x42);
    t.profile.chatKey = b;
    t.status = Flagged;
    t.reason = "why";
    Credential c;
    c.verifier = b;
    c.licenceHash = a;
    c.jurisdiction = "NL";
    c.expires = 1800000000;
    c.time = 1700000000;
    t.credential = c;
    t.tickets = 3; t.ratingSum = 14; t.ratingCount = 3; t.reports = 1; t.reviews = 3; t.registered = 1; t.updated = 2;
    const auto enc = encodeTherapist(t);
    const auto dec = decodeTherapist(enc);
    check(dec.has_value(), "therapist record decodes");
    check(dec && dec->profile.name == "Dr. Example" && dec->profile.price == t.profile.price &&
              dec->profile.receive.size() == 1248 && dec->status == Flagged && dec->credential &&
              dec->credential->jurisdiction == "NL" && dec->credential->expires == 1800000000 && dec->ratingSum == 14 &&
              dec->updated == 2,
          "therapist fields survive the round trip");
    t.credential.reset();
    check(decodeTherapist(encodeTherapist(t)).has_value() && !decodeTherapist(encodeTherapist(t))->credential,
          "a record without a credential decodes");
    std::vector<uint8_t> truncated(enc.begin(), enc.end() - 3);
    check(!decodeTherapist(truncated), "a truncated record is rejected");

    // Verifier list: Vec<Verifier> = u32 count then entries.
    std::vector<uint8_t> vl{2, 0, 0, 0};
    for (int i = 0; i < 2; ++i) {
        vl.insert(vl.end(), a.begin(), a.end());
        vl.insert(vl.end(), {1, 0, 0, 0, 'V'});
        vl.insert(vl.end(), b.begin(), b.end());
        vl.push_back(uint8_t(i));
    }
    const auto vs = decodeVerifiers(vl);
    check(vs && vs->size() == 2 && !(*vs)[0].active && (*vs)[1].active && (*vs)[1].name == "V", "verifier list decodes");
    check(decodeVerifiers({}) && decodeVerifiers({})->empty(), "an empty verifier list is empty");

    // Ticket and review.
    std::vector<uint8_t> tk(a.begin(), a.end());
    tk.insert(tk.end(), {5, 0, 0, 0, 0, 0, 0, 0, 1});
    const auto tkd = decodeTicket(tk);
    check(tkd && tkd->therapist == a && tkd->issued == 5 && tkd->used, "ticket decodes");

    // SHA-256 and the seeds built on it.
    check(hex(sha256(bytesOf("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\") is right");
    check(hex(sha256(std::vector<uint8_t>{})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(\"\") is right");
    std::vector<uint8_t> big(1000, 'x');
    Bytes32 viaSodium{};
    crypto_hash_sha256(viaSodium.data(), big.data(), big.size());
    check(sha256(big) == viaSodium, "sha256 of 1000 bytes agrees with libsodium");
    check(hex(indexSeed()).rfind("686176656e2f696e646578", 0) == 0 && hex(indexSeed()).substr(22) == std::string(42, '0'),
          "index seed is the tag, zero padded");
    check(hex(listingSeed(258)).substr(28, 16) == "0201000000000000", "listing seed carries the index LE");
    check(recordSeed(a) != recordSeed(b) && recordSeed(a) == sha256({bytesOf("haven/therapist/"), bytesOf(a)}), "record seed");
    check(reviewSeed(a, 0) != reviewSeed(a, 1), "review seeds differ by index");

    // Amounts.
    check(formatLgo(1500000000) == "1.50", "1.5 LGO formats");
    check(formatLgo(20000000000000ull) == "20000.00", "20k LGO formats");
    check(formatLgo(999720104) == "0.9997", "at most four decimals");
    check(parseLgo("2.5") == u128(2500000000) && parseLgo("0.01") == u128(10000000) && parseLgo("3") == u128(3000000000),
          "parseLgo");
    check(!parseLgo("abc") && !parseLgo("") && !parseLgo("1.1234567891"), "parseLgo rejects junk");
    check(ticketFee(kLgo / 4) == kMinTicketFee && ticketFee(kLgo) == kLgo / 50 && ticketFee(10 * kLgo) == kLgo / 5, "ticket fee is max(0.01, 2%)");

    // Chat crypto: a sealed hello only the host opens; a shared key both sides derive.
    printf("=== chat ===\n");
    const KeyPair host = chat::newKeyPair(), other = chat::newKeyPair();
    const auto sealed = chat::sealHello(host.pk, "{\"hi\":1}");
    std::string plain;
    check(chat::openHello(host, sealed, plain) && plain == "{\"hi\":1}", "host opens the sealed hello");
    check(!chat::openHello(other, sealed, plain), "nobody else can");
    const KeyPair client = chat::newKeyPair();
    Bytes32 k1{}, k2{};
    check(chat::sharedKey(host.pk, client.sk, k1) && chat::sharedKey(client.pk, host.sk, k2) && k1 == k2, "both sides derive the same key");
    const auto wire = chat::encrypt(k1, "hello there");
    check(chat::decrypt(k2, wire, plain) && plain == "hello there", "a message round-trips");
    Bytes32 k3{};
    chat::sharedKey(other.pk, client.sk, k3);
    check(!chat::decrypt(k3, wire, plain), "the wrong key fails");
    check(chat::inboxTopic(host.pk) == "/ghostmind-inbox/1/" + hex(host.pk).substr(0, 16) + "/proto", "inbox topic");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

// ── live ────────────────────────────────────────────────────────────────

struct Node : GhostmindImpl {
    LogosModules mods;
    explicit Node(const std::string& dir)
    {
        setDataDir(dir);
        _logosCoreSetLogosModulesPtr_(&mods);
    }
};

static json stateOf(GhostmindImpl& m) { return json::parse(m.state()); }

static json waitReady(GhostmindImpl& m)
{
    for (int i = 0; i < 240; ++i) {
        json s = stateOf(m);
        if (s["phase"] == "error") { fprintf(stderr, "wallet error: %s\n", s["error"].get<std::string>().c_str()); exit(2); }
        if (s["phase"] == "ready" && s["network"]["block"].get<int64_t>() > 0) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "wallet never became ready\n");
    exit(2);
}

/** Ready, and the first registry read is in: council, verifiers and every listing. */
static json waitLoaded(GhostmindImpl& m)
{
    waitReady(m);
    for (int i = 0; i < 240; ++i) {
        json s = stateOf(m);
        if (!s["program"]["treasury"].get<std::string>().empty() && !s["count"].is_null() &&
            s["therapists"].size() == s["count"].get<size_t>())
            return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "the registry never finished loading\n");
    exit(2);
}

static json waitJob(GhostmindImpl& m, const std::string& reply, bool exitOnFail = true)
{
    json r = json::parse(reply);
    if (!r.value("ok", false)) { fprintf(stderr, "refused: %s\n", r.value("error", "").c_str()); if (exitOnFail) exit(3); return r; }
    if (!r.contains("job")) return r;
    const int64_t id = r["job"];
    std::string last;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        json s = stateOf(m);
        for (const auto& j : s["jobs"]) {
            if (j["id"] != id) continue;
            const std::string st = j["status"];
            if (st != last) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                printf("  [%3llds] %-10s %s\n", (long long)secs, st.c_str(), j["label"].get<std::string>().c_str());
                fflush(stdout);
                last = st;
            }
            if (st == "done" || st == "failed") {
                if (st == "failed") {
                    fprintf(stderr, "job failed: %s\n", j["error"].get<std::string>().c_str());
                    if (exitOnFail) exit(4);
                }
                return j;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

static json waitFor(GhostmindImpl& m, const std::function<bool(const json&)>& pred, int seconds, const char* what)
{
    for (int i = 0; i < seconds * 2; ++i) {
        json s = stateOf(m);
        if (pred(s)) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "timed out waiting for %s\n", what);
    exit(5);
}

static json findConv(const json& s, const std::string& id)
{
    for (const auto& c : s["conversations"]) if (c["id"] == id) return c;
    return nullptr;
}

static int session(const std::string& tDir, const std::string& cDir, const std::string& vDir)
{
    printf("=== session: therapist %s, client %s%s ===\n", tDir.c_str(), cDir.c_str(), vDir.empty() ? "" : (", verifier " + vDir).c_str());
    Node T(tDir), C(cDir);
    json ts = waitLoaded(T), cs = waitLoaded(C);
    const std::string tAddr = ts["me"]["address"], cAddr = cs["me"]["address"];
    printf("therapist %s  client %s  program %s\n", tAddr.c_str(), cAddr.c_str(), ts["program"]["address"].get<std::string>().c_str());

    if (!ts["me"]["isTherapist"].get<bool>()) {
        printf("\n-- therapist registers --\n");
        waitJob(T, T.registerTherapist("Dr. Harness", "A test listing from the harness.", "anxiety, burnout", "0.5"));
    }
    ts = waitFor(T, [](const json& s) { return s["me"]["isTherapist"].get<bool>(); }, 60, "the listing to show up");
    printf("listed: %s  price %s  tickets %s\n", ts["me"]["therapist"]["name"].get<std::string>().c_str(),
           ts["me"]["therapist"]["price"].get<std::string>().c_str(), ts["me"]["therapist"]["tickets"].dump().c_str());

    if (!vDir.empty()) {
        printf("\n-- verifier attests the therapist --\n");
        Node V(vDir);
        json vs = waitLoaded(V);
        if (!vs["me"]["isVerifier"].get<bool>()) {
            // The verifier wallet must be the council too for the harness to appoint it.
            if (!vs["program"]["hasCouncil"].get<bool>()) waitJob(V, V.initCouncil());
            waitJob(V, V.setVerifier(vs["me"]["address"], "Harness Licensing Board", vs["me"]["chatKey"], true));
            vs = waitFor(V, [](const json& s) { return s["me"]["isVerifier"].get<bool>(); }, 60, "the verifier to appear");
        }
        // The therapist asks over an encrypted conversation; the verifier reads the request and attests.
        ts = waitFor(T, [&](const json& s) { return !s["verifiers"].empty(); }, 60, "the therapist to see the verifier");
        json r = json::parse(T.requestVerification(vs["me"]["address"], "LIC-12345", "NL", "2028-01-01"));
        if (!r.value("ok", false)) { fprintf(stderr, "request refused: %s\n", r.value("error", "").c_str()); return 3; }
        const std::string vconv = r["conversation"];
        json vst = waitFor(V, [&](const json& s) {
            for (const auto& c : s["conversations"]) if (c["purpose"] == "verification" && c["licence"] == "LIC-12345") return true;
            return false;
        }, 30, "the verifier to receive the request");
        json req;
        for (const auto& c : vst["conversations"]) if (c["purpose"] == "verification" && c["licence"] == "LIC-12345") req = c;
        printf("verifier sees request from %s: licence %s, %s, expires %s\n", req["peerAccount"].get<std::string>().c_str(),
               req["licence"].get<std::string>().c_str(), req["jurisdiction"].get<std::string>().c_str(), req["expires"].get<std::string>().c_str());
        waitJob(V, V.verifyTherapist(req["peerAccount"], req["licence"], req["jurisdiction"], req["expires"]));
        ts = waitFor(T, [](const json& s) { return s["me"]["therapist"]["verified"].get<bool>(); }, 90, "the credential to show on the therapist");
        printf("therapist verified by %s (%s)\n", ts["me"]["therapist"]["credential"]["verifierName"].get<std::string>().c_str(),
               ts["me"]["therapist"]["credential"]["jurisdiction"].get<std::string>().c_str());
        json tconv = findConv(waitFor(T, [&](const json& s) {
            json c = findConv(s, vconv);
            for (const auto& m : c["messages"]) if (m["text"].get<std::string>().find("attested") != std::string::npos) return true;
            return false;
        }, 30, "the 'verified' message to reach the therapist"), vconv);
        printf("therapist's conversation got the verifier's note (%zu messages)\n", tconv["messages"].size());
    }

    printf("\n-- client finds the therapist and opens a conversation --\n");
    cs = waitFor(C, [&](const json& s) {
        for (const auto& t : s["therapists"]) if (t["account"] == tAddr) return true;
        return false;
    }, 120, "the client to see the listing");
    json r = json::parse(C.openConversation(tAddr));
    if (!r.value("ok", false)) { fprintf(stderr, "open refused: %s\n", r.value("error", "").c_str()); return 3; }
    const std::string conv = r["conversation"];
    printf("conversation %s\n", conv.c_str());
    waitFor(C, [&](const json& s) { json c = findConv(s, conv); return !c.is_null() && c["acked"].get<bool>(); }, 30, "the therapist's ack");
    printf("therapist acked\n");
    waitJob(C, C.sendMessage(conv, "Hello — I'd like to talk about work stress."));
    waitFor(T, [&](const json& s) {
        json c = findConv(s, conv);
        if (c.is_null()) return false;
        for (const auto& m : c["messages"]) if (m["kind"] == "msg" && m["from"] == "peer") return true;
        return false;
    }, 30, "the therapist to receive the message");
    json tc = findConv(stateOf(T), conv);
    printf("therapist sees it from \"%s\" (role %s, %s)\n", tc["peerName"].get<std::string>().c_str(), tc["role"].get<std::string>().c_str(),
           tc["messages"].back()["text"].get<std::string>().c_str());
    waitJob(T, T.sendMessage(conv, "Of course. When would suit you?"));
    waitFor(C, [&](const json& s) {
        json c = findConv(s, conv);
        for (const auto& m : c["messages"]) if (m["kind"] == "msg" && m["from"] == "peer") return true;
        return false;
    }, 30, "the client to receive the reply");
    printf("client got the reply\n");

    printf("\n-- client pays privately (shield if needed, then private → private) --\n");
    const std::string tPrivBefore = stateOf(T)["me"]["lgoPrivate"];
    waitJob(C, C.pay(conv));
    json cc = findConv(stateOf(C), conv);
    printf("paid tx %s, amount %s\n", cc["paidTx"].get<std::string>().c_str(), cc["paidAmount"].get<std::string>().c_str());

    printf("\n-- therapist's module sees the payment and issues the ticket --\n");
    tc = findConv(waitFor(T, [&](const json& s) { json c = findConv(s, conv); return !c.is_null() && c["paid"].get<bool>(); }, 60, "the paid notice"), conv);
    json ticketJob = nullptr;
    for (int i = 0; i < 600 && ticketJob.is_null(); ++i) {
        const json st = stateOf(T);   // not stateOf(T)["jobs"] in the loop: that temporary dies before the body runs
        for (const auto& j : st["jobs"]) if (j["kind"] == "ticket" && j["conversation"] == conv) ticketJob = j;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (ticketJob.is_null()) { fprintf(stderr, "no ticket job was queued\n"); return 5; }
    waitJob(T, json{{"ok", true}, {"job", ticketJob["id"]}}.dump());
    ts = stateOf(T);
    printf("therapist private balance %s → %s LGO; tickets %s\n", tPrivBefore.c_str(), ts["me"]["lgoPrivate"].get<std::string>().c_str(),
           ts["me"]["therapist"]["tickets"].dump().c_str());
    cc = findConv(waitFor(C, [&](const json& s) { json c = findConv(s, conv); return !c.is_null() && c["ticketIssued"].get<bool>(); }, 60, "the ticket notice"), conv);
    printf("client holds ticket %s (tx %s)\n", cc["ticketHash"].get<std::string>().substr(0, 16).c_str(), cc["ticketTx"].get<std::string>().c_str());

    printf("\n-- client rates from a fresh, unlinked account --\n");
    waitJob(C, C.rate(conv, 5, "Thoughtful and patient. Would recommend.", false));
    cs = waitFor(C, [&](const json& s) {
        for (const auto& t : s["therapists"]) if (t["account"] == tAddr && t["ratingCount"].get<uint64_t>() >= 1) return true;
        return false;
    }, 90, "the rating to show in the directory");
    for (const auto& t : cs["therapists"])
        if (t["account"] == tAddr)
            printf("directory: %s · rating %.2f (%s) · tickets %s · reports %s\n", t["name"].get<std::string>().c_str(),
                   t["rating"].get<double>(), t["ratingCount"].dump().c_str(), t["tickets"].dump().c_str(), t["reports"].dump().c_str());
    json again = json::parse(C.rate(conv, 1, "again", true));
    printf("rating twice: %s\n", again.value("ok", false) ? "ACCEPTED (bug)" : ("refused — " + again.value("error", "")).c_str());
    C.loadReviews(tAddr);
    cs = waitFor(C, [&](const json& s) {
        for (const auto& t : s["therapists"]) if (t["account"] == tAddr && t.contains("reviewList") && !t["reviewList"].empty()) return true;
        return false;
    }, 60, "the review text to load");
    for (const auto& t : cs["therapists"])
        if (t["account"] == tAddr) printf("review: %d/5 \"%s\"\n", t["reviewList"].back()["score"].get<int>(), t["reviewList"].back()["text"].get<std::string>().c_str());
    printf("\nsession PASSED\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (sodium_init() < 0) return 2;
    if (argc < 2 || std::string(argv[1]) == "codec") return codec();
    const std::string cmd = argv[1];
    auto arg = [&](int i) { return argc > i ? std::string(argv[i]) : std::string(); };

    if (cmd == "session") {
        if (argc < 4) { fprintf(stderr, "session THERAPIST_DIR CLIENT_DIR [VERIFIER_DIR]\n"); return 2; }
        return session(arg(2), arg(3), arg(4));
    }

    const char* dir = std::getenv("GHOSTMIND_DATA_DIR");
    Node m(dir && *dir ? dir : "./ghostmind-data");
    json s = waitReady(m);
    printf("wallet %s  private %s  chat key %s  block %lld  program %s\n", s["me"]["address"].get<std::string>().c_str(),
           s["me"]["privateAddress"].get<std::string>().c_str(), s["me"]["chatKey"].get<std::string>().c_str(),
           (long long)s["network"]["block"].get<int64_t>(), s["program"]["address"].get<std::string>().c_str());

    std::string reply;
    if (cmd == "state")               { /* just print */ }
    else if (cmd == "deploy")         reply = m.deploy(arg(2));
    else if (cmd == "use")            reply = m.setProgram(arg(2));
    else if (cmd == "init")           reply = m.initCouncil();
    else if (cmd == "gas")            reply = m.getGas();
    else if (cmd == "shield")         reply = m.shield(arg(2));
    else if (cmd == "set-verifier")   reply = m.setVerifier(arg(2), arg(3), arg(4), arg(5) != "off");
    else if (cmd == "register")       reply = m.registerTherapist(arg(2), arg(3), arg(4), arg(5));
    else if (cmd == "update")         reply = m.updateProfile(arg(2), arg(3), arg(4), arg(5));
    else if (cmd == "verify")         reply = m.verifyTherapist(arg(2), arg(3), arg(4), arg(5));
    else if (cmd == "revoke")         reply = m.revokeTherapist(arg(2));
    else if (cmd == "suspend")        reply = m.suspendTherapist(arg(2), arg(3));
    else if (cmd == "reinstate")      reply = m.reinstateTherapist(arg(2));
    else if (cmd == "rate")           reply = m.rate(arg(2), std::atoll(arg(3).c_str()), arg(4), arg(5) == "report");
    else if (cmd == "reviews")        { m.loadReviews(arg(2)); std::this_thread::sleep_for(std::chrono::seconds(12)); }
    else if (cmd == "recovery")       reply = m.recoveryPhrase();
    else if (cmd == "pay") {          // pay THERAPIST: what a client's "Pay" button does
        const std::string t = arg(2);
        waitFor(m, [&](const json& st) {
            for (const auto& x : st["therapists"]) if (x["account"] == t) return true;
            return false;
        }, 120, "the therapist's listing");
        json o = json::parse(m.openConversation(t));
        if (!o.value("ok", false)) { printf("%s\n", o.dump(2).c_str()); return 1; }
        reply = m.pay(o["conversation"].get<std::string>());
    }
    else { fprintf(stderr, "unknown command %s\n", cmd.c_str()); return 2; }

    if (!reply.empty()) {
        json r = json::parse(reply);
        if (r.contains("job")) waitJob(m, reply);
        else printf("%s\n", r.dump(2).c_str());
    }
    // Let the next refresh pick up what changed.
    std::this_thread::sleep_for(std::chrono::seconds(6));
    s = stateOf(m);
    s.erase("conversations");
    printf("%s\n", s.dump(2).c_str());
    return 0;
}
