# Glaciem — Setup & Build Guide

Glaciem is a privacy cryptocurrency (ticker **GLAC**) forked from Monero. Its
proof-of-work is **Lattice** — a CPU-only, latency-bound algorithm designed so
the mining contest lands on CPUs, where Apple Silicon's performance-per-watt
lead is largest.

This guide covers building the daemon and the miner apps, the changes made when
forking from Monero, and running a local testnet.

- Repo: `github.com/hughson/rime`, branch **`rime`**.
- Based on Monero v0.18.x. The `rime` branch has a squashed history (a clean
  root commit); the `upstream` remote still points at `monero-project/monero`
  for reference.

---

## 1. Build environment (daemon, on macOS / Apple Silicon)

- macOS on Apple Silicon (developed on an M4), Xcode Command Line Tools, Homebrew.

```sh
brew install boost openssl@3 libsodium zeromq unbound miniupnpc expat \
  libunwind-headers protobuf pkg-config hidapi libusb ccache cmake
```

(`unbound` provides `libunbound`. Qt is not needed for the daemon or the
native miner apps.)

**CMake 4.x note:** modern CMake errors on submodules that declare
`cmake_minimum_required` below 3.5, so the `cmake` invocation passes
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

---

## 2. Building the daemon

```sh
cd ~/rime
mkdir -p build/release && cd build/release
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
      ../..
make -j8
```

Binaries land in `build/release/bin/`: `rimed`, `rime-wallet-cli`,
`rime-wallet-rpc`, and the `rime-blockchain-*` tools.

---

## 3. The miner apps

Glaciem ships native CPU miners with an embedded wallet (generate address, sync,
balance, send, receive, sweep, history) for three platforms:

| Platform | Build | Notes |
|---|---|---|
| macOS | `pow/app/build.sh` | SwiftUI; installs to `/Applications` |
| Windows | `pow/app_win/build_win.sh` | cross-built from macOS via MinGW-w64; one self-contained `.exe` |
| Android | `cd android && ./gradlew assembleDebug` | Jetpack Compose; NDK builds the native libs |
| Linux | `pow/app_linux/build.sh` | Qt6/QML scaffold — work in progress |

The miner apps share two pieces of C/C++:

- `pow/lattice_ref.c` — the canonical Lattice reference implementation.
- `pow/wallet/rime_wallet.{h,cpp}` — a C ABI over Monero's `wallet_api`,
  giving the apps an embedded wallet.

Build flags for the Lattice hot path are `-O3 -funroll-loops` (plus
`-mavx2 -mbmi2` on x86). Measurement showed Lattice has essentially no further
SIMD headroom on current consumer CPUs — the `-O3` build is near-optimal.

---

## 4. The fork — changes from Monero

### 4.1 Network identity — `src/cryptonote_config.h`

Fresh `NETWORK_ID` UUIDs per network isolate Glaciem's P2P from Monero's. All Glaciem
addresses begin with **`R`**.

| | Mainnet | Testnet | Stagenet |
|---|---|---|---|
| Address prefix | 144 | 528 | 912 |
| Integrated-address prefix | 272 | 656 | 1040 |
| Subaddress prefix | 400 | 784 | 1168 |
| P2P / RPC / ZMQ ports | 19080-19082 | 29080-29082 | 39080-39082 |
| `GENESIS_NONCE` | 80000 | 80001 | 80002 |

### 4.2 Proof-of-work — Lattice

The PoW is **Lattice**, a CPU-only, integer-only, latency-bound walk over a
per-nonce scratchpad (`pow/POW_DESIGN.md` has the rationale). It lives in
`src/crypto/lattice.{c,h}` for daemon block hashing/verification, kept
algorithm-identical to the canonical `pow/lattice_ref.c`. Monero's RandomX
path and the abandoned v1 GPU phase were dropped.

> `src/crypto/lattice.c` and `pow/lattice_ref.c` MUST stay in sync — the daemon
> verifies what the miners produce.

Lattice is provisional: parameters (`SCRATCH_WORDS`, `L_WALK`) are set by
measurement, and the algorithm is **not yet cryptographically reviewed** — a
required step before mainnet.

