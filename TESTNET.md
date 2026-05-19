# Glaciem public testnet

How to run the Glaciem testnet so the miner apps (Mac / Android / Windows) can
connect over the internet and prove the coin works end-to-end.

## What is running now

The seed node runs on the Mac Mini as a launchd service:

- **Service:** `org.rime.testnet-node` (`~/Library/LaunchAgents/org.rime.testnet-node.plist`)
- **Binary:** `~/rime/build/release/bin/rimed`
- **Data dir:** `~/rime-testnet` (persistent — survives reboot)
- **Chain:** fresh, real difficulty (no `--fixed-difficulty`)
- **RPC:** `0.0.0.0:29081` — the miner apps connect here
- **P2P:** disabled (`--offline`). A single node with P2P enabled never reports
  itself "synchronized" and refuses `get_block_template` ("Core is busy"), so
  for a one-node testnet `--offline` is required. P2P is only needed once a
  second node exists (Model B) — see the notes at the bottom.
- Auto-restarts on crash (`KeepAlive`) and at login (`RunAtLoad`).

Manage it:

```sh
launchctl list | grep rime                       # status (exit code 0 = ok)
launchctl unload ~/Library/LaunchAgents/org.rime.testnet-node.plist   # stop
launchctl load -w ~/Library/LaunchAgents/org.rime.testnet-node.plist  # start
tail -f ~/rime-testnet/node.err.log       # logs
```

Check the chain:

```sh
curl -s http://127.0.0.1:29081/json_rpc \
  -d '{"jsonrpc":"2.0","id":"0","method":"get_info"}' \
  -H 'Content-Type: application/json'
```

## To make it reachable from the internet — your homework

1. **Port-forward** on your home router, TCP, to the Mac Mini's LAN IP
   (`192.168.0.65` — set a DHCP reservation so it doesn't change):
   - `29081` → RPC — this is all that's needed for the apps to mine.
   - `29080` (P2P) is **not** needed yet — the node runs `--offline`. Forward
     it only when you move to a multi-node setup (Model B).

2. **Dynamic DNS** — your home public IP can change. Sign up for a free
   DuckDNS / No-IP hostname pointed at your IP, so the apps have a stable
   address. Current public IP: `107.171.129.138` (may change).

3. **Check for CGNAT** — if your router's WAN IP does not match what a
   "what is my IP" site shows, your ISP is using carrier-grade NAT and
   port-forwarding will not work. A VPN/tunnel would be needed instead.

4. **Keep the Mac awake & logged in** — this is a LaunchAgent (runs in your
   login session). In System Settings → Displays/Battery, disable sleep, or
   run `caffeinate -s` in a terminal.

## How a tester connects an app

Each app does JSON-RPC to the node's RPC port. Point it at the node:

- **Mac app** — on the Mac Mini itself it already uses `127.0.0.1`; no change.
- **Android app** — tap **HOST**, set the node host to your DDNS name (or
  public IP) and port `29081`.
- **Windows app** — put your DDNS name / public IP in a `rime_host.txt`
  next to the `.exe`, or pass it as a command-line argument.

The app then pulls block templates, mines on the device's CPU/GPU, and
submits blocks to the live chain.

## Notes & limits

- **RPC is unrestricted.** Anyone who reaches `:29081` can call admin methods
  such as `stop_daemon`. For a testnet this is low-stakes, and launchd
  restarts the node within seconds. Do not reuse this setup for mainnet.
- **Wallet is per-user.** `rime-wallet-rpc` holds spend keys and must never
  be exposed to the internet. Each tester runs their own wallet locally, or
  waits for the v2 in-app wallet. The miner works without a wallet (it falls
  back to a built-in mining address).
- **This is Model A** — a single public node that apps mine against. A true
  multi-node P2P testnet (Model B) needs `rimed` built for Windows/Linux so
  each tester runs their own node; that is a separate task.
- **Testnet only.** Lattice's parameters are not cryptographically reviewed —
  that review is the gate before any mainnet launch.
