//! Ghostmind — a registry of therapists on the Logos Execution Zone, with
//! verification by accredited verifiers, reputation from paying clients only,
//! and a council that can suspend.
//!
//! Layout, all in this program's own shards of public PDAs it derives itself:
//!
//!   council     PDA(self, "haven/council")                   32 bytes: the council's account
//!   verifiers   PDA(self, "haven/verifiers")                 Borsh Vec<Verifier>
//!   index       PDA(self, "haven/index")                     u64 LE: number of listed therapists
//!   listing n   PDA(self, "haven/listing/" ‖ n_le)           32 bytes: the n-th therapist's account
//!   record T    PDA(self, sha256("haven/therapist/" ‖ T))    Borsh Therapist
//!   ticket h    PDA(self, sha256("haven/ticket/" ‖ h))       Borsh Ticket, h = sha256(secret)
//!   review T,n  PDA(self, sha256("haven/review/" ‖ T ‖ n))   Borsh Review
//!   treasury    PDA(self, "haven/treasury")                  native LGO shard: ticket fees land here
//!
//! Who may do what is decided in `apply`, against the stored state: a plan
//! can only see account ids and signatures, so "is this signer the council"
//! is an effect on the council account that leaves it unchanged and panics
//! if the stored authority differs. The same trick checks verifiers.
//!
//! Reputation: a therapist who has been paid issues a review *ticket* for
//! that client — the hash of a secret only the client knows — and pays a
//! fee into the treasury for it (2% of the listed price, at least 0.01 LGO).
//! Whoever presents the secret can rate once. Tickets are issued when the
//! payment lands, before the session, so a therapist can't withhold the right
//! to complain, and faking reviews costs the fee each time.

use borsh::{BorshDeserialize, BorshSerialize};
use lee_core::{
    account::{AccountId, ProgramShardSelector},
    native_token::{Instruction as NativeInstruction, NATIVE_TOKEN_PROGRAM_ID},
    program::{AccountMeta, ChainedCall, PdaSeed, Plan, PlanInput, run_program},
};
use risc0_zkvm::sha::{Impl, Sha256 as _};

type Bytes32 = [u8; 32];

const MAX_NAME: usize = 60;
const MAX_BIO: usize = 1000;
const MAX_SPECIALTIES: usize = 200;
const MAX_JURISDICTION: usize = 60;
const MAX_REASON: usize = 300;
const MAX_REVIEW: usize = 600;
/// npk (32) + ML-KEM-768 viewing key (1184) + identifier (32), with slack.
const MAX_RECEIVE: usize = 1300;
/// 0.01 LGO, in atomic units.
const MIN_TICKET_FEE: u128 = 10_000_000;
/// 2% of the listed session price.
const TICKET_FEE_BPS: u128 = 200;
/// Reports from paying clients before a listing is hidden pending the council.
const REPORTS_TO_FLAG: u64 = 3;

const LISTED: u8 = 0;
const SUSPENDED: u8 = 1;
const FLAGGED: u8 = 2;

#[derive(BorshDeserialize)]
enum Instruction {
    /// [council, authority(signs)] — claim an uninitialised registry.
    Init,
    /// [council, verifiers, signer(signs)] — council adds, renames or disables a verifier.
    SetVerifier { verifier: Bytes32, name: String, chat_key: Bytes32, active: bool },
    /// [index, listing n, record, therapist(signs)] — list a new therapist.
    Register { index: u64, profile: Profile, time: u64 },
    /// [record, therapist(signs)] — edit a listing.
    Update { profile: Profile, time: u64 },
    /// [verifiers, record, verifier(signs)] — attest a therapist's licence.
    Verify { therapist: Bytes32, licence_hash: Bytes32, jurisdiction: String, expires: u64, time: u64 },
    /// [verifiers, record, verifier(signs)] — withdraw one's own attestation.
    Revoke { therapist: Bytes32 },
    /// [council, record, signer(signs)]
    Suspend { therapist: Bytes32, reason: String },
    /// [council, record, signer(signs)] — back to listed, reports cleared.
    Reinstate { therapist: Bytes32 },
    /// [record, ticket, therapist(native shard, signs), treasury(native shard)]
    IssueTicket { hash: Bytes32, fee: u128, time: u64 },
    /// [record, ticket, review n] — anyone holding the ticket's secret.
    Rate { therapist: Bytes32, secret: Bytes32, index: u64, score: u8, text: String, report: bool, time: u64 },
}

