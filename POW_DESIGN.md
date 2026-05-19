# Lattice — Proof-of-Work Design

**Status: provisional parameters — testnet-only — NOT cryptographically reviewed.**

Lattice is Glaciem's custom proof-of-work. It is implemented, integrated into the
daemon at hard-fork **v18** (`HF_VERSION_LATTICE`), and running on the Glaciem
testnet. Its parameters are provisional. It has had **no external cryptographic
review** — that review is a hard prerequisite before any mainnet use, and
nothing here ships to mainnet without it.

This document describes what Lattice is. It also keeps, on the record, the
design dead-end that came before it: Lattice's predecessor was a two-phase
CPU+GPU algorithm built around a structural "moat" that was tested and
disproven (§7). That history is kept on purpose.

---

## 1. Overview

Lattice is a **single-phase, CPU-only, integer-only** proof-of-work. One
evaluation for a nonce is:

```
seed state from (header|nonce) → walk → final hash → compare to target
```

The **walk** is the whole algorithm: a long, serial, latency-bound, branchy
traversal of a large per-nonce scratchpad and a shared epoch dataset (§3).
There is no GPU phase and no second algorithm — one kind of work, one kind of
hardware.

The **sole cryptographic primitive** is `arx_perm()` — the BLAKE2b round
function, a well-analysed 64-bit ARX permutation over a 1024-bit state. The
sponge hash, the `expand` keystream, and the walk are all built on it. One
primitive to review.

The canonical definition is [`pow/lattice_ref.c`](pow/lattice_ref.c); the
daemon's consensus copy is `src/crypto/lattice.{c,h}`, bit-identical to it.

## 2. Parameters (provisional)

| Constant | Value | Meaning |
|---|---|---|
| `ARX_ROUNDS` | 8 | rounds inside `arx_perm` |
| `ST_WORDS` | 256 (2 KiB) | working state, per nonce |
| `SCRATCH_WORDS` | 1 MiB | per-nonce scratchpad — **tunable** |
| `DATASET_WORDS` | 4 MiB | global per-epoch dataset |
| `L_WALK` | 32768 | walk iterations — **tunable** |

All are consensus constants. `SCRATCH_WORDS` and `L_WALK` are the two tuning
knobs (§9): the scratchpad is sized to live in a large CPU cache while
collapsing GPU occupancy and thrashing small caches; `L_WALK` sets the cost of
one evaluation, tuned so a single CPU verification is a few milliseconds. The
values here are provisional — chosen for a working algorithm, not yet tuned by
measurement or reviewed.

## 3. The walk — the algorithm

Each nonce:

1. **Seed.** The `(header | nonce)` blob is sponge-absorbed into a 2 KiB
   working state.
2. **Scratchpad.** A 1 MiB per-nonce scratchpad is filled with an `arx_perm`
   keystream derived from the state.
3. **Walk.** `L_WALK` iterations of a sequential, data-dependent loop. Each
   iteration derives scratchpad / dataset / state block indices from an
   accumulator, mixes those blocks into the accumulator, applies `arx_perm`
   **once or twice** depending on a data-dependent bit (a branch → warp
   divergence on a GPU), and writes the result back into both the scratchpad
   and the working state.
4. **Finalize.** The state is squeezed to a 32-byte hash; compare to target.

Two properties carry the design:

- **Latency-bound serial dependency.** Each iteration depends on the previous
  one — there is no parallelism *within* a nonce. This rewards wide, high-IPC
  cores: a CPU strength.
- **A 1 MiB per-nonce working set.** A GPU mines by running thousands of
  threads in parallel; thousands × 1 MiB of live state vastly exceeds its
  on-chip memory, so occupancy collapses. This is the **RandomX-style
  mechanism** — the proven source of GPU-farm resistance.

This is the predecessor's CPU phase, promoted from one stage of a 16-stage
pipeline to the entire algorithm. The GPU phase was deleted (§7).

## 4. The epoch dataset

