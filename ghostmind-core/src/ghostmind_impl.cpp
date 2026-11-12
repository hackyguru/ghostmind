#include "ghostmind_impl.h"
#include "ghostmind_chat.h"

#include "logos_sdk.h"          // generated: modules().delivery_module

#include <nlohmann/json.hpp>
#include <sodium.h>

extern "C" {
#include "wallet_ffi.h"
}

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>

using nlohmann::json;
namespace fs = std::filesystem;
using namespace ghostmind;

namespace {

// The Ghostmind registry on the LEZ v0.3.1 public testnet, deployed 2026-10-29
// from ghostmind-program/ghostmind.bin (image id e76b88b8…). After a testnet reset,
// redeploy (tests/run.sh deploy ../ghostmind-program/ghostmind.bin) and update this,
// or use setProgram() / the UI's Wallet tab.
constexpr const char* kDefaultProgram = "HyFn5FKaUL4NMZqJ5KC1LB2JXWN6UiagQsNaXTTqVPXW";

// The testnet's shared faucet: a genesis account whose key is public on
// purpose (`PRIVATE_KEY_PUB_ACC_B` in lez/testnet_initial_state).
constexpr const char* kFaucetKeyHex = "717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b";
constexpr const char* kFaucetB58 = "7wHg9sbJwc6h3NP1S9bekfAzB8CHifEcxKswCKUt3YQo";

constexpr int64_t kTickMs = 5000;
constexpr int64_t kConfirmTimeoutMs = 8 * 60 * 1000;
constexpr int kReadsPerTick = 12;           // listing records refreshed per tick
constexpr size_t kMaxMessages = 200;        // kept per conversation
constexpr u128 kRaterFunding = kLgo / 20;   // 0.05 LGO funds a review account
constexpr size_t kVpkLen = 1184;            // ML-KEM-768 encapsulation key
constexpr size_t kReceiveLen = 32 + kVpkLen + 32;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

FfiBytes32 ffi(const Bytes32& b)
{
    FfiBytes32 f{};
    std::copy(b.begin(), b.end(), f.data);
    return f;
}

Bytes32 fromFfi(const FfiBytes32& f)
{
    Bytes32 b{};
    std::copy(std::begin(f.data), std::end(f.data), b.begin());
    return b;
}

void u128Le(u128 v, uint8_t (*out)[16])
{
    for (int i = 0; i < 16; ++i) { (*out)[i] = uint8_t(v & 0xff); v >>= 8; }
}

std::string errorText(int code)
{
    switch (code) {
    case NETWORK_ERROR:      return "Couldn't reach the LEZ sequencer.";
    case INSUFFICIENT_FUNDS: return "Not enough LGO.";
    case PAYER_CANNOT_FUND:  return "Not enough LGO for the fee — tap Get LGO.";
    case INVALID_BYTECODE:   return "That file isn't a valid LEZ program.";
    case ACCOUNT_NOT_FOUND:  return "The wallet doesn't know that account.";
    case SYNC_ERROR:         return "Couldn't sync private state with the sequencer.";
    case INTERNAL_ERROR:     return "The wallet couldn't build that transaction (error 99) — the Basecamp log has details.";
    default:                 return "Wallet error " + std::to_string(code) + ".";
    }
}

void check(WalletFfiError e, const char* what)
{
    if (e != SUCCESS) {
        fprintf(stderr, "[ghostmind] %s failed: %d\n", what, int(e));
        throw std::runtime_error(errorText(int(e)));
    }
}

std::string jsonOk(const json& extra = json::object())
{
    json j = extra;
    j["ok"] = true;
    return j.dump();
}

std::string jsonErr(const std::string& msg)
{
    return json{{"ok", false}, {"error", msg}}.dump();
}

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/** Cut to at most `n` bytes without splitting a UTF-8 character. */
std::string clip(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string upper(std::string s)
{
    for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/** "2027-12-31" → unix seconds at 00:00 UTC; 0 when empty or malformed. */
uint64_t parseDate(const std::string& iso)
{
    const std::string s = trim(iso);
    if (s.empty()) return 0;
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1970 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    const time_t v = timegm(&t);
    return v > 0 ? uint64_t(v) : 0;
}

std::string formatDate(uint64_t unix)
{
    if (!unix) return "";
    const time_t t = time_t(unix);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

/** What a verifier attests to: the licence and where it was issued, hashed so the chain never holds the number. */
Bytes32 licenceHash(const std::string& licence, const std::string& jurisdiction)
{
    const std::string s = upper(trim(licence)) + "|" + upper(trim(jurisdiction));
    return sha256(std::vector<uint8_t>(s.begin(), s.end()));
}

std::string shortId(const std::string& s)
{
    return s.size() > 8 ? s.substr(0, 4) + "…" + s.substr(s.size() - 4) : s;
}

} // namespace


GhostmindImpl::GhostmindImpl()
{
    if (sodium_init() < 0) fprintf(stderr, "[ghostmind] libsodium failed to initialise\n");
}

GhostmindImpl::~GhostmindImpl()
{
    m_running = false;
    m_qcv.notify_all();
    m_outCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_netThread.joinable()) m_netThread.join();
    if (m_wallet) {
        wallet_ffi_save(m_wallet);
        wallet_ffi_destroy(m_wallet);
    }
}

void GhostmindImpl::onContextReady() { start(); }

void GhostmindImpl::start()
{
    if (m_running.exchange(true)) return;
    m_worker = std::thread([this] { workerLoop(); });
    m_netThread = std::thread([this] { netLoop(); });
}

std::string GhostmindImpl::dataDir() const
{
    if (!m_dataDirOverride.empty()) return m_dataDirOverride;
    if (const char* env = std::getenv("GHOSTMIND_DATA_DIR"); env && *env) return env;
    if (isContextReady() && !instancePersistencePath().empty()) return instancePersistencePath();
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.ghostmind";
}

// ── persistence ─────────────────────────────────────────────────────────

void GhostmindImpl::loadMeta()
{
    std::ifstream f(dataDir() + "/ghostmind.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_haveMe = unhex32(j.value("me", ""), m_me) && unhex32(j.value("mePrivate", ""), m_mePrivate);
    m_haveFaucet = j.value("faucetImported", false);
    m_programOverride = j.value("program", "");
    m_mnemonic = j.value("mnemonic", "");
    m_password = j.value("password", "");
    m_myName = j.value("name", "");
    m_mode = j.value("mode", "");
    Bytes32 sk{};
    if (unhex32(j.value("chatSecret", ""), sk)) {
        m_chat = chat::keyPairFromSecret(sk);
        m_haveChat = true;
    }
    if (auto b = j.find("privateBaseline"); b != j.end() && b->is_string()) {
        u128 v = 0;
        for (char c : b->get<std::string>()) if (c >= '0' && c <= '9') v = v * 10 + u128(c - '0');
        m_privateBaseline = v;
    }
}

void GhostmindImpl::saveMeta()
{
    json j;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_haveMe) { j["me"] = hex(m_me); j["mePrivate"] = hex(m_mePrivate); }
        j["faucetImported"] = m_haveFaucet;
        j["program"] = m_programOverride;
        j["mnemonic"] = m_mnemonic;
        j["password"] = m_password;
        j["name"] = m_myName;
        j["mode"] = m_mode;
        if (m_haveChat) j["chatSecret"] = hex(m_chat.sk);
        j["privateBaseline"] = u128ToString(m_privateBaseline);
    }
    const std::string path = dataDir() + "/ghostmind.json";
    std::ofstream(path + ".tmp", std::ios::trunc) << j.dump(2);
    std::error_code ec;
    fs::rename(path + ".tmp", path, ec);
    ::chmod(path.c_str(), 0600);
}

void GhostmindImpl::loadConversations()
{
    std::ifstream f(dataDir() + "/conversations.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return;
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto& c : j) {
        Conversation v;
        v.id = c.value("id", "");
        if (v.id.empty()) continue;
        v.role = c.value("role", "client");
        v.purpose = c.value("purpose", "therapy");
        if (!unhex32(c.value("peerPk", ""), v.peerPk) || !unhex32(c.value("myPk", ""), v.myPk) ||
            !unhex32(c.value("mySk", ""), v.mySk) || !unhex32(c.value("key", ""), v.key))
            continue;
        v.peerAccount = c.value("peerAccount", "");
        v.peerName = c.value("peerName", "");
        v.acked = c.value("acked", false);
        v.paidTx = c.value("paidTx", "");
        v.paidAmount = c.value("paidAmount", "");
        v.ticketHash = c.value("ticketHash", "");
        v.ticketSecret = c.value("ticketSecret", "");
        v.ticketTx = c.value("ticketTx", "");
        v.ticketIssued = c.value("ticketIssued", false);
        v.rated = c.value("rated", false);
        v.licence = c.value("licence", "");
        v.jurisdiction = c.value("jurisdiction", "");
        v.expires = c.value("expires", "");
        v.lastMs = c.value("lastMs", int64_t(0));
        v.unread = c.value("unread", 0);
        for (const auto& m : c.value("messages", json::array())) {
            Message x;
            x.id = m.value("id", "");
            x.from = m.value("from", "peer");
            x.kind = m.value("kind", "msg");
            x.text = m.value("text", "");
            x.time = m.value("time", int64_t(0));
            v.messages.push_back(x);
        }
        m_convs[v.id] = v;
    }
}

void GhostmindImpl::saveConversations()
{
    std::lock_guard<std::mutex> saveLk(m_saveMu);  // worker and net loop both save
    json arr = json::array();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (const auto& [id, v] : m_convs) {
            json msgs = json::array();
            for (const auto& m : v.messages)
                msgs.push_back({{"id", m.id}, {"from", m.from}, {"kind", m.kind}, {"text", m.text}, {"time", m.time}});
            arr.push_back({{"id", v.id}, {"role", v.role}, {"purpose", v.purpose}, {"peerPk", hex(v.peerPk)},
                           {"myPk", hex(v.myPk)}, {"mySk", hex(v.mySk)}, {"key", hex(v.key)},
                           {"peerAccount", v.peerAccount}, {"peerName", v.peerName}, {"acked", v.acked},
                           {"paidTx", v.paidTx}, {"paidAmount", v.paidAmount}, {"ticketHash", v.ticketHash},
                           {"ticketSecret", v.ticketSecret}, {"ticketTx", v.ticketTx}, {"ticketIssued", v.ticketIssued},
                           {"rated", v.rated}, {"licence", v.licence}, {"jurisdiction", v.jurisdiction},
                           {"expires", v.expires}, {"lastMs", v.lastMs}, {"unread", v.unread}, {"messages", msgs}});
        }
    }
    const std::string path = dataDir() + "/conversations.json";
    std::ofstream(path + ".tmp", std::ios::trunc) << arr.dump();
    std::error_code ec;
    fs::rename(path + ".tmp", path, ec);
    ::chmod(path.c_str(), 0600);
}

