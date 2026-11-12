#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "logos_module_context.h"
#include "ghostmind_codec.h"

struct WalletHandle;

/**
 * @brief Ghostmind — anonymous therapy on Logos.
 *
 * Three things, glued together by one embedded LEZ wallet and one delivery
 * node:
 *
 *  - **The registry** is a program on the Logos Execution Zone
 *    (ghostmind-program/). Therapists list themselves; accredited verifiers,
 *    appointed by a council, attest their licence; paying clients rate them;
 *    the council (or three client reports) can take a listing down.
 *  - **Conversations** run over Logos delivery topics, end-to-end encrypted.
 *    A client opens one with a fresh key sealed to the therapist's published
 *    chat key, so the therapist — and the network — learn no identity.
 *  - **Payment** is a private-to-private LGO transfer to the therapist's
 *    published receiving keys. The chain shows no sender, no recipient and no
 *    amount. When it lands, the therapist's module issues the client a review
 *    ticket on-chain and pays the ticket fee, which is what keeps ratings
 *    honest: a review needs a ticket, and a ticket costs money.
 *
 * Universal module: these public methods are the API. Wallet and network
 * work happens on one worker thread; `state()` returns a cached snapshot the
 * UI polls once a second. Amounts cross the bridge as decimal LGO strings.
 */
class GhostmindImpl : public LogosModuleContext
{
public:
    GhostmindImpl();
    ~GhostmindImpl();

    /// Everything the UI draws, as one JSON object. Never blocks.
    std::string state();

    // ── clients ───────────────────────────────────────────────────────
    /// Start an anonymous conversation with a listed therapist. Returns {"conversation": id}.
    std::string openConversation(const std::string& therapistAddress);
    /// Say something in a conversation.
    std::string sendMessage(const std::string& conversationId, const std::string& text);
    /// Pay the therapist's listed price privately. Shields LGO first if the
    /// private balance is short. The therapist's module answers with a ticket.
    std::string pay(const std::string& conversationId);
    /// Spend the ticket: score 1–5, optional text, and whether this is a
    /// report. Sent from a fresh account funded out of the private balance.
    std::string rate(const std::string& conversationId, int64_t score, const std::string& text, bool report);
    /// Remember who uses this app: "client" or "therapist" ("" = not chosen).
    /// Only changes which screens the UI shows.
    std::string setMode(const std::string& mode);
    /// Mark a conversation read.
    std::string markRead(const std::string& conversationId);
    /// Forget a conversation (keys and all).
    std::string closeConversation(const std::string& conversationId);
    /// Load a therapist's reviews from the chain into state().
    std::string loadReviews(const std::string& therapistAddress);

    // ── therapists ────────────────────────────────────────────────────
    /// List yourself. `priceLgo` is per session.
    std::string registerTherapist(const std::string& name, const std::string& bio, const std::string& specialties,
                                  const std::string& priceLgo);
    std::string updateProfile(const std::string& name, const std::string& bio, const std::string& specialties,
                              const std::string& priceLgo);
    /// Ask a verifier to attest your licence: opens an encrypted conversation
    /// with them carrying the details. Nothing goes on-chain here.
    std::string requestVerification(const std::string& verifierAddress, const std::string& licence,
                                    const std::string& jurisdiction, const std::string& expiresIso);
    /// Issue a review ticket by hand (the module does this itself when a
    /// payment arrives in a conversation).
    std::string issueTicket(const std::string& conversationId);

    // ── verifiers ─────────────────────────────────────────────────────
    /// Attest a therapist. The chain gets sha256(licence ‖ jurisdiction), not the number.
    std::string verifyTherapist(const std::string& therapistAddress, const std::string& licence,
                                const std::string& jurisdiction, const std::string& expiresIso);
    std::string revokeTherapist(const std::string& therapistAddress);

    // ── council ───────────────────────────────────────────────────────
    /// Claim an uninitialised registry for this wallet.
    std::string initCouncil();
    std::string setVerifier(const std::string& verifierAddress, const std::string& name, const std::string& chatKeyHex,
                            bool active);
    std::string suspendTherapist(const std::string& therapistAddress, const std::string& reason);
    std::string reinstateTherapist(const std::string& therapistAddress);