### 4.3 Hard-fork tables — `src/hardforks/hardforks.cpp`

All three networks run:

```c
{ 1,  1, 0, 1341378000 },   // genesis-era
{ 18, 2, 0, 1341378001 },   // Lattice consensus, from block 2
```

so the modern Monero ruleset (RingCT, Bulletproofs+, view-tags) plus Lattice
PoW are active from block 2. RingCT / ring-signature crypto is inherited from
Monero unchanged.

### 4.4 Genesis blocks

Genesis was regenerated for all three networks with fresh `GENESIS_TX` blobs
and nonces (80000/80001/80002). Each genesis coinbase carries a `TX_EXTRA_NONCE`
string, e.g.:

```
Glaciem mainnet - crystallized on Apple Silicon - 2026-05-18
```

This gives each Glaciem network its own chain root, distinct from Monero's.

### 4.5 Checkpoints & seed nodes

Monero's hardcoded block-hash checkpoints, `moneropulse` DNS checkpoints, and
hardcoded seed-node IPs were removed (a Monero checkpoint would otherwise reject
Glaciem's genesis). Peers are added with `--add-peer` / `--seed-node`. A mainnet
`checkpoints.dat` must be generated before launch.

---

## 5. Running a local testnet

### 5.1 Start the daemon

```sh
cd ~/rime/build/release/bin
./rimed --testnet --offline --no-igd --data-dir /tmp/rime-tn \
  --fixed-difficulty 1 --non-interactive
```

`--fixed-difficulty 1` keeps local mining instant; `--offline` runs a private
single node. Testnet RPC is on port **29081**.

### 5.2 Create wallets

```sh
./rime-wallet-cli --testnet --offline \
  --generate-new-wallet /tmp/rime-w/walletA --password "" \
  --mnemonic-language English --command address
```

Glaciem testnet addresses begin with `R`.

> Two gotchas for fresh-chain test wallets:
> - A new wallet assumes Monero's old chain and sets a far-future restore
>   height. Fix: `set refresh-from-block-height 1`.
> - Empty-password wallets fail key-image computation on refresh. Fix:
>   `set ask-password 0`.
> Then run `rescan_bc`.

### 5.3 Mine

```sh
curl -s http://127.0.0.1:29081/start_mining -H 'Content-Type: application/json' \
  -d '{"miner_address":"<walletA address>","threads_count":4,
       "do_background_mining":false,"ignore_battery":true}'
# ... mined (coinbase) rewards need 60 confirmations to be spendable ...
curl -s http://127.0.0.1:29081/stop_mining
```

### 5.4 Send a transaction

```sh
./rime-wallet-cli --testnet --wallet-file /tmp/rime-w/walletA \
  --password "" --daemon-address 127.0.0.1:29081 --trusted-daemon \
  --command transfer <walletB address> 100
```

> On a young chain, mined (coinbase) outputs are often "unmixable" — too few
> same-denomination outputs exist to form a ring, so a send fails with "not
> enough outputs for ring size". Sweep them first (`sweep_unmixable` in the CLI,
> or the **Sweep Unmixable** button in the miner apps).

---

## 6. Economic parameters

Block time, emission curve, tail emission, dynamic block size, and decimal
precision are currently inherited from Monero unchanged:

| Parameter | Value |
|---|---|
| Target block time | 120 s |
| Emission | Monero smooth-emission curve, `EMISSION_SPEED_FACTOR 20` |
| Tail emission | `FINAL_SUBSIDY_PER_MINUTE` 0.3 GLAC/min |
| Coinbase maturity | 60 blocks |
| Decimal places | 12 |

These are due for a deliberate review before mainnet.

---

## 7. Known issues

- `unit_tests` — `boosted_tcp_server.shutdown` (segfault) and
  `node_server.bind_same_p2p_port` (assertion) fail on macOS/arm64. These are
  test-harness issues inherited from upstream; `rimed` itself runs and shuts
  down cleanly.

---

## 8. Before mainnet

- Cryptographic review of Lattice.
- Economic-parameters review (section 6).
- Mainnet `checkpoints.dat` and a checkpoint cadence for the low-hashrate
  launch window.
- Trademark check on the name "Glaciem".