// ── worker ──────────────────────────────────────────────────────────────

void GhostmindImpl::workerLoop()
{
    try {
        openWallet();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_phase = "error";
        m_error = e.what();
        return;
    }
    {
        std::string err;
        if (!startNetworkNow(err)) fprintf(stderr, "[ghostmind] network: %s\n", err.c_str());
    }

    int64_t lastRefresh = 0;
    while (m_running) {
        int64_t jobId = 0;
        {
            std::unique_lock<std::mutex> lk(m_qmu);
            m_qcv.wait_for(lk, std::chrono::milliseconds(400),
                           [this] { return !m_running || !m_queue.empty() || m_kick; });
            if (!m_running) break;
            if (!m_queue.empty()) { jobId = m_queue.front(); m_queue.pop_front(); }
        }
        if (jobId) {
            std::function<void(Job&)> run;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                for (auto& j : m_jobs) if (j.id == jobId) { j.status = "working"; run = j.run; }
            }
            Job scratch;
            scratch.id = jobId;
            try {
                if (run) run(scratch);
                setJob(jobId, [](Job& j) { j.status = "done"; j.finishedMs = nowMs(); });
            } catch (const std::exception& e) {
                setJob(jobId, [&](Job& j) { j.status = "failed"; j.error = e.what(); j.finishedMs = nowMs(); });
            }
            wallet_ffi_save(m_wallet);
            saveMeta();
            lastRefresh = 0;
        }
        if (m_dirty.exchange(false)) saveMeta();
        if (m_convDirty.exchange(false)) saveConversations();
        if (m_kick.exchange(false) || nowMs() - lastRefresh >= kTickMs) {
            try {
                refreshNow();
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = true;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = false;
                fprintf(stderr, "[ghostmind] refresh: %s\n", e.what());
            }
            lastRefresh = nowMs();
        }
    }
}

// The app was called Haven before. Carry an existing install's wallet, chats
// and node key over the first time Ghostmind starts, so a rename never costs
// anyone their LGO or their conversations. The old folder is left in place.
static void migrateFromHaven(const std::string& dir)
{
    std::error_code ec;
    const fs::path d(dir);
    if (fs::exists(d / "ghostmind.json", ec)) return;
    if (fs::exists(d / "haven.json", ec)) {           // same folder, old file name (CLI data dirs)
        fs::rename(d / "haven.json", d / "ghostmind.json", ec);
        return;
    }
    // Basecamp: module_data/ghostmind_core/<id>  <-  module_data/haven_core/<id>
    if (d.parent_path().filename() != "ghostmind_core") return;
    const fs::path legacyRoot = d.parent_path().parent_path() / "haven_core";
    if (!fs::is_directory(legacyRoot, ec)) return;
    for (const auto& e : fs::directory_iterator(legacyRoot, ec)) {
        if (!e.is_directory() || !fs::exists(e.path() / "haven.json", ec)) continue;
        fs::create_directories(d, ec);
        fs::copy(e.path(), d, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        if (ec) { fprintf(stderr, "[ghostmind] migrating %s: %s\n", e.path().c_str(), ec.message().c_str()); return; }
        fs::rename(d / "haven.json", d / "ghostmind.json", ec);
        fprintf(stderr, "[ghostmind] carried over the Haven wallet and chats from %s\n", e.path().c_str());
        return;
    }
}

void GhostmindImpl::openWallet()
{
    const std::string dir = dataDir();
    migrateFromHaven(dir);
    fs::create_directories(dir + "/wallet");
    loadMeta();
    loadConversations();
    const std::string config = dir + "/wallet/config.json";
    const std::string storage = dir + "/wallet/storage.json";

    if (!fs::exists(config)) {
        // The v0.3.1 wallet config: one sequencer, a long poll window because
        // testnet blocks arrive in bursts and big transactions can wait 10+.
        json cfg = {
            {"sequencer", {{"sequencer_addr", "https://testnet.lez.logos.co"}}},
            {"seq_poll_timeout", "30s"},
            {"seq_tx_poll_max_blocks", 30},
            {"seq_poll_max_retries", 8},
            {"seq_block_poll_max_amount", 100},
        };
        if (const char* seq = std::getenv("GHOSTMIND_SEQUENCER"); seq && *seq) cfg["sequencer"]["sequencer_addr"] = seq;
        std::ofstream(config) << cfg.dump(2);
    }
    if (fs::exists(storage)) {
        m_wallet = wallet_ffi_open(config.c_str(), storage.c_str());
        if (!m_wallet) throw std::runtime_error("Couldn't open the wallet in " + dir);
    } else {
        std::string password;
        {
            std::random_device rd;
            uint8_t raw[24];
            for (auto& b : raw) b = uint8_t(rd() & 0xff);
            password = hex(raw, sizeof raw);
        }
        FfiCreateWalletOutput out = wallet_ffi_create_new(config.c_str(), storage.c_str(), password.c_str());
        if (!out.wallet) throw std::runtime_error("Couldn't create a wallet in " + dir);
        m_wallet = out.wallet;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_password = password;
            m_mnemonic = out.mnemonic ? out.mnemonic : "";
            m_haveMe = false;
            m_haveFaucet = false;
        }
        if (out.mnemonic) wallet_ffi_free_string(out.mnemonic);

        // A wallet made just now can't have been paid before it existed, so
        // start its private sync at the current block instead of scanning the
        // whole chain. The FFI has no setter: write the field and reopen.
        uint64_t height = 0;
        if (wallet_ffi_get_current_block_height(m_wallet, &height) == SUCCESS && height > 0) {
            wallet_ffi_save(m_wallet);
            wallet_ffi_destroy(m_wallet);
            m_wallet = nullptr;
            {
                std::ifstream in(storage);
                json st = json::parse(in, nullptr, false);
                if (!st.is_discarded() && st.contains("last_synced_block")) {
                    st["last_synced_block"] = height;
                    in.close();
                    std::ofstream(storage) << st.dump();
                }
            }
            m_wallet = wallet_ffi_open(config.c_str(), storage.c_str());
            if (!m_wallet) throw std::runtime_error("Couldn't reopen the new wallet in " + dir);
        }
    }
    if (!m_haveMe) {
        FfiBytes32 pub{}, priv{};
        check(wallet_ffi_create_account_public(m_wallet, &pub), "create_account_public");
        check(wallet_ffi_create_account_private(m_wallet, &priv), "create_account_private");
        std::lock_guard<std::mutex> lk(m_mu);
        m_me = fromFfi(pub);
        m_mePrivate = fromFfi(priv);
        m_haveMe = true;
    }
    if (!m_haveChat) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_chat = chat::newKeyPair();
        m_haveChat = true;
    }
    const std::string me58 = toBase58(m_me), priv58 = toBase58(m_mePrivate);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_meB58 = me58;
        m_mePrivateB58 = priv58;
        m_phase = "ready";
    }
    wallet_ffi_save(m_wallet);
    saveMeta();
}

void GhostmindImpl::refreshNow()
{
    uint64_t height = 0;
    check(wallet_ffi_get_current_block_height(m_wallet, &height), "get_current_block_height");

    std::string programStr;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        programStr = m_programOverride.empty() ? kDefaultProgram : m_programOverride;
    }
    Bytes32 program{};
    const bool ok = !programStr.empty() && fromBase58(programStr, program);
    const u128 gas = nativeBalance(m_me);
    try {
        syncPrivate();
    } catch (const std::exception& e) {
        fprintf(stderr, "[ghostmind] private sync: %s\n", e.what());
    }
    const u128 priv = privateBalance();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_block = int64_t(height);
        m_gas = gas;
        m_private = priv;
        if (m_programB58 != programStr) {   // another registry: forget this one
            m_records.clear();
            m_listings.clear();
            m_reviews.clear();
            m_countKnown = false;
            m_haveCouncil = false;
            m_verifiers.clear();
            m_mine.reset();
            m_nextListingRead = 0;
        }
        m_programOk = ok;
        m_program = program;
        m_programB58 = ok ? programStr : "";
        if (!ok) return;
    }
    const Bytes32 treasury = pda(program, treasurySeed());
    const std::string treasury58 = toBase58(treasury);
    const u128 treasuryBal = nativeBalance(treasury);
    Bytes32 council{};
    const bool haveCouncil = readCouncil(program, council);
    const std::vector<Verifier> verifiers = readVerifiers(program);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_treasuryB58 = treasury58;
        m_treasury = treasuryBal;
        m_haveCouncil = haveCouncil;
        m_council = council;
        m_councilB58 = haveCouncil ? toBase58(council) : "";
        m_verifiers = verifiers;
    }
    refreshDirectory(program);
    readReviews(program);
}

void GhostmindImpl::refreshDirectory(const Bytes32& program)
{
    uint64_t count = 0;
    if (!readIndex(program, count)) return;
    // Listings are write-once: fetch the ones we don't have. Records change
    // (ratings, status), so re-read them round-robin, a few per tick.
    std::vector<uint64_t> missing;
    std::vector<Bytes32> stale;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_count = count;
        m_countKnown = true;
        for (uint64_t i = 0; i < count && missing.size() < size_t(kReadsPerTick); ++i)
            if (!m_listings.count(i)) missing.push_back(i);
        if (!m_listings.empty()) {
            const int64_t now = nowMs();
            for (uint64_t k = 0; k < count && stale.size() < size_t(kReadsPerTick); ++k) {
                const uint64_t i = (m_nextListingRead + k) % count;
                auto it = m_listings.find(i);
                if (it == m_listings.end()) continue;
                auto r = m_records.find(it->second);
                if (r == m_records.end() || now - r->second.readMs > 20000) stale.push_back(it->second);
            }
            m_nextListingRead = (m_nextListingRead + uint64_t(kReadsPerTick)) % std::max<uint64_t>(count, 1);
        }
    }
    for (uint64_t i : missing) {
        std::vector<uint8_t> raw;
        if (!readShard(pda(program, listingSeed(i)), program, raw) || raw.size() != 32) continue;
        Bytes32 who{};
        std::copy(raw.begin(), raw.end(), who.begin());
        std::lock_guard<std::mutex> lk(m_mu);
        m_listings[i] = who;
        if (!m_records.count(who)) stale.push_back(who);
    }
    // Always keep my own record fresh.
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (std::find(stale.begin(), stale.end(), m_me) == stale.end() &&
            std::any_of(m_listings.begin(), m_listings.end(), [&](const auto& kv) { return kv.second == m_me; }))
            stale.push_back(m_me);
    }
    for (const Bytes32& who : stale) {
        auto rec = readRecord(program, who);
        if (!rec) continue;
        Listing l;
        l.rec = *rec;
        l.accountB58 = toBase58(who);
        l.readMs = nowMs();
        std::lock_guard<std::mutex> lk(m_mu);
        m_records[who] = l;
        if (who == m_me) m_mine = *rec;
    }
    if (missing.size() == size_t(kReadsPerTick)) m_kick = true;
}

