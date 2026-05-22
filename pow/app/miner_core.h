/*
 * miner_core.h -- Lattice miner core for Rime Miner.app.
 *
 * Connects to a local rimed over JSON-RPC: pulls block templates, mines them
 * on the CPU with the Lattice PoW, and submits found blocks back to the daemon.
 */
#ifndef RIME_MINER_CORE_H
#define RIME_MINER_CORE_H

#include <stdint.h>

typedef struct {
  int      running;          /* 1 while the worker thread is mining          */
  int      daemon_connected; /* 1 if the last get_block_template succeeded    */
  double   hashrate;         /* recent hashes/sec (smoothed)                  */
  uint64_t total_hashes;     /* hashes since start                            */
  uint64_t height;           /* current testnet height (from the template)   */
  uint64_t difficulty;       /* current block difficulty                      */
  uint64_t blocks_found;     /* blocks this miner has submitted and got OK    */
  int      best_bits;        /* best leading-zero bits seen                   */
  double   uptime_s;         /* seconds since miner_start()                   */
  int      wallet_connected; /* 1 if rime-wallet-rpc is reachable           */
  uint64_t balance;          /* wallet balance, atomic units                  */
  uint64_t unlocked_balance; /* spendable balance, atomic units               */
  int      wallet_syncing;   /* 1 while the wallet is still scanning the chain */
  uint64_t wallet_height;    /* block height the wallet has scanned to         */
  uint64_t target_height;    /* node blockchain height -- the sync target      */
  int      no_address;       /* 1 if mining is blocked: no mining address set  */
} MinerStats;

#ifdef __cplusplus
extern "C" {
#endif

void        miner_start(void);        /* begin mining on a background thread */
void        miner_stop(void);         /* stop and join the worker            */
MinerStats  miner_get_stats(void);    /* snapshot of current stats           */
const char *miner_device(void);       /* GPU device name (e.g. "Apple M4")   */
const char *miner_last_hash(void);    /* hex of the most recent PoW hash     */
const char *miner_wallet_address(void); /* wallet primary address, "" if none */
const char *miner_send(const char *address, double amount); /* transfer; returns a result line */
const char *miner_sweep_unmixable(void); /* make mined coins spendable; returns a result line */
const char *miner_history(void); /* recent sends/sweeps/receives, newest first, multi-line */

/* set the rimed node this app connects to (host + RPC port; UI persists it) */
void        miner_set_node(const char *host, int port);

/* v1.1.6: configure pool mode.
 *   enabled = 1 -> fetch jobs from POST {url}/pool/job and submit shares
 *                  to POST {url}/pool/submit instead of talking to a
 *                  daemon directly. Block rewards go to the pool wallet;
 *                  the pool credits miners proportionally to share count
 *                  and auto-pays via the pool's wallet-rpc.
 *   enabled = 0 -> classic solo mining via the configured daemon (see
 *                  miner_set_node above). This is the original behavior.
 * `url` is the pool's base URL (e.g. "https://glaciem-pool.frostmine.workers.dev").
 * Safe to call at any time; takes effect on the next mining-loop iteration. */
void        miner_set_pool_config(int enabled, const char *url);

/* Open the app's embedded wallet at `path`. `seed` non-empty recovers a wallet
   for that 25-word mnemonic (replacing any existing file); `seed` empty/NULL
   opens the existing wallet file. Runs on a background thread -- the scan can
   take a while. The miner mines coinbase rewards to this wallet's address. */
void        miner_open_wallet(const char *path, const char *seed);

/* generate a fresh Rime wallet; fills addr_out and seed_out (the 25-word
   mnemonic). Returns 1 on success, 0 on failure. */
int         miner_generate_address(char *addr_out, int addr_cap,
                                   char *seed_out, int seed_cap);

#ifdef __cplusplus
}
#endif

#endif
