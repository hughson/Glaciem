#!/usr/bin/env python3
"""
Glaciem pool -- v1 trust-mode HTTP-API pool server.

Deployed at glaciem-node Hetzner VM, port 8088. Fronted by Cloudflare
(pool.frostmine.workers.dev). Talks to the local rimed daemon via its
restricted RPC on 127.0.0.1:19099.

v1 LIMITATIONS (documented; address before broader rollout):

  - TRUST MODE: shares are NOT verified server-side. The Lattice PoW
    would need to be linked into the pool via a native module or a
    shell-out to a verifier binary. Until then, a hostile miner could
    submit fake shares and steal credit. Acceptable for the small
    friend-group network at launch; not acceptable once miner count and
    payouts scale.

  - MANUAL PAYOUTS: this server tracks credits in a JSON file. The
    operator runs `pool-payout.py` from a separate secure machine to
    actually send GLAC to miners. Wallet keys never live on this VM.

Protocol (all JSON):

  POST /pool/job
    body  { wallet }
    200   { job_id, blockhashing_blob, blocktemplate_blob,
            seed_hash, height, network_difficulty,
            share_difficulty, target }

  POST /pool/submit
    body  { job_id, wallet, nonce, result?, full_block? }
    200   { accepted, block?, reason? }

  GET   /pool/stats              pool-wide JSON
  GET   /pool/miner/<wallet>     per-wallet stats
  GET   /pool/blocks?limit=N     recent blocks the pool found
"""

import collections
import ctypes
import http.server
import json
import os
import os.path
import re
import secrets
import socketserver
import struct
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone


# ---- config ---------------------------------------------------------------

POOL_PORT             = int(os.environ.get("POOL_PORT", "8088"))
DAEMON_RPC            = os.environ.get("DAEMON_RPC", "http://127.0.0.1:19099/json_rpc")
POOL_WALLET           = os.environ.get("POOL_WALLET",
                                       "POOL_WALLET_NOT_CONFIGURED_set_POOL_WALLET_env_var")
POOL_FEE_PERCENT      = float(os.environ.get("POOL_FEE_PERCENT", "0"))
SHARE_DIFF_DIVISOR    = int(os.environ.get("SHARE_DIFF_DIVISOR", "1000"))
TEMPLATE_REFRESH_S    = int(os.environ.get("TEMPLATE_REFRESH_S", "10"))
# v1.1.16+: how long the pool is allowed to lag the chain tip before we
# decide the daemon link is broken and exit() so systemd restarts us.
# Failure modes this catches:
#   - daemon crashed / unreachable: refresh_job() keeps failing, height stays
#     pinned; watchdog fires after JOB_WATCHDOG_S of no advance past chain tip.
#   - daemon up but returning stale template (e.g. RandomX verifier state
#     corruption from a rejected submitblock): refresh_job() "succeeds" but
#     pool.height < chain.height; watchdog notices and forces restart.
# Default 180s is 1.5x the block target, generous enough that normal dry
# spells don't trip it but tight enough that a 3h47m stuck-template event
# (tonight's outage) becomes a 3-minute one.
JOB_WATCHDOG_S        = int(os.environ.get("JOB_WATCHDOG_S", "180"))
CREDITS_FILE          = os.environ.get(
    "CREDITS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-credits.json"))
BLOCKS_FILE           = os.environ.get(
    "BLOCKS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-blocks.json"))
PAYOUTS_FILE          = os.environ.get(
    "PAYOUTS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-payouts.json"))
# v1.1.13: per-wallet ledger adjustments. Positive entries add to a
# wallet's pending balance; negative entries subtract. Used to absorb
# historical operator-side losses (e.g. when a since-fixed bug caused
# the pool to overpay other miners and the operator decides to take
# the resulting wallet shortfall on their own books instead of
# spreading it across future contributors). Format mirrors credits/
# payouts so the same load helper works. Each entry should include a
# human-readable `reason` field for audit purposes.
ADJUSTMENTS_FILE      = os.environ.get(
    "ADJUSTMENTS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-adjustments.json"))
# v1.1.13: growth-fund sponsorship. Operator pre-funds the pool wallet
# with a stated amount of GLAC; for each block the pool finds while
# the fund is active, an extra per-block bonus is added to the round's
# distributable amount BEFORE the per-miner pro-rata calculation. The
# bonus comes out of the pre-funded balance until it drains.
#
# State (pool-sponsorship.json) is written by the admin fund_sponsorship
# script, mutated by settle_round() each time a bonus is consumed.
# Shape:
#   { active: bool, label: str, total_atomic, remaining_atomic,
#     per_block_atomic, blocks_seeded, blocks_used, started_at, ended_at? }
SPONSORSHIP_FILE      = os.environ.get(
    "SPONSORSHIP_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-sponsorship.json"))

# Wallet-RPC for auto-payout (matches the faucet's setup pattern; this
# is a SEPARATE wallet-rpc instance running on a different port from
# the faucet). The pool wallet's spend key lives on this VM (industry-
# standard hot-pool model). Mitigate by sweeping accumulated balance
# to a cold address regularly.
WALLET_RPC_URL        = os.environ.get("WALLET_RPC_URL", "http://127.0.0.1:28084/json_rpc")
WALLET_PASSWORD       = os.environ.get("WALLET_PASSWORD", "")
# Minimum credit to trigger a payout. 0.1 GLAC = 1e11 atomic units.
MIN_PAYOUT_ATOMIC     = int(os.environ.get("MIN_PAYOUT_ATOMIC", str(10**11)))
# How often the payout loop drains pending credits.
PAYOUT_INTERVAL_S     = int(os.environ.get("PAYOUT_INTERVAL_S", "60"))
# Per-transfer mixin (Monero ringsize). Default matches RingCT v15+.
PAYOUT_MIXIN          = int(os.environ.get("PAYOUT_MIXIN", "15"))
# Wallet priority: 0=default, 1=unimportant, 2=normal, 3=elevated, 4=priority
PAYOUT_PRIORITY       = int(os.environ.get("PAYOUT_PRIORITY", "0"))

MAX_RECENT_JOBS = 8

# Sliding-window length for hashrate estimation, in seconds. Each share at
# difficulty D represents D expected hashes; pool hashrate is
# sum(D in window) / window. Replaced the v1.1.9 EWMA-on-D/dt estimator,
# which underreported by ~25% because share inter-arrival times are
# exponentially distributed and the harmonic-mean nature of D/dt biased
# the average low once the short-dt clamp kicked in. 300 s is the
# industry-standard window for pool-side hashrate displays.
HASHRATE_WINDOW_S = int(os.environ.get("HASHRATE_WINDOW_S", "300"))

# ---- v1.1 hardening -----------------------------------------------------
#
# Share verification via the Lattice PoW shared library, cold-sweep of the
# hot wallet, and stricter input validation.

# Lattice verifier (pool_verify.c -> libpool_verify.so on this VM).
VERIFY_LIB_PATH       = os.environ.get(
    "VERIFY_LIB_PATH", "/root/libpool_verify.so")
# After N invalid shares from the same wallet, drop further submissions.
# Honest miners hit 0 invalid shares; cheaters hit this fast. v1.1.10:
# raised from 10 to 25 -- a real epoch-transition bug locked an honest
# miner out at the lower threshold. We also log a WARNING at 10 so an
# operator sees the wallet is in trouble before it gets banned.
INVALID_SHARE_BAN_AT  = int(os.environ.get("INVALID_SHARE_BAN_AT", "25"))
INVALID_SHARE_WARN_AT = int(os.environ.get("INVALID_SHARE_WARN_AT", "10"))
# How long a wallet stays banned before auto-unbanning. The block-found
# epoch transitions seem to cause honest miners to occasionally burst
# invalid shares (right job_id, stale internal dataset state) -- a
# self-healing ban is much friendlier than a permanent one that needs
# operator intervention or pool restart. Cheaters that keep submitting
# invalid shares just get re-banned in a few seconds.
INVALID_SHARE_BAN_SEC = int(os.environ.get("INVALID_SHARE_BAN_SEC", "900"))  # 15 min
# How many shares per wallet per second we'll accept (rate-limit spam).
# At share_diff = network/1000 a single fast CPU does maybe 5-10 shares/s;
# 60/s gives ridiculous headroom while still capping abuse.
MAX_SHARES_PER_SEC    = float(os.environ.get("MAX_SHARES_PER_SEC", "60"))

# Cold-sweep: every COLD_SWEEP_INTERVAL_S the pool's unlocked balance above
# COLD_HOT_BUFFER_ATOMIC is transferred to COLD_ADDRESS. The hot wallet
# never holds more than the buffer + immediate payout queue, so if the VM
# is compromised the attacker only gets ~1 sweep window's worth.
COLD_ADDRESS          = os.environ.get("COLD_ADDRESS", "").strip()
COLD_HOT_BUFFER_ATOMIC = int(os.environ.get("COLD_HOT_BUFFER_ATOMIC",
                                            str(50 * 10**12)))  # 50 GLAC
COLD_SWEEP_INTERVAL_S  = int(os.environ.get("COLD_SWEEP_INTERVAL_S", "3600"))  # 1h

