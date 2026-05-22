#!/usr/bin/env python3
"""Faucet UTXO split -- runs once.

Why: the faucet wallet currently has its entire balance in one UTXO.
After every payout, the change becomes a new locked output and the
unlocked balance hits 0 until 10 blocks confirm. That's a ~20-min outage
after each payout from the user's perspective.

Fix: split into multiple outputs across subaddresses up front. After the
split, individual payouts only lock one small output at a time; the
anchor output and the other subs stay unlocked.

Strategy:
  - Reserve ~800 GLAC at the main address (the anchor; keeps max_reward
    calc seeing a healthy unlocked balance during lock windows).
  - Split a smaller chunk (~300 GLAC) across 5 fresh subaddresses (60
    each). These are the "service" outputs that absorb payout locks.

After the split lands and confirms, total unlocked stays ~1099 even
during single payouts -- which means max_reward stays at 1.0 GLAC and
the faucet feels continuously alive.
"""
import json, sys, time, urllib.request

WALLET = "http://127.0.0.1:28083/json_rpc"
N           = 5     # number of new service subaddresses
CHUNK_GLAC  = 60    # GLAC per service subaddress (300 total split)
# Don't pin a hard reserve -- whatever's left after sending 300 GLAC to subs
# (minus tx fee) goes back to main as change. With ~1099 unlocked, that's
# ~799 GLAC anchor. Threshold just needs to be enough to do the split + fee.
THRESHOLD   = (N * CHUNK_GLAC) + 5  # 305 GLAC -- generous fee headroom

ATOMIC = 10**12


def wallet(method, params=None):
    body = json.dumps({
        "jsonrpc": "2.0", "id": "0",
        "method": method, "params": params or {}
    }).encode()
    req = urllib.request.Request(
        WALLET, data=body, headers={"Content-Type": "application/json"})
    j = json.loads(urllib.request.urlopen(req, timeout=20).read())
    if "error" in j:
        raise RuntimeError(f"wallet error: {j['error']}")
    return j.get("result", {})


def main():
    print(f"[split] will split when unlocked >= {THRESHOLD} GLAC (sending {N} x {CHUNK_GLAC} = {N*CHUNK_GLAC} GLAC to subs, change returns to main as anchor)")
    sys.stdout.flush()

    # 1. Wait until enough unlocked to safely split.
    while True:
        try:
            b = wallet("get_balance", {"account_index": 0})
        except Exception as e:
            print(f"[split] balance poll error: {e}")
            sys.stdout.flush()
            time.sleep(30)
            continue
        total    = b.get("balance", 0) / ATOMIC
        unlocked = b.get("unlocked_balance", 0) / ATOMIC
        blocks   = b.get("blocks_to_unlock", 0)
        print(f"[split] total={total:.3f} unlocked={unlocked:.3f} blocks_to_unlock={blocks}")
        sys.stdout.flush()
        if unlocked >= THRESHOLD:
            break
        time.sleep(30)

    # 2. Create N service subaddresses.
    print(f"[split] creating {N} service subaddresses...")
    sys.stdout.flush()
    subs = []
    for i in range(N):
        a = wallet("create_address", {
            "account_index": 0,
            "label": f"faucet-split-{i+1}",
        })
        addr = a["address"]
        subs.append(addr)
        print(f"[split]   sub {i+1}: {addr[:24]}... idx={a.get('address_index')}")
        sys.stdout.flush()

    # 3. Send the N-way transfer (change automatically returns to main).
    print(f"[split] sending {CHUNK_GLAC} GLAC x {N} subaddresses...")
    sys.stdout.flush()
    destinations = [
        {"address": s, "amount": int(CHUNK_GLAC * ATOMIC)}
        for s in subs
    ]
    tx = wallet("transfer_split", {
        "destinations": destinations,
        "account_index": 0,
        "subaddr_indices": [0],   # spend from the main address
        "priority": 1,
        "get_tx_keys": False,
    })
    hashes = tx.get("tx_hash_list", [])
    fees   = tx.get("fee_list", [])
    print(f"[split] OK -- {len(hashes)} tx(s):")
    for h, f in zip(hashes, fees):
        print(f"[split]   tx={h[:12]} fee={f/ATOMIC:.6f} GLAC")
    sys.stdout.flush()
    print(f"[split] DONE. After ~10 blocks (~20 min), faucet has {N+1} unlocked outputs.")


if __name__ == "__main__":
    main()