A 4 MiB global dataset is filled by an `arx_perm` keystream from an epoch seed.
In consensus the epoch seed is a past block hash (RandomX-style), so it is
stable across an epoch: a miner builds the dataset **once per epoch**, not per
nonce, and that cost amortises away. The walk reads it at data-dependent
indices.

## 5. Determinism (hard requirement)

Consensus requires **bit-identical** results on every node and platform.

- **Integer-only.** No floating point anywhere — FP results are not portable.
- **Defined overflow** — wrapping (modular) arithmetic only.
- Fixed walk length, state size, scratchpad and dataset sizes — all consensus
  constants.

Because Lattice is CPU-only and integer-only, there are no GPU kernels to keep
bit-identical and no cross-vendor floating-point behaviour to police — a real
simplification over the two-phase predecessor, which needed Metal and OpenCL
kernels cross-validated against the C reference.

## 6. Verification (cheap, CPU-only, universal)

A validating node — any platform, no GPU — verifies a block by running **one**
walk evaluation for the block's nonce, in integer math, and checking
`hash ≤ target`. The epoch dataset is built once per epoch and reused, so
per-block verification is a single walk: a few milliseconds (RandomX's
ballpark). The block carries only the nonce and standard header fields — no GPU
proof, no extra artefacts. Non-Mac full nodes validate Glaciem exactly as they
validate any chain.

## 7. The strategy: an economic gravity well, not a structural moat

Lattice's predecessor began with a stronger claim — that the algorithm would
make Apple Silicon *out-mine* other hardware, a structural **"moat."** That
claim is abandoned. This section records why a moat cannot work, and the
economic strategy that replaces it.

### 7.1 Why a structural moat is impossible

A consensus PoW is, by construction, a **portable deterministic function any
node can verify cheaply.** Those three properties are a cage:

- The verifier and the miner compute the *same function*. Whatever path is
  cheap for the verifier is available to the miner — you cannot make the
  miner's work exotic while the verifier's stays plain.
- You can bias the function toward a **resource** (memory capacity, latency,
  branch density) but never toward a **vendor**. For every resource Apple is
  good at, some non-Apple machine is better — memory channels → a multi-socket
  server, matrix throughput → a datacenter GPU, raw bandwidth → HBM.
- The **miner**, not the designer, chooses how to map the function onto
  silicon. A function cannot force a CPU↔GPU split or a cross-socket hop.

The predecessor's moat — a CPU↔GPU hand-off, free on Apple's unified memory, a
PCIe tax on a discrete GPU — was tested empirically (§7.3) and does not hold.
Not a tuning bug; a structural result.

### 7.2 What replaces it: the economic gravity well

Lattice's advantage for Apple Silicon is **economic, not structural — and in
crypto that is exactly the goal.** Miners deploy whatever hardware is most
profitable; profitability is revenue minus electricity, amortised over hardware
cost. Two economic facts favour Apple Silicon, and Lattice is shaped to lean on
them:

1. **Performance per watt.** Apple's wide, efficient CPU cores lead the
   consumer field on compute-per-watt. A **CPU-hard, latency-bound, branchy**
   algorithm (§3) puts the contest squarely on CPUs — the terrain where that
   lead is largest and least contested. Where electricity is normally priced,
   an Apple machine is simply the most profitable box to mine on. Lattice being
   *CPU-only* — no GPU phase at all — sharpens this: the GPU is the one place
   Apple's perf-per-watt lead narrows, and Lattice never goes there.
2. **Barrier to entry.** Millions of capable Macs already exist, owned and
   idle. A near-zero-friction native miner — generate a wallet, tap mine —
   puts the optimal hardware in users' hands on day one. This is the part of
   the advantage fully under Glaciem's control, and the part that cannot be
   competed away.

The outcome is organic: no consensus rule forces Macs to win; the market does,
because they are the most profitable box — most strongly where electricity is
ordinarily priced.

### 7.3 The honest record: the moat experiment

A profiling miner was run on a real discrete GPU — an NVIDIA Tesla T4 — with
OpenCL event profiling measuring time in CPU↔GPU transfers versus GPU compute:

