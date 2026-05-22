# Changelog

All notable changes to Glaciem are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

GUI miner apps and the `rimed` daemon ship from the same repo under the same
version tag. Source builders pull the tag and run the relevant build script
under [`pow/app/`](pow/app), [`pow/app_win/`](pow/app_win),
[`pow/app_linux/`](pow/app_linux), or [`android/`](android); the binary's
embedded version string is read from the repo-root [`VERSION`](VERSION) file
at build time.

## [Unreleased]

## [1.1.7] – 2026-05-22

Windows joins the pool-mode lineup (Mac + Android shipped in 1.1.6).
Linux still pending — v1.1.8.

### Added
- Windows miner: SOLO / POOL toggle in a new Pool dialog. Pool URL
  field defaults to `https://glaciem-pool.frostmine.workers.dev` but
  is editable for any compatible pool.
- Persisted to `rime_pool.txt` next to the .exe (empty file = pool off,
  single URL line = pool on).
- New `pool_get_job()` / `pool_submit_share()` HTTP helpers + a
  `parse_pool_url()` that splits `g_pool_url` into host / port / ssl
  for WinHTTP. Reuses the existing `http_post_ep` for the actual
  TLS + POST work.
- "POOL" / "POOL ON" header button (left of HOST) so the current
  mode is visible at a glance.

### Changed
- Mining loop branches on pool mode:
  - Pool mode: fetch via `/pool/job`, hash against `share_difficulty`,
    on found-share re-hash against `network_difficulty` to flag
    `full_block: true`, submit via `/pool/submit`.
  - Solo mode: unchanged (`get_block_template` / `submit_block` direct
    to daemon).

### Notes
- Same Lattice PoW + dataset code as the rest of the family; no new
  crypto. Settings take effect at the next batch (~1 second) — no
  Windows app restart required when toggling pool mode.
- Linux pool support deferred to v1.1.8 (needs the same edits in
  `pow/app_linux/miner_engine.cpp` + a Qt dialog).

## [1.1.6] – 2026-05-22

Adds a **POOL** mining mode to the Mac and Android apps alongside
the existing SOLO behavior. Lets miners connect to the official pool
at **glaciem-pool.frostmine.workers.dev** — or any compatible pool —
and get proportional payouts based on share contribution.

Pool infrastructure ships as a separate piece (`pool/` directory in
the repo). Mac + Android are the first two clients; Windows + Linux
follow in v1.1.7. Public website only advertises the **Android**
v1.1.6 download for now; Mac users with the pool mode pull from
GitHub Releases directly until the Mac build is verified in the wild.

### Added
- New SOLO/POOL toggle + pool URL field in the Settings sheet
  (`RimeMiner.swift`). Persisted to UserDefaults as `poolEnabled`
  and `poolURL`. Defaults to SOLO (existing behavior unchanged) and
  the official pool URL.
- `miner_set_pool_config(int enabled, const char *url)` C bridge
  (`miner_core.h` + `miner_core.m`) so the Swift UI can flip the
  mining mode at runtime.
- Pool-aware mining loop (`miner_core.m::worker()`). When pool mode
  is on, the loop:
  - Fetches each template via `POST {pool_url}/pool/job` with the
    miner's payout wallet, instead of `get_block_template` on the
    daemon.
  - Uses the pool's `share_difficulty` for the inner meets-target
    check, so shares come ~1000× more frequently than full blocks.
  - Reports every found share via `POST {pool_url}/pool/submit`,
    setting `full_block: true` when the share's hash also meets the
    network difficulty (the pool then forwards to the daemon as
    submitblock).
- Pool jobs refresh every 2.5s in pool mode (vs 10s in solo mode);
  pool's upstream is cached at the proxy so frequent fetches are cheap.

### Notes
- v1 trust-mode pool: shares are NOT re-verified server-side yet.
  Acceptable for the small early network; a hostile miner could
  falsely claim shares. Server-side Lattice verification is v2.
- Pool fee is 0% at launch. Auto-payout threshold is **10 GLAC**.
  See <https://glaciem-pool.frostmine.workers.dev/> for live stats.
