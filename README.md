# Part 18 — Ghostmind (anonymous therapy)

Find a therapist, talk to them without telling them who you are, pay them
without anyone seeing it, and rate them afterwards. All of this runs inside
Basecamp:

| Need | Built on |
|---|---|
| Private conversations | **Logos messaging** (delivery_module). Messages are end-to-end encrypted, and the client is only a one-time key. |
| Knowing a therapist is real | A **registry program on the Logos Execution Zone (LEZ)**. Accredited verifiers attest licences on it, and a council appoints the verifiers. |
| Keeping therapists good | **Ticketed reputation** on the same program. Only paying clients can rate. Three reports hide a listing, and the council can suspend one. |
| Paying without anyone knowing | **Private → private LGO transfers** on LEZ. The chain shows no sender, no recipient and no amount. |

```
part18-ghostmind/
├── ghostmind-program/  the registry (Rust → risc0 guest), build.sh, ghostmind.bin
├── ghostmind-core/     ghostmind_core — universal C++ module: LEZ wallet FFI + delivery_module + libsodium
│   ├── src/ghostmind_codec.h   hand-written Borsh, PDA seeds, SHA-256, LGO amounts
│   ├── src/ghostmind_chat.h    sealed-box hellos + X25519 conversation keys
│   ├── src/ghostmind_impl.*    wallet, registry reads, jobs, conversations
│   └── tests/              offline checks + live harness (in-process message bus)
├── ghostmind-ui/       ghostmind — QML (Logos.Theme / Logos.Controls)
└── install.sh      drop both into a local Basecamp
```

## Live deployment

| | |
|---|---|
| Program | `HyFn5FKaUL4NMZqJ5KC1LB2JXWN6UiagQsNaXTTqVPXW` |
| Council | `EZm9kgmnHXZdRNWyLueNFaX31dvqTnDG69uznCAZxaMM` (the deployer wallet) |
| Treasury (ticket fees) | `2JGmso58h2W2irC4AL1EZnKmcBn9GWQZnBs7uxByGmcs` |
| Image ID | `e76b88b887dbcc4cc8f1c28f1afba5c6db6b81aa90b0b266c11e442f28cd9604` |
| Deployed | 2026-10-29, LEZ v0.3.1: 4 segments + header, 17 min |

The testnet was producing about one block a minute that day. The first deploy
attempt failed with "segment N transaction did not finalize" because the
wallet's default 30-block poll window ran out. The deployer's config was then
raised to `seq_tx_poll_max_blocks: 120`, and the retry landed.

## Roles

| Who | What they do in the app |
|---|---|
| **Client** | Browses *Find help*, opens a conversation, pays privately, rates or reports. Needs nothing set up. |
| **Therapist** | Lists themselves under *Therapists* with a name, bio, specialties and price. Asks a verifier for verification, answers clients, and issues review tickets automatically. |
| **Verifier** | A licensing board or professional association. Receives encrypted licence details, checks them off-chain, and attests on-chain. |
| **Council** | The registry's governor. It appoints and disables verifiers and suspends or reinstates listings. The first wallet to claim a fresh deployment becomes the council. |

## Why verification works this way

A whitelist the council edits directly would make the council judge every
licence in every country. Self-attestation proves nothing. So there are two
tiers:

1. **The council appoints verifiers.** These are institutions that can
   actually check a licence with the issuing body. They are stored on-chain
   with a name and a chat key.
2. **A verifier attests a therapist.** The record gets
   `sha256(LICENCE|JURISDICTION)`, the jurisdiction, an expiry date and the
   verifier's address. The licence number itself never goes on-chain. A
   verifier can only revoke its own attestation.

The licence details travel to the verifier in an encrypted Logos
conversation, just like a therapy session. Clients see "Verified · attested
by *Dutch Psychologists' Register* · NL · valid to 2028-01-01". They also see
"EXPIRED" once that date passes.

## Why reputation can't be gamed cheaply

Open ratings get flooded. Ratings tied to a payment would reveal who paid.
Ghostmind uses a **review ticket** instead:

1. The client pays privately. The client's module then makes a random
   `secret` and sends `sha256(secret)` to the therapist inside the encrypted
   conversation.
2. The therapist's module watches its private balance. When the balance rises
   by at least the price, it calls `IssueTicket(hash)` and pays a **fee of 2%
   of the listed price (minimum 0.01 LGO)** into the registry treasury. This
   happens automatically before the session, so a therapist can't hold back a
   ticket from a client who seems unhappy.
3. The client calls `Rate(secret, score, text, report)` from a **brand-new
   account** funded out of the private balance. The program checks
   `sha256(secret)` against an unused ticket for that therapist, marks it
   used, and adds the score.

Here is what that buys:

- **No fake reviews from outsiders.** Rating needs a ticket, and only the
  therapist can issue one.