#[derive(BorshSerialize, BorshDeserialize, Clone)]
struct Profile {
    name: String,
    bio: String,
    specialties: String,
    /// Per session, in atomic LGO.
    price: u128,
    /// Private receiving keys: npk ‖ vpk ‖ identifier. Paying this needs no
    /// public account on either side.
    receive: Vec<u8>,
    /// X25519 public key: clients seal their first message to it.
    chat_key: Bytes32,
}

#[derive(BorshSerialize, BorshDeserialize)]
struct Credential {
    verifier: Bytes32,
    /// sha256 of the licence details the verifier checked; the chain never
    /// holds the licence number itself.
    licence_hash: Bytes32,
    jurisdiction: String,
    expires: u64,
    time: u64,
}

#[derive(BorshSerialize, BorshDeserialize)]
struct Therapist {
    account: Bytes32,
    profile: Profile,
    status: u8,
    reason: String,
    credential: Option<Credential>,
    tickets: u64,
    rating_sum: u64,
    rating_count: u64,
    reports: u64,
    reviews: u64,
    registered: u64,
    updated: u64,
}

#[derive(BorshSerialize, BorshDeserialize)]
struct Verifier {
    account: Bytes32,
    name: String,
    chat_key: Bytes32,
    active: bool,
}

#[derive(BorshSerialize, BorshDeserialize)]
struct Ticket {
    therapist: Bytes32,
    issued: u64,
    used: bool,
}

#[derive(BorshSerialize, BorshDeserialize)]
struct Review {
    ticket: Bytes32,
    score: u8,
    text: String,
    report: bool,
    time: u64,
}

#[derive(BorshSerialize, BorshDeserialize)]
enum Effect {
    /// A signer's own account: only here to sign.
    Keep,
    /// Council account: must be empty, becomes this authority.
    InitCouncil(Bytes32),
    /// Council account: must hold this signer. Unchanged.
    RequireCouncil(Bytes32),
    /// Verifiers list: add or replace by account.
    SetVerifier(Verifier),
    /// Verifiers list: this signer must be an active verifier. Unchanged.
    RequireVerifier(Bytes32),
    /// Index: must hold `expected`, becomes `expected + 1`.
    Bump { expected: u64 },
    /// Listing / record / ticket / review: must be empty, becomes this.
    WriteOnce(Vec<u8>),
    Update { profile: Profile, signer: Bytes32, time: u64 },
    SetCredential(Credential),
    ClearCredential { verifier: Bytes32 },
    SetStatus { status: u8, reason: String },
    Issue { signer: Bytes32, fee: u128 },
    Consume { therapist: Bytes32 },
    AddReview { index: u64, score: u8, report: bool, time: u64 },
}

// ── derivations ────────────────────────────────────────────────────────────

fn sha256(parts: &[&[u8]]) -> Bytes32 {
    let mut bytes = Vec::new();
    for p in parts {
        bytes.extend_from_slice(p);
    }
    Impl::hash_bytes(&bytes).as_bytes().try_into().expect("sha256 is 32 bytes")
}

fn tag_seed(tag: &[u8]) -> PdaSeed {
    let mut s = [0u8; 32];
    s[..tag.len()].copy_from_slice(tag);
    PdaSeed::new(s)
}

fn listing_seed(n: u64) -> PdaSeed {
    let mut s = [0u8; 32];
    s[..14].copy_from_slice(b"haven/listing/");
    s[14..22].copy_from_slice(&n.to_le_bytes());
    PdaSeed::new(s)
}

fn record_seed(therapist: &Bytes32) -> PdaSeed {
    PdaSeed::new(sha256(&[b"haven/therapist/", therapist]))
}

fn ticket_seed(hash: &Bytes32) -> PdaSeed {
    PdaSeed::new(sha256(&[b"haven/ticket/", hash]))
}

fn review_seed(therapist: &Bytes32, n: u64) -> PdaSeed {
    PdaSeed::new(sha256(&[b"haven/review/", therapist, &n.to_le_bytes()]))
}

fn bytes(id: &AccountId) -> Bytes32 {
    *id.value()
}