void GhostmindImpl::readReviews(const Bytes32& program)
{
    std::vector<std::pair<Bytes32, uint64_t>> wanted;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (const Bytes32& t : m_reviewsWanted) {
            auto r = m_records.find(t);
            if (r == m_records.end()) continue;
            const uint64_t have = m_reviews.count(t) ? m_reviews[t].size() : 0;
            if (have < r->second.rec.reviews) wanted.push_back({t, have});
        }
    }
    for (const auto& [t, from] : wanted) {
        std::vector<Review> got;
        for (uint64_t i = from; i < from + 20; ++i) {
            std::vector<uint8_t> raw;
            if (!readShard(pda(program, reviewSeed(t, i)), program, raw)) break;
            auto r = decodeReview(raw);
            if (!r) break;
            got.push_back(*r);
        }
        if (got.empty()) continue;
        std::lock_guard<std::mutex> lk(m_mu);
        auto& list = m_reviews[t];
        if (list.size() == from) list.insert(list.end(), got.begin(), got.end());
    }
}

// ── jobs ────────────────────────────────────────────────────────────────

std::string GhostmindImpl::enqueue(const std::string& kind, const std::string& label, std::function<void(Job&)> run,
                               const std::string& conversation)
{
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_phase != "ready") return jsonErr("The wallet is still starting.");
        Job j;
        j.id = id = m_nextJob++;
        j.kind = kind;
        j.label = label;
        j.status = "queued";
        j.conversation = conversation;
        j.createdMs = nowMs();
        j.run = std::move(run);
        m_jobs.insert(m_jobs.begin(), std::move(j));
        if (m_jobs.size() > 40) m_jobs.resize(40);
    }
    {
        std::lock_guard<std::mutex> lk(m_qmu);
        m_queue.push_back(id);
    }
    m_qcv.notify_all();
    return jsonOk({{"job", id}});
}

void GhostmindImpl::setJob(int64_t id, const std::function<void(Job&)>& f)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& j : m_jobs) if (j.id == id) { f(j); return; }
}

// Thrown when a submitted transaction never shows up in a block.
struct NotIncluded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void GhostmindImpl::confirm(const std::string& txHash, int64_t jobId)
{
    FfiBytes32 h{};
    const bool isHash = unhex(txHash, h.data, 32);
    setJob(jobId, [&](Job& j) { if (isHash) j.tx = txHash; j.status = "confirming"; });
    if (!isHash) return;   // program_loader_deploy reports the header id here and has already waited
    const int64_t deadline = nowMs() + kConfirmTimeoutMs;
    while (m_running && nowMs() < deadline) {
        bool included = false;
        if (wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included) return;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    throw NotIncluded("The LEZ testnet didn't include this transaction within 8 minutes, so nothing was sent. It's safe to try again: the same LGO can't be spent twice.");
}

// ── chain reads ─────────────────────────────────────────────────────────

std::string GhostmindImpl::toBase58(const Bytes32& id)
{
    const FfiBytes32 f = ffi(id);
    char* s = wallet_ffi_account_id_to_base58(&f);
    if (!s) return hex(id);
    std::string out = s;
    wallet_ffi_free_string(s);
    return out;
}

bool GhostmindImpl::fromBase58(const std::string& in, Bytes32& out)
{
    std::string s;
    for (char c : in) if (c != ' ' && c != '\n' && c != '\t') s.push_back(c);
    for (const char* p : {"Public/", "public/"})
        if (s.rfind(p, 0) == 0) s = s.substr(std::strlen(p));
    if (s.size() == 64 && unhex32(s, out)) return true;
    FfiBytes32 f{};
    if (s.empty() || wallet_ffi_account_id_from_base58(s.c_str(), &f) != SUCCESS) return false;
    out = fromFfi(f);
    return true;
}

Bytes32 GhostmindImpl::pda(const Bytes32& program, const Bytes32& seed)
{
    FfiPdaSeed s{};
    std::copy(seed.begin(), seed.end(), s.data);
    return fromFfi(wallet_ffi_account_id_for_public_pda(ffi(program), s));
}

bool GhostmindImpl::readShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out)
{
    out.clear();
    const FfiBytes32 id = ffi(account);
    FfiAccount acc{};
    if (wallet_ffi_get_account_public(m_wallet, &id, &acc) != SUCCESS) return false;
    bool found = false;
    for (uintptr_t i = 0; i < acc.shards_len; ++i) {
        if (std::equal(program.begin(), program.end(), acc.shards[i].program.data)) {
            out.assign(acc.shards[i].data, acc.shards[i].data + acc.shards[i].data_len);
            found = true;
            break;
        }
    }
    wallet_ffi_free_account_data(&acc);
    return found && !out.empty();
}

bool GhostmindImpl::readIndex(const Bytes32& program, uint64_t& count)
{
    count = 0;
    std::vector<uint8_t> raw;
    if (!readShard(pda(program, indexSeed()), program, raw)) return true;   // nobody listed yet
    if (raw.size() != 8) return false;
    for (int i = 7; i >= 0; --i) count = (count << 8) | raw[size_t(i)];
    return true;
}

std::optional<Therapist> GhostmindImpl::readRecord(const Bytes32& program, const Bytes32& therapist)
{
    std::vector<uint8_t> raw;
    if (!readShard(pda(program, recordSeed(therapist)), program, raw)) return std::nullopt;
    return decodeTherapist(raw);
}

std::optional<Ticket> GhostmindImpl::readTicket(const Bytes32& program, const Bytes32& hash)
{
    std::vector<uint8_t> raw;
    if (!readShard(pda(program, ticketSeed(hash)), program, raw)) return std::nullopt;
    return decodeTicket(raw);
}

std::vector<Verifier> GhostmindImpl::readVerifiers(const Bytes32& program)
{
    std::vector<uint8_t> raw;
    if (!readShard(pda(program, verifiersSeed()), program, raw)) return {};
    return decodeVerifiers(raw).value_or(std::vector<Verifier>{});
}

bool GhostmindImpl::readCouncil(const Bytes32& program, Bytes32& out)
{
    std::vector<uint8_t> raw;
    if (!readShard(pda(program, councilSeed()), program, raw) || raw.size() != 32) return false;
    std::copy(raw.begin(), raw.end(), out.begin());
    return true;
}

u128 GhostmindImpl::nativeBalance(const Bytes32& account)
{
    const FfiBytes32 id = ffi(account);
    uint8_t out[16] = {};
    if (wallet_ffi_get_balance(m_wallet, &id, true, &out) != SUCCESS) return 0;
    u128 v = 0;
    for (int i = 15; i >= 0; --i) v = (v << 8) | out[i];
    return v;
}

// Every private account this wallet holds: the main one plus one per
// payment received (each client pays into a fresh account, see payPrivately).
u128 GhostmindImpl::privateBalance()
{
    auto balanceOf = [&](const FfiBytes32& id) {
        uint8_t out[16] = {};
        if (wallet_ffi_get_balance(m_wallet, &id, false, &out) != SUCCESS) return u128(0);
        u128 v = 0;
        for (int i = 15; i >= 0; --i) v = (v << 8) | out[i];
        return v;
    };
    FfiAccountList list{};
    if (wallet_ffi_list_accounts(m_wallet, &list) != SUCCESS) return balanceOf(ffi(m_mePrivate));
    u128 total = 0;
    for (size_t i = 0; i < list.count; ++i)
        if (!list.entries[i].is_public) total += balanceOf(list.entries[i].account_id);
    wallet_ffi_free_account_list(&list);
    return total;
}

// What can be spent in one payment: only the main private account.
u128 GhostmindImpl::spendableBalance()
{
    const FfiBytes32 id = ffi(m_mePrivate);
    uint8_t out[16] = {};
    if (wallet_ffi_get_balance(m_wallet, &id, false, &out) != SUCCESS) return 0;
    u128 v = 0;
    for (int i = 15; i >= 0; --i) v = (v << 8) | out[i];
    return v;
}

void GhostmindImpl::syncPrivate()
{
    uint64_t height = 0, synced = 0;
    check(wallet_ffi_get_current_block_height(m_wallet, &height), "get_current_block_height");
    check(wallet_ffi_get_last_synced_block(m_wallet, &synced), "get_last_synced_block");
    if (height > synced) {
        check(wallet_ffi_sync_to_block(m_wallet, height), "sync_to_block");
        synced = height;
    }
    std::lock_guard<std::mutex> lk(m_mu);
    m_synced = int64_t(synced);
}

void GhostmindImpl::ensureFaucet()
{
    if (m_haveFaucet) return;
    wallet_ffi_import_public_account(m_wallet, kFaucetKeyHex);   // harmless if already there
    std::lock_guard<std::mutex> lk(m_mu);
    m_haveFaucet = true;
}

void GhostmindImpl::ensureGas(const Bytes32& account, u128 atLeast, u128 topUp)
{
    if (nativeBalance(account) >= atLeast) return;
    Bytes32 faucet{};
    if (!fromBase58(kFaucetB58, faucet)) throw std::runtime_error("Bad faucet address.");
    ensureFaucet();
    // Everyone shares this faucet account, so simultaneous top-ups collide on
    // its nonce; retry a couple of blocks later.
    std::string lastErr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        try {
            const FfiBytes32 f = ffi(faucet), t = ffi(account);
            uint8_t amount[16] = {};
            u128Le(topUp, &amount);
            FfiTransferResult res{};
            check(wallet_ffi_transfer_public(m_wallet, &f, &t, &amount, &res), "transfer_public");
            const std::string tx = res.tx_hash ? res.tx_hash : "";
            wallet_ffi_free_transfer_result(&res);
            FfiBytes32 h{};
            if (unhex(tx, h.data, 32)) {
                const int64_t deadline = nowMs() + kConfirmTimeoutMs;
                bool included = false;
                while (nowMs() < deadline &&
                       !(wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included))
                    std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            if (nativeBalance(account) >= atLeast) return;
        } catch (const std::exception& e) {
            lastErr = e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(15 + 10 * attempt));
    }
    throw std::runtime_error("Couldn't get LGO from the testnet faucet" + (lastErr.empty() ? "." : ": " + lastErr));
}

std::string GhostmindImpl::sendCall(const Bytes32& program, const std::vector<std::tuple<Bytes32, Bytes32, bool>>& accounts,
                                const std::vector<uint8_t>& instruction, const Bytes32* payer)
{
    std::vector<FfiAccountMention> m(accounts.size());
    for (size_t i = 0; i < accounts.size(); ++i) {
        const auto& [id, owner, signs] = accounts[i];
        check(wallet_ffi_resolve_public_account(ffi(id), signs, &m[i].identity), "resolve_public_account");
        m[i].program_account_id = ffi(owner);
    }
    const FfiBytes32 p = ffi(payer ? *payer : m_me);
    FfiTransactionResult res{};
    const WalletFfiError e = wallet_ffi_send_generic_public_transaction(
        m_wallet, m.data(), m.size(), instruction.data(), instruction.size(), ffi(program), &p, &res);
    for (auto& x : m) wallet_ffi_free_account_identity(&x.identity);
    check(e, "send_generic_public_transaction");
    const std::string tx = res.tx_hash ? res.tx_hash : "";
    const bool ok = res.success;
    wallet_ffi_free_transaction_result(&res);
    if (!ok) throw std::runtime_error("The sequencer rejected the transaction.");
    return tx;
}