    // ── wallet ────────────────────────────────────────────────────────
    /// Top up the public account from the testnet faucet.
    std::string getGas();
    /// Move LGO from the public balance into the private one.
    std::string shield(const std::string& amountLgo);
    /// The wallet's recovery words.
    std::string recoveryPhrase();
    /// Point at a different deployment of the program; empty resets to the built-in one.
    std::string setProgram(const std::string& address);
    /// Deploy ghostmind.bin from this wallet and switch to it.
    std::string deploy(const std::string& binPath);
    /// Bring the delivery node up (done automatically once the wallet is ready).
    std::string startNetwork();

protected:
    void onContextReady() override;
    /// Tests: pick the data directory before the first call.
    void setDataDir(const std::string& dir) { m_dataDirOverride = dir; }

private:
    using Bytes32 = ghostmind::Bytes32;
    using u128 = ghostmind::u128;

    struct Job {
        int64_t     id = 0;
        std::string kind;
        std::string label;
        std::string status;      // queued, working, proving, confirming, done, failed
        std::string tx;
        std::string error;
        std::string result;
        std::string conversation;
        int64_t     createdMs = 0;
        int64_t     finishedMs = 0;
        std::function<void(Job&)> run;
    };

    struct Message {
        std::string id;
        std::string from;        // "me" or "peer"
        std::string kind;        // msg, paid, ticket, request, system
        std::string text;
        int64_t     time = 0;
    };

    struct Conversation {
        std::string id;
        std::string role;        // "client": I opened it; "host": it was opened to my chat key
        std::string purpose;     // "therapy" or "verification"
        Bytes32     peerPk{};    // their key for this conversation
        Bytes32     myPk{}, mySk{};
        Bytes32     key{};       // shared
        std::string peerAccount; // therapist / verifier address (client side); therapist address (verification host side)
        std::string peerName;
        bool        acked = false;
        // payment and the ticket it earns
        std::string paidTx;
        std::string paidAmount;  // atomic, decimal
        std::string ticketHash;  // hex
        std::string ticketSecret;// hex, client side only
        std::string ticketTx;
        bool        ticketIssued = false;
        bool        rated = false;
        // verification request details (host side)
        std::string licence, jurisdiction, expires;
        std::vector<Message> messages;
        // The opener's messages wait here until the host acks: the host only
        // subscribes to the conversation topic once it has opened the hello,
        // so anything sent before that would be lost. Not persisted.
        std::vector<std::vector<uint8_t>> held;
        int64_t     lastMs = 0;
        int         unread = 0;
    };

    struct Listing {
        ghostmind::Therapist rec;
        std::string accountB58;
        int64_t     readMs = 0;
    };

    // ── lifecycle ─────────────────────────────────────────────────────
    void start();
    void workerLoop();
    void openWallet();
    void refreshNow();
    void refreshDirectory(const Bytes32& program);
    void readReviews(const Bytes32& program);
    std::string enqueue(const std::string& kind, const std::string& label, std::function<void(Job&)> run,
                        const std::string& conversation = "");
    void setJob(int64_t id, const std::function<void(Job&)>& f);
    void confirm(const std::string& txHash, int64_t jobId);