- **Self-praise costs money.** A therapist who issues tickets to themselves
  pays the fee every time, and the ticket count shows how many sessions back
  the rating.
- **No ticket withholding.** The client's module expects the ticket, and the
  UI shows "Paid, waiting for ticket" until it arrives.
- **Nothing is linkable.** The payment is private, and the rating comes from
  a fresh account.
- **Blacklisting.** Three reports from ticket holders flip a listing to
  *Flagged*, which hides it and stops new tickets. The council can *Suspend*
  any listing with a reason, or *Reinstate* it, which clears the report count.

## What the network sees

| Step | Visible to observers | Hidden |
|---|---|---|
| Opening a conversation | A sealed message lands on the therapist's inbox topic. | Who sent it. A sealed box carries no sender, and the key is single-use. |
| Talking | Ciphertexts on `/ghostmind-conv/1/<random id>/proto`. | Contents, and who is behind the id. |
| Shielding (only when the private balance is short) | A public→private transfer from your public account. | What it's for. |
| Paying | A private transaction (a proof plus commitments). | Sender, recipient and amount. |
| Issuing a ticket | The therapist paid a fee for a ticket hash. | Which client the ticket is for. |
| Rating | A fresh account submits a score and text for a therapist. | Who the client is. |

**Limits worth stating plainly**

- **Delivery peers see IP addresses.** Messaging hides content, not network
  location. Use Tor or a VPN if that matters to you.
- **Timing can correlate.** A shield, then a private payment, then a ticket
  issued minutes later can be matched by someone watching closely.
  Shielding ahead of time (Wallet tab) breaks that chain.
- **Conversations are as private as the endpoints.** They are stored
  unencrypted in Basecamp's `module_data/ghostmind_core`, with mode 0600. Anyone
  with that disk can read them.
- **The therapist gets your money's worth on trust.** Refunds and disputes
  are out of scope. Reports and the council are the recourse.

## Build, deploy, install

```bash
cd ghostmind-program && ./build.sh                 # local risc0 toolchain; → ghostmind.bin
cd ../ghostmind-core
tests/run.sh                                   # offline: Borsh, seeds, SHA-256, amounts, chat crypto
GHOSTMIND_DATA_DIR=~/ghostmind-deployer tests/run.sh deploy ../ghostmind-program/ghostmind.bin   # once per testnet reset
GHOSTMIND_DATA_DIR=~/ghostmind-deployer tests/run.sh init                                # claim the council
#   → paste the program address into kDefaultProgram (src/ghostmind_impl.cpp)
nix build 'path:.#lgx-portable' -o result-portable
cd ../ghostmind-ui && nix build 'path:.#lgx-portable' -o result-portable --override-input ghostmind_core path:../ghostmind-core
cd .. && ./install.sh
```

`delivery_module` must be installed in Basecamp. Basecamp 0.3.2 doesn't bundle
it, so install it as described in the part 10 README.

Run the whole flow from the CLI with three wallets. The verifier wallet also
acts as the council here. The therapist and client talk over an in-process
bus that stands in for delivery. Payments and the registry run on the real
testnet.

```bash
GHOSTMIND_DATA_DIR=~/ghostmind-deployer tests/run.sh use <PROGRAM>   # if kDefaultProgram is empty
tests/run.sh session ~/ghostmind-therapist ~/ghostmind-client ~/ghostmind-deployer
```

To try two Basecamps on one machine, start the second with `GHOSTMIND_TCPPORT=60001`
so the two delivery nodes don't collide on a port, then dial each other over
loopback.

## On-chain layout

All accounts are public PDAs of the program.

```
council     "haven/council"                     32-byte council account
verifiers   "haven/verifiers"                   Vec<Verifier { account, name, chat_key, active }>
index       "haven/index"                       u64 number of listings
listing n   "haven/listing/" ‖ n_le             therapist account (write-once)
record T    sha256("haven/therapist/" ‖ T)      Therapist { profile, status, credential, tickets, ratings, reports, … }
ticket h    sha256("haven/ticket/" ‖ h)         Ticket { therapist, issued, used }
review T,n  sha256("haven/review/" ‖ T ‖ n_le)  Review { ticket, score, text, report, time }
treasury    "haven/treasury"  (native LGO)      ticket fees
```

A plan can see signatures but not stored state. So "is this signer the
council or an active verifier" is an effect on the council or verifiers
account that leaves it unchanged, and panics in `apply` if the signer isn't
there. Races on `index` and `reviews` use the guestbook pattern (part 17): an
expected-value check, then a client retry.

The listing carries the therapist's **receiving keys**: the nullifier public
key, the ML-KEM viewing key and the identifier, 1,248 bytes in all. Clients
pay those keys with `wallet_ffi_transfer_private` and never need a public
address.