std::string GhostmindImpl::shieldNow(u128 amount, int64_t jobId)
{
    setJob(jobId, [](Job& j) { j.status = "proving"; });
    const FfiBytes32 from = ffi(m_me), to = ffi(m_mePrivate);
    uint8_t amt[16] = {};
    u128Le(amount, &amt);
    FfiTransferResult res{};
    check(wallet_ffi_transfer_shielded_owned(m_wallet, &from, &to, &amt, nullptr, &res), "transfer_shielded_owned");
    const std::string tx = res.tx_hash ? res.tx_hash : "";
    const bool ok = res.success;
    wallet_ffi_free_transfer_result(&res);
    if (!ok) throw std::runtime_error("The sequencer rejected the shielding transfer.");
    confirm(tx, jobId);
    syncPrivate();
    return tx;
}

std::string GhostmindImpl::payPrivately(const std::vector<uint8_t>& receive, u128 amount, int64_t jobId)
{
    if (receive.size() != kReceiveLen) throw std::runtime_error("The therapist's receiving keys are malformed.");
    FfiPrivateAccountKeys keys{};
    std::copy(receive.begin(), receive.begin() + 32, keys.nullifier_public_key.data);
    keys.viewing_public_key = receive.data() + 32;
    keys.viewing_public_key_len = kVpkLen;
    // Pay into a FRESH private account under the therapist's keys every time.
    // A transfer to someone else's private account always initialises it, and
    // the initialisation nullifier is fixed per (keys, identifier): reusing the
    // listing's identifier makes every payment after the first a duplicate
    // that the sequencer silently drops. The therapist's wallet finds each new
    // account by its view tag during sync, so nothing has to be announced.
    //
    // If the testnet drops a payment, build it again (new identifier, fresh
    // proof) and resend, up to three times. That can't pay twice: both
    // attempts spend the same note, so at most one is ever accepted. Before
    // each resend, check whether the earlier one landed late after all.
    const FfiBytes32 from = ffi(m_mePrivate);
    uint8_t amt[16] = {};
    u128Le(amount, &amt);
    syncPrivate();
    const u128 before = spendableBalance();
    constexpr int kAttempts = 3;
    for (int attempt = 1;; ++attempt) {
        FfiIdentifier ident{};
        randombytes_buf(ident.data, sizeof ident.data);
        setJob(jobId, [](Job& j) { j.status = "proving"; });
        FfiTransferResult res{};
        check(wallet_ffi_transfer_private(m_wallet, &from, &keys, &ident, &amt, &res), "transfer_private");
        const std::string tx = res.tx_hash ? res.tx_hash : "";
        const bool ok = res.success;
        wallet_ffi_free_transfer_result(&res);
        if (!ok) throw std::runtime_error("The sequencer rejected the private payment.");
        try {
            confirm(tx, jobId);
            syncPrivate();
            return tx;
        } catch (const NotIncluded&) {
            syncPrivate();
            if (spendableBalance() + amount <= before) return tx;   // it landed after all
            if (attempt >= kAttempts)
                throw std::runtime_error("The LEZ testnet dropped this payment " + std::to_string(kAttempts) +
                                         " times, so nothing was sent and your LGO is still yours. The testnet may be struggling; try again later.");
            fprintf(stderr, "[ghostmind] payment %s wasn't included; resending (attempt %d)\n", tx.c_str(), attempt + 1);
        }
    }
}

std::string GhostmindImpl::deshieldTo(const Bytes32& to, u128 amount, int64_t jobId)
{
    setJob(jobId, [](Job& j) { j.status = "proving"; });
    const FfiBytes32 from = ffi(m_mePrivate), t = ffi(to);
    uint8_t amt[16] = {};
    u128Le(amount, &amt);
    FfiTransferResult res{};
    check(wallet_ffi_transfer_deshielded(m_wallet, &from, &t, &amt, &res), "transfer_deshielded");
    const std::string tx = res.tx_hash ? res.tx_hash : "";
    const bool ok = res.success;
    wallet_ffi_free_transfer_result(&res);
    if (!ok) throw std::runtime_error("The sequencer rejected the funding transfer.");
    confirm(tx, jobId);
    syncPrivate();
    return tx;
}

std::vector<uint8_t> GhostmindImpl::myReceivingKeys()
{
    const FfiBytes32 id = ffi(m_mePrivate);
    FfiPrivateAccountKeys keys{};
    check(wallet_ffi_get_private_account_keys(m_wallet, &id, &keys), "get_private_account_keys");
    std::vector<uint8_t> out(keys.nullifier_public_key.data, keys.nullifier_public_key.data + 32);
    out.insert(out.end(), keys.viewing_public_key, keys.viewing_public_key + keys.viewing_public_key_len);
    wallet_ffi_free_private_account_keys(&keys);
    FfiAccountIdentity ident{};
    check(wallet_ffi_resolve_private_account(m_wallet, id, &ident), "resolve_private_account");
    out.insert(out.end(), ident.identifier.data, ident.identifier.data + 32);
    wallet_ffi_free_account_identity(&ident);
    if (out.size() != kReceiveLen) throw std::runtime_error("Unexpected receiving key size.");
    return out;
}

Profile GhostmindImpl::buildProfile(const std::string& name, const std::string& bio, const std::string& specialties, u128 price)
{
    Profile p;
    p.name = clip(trim(name), kMaxName);
    p.bio = clip(trim(bio), kMaxBio);
    p.specialties = clip(trim(specialties), kMaxSpecialties);
    p.price = price;
    p.receive = myReceivingKeys();
    std::lock_guard<std::mutex> lk(m_mu);
    p.chatKey = m_chat.pk;
    return p;
}

Bytes32 GhostmindImpl::programOrThrow()
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_programOk) throw std::runtime_error("No registry program configured.");
    return m_program;
}

// ── messaging ───────────────────────────────────────────────────────────

void GhostmindImpl::wireDeliveryEvents()
{
    if (m_eventsWired) return;
    m_eventsWired = true;
    // delivery_module 0.3.x: (messageHash, contentTopic, payload, source, timestamp).
    modules().delivery_module.onMessageReceived(
        [this](const std::string&, const std::string& contentTopic, const std::vector<uint8_t>& payload,
               const std::string&, int64_t) { onWire(contentTopic, payload); });
    modules().delivery_module.onConnectionStateChanged([this](const std::string& status, int64_t) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_net = status.find("Connected") != std::string::npos ? 2 : 1;
    });
}

bool GhostmindImpl::startNetworkNow(std::string& err)
{
    // One caller at a time: delivery_module 0.4.0 aborts the host on a second start().
    std::lock_guard<std::mutex> netLk(m_netMu);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_started) return true;
        m_net = 1;
        m_netError.clear();
    }
    if (!m_nodeCreated) {
        // A second Basecamp on the same machine sets GHOSTMIND_TCPPORT so the two
        // don't collide on the delivery port, keeps a fixed key so the first
        // can dial it over loopback, and the pair never have to meet through
        // the public fleet. Everyone else gets a node key of their own: a key
        // every install shares can't be on the network twice.
        const char* portEnv = std::getenv("GHOSTMIND_TCPPORT");
        const int customPort = portEnv ? std::atoi(portEnv) : 0;
        const bool isB = customPort > 0;
        const int tcpPort = isB ? customPort : 60000;
        const int udpPort = isB ? 9000 + (tcpPort - 60000) : 9000;
        static const char* KEY_B = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f21";
        static const char* PEERID_B = "16Uiu2HAmKCaJ7sfcm1aHY8TAShdsKttUHvDwTPbgTkJhLQu8AMiG";

        std::string nodeKey;
        if (isB) {
            nodeKey = KEY_B;
        } else {
            const std::string path = dataDir() + "/node.key";
            std::ifstream in(path);
            if (!(in >> nodeKey) || nodeKey.size() != 64) {
                nodeKey = chat::randomHex(32);
                std::ofstream(path, std::ios::trunc) << nodeKey;
                ::chmod(path.c_str(), 0600);
            }
        }
        json cfg;
        cfg["logLevel"] = "INFO";
        cfg["mode"] = "Core";
        cfg["preset"] = "logos.dev";
        // The logos.dev fleet runs cluster 3; an explicit clusterId wins over the preset.
        const char* clusterEnv = std::getenv("GHOSTMIND_CLUSTER");
        cfg["clusterId"] = clusterEnv && *clusterEnv ? std::atoi(clusterEnv) : 3;
        cfg["relay"] = true;
        cfg["tcpPort"] = tcpPort;
        cfg["discv5UdpPort"] = udpPort;
        cfg["nodeKey"] = nodeKey;
        json statics = json::array();
        if (!isB) statics.push_back(std::string("/ip4/127.0.0.1/tcp/60001/p2p/") + PEERID_B);
        if (const char* extra = std::getenv("GHOSTMIND_STATIC_PEER"); extra && *extra) statics.push_back(extra);
        cfg["staticNodes"] = statics;

        const StdLogosResult r = modules().delivery_module.createNode(cfg.dump());
        const bool alreadyUp = !r.success && (r.error.find("already") != std::string::npos ||
                                              r.error.find("Already") != std::string::npos);
        if (!r.success && !alreadyUp) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_net = 3;
            m_netError = err = r.error.empty() ? "createNode failed" : r.error;
            return false;
        }
        m_nodeCreated = true;
    }
    wireDeliveryEvents();
    const StdLogosResult s = modules().delivery_module.start();
    if (!s.success) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_net = 3;
        m_netError = err = s.error.empty() ? "start failed" : s.error;
        return false;
    }
    std::vector<std::string> topics;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_started = true;
        if (m_net < 2) m_net = 2;
        topics.push_back(chat::inboxTopic(m_chat.pk));
        for (const auto& [id, c] : m_convs) topics.push_back(chat::convTopic(id));
    }
    for (const auto& t : topics) {
        const StdLogosResult sub = modules().delivery_module.subscribe(t);
        if (sub.success) { std::lock_guard<std::mutex> lk(m_mu); m_subscribed.insert(t); }
        else fprintf(stderr, "[ghostmind] subscribe %s: %s\n", t.c_str(), sub.error.c_str());
    }
    return true;
}

void GhostmindImpl::subscribeConv(const std::string& convId)
{
    const std::string topic = chat::convTopic(convId);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_started || m_subscribed.count(topic)) return;
    }
    queueWire("", std::vector<uint8_t>(topic.begin(), topic.end()));
}

