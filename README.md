# Glaciem (GLAC)

A privacy-focused cryptocurrency forked from
[Monero](https://github.com/monero-project/monero), with an original
CPU-only proof-of-work (**Lattice**) and a Mac-first toolchain.

> **Status: mainnet launches 2026-05-20, 00:00 EDT** — a fair start: no
> premine, everyone mines from block 1. Native miner + wallet apps for macOS,
> Windows, and Android are on the
> [Releases page](https://github.com/hughson/Glaciem/releases).
>
> Glaciem is an experimental project, not an investment — GLAC has no exchange
> listing and no market price. Lattice has **not** had external cryptographic
> review; mine at your own risk. "Glaciem" / "GLAC" are working names, pending
> a trademark check.

## What Glaciem is

Glaciem is a fork of Monero. It inherits Monero's privacy model **unchanged** —
RingCT, ring signatures, Bulletproofs+, and stealth addresses, all mandatory
and exactly as upstream. Economic parameters (~2-minute blocks, tail emission,
dynamic block size, fair launch, no premine) are inherited from Monero and may
be revisited later.

What Glaciem adds is its own proof-of-work: **Lattice**.

## Proof-of-work: Lattice

Lattice is an original, integer-only, **CPU-only** proof-of-work. Each hash is
a long, serial, latency-bound walk over a large per-nonce scratchpad and a
shared epoch dataset — a branchy, memory-hard loop with no parallelism within a
nonce. That shape rewards wide, high-IPC CPU cores and collapses GPU occupancy
the way RandomX does; there is no GPU phase at all.

Verification is cheap and **CPU-only** — any full node, on any platform, with
no GPU, validates a block by running one walk and checking the result. Lattice
is integer-only and deterministic, a hard requirement for a consensus
algorithm.

Lattice is the live consensus proof-of-work at hard-fork **v18** — the daemon
mines and validates Lattice blocks on the Glaciem network.

> ⚠️ **Lattice is experimental** and has **not** had external cryptographic
> review. Its parameters are still provisional. Glaciem is a project, not an
> investment — mine at your own risk.

The design notes and the strategy behind it are in
[`POW_DESIGN.md`](POW_DESIGN.md); the canonical algorithm is
[`pow/lattice_ref.c`](pow/lattice_ref.c).

## Mining

Glaciem is developed Mac-first and ships native mining apps with a built-in
wallet — generate an address in-app and the blocks you mine pay straight to it
(no separate wallet daemon). Each app can generate a wallet, sync the chain,
show balance, and send / receive / sweep / view transaction history:

- **macOS** — a native SwiftUI app ([`pow/app/`](pow/app)).
- **Windows** — a self-contained `.exe`, cross-built from macOS ([`pow/app_win/`](pow/app_win)).
- **Android** — a Jetpack Compose app ([`android/`](android)).

All three run the same CPU-only Lattice miner. Download them from the
[Releases page](https://github.com/hughson/Glaciem/releases) or
[glaciem.frostmine.workers.dev](https://glaciem.frostmine.workers.dev).

## Forked from

| | |
|---|---|
| Upstream | `monero-project/monero` |
| Fork commit | `67c283c98` (`v0.18.0.0-1653-g67c283c98`) |
| Fork date | 2026-05-16 |

The upstream remote is kept as `upstream`, so Monero fixes can be merged in.
Monero's original README is preserved as [`README.Monero.md`](README.Monero.md).

## What changed from Monero

**Phase 1 — network-identity rebrand** (no cryptography or consensus logic
changed): new network IDs and base58 address prefixes (all Glaciem
addresses begin with `R`), new default ports, Monero's hardcoded checkpoints /
DNS checkpoints / seed nodes removed, fresh genesis blocks (no premine), and
renamed binaries (`rimed`, `rime-wallet-cli`, `rime-wallet-rpc`).

**Phase 2 — the Lattice proof-of-work**: an original CPU-only PoW, designed,
implemented (a C reference plus the daemon's consensus copy), and integrated
into the daemon at hard-fork v18 — live on the network.

Exact file/line changes for Phase 1 are in [`SETUP.md`](SETUP.md).

## Building

Glaciem is developed on **macOS (Apple Silicon, M-series)**.

Install dependencies with Homebrew:

```sh
brew install boost openssl@3 libsodium zeromq unbound miniupnpc expat \
  libunwind-headers protobuf pkg-config hidapi libusb cmake ccache
```

Then follow [`SETUP.md`](SETUP.md) for the configure + build steps. Built
binaries land in `build/release/bin/`.

## Running a local testnet

```sh
./build/release/bin/rimed --testnet
```

`SETUP.md` walks through spinning up a local testnet — mining the first blocks
and sending a test transaction between two wallets.

## Roadmap

- ☑ **Phase 1** — clean fork: builds, runs, mines, sends RingCT transactions,
  rebranded network identity.
- ☑ **Phase 2** — the Lattice PoW: an original CPU-only proof-of-work,
  designed, built, and integrated at HF v18, with native macOS, Windows, and
  Android miners.
- ☑ **Mainnet launch** — 2026-05-20, with bootstrap-window protections.
- ☐ **External cryptographic review of Lattice** — strongly wanted; a novel
  consensus algorithm deserves cryptographer review. Until then, mine at your
  own risk.
- ☐ **Masternodes** — a planned incentive layer for node operators: lock
  collateral and earn a share of the block reward, to reward and grow the
  full-node network beyond miners and volunteers.

## Known issues

The inherited Monero **unit-test suite** has ~34 failing tests (1227 of ~1261
pass). They fall into two groups, and neither is a daemon defect:

- **Fork-identity fixture mismatches (~27 tests)** — `uri.*`, `multisig.make_*`,
  `get_account_address_*`, `wallet_storage.*`, `Serialization.portability_wallet`.
  These upstream tests hardcode *Monero's* addresses and genesis block. Glaciem
  deliberately changed the address prefix (Glaciem addresses begin with `R`) and
  minted a fresh genesis, so the tests fail — the code is correct; the test
  fixtures still describe Monero. Re-pointing them at Glaciem's identity is
  tracked as post-fork cleanup.
- **macOS/arm64 networking/concurrency test-harness issues (~7 tests)** —
  `boosted_tcp_server.shutdown` and `cryptonote_protocol_handler.race_condition`
  hang; `node_server.bind_same_p2p_port`, `http_server.*`, and `notify.works`
  fail (the last needs a test helper binary). Inherited from upstream; the
  daemon itself binds, runs, and shuts down cleanly.

Address handling, wallet storage/restore, RPC, and mining are all exercised
directly by the running daemon and the miner apps, independent of these tests.

## License

Glaciem is a fork of Monero and is distributed under the same
**BSD 3-Clause License**. Copyright for the inherited code remains with
The Monero Project and the Cryptonote developers. See [`LICENSE`](LICENSE).

## Credit

Glaciem exists entirely because of the work of
[The Monero Project](https://github.com/monero-project/monero) and the wider
Monero community. All privacy and consensus technology in Glaciem is theirs.
