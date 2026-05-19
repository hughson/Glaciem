# Glaciem (GLAC)

A privacy-focused cryptocurrency forked from
[Monero](https://github.com/monero-project/monero), with an original
CPU-only proof-of-work (**Lattice**) and a Mac-first toolchain.

> **Status: pre-mainnet — testnet only.**
> "Glaciem" / "GLAC" are working names, pending a final trademark check.
> There is no mainnet, no public network, and no released software. Nothing
> here can hold real funds — please don't try.

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

On the testnet, Lattice is live: it is the consensus proof-of-work at
hard-fork **v18**, and the daemon mines and validates Lattice blocks today.

> ⚠️ **Lattice is experimental.** Its parameters are provisional and it has
> **not** had external cryptographic review. That review is a hard prerequisite
> before any mainnet launch — until then Lattice is strictly testnet-only.

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

All three run the same CPU-only Lattice miner.

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
into the daemon at hard-fork v18 — which the testnet runs today.

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

- ☑ **Phase 1** — clean fork: builds, runs a testnet, mines, sends RingCT
  transactions, rebranded network identity.
- ◑ **Phase 2** — the Lattice PoW: an original CPU-only proof-of-work,
  designed, built, integrated at HF v18, running on the testnet, with native
  macOS and Android miners. *In progress — parameters are still being tuned.*
- ☐ **External cryptographic review of Lattice** — the hard gate. A novel
  consensus algorithm must be reviewed by cryptographers before real value
  depends on it.
- ☐ **Mainnet launch** — only after review, with bootstrap-window protections.
- ☐ **Masternodes** — a planned incentive layer for node operators: lock
  collateral and earn a share of the block reward, to reward and grow the
  full-node network beyond miners and volunteers.

## Known issues

- Two Monero networking **unit tests** (`boosted_tcp_server.shutdown` and
  `node_server.bind_same_p2p_port`) fail on macOS/arm64. These are
  test-harness issues — the daemon itself runs and shuts down cleanly. They are
  inherited from upstream and tracked for a later fix.

## License

Glaciem is a fork of Monero and is distributed under the same
**BSD 3-Clause License**. Copyright for the inherited code remains with
The Monero Project and the Cryptonote developers. See [`LICENSE`](LICENSE).

## Credit

Glaciem exists entirely because of the work of
[The Monero Project](https://github.com/monero-project/monero) and the wider
Monero community. All privacy and consensus technology in Glaciem is theirs.