/// `account` must be the PDA this program derives from `seed`, naming `owner`'s shard.
fn expect_pda(input: &PlanInput, account: &AccountMeta, seed: PdaSeed, owner: AccountId, what: &str) {
    let me = input.self_account_id;
    assert_eq!(account.account_id, AccountId::for_public_pda(&me, &seed), "Wrong {what} account");
    assert_eq!(account.program_account_id, owner, "The {what} account names the wrong shard");
}

fn check_profile(p: &Profile) {
    let name = p.name.trim();
    assert!(!name.is_empty(), "A listing needs a name");
    assert!(name.len() <= MAX_NAME, "The name is too long");
    assert!(p.bio.len() <= MAX_BIO, "The bio is too long");
    assert!(p.specialties.len() <= MAX_SPECIALTIES, "The specialties are too long");
    assert!(p.price > 0, "The session price must be positive");
    assert!(p.receive.len() >= 64 && p.receive.len() <= MAX_RECEIVE, "The receiving keys look wrong");
}

fn signer(account: &AccountMeta, who: &str) -> Bytes32 {
    assert!(account.is_authorized, "{who} must sign");
    bytes(&account.account_id)
}

fn main() {
    run_program(plan, apply)
}

// ── plan: check the accounts, emit effects ─────────────────────────────────