// Delivery calls are blocking RPCs into another process. Nothing here makes
// one from inside a delivery callback or while holding m_mu: sends and
// subscriptions queue up and this thread drains them.
void GhostmindImpl::queueWire(const std::string& topic, std::vector<uint8_t> payload)
{
    {
        std::lock_guard<std::mutex> lk(m_outMu);
        m_outbox.emplace_back(topic, std::move(payload));
    }
    m_outCv.notify_all();
}

void GhostmindImpl::netLoop()
{
    while (m_running) {
        std::pair<std::string, std::vector<uint8_t>> item;
        {
            std::unique_lock<std::mutex> lk(m_outMu);
            m_outCv.wait_for(lk, std::chrono::seconds(1), [this] { return !m_running || !m_outbox.empty(); });
            if (!m_running) break;
            if (m_outbox.empty()) {
                // The worker can sit in one payment for many minutes on a slow
                // testnet; save chat changes from here so they don't wait on it.
                lk.unlock();
                if (m_convDirty.exchange(false)) saveConversations();
                continue;
            }
            item = std::move(m_outbox.front());
            m_outbox.pop_front();
        }
        bool started;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            started = m_started;
        }
        if (!started) {
            std::string err;
            if (!startNetworkNow(err)) {
                fprintf(stderr, "[ghostmind] dropping a message, messaging is down: %s\n", err.c_str());
                continue;
            }
        }
        if (item.first.empty()) {
            const std::string topic(item.second.begin(), item.second.end());
            const StdLogosResult sub = modules().delivery_module.subscribe(topic);
            if (sub.success) { std::lock_guard<std::mutex> lk(m_mu); m_subscribed.insert(topic); }
            else fprintf(stderr, "[ghostmind] subscribe %s: %s\n", topic.c_str(), sub.error.c_str());
        } else {
            modules().delivery_module.send(item.first, item.second);
        }
    }
}

GhostmindImpl::Conversation* GhostmindImpl::conv(const std::string& id)
{
    auto it = m_convs.find(id);
    return it == m_convs.end() ? nullptr : &it->second;
}

void GhostmindImpl::onWire(const std::string& topic, const std::vector<uint8_t>& payload)
{
    if (payload.empty()) return;
    static const std::string inbox = "/ghostmind-inbox/1/", convp = "/ghostmind-conv/1/";
    if (topic.rfind(inbox, 0) == 0) {
        std::string plain;
        chat::KeyPair me;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            me = m_chat;
        }
        if (chat::openHello(me, payload, plain)) onHello(plain);
    } else if (topic.rfind(convp, 0) == 0) {
        const size_t end = topic.find('/', convp.size());
        if (end == std::string::npos) return;
        onConvMessage(topic.substr(convp.size(), end - convp.size()), payload);
    }
}

void GhostmindImpl::onHello(const std::string& plaintext)
{
    json h = json::parse(plaintext, nullptr, false);
    if (h.is_discarded() || !h.is_object()) return;
    const std::string id = h.value("conv", "");
    Bytes32 pk{};
    if (id.size() != 32 || !unhex32(h.value("pk", ""), pk)) return;
    const std::string purpose = h.value("purpose", "therapy");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_convs.count(id)) return;   // Store replayed it, or a duplicate
        Conversation c;
        c.id = id;
        c.role = "host";
        c.purpose = purpose == "verification" ? "verification" : "therapy";
        c.peerPk = pk;
        c.myPk = m_chat.pk;
        c.mySk = m_chat.sk;
        if (!chat::sharedKey(pk, m_chat.sk, c.key)) return;
        c.peerAccount = c.purpose == "verification" ? h.value("account", "") : "";
        c.peerName = c.purpose == "verification" ? clip(trim(h.value("name", "")), kMaxName) : "Client " + id.substr(0, 4);
        if (c.peerName.empty()) c.peerName = "Therapist " + shortId(c.peerAccount);
        c.lastMs = nowMs();
        c.acked = true;
        Message m;
        m.id = chat::randomHex(8);
        m.from = "peer";
        m.kind = "system";
        m.text = c.purpose == "verification" ? "Sent you their licence details to check." : "Started a conversation with you.";
        m.time = c.lastMs;
        c.messages.push_back(m);
        c.unread = 1;
        m_convs[id] = c;
        m_convDirty = true;
    }
    subscribeConv(id);
    sendOn(id, "ack", "");
}

void GhostmindImpl::onConvMessage(const std::string& convId, const std::vector<uint8_t>& payload)
{
    std::string plain;
    Bytes32 key{};
    std::string myRole;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(convId);
        if (!c) return;
        key = c->key;
        myRole = c->role;
    }
    if (!chat::decrypt(key, payload, plain)) return;
    json m = json::parse(plain, nullptr, false);
    if (m.is_discarded() || !m.is_object()) return;
    if (m.value("from", "") == myRole) return;   // our own message, echoed back
    const std::string id = m.value("id", "");
    std::vector<std::vector<uint8_t>> flush;
    const std::string kind = m.value("t", "msg");
    std::string notify;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(convId);
        if (!c) return;
        if (std::any_of(c->messages.begin(), c->messages.end(), [&](const Message& x) { return x.id == id; })) return;
        Message x;
        x.id = id.empty() ? chat::randomHex(8) : id;
        x.from = "peer";
        x.kind = kind;
        x.time = m.value("time", nowMs());
        if (kind == "ack") {
            c->acked = true;
            flush.swap(c->held);
            x.kind = "system";
            x.text = c->purpose == "verification" ? "Delivered to the licensing body." : "Delivered. The therapist has your message.";
        } else if (kind == "msg") {
            x.text = clip(m.value("text", ""), 4000);
        } else if (kind == "paid" && c->role == "host" && c->purpose == "therapy") {
            c->paidTx = m.value("tx", "");
            c->paidAmount = m.value("amount", "");
            c->ticketHash = m.value("hash", "");
            u128 amt = 0;
            for (char ch : c->paidAmount) if (ch >= '0' && ch <= '9') amt = amt * 10 + u128(ch - '0');
            x.text = "The client paid " + formatLgo(amt) + " LGO privately. Confirming it now…";
            notify = "ticket";
        } else if (kind == "ticket" && c->role == "client") {
            c->ticketHash = m.value("hash", c->ticketHash);
            c->ticketTx = m.value("tx", "");
            c->ticketIssued = true;
            x.text = "Payment confirmed. You can leave an anonymous review whenever you like.";
        } else if (kind == "request" && c->role == "host" && c->purpose == "verification") {
            c->licence = clip(m.value("licence", ""), 120);
            c->jurisdiction = clip(m.value("jurisdiction", ""), kMaxJurisdiction);
            c->expires = clip(m.value("expires", ""), 20);
            x.text = "Licence " + c->licence + " · " + c->jurisdiction + (c->expires.empty() ? "" : " · expires " + c->expires);
        } else if (kind == "verified" && c->role == "client" && c->purpose == "verification") {
            x.kind = "system";
            x.text = "Your licence was checked and verified.";
        } else {
            x.text = clip(m.value("text", ""), 4000);
        }
        c->messages.push_back(x);
        if (c->messages.size() > kMaxMessages) c->messages.erase(c->messages.begin());
        c->lastMs = x.time;
        c->unread += 1;
        m_convDirty = true;
    }
    for (auto& w : flush) queueWire(chat::convTopic(convId), std::move(w));
    if (notify == "ticket") autoIssueTicket(convId);
}

void GhostmindImpl::sendOn(const std::string& convId, const std::string& kind, const std::string& text, const json& extra)
{
    std::vector<uint8_t> wire;
    std::string topic;
    bool hold = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(convId);
        if (!c) return;
        hold = c->role == "client" && !c->acked;
        json m = extra;
        m["v"] = 1;
        m["id"] = chat::randomHex(8);
        m["from"] = c->role;
        m["t"] = kind;
        m["time"] = nowMs();
        if (!text.empty()) m["text"] = text;
        wire = chat::encrypt(c->key, m.dump());
        topic = chat::convTopic(convId);
        if (hold) c->held.push_back(wire);
        if (kind != "ack") {
            Message x;
            x.id = m["id"].get<std::string>();
            x.from = "me";
            x.kind = kind;
            x.text = text;
            x.time = m["time"].get<int64_t>();
            c->messages.push_back(x);
            if (c->messages.size() > kMaxMessages) c->messages.erase(c->messages.begin());
            c->lastMs = x.time;
            m_convDirty = true;
        }
    }
    if (!hold) queueWire(topic, std::move(wire));
}

std::string GhostmindImpl::openTo(const Bytes32& hostChatKey, const std::string& peerAccount, const std::string& peerName,
                              const std::string& purpose, const std::string& helloExtra)
{
    Conversation c;
    c.id = chat::randomHex(16);
    c.role = "client";
    c.purpose = purpose;
    c.peerPk = hostChatKey;
    const chat::KeyPair mine = chat::newKeyPair();
    c.myPk = mine.pk;
    c.mySk = mine.sk;
    if (!chat::sharedKey(hostChatKey, mine.sk, c.key)) return jsonErr("Couldn't derive a conversation key.");
    c.peerAccount = peerAccount;
    c.peerName = peerName;
    c.lastMs = nowMs();
    json hello = json::parse(helloExtra.empty() ? "{}" : helloExtra, nullptr, false);
    if (hello.is_discarded()) hello = json::object();
    hello["v"] = 1;
    hello["conv"] = c.id;
    hello["pk"] = hex(c.myPk);
    hello["purpose"] = purpose;
    hello["time"] = c.lastMs;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_convs[c.id] = c;
        m_convDirty = true;
    }
    subscribeConv(c.id);
    queueWire(chat::inboxTopic(hostChatKey), chat::sealHello(hostChatKey, hello.dump()));
    return jsonOk({{"conversation", c.id}});
}

