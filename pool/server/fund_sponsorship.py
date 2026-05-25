#!/usr/bin/env python3
"""
fund_sponsorship.py -- activate (or update) the pool's growth-fund
sponsorship.

This script ONLY writes the sponsorship state file. The operator must
have ALREADY transferred the GLAC amount into the pool wallet before
activating, otherwise payouts will be insolvent (the pool will try to
distribute bonuses against funds it doesn't have).

Once activated, the pool's settle_round() consumes one per-block bonus
each time it credits a block. When the fund drains, the state flips
active=false and rounds revert to normal block reward only.

Usage:
    python3 fund_sponsorship.py <total_glac> <blocks_window> [label]

Example (25 K GLAC over ~10 K blocks, 2 weeks at 120 s target):
    python3 fund_sponsorship.py 25000 10000 "Growth fund - May 2026"

Outputs the resulting state file and a summary.
"""
import json
import os
import sys
import time


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(1 if len(argv) < 3 else 0)

    try:
        total_glac = float(argv[1])
    except ValueError:
        print(f"error: total_glac must be a number, got {argv[1]!r}")
        sys.exit(1)
    try:
        blocks = int(argv[2])
    except ValueError:
        print(f"error: blocks_window must be an integer, got {argv[2]!r}")
        sys.exit(1)
    if total_glac <= 0 or blocks <= 0:
        print("error: total_glac and blocks_window must both be > 0")
        sys.exit(1)
    label = argv[3] if len(argv) > 3 else "Growth fund"

    total_atomic = int(total_glac * 1e12)
    per_block_atomic = total_atomic // blocks

    state = {
        "active":           True,
        "label":            label,
        "total_atomic":     total_atomic,
        "remaining_atomic": total_atomic,
        "per_block_atomic": per_block_atomic,
        "blocks_seeded":    blocks,
        "blocks_used":      0,
        "started_at":       int(time.time()),
    }

    # Resolve the file path the pool server uses. Pool's default is the
    # working directory of pool-server.py. On glaciem-node that's /root/.
    path = os.environ.get(
        "SPONSORSHIP_FILE",
        os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "pool-sponsorship.json"))

    if os.path.exists(path):
        try:
            old = json.load(open(path))
            print("WARNING: existing sponsorship state will be replaced:")
            print(json.dumps(old, indent=2))
            print()
        except Exception:
            pass

    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, path)

    print(f"Wrote {path}:")
    print(json.dumps(state, indent=2))
    print()
    print(f"  per-block bonus : {per_block_atomic/1e12:.6f} GLAC")
    print(f"  total seeded    : {total_atomic/1e12:.2f} GLAC")
    print(f"  block window    : {blocks}")
    print(f"  approx window   : ~{blocks * 120 / 86400:.1f} days at 120 s target")
    print()
    print("IMPORTANT: ensure the pool wallet has AT LEAST this many GLAC")
    print("(pre-transferred from the operator's mining wallet) BEFORE the")
    print("pool starts paying out bonuses. Otherwise payouts will be")
    print("insolvent and the pending<=wallet invariant will break.")
    print()
    print("To deactivate later: delete the file or set 'active': false.")


if __name__ == "__main__":
    main(sys.argv)