# Maturity tracker. Coinbase outputs on Monero-derived chains (Rime
# included) are locked for 60 confirmations. We poll the wallet's
# balance + blocks_to_unlock periodically so the stats page can show
# users when their next payout chunk unlocks instead of leaving them
# wondering why "pending X GLAC" doesn't translate to a transfer.
MATURITY_REFRESH_S     = int(os.environ.get("MATURITY_REFRESH_S", "30"))
# v1.1.13: how many chain confirmations a credit's underlying block must
# have before it counts toward a wallet's pending balance. With the
# default of 60 (= Monero coinbase lock), the credit ledger pays out
# in lockstep with when the actual coinbase output unlocks in the pool
# wallet -- making it impossible for the pool to overpay for a block
# that ends up orphaned, regardless of how much unlocked liquidity
# happens to be sitting in the wallet at the time. Setting lower
# (e.g. 10) trades a small orphan risk for faster payouts.
SETTLEMENT_DEPTH       = int(os.environ.get("SETTLEMENT_DEPTH", "60"))
# Rime block target (matches src/cryptonote_config.h DIFFICULTY_TARGET_V2).
BLOCK_TIME_S           = int(os.environ.get("BLOCK_TIME_S", "120"))

# Wallet address validator. Rime mainnet addresses use the Monero scheme
# adapted to R-prefix: 95 chars, base58 alphabet, leading 'R'. Integrated
# addresses (106 chars, starting with R + i-character) are also accepted.
# This is a sanity filter against accidental garbage / shell-meta tricks;
# the daemon does the real cryptographic check on payout time.
R_ADDR_RE = re.compile(r"^R[1-9A-HJ-NP-Za-km-z]{94,105}$")

def is_valid_wallet(addr):
    return isinstance(addr, str) and R_ADDR_RE.match(addr) is not None

# ---- Lattice verifier handles ------------------------------------------

_lattice = None
try:
    _lattice = ctypes.CDLL(VERIFY_LIB_PATH)
    _lattice.pool_build_dataset.restype  = ctypes.c_void_p
    _lattice.pool_build_dataset.argtypes = [ctypes.c_char_p]
    _lattice.pool_free_dataset.argtypes  = [ctypes.c_void_p]
    _lattice.pool_verify_share.restype   = ctypes.c_int
    _lattice.pool_verify_share.argtypes  = [
        ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint64,
    ]
    # v1.1.10: enriched variant that also returns the 32-byte hash so we
    # can log it on rejection. Symbol may be missing if libpool_verify.so
    # hasn't been rebuilt yet -- fall back to plain pool_verify_share.
    try:
        _lattice.pool_verify_share_v2.restype   = ctypes.c_int
        _lattice.pool_verify_share_v2.argtypes  = [
            ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
            ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint64,
            ctypes.c_char_p,
        ]
        _lattice_has_v2 = True
    except AttributeError:
        _lattice_has_v2 = False
except OSError as e:
    print(f"[pool] WARNING: could not load {VERIFY_LIB_PATH}: {e}. "
          "Falling back to v1 TRUST MODE.", file=sys.stderr)
    _lattice = None
    _lattice_has_v2 = False

# Dataset cache: seed_hash hex -> opaque pointer from pool_build_dataset().
# Bounded -- LRU-evict oldest when we exceed MAX_CACHED_DATASETS.
_datasets        = collections.OrderedDict()
_datasets_lock   = threading.Lock()
MAX_CACHED_DATASETS = 4


def get_dataset_for(seed_hash_hex):
    """Return the cached dataset pointer for `seed_hash_hex`, building it
    lazily if needed. Returns None if verification is disabled (lib not
    loaded) or seed is invalid."""
    if _lattice is None:
        return None
    if not seed_hash_hex or len(seed_hash_hex) != 64:
        return None
    try:
        seed = bytes.fromhex(seed_hash_hex)
    except ValueError:
        return None
    with _datasets_lock:
        if seed_hash_hex in _datasets:
            # Move to end (most-recently-used).
            _datasets.move_to_end(seed_hash_hex)
            return _datasets[seed_hash_hex]
        # Evict oldest if at capacity.
        while len(_datasets) >= MAX_CACHED_DATASETS:
            old_seed, old_ptr = _datasets.popitem(last=False)
            _lattice.pool_free_dataset(old_ptr)
        ptr = _lattice.pool_build_dataset(seed)
        _datasets[seed_hash_hex] = ptr
        return ptr


def find_nonce_offset_buf(buf):
    """Same logic as find_nonce_offset() but for an arbitrary buffer.
    The daemon zeros the nonce slot in blockhashing_blob; we scan for
    the first 4 zero bytes between offsets 34 and 80."""
    for off in range(34, min(len(buf) - 4, 80)):
        if buf[off] == 0 and buf[off+1] == 0 and buf[off+2] == 0 and buf[off+3] == 0:
            return off
    return 39


def verify_share_against_job(job, nonce):
    """Run the actual Lattice PoW check. Returns (ok, info_dict) where
    info_dict captures details useful for logging on failure. If the
    verifier lib isn't loaded, returns (True, {}) -- trust-mode fallback;
    we'd rather accept than reject when crippled.

    v1.1.10: uses pool_verify_share_v2 when available so we can log the
    actual hash on rejection for diagnostics. Falls back to the old
    boolean-only call if the lib is from before v1.1.10."""
    if _lattice is None:
        return True, {}
    seed_hex = job.get("seed_hash", "")
    ds = get_dataset_for(seed_hex)
    if ds is None:
        # No dataset available -- accept (better than locking miners out).
        return True, {"reason": "no_dataset"}
    blob_hex = job["blockhashing_blob"]
    try:
        blob = bytes.fromhex(blob_hex)
    except ValueError:
        return False, {"reason": "bad_blob_hex"}
    off = find_nonce_offset_buf(blob)
    if _lattice_has_v2:
        out_hash = ctypes.create_string_buffer(32)
        res = _lattice.pool_verify_share_v2(
            blob, len(blob), off, nonce & 0xFFFFFFFF,
            ctypes.c_void_p(ds), job["share_difficulty"], out_hash)
        info = {
            "seed_hash":   seed_hex[:16] + "...",
            "share_diff":  job["share_difficulty"],
            "nonce":       nonce & 0xFFFFFFFF,
            "blob_off":    off,
            "blob_len":    len(blob),
            "hash":        out_hash.raw.hex(),
            "hash_top64":  int.from_bytes(out_hash.raw[24:32], "little"),
        }
    else:
        res = _lattice.pool_verify_share(
            blob, len(blob), off, nonce & 0xFFFFFFFF,
            ctypes.c_void_p(ds), job["share_difficulty"])
        info = {"seed_hash": seed_hex[:16] + "...", "v2": False}
    return (res == 1), info


# Per-wallet hardening state.
_wallet_bans      = set()          # wallets we've banned for too many bad shares
_wallet_ban_time  = {}             # wallet -> unix ts the ban was issued
_wallet_bad_count = collections.defaultdict(int)


def is_banned(wallet):
    """Check ban with lazy auto-expiry. Must be called under _hard_lock.

    Bans older than INVALID_SHARE_BAN_SEC are cleared on read -- the
    wallet gets a fresh chance and a zeroed bad-count. Cheaters who
    are still submitting bad shares will just hit the threshold and
    re-ban within seconds; honest miners caught in a transient burst
    can come back without operator intervention or a pool restart.
    """
    if wallet not in _wallet_bans:
        return False
    issued = _wallet_ban_time.get(wallet, 0)
    if time.time() - issued >= INVALID_SHARE_BAN_SEC:
        _wallet_bans.discard(wallet)
        _wallet_ban_time.pop(wallet, None)
        _wallet_bad_count[wallet] = 0
        print(f"[pool] auto-unban {wallet[:12]}... after "
              f"{INVALID_SHARE_BAN_SEC}s", file=sys.stderr)
        return False
    return True
_wallet_last_acc  = {}             # wallet -> last accept timestamp (rate limit)
_hard_lock        = threading.Lock()


# ---- in-memory state (protected by STATE_LOCK) ----------------------------

STATE_LOCK = threading.Lock()

# Latest job we got from the daemon. Refreshed by refresh_job() on a
# timer; also re-fetched eagerly when a new block lands.
current_job = None

# OrderedDict: job_id -> { template metadata + submissions set }
recent_jobs = collections.OrderedDict()

# v1.1.13: heights we've already credited (orphan-double-credit guard).
# When two miners both find full-block-meeting nonces on the same
# template, the daemon accepts both submissions, and `settle_round`
# would otherwise run TWICE -- crediting each submitting miner the
# full block reward against their accumulated shares since the last
# reset. Pool wallet only ever receives ONE coinbase (the canonical
# block; the other orphans), so the second credit is a phantom. We
# refuse to write the credit twice. First-to-settle wins; later
# submissions for the same height are silently dropped from credit
# accounting (the daemon still gets the submitblock attempt, that's
# fine -- redundant submissions of the same chain position cost
# nothing).
#
# This set is populated from pool-credits.json on startup, so the guard
# survives restarts. New heights are added inside the STATE_LOCK when
# settle_round runs.
_settled_heights: set[int] = set()

# wallet -> dict of share counters + sliding-window share history
miner_stats = {}

# Pool-wide rolling counters.
pool_stats = {
    "shares_total": 0,
    "shares_today": 0,
    "blocks_total": 0,
    "blocks_today": 0,
    "last_block": None,
    "current_round_shares": 0,
    "started_at": time.time(),
    "day_anchor": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
    # v1.1.10: track WHY shares get rejected. Anything non-zero here is
    # an effective hashrate leak between miners and the pool counter, so
    # the page (and operator) can see what's bleeding.
    "submits_total":   0,
    "rejects_by_reason": {
        "rate_limit":    0,
        "stale_job":     0,
        "duplicate":     0,
        "invalid_share": 0,
        "bad_request":   0,
        "banned":        0,
    },
    # v1.1.13: count orphan-double-credit guard firings. Non-zero is
    # normal and expected -- it means two miners hit full-block target
    # on the same template and we correctly refused to pay both. Each
    # firing represents ~35 GLAC of phantom credit we DIDN'T write to
    # the ledger.
    "orphan_dedupes":  0,
}


