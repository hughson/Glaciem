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
import http.server
import json
import os
import os.path
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
CREDITS_FILE          = os.environ.get(
    "CREDITS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-credits.json"))
BLOCKS_FILE           = os.environ.get(
    "BLOCKS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-blocks.json"))
PAYOUTS_FILE          = os.environ.get(
    "PAYOUTS_FILE",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "pool-payouts.json"))

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


# ---- in-memory state (protected by STATE_LOCK) ----------------------------

STATE_LOCK = threading.Lock()

# Latest job we got from the daemon. Refreshed by refresh_job() on a
# timer; also re-fetched eagerly when a new block lands.
current_job = None

# OrderedDict: job_id -> { template metadata + submissions set }
recent_jobs = collections.OrderedDict()

# wallet -> dict of share counters + EWMA hashrate
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


def credit_share(wallet):
    """Record a share for the submitting wallet. Updates rolling stats."""
    rollover_if_needed()
    s = miner_stats.get(wallet)
    if s is None:
        s = {
            "shares_total": 0, "shares_today": 0, "shares_this_round": 0,
            "last_share_ts": 0.0, "hashrate_ewma": 0.0, "blocks_found": 0,
        }
        miner_stats[wallet] = s
    now = time.time()
    s["shares_total"]     += 1
    s["shares_today"]     += 1
    s["shares_this_round"] += 1
    if s["last_share_ts"] > 0 and current_job is not None:
        dt = max(0.5, now - s["last_share_ts"])
        inst = current_job["share_difficulty"] / dt
        s["hashrate_ewma"] = (0.7 * s["hashrate_ewma"] + 0.3 * inst
                              if s["hashrate_ewma"] > 0 else inst)
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


def settle_round(block):
    """Append credit entries per contributing miner. The payout helper
    reads this file, sums per wallet, and submits actual transfers from
    a separate machine. This VM never holds the spend key."""
    reward = block.get("reward_atomic", 0)
    if not reward:
        return
    pool_cut = int(reward * POOL_FEE_PERCENT / 100.0)
    distributable = reward - pool_cut
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
        if not isinstance(wallet, str) or not wallet:
            return self._send_json(400, {"error": "wallet required"})
        with STATE_LOCK:
            job = current_job
            if job is None:
                return self._send_json(503, {"error": "pool warming up; retry in a moment"})
            if wallet not in miner_stats:
                miner_stats[wallet] = {
                    "shares_total": 0, "shares_today": 0, "shares_this_round": 0,
                    "last_share_ts": 0.0, "hashrate_ewma": 0.0, "blocks_found": 0,
                }
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
        body = self._read_json()
        job_id = body.get("job_id")
        wallet = body.get("wallet")
        nonce  = body.get("nonce")
        if not job_id or not wallet or not isinstance(nonce, int):
            return self._send_json(400, {"error": "job_id, wallet, nonce required"})
        nonce &= 0xFFFFFFFF
        with STATE_LOCK:
            job = recent_jobs.get(job_id)
            if job is None:
                return self._send_json(200, {"accepted": False, "reason": "stale job"})
            dedupe_key = f"{wallet}:{nonce}"
            if dedupe_key in job["submissions"]:
                return self._send_json(200, {"accepted": False, "reason": "duplicate"})
            job["submissions"].add(dedupe_key)
            # v1 TRUST MODE: credit the share without verifying.
            credit_share(wallet)
            full_block = bool(body.get("full_block"))
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
            hashrate = sum(s["hashrate_ewma"] for s in miner_stats.values())
            payload = {
                "pool_wallet":          POOL_WALLET,
                "pool_fee_percent":     POOL_FEE_PERCENT,
                "share_diff_divisor":   SHARE_DIFF_DIVISOR,
                "hashrate":             int(round(hashrate)),
                "active_miners":        active,
                "shares_today":         pool_stats["shares_today"],
                "shares_total":         pool_stats["shares_total"],
                "blocks_today":         pool_stats["blocks_today"],
                "blocks_total":         pool_stats["blocks_total"],
                "current_round_shares": pool_stats["current_round_shares"],
                "last_block":           pool_stats["last_block"],
                "uptime_s":             int(now - pool_stats["started_at"]),
                "current_job": ({
                    "job_id":             current_job["job_id"],
                    "height":             current_job["height"],
                    "network_difficulty": current_job["network_difficulty"],
                    "share_difficulty":   current_job["share_difficulty"],
                    "age_s":              int(now - current_job["created_at"]),
                } if current_job else None),
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
                "hashrate":          int(round(s["hashrate_ewma"])),
                "shares_total":      s["shares_total"],
                "shares_today":      s["shares_today"],
                "shares_this_round": s["shares_this_round"],
                "last_share_s_ago":  (int(now - s["last_share_ts"])
                                      if s["last_share_ts"] > 0 else None),
                "blocks_found":      s["blocks_found"],
            }
        # Pending payout balance: read fresh (cheap, JSON file).
        pending = compute_pending_balances().get(wallet, 0)
        payload["pending_atomic"] = pending
        payload["pending_glac"]   = pending / 1e12
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
    while True:
        try:
            refresh_job()
        except Exception as e:
            print(f"[pool] refresh_loop error: {e}", file=sys.stderr)
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
    """Return {wallet: pending_atomic} based on credits minus payouts.
    Reads both files fresh each call so an external editor (sweep job,
    payout helper) can take effect without restart."""
    credits = load_json_file(CREDITS_FILE, [])
    payouts = load_json_file(PAYOUTS_FILE, [])
    earned = {}
    for c in credits:
        w = c.get("wallet")
        if not w:
            continue
        earned[w] = earned.get(w, 0) + int(c.get("atomic", 0))
    paid = {}
    for p in payouts:
        w = p.get("wallet")
        if not w:
            continue
        paid[w] = paid.get(w, 0) + int(p.get("atomic", 0))
    pending = {}
    for w, e in earned.items():
        net = e - paid.get(w, 0)
        if net > 0:
            pending[w] = net
    return pending