- Android port mirrors the Mac change: `MinerEngine.kt` adds
  `poolEnabled` / `poolUrl` + `setPoolConfig()`, the mining loop
  branches on pool mode, and the Settings dialog gains a POOL MODE
  switch + URL field (persisted in SharedPreferences as
  `poolEnabled` / `poolUrl`). `RpcClient.kt` gains `poolGetJob` and
  `poolSubmit` HTTP helpers.
- Windows / Linux stay at v1.1.4 on the website; same pool-mode
  patch coming as a follow-up.

## [1.1.5] – 2026-05-22

Android-only. Fixes a long-standing UX bug where the START / STOP mining
button felt unresponsive — taps registered immediately but the UI didn't
repaint for 1–5 seconds, leading users to retry and accidentally
re-toggle.

### Fixed
- Android miner: `MinerEngine.start()` and `MinerEngine.stop()` now call
  `publish()` immediately so the START button repaints on the same frame
  as the tap. Previously the first `publish()` from start only fired
  after the initial template fetch / dataset build (1–5s), and `stop()`
  only published after waiting up to 8s for the current hash batch to
  finish.

### Notes
- Only the Android APK is rebuilt at 1.1.5. Mac, Windows, and Linux are
  unaffected (different UI paths) and stay at 1.1.4.

## [1.1.4] – 2026-05-22

Public-RPC bandwidth diet rolled out to Windows, Linux, and Android.
Same change that shipped for Mac in 1.1.3.

### Changed
- Miner apps: wallet refresh poll bumped **4s → 20s** on Windows
  (`pow/app_win/rime_miner_win.c`), Linux (`pow/app_linux/miner_engine.cpp`),
  and Android (`android/app/src/main/java/com/hughson/rime/MinerEngine.kt`).
  Cuts `/getblocks.bin` traffic to the public RPC proxy ~5×.
- Miner apps: `try_discover_peers()` / `tryDiscoverPeers()` call removed on
  all four platforms. The public proxy now 403s `/get_peer_list`, so this
  call was wasted bandwidth.

### Notes
- All four platform binaries (Mac/Windows/Android/Linux) ship under this tag.
- Mac binary at 1.1.4 is byte-identical in behavior to 1.1.3; the rebuild is
  for version-string parity only.
- The mining template-reuse change from 1.1.3 (1.5s → 10s) is Mac-only; the
  other platforms do one mining batch per template by design and a
  structurally similar reuse loop is deferred to a follow-up release.

## [1.1.3] – 2026-05-22

Mac-only. Cuts the macOS app's daemon-proxy bandwidth ~5–7× by relaxing two
over-aggressive polling intervals and removing a wasted admin-endpoint call.

### Changed
- Mac miner: wallet refresh poll **4s → 20s** (`pow/app/miner_core.m`).
- Mac miner: mining template lifetime **1.5s → 10s** per template — the
  outer loop now reuses each `get_block_template` response for up to 10s
  before requesting a new one.
- Mac miner: `try_discover_peers()` call removed for the same reason as
  in 1.1.4.

### Notes
- Block time is ~120s, so 20s wallet refresh / 10s template reuse stays
  well inside the freshness envelope.
- Trade-off on the template reuse: ~8% wasted compute on stale templates
  when a new block lands mid-window (vs ~1.25% at 1.5s); acceptable
  while the network is small.
- All wallets, balances, addresses, and mining state carry forward — no
  migration.

## [1.1.2] – 2026-05-22