void GhostmindImpl::autoIssueTicket(const std::string& convId)
{
    std::string hashHex, amountStr;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(convId);
        if (!c || c->ticketIssued) return;
        hashHex = c->ticketHash;
        amountStr = c->paidAmount;
    }
    Bytes32 hash{};
    if (!unhex32(hashHex, hash)) return;
    u128 amount = 0;
    for (char ch : amountStr) if (ch >= '0' && ch <= '9') amount = amount * 10 + u128(ch - '0');
    enqueue("ticket", "Confirm a client's payment", [this, convId, hash, amount](Job& job) {
        const Bytes32 program = programOrThrow();
        auto mine = readRecord(program, m_me);
        if (!mine) throw std::runtime_error("You aren't listed in this registry.");
        if (mine->status != Listed) throw std::runtime_error("Your listing isn't active, so tickets can't be issued.");
        // The payment is private: what we can see is our own private balance
        // rising by at least the amount, once the wallet has synced the block.
        bool seen = false;
        for (int attempt = 0; attempt < 12 && m_running; ++attempt) {
            syncPrivate();
            const u128 bal = privateBalance();
            u128 baseline;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                baseline = m_privateBaseline;
                m_private = bal;
            }
            if (bal >= baseline + amount) {
                std::lock_guard<std::mutex> lk(m_mu);
                m_privateBaseline = baseline + amount;
                m_dirty = true;
                seen = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(20));
        }
        if (!seen) throw std::runtime_error("No private payment of that size has reached this wallet yet.");
        const u128 fee = ticketFee(mine->profile.price);
        ensureGas(m_me, fee + kLgo / 20, fee + kLgo);
        Bytes32 native{};
        const std::vector<uint8_t> ins = encodeIssueTicket(hash, fee, uint64_t(nowMs() / 1000));
        const std::string tx = sendCall(program,
                                        {{pda(program, recordSeed(m_me)), program, false},
                                         {pda(program, ticketSeed(hash)), program, false},
                                         {m_me, native, true},
                                         {pda(program, treasurySeed()), native, false}},
                                        ins);
        confirm(tx, job.id);
        bool landed = false;
        for (int i = 0; i < 8 && !landed; ++i) {
            if (readTicket(program, hash)) landed = true;
            else std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        if (!landed) throw std::runtime_error("The ticket transaction landed but the ticket isn't there — the fee may have been refused.");
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (Conversation* c = conv(convId)) { c->ticketIssued = true; c->ticketTx = tx; m_convDirty = true; }
        }
        sendOn(convId, "ticket", "Payment confirmed. This client can now leave a review.", {{"hash", hex(hash)}, {"tx", tx}});
        m_kick = true;
    }, convId);
}

// ── API: state ──────────────────────────────────────────────────────────

std::string GhostmindImpl::state()
{
    start();
    std::lock_guard<std::mutex> lk(m_mu);
    auto verifierName = [&](const Bytes32& a) {
        for (const auto& v : m_verifiers) if (v.account == a) return v.name;
        return std::string();
    };
    auto therapistJson = [&](const Listing& l) {
        const Therapist& t = l.rec;
        json j = {
            {"account", l.accountB58}, {"name", t.profile.name}, {"bio", t.profile.bio},
            {"specialties", t.profile.specialties}, {"price", formatLgo(t.profile.price)},
            {"priceAtomic", u128ToString(t.profile.price)}, {"chatKey", hex(t.profile.chatKey)},
            {"status", t.status}, {"reason", t.reason}, {"verified", t.credential.has_value()},
            {"tickets", t.tickets}, {"ratingCount", t.ratingCount},
            {"rating", t.ratingCount ? double(t.ratingSum) / double(t.ratingCount) : 0.0},
            {"reports", t.reports}, {"reviews", t.reviews}, {"registered", t.registered},
            {"mine", t.account == m_me}, {"ticketFee", formatLgo(ticketFee(t.profile.price))},
        };
        if (t.credential) {
            const auto& c = *t.credential;
            j["credential"] = {{"verifier", toBase58(c.verifier)}, {"verifierName", verifierName(c.verifier)},
                               {"jurisdiction", c.jurisdiction}, {"expires", formatDate(c.expires)},
                               {"expired", c.expires && c.expires < uint64_t(nowMs() / 1000)},
                               {"licenceHash", hex(c.licenceHash)}, {"time", c.time}};
        }
        if (auto r = m_reviews.find(t.account); r != m_reviews.end()) {
            json arr = json::array();
            for (const auto& v : r->second)
                arr.push_back({{"score", v.score}, {"text", v.text}, {"report", v.report}, {"time", v.time}});
            j["reviewList"] = arr;
        }
        return j;
    };

    json therapists = json::array();
    for (uint64_t i = 0; i < m_count; ++i) {
        auto l = m_listings.find(i);
        if (l == m_listings.end()) continue;
        auto r = m_records.find(l->second);
        if (r == m_records.end()) continue;
        therapists.push_back(therapistJson(r->second));
    }
    json verifiers = json::array();
    for (const auto& v : m_verifiers)
        verifiers.push_back({{"account", toBase58(v.account)}, {"name", v.name}, {"chatKey", hex(v.chatKey)}, {"active", v.active}});

    json convs = json::array();
    for (const auto& [id, c] : m_convs) {
        json msgs = json::array();
        const size_t from = c.messages.size() > 100 ? c.messages.size() - 100 : 0;
        for (size_t i = from; i < c.messages.size(); ++i) {
            const Message& m = c.messages[i];
            msgs.push_back({{"id", m.id}, {"from", m.from}, {"kind", m.kind}, {"text", m.text}, {"time", m.time}});
        }
        u128 amt = 0;
        for (char ch : c.paidAmount) if (ch >= '0' && ch <= '9') amt = amt * 10 + u128(ch - '0');
        json j = {
            {"id", c.id}, {"role", c.role}, {"purpose", c.purpose}, {"peerAccount", c.peerAccount},
            {"peerName", c.peerName}, {"acked", c.acked}, {"unread", c.unread}, {"lastMs", c.lastMs},
            {"paid", !c.paidTx.empty()}, {"paidTx", c.paidTx}, {"paidAmount", c.paidTx.empty() ? "" : formatLgo(amt)},
            {"ticketIssued", c.ticketIssued}, {"ticketHash", c.ticketHash}, {"ticketTx", c.ticketTx},
            {"hasSecret", !c.ticketSecret.empty()}, {"rated", c.rated},
            {"licence", c.licence}, {"jurisdiction", c.jurisdiction}, {"expires", c.expires},
            {"messages", msgs},
        };
        for (const auto& job : m_jobs)
            if (job.conversation == c.id && job.status != "done" && job.status != "failed") {
                j["busy"] = {{"kind", job.kind}, {"status", job.status}, {"label", job.label}};
                break;
            }
        Bytes32 peer{};
        if (!c.peerAccount.empty() && fromBase58(c.peerAccount, peer))
            if (auto r = m_records.find(peer); r != m_records.end()) j["therapist"] = therapistJson(r->second);
        convs.push_back(j);
    }

    json jobs = json::array();
    for (const auto& j : m_jobs)
        jobs.push_back({{"id", j.id}, {"kind", j.kind}, {"label", j.label}, {"status", j.status}, {"tx", j.tx},
                        {"error", j.error}, {"result", j.result}, {"conversation", j.conversation},
                        {"created", j.createdMs}, {"finished", j.finishedMs}});

    const bool isCouncil = m_haveCouncil && m_council == m_me;
    const bool isVerifier = std::any_of(m_verifiers.begin(), m_verifiers.end(),
                                        [&](const Verifier& v) { return v.active && v.account == m_me; });
    json me = {
        {"address", m_meB58}, {"privateAddress", m_mePrivateB58}, {"name", m_myName},
        {"lgo", formatLgo(m_gas)}, {"lgoPrivate", formatLgo(m_private)}, {"hasGas", m_gas >= kLgo / 100},
        {"chatKey", m_haveChat ? hex(m_chat.pk) : ""}, {"isCouncil", isCouncil}, {"isVerifier", isVerifier},
        {"isTherapist", m_mine.has_value()}, {"hasRecovery", !m_mnemonic.empty()}, {"mode", m_mode},
    };
    if (m_mine) {
        Listing l;
        l.rec = *m_mine;
        l.accountB58 = m_meB58;
        me["therapist"] = therapistJson(l);
    }
    return json{
        {"phase", m_phase},
        {"error", m_error},
        {"network", {{"block", m_block}, {"synced", m_synced}, {"online", m_online}, {"messaging", m_net},
                     {"messagingError", m_netError}}},
        {"program", {{"address", m_programB58}, {"configured", m_programOk}, {"custom", !m_programOverride.empty()},
                     {"council", m_councilB58}, {"hasCouncil", m_haveCouncil}, {"treasury", m_treasuryB58},
                     {"treasuryLgo", formatLgo(m_treasury)}}},
        {"count", m_countKnown ? json(m_count) : json(nullptr)},
        {"therapists", therapists},
        {"verifiers", verifiers},
        {"conversations", convs},
        {"me", me},
        {"jobs", jobs},
    }.dump();
}

// ── API: clients ────────────────────────────────────────────────────────

std::string GhostmindImpl::openConversation(const std::string& therapistAddress)
{
    Bytes32 who{};
    if (!fromBase58(therapistAddress, who)) return jsonErr("That isn't a valid therapist address.");
    Bytes32 chatKey{};
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_phase != "ready") return jsonErr("The wallet is still starting.");
        auto r = m_records.find(who);
        if (r == m_records.end()) return jsonErr("That therapist isn't in the registry.");
        if (r->second.rec.status != Listed) return jsonErr("That therapist isn't taking clients right now.");
        chatKey = r->second.rec.profile.chatKey;
        name = r->second.rec.profile.name;
        for (const auto& [id, c] : m_convs)
            if (c.role == "client" && c.purpose == "therapy" && c.peerAccount == r->second.accountB58 && c.paidTx.empty())
                return jsonOk({{"conversation", id}, {"existing", true}});
    }
    return openTo(chatKey, toBase58(who), name, "therapy", "");
}

std::string GhostmindImpl::sendMessage(const std::string& conversationId, const std::string& text)
{
    const std::string t = trim(text);
    if (t.empty()) return jsonErr("Write something first.");
    if (t.size() > 4000) return jsonErr("That message is too long.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!conv(conversationId)) return jsonErr("No such conversation.");
    }
    sendOn(conversationId, "msg", t);
    return jsonOk();
}

std::string GhostmindImpl::pay(const std::string& conversationId)
{
    u128 price = 0;
    std::vector<uint8_t> receive;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(conversationId);
        if (!c) return jsonErr("No such conversation.");
        if (c->role != "client" || c->purpose != "therapy") return jsonErr("Only a client pays in a conversation.");
        if (!c->paidTx.empty()) return jsonErr("This conversation is already paid for.");
        Bytes32 who{};
        if (!fromBase58(c->peerAccount, who)) return jsonErr("The therapist's address is unreadable.");
        auto r = m_records.find(who);
        if (r == m_records.end()) return jsonErr("The therapist's listing hasn't loaded yet.");
        if (r->second.rec.status != Listed) return jsonErr("That therapist isn't listed right now.");
        price = r->second.rec.profile.price;
        receive = r->second.rec.profile.receive;
        name = r->second.rec.profile.name;
        for (const auto& j : m_jobs)
            if (j.conversation == conversationId && j.kind == "pay" && j.status != "done" && j.status != "failed")
                return jsonErr("That payment is already in progress.");
    }
    return enqueue("pay", "Pay " + formatLgo(price) + " LGO privately to " + name, [this, conversationId, price, receive](Job& job) {
        // Private LGO comes from the private balance; top it up from the
        // public one (shielding) when it's short, and the public one from the
        // faucet before that. Shielding is the only step that shows on chain,
        // and it shows a transfer into a private note, not who it's for.
        syncPrivate();
        u128 priv = spendableBalance();
        if (priv < price) {
            const u128 need = price - priv;
            ensureGas(m_me, need + kLgo / 20, need + kLgo);
            shieldNow(need, job.id);
            for (int i = 0; i < 10 && (priv = spendableBalance()) < price; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(6));
                syncPrivate();
            }
            if (priv < price) throw std::runtime_error("Shielded, but the private balance hasn't caught up yet. Try again in a minute.");
        }
        const std::string tx = payPrivately(receive, price, job.id);
        const Bytes32 secret = chat::randomBytes32();
        const Bytes32 hash = sha256(bytesOf(secret));
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (Conversation* c = conv(conversationId)) {
                c->paidTx = tx;
                c->paidAmount = u128ToString(price);
                c->ticketSecret = hex(secret);
                c->ticketHash = hex(hash);
                m_convDirty = true;
            }
            m_private = privateBalance();
        }
        sendOn(conversationId, "paid", "You paid " + formatLgo(price) + " LGO privately.",
               {{"tx", tx}, {"amount", u128ToString(price)}, {"hash", hex(hash)}});
    }, conversationId);
}