#[expect(clippy::needless_pass_by_value, reason = "run_program hands the instruction over by value")]
#[expect(clippy::too_many_lines, reason = "one arm per instruction")]
fn plan(input: &PlanInput, ins: Instruction) -> Plan {
    let me = input.self_account_id;
    let mut plan = Plan::new(input);
    let accounts = input.accounts.as_slice();
    match ins {
        Instruction::Init => {
            let [council, authority] = accounts else {
                panic!("Init takes two accounts: council, authority");
            };
            expect_pda(input, council, tag_seed(b"haven/council"), me, "council");
            let who = signer(authority, "The council");
            plan.effect(council, &Effect::InitCouncil(who));
            plan.effect(authority, &Effect::Keep);
        }
        Instruction::SetVerifier { verifier, name, chat_key, active } => {
            let [council, verifiers, who] = accounts else {
                panic!("SetVerifier takes three accounts: council, verifiers, signer");
            };
            expect_pda(input, council, tag_seed(b"haven/council"), me, "council");
            expect_pda(input, verifiers, tag_seed(b"haven/verifiers"), me, "verifiers");
            let who = signer(who, "The council");
            let name = name.trim().to_owned();
            assert!(!name.is_empty() && name.len() <= MAX_NAME, "A verifier needs a name of up to {MAX_NAME} bytes");
            plan.effect(council, &Effect::RequireCouncil(who));
            plan.effect(verifiers, &Effect::SetVerifier(Verifier { account: verifier, name, chat_key, active }));
            plan.effect(&accounts[2], &Effect::Keep);
        }
        Instruction::Register { index, profile, time } => {
            let [idx, listing, record, therapist] = accounts else {
                panic!("Register takes four accounts: index, listing, record, therapist");
            };
            let who = signer(therapist, "The therapist");
            expect_pda(input, idx, tag_seed(b"haven/index"), me, "index");
            expect_pda(input, listing, listing_seed(index), me, "listing");
            expect_pda(input, record, record_seed(&who), me, "record");
            check_profile(&profile);
            let rec = Therapist {
                account: who,
                profile,
                status: LISTED,
                reason: String::new(),
                credential: None,
                tickets: 0,
                rating_sum: 0,
                rating_count: 0,
                reports: 0,
                reviews: 0,
                registered: time,
                updated: time,
            };
            plan.effect(idx, &Effect::Bump { expected: index });
            plan.effect(listing, &Effect::WriteOnce(who.to_vec()));
            plan.effect(record, &Effect::WriteOnce(borsh::to_vec(&rec).expect("record encodes")));
            plan.effect(therapist, &Effect::Keep);
        }
        Instruction::Update { profile, time } => {
            let [record, therapist] = accounts else {
                panic!("Update takes two accounts: record, therapist");
            };
            let who = signer(therapist, "The therapist");
            expect_pda(input, record, record_seed(&who), me, "record");
            check_profile(&profile);
            plan.effect(record, &Effect::Update { profile, signer: who, time });
            plan.effect(therapist, &Effect::Keep);
        }
        Instruction::Verify { therapist, licence_hash, jurisdiction, expires, time } => {
            let [verifiers, record, verifier] = accounts else {
                panic!("Verify takes three accounts: verifiers, record, verifier");
            };
            let who = signer(verifier, "The verifier");
            expect_pda(input, verifiers, tag_seed(b"haven/verifiers"), me, "verifiers");
            expect_pda(input, record, record_seed(&therapist), me, "record");
            assert!(jurisdiction.len() <= MAX_JURISDICTION, "The jurisdiction is too long");
            plan.effect(verifiers, &Effect::RequireVerifier(who));
            plan.effect(record, &Effect::SetCredential(Credential { verifier: who, licence_hash, jurisdiction, expires, time }));
            plan.effect(verifier, &Effect::Keep);
        }
        Instruction::Revoke { therapist } => {
            let [verifiers, record, verifier] = accounts else {
                panic!("Revoke takes three accounts: verifiers, record, verifier");
            };
            let who = signer(verifier, "The verifier");
            expect_pda(input, verifiers, tag_seed(b"haven/verifiers"), me, "verifiers");
            expect_pda(input, record, record_seed(&therapist), me, "record");
            plan.effect(verifiers, &Effect::RequireVerifier(who));
            plan.effect(record, &Effect::ClearCredential { verifier: who });
            plan.effect(verifier, &Effect::Keep);
        }
        Instruction::Suspend { therapist, reason } => {
            let [council, record, who] = accounts else {
                panic!("Suspend takes three accounts: council, record, signer");
            };
            let who = signer(who, "The council");
            expect_pda(input, council, tag_seed(b"haven/council"), me, "council");
            expect_pda(input, record, record_seed(&therapist), me, "record");
            assert!(reason.len() <= MAX_REASON, "The reason is too long");
            plan.effect(council, &Effect::RequireCouncil(who));
            plan.effect(record, &Effect::SetStatus { status: SUSPENDED, reason });
            plan.effect(&accounts[2], &Effect::Keep);
        }
        Instruction::Reinstate { therapist } => {
            let [council, record, who] = accounts else {
                panic!("Reinstate takes three accounts: council, record, signer");
            };
            let who = signer(who, "The council");
            expect_pda(input, council, tag_seed(b"haven/council"), me, "council");
            expect_pda(input, record, record_seed(&therapist), me, "record");
            plan.effect(council, &Effect::RequireCouncil(who));
            plan.effect(record, &Effect::SetStatus { status: LISTED, reason: String::new() });
            plan.effect(&accounts[2], &Effect::Keep);
        }
        Instruction::IssueTicket { hash, fee, time } => {
            let [record, ticket, therapist, treasury] = accounts else {
                panic!("IssueTicket takes four accounts: record, ticket, therapist, treasury");
            };
            let who = signer(therapist, "The therapist");
            assert_eq!(therapist.program_account_id, NATIVE_TOKEN_PROGRAM_ID, "The therapist must name their LGO balance");
            expect_pda(input, record, record_seed(&who), me, "record");
            expect_pda(input, ticket, ticket_seed(&hash), me, "ticket");
            expect_pda(input, treasury, tag_seed(b"haven/treasury"), NATIVE_TOKEN_PROGRAM_ID, "treasury");
            assert!(fee >= MIN_TICKET_FEE, "The ticket fee is below the minimum");
            plan.effect(record, &Effect::Issue { signer: who, fee });
            plan.effect(ticket, &Effect::WriteOnce(borsh::to_vec(&Ticket { therapist: who, issued: time, used: false }).expect("ticket encodes")));
            plan.call(ChainedCall::new(
                NATIVE_TOKEN_PROGRAM_ID,
                vec![ProgramShardSelector::from(therapist), ProgramShardSelector::from(treasury)],
                &NativeInstruction::Transfer { amount: fee },
            ));
        }
        Instruction::Rate { therapist, secret, index, score, text, report, time } => {
            let [record, ticket, review] = accounts else {
                panic!("Rate takes three accounts: record, ticket, review");
            };
            let hash = sha256(&[&secret]);
            expect_pda(input, record, record_seed(&therapist), me, "record");
            expect_pda(input, ticket, ticket_seed(&hash), me, "ticket");
            expect_pda(input, review, review_seed(&therapist, index), me, "review");
            assert!((1..=5).contains(&score), "A score is 1 to 5");
            assert!(text.len() <= MAX_REVIEW, "The review is too long");
            plan.effect(ticket, &Effect::Consume { therapist });
            plan.effect(record, &Effect::AddReview { index, score, report, time });
            plan.effect(review, &Effect::WriteOnce(borsh::to_vec(&Review { ticket: hash, score, text, report, time }).expect("review encodes")));
        }
    }
    plan
}

