/*
 * rime_wallet.h -- a small C ABI over Rime's real wallet (Monero
 * wallet2 / wallet_api). Lets the miner apps open the wallet for a generated
 * 25-word seed, scan the chain, show balance, and send -- so the address you
 * generate IS the app's wallet.
 *
 * Shared by the macOS, Android and Windows apps. Backed by libwallet_api
 * (src/wallet/api) built from the Rime fork.
 */
#ifndef RIME_WALLET_H
#define RIME_WALLET_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RimeWallet RimeWallet;   /* opaque handle */

/* Recover (or open, if it already exists) the testnet wallet for `seed` at
   `path`, and point it at `daemon_address` (e.g. "127.0.0.1:29081").
   restore_height 0 scans the whole chain. Returns NULL on failure. */
RimeWallet *rime_wallet_recover(const char *path, const char *seed,
                                    const char *daemon_address,
                                    unsigned long long restore_height);

/* Open an existing wallet cache file at `path`. Returns NULL on failure. */
RimeWallet *rime_wallet_open(const char *path, const char *password,
                                 const char *daemon_address);

void rime_wallet_close(RimeWallet *w);

/* Re-point an already-open wallet at a different daemon. Keys, balance, and
   history are preserved; only the underlying HTTP connection swaps. Used by
   the apps for daemon failover (e.g. when the Cloudflare Worker is
   unreachable). Returns 1 on success, 0 on failure. */
int rime_wallet_set_daemon(RimeWallet *w, const char *daemon_address);

/* Blocking chain scan -- call from a background thread. Returns 1 on success. */
int rime_wallet_refresh(RimeWallet *w);

/* Blocking rescan from genesis -- drops the cached chain and re-scans against
   the current daemon. Needed when the wallet is stranded on a dead fork (e.g.
   a testnet was reset): plain refresh() will not roll back. Returns 1 on
   success. Call from a background thread. */
int rime_wallet_rescan(RimeWallet *w);

/* Persist the wallet cache (scanned balance/outputs) to disk so it survives a
   relaunch. Call periodically, e.g. after each refresh. */
void rime_wallet_store(RimeWallet *w);

int                rime_wallet_connected(RimeWallet *w);          /* 1 = daemon reachable */
int                rime_wallet_synchronized(RimeWallet *w);       /* 1 = fully scanned    */
unsigned long long rime_wallet_balance(RimeWallet *w);            /* atomic units         */
unsigned long long rime_wallet_unlocked_balance(RimeWallet *w);   /* atomic units         */
unsigned long long rime_wallet_height(RimeWallet *w);             /* wallet scanned height*/
unsigned long long rime_wallet_daemon_height(RimeWallet *w);      /* node chain tip       */

/* Copy the wallet's primary address / 25-word seed into out (NUL-terminated). */
void rime_wallet_address(RimeWallet *w, char *out, int cap);
void rime_wallet_seed(RimeWallet *w, char *out, int cap);

/* Send `amount_atomic` to `address`. Writes a human-readable result line into
   `result`. Returns 1 on success, 0 on failure. */
int rime_wallet_send(RimeWallet *w, const char *address,
                       unsigned long long amount_atomic,
                       char *result, int result_cap);

/* Sweep "unmixable" outputs into normal mixable ones. On a young chain almost
   all mined (coinbase) rewards are unmixable -- they have cleartext amounts and
   there are not yet enough same-denomination outputs on-chain to form a ring,
   so they cannot be spent normally ("not enough outputs for ring size"). This
   sweeps them (ring size 1, allowed for provably-unmixable outputs) into
   ordinary RingCT outputs that CAN be spent. Writes a human-readable result
   line into `result`. Returns 1 on success, 0 on failure / nothing to sweep. */
int rime_wallet_sweep_unmixable(RimeWallet *w, char *result, int result_cap);

/* Write the recent non-coinbase transaction history (newest first) into `out`,
   one transaction per line: "<in|out>  <±amount> RME  <status>  <date>".
   Coinbase (mining-reward) entries are omitted -- on a mining wallet there are
   far too many to be useful; this shows sends, sweeps and received transfers.
   `out` should be sizeable (>= 8 KiB) -- up to 50 entries are written. */
void rime_wallet_history(RimeWallet *w, char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