### Fixed
- `rimed` daemon built from source: emptied `src/blocks/checkpoints.dat`
  so the embedded Monero hash-of-hashes blob no longer rejects every
  Glaciem block ([`9897e1e9`](https://github.com/hughson/Glaciem/commit/9897e1e9)).
  Symptom before the fix: `prevalidate_block_hashes()` threw "usable is
  negative" at `blockchain.cpp:4891` on every incoming block, peers were
  banned, and the daemon got stuck at low height with 0 peers.

### Notes
- GUI miner apps were not linked against the affected library so they
  kept working, but they're rebuilt at 1.1.2 for parity.
- Source builders running `rimed` who hit the "can't keep peers" symptom
  should re-pull, rebuild, and wipe `~/.glaciem` before restarting.

## [1.1.1] – 2026-05-21

### Changed
- Settings copy fix on Mac / Windows / Android / Linux: the HOST/Settings
  field description now accurately says HOST controls the wallet's
  daemon, not the miner's (the miner has used an automatic multi-node
  fallback since 1.1.0).

### Notes
- Patch release; no functional changes beyond the text + version stamp.

## [1.1.0] – 2026-05-21

### Added
- **Linux GUI miner+wallet app** (Qt6/QML), shipped as a single
  self-contained `.AppImage` for x86_64. Feature parity with Mac /
  Windows / Android.
- [`BUILD_LINUX.md`](BUILD_LINUX.md) for source-build instructions on
  non-x86_64 Linux distros.
- **Multi-node fallback** (Phases 1–3):
  - Phase 1 — miner JSON-RPC tries the Cloudflare Worker, then direct
    node hostnames in order, falling through on transport error or 5xx.
  - Phase 2 — embedded wallet swaps daemons via the new
    `rime_wallet_set_daemon` C ABI after 3 consecutive disconnects;
    keys, balance, and scanned height are preserved across the swap.
  - Phase 3 — runtime peer discovery via `/get_peer_list`, with the
    cache persisted across app launches in each platform's native
    key/value store.
- Android: MINING INTENSITY picker in Settings (Eco / Balanced / Max).
- Whitepaper v1.0 (13-page PDF) covering design, monetary policy,
  threat model, and explicit limitations.

### Changed
- Wallet RPC routes through the Cloudflare Worker on all four apps now
  (previously the wallet went direct-to-VM and only the miner used the
  Worker — that was the original VM-scrub scenario that motivated the
  Worker in the first place).
- Android: per-thread `CHUNK` bumped **2 → 64**, closing the
  barrier-overhead gap that left phones idling at 55–65% CPU.
- `POW_DESIGN.md` rewritten to reflect mainnet launch (had been framed
  as testnet-only).
- Default branch renamed `rime` → `main`.

### Fixed
- Android: dark-mode text-field rendering in the Restore-seed and Send
  dialogs.
- Android: `onCreate` `lateinit` crash when the new intensity preference
  was touched before `engine` was constructed.

### Notes
- A Cloudflare Worker outage no longer takes mining or wallets offline
  thanks to the peer-cache fallthrough.

## [1.0.0] – 2026-05-19

### Added
- Mainnet launch of **Glaciem (GLAC)**.
- Native miner+wallet apps for **macOS** (SwiftUI), **Windows** (Win32 C),
  and **Android** (Jetpack Compose).
- Each app bundles the **Lattice** CPU proof-of-work + an embedded
  wallet (generate/restore/send/receive/sweep/history).
- Mainnet genesis: **2026-05-20 00:00 EDT**. Blocks dated earlier are
  rejected by consensus.

### Notes
- Lattice is an original proof-of-work and has not had external
  cryptographic review. Mine at your own risk.

[Unreleased]: https://github.com/hughson/Glaciem/compare/v1.1.7...HEAD
[1.1.7]: https://github.com/hughson/Glaciem/releases/tag/v1.1.7
[1.1.6]: https://github.com/hughson/Glaciem/releases/tag/v1.1.6
[1.1.5]: https://github.com/hughson/Glaciem/releases/tag/v1.1.5
[1.1.4]: https://github.com/hughson/Glaciem/releases/tag/v1.1.4
[1.1.3]: https://github.com/hughson/Glaciem/releases/tag/v1.1.3
[1.1.2]: https://github.com/hughson/Glaciem/releases/tag/v1.1.2
[1.1.1]: https://github.com/hughson/Glaciem/releases/tag/v1.1.1
[1.1.0]: https://github.com/hughson/Glaciem/releases/tag/v1.1.0
[1.0.0]: https://github.com/hughson/Glaciem/releases/tag/v1.0.0
