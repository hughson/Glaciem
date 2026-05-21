/*
 * peer_cache.h -- portable C peer cache for the miner apps.
 *
 * Manages a small in-memory list of known Glaciem RPC endpoints (seeds +
 * discovered peers). Used by every miner app for both miner-RPC and wallet-RPC
 * failover. Persistence is per-platform -- the caller hands us a load/save
 * callback that reads/writes a single JSON blob to whatever storage the
 * platform uses (NSUserDefaults, SharedPreferences, a file, etc.).
 *
 * Thread-safe: all functions take an internal lock.
 */
#ifndef GLACIEM_PEER_CACHE_H
#define GLACIEM_PEER_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PeerCache PeerCache;

/* One endpoint we know about. `use_ssl` is 1 for the Cloudflare worker, 0 for
   direct nodes; `is_seed` peers are permanent and never dropped. */
typedef struct {
  char     host[128];
  int      port;
  int      use_ssl;
  int      is_seed;
  long long last_ok;           /* unix seconds; 0 = never */
  int      consec_failures;
  int      avg_latency_ms;     /* exponential moving average */
} PeerEntry;

/* Persistence callbacks. `load` is called once at peer_cache_new() and must
   either fill `out` with the previously-saved JSON (NUL-terminated, return
   1) or return 0 if nothing is saved yet. `save` is called whenever the
   cache mutates. Both may be NULL for an in-memory-only cache. */
typedef int  (*peer_cache_load_fn)(char *out, int cap, void *ctx);
typedef void (*peer_cache_save_fn)(const char *json, void *ctx);

/* Create a cache. Seeds are baked in by peer_cache_add_seed() afterwards. */
PeerCache *peer_cache_new(peer_cache_load_fn load, peer_cache_save_fn save, void *ctx);
void       peer_cache_free(PeerCache *);

/* Add a permanent seed. Seeds are tried before discovered peers and never
   dropped on failure. Idempotent. */
void peer_cache_add_seed(PeerCache *, const char *host, int port, int use_ssl);

/* Add a discovered peer (from get_peer_list). Idempotent; no-op if already
   present (seed or discovered). use_ssl defaults to 0 for discovered peers. */
void peer_cache_add_discovered(PeerCache *, const char *host, int port);

/* Update score after an RPC attempt. */
void peer_cache_mark_success(PeerCache *, const char *host, int port,
                              int latency_ms);
void peer_cache_mark_failure(PeerCache *, const char *host, int port);

/* Snapshot the cache into `out` in attempt order:
     1. seeds first, in insertion order
     2. then discovered peers sorted by score (consec_failures asc,
        last_ok desc, avg_latency_ms asc).
   Returns the number of entries actually written (<= cap). */
int peer_cache_snapshot(PeerCache *, PeerEntry *out, int cap);

/* Decrement the until-next-discovery counter; returns 1 if a get_peer_list
   call should be issued now, else 0. After issuing, call
   peer_cache_reset_discovery_counter(). */
int peer_cache_should_discover(PeerCache *);
void peer_cache_reset_discovery_counter(PeerCache *);

#ifdef __cplusplus
}
#endif

#endif /* GLACIEM_PEER_CACHE_H */