def payout_round():
    """One pass through pending credits. Sends any wallet that's over the
    payout threshold. Returns the number of successful transfers."""
    pending = compute_pending_balances()
    eligible = [(w, a) for w, a in pending.items() if a >= MIN_PAYOUT_ATOMIC]
    if not eligible:
        return 0

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


# ---- entry point ---------------------------------------------------------

def main():
    if POOL_WALLET.startswith("POOL_WALLET_NOT_CONFIGURED"):
        print("[pool] WARNING: POOL_WALLET env var not set. Blocks the "
              "pool finds will fail to submit (daemon won't accept a "
              "block addressed to a placeholder).", file=sys.stderr)

    # Prime the job once before opening the listener so the first
    # /pool/job request doesn't 503.
    refresh_job()

    refresher = threading.Thread(target=refresh_loop, name="refresh", daemon=True)
    refresher.start()

    payouter = threading.Thread(target=payout_loop, name="payout", daemon=True)
    payouter.start()

    server = ThreadedHTTPServer(("0.0.0.0", POOL_PORT), PoolHandler)
    print(f"[pool] listening on 0.0.0.0:{POOL_PORT}")
    print(f"[pool] daemon RPC: {DAEMON_RPC}")
    print(f"[pool] wallet RPC: {WALLET_RPC_URL}")
    print(f"[pool] pool wallet: {POOL_WALLET[:12]}...")
    print(f"[pool] share diff divisor: {SHARE_DIFF_DIVISOR}")
    print(f"[pool] fee: {POOL_FEE_PERCENT}%")
    print(f"[pool] min payout: {MIN_PAYOUT_ATOMIC/1e12:.4f} GLAC, "
          f"payout interval: {PAYOUT_INTERVAL_S}s")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[pool] shutting down")


if __name__ == "__main__":
    main()