std::string GhostmindImpl::rate(const std::string& conversationId, int64_t score, const std::string& textIn, bool report)
{
    if (score < 1 || score > 5) return jsonErr("A score is 1 to 5.");
    const std::string text = clip(trim(textIn), kMaxReview);
    Bytes32 secret{}, therapist{};
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(conversationId);
        if (!c) return jsonErr("No such conversation.");
        if (c->rated) return jsonErr("You already rated this session.");
        if (c->ticketSecret.empty() || !unhex32(c->ticketSecret, secret)) return jsonErr("Pay for the session first.");
        if (!fromBase58(c->peerAccount, therapist)) return jsonErr("The therapist's address is unreadable.");
        for (const auto& j : m_jobs)
            if (j.conversation == conversationId && j.kind == "rate" && j.status != "done" && j.status != "failed")
                return jsonErr("That review is already on its way.");
    }
    return enqueue("rate", std::string(report ? "Report" : "Rate") + " the session", [this, conversationId, secret, therapist, score, text, report](Job& job) {
        const Bytes32 program = programOrThrow();
        const Bytes32 hash = sha256(bytesOf(secret));
        auto ticket = readTicket(program, hash);
        if (!ticket) throw std::runtime_error("Your review ticket isn't on the chain yet. The therapist's module issues it once the payment lands.");
        if (ticket->used) throw std::runtime_error("That ticket was already used.");
        // The review goes out from a brand-new account so nothing ties it to
        // this wallet. Its fee money comes out of the private balance, which
        // the chain can't trace either; the faucet stands in when that's empty.
        FfiBytes32 raterF{};
        check(wallet_ffi_create_account_public(m_wallet, &raterF), "create_account_public");
        const Bytes32 rater = fromFfi(raterF);
        wallet_ffi_save(m_wallet);
        syncPrivate();
        if (spendableBalance() >= kRaterFunding) deshieldTo(rater, kRaterFunding, job.id);
        else ensureGas(rater, kRaterFunding, kRaterFunding * 2);
        setJob(job.id, [](Job& j) { j.status = "working"; });
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto rec = readRecord(program, therapist);
            if (!rec) throw std::runtime_error("The therapist's listing is gone.");
            const uint64_t index = rec->reviews;
            const std::vector<uint8_t> ins = encodeRate(therapist, secret, index, uint8_t(score), text, report, uint64_t(nowMs() / 1000));
            const std::string tx = sendCall(program,
                                            {{pda(program, recordSeed(therapist)), program, false},
                                             {pda(program, ticketSeed(hash)), program, false},
                                             {pda(program, reviewSeed(therapist, index)), program, false}},
                                            ins, &rater);
            confirm(tx, job.id);
            for (int i = 0; i < 8; ++i) {
                auto t = readTicket(program, hash);
                if (t && t->used) {
                    std::lock_guard<std::mutex> lk(m_mu);
                    if (Conversation* c = conv(conversationId)) {
                        c->rated = true;
                        Message x;
                        x.id = chat::randomHex(8);
                        x.from = "me";
                        x.kind = "system";
                        x.text = std::string(report ? "You reviewed and reported" : "You reviewed") + " this session (" + std::to_string(score) + "/5). It can't be traced back to you.";
                        x.time = nowMs();
                        c->messages.push_back(x);
                        m_convDirty = true;
                    }
                    m_kick = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            setJob(job.id, [](Job& j) { j.status = "working"; j.tx.clear(); });
        }
        throw std::runtime_error("The registry was busy — the review didn't land. Try again.");
    }, conversationId);
}

std::string GhostmindImpl::setMode(const std::string& mode)
{
    if (mode != "client" && mode != "therapist" && !mode.empty()) return jsonErr("Unknown mode.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_mode = mode;
    }
    saveMeta();
    return jsonOk();
}

std::string GhostmindImpl::markRead(const std::string& conversationId)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (Conversation* c = conv(conversationId)) { if (c->unread) { c->unread = 0; m_convDirty = true; } }
    return jsonOk();
}

std::string GhostmindImpl::closeConversation(const std::string& conversationId)
{
    std::string topic;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_convs.erase(conversationId)) return jsonErr("No such conversation.");
        topic = chat::convTopic(conversationId);
        m_subscribed.erase(topic);
        m_convDirty = true;
    }
    modules().delivery_module.unsubscribe(topic);
    return jsonOk();
}

std::string GhostmindImpl::loadReviews(const std::string& therapistAddress)
{
    Bytes32 who{};
    if (!fromBase58(therapistAddress, who)) return jsonErr("That isn't a valid therapist address.");
    std::lock_guard<std::mutex> lk(m_mu);
    m_reviewsWanted.insert(who);
    m_kick = true;
    m_qcv.notify_all();
    return jsonOk();
}

// ── API: therapists ─────────────────────────────────────────────────────

std::string GhostmindImpl::registerTherapist(const std::string& name, const std::string& bio, const std::string& specialties,
                                         const std::string& priceLgo)
{
    if (trim(name).empty()) return jsonErr("A listing needs a name.");
    const auto price = parseLgo(priceLgo);
    if (!price || *price == 0) return jsonErr("Enter a session price in LGO, like 2.5.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_programOk) return jsonErr("No registry program configured.");
        if (m_mine) return jsonErr("You're already listed — edit your profile instead.");
        m_myName = clip(trim(name), kMaxName);
        m_dirty = true;
    }
    return enqueue("register", "List " + clip(trim(name), 30) + " in the registry", [this, name, bio, specialties, price](Job& job) {
        const Bytes32 program = programOrThrow();
        if (readRecord(program, m_me)) {
            m_kick = true;
            throw std::runtime_error("You're already listed — edit your profile instead.");
        }
        ensureGas(m_me, kLgo / 10, kLgo);
        const Profile p = buildProfile(name, bio, specialties, *price);
        const uint64_t time = uint64_t(nowMs() / 1000);
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint64_t index = 0;
            if (!readIndex(program, index)) throw std::runtime_error("The registry index is unreadable.");
            const std::string tx = sendCall(program,
                                            {{pda(program, indexSeed()), program, false},
                                             {pda(program, listingSeed(index)), program, false},
                                             {pda(program, recordSeed(m_me)), program, false},
                                             {m_me, program, true}},
                                            encodeRegister(index, p, time));
            confirm(tx, job.id);
            for (int i = 0; i < 8; ++i) {
                if (auto rec = readRecord(program, m_me)) {
                    std::lock_guard<std::mutex> lk(m_mu);
                    m_mine = *rec;
                    m_listings[index] = m_me;
                    Listing l;
                    l.rec = *rec;
                    l.accountB58 = m_meB58;
                    l.readMs = nowMs();
                    m_records[m_me] = l;
                    m_kick = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            setJob(job.id, [](Job& j) { j.status = "working"; j.tx.clear(); });
        }
        throw std::runtime_error("The registry was busy — your listing didn't land. Try again.");
    });
}

std::string GhostmindImpl::updateProfile(const std::string& name, const std::string& bio, const std::string& specialties,
                                     const std::string& priceLgo)
{
    if (trim(name).empty()) return jsonErr("A listing needs a name.");
    const auto price = parseLgo(priceLgo);
    if (!price || *price == 0) return jsonErr("Enter a session price in LGO, like 2.5.");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_mine) return jsonErr("You aren't listed yet.");
        m_myName = clip(trim(name), kMaxName);
        m_dirty = true;
    }
    return enqueue("update", "Update your listing", [this, name, bio, specialties, price](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const Profile p = buildProfile(name, bio, specialties, *price);
        const std::string tx = sendCall(program, {{pda(program, recordSeed(m_me)), program, false}, {m_me, program, true}},
                                        encodeUpdate(p, uint64_t(nowMs() / 1000)));
        confirm(tx, job.id);
        if (auto rec = readRecord(program, m_me)) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_mine = *rec;
            if (auto r = m_records.find(m_me); r != m_records.end()) { r->second.rec = *rec; r->second.readMs = nowMs(); }
        }
        m_kick = true;
    });
}

std::string GhostmindImpl::requestVerification(const std::string& verifierAddress, const std::string& licence,
                                           const std::string& jurisdiction, const std::string& expiresIso)
{
    Bytes32 who{};
    if (!fromBase58(verifierAddress, who)) return jsonErr("That isn't a valid verifier address.");
    if (trim(licence).empty()) return jsonErr("Enter your licence number.");
    if (trim(jurisdiction).empty()) return jsonErr("Enter the jurisdiction that issued it.");
    Verifier v;
    bool found = false;
    std::string myName;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_mine) return jsonErr("List yourself first, then ask for verification.");
        for (const auto& x : m_verifiers) if (x.account == who && x.active) { v = x; found = true; }
        myName = m_mine->profile.name;
    }
    if (!found) return jsonErr("That verifier isn't active in this registry.");
    const json hello = {{"account", m_meB58}, {"name", myName}};
    const std::string r = openTo(v.chatKey, toBase58(who), v.name, "verification", hello.dump());
    json rj = json::parse(r, nullptr, false);
    if (rj.is_discarded() || !rj.value("ok", false)) return r;
    const std::string id = rj.value("conversation", "");
    sendOn(id, "request", "Licence " + trim(licence) + " · " + trim(jurisdiction) + (trim(expiresIso).empty() ? "" : " · expires " + trim(expiresIso)),
           {{"licence", trim(licence)}, {"jurisdiction", trim(jurisdiction)}, {"expires", trim(expiresIso)}});
    return r;
}

std::string GhostmindImpl::issueTicket(const std::string& conversationId)
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        Conversation* c = conv(conversationId);
        if (!c) return jsonErr("No such conversation.");
        if (c->role != "host" || c->purpose != "therapy") return jsonErr("Tickets are issued to paying clients.");
        if (c->paidTx.empty()) return jsonErr("This client hasn't reported a payment yet.");
        if (c->ticketIssued) return jsonErr("A ticket was already issued here.");
    }
    autoIssueTicket(conversationId);
    return jsonOk();
}

