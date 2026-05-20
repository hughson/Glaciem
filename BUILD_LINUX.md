# Glaciem on Linux — build & mine from source

Glaciem doesn't ship a prebuilt Linux binary yet. The daemon and the CLI wallet
build cleanly on Ubuntu / Debian in ~15 minutes, so until there's a public
binary release, this is the path.

What you'll end up with:

| Binary             | What it does                                                |
| ------------------ | ----------------------------------------------------------- |
| `rimed`            | The Glaciem daemon — syncs the chain, mines, serves the RPC |
| `rime-wallet-cli`  | The CLI wallet — generate an address, send, sweep, history  |

The `rime-` prefix is a holdover from the project's previous name — the
binaries themselves are the right ones; they'll be renamed in a future build.

## 1. Install build dependencies

Tested on Ubuntu 22.04 / 24.04 and Debian 12. ~3 GB of dev libraries.

```sh
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git ca-certificates \
    libssl-dev libzmq3-dev libsodium-dev libunbound-dev libnorm-dev libpgm-dev \
    libhidapi-dev libusb-1.0-0-dev libudev-dev libreadline-dev \
    libboost-chrono-dev libboost-container-dev libboost-date-time-dev \
    libboost-filesystem-dev libboost-locale-dev libboost-program-options-dev \
    libboost-regex-dev libboost-serialization-dev libboost-system-dev \
    libboost-thread-dev
```

## 2. Clone and build

```sh
git clone --recursive https://github.com/hughson/Glaciem.git
cd Glaciem
make release -j$(nproc)
```

`--recursive` matters — the build pulls in submodules (RandomX, easylogging,
miniupnpc, etc.) and the CMake config will refuse to start without them.

Build time is roughly 10-20 min depending on CPU and core count. The binaries
land in `build/Linux/main/release/bin/` (the per-arch / per-branch path Monero's
Makefile uses for git clones).

> On low-memory VPS hosts (≤ 4 GB RAM), the C++ compile can OOM-kill. Either
> drop `-j$(nproc)` to `-j1`, or add a swapfile:
> ```sh
> sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile
> sudo mkswap /swapfile && sudo swapon /swapfile
> echo "/swapfile none swap sw 0 0" | sudo tee -a /etc/fstab
> ```

## 3. Run the daemon

```sh
./build/Linux/main/release/bin/rimed --data-dir ~/.glaciem
```

The daemon picks up public peers automatically and starts syncing. You can run
it in the background (`tmux`, `nohup`, or a systemd unit) — it'll keep syncing
until you stop it.

The default Monero data dir is `~/.bitmonero/`, so passing `--data-dir
~/.glaciem` keeps Glaciem's chain in its own directory and avoids any conflict
if you also run Monero on the same host.

## 4. Open a wallet and mine

In another terminal:

```sh
./build/Linux/main/release/bin/rime-wallet-cli --daemon-address 127.0.0.1:19081
```

`--daemon-address` points at your local daemon's RPC port (Glaciem uses 19081,
not Monero's 18081). The wallet prompts for:

- A wallet file path — pick a new name; the wallet creates this file.
- A password.
- A language for the 25-word recovery seed.

**Write the seed down.** It is the only way to recover this wallet, and it is
never stored anywhere else. Anyone with the seed can spend your coins.

Once the wallet shows the `[wallet ...]>` prompt, start mining to your own
address with:

```
start_mining 4
```

`4` is the number of CPU threads to dedicate to mining. Use as many as you
want; the daemon does the hashing — the wallet just tells it what address to
mine rewards to. The wallet must remain unlocked for the daemon to know the
mining address.

To stop mining: `stop_mining`. To check progress: `balance`, `refresh`, or
`mining_status` in the wallet, or `status` in the daemon.

## Where things live

- `~/.glaciem/` — chain data, peer list, daemon log. Glaciem just launched, so
  the chain is tiny today; expect this to grow with use (Monero's chain after
  several years is tens of GB).
- Wherever you put the wallet file — keys (`.keys`), cache, and address file
  end up next to it.
- `build/Linux/main/release/bin/` — the compiled binaries.

## Troubleshooting

- **`Submodule not found` during cmake** — you didn't pass `--recursive` to
  `git clone`. Fix:
  ```sh
  git submodule update --init --recursive
  ```
- **`port 19080 already in use`** — another `rimed` is running. Stop it or
  pass `--p2p-bind-port 19082` to this one.
- **Daemon never connects to peers** — usually a firewall on port 19080/tcp.
  Open it, or add `--add-priority-node 178.105.142.34:19080` to talk to a
  known good peer.
- **Mining produces no blocks** — Glaciem's network difficulty is real; a
  single 4-thread CPU will find a block every few hours at most.

## Reporting issues

If the build fails, open an issue at
[github.com/hughson/Glaciem/issues](https://github.com/hughson/Glaciem/issues)
with the failing command, the error tail, and `uname -a` + your distro version.