    // ── chain (worker thread) ─────────────────────────────────────────
    bool readShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out);
    Bytes32 pda(const Bytes32& program, const Bytes32& seed);
    bool readIndex(const Bytes32& program, uint64_t& count);
    std::optional<ghostmind::Therapist> readRecord(const Bytes32& program, const Bytes32& therapist);
    std::optional<ghostmind::Ticket> readTicket(const Bytes32& program, const Bytes32& hash);
    std::vector<ghostmind::Verifier> readVerifiers(const Bytes32& program);
    bool readCouncil(const Bytes32& program, Bytes32& out);
    u128 nativeBalance(const Bytes32& account);
    u128 privateBalance();
    u128 spendableBalance();
    void syncPrivate();
    std::string toBase58(const Bytes32& id);
    bool fromBase58(const std::string& s, Bytes32& out);
    void ensureGas(const Bytes32& account, u128 atLeast, u128 topUp);
    void ensureFaucet();
    /// A public call: {account, shard owner, signs}. `payer` null means `m_me`.
    std::string sendCall(const Bytes32& program, const std::vector<std::tuple<Bytes32, Bytes32, bool>>& accounts,
                         const std::vector<uint8_t>& instruction, const Bytes32* payer = nullptr);
    std::string shieldNow(u128 amount, int64_t jobId);
    std::string payPrivately(const std::vector<uint8_t>& receive, u128 amount, int64_t jobId);
    std::string deshieldTo(const Bytes32& to, u128 amount, int64_t jobId);
    std::vector<uint8_t> myReceivingKeys();
    ghostmind::Profile buildProfile(const std::string& name, const std::string& bio, const std::string& specialties,
                                u128 price);
    Bytes32 programOrThrow();
    std::optional<Bytes32> councilOrNull();

    // ── messaging ─────────────────────────────────────────────────────
    bool startNetworkNow(std::string& err);
    void wireDeliveryEvents();
    void onWire(const std::string& topic, const std::vector<uint8_t>& payload);
    void onHello(const std::string& plaintext);
    void onConvMessage(const std::string& convId, const std::vector<uint8_t>& payload);
    void sendOn(const std::string& convId, const std::string& kind, const std::string& text,
                const nlohmann::json& extra = nlohmann::json::object());
    void subscribeConv(const std::string& convId);
    void queueWire(const std::string& topic, std::vector<uint8_t> payload);
    void netLoop();
    void autoIssueTicket(const std::string& convId);
    std::string openTo(const Bytes32& hostChatKey, const std::string& peerAccount, const std::string& peerName,
                       const std::string& purpose, const std::string& hello);
    Conversation* conv(const std::string& id);   // m_mu held

    // ── persistence ───────────────────────────────────────────────────
    std::string dataDir() const;
    void loadMeta();
    void saveMeta();
    void loadConversations();
    void saveConversations();

    // ── fields ────────────────────────────────────────────────────────
    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::mutex              m_qmu;
    std::condition_variable m_qcv;
    std::deque<int64_t>     m_queue;
    std::atomic<bool>       m_kick{false};
    std::atomic<bool>       m_dirty{false};
    std::atomic<bool>       m_convDirty{false};
    std::string             m_dataDirOverride;

    WalletHandle* m_wallet = nullptr;   // worker thread only

    mutable std::mutex m_mu;            // guards everything below
    std::string m_phase = "starting";
    std::string m_error;
    int64_t     m_block = 0;
    int64_t     m_synced = 0;
    bool        m_online = false;

    std::string m_mnemonic, m_password;
    bool        m_haveMe = false;
    Bytes32     m_me{}, m_mePrivate{};
    std::string m_meB58, m_mePrivateB58;
    u128        m_gas = 0, m_private = 0;
    u128        m_privateBaseline = 0;  // what incoming payments are measured against
    bool        m_haveFaucet = false;
    ghostmind::KeyPair m_chat;              // my chat key: therapists and verifiers publish pk
    bool        m_haveChat = false;
    std::string m_myName;
    std::string m_mode;
    std::mutex m_saveMu;

    std::string m_programOverride;
    Bytes32     m_program{};
    std::string m_programB58, m_treasuryB58;
    bool        m_programOk = false;
    bool        m_haveCouncil = false;
    Bytes32     m_council{};
    std::string m_councilB58;
    std::vector<ghostmind::Verifier> m_verifiers;
    bool        m_countKnown = false;
    uint64_t    m_count = 0;
    std::map<uint64_t, Bytes32> m_listings;
    std::map<Bytes32, Listing> m_records;
    uint64_t    m_nextListingRead = 0;
    std::set<Bytes32> m_reviewsWanted;
    std::map<Bytes32, std::vector<ghostmind::Review>> m_reviews;
    std::optional<ghostmind::Therapist> m_mine;
    u128        m_treasury = 0;

    std::map<std::string, Conversation> m_convs;
    int m_net = 0;                      // 0 off, 1 connecting, 2 up, 3 error
    std::string m_netError;
    bool m_nodeCreated = false, m_started = false, m_eventsWired = false;
    std::mutex m_netMu;
    std::set<std::string> m_subscribed;
    std::thread m_netThread;
    std::mutex m_outMu;
    std::condition_variable m_outCv;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> m_outbox;   // "" topic = subscribe to payload

    std::vector<Job> m_jobs;
    int64_t          m_nextJob = 1;
};