// ── API: verifiers ──────────────────────────────────────────────────────

std::string GhostmindImpl::verifyTherapist(const std::string& therapistAddress, const std::string& licence,
                                       const std::string& jurisdiction, const std::string& expiresIso)
{
    Bytes32 who{};
    if (!fromBase58(therapistAddress, who)) return jsonErr("That isn't a valid therapist address.");
    if (trim(licence).empty()) return jsonErr("Enter the licence number you checked.");
    const std::string jur = clip(trim(jurisdiction), kMaxJurisdiction);
    if (jur.empty()) return jsonErr("Enter the issuing jurisdiction.");
    const uint64_t expires = parseDate(expiresIso);
    if (!trim(expiresIso).empty() && !expires) return jsonErr("The expiry date should look like 2027-12-31.");
    const Bytes32 hash = licenceHash(licence, jur);
    // Every open request from this therapist hears about it: a therapist may
    // have asked more than once, and any of those is the one they're watching.
    std::vector<std::string> convIds;
    std::string convId;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        int64_t newest = -1;
        for (const auto& [id, c] : m_convs)
            if (c.role == "host" && c.purpose == "verification" && c.peerAccount == therapistAddress) {
                convIds.push_back(id);
                if (!c.licence.empty() && c.lastMs > newest) { newest = c.lastMs; convId = id; }
            }
    }
    return enqueue("verify", "Attest " + shortId(therapistAddress), [this, who, hash, jur, expires, convIds](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const std::string tx = sendCall(program,
                                        {{pda(program, verifiersSeed()), program, false},
                                         {pda(program, recordSeed(who)), program, false},
                                         {m_me, program, true}},
                                        encodeVerify(who, hash, jur, expires, uint64_t(nowMs() / 1000)));
        confirm(tx, job.id);
        auto rec = readRecord(program, who);
        if (!rec || !rec->credential) throw std::runtime_error("Included, but the credential isn't on the record — are you an active verifier?");
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (auto r = m_records.find(who); r != m_records.end()) { r->second.rec = *rec; r->second.readMs = nowMs(); }
        }
        for (const auto& id : convIds) sendOn(id, "verified", "Your licence is verified.", {{"tx", tx}});
        m_kick = true;
    }, convId);
}

std::string GhostmindImpl::revokeTherapist(const std::string& therapistAddress)
{
    Bytes32 who{};
    if (!fromBase58(therapistAddress, who)) return jsonErr("That isn't a valid therapist address.");
    return enqueue("revoke", "Revoke " + shortId(therapistAddress), [this, who](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const std::string tx = sendCall(program,
                                        {{pda(program, verifiersSeed()), program, false},
                                         {pda(program, recordSeed(who)), program, false},
                                         {m_me, program, true}},
                                        encodeRevoke(who));
        confirm(tx, job.id);
        auto rec = readRecord(program, who);
        if (!rec || rec->credential) throw std::runtime_error("Included, but the credential is still there — only its issuer can revoke it.");
        m_kick = true;
    });
}

// ── API: council ────────────────────────────────────────────────────────

std::string GhostmindImpl::initCouncil()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_programOk) return jsonErr("No registry program configured.");
        if (m_haveCouncil) return jsonErr("This registry already has a council.");
    }
    return enqueue("init", "Become the council", [this](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const std::string tx = sendCall(program, {{pda(program, councilSeed()), program, false}, {m_me, program, true}}, encodeInit());
        confirm(tx, job.id);
        Bytes32 c{};
        if (!readCouncil(program, c) || c != m_me) throw std::runtime_error("Included, but the council isn't this wallet — someone else got there first.");
        m_kick = true;
    });
}

std::string GhostmindImpl::setVerifier(const std::string& verifierAddress, const std::string& name, const std::string& chatKeyHex,
                                   bool active)
{
    Bytes32 who{}, chatKey{};
    if (!fromBase58(verifierAddress, who)) return jsonErr("That isn't a valid verifier address.");
    if (!unhex32(trim(chatKeyHex), chatKey)) return jsonErr("The verifier's chat key is 64 hex characters — they can copy it from their Wallet tab.");
    const std::string n = clip(trim(name), kMaxName);
    if (n.empty()) return jsonErr("Give the verifier a name.");
    return enqueue("verifier", std::string(active ? "Appoint " : "Disable ") + n, [this, who, n, chatKey, active](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const std::string tx = sendCall(program,
                                        {{pda(program, councilSeed()), program, false},
                                         {pda(program, verifiersSeed()), program, false},
                                         {m_me, program, true}},
                                        encodeSetVerifier(who, n, chatKey, active));
        confirm(tx, job.id);
        const auto list = readVerifiers(program);
        const bool there = std::any_of(list.begin(), list.end(), [&](const Verifier& v) { return v.account == who && v.active == active; });
        if (!there) throw std::runtime_error("Included, but the verifier list didn't change — only the council can do that.");
        std::lock_guard<std::mutex> lk(m_mu);
        m_verifiers = list;
        m_kick = true;
    });
}

std::string GhostmindImpl::suspendTherapist(const std::string& therapistAddress, const std::string& reason)
{
    Bytes32 who{};
    if (!fromBase58(therapistAddress, who)) return jsonErr("That isn't a valid therapist address.");
    const std::string r = clip(trim(reason), kMaxReason);
    return enqueue("suspend", "Suspend " + shortId(therapistAddress), [this, who, r](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const std::string tx = sendCall(program,
                                        {{pda(program, councilSeed()), program, false},
                                         {pda(program, recordSeed(who)), program, false},
                                         {m_me, program, true}},
                                        encodeSuspend(who, r));
        confirm(tx, job.id);
        auto rec = readRecord(program, who);
        if (!rec || rec->status != Suspended) throw std::runtime_error("Included, but the listing isn't suspended — only the council can do that.");
        std::lock_guard<std::mutex> lk(m_mu);
        if (auto x = m_records.find(who); x != m_records.end()) { x->second.rec = *rec; x->second.readMs = nowMs(); }
        m_kick = true;
    });
}

std::string GhostmindImpl::reinstateTherapist(const std::string& therapistAddress)
{
    Bytes32 who{};
    if (!fromBase58(therapistAddress, who)) return jsonErr("That isn't a valid therapist address.");
    return enqueue("reinstate", "Reinstate " + shortId(therapistAddress), [this, who](Job& job) {
        const Bytes32 program = programOrThrow();
        ensureGas(m_me, kLgo / 20, kLgo);
        const std::string tx = sendCall(program,
                                        {{pda(program, councilSeed()), program, false},
                                         {pda(program, recordSeed(who)), program, false},
                                         {m_me, program, true}},
                                        encodeReinstate(who));
        confirm(tx, job.id);
        auto rec = readRecord(program, who);
        if (!rec || rec->status != Listed) throw std::runtime_error("Included, but the listing isn't back — only the council can do that.");
        std::lock_guard<std::mutex> lk(m_mu);
        if (auto x = m_records.find(who); x != m_records.end()) { x->second.rec = *rec; x->second.readMs = nowMs(); }
        m_kick = true;
    });
}

// ── API: wallet ─────────────────────────────────────────────────────────

std::string GhostmindImpl::getGas()
{
    return enqueue("gas", "Get LGO from the faucet", [this](Job&) { ensureGas(m_me, kLgo / 2, kLgo); });
}

std::string GhostmindImpl::shield(const std::string& amountLgo)
{
    const auto amount = parseLgo(amountLgo);
    if (!amount || *amount == 0) return jsonErr("Enter an amount in LGO, like 1.5.");
    return enqueue("shield", "Shield " + formatLgo(*amount) + " LGO", [this, amount](Job& job) {
        ensureGas(m_me, *amount + kLgo / 20, *amount + kLgo);
        shieldNow(*amount, job.id);
        std::lock_guard<std::mutex> lk(m_mu);
        m_private = privateBalance();
        m_privateBaseline = m_private;   // your own shielding isn't a client's payment
        m_dirty = true;
    });
}

std::string GhostmindImpl::recoveryPhrase()
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_mnemonic.empty()) return jsonErr("This wallet was created without keeping its recovery words.");
    return jsonOk({{"phrase", m_mnemonic}});
}

std::string GhostmindImpl::setProgram(const std::string& address)
{
    std::lock_guard<std::mutex> lk(m_mu);
    std::string a;
    for (char c : address) if (c != ' ' && c != '\n' && c != '\t') a.push_back(c);
    m_programOverride = a;
    m_dirty = true;
    m_kick = true;
    m_qcv.notify_all();
    return jsonOk();
}

std::string GhostmindImpl::deploy(const std::string& binPath)
{
    std::ifstream f(binPath, std::ios::binary);
    if (!f) return jsonErr("Can't read " + binPath);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (elf.size() < 16 || std::string(elf.begin(), elf.begin() + 4) != "R0BF") return jsonErr(binPath + " isn't a risc0 program binary.");
    return enqueue("deploy", "Deploy the Ghostmind program", [this, elf](Job& job) {
        ensureGas(m_me, 3 * kLgo, 6 * kLgo);
        // A risc0 ProgramBinary: "R0BF" | u32 version | u32 header_len | header |
        // u32 user_len | user elf | kernel. Only the user elf is uploaded, in
        // write-once 96 KiB segments, and the count has to match exactly.
        auto u32at = [&](size_t o) { return uint32_t(elf[o]) | uint32_t(elf[o + 1]) << 8 | uint32_t(elf[o + 2]) << 16 | uint32_t(elf[o + 3]) << 24; };
        const size_t userLen = u32at(12 + u32at(8));
        constexpr size_t kSegment = 96 * 1024;
        const size_t segments = (userLen + kSegment - 1) / kSegment;
        FfiBytes32 header{};
        check(wallet_ffi_create_account_public(m_wallet, &header), "create_account_public");
        std::vector<FfiBytes32> segs(segments);
        for (auto& s : segs) check(wallet_ffi_create_account_public(m_wallet, &s), "create_account_public");
        wallet_ffi_save(m_wallet);
        const FfiBytes32 payer = ffi(m_me);
        FfiTransactionResult res{};
        check(wallet_ffi_program_loader_deploy(m_wallet, &header, segs.data(), segs.size(), elf.data(), elf.size(),
                                               true, &payer, &res),
              "program_loader_deploy");
        const std::string tx = res.tx_hash ? res.tx_hash : "";
        const bool ok = res.success;
        wallet_ffi_free_transaction_result(&res);
        if (!ok) throw std::runtime_error("The deployment was rejected.");
        confirm(tx, job.id);
        const std::string address = toBase58(fromFfi(header));
        setJob(job.id, [&](Job& j) { j.result = address; });
        std::lock_guard<std::mutex> lk(m_mu);
        m_programOverride = address;
        m_kick = true;
    });
}

std::string GhostmindImpl::startNetwork()
{
    std::string err;
    if (!startNetworkNow(err)) return jsonErr(err);
    return jsonOk();
}