- a ~128 KB per-stage hand-off: transfer measured **0.0%** of mining time.
- widening the hand-off **32×** to ~4 MiB: transfer still measured **0.2%**.

The ratio did not improve and *cannot*: transfer time and compute time both
scale linearly with state size, so the ratio is size-independent — and compute
dominates. A hand-off can never be the bottleneck. Worse, a hand-off heavy
enough to matter would also make the §6 CPU-only verification expensive. The
two-phase architecture, the Metal and OpenCL kernels, and the profiling miner
were all retired once this was settled; Lattice is what remained.

### 7.4 Limits of the economic strategy (honest)

- The well is **strong where electricity is expensive, weak where it is
  cheap** — a miner on subsidised power optimises hardware cost and absolute
  hashrate, not perf-per-watt. It is a regional tilt, not universal.
- A perf-per-watt edge **arbitrages into farms** of the favoured device. That
  still makes Glaciem an Apple-hardware-mined coin; it does not by itself
  preserve hobbyist mining.
- The durable, un-competable piece is the **low barrier to entry** — wide
  pre-existing ownership of capable hardware.

## 8. Honest threat model

| Threat | Reality |
|---|---|
| GPU mining farm | Resisted — the 1 MiB per-nonce scratchpad collapses GPU occupancy (RandomX-style), and Lattice is CPU-only, so a GPU has no native path. GPU-*hard*, like RandomX — not GPU-*proof*. |
| Large multi-core CPU server (EPYC) | **Not specifically defended.** Lattice is CPU-hard; a strong many-core CPU mines it well. Same property as RandomX. The economic tilt (§7.2), not the algorithm, is what favours Apple. |
| Rented cloud hashrate | Cloud GPU instances are GPU-heavy and CPU-light; a CPU-bound PoW mines poorly on them. A mild, real, incidental effect — not a designed moat. |
| Novel-algorithm risk | **The real risk.** Lattice is unreviewed. A novel PoW can hide a shortcut (mine far faster than intended) or an ASIC path. Only external review reduces this. |
| Bootstrap-window 51% | Mitigated operationally — developer checkpoints during the low-hashrate window, retired once honest hashrate is healthy; longer early coinbase maturity. |

## 9. Implementation status

- [`pow/lattice_ref.c`](pow/lattice_ref.c) — canonical C reference + self-tests
  (`--vectors`, `--bench`).
- `src/crypto/lattice.{c,h}` — the daemon's consensus copy; bit-identical to
  the reference on all test vectors.
- [`pow/test_vectors.txt`](pow/test_vectors.txt) — frozen input→hash vectors.
- Miners: [`pow/app/`](pow/app) (native macOS app) and [`android/`](android)
  (native Android app), both with a built-in wallet.
- Integrated into the daemon at `HF_VERSION_LATTICE` (v18). On a fresh chain
  Lattice runs from block 2; the testnet mines and validates it.

## 10. Open risks

- **Not cryptographically reviewed.** This is the dominant risk and the hard
  gate before mainnet.
- `SCRATCH_WORDS` and `L_WALK` are provisional — they need a measurement-driven
  sweep on real hardware.
- A novel PoW may contain an unknown optimisation or ASIC path (§8).
- The Apple-Silicon advantage is **economic, not structural** (§7) — it is a
  market tilt, and the algorithm alone does not guarantee it.

## 11. Roadmap

1. ☑ CPU reference, test vectors, self-tests.
2. ☑ Integrated into the daemon at `HF_VERSION_LATTICE` (v18); testnet mines
   and validates.
3. ☑ Native miners — macOS app and Android app, with a built-in wallet.
4. ☑ The two-phase CPU+GPU predecessor and its unified-memory moat were
   empirically disproven and retired (§7).
5. ☐ Tune `SCRATCH_WORDS` and `L_WALK` by measurement.
6. ☐ **External cryptographic review** — the hard gate.
7. ☐ Schedule the mainnet hard fork (only after review).