# ---- helpers --------------------------------------------------------------

def today_utc():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d")


def rollover_if_needed():
    """Roll the daily counters at UTC midnight."""
    d = today_utc()
    if d != pool_stats["day_anchor"]:
        pool_stats["day_anchor"]   = d
        pool_stats["shares_today"] = 0
        pool_stats["blocks_today"] = 0
        for s in miner_stats.values():
            s["shares_today"] = 0


def rpc(method, params=None, timeout=8):
    """Call the local rimed via JSON-RPC. Raises on transport / RPC error."""
    body = json.dumps({"jsonrpc": "2.0", "id": "0",
                       "method": method, "params": params or {}}).encode("utf-8")
    req = urllib.request.Request(
        DAEMON_RPC, data=body,
        headers={"Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.loads(r.read().decode("utf-8"))
    if "error" in resp and resp["error"]:
        raise RuntimeError(f"RPC {method}: {resp['error'].get('message', resp['error'])}")
    return resp.get("result")


def diff_to_target_hex(diff):
    """Approximate target as 4-byte little-endian hex (Monero stratum
    convention). Miners don't need this for validation; it's a UI hint."""
    top = (2**32) // max(1, diff)
    if top >= 2**32:
        top = 2**32 - 1
    return struct.pack("<I", top).hex()


def refresh_job():
    """Pull a fresh block template from the daemon, addressed to the pool
    wallet. Stored as `current_job`; also pushed into `recent_jobs` so
    miners that just received an old job can still submit briefly."""
    global current_job
    try:
        t = rpc("get_block_template", {
            "wallet_address": POOL_WALLET,
            "reserve_size": 8,
        })
        job_id = "j_" + secrets.token_hex(6)
        net_diff = int(t["difficulty"])
        share_diff = max(1, net_diff // SHARE_DIFF_DIVISOR)
        job = {
            "job_id":              job_id,
            "blockhashing_blob":   t["blockhashing_blob"],
            "blocktemplate_blob":  t["blocktemplate_blob"],
            "seed_hash":           t.get("seed_hash", ""),
            "height":              int(t["height"]),
            "network_difficulty":  net_diff,
            "share_difficulty":    share_diff,
            "target":              diff_to_target_hex(share_diff),
            "created_at":          time.time(),
            "submissions":         set(),
        }
        with STATE_LOCK:
            current_job = job
            recent_jobs[job_id] = job
            while len(recent_jobs) > MAX_RECENT_JOBS:
                recent_jobs.popitem(last=False)
    except Exception as e:
        print(f"[pool] refresh_job failed: {e}", file=sys.stderr)


def _new_miner_stats():
    return {
        "shares_total": 0, "shares_today": 0, "shares_this_round": 0,
        "last_share_ts": 0.0, "blocks_found": 0,
        # Deque of (ts, share_difficulty) for the sliding-window hashrate.
        # Trimmed lazily in _compute_hashrate().
        "share_hist": collections.deque(),
    }


def _compute_hashrate(s, now):
    """Unbiased pool hashrate: sum of share difficulties in the last
    HASHRATE_WINDOW_S seconds, divided by the window length. Each share
    at difficulty D represents D expected hashes."""
    cutoff = now - HASHRATE_WINDOW_S
    hist = s["share_hist"]
    while hist and hist[0][0] < cutoff:
        hist.popleft()
    if not hist:
        return 0.0
    return sum(d for _, d in hist) / HASHRATE_WINDOW_S


def credit_share(wallet):
    """Record a share for the submitting wallet. Updates rolling stats."""
    rollover_if_needed()
    s = miner_stats.get(wallet)
    if s is None:
        s = _new_miner_stats()
        miner_stats[wallet] = s
    now = time.time()
    s["shares_total"]     += 1
    s["shares_today"]     += 1
    s["shares_this_round"] += 1
    if current_job is not None:
        s["share_hist"].append((now, current_job["share_difficulty"]))
    s["last_share_ts"] = now
    pool_stats["shares_total"]         += 1
    pool_stats["shares_today"]         += 1
    pool_stats["current_round_shares"] += 1


# ---- block submission + payout settlement --------------------------------

def find_nonce_offset(buf):
    """Locate the 4 zero bytes that mark the nonce slot in the
    blockhashing_blob. After the daemon serializes the template the nonce
    is zeroed; we look for the first 4-zero-byte sequence between offset
    34 and 80, falling back to the canonical 39 if no match."""
    for off in range(34, min(len(buf) - 4, 80)):
        if buf[off] == 0 and buf[off+1] == 0 and buf[off+2] == 0 and buf[off+3] == 0:
            return off
    return 39


def submit_block(job, nonce, finder_wallet):
    """A miner claims this nonce produces a hash meeting network_difficulty.
    Splice the nonce into the blocktemplate blob and forward to the daemon.
    The daemon verifies the PoW server-side, so a false claim is harmless
    (just rejected)."""
    try:
        tb = bytearray.fromhex(job["blocktemplate_blob"])
        hb = bytearray.fromhex(job["blockhashing_blob"])
        off = find_nonce_offset(hb)
        if off + 4 > len(tb):
            return {"accepted": True, "block": False, "reason": "nonce offset OOB"}
        struct.pack_into("<I", tb, off, nonce & 0xFFFFFFFF)
        try:
            result = rpc("submitblock", [tb.hex()])
        except Exception as e:
            print(f"[pool] submitblock RPC error: {e}", file=sys.stderr)
            return {"accepted": True, "block": False, "reason": str(e)}
        if isinstance(result, dict) and result.get("status") == "OK":
            try:
                hdr = rpc("get_block_header_by_height", {"height": job["height"]})
                reward = int(hdr["block_header"]["reward"])
            except Exception:
                reward = 0
            block = {
                "height":       job["height"],
                "ts":           int(time.time()),
                "finder":       finder_wallet,
                "reward_atomic": reward,
                "round_shares": pool_stats["current_round_shares"],
                "share_diff":   job["share_difficulty"],
                "net_diff":     job["network_difficulty"],
            }
            with STATE_LOCK:
                # v1.1.13: orphan-double-credit guard. If a credit at
                # this height has already been written (i.e. an earlier
                # submitter beat us to settle_round), DO NOT credit
                # again. The pool wallet receives only one coinbase per
                # height; crediting twice creates a phantom that gets
                # paid out as real GLAC before the reconciler can catch
                # it. First-to-settle wins.
                if block["height"] in _settled_heights:
                    pool_stats["orphan_dedupes"] += 1
                    print(f"[pool] BLOCK submitted at height={block['height']} but "
                          f"already settled; SKIPPING credit (would have gone to "
                          f"{finder_wallet[:12]}...). This is the orphan-race guard; "
                          f"normal and expected when multiple miners hit full-block "
                          f"target on the same template.", file=sys.stderr)
                    return {"accepted": True, "block": True}
                _settled_heights.add(block["height"])
                pool_stats["blocks_total"] += 1
                pool_stats["blocks_today"] += 1
                pool_stats["last_block"]    = block
                fs_stats = miner_stats.get(finder_wallet)
                if fs_stats:
                    fs_stats["blocks_found"] += 1
                settle_round(block)
                pool_stats["current_round_shares"] = 0
                for s in miner_stats.values():
                    s["shares_this_round"] = 0
                append_block(block)
            print(f"[pool] BLOCK FOUND height={block['height']} "
                  f"finder={finder_wallet[:12]}... "
                  f"reward={reward/1e12:.4f} GLAC")
            return {"accepted": True, "block": True}
        # Daemon rejected -- usually because the share didn't actually meet
        # network diff (miner mis-claimed). Harmless; share already credited.
        return {"accepted": True, "block": False, "reason": f"daemon rejected: {result}"}
    except Exception as e:
        print(f"[pool] submit_block error: {e}\n{traceback.format_exc()}", file=sys.stderr)
        return {"accepted": True, "block": False, "reason": str(e)}


# v1.1.13: growth-fund sponsorship state lock. The sponsorship file
# (pool-sponsorship.json) is mutated atomically inside this lock by
# settle_round() each time a bonus is consumed.
_sponsorship_lock = threading.Lock()


def sponsorship_consume_block():
    """Atomically consume one block's worth of growth-fund bonus.
    Returns the atomic amount to add to this block's distributable
    reward, or 0 if no sponsorship is active / drained. Updates the
    on-disk state file."""
    with _sponsorship_lock:
        state = load_json_file(SPONSORSHIP_FILE, None)
        if not state or not state.get("active"):
            return 0
        per_block = int(state.get("per_block_atomic", 0))
        remaining = int(state.get("remaining_atomic", 0))
        if per_block <= 0 or remaining <= 0:
            return 0
        # Final block may have less than a full per-block bonus left.
        bonus = min(per_block, remaining)
        state["remaining_atomic"] = remaining - bonus
        state["blocks_used"]      = int(state.get("blocks_used", 0)) + 1
        if state["remaining_atomic"] <= 0:
            state["active"]    = False
            state["ended_at"]  = int(time.time())
        save_json_atomic(SPONSORSHIP_FILE, state)
        return bonus


def sponsorship_snapshot():
    """Read-only snapshot of the sponsorship state for /pool/stats.
    Returns None when no sponsorship has ever been activated."""
    state = load_json_file(SPONSORSHIP_FILE, None)
    if not state:
        return None
    remaining = int(state.get("remaining_atomic", 0))
    per_block = int(state.get("per_block_atomic", 0))
    return {
        "active":               bool(state.get("active")),
        "label":                state.get("label", ""),
        "total_atomic":         int(state.get("total_atomic", 0)),
        "total_glac":           int(state.get("total_atomic", 0)) / 1e12,
        "remaining_atomic":     remaining,
        "remaining_glac":       remaining / 1e12,
        "per_block_atomic":     per_block,
        "per_block_glac":       per_block / 1e12,
        "blocks_seeded":        int(state.get("blocks_seeded", 0)),
        "blocks_used":          int(state.get("blocks_used", 0)),
        "blocks_remaining_est": (remaining // per_block) if per_block > 0 else 0,
        "started_at":           state.get("started_at"),
        "ended_at":             state.get("ended_at"),
    }


def settle_round(block):
    """Append credit entries per contributing miner. The payout helper
    reads this file, sums per wallet, and submits actual transfers from
    a separate machine. This VM never holds the spend key."""
    reward = block.get("reward_atomic", 0)
    if not reward:
        return
    pool_cut = int(reward * POOL_FEE_PERCENT / 100.0)
    distributable = reward - pool_cut

    # v1.1.13: top up with growth-fund sponsorship bonus if active.
    # The bonus comes from the pre-funded balance the operator put into
    # the pool wallet; it is added to `distributable` BEFORE the
    # per-miner pro-rata calculation so every contributor benefits in
    # proportion to their share contribution this round.
    bonus_atomic = sponsorship_consume_block()
    if bonus_atomic > 0:
        distributable += bonus_atomic
        # Stamp the block record itself so the recent-blocks table
        # (read from pool-blocks.json) can display reward + bonus =
        # total paid out, not just the chain coinbase. settle_round
        # runs while append_block hasn't yet been called, so this
        # mutation propagates straight into the persisted block row.
        block["sponsorship_atomic"]   = bonus_atomic
        block["total_paid_atomic"]    = reward + bonus_atomic

    total_shares = block.get("round_shares") or 0
    if total_shares <= 0:
        return
    entries = []
    for wallet, s in miner_stats.items():
        if s["shares_this_round"] <= 0:
            continue
        credit = int(distributable * s["shares_this_round"] / total_shares)
        if credit > 0:
            entries.append({
                "ts":            int(time.time()),
                "height":        block["height"],
                "wallet":        wallet,
                "atomic":        credit,
                "shares":        s["shares_this_round"],
                "round_shares":  total_shares,
                # Tag the entry with the sponsorship bonus that was
                # rolled into this block's distributable (for audit).
                "sponsorship_atomic": bonus_atomic,
            })
    if entries:
        append_credits(entries)


def append_credits(entries):
    try:
        arr = []
        if os.path.exists(CREDITS_FILE):
            with open(CREDITS_FILE, "r", encoding="utf-8") as f:
                arr = json.load(f)
        arr.extend(entries)
        tmp = CREDITS_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(arr, f)
        os.replace(tmp, CREDITS_FILE)
    except Exception as e:
        print(f"[pool] credits file write failed: {e}", file=sys.stderr)


def append_block(block):
    try:
        arr = []
        if os.path.exists(BLOCKS_FILE):
            with open(BLOCKS_FILE, "r", encoding="utf-8") as f:
                arr = json.load(f)
        arr.insert(0, block)
        arr = arr[:200]
        tmp = BLOCKS_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(arr, f)
        os.replace(tmp, BLOCKS_FILE)
    except Exception as e:
        print(f"[pool] blocks file write failed: {e}", file=sys.stderr)


# ---- HTTP handler --------------------------------------------------------

CORS_HEADERS = {
    "Access-Control-Allow-Origin":  "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
    "Access-Control-Max-Age":       "86400",
}


class PoolHandler(http.server.BaseHTTPRequestHandler):

    # Quiet down the default per-request stdout line; we log our own
    # important events explicitly.
    def log_message(self, fmt, *args):
        return

    def _send_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in CORS_HEADERS.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def do_OPTIONS(self):
        self.send_response(204)
        for k, v in CORS_HEADERS.items():
            self.send_header(k, v)
        self.end_headers()

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        try:
            if url.path == "/pool/job":
                return self._handle_job()
            if url.path == "/pool/submit":
                return self._handle_submit()
            self._send_json(404, {"error": "not found"})
        except Exception as e:
            print(f"[pool] POST error: {e}\n{traceback.format_exc()}", file=sys.stderr)
            self._send_json(500, {"error": str(e)})

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        qs  = urllib.parse.parse_qs(url.query)
        try:
            if url.path == "/pool/stats":
                return self._handle_stats()
            if url.path == "/pool/blocks":
                return self._handle_blocks(qs)
            if url.path == "/pool/payouts":
                return self._handle_payouts(qs)
            if url.path.startswith("/pool/miner/"):
                wallet = url.path[len("/pool/miner/"):]
                return self._handle_miner(wallet)
            if url.path == "/":
                return self._send_json(200, {
                    "service": "glaciem-pool",
                    "version": "v1",
                    "endpoints": ["/pool/job", "/pool/submit", "/pool/stats",
                                  "/pool/miner/<wallet>", "/pool/blocks"],
                })
            self._send_json(404, {"error": "not found"})
        except Exception as e:
            print(f"[pool] GET error: {e}\n{traceback.format_exc()}", file=sys.stderr)
            self._send_json(500, {"error": str(e)})

    # ---- handlers -----------------------------------------------------

    def _handle_job(self):
        body = self._read_json()
        wallet = body.get("wallet")
        if not is_valid_wallet(wallet):
            return self._send_json(400, {"error": "invalid wallet address"})
        with _hard_lock:
            if is_banned(wallet):
                return self._send_json(403, {"error": "wallet banned for repeated invalid shares"})
        with STATE_LOCK:
            job = current_job
            if job is None:
                return self._send_json(503, {"error": "pool warming up; retry in a moment"})
            if wallet not in miner_stats:
                miner_stats[wallet] = _new_miner_stats()
            payload = {
                "job_id":             job["job_id"],
                "blockhashing_blob":  job["blockhashing_blob"],
                "blocktemplate_blob": job["blocktemplate_blob"],
                "seed_hash":          job["seed_hash"],
                "height":             job["height"],
                "network_difficulty": job["network_difficulty"],
                "share_difficulty":   job["share_difficulty"],
                "target":             job["target"],
            }
        self._send_json(200, payload)

    def _handle_submit(self):
        # v1.1.10: count every submit attempt so the page can compute a
        # straight accept-rate and surface the breakdown of WHY rejected.
        with STATE_LOCK:
            pool_stats["submits_total"] += 1
        body = self._read_json()
        job_id = body.get("job_id")
        wallet = body.get("wallet")
        nonce  = body.get("nonce")
        if not job_id or not isinstance(nonce, int):
            with STATE_LOCK:
                pool_stats["rejects_by_reason"]["bad_request"] += 1
            return self._send_json(400, {"error": "job_id and nonce required"})
        if not is_valid_wallet(wallet):
            with STATE_LOCK:
                pool_stats["rejects_by_reason"]["bad_request"] += 1
            return self._send_json(400, {"error": "invalid wallet address"})
        nonce &= 0xFFFFFFFF

        # Hardening: check banlist + rate limit BEFORE doing PoW verify.
        now = time.time()
        with _hard_lock:
            if is_banned(wallet):
                with STATE_LOCK:
                    pool_stats["rejects_by_reason"]["banned"] += 1
                return self._send_json(403, {"error": "wallet banned for repeated invalid shares"})
            last = _wallet_last_acc.get(wallet, 0.0)
            min_gap = 1.0 / MAX_SHARES_PER_SEC
            if now - last < min_gap:
                with STATE_LOCK:
                    pool_stats["rejects_by_reason"]["rate_limit"] += 1
                return self._send_json(429, {"accepted": False, "reason": "rate limit"})

        with STATE_LOCK:
            job = recent_jobs.get(job_id)
            if job is None:
                pool_stats["rejects_by_reason"]["stale_job"] += 1
                return self._send_json(200, {"accepted": False, "reason": "stale job"})
            dedupe_key = f"{wallet}:{nonce}"
            if dedupe_key in job["submissions"]:
                pool_stats["rejects_by_reason"]["duplicate"] += 1
                return self._send_json(200, {"accepted": False, "reason": "duplicate"})
            # Snapshot the job under the lock so verify_share_against_job
            # can run without holding STATE_LOCK (the hash is microseconds
            # but we'd rather not block other threads).
            job_snap = {
                "blockhashing_blob": job["blockhashing_blob"],
                "seed_hash":         job["seed_hash"],
                "share_difficulty":  job["share_difficulty"],
            }
            full_block = bool(body.get("full_block"))

        # v1.1 hardening: Lattice-verify the share before crediting.
        valid, vinfo = verify_share_against_job(job_snap, nonce)
        if not valid:
            # v1.1.12: don't penalize stale-job invalid shares. When the
            # pool rotates jobs (new block found, new template), in-flight
            # miner hashes naturally fail verification against the new
            # target until the miner picks up the next job. Those shares
            # show up here with an *older* job_id that's still in
            # recent_jobs (i.e. not stale enough to hit the earlier
            # stale-job branch) but isn't current_job. Counting them
            # toward the ban threshold means honest miners with normal
            # job-pickup latency get banned periodically -- which is what
            # was happening.
            #
            # Cheater detection still works: a real attacker has to forge
            # shares against the *current* job_id to credit themselves
            # (older jobs' template_data is no longer the work the pool
            # is paying for), so only current-job invalid shares count.
            current_top = current_job["job_id"] if current_job else None
            if job_id != current_top:
                with STATE_LOCK:
                    pool_stats["rejects_by_reason"].setdefault(
                        "stale_against_old_job", 0)
                    pool_stats["rejects_by_reason"]["stale_against_old_job"] += 1
                return self._send_json(200, {
                    "accepted": False,
                    "reason":   "stale -- newer job available",
                })

            with _hard_lock:
                _wallet_bad_count[wallet] += 1
                bad = _wallet_bad_count[wallet]
                if bad == INVALID_SHARE_WARN_AT:
                    # Early-warning log so the operator sees a wallet in
                    # trouble *before* it gets banned. Real cheaters will
                    # blow past this; an honest miner with a temporary
                    # bug (stale dataset, epoch transition, etc.) often
                    # gets here briefly and then self-corrects.
                    print(f"[pool] WARN {wallet[:12]}... has {bad} invalid shares "
                          f"(ban threshold {INVALID_SHARE_BAN_AT}). "
                          f"sample info={vinfo}",
                          file=sys.stderr)
                if bad >= INVALID_SHARE_BAN_AT:
                    _wallet_bans.add(wallet)
                    _wallet_ban_time[wallet] = time.time()
                    print(f"[pool] BANNED {wallet[:12]}... after {bad} invalid shares. "
                          f"last info={vinfo}",
                          file=sys.stderr)
            with STATE_LOCK:
                pool_stats["rejects_by_reason"]["invalid_share"] += 1
            # v1.1.10: log every individual invalid share with details so
            # we can diagnose the root cause when it happens again.
            # Top-64 bits of the hash * difficulty should be 0 for a
            # valid share; logging the actual top64 lets us see how far
            # off the rejected hash was -- close-to-target = real share
            # against the wrong dataset; uniformly random = cheating or
            # blob mangling.
            print(f"[pool] invalid share from {wallet[:12]}... bad_count={bad}/{INVALID_SHARE_BAN_AT} "
                  f"info={vinfo}",
                  file=sys.stderr)
            return self._send_json(200, {"accepted": False, "reason": "invalid share"})

        # Valid share -- mark the dedupe key, credit, update rate-limit timestamp.
        # Reset bad-count on success so an old transient blip doesn't
        # haunt a wallet forever.
        with STATE_LOCK:
            job["submissions"].add(dedupe_key)
            credit_share(wallet)
        with _hard_lock:
            _wallet_last_acc[wallet] = now
            if _wallet_bad_count.get(wallet, 0) > 0:
                _wallet_bad_count[wallet] = 0

        if full_block:
            r = submit_block(job, nonce, wallet)
            return self._send_json(200, r)
        self._send_json(200, {"accepted": True})

    def _handle_stats(self):
        with STATE_LOCK:
            rollover_if_needed()
            now = time.time()
            active = sum(1 for s in miner_stats.values()
                         if s["last_share_ts"] > now - 5 * 60)
            hashrate = sum(_compute_hashrate(s, now) for s in miner_stats.values())
            payload = {
                "pool_wallet":          POOL_WALLET,
                "pool_fee_percent":     POOL_FEE_PERCENT,
                "share_diff_divisor":   SHARE_DIFF_DIVISOR,
                "min_payout_atomic":    MIN_PAYOUT_ATOMIC,
                "hashrate":             int(round(hashrate)),
                "active_miners":        active,
                "shares_today":         pool_stats["shares_today"],
                "shares_total":         pool_stats["shares_total"],
                "blocks_today":         pool_stats["blocks_today"],
                "blocks_total":         pool_stats["blocks_total"],
                "current_round_shares": pool_stats["current_round_shares"],
                "last_block":           pool_stats["last_block"],
                "uptime_s":             int(now - pool_stats["started_at"]),
                # Hardening visibility -- lets the stats page advertise
                # what the pool actually enforces.
                "share_verification":   "lattice" if _lattice else "trust",
                "cold_sweep_enabled":   bool(COLD_ADDRESS),
                "banned_wallets_today": len(_wallet_bans),
                "current_job": ({
                    "job_id":             current_job["job_id"],
                    "height":             current_job["height"],
                    "network_difficulty": current_job["network_difficulty"],
                    "share_difficulty":   current_job["share_difficulty"],
                    "age_s":              int(now - current_job["created_at"]),
                } if current_job else None),
                # Coinbase-maturity countdown so the stats page can
                # answer the obvious question: "I'm owed X GLAC -- when
                # is it actually going to arrive?"
                "maturity":             maturity_snapshot(),
                # v1.1.10: every submit attempt and every per-reason
                # rejection. Lets the operator (and the page) see if the
                # accepted-share-derived hashrate is being depressed by
                # something fixable (rate-limit too tight, jobs evicted
                # too early, etc.) vs the miner just running that fast.
                "submits_total":        pool_stats["submits_total"],
                "rejects_by_reason":    dict(pool_stats["rejects_by_reason"]),
                # v1.1.13: orphan-double-credit guard count. Non-zero
                # is healthy and expected on a multi-miner pool.
                "orphan_dedupes":       pool_stats["orphan_dedupes"],
                # v1.1.13: growth-fund sponsorship state. Null when no
                # sponsorship has ever been active. Page reads this to
                # show "Growth fund: +X GLAC/block, Y blocks remaining".
                "sponsorship":          sponsorship_snapshot(),
            }
        self._send_json(200, payload)

    def _handle_miner(self, wallet):
        with STATE_LOCK:
            s = miner_stats.get(wallet)
            if not s:
                return self._send_json(200, {"wallet": wallet, "found": False})
            now = time.time()
            payload = {
                "wallet":            wallet,
                "found":             True,
                "hashrate":          int(round(_compute_hashrate(s, now))),
                "shares_total":      s["shares_total"],
                "shares_today":      s["shares_today"],
                "shares_this_round": s["shares_this_round"],
                "last_share_s_ago":  (int(now - s["last_share_ts"])
                                      if s["last_share_ts"] > 0 else None),
                "blocks_found":      s["blocks_found"],
            }
        # Pending payout balance: read fresh (cheap, JSON file).
        pending  = compute_pending_balances().get(wallet, 0)
        maturing = compute_maturing_balances().get(wallet, 0)
        payload["pending_atomic"]  = pending
        payload["pending_glac"]    = pending / 1e12
        # v1.1.13: maturing = credits earned but not yet at
        # SETTLEMENT_DEPTH confs deep. Lets the page show "you've
        # earned X, of which Y is in the maturity window and Z is
        # ready to pay out next round." Without this users see
        # pending=0 between block-find and 60-conf maturity and
        # think they're not getting credited.
        payload["maturing_atomic"] = maturing
        payload["maturing_glac"]   = maturing / 1e12
        payload["unpaid_total_glac"] = (pending + maturing) / 1e12
        # v1.1.13: surface any operator adjustments transparently so
        # anyone querying the API (or the page reading from it) can
        # see why their effective pending differs from raw earned -
        # paid. Empty list when no adjustments apply.
        adjustments = load_json_file(ADJUSTMENTS_FILE, [])
        wallet_adjustments = [
            {
                "ts":     a.get("ts"),
                "atomic": a.get("atomic"),
                "glac":   int(a.get("atomic", 0)) / 1e12,
                "reason": a.get("reason", ""),
            }
            for a in adjustments
            if a.get("wallet") == wallet
        ]
        payload["adjustments"] = wallet_adjustments
        self._send_json(200, payload)

    def _handle_blocks(self, qs):
        try:
            limit = min(100, max(1, int(qs.get("limit", ["20"])[0])))
        except (TypeError, ValueError):
            limit = 20
        arr = load_json_file(BLOCKS_FILE, [])
        self._send_json(200, {"blocks": arr[:limit]})

    def _handle_payouts(self, qs):
        """Recent payouts paid by the pool. Useful for miners to verify
        they were credited (tx hash they can look up in the explorer)."""
        try:
            limit = min(200, max(1, int(qs.get("limit", ["50"])[0])))
        except (TypeError, ValueError):
            limit = 50
        arr = load_json_file(PAYOUTS_FILE, [])
        # Newest first.
        arr = list(reversed(arr))[:limit]
        # Allow per-wallet filter.
        wallet = qs.get("wallet", [None])[0]
        if wallet:
            arr = [p for p in arr if p.get("wallet") == wallet]
        self._send_json(200, {"payouts": arr})


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---- background refresher -------------------------------------------------

def refresh_loop():
    # v1.1.16+ watchdog: tracks the last time the pool's current_job was
    # actually at the chain tip. If we fall behind for JOB_WATCHDOG_S
    # without recovering, we exit() so systemd respawns us with a fresh
    # daemon connection + a fresh RandomX dataset (recovers from both
    # daemon-down and stale-template failure modes; see env-var docstring).
    last_synced_ts = time.time()
    consecutive_failures = 0
    while True:
        # 1. Try to refresh the template. Errors are logged and counted,
        #    but we don't bail here -- the watchdog below is the bail logic.
        try:
            refresh_job()
            if current_job is not None:
                consecutive_failures = 0
            else:
                consecutive_failures += 1
        except Exception as e:
            consecutive_failures += 1
            print(f"[pool] refresh_loop error: {e}", file=sys.stderr)

        # 2. Cross-check against chain tip. get_info is cheap and tells us
        #    whether the daemon agrees with the template we just got. If
        #    get_block_template returned a stale height (this morning's
        #    bug), this comparison catches it; if the daemon is unreachable,
        #    both calls fail and last_synced_ts simply doesn't advance.
        try:
            info = rpc("get_info")
            chain_height = int(info["height"])
            pool_height = current_job["height"] if current_job else 0
            # get_block_template returns the next-block height, which equals
            # chain.height (block count). >= covers a brief race where the
            # pool fetched template first, then chain advanced before our
            # get_info round-trip landed.
            if pool_height >= chain_height:
                last_synced_ts = time.time()
        except Exception:
            # Can't reach daemon for the cross-check. refresh_job() already
            # logged the failure; don't double-log.
            pass

        # 3. Watchdog: have we been stuck too long?
        stale_for = time.time() - last_synced_ts
        if stale_for > JOB_WATCHDOG_S:
            ph = current_job["height"] if current_job else "NONE"
            print(f"[pool] WATCHDOG: template stuck for {int(stale_for)}s "
                  f"(pool_height={ph}, consecutive_failures={consecutive_failures}). "
                  f"Exiting for systemd restart.", file=sys.stderr)
            sys.stderr.flush()
            os._exit(1)

        time.sleep(TEMPLATE_REFRESH_S)


# ---- payout loop ---------------------------------------------------------
#
# Industry-standard hot-pool model. The pool wallet's spend key is loaded
# in a rime-wallet-rpc instance running on this same VM (WALLET_RPC_URL).
# Periodically:
#   1. Sum each miner's pending credits = total earned - total paid out.
#   2. For any miner whose pending >= MIN_PAYOUT_ATOMIC, build a transfer
#      via wallet-rpc.
#   3. Record the resulting tx_hash in PAYOUTS_FILE so the next iteration
#      sees the credit as cleared.
#
# Wallet keys NEVER leave this VM. To minimize hot-wallet exposure, the
# operator should run a separate sweep job (cron) that moves accumulated
# balance to a cold address daily.


def wallet_rpc(method, params=None, timeout=30):
    """Call the pool's rime-wallet-rpc (Monero-compatible)."""
    body = json.dumps({"jsonrpc": "2.0", "id": "0",
                       "method": method, "params": params or {}}).encode("utf-8")
    req = urllib.request.Request(
        WALLET_RPC_URL, data=body,
        headers={"Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.loads(r.read().decode("utf-8"))
    if "error" in resp and resp["error"]:
        raise RuntimeError(f"wallet-rpc {method}: {resp['error'].get('message', resp['error'])}")
    return resp.get("result")


def load_json_file(path, default):
    try:
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
    except Exception as e:
        print(f"[pool] load {path} failed: {e}", file=sys.stderr)
    return default


def save_json_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)


def compute_pending_balances():
    """Return {wallet: pending_atomic} based on credits minus payouts
    plus adjustments. Reads files fresh each call so an external editor
    can take effect without restart.

    v1.1.13b: only credits whose underlying block is at least
    SETTLEMENT_DEPTH confirmations deep are counted toward pending.
    Past that depth a reorg is effectively impossible, so paying
    against the credit can never end up overpaying for an orphaned
    block -- regardless of how much unlocked liquidity happens to be
    sitting in the pool wallet at the time. Freshly-credited blocks
    are deferred until they age in; they automatically count on the
    next /pool/miner query.

    Adjustments still apply unconditionally (they're not tied to a
    block height -- they're operator-recorded ledger corrections).
    Payouts also count unconditionally (they're already a tx that
    actually happened)."""
    credits     = load_json_file(CREDITS_FILE, [])
    payouts     = load_json_file(PAYOUTS_FILE, [])
    adjustments = load_json_file(ADJUSTMENTS_FILE, [])

    # Snapshot the arrived-heights cache. If it's empty (e.g. on the
    # very first request after startup, before the maturity loop has
    # had a chance to run), fall back to counting all credits -- worse
    # to show pending=0 to everyone for 30 seconds than to briefly
    # exceed wallet by one block's worth.
    with _arrived_heights_lock:
        arrived = frozenset(_arrived_heights)
        have_arrived_data = _arrived_heights_ts > 0

    earned = {}
    for c in credits:
        w = c.get("wallet")
        if not w:
            continue
        if have_arrived_data:
            h = int(c.get("height", 0))
            if h not in arrived:
                # Block's coinbase hasn't been scanned into the wallet
                # yet. Defer the credit so pending never exceeds wallet.
                continue
        earned[w] = earned.get(w, 0) + int(c.get("atomic", 0))
    paid = {}
    for p in payouts:
        w = p.get("wallet")
        if not w:
            continue
        # v1.1.13a: each payout deducts the recipient's amount AND their
        # share of the transaction fee from their pending. Otherwise the
        # fee comes out of the pool wallet but no one's pending reflects
        # it -- creating a slow drift where pending exceeds the wallet
        # balance by exactly the cumulative fees. round_fee_share has
        # been recorded on every payout since v1.1.9; fall back to the
        # full tx_fee_atomic on any older entry to be safe.
        amount = int(p.get("atomic", 0))
        fee    = int(p.get("round_fee_share",
                          p.get("tx_fee_atomic", 0)))
        paid[w] = paid.get(w, 0) + amount + fee
    adj = {}
    for a in adjustments:
        w = a.get("wallet")
        if not w:
            continue
        adj[w] = adj.get(w, 0) + int(a.get("atomic", 0))
    # Build the full wallet set so an adjustment-only wallet (e.g. a
    # negative-only operator absorption) still gets accounted for.
    all_wallets = set(earned) | set(paid) | set(adj)
    pending = {}
    for w in all_wallets:
        net = earned.get(w, 0) - paid.get(w, 0) + adj.get(w, 0)
        if net > 0:
            pending[w] = net
    return pending


def compute_maturing_balances():
    """Return {wallet: atomic} of credits the user has earned but whose
    underlying block is not yet at SETTLEMENT_DEPTH confirmations -- i.e.
    rewards in the maturity window that the payout loop is deferring
    until they're safe from orphan reorgs. This is "what's earned but
    not yet payable", complementary to compute_pending_balances() which
    is "what's earned AND safe enough to pay right now"."""
    credits = load_json_file(CREDITS_FILE, [])
    with _arrived_heights_lock:
        arrived = frozenset(_arrived_heights)
        have_data = _arrived_heights_ts > 0
    maturing = {}
    if not have_data:
        # Without maturity data we can't classify -- treat everything
        # as immediately pending (matches compute_pending_balances).
        return maturing
    for c in credits:
        w = c.get("wallet")
        if not w:
            continue
        h = int(c.get("height", 0))
        if h in arrived:
            continue   # past settlement depth, counted as pending
        maturing[w] = maturing.get(w, 0) + int(c.get("atomic", 0))
    return maturing


def payout_round():
    """One pass through pending credits. Sends any wallet that's over the
    payout threshold. Returns the number of successful transfers.

    v1.1.10: caps the round at the wallet's CURRENT unlocked balance.
    Block rewards inherit Monero's 60-confirmation coinbase lock, so on
    a young pool the wallet's `balance` is much bigger than its
    `unlocked_balance`. Naively trying to ship the full pending amount
    just bounces off wallet-rpc as "not enough money" until everything
    matures -- meanwhile users see "pending X GLAC" with no payouts
    landing. Instead we pro-rata each miner across whatever's spendable
    right now and ship that, then catch up in subsequent rounds as more
    outputs unlock."""
    pending = compute_pending_balances()
    eligible = [(w, a) for w, a in pending.items() if a >= MIN_PAYOUT_ATOMIC]
    if not eligible:
        return 0

    # Fetch the live unlocked balance so we know what we can actually
    # spend in this round. The maturity tracker has a cached snapshot
    # but the cache is up to MATURITY_REFRESH_S seconds stale, which is
    # too risky for an actual transfer -- ask the wallet directly.
    try:
        bal = wallet_rpc("get_balance", timeout=8)
    except Exception as e:
        print(f"[pool] payout: get_balance failed: {e}", file=sys.stderr)
        return 0
    unlocked = int(bal.get("unlocked_balance", 0))
    # Leave a small fee reserve. Rime/Monero fees per transfer are tiny
    # (~1e8-1e9 atomic for a normal tx) but a generous reserve protects
    # against multi-destination fee growth and round-up surprises.
    FEE_RESERVE_ATOMIC = 5 * 10**10   # 0.05 GLAC
    available = unlocked - FEE_RESERVE_ATOMIC
    if available < MIN_PAYOUT_ATOMIC:
        return 0   # nothing meaningful to send yet; wait for more maturity

    total_owed = sum(a for _, a in eligible)
    if total_owed > available:
        # Partial round: scale each miner's payout by available/total_owed
        # so everyone gets paid a fair fraction of what they're owed.
        # Anyone whose pro-rata share falls below MIN_PAYOUT_ATOMIC waits
        # for the next round.
        scale = available / total_owed
        capped = []
        for w, a in eligible:
            share = int(a * scale)
            if share >= MIN_PAYOUT_ATOMIC:
                capped.append((w, share))
        if not capped:
            return 0
        eligible = capped

    # Build a SINGLE multi-destination transfer per wallet-rpc call when
    # possible -- saves on fees and ring-signature work. wallet-rpc's
    # `transfer` accepts a destinations array.
    destinations = [{"amount": atomic, "address": wallet}
                    for wallet, atomic in eligible]
    try:
        r = wallet_rpc("transfer", {
            "destinations": destinations,
            "mixin":        PAYOUT_MIXIN,
            "ring_size":    PAYOUT_MIXIN + 1,
            "unlock_time":  0,
            "priority":     PAYOUT_PRIORITY,
            "get_tx_key":   True,
        }, timeout=120)
    except Exception as e:
        print(f"[pool] payout transfer failed: {e}", file=sys.stderr)
        return 0

    tx_hash = r.get("tx_hash", "")
    tx_fee  = int(r.get("fee", 0))
    payouts_log = load_json_file(PAYOUTS_FILE, [])
    ts = int(time.time())
    for wallet, atomic in eligible:
        payouts_log.append({
            "ts":     ts,
            "wallet": wallet,
            "atomic": atomic,
            "tx_hash": tx_hash,
            "tx_fee_atomic": tx_fee,
            "round_fee_share": tx_fee // max(1, len(eligible)),
        })
    save_json_atomic(PAYOUTS_FILE, payouts_log)
    print(f"[pool] PAYOUT round: {len(eligible)} wallets, "
          f"total={sum(a for _, a in eligible)/1e12:.4f} GLAC, "
          f"fee={tx_fee/1e12:.6f} GLAC, tx={tx_hash[:16]}...")
    return len(eligible)


def payout_loop():
    while True:
        time.sleep(PAYOUT_INTERVAL_S)
        try:
            payout_round()
        except Exception as e:
            print(f"[pool] payout_loop error: {e}", file=sys.stderr)


# ---- cold-sweep loop --------------------------------------------------
#
# Industry-standard hot-wallet hardening: every COLD_SWEEP_INTERVAL_S we
# transfer the pool wallet's unlocked balance above COLD_HOT_BUFFER_ATOMIC
# to COLD_ADDRESS (operator's offline wallet). The hot wallet never holds
# more than ~one sweep window's worth of rewards, so a VM compromise
# bounds the loss.
#
# The buffer keeps a small operating balance available for immediate
# payouts; the sweep only touches "extra" funds.


def cold_sweep_once():
    if not COLD_ADDRESS:
        return  # not configured; skip silently
    if not is_valid_wallet(COLD_ADDRESS):
        print(f"[pool] cold-sweep: COLD_ADDRESS doesn't look like a valid R-address; "
              f"refusing to send", file=sys.stderr)
        return
    try:
        bal = wallet_rpc("get_balance")
    except Exception as e:
        print(f"[pool] cold-sweep: get_balance failed: {e}", file=sys.stderr)
        return
    unlocked = int(bal.get("unlocked_balance", 0))
    # v1.1.10: miners come first. Subtract everything they're owed from
    # the spendable balance before deciding what to sweep -- otherwise
    # the cold sweep starves the payout loop when pending balances
    # exceed the buffer. Only the genuine surplus goes to cold storage.
    pending_owed = sum(a for a in compute_pending_balances().values()
                       if a >= MIN_PAYOUT_ATOMIC)
    available_for_sweep = max(0, unlocked - pending_owed)
    if available_for_sweep <= COLD_HOT_BUFFER_ATOMIC:
        return
    amount = available_for_sweep - COLD_HOT_BUFFER_ATOMIC
    try:
        r = wallet_rpc("transfer", {
            "destinations": [{"amount": amount, "address": COLD_ADDRESS}],
            "mixin":        PAYOUT_MIXIN,
            "ring_size":    PAYOUT_MIXIN + 1,
            "unlock_time":  0,
            "priority":     PAYOUT_PRIORITY,
            "get_tx_key":   True,
        }, timeout=120)
    except Exception as e:
        print(f"[pool] cold-sweep transfer failed: {e}", file=sys.stderr)
        return
    fee = int(r.get("fee", 0))
    tx  = r.get("tx_hash", "")
    print(f"[pool] COLD SWEEP {amount/1e12:.4f} GLAC -> {COLD_ADDRESS[:12]}... "
          f"fee={fee/1e12:.6f} GLAC tx={tx[:16]}...")
    # Record in payouts log so /pool/payouts reflects sweeps too.
    payouts_log = load_json_file(PAYOUTS_FILE, [])
    payouts_log.append({
        "ts":     int(time.time()),
        "wallet": COLD_ADDRESS,
        "atomic": amount,
        "tx_hash": tx,
        "tx_fee_atomic": fee,
        "kind":   "cold_sweep",
    })
    save_json_atomic(PAYOUTS_FILE, payouts_log)


def cold_sweep_loop():
    while True:
        time.sleep(COLD_SWEEP_INTERVAL_S)
        try:
            cold_sweep_once()
        except Exception as e:
            print(f"[pool] cold_sweep_loop error: {e}", file=sys.stderr)


# ---- coinbase-maturity tracker ----------------------------------------
#
# The wallet RPC exposes:
#   balance:           sum of every output we control (locked + unlocked)
#   unlocked_balance:  outputs available to spend right now
#   blocks_to_unlock:  chain confirmations the OLDEST locked output still
#                      needs before it joins unlocked_balance
#
# Coinbase outputs are locked for 60 confirmations on a Monero-derived
# chain. When a pool block lands, the reward shows up in `balance`
# immediately but cannot be paid out until it matures. Without exposing
# this, users see "pending 175 GLAC" and conclude the pool is broken.
# We poll the wallet every MATURITY_REFRESH_S and cache the answer.

# v1.1.13: arrived-heights cache. Populated alongside the maturity
# poll. compute_pending_balances() consults it so we never credit a
# block toward a wallet's pending balance until the pool wallet has
# actually received the coinbase. Guarantees pending <= wallet for
# every wallet, at all times, even between submit_block OK and the
# next wallet scan (~30s window without this cache).
_arrived_heights: set = set()
_arrived_heights_lock = threading.Lock()
_arrived_heights_ts: float = 0.0

_maturity_cache = {
    "ts":                       0.0,   # when we last refreshed
    "balance_atomic":           None,  # int or None if never fetched
    "unlocked_atomic":          None,
    "locked_atomic":            None,
    "blocks_to_next_unlock":    None,  # int or None
    "seconds_to_next_unlock":   None,
    "unspent_outputs":          None,  # int or None
}
_maturity_lock = threading.Lock()


def maturity_refresh_once():
    try:
        bal = wallet_rpc("get_balance", timeout=8)
    except Exception as e:
        print(f"[pool] maturity refresh failed: {e}", file=sys.stderr)
        return
    try:
        balance     = int(bal.get("balance", 0))
        unlocked    = int(bal.get("unlocked_balance", 0))
        locked      = max(0, balance - unlocked)
        # blocks_to_unlock is 0 when nothing is locked, so guard against it.
        btu         = int(bal.get("blocks_to_unlock", 0)) if locked > 0 else 0
        outputs     = 0
        for sub in bal.get("per_subaddress", []) or []:
            outputs += int(sub.get("num_unspent_outputs", 0))
        with _maturity_lock:
            _maturity_cache["ts"]                     = time.time()
            _maturity_cache["balance_atomic"]         = balance
            _maturity_cache["unlocked_atomic"]        = unlocked
            _maturity_cache["locked_atomic"]          = locked
            _maturity_cache["blocks_to_next_unlock"]  = btu
            _maturity_cache["seconds_to_next_unlock"] = btu * BLOCK_TIME_S
            _maturity_cache["unspent_outputs"]        = outputs
    except Exception as e:
        print(f"[pool] maturity refresh parse failed: {e}", file=sys.stderr)
    # v1.1.13: refresh the mature-heights cache used by
    # compute_pending_balances. A height is only included if its block
    # is at least SETTLEMENT_DEPTH confirmations deep in the chain --
    # past that depth a reorg is effectively impossible, so paying out
    # against the credit can never end up overpaying for an orphaned
    # block.
    #
    # The 25K growth-fund transfer provides unlocked liquidity in the
    # pool wallet, which without this gate would allow the payout loop
    # to fire on fresh (still-locked, still-orphan-risky) block rewards
    # by drawing from the transfer pile instead of waiting for the new
    # coinbase to mature. With this gate, payouts naturally synchronize
    # with the underlying coinbase becoming spendable.
    try:
        ins = wallet_rpc("get_transfers", {"in": True, "pool": False}, timeout=15)
        wh_resp = wallet_rpc("get_height", timeout=5)
        wallet_height = int(wh_resp.get("height", 0))
        mature_heights = set()
        for t in (ins.get("in") or []):
            h = int(t.get("height", 0))
            if h > 0 and wallet_height - h >= SETTLEMENT_DEPTH:
                mature_heights.add(h)
        with _arrived_heights_lock:
            global _arrived_heights_ts
            _arrived_heights.clear()
            _arrived_heights.update(mature_heights)
            _arrived_heights_ts = time.time()
    except Exception as e:
        print(f"[pool] mature-heights refresh failed: {e}", file=sys.stderr)


def maturity_loop():
    # First refresh runs immediately so /pool/stats has data without
    # waiting MATURITY_REFRESH_S.
    while True:
        try:
            maturity_refresh_once()
        except Exception as e:
            print(f"[pool] maturity_loop error: {e}", file=sys.stderr)
        time.sleep(MATURITY_REFRESH_S)


# ---- credit reconciliation -----------------------------------------------
#
# submit_block records a "block found" event the moment the daemon returns
# OK for our submitblock RPC. In practice the daemon can accept multiple
# competing blocks at the same height (alt-chain races) and only one ends
# up canonical -- the others get orphaned. submit_block credits ALL of
# them, so pool-credits.json accrues phantom credits whose coinbase outputs
# never actually arrive in the pool wallet. The user-visible symptom is
# "pending X GLAC" reading higher than the wallet actually holds; partial
# payouts then fail to converge because the math is off.
#
# Reconcile against the wallet's get_transfers list -- it's the only
# source of truth for "money the pool actually received". For every
# credited height that doesn't have a matching wallet arrival after a
# safety age in confirmations, drop the credit. Also collapse duplicate
# credits per (height, finder) -- only ONE block of value ever lands at
# a given height regardless of how many forks the daemon momentarily
# accepted, so a second credit at the same height for the same miner is
# always phantom.

RECONCILE_INTERVAL_S = int(os.environ.get("RECONCILE_INTERVAL_S", "60"))
# How many blocks of confirmation must pass before we'll declare a
# credit phantom. Wallet sometimes lags the daemon by a block or two.
RECONCILE_SAFETY_AGE = int(os.environ.get("RECONCILE_SAFETY_AGE", "10"))


def reconcile_credits_once():
    """Walk pool-credits.json, drop entries whose block coinbase never
    arrived in the pool wallet (orphaned), and -- when multiple credits
    exist at the same height -- distribute the actually-received amount
    among them by share ratio. Idempotent.

    Background: when two miners both find full-block nonces on the same
    template, both submissions are accepted by the daemon initially.
    settle_round runs for each, crediting each submitter the full block
    reward against THEIR accumulated shares (post-reset). Only one of
    those blocks is canonical; the other orphans. Pool wallet receives
    ONE coinbase but pool-credits.json has TWO credit entries at full
    reward. Earlier dedupe keyed by (height, wallet) missed this because
    the wallets are different.

    Fix: dedupe by height. If multiple credits exist at a height where
    the wallet received a coinbase, distribute the actual amount among
    them by share ratio (same fairness model as the normal proportional
    payout). Lost-race miners still get a fair slice of the actual win
    in proportion to the work they did."""
    try:
        ins = wallet_rpc("get_transfers", {"in": True, "pool": False}, timeout=15)
    except Exception as e:
        print(f"[pool] reconcile: get_transfers failed: {e}", file=sys.stderr)
        return
    arrived = {int(t.get("height", 0)): int(t.get("amount", 0))
               for t in (ins.get("in") or [])}
    try:
        wh_resp = wallet_rpc("get_height", timeout=8)
        wallet_h = int(wh_resp.get("height", 0))
    except Exception:
        wallet_h = 0
    credits = load_json_file(CREDITS_FILE, [])

    # Bucket by height
    by_height = collections.OrderedDict()
    for c in credits:
        h = int(c.get("height", 0))
        by_height.setdefault(h, []).append(c)

    kept = []
    dropped = []
    rescaled = 0
    for h, group in by_height.items():
        too_recent = (wallet_h - h) < RECONCILE_SAFETY_AGE
        if h in arrived:
            actual = arrived[h]
            if len(group) == 1:
                # Single credit at this height -- snap to actual.
                if group[0].get("atomic") != actual:
                    group[0]["atomic"] = actual
                kept.append(group[0])
            else:
                # Multiple credits at same height -- orphan race.
                # Distribute the actual coinbase by share ratio.
                total_shares = sum(int(c.get("shares", 0)) for c in group)
                if total_shares > 0:
                    assigned = 0
                    for c in group[:-1]:
                        c["atomic"] = int(actual * int(c.get("shares", 0)) / total_shares)
                        assigned += c["atomic"]
                        if c["atomic"] > 0:
                            kept.append(c)
                        else:
                            dropped.append(c)
                    # Last entry gets the remainder so rounding doesn't lose dust.
                    last = group[-1]
                    last["atomic"] = max(0, actual - assigned)
                    if last["atomic"] > 0:
                        kept.append(last)
                    else:
                        dropped.append(last)
                else:
                    # No share data -- keep one, drop the rest.
                    group[0]["atomic"] = actual
                    kept.append(group[0])
                    for c in group[1:]:
                        dropped.append(c)
                rescaled += 1
        elif too_recent:
            # Block hasn't matured in wallet yet -- don't touch.
            kept.extend(group)
        else:
            # No matching wallet incoming, not recent -- all entries are
            # phantoms (orphan or failed submission).
            dropped.extend(group)
    if dropped or rescaled:
        for d in dropped:
            print(f"[pool] reconcile: dropping phantom credit height={d.get('height')} "
                  f"amount={d.get('atomic', 0)/1e12:.4f} GLAC wallet={d.get('wallet','')[:12]}...",
                  file=sys.stderr)
        if rescaled:
            print(f"[pool] reconcile: distributed actual reward at {rescaled} "
                  f"orphan-race height(s) by share ratio",
                  file=sys.stderr)
        save_json_atomic(CREDITS_FILE, kept)

    # Same housekeeping for the public block list (cosmetic). One row per
    # canonical block; if we'd recorded an orphan-race loser, drop it.
    blocks = load_json_file(BLOCKS_FILE, [])
    bkept = []
    bseen = set()
    for b in blocks:
        h = int(b.get("height", 0))
        too_recent = (wallet_h - h) < RECONCILE_SAFETY_AGE
        if (h in arrived and h not in bseen) or too_recent:
            if h in arrived:
                b["reward_atomic"] = arrived[h]
            bkept.append(b)
            bseen.add(h)
    if len(bkept) != len(blocks):
        save_json_atomic(BLOCKS_FILE, bkept)


def reconcile_loop():
    while True:
        time.sleep(RECONCILE_INTERVAL_S)
        try:
            reconcile_credits_once()
        except Exception as e:
            print(f"[pool] reconcile_loop error: {e}", file=sys.stderr)


def maturity_snapshot():
    """Return a copy of the maturity cache, plus a `refreshed_s_ago`
    derived field. Returns None if we have never gotten a successful
    poll back from the wallet (caller should hide the maturity UI)."""
    with _maturity_lock:
        if _maturity_cache["ts"] <= 0:
            return None
        snap = dict(_maturity_cache)
    snap["refreshed_s_ago"] = int(time.time() - snap["ts"])
    snap.pop("ts", None)
    return snap


# ---- entry point ---------------------------------------------------------

def main():
    if POOL_WALLET.startswith("POOL_WALLET_NOT_CONFIGURED"):
        print("[pool] WARNING: POOL_WALLET env var not set. Blocks the "
              "pool finds will fail to submit (daemon won't accept a "
              "block addressed to a placeholder).", file=sys.stderr)

    # v1.1.13: prime the orphan-double-credit guard from existing
    # credits + blocks files. Any height we've previously written a
    # credit for must NOT be credited again after restart -- otherwise
    # a daemon resubmit (which can happen if a miner retries) would
    # create a phantom credit identical to the one we just spent
    # restart effort to make sure didn't happen.
    try:
        existing_credits = load_json_file(CREDITS_FILE, [])
        existing_blocks  = load_json_file(BLOCKS_FILE, [])
        for c in existing_credits:
            _settled_heights.add(int(c.get("height", 0)))
        for b in existing_blocks:
            _settled_heights.add(int(b.get("height", 0)))
        print(f"[pool] settled-height guard primed with "
              f"{len(_settled_heights)} historic heights")
    except Exception as e:
        print(f"[pool] WARNING: could not prime settled-height guard: {e}",
              file=sys.stderr)

    # Prime the job once before opening the listener so the first
    # /pool/job request doesn't 503.
    refresh_job()

    # v1.1.13: prime the arrived-heights cache synchronously so the very
    # first /pool/miner request applies the pending <= wallet invariant
    # instead of falling back to "count all credits".
    try:
        maturity_refresh_once()
        with _arrived_heights_lock:
            n_arrived = len(_arrived_heights)
        print(f"[pool] arrived-heights cache primed with {n_arrived} entries")
    except Exception as e:
        print(f"[pool] WARNING: could not prime arrived-heights cache: {e}",
              file=sys.stderr)

    refresher = threading.Thread(target=refresh_loop, name="refresh", daemon=True)
    refresher.start()

    payouter = threading.Thread(target=payout_loop, name="payout", daemon=True)
    payouter.start()

    sweeper = threading.Thread(target=cold_sweep_loop, name="cold-sweep", daemon=True)
    sweeper.start()

    maturity = threading.Thread(target=maturity_loop, name="maturity", daemon=True)
    maturity.start()

    reconciler = threading.Thread(target=reconcile_loop, name="reconcile", daemon=True)
    reconciler.start()

    server = ThreadedHTTPServer(("0.0.0.0", POOL_PORT), PoolHandler)
    print(f"[pool] listening on 0.0.0.0:{POOL_PORT}")
    print(f"[pool] daemon RPC: {DAEMON_RPC}")
    print(f"[pool] wallet RPC: {WALLET_RPC_URL}")
    print(f"[pool] pool wallet: {POOL_WALLET[:12]}...")
    print(f"[pool] share diff divisor: {SHARE_DIFF_DIVISOR}")
    print(f"[pool] fee: {POOL_FEE_PERCENT}%")
    print(f"[pool] min payout: {MIN_PAYOUT_ATOMIC/1e12:.4f} GLAC, "
          f"payout interval: {PAYOUT_INTERVAL_S}s")
    print(f"[pool] verify lib: {VERIFY_LIB_PATH} "
          f"({'LOADED' if _lattice else 'NOT LOADED -- trust mode'})")
    if COLD_ADDRESS:
        print(f"[pool] cold sweep: every {COLD_SWEEP_INTERVAL_S}s, "
              f"buffer={COLD_HOT_BUFFER_ATOMIC/1e12:.0f} GLAC, "
              f"-> {COLD_ADDRESS[:12]}...")
    else:
        print(f"[pool] cold sweep: DISABLED (set COLD_ADDRESS env var to enable)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[pool] shutting down")


if __name__ == "__main__":
    main()