// ── apply: the rules, against each account's stored state ──────────────────

fn therapist(pre: &[u8]) -> Therapist {
    assert!(!pre.is_empty(), "No such therapist");
    borsh::from_slice(pre).expect("therapist record decodes")
}

fn verifiers(pre: &[u8]) -> Vec<Verifier> {
    if pre.is_empty() {
        Vec::new()
    } else {
        borsh::from_slice(pre).expect("verifier list decodes")
    }
}

fn encode<T: BorshSerialize>(t: &T) -> Option<Vec<u8>> {
    Some(borsh::to_vec(t).expect("encodes"))
}

fn apply(effect: Effect, pre: &[u8]) -> Option<Vec<u8>> {
    match effect {
        Effect::Keep => None,
        Effect::InitCouncil(authority) => {
            assert!(pre.is_empty(), "This registry already has a council");
            Some(authority.to_vec())
        }
        Effect::RequireCouncil(who) => {
            assert_eq!(pre, &who[..], "Only the council can do that");
            None
        }
        Effect::SetVerifier(v) => {
            let mut list = verifiers(pre);
            if let Some(slot) = list.iter_mut().find(|e| e.account == v.account) {
                *slot = v;
            } else {
                list.push(v);
            }
            encode(&list)
        }
        Effect::RequireVerifier(who) => {
            assert!(verifiers(pre).iter().any(|v| v.active && v.account == who), "Only an active verifier can do that");
            None
        }
        Effect::Bump { expected } => {
            let count = match pre.len() {
                0 => 0,
                8 => u64::from_le_bytes(pre.try_into().expect("8 bytes")),
                n => panic!("Index holds {n} bytes, expected 8"),
            };
            assert_eq!(count, expected, "Listing {expected} was just taken — try again");
            Some((count + 1).to_le_bytes().to_vec())
        }
        Effect::WriteOnce(data) => {
            assert!(pre.is_empty(), "That slot is already written");
            Some(data)
        }
        Effect::Update { profile, signer, time } => {
            let mut t = therapist(pre);
            assert_eq!(t.account, signer, "Only the therapist can edit their listing");
            t.profile = profile;
            t.updated = time;
            encode(&t)
        }
        Effect::SetCredential(c) => {
            let mut t = therapist(pre);
            t.credential = Some(c);
            encode(&t)
        }
        Effect::ClearCredential { verifier } => {
            let mut t = therapist(pre);
            let c = t.credential.as_ref().expect("No credential to revoke");
            assert_eq!(c.verifier, verifier, "Only the verifier who issued a credential can revoke it");
            t.credential = None;
            encode(&t)
        }
        Effect::SetStatus { status, reason } => {
            let mut t = therapist(pre);
            t.status = status;
            t.reason = reason;
            if status == LISTED {
                t.reports = 0;
            }
            encode(&t)
        }
        Effect::Issue { signer, fee } => {
            let mut t = therapist(pre);
            assert_eq!(t.account, signer, "Only the therapist can issue their own tickets");
            assert_eq!(t.status, LISTED, "This therapist is not listed right now");
            let due = (t.profile.price * TICKET_FEE_BPS / 10_000).max(MIN_TICKET_FEE);
            assert!(fee >= due, "The ticket fee is below 2% of the listed price");
            t.tickets += 1;
            encode(&t)
        }
        Effect::Consume { therapist: who } => {
            assert!(!pre.is_empty(), "No such ticket");
            let mut k: Ticket = borsh::from_slice(pre).expect("ticket decodes");
            assert_eq!(k.therapist, who, "That ticket is for another therapist");
            assert!(!k.used, "That ticket was already used");
            k.used = true;
            encode(&k)
        }
        Effect::AddReview { index, score, report, time } => {
            let mut t = therapist(pre);
            assert_eq!(t.reviews, index, "Review slot {index} was just taken — try again");
            t.reviews += 1;
            t.rating_sum += u64::from(score);
            t.rating_count += 1;
            if report {
                t.reports += 1;
                if t.reports >= REPORTS_TO_FLAG && t.status == LISTED {
                    t.status = FLAGGED;
                    t.reason = format!("Hidden after {} reports from paying clients, pending the council", t.reports);
                }
            }
            t.updated = time;
            encode(&t)
        }
    }
}
