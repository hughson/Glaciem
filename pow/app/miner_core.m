/*
 * miner_core.m -- Lattice miner core for Rime Miner.app.
 *
 * Connects to a rimed node over JSON-RPC:
 *   - get_block_template -> a real block to mine
 *   - hashes nonces across CPU worker threads with the Lattice PoW
 *   - submit_block when a nonce's hash meets the difficulty target
 * so the app drives the live testnet's chain height. CPU-only.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/sysctl.h>

#include "miner_core.h"
#include "rime_keygen.h"        /* rime_generate_address */
#include "rime_wallet.h"        /* embedded wallet (Monero wallet_api) */
#include "peer_cache.h"         /* seed + discovered RPC endpoints */

typedef uint64_t u64;
typedef uint8_t  u8;

/* epoch dataset size -- must match pow/lattice_ref.c */
#define DATASET_WORDS (4*1024*1024/8)

/* Lattice PoW (CPU-only) -- compiled as a separate unit (pow/lattice_ref.c). */
extern void lattice_build_dataset(const u8 epoch_seed[32], u64 *ds);
extern void lattice_hash_ds(const u8 *in, size_t len, const u64 *ds, u8 out[32]);

#define BATCH 512   /* nonces hashed per parallel round */

static pthread_t       g_worker;
static volatile int    g_running = 0;
static id<NSObject>    g_activity = nil;   /* App Nap / idle-sleep assertion */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static MinerStats      g_stats;
static char            g_device[64]   = "-";
static char            g_lasthash[65] = "";
static char            g_wallet_addr[160] = "";   /* embedded wallet's address */
/* Wallet RPC goes through the Cloudflare proxy (same path as the miner). The
   Worker fails over between nodes, so a Hetzner traffic-scrub on one VM
   doesn't take the wallet offline. wallet2 connects over TLS on :443 via
   its e_ssl_support_autodetect default. */
static char            g_node_host[128]   = "glaciem-rpc.frostmine.workers.dev";
static int             g_node_port        = 443;
static char            g_wallet_path[1024]= "";   /* embedded wallet file path */
static RimeWallet   *g_wallet           = NULL; /* embedded wallet handle    */

/* v1.1.6: pool-mode state. When pool_enabled is set, the mining loop
 * fetches jobs from {pool_url}/pool/job and submits shares to
 * {pool_url}/pool/submit instead of talking to a daemon directly.
 * Wallet still mines to the user's payout address (carried in the
 * /pool/job request body); block rewards land in the POOL's wallet
 * and are paid out proportionally to share contribution. */
static volatile int    g_pool_enabled     = 0;
static char            g_pool_url[256]    = "https://glaciem-pool.frostmine.workers.dev";

/* v1.1.14+: thread-count picker. Init from activeProcessorCount on first use;
 * the Swift UI (RimeMiner.swift @AppStorage) calls miner_set_thread_count
 * after launch with the value the user picked last session. */
static volatile int    g_thread_count     = 0;   /* 0 = "not yet set, use recommended" */
static int             g_max_cores        = 0;   /* lazy-init from NSProcessInfo      */

static void ensure_cores_init(void) {
  if (g_max_cores > 0) return;
  int n = (int)[[NSProcessInfo processInfo] activeProcessorCount];
  if (n < 1) n = 1;
  g_max_cores = n;
  if (g_thread_count == 0) {
    int rec = (n + 1) / 2;
    if (rec < 1) rec = 1;
    g_thread_count = rec;
  }
}

static double now_s(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec + t.tv_nsec/1e9;
}
static int lz_bits(const u8 out[32]) {
  int n=0;
  for(int i=31;i>=0;i--){ if(out[i]==0){n+=8;continue;}
    for(int b=7;b>=0;b--){ if(out[i]&(1<<b)) return n; n++; } }
  return n;
}
/* hash (32B, little-endian 256-bit) meets target iff hash*difficulty < 2^256 */
static int meets_target(const u8 h[32], uint64_t difficulty) {
  if (difficulty <= 1) return 1;
  uint64_t w[4];
  for(int i=0;i<4;i++){ uint64_t x=0; for(int b=0;b<8;b++) x|=(uint64_t)h[i*8+b]<<(8*b); w[i]=x; }
  unsigned __int128 carry=0;
  for(int i=0;i<4;i++){ unsigned __int128 p=(unsigned __int128)w[i]*difficulty+carry; carry=p>>64; }
  return carry==0;
}
/* offset of the 4-byte nonce in a block (hashing or template) blob:
   3 leading varints (major, minor, timestamp) + 32-byte prev_id */
static int nonce_offset(const u8 *blob, int len) {
  int p=0;
  for(int v=0; v<3 && p<len; v++){ int n=1; while(p+n-1<len && (blob[p+n-1]&0x80)) n++; p+=n; }
  return p + 32;
}

/* ---- JSON-RPC to rimed ---- */
static NSDictionary *json_rpc_url(NSString *url, NSString *method, id params) {
  NSDictionary *body = @{@"jsonrpc":@"2.0",@"id":@"0",@"method":method,
                         @"params":(params ?: @{})};
  NSData *bd = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
  NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
  req.HTTPMethod = @"POST";
  req.HTTPBody = bd;
  req.timeoutInterval = 5;
  [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  __block NSData *out = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [[[NSURLSession sharedSession] dataTaskWithRequest:req
      completionHandler:^(NSData *d, NSURLResponse *r, NSError *e){
        out = d; dispatch_semaphore_signal(sem);
      }] resume];
  if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 6*NSEC_PER_SEC)))
    return nil;
  if (!out) return nil;
  id resp = [NSJSONSerialization JSONObjectWithData:out options:0 error:nil];
  return [resp isKindOfClass:NSDictionary.class] ? resp[@"result"] : nil;
}

/* v1.1.6 pool-mode HTTP helpers.
 *
 * Pool endpoints take a flat JSON body (not JSON-RPC), so we have a
 * dedicated http_post_json() that POSTs `body` to `url` and returns
 * the decoded NSDictionary. Used by the mining loop in pool mode. */
static NSDictionary *http_post_json(NSString *url, NSDictionary *body) {
  NSData *bd = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
  NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
  req.HTTPMethod = @"POST";
  req.HTTPBody = bd;
  req.timeoutInterval = 8;
  [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  __block NSData *out = nil;
  __block NSHTTPURLResponse *httpResp = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [[[NSURLSession sharedSession] dataTaskWithRequest:req
      completionHandler:^(NSData *d, NSURLResponse *r, NSError *e){
        out = d;
        if ([r isKindOfClass:NSHTTPURLResponse.class]) httpResp = (NSHTTPURLResponse *)r;
        dispatch_semaphore_signal(sem);
      }] resume];
  if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 9*NSEC_PER_SEC)))
    return nil;
  if (!out || httpResp.statusCode >= 400) return nil;
  id resp = [NSJSONSerialization JSONObjectWithData:out options:0 error:nil];
  return [resp isKindOfClass:NSDictionary.class] ? resp : nil;
}

/* Fetch a job from the pool. Returns a dict shaped like:
 *   { job_id, blockhashing_blob, blocktemplate_blob, seed_hash,
 *     height, network_difficulty, share_difficulty, target } */
static NSDictionary *pool_get_job(NSString *wallet) {
  char url_c[320]; char host_c[256];
  pthread_mutex_lock(&g_lock);
  strncpy(host_c, g_pool_url, sizeof(host_c)-1); host_c[sizeof(host_c)-1] = 0;
  pthread_mutex_unlock(&g_lock);
  snprintf(url_c, sizeof(url_c), "%s/pool/job", host_c);
  NSString *url = [NSString stringWithUTF8String:url_c];
  return http_post_json(url, @{@"wallet": wallet ?: @""});
}

/* Submit a share or full block to the pool. `is_full_block = 1` tells
 * the pool the nonce ALSO meets network difficulty -- the pool then
 * forwards to the daemon as submitblock. */
static NSDictionary *pool_submit_share(NSString *jobId, NSString *wallet,
                                       uint32_t nonce, int is_full_block) {
  char url_c[320]; char host_c[256];
  pthread_mutex_lock(&g_lock);
  strncpy(host_c, g_pool_url, sizeof(host_c)-1); host_c[sizeof(host_c)-1] = 0;
  pthread_mutex_unlock(&g_lock);
  snprintf(url_c, sizeof(url_c), "%s/pool/submit", host_c);
  NSString *url = [NSString stringWithUTF8String:url_c];
  return http_post_json(url, @{
    @"job_id":     jobId ?: @"",
    @"wallet":     wallet ?: @"",
    @"nonce":      @(nonce),
    @"full_block": @(is_full_block ? YES : NO),
  });
}

/* v1.1.10: fire-and-forget share submission.
 *
 * Through v1.1.9 the mining loop called pool_submit_share() synchronously
 * for every winning nonce. The Cloudflare -> pool roundtrip is ~200-400 ms,
 * and at the live share rate (~2/s) that meant ~40-80% of wall-clock time
 * was spent blocked on the network instead of hashing. Because the local
 * compute-only hashrate estimator only measures dispatch_apply time, the
 * miner reported a number that the pool (correctly) could not corroborate.
 *
 * Off-loading submissions to a serial background queue makes the mining
 * loop wall-clock continuous and brings the miner's reported hashrate
 * back into agreement with the pool's accepted-share estimate. The serial
 * queue preserves submit order, which matters because the pool dedupes
 * (wallet, nonce) per job and we don't want concurrent submits racing. */
static dispatch_queue_t pool_submit_queue(void) {
  static dispatch_queue_t q = NULL;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    q = dispatch_queue_create("rime.pool.submit", DISPATCH_QUEUE_SERIAL);
  });
  return q;
}

/* v1.1.10: track consecutive invalid-share rejections so the worker can
 * detect a stale dataset / borked template and force a full refresh
 * before the pool's ban threshold trips. If the pool says "invalid
 * share" CONSECUTIVE_INVALID_THRESHOLD times in a row, the worker
 * thread observes g_force_refresh on its next iteration and:
 *   1. discards the current job_id / template / dataset (have_ds = 0)
 *   2. re-fetches the job from the pool
 *   3. rebuilds the dataset from the freshly-received seed_hash
 * That self-corrects whatever drift caused the rejection — usually an
 * epoch transition the miner missed. Honest miners never see this code
 * path; it only kicks in when something has genuinely gone wrong.
 *
 * v1.1.11: switched from g_lock-protected ints to <stdatomic.h>.
 * Earlier code grabbed g_lock per share response, but g_lock is also
 * acquired by every dispatch_apply thread on best-bits and winner
 * updates -- adding a second contender across 8 cores measurably
 * slowed hashing. Atomics make these counters lock-free. */
static atomic_int g_consecutive_invalid    = 0;
static atomic_int g_force_refresh          = 0;
#define CONSECUTIVE_INVALID_THRESHOLD 3

static void submit_share_async(NSString *jobId, NSString *wallet,
                               uint32_t nonce, int is_full_block) {
  dispatch_async(pool_submit_queue(), ^{
    NSDictionary *sr = pool_submit_share(jobId, wallet, nonce, is_full_block);
    if (!sr) return;        // network failure -- don't update counters

    BOOL accepted = [sr[@"accepted"] boolValue];
    NSString *reason = sr[@"reason"];
    BOOL is_block   = [sr[@"block"] boolValue];

    if (accepted || is_block) {
      // A good share clears the consecutive-invalid streak; one
      // transient bad submit doesn't accumulate forever.
      atomic_store(&g_consecutive_invalid, 0);
      if (is_block) {
        // blocks_found is in g_stats which is still mutex-protected.
        pthread_mutex_lock(&g_lock);
        g_stats.blocks_found++;
        pthread_mutex_unlock(&g_lock);
      }
      return;
    }

    // Pool rejected the share. Only "invalid share" indicates a hash
    // mismatch (stale dataset, etc.). Things like "stale job",
    // "duplicate", or "rate limit" are normal and shouldn't trip the
    // force-refresh logic.
    if ([reason isEqualToString:@"invalid share"]) {
      int n = atomic_fetch_add(&g_consecutive_invalid, 1) + 1;
      if (n >= CONSECUTIVE_INVALID_THRESHOLD) {
        atomic_store(&g_force_refresh, 1);
        NSLog(@"[miner] %d consecutive invalid-share rejections from pool; "
              @"forcing template + dataset refresh", n);
      }
    }
  });
}
/* ---- peer cache (seeds + discovered) ---------------------------------- */
/* The Cloudflare Worker stays primary -- it absorbs the connection storm a
   difficulty-1 miner produces. The two direct-node hostnames are seed
   fallbacks for when the Worker is unreachable. Additional peers are
   discovered via get_peer_list and remembered across launches. */
static PeerCache *g_peers = NULL;

static int peers_load_cb(char *out, int cap, void *ctx) {
  (void)ctx;
  NSString *s = [[NSUserDefaults standardUserDefaults] stringForKey:@"peerCache"];
  if (!s) return 0;
  const char *c = s.UTF8String;
  if (!c) return 0;
  strncpy(out, c, cap - 1);
  out[cap - 1] = 0;
  return 1;
}
static void peers_save_cb(const char *json, void *ctx) {
  (void)ctx;
  if (!json) return;
  [[NSUserDefaults standardUserDefaults]
      setObject:[NSString stringWithUTF8String:json] forKey:@"peerCache"];
}
static void peers_init_once(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    g_peers = peer_cache_new(peers_load_cb, peers_save_cb, NULL);
    peer_cache_add_seed(g_peers, "glaciem-rpc.frostmine.workers.dev", 443, 1);
    peer_cache_add_seed(g_peers, "static.197.125.225.46.clients.your-server.de", 19081, 0);
    peer_cache_add_seed(g_peers, "static.34.142.105.178.clients.your-server.de", 19081, 0);
  });
}

/* Periodic peer discovery: hit /get_peer_list on a node that just answered,
   extract peers that advertise an RPC port (i.e. --public-node operators),
   add them to the cache for future fallback. */
static void try_discover_peers(NSString *base_url) {
  if (!peer_cache_should_discover(g_peers)) return;
  /* base_url is the full /json_rpc URL; swap suffix for /get_peer_list */
  NSString *pl_url =
      [base_url stringByReplacingOccurrencesOfString:@"/json_rpc"
                                          withString:@"/get_peer_list"];
  NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:pl_url]];
  req.HTTPMethod = @"POST";
  req.HTTPBody = [@"" dataUsingEncoding:NSUTF8StringEncoding];
  req.timeoutInterval = 5;
  __block NSData *out = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [[[NSURLSession sharedSession] dataTaskWithRequest:req
      completionHandler:^(NSData *d, NSURLResponse *r, NSError *e){
        out = d; dispatch_semaphore_signal(sem);
      }] resume];
  if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 6*NSEC_PER_SEC)))
    return;
  if (!out) return;
  id resp = [NSJSONSerialization JSONObjectWithData:out options:0 error:nil];
  if (![resp isKindOfClass:NSDictionary.class]) return;
  NSArray *white = ((NSDictionary *)resp)[@"white_list"];
  if (![white isKindOfClass:NSArray.class]) { peer_cache_reset_discovery_counter(g_peers); return; }
  int added = 0;
  for (NSDictionary *p in white) {
    if (![p isKindOfClass:NSDictionary.class]) continue;
    NSNumber *rpc_port = p[@"rpc_port"];
    NSString *host     = p[@"host"];
    if (![host isKindOfClass:NSString.class] || ![rpc_port isKindOfClass:NSNumber.class]) continue;
    int rp = rpc_port.intValue;
    if (rp <= 0 || rp > 65535) continue;   /* operator didn't run --public-node */
    peer_cache_add_discovered(g_peers, host.UTF8String, rp);
    if (++added >= 4) break;               /* cap per discovery */
  }
  peer_cache_reset_discovery_counter(g_peers);
}

static NSDictionary *json_rpc(NSString *method, id params) {
  peers_init_once();
  PeerEntry snap[64];
  int n = peer_cache_snapshot(g_peers, snap, 64);
  for (int i = 0; i < n; i++) {
    NSString *url = [NSString stringWithFormat:@"%s://%s:%d/json_rpc",
                     snap[i].use_ssl ? "https" : "http", snap[i].host, snap[i].port];
    double t0 = now_s();
    NSDictionary *r = json_rpc_url(url, method, params);
    int latency_ms = (int)((now_s() - t0) * 1000);
    if (r) {
      peer_cache_mark_success(g_peers, snap[i].host, snap[i].port, latency_ms);
      /* v1.1.3: try_discover_peers() removed. The public proxy now 403s
         /get_peer_list (admin/debug endpoint that shouldn't be exposed),
         so this call always failed silently and was wasted bandwidth.
         Peers come from seeded endpoints only; revisit if peer churn
         becomes a real problem. */
      return r;
    }
    peer_cache_mark_failure(g_peers, snap[i].host, snap[i].port);
  }
  return nil;
}

/* Wallet failover uses the same peer cache as the miner -- when refresh
   can't reach the current daemon for several cycles, snapshot the cache
   and rotate to the next entry. Seeds always lead; discovered peers
   follow them in score order. The swap goes through wallet2::set_daemon
   (via rime_wallet_set_daemon) so keys/balance/scanned-height are
   preserved across the swap. */
#define WALLET_FAILOVER_THRESHOLD 3   /* consecutive disconnects before swap */

/* ---- embedded-wallet poll: address + balance + sync state ---- */
static void *wallet_poll(void *arg) {
  (void)arg;
  peers_init_once();
  int disconnect_count = 0;
  int endpoint_idx = 0;       /* index into the snapshot order */
  while (1) {
    RimeWallet *w;
    pthread_mutex_lock(&g_lock);
    w = g_wallet;
    pthread_mutex_unlock(&g_lock);
    if (w) {
      /* publish address + persisted balance FIRST -- both are available the
         moment the wallet file opens, before the blocking chain scan below.
         Otherwise the UI shows "no wallet" for the whole initial re-scan. */
      char addr[160] = "";
      rime_wallet_address(w, addr, sizeof addr);
      pthread_mutex_lock(&g_lock);
      strncpy(g_wallet_addr, addr, sizeof(g_wallet_addr)-1);
      g_wallet_addr[sizeof(g_wallet_addr)-1] = 0;
      g_stats.balance          = rime_wallet_balance(w);
      g_stats.unlocked_balance = rime_wallet_unlocked_balance(w);
      pthread_mutex_unlock(&g_lock);

      rime_wallet_refresh(w);            /* blocking chain scan */
      int conn     = rime_wallet_connected(w);
      int synced   = rime_wallet_synchronized(w);
      uint64_t bal = rime_wallet_balance(w);
      uint64_t unl = rime_wallet_unlocked_balance(w);
      uint64_t wht = rime_wallet_height(w);
      uint64_t tgt = rime_wallet_daemon_height(w);
      /* Wallet stranded ahead of the daemon -> it scanned a now-dead fork
         (e.g. the testnet was reset). wallet2 reads this as "the daemon is
         behind" and refresh() never rolls back, so the UI sticks on
         "catching up". Force a rescan from genesis, then re-read state. */
      if (conn && tgt > 0 && wht > tgt) {
        rime_wallet_rescan(w);           /* blocking */
        synced = rime_wallet_synchronized(w);
        bal    = rime_wallet_balance(w);
        unl    = rime_wallet_unlocked_balance(w);
        wht    = rime_wallet_height(w);
        tgt    = rime_wallet_daemon_height(w);
      }
      rime_wallet_store(w);              /* persist so balance survives relaunch */
      pthread_mutex_lock(&g_lock);
      g_stats.wallet_connected = conn;
      g_stats.balance          = bal;
      g_stats.unlocked_balance = unl;
      g_stats.wallet_height    = wht;
      g_stats.target_height    = tgt;
      g_stats.wallet_syncing   = (conn && !synced) ? 1 : 0;
      pthread_mutex_unlock(&g_lock);

      /* Failover: after N consecutive disconnects, snapshot the peer
         cache (seeds + discovered, in score order) and rotate to the
         next entry. The wallet's keys, balance, and scanned height are
         preserved across the swap -- only the HTTP connection changes. */
      if (conn) {
        disconnect_count = 0;
      } else if (++disconnect_count >= WALLET_FAILOVER_THRESHOLD) {
        PeerEntry snap[64];
        int n = peer_cache_snapshot(g_peers, snap, 64);
        if (n > 0) {
          endpoint_idx = (endpoint_idx + 1) % n;
          char daemon[200];
          snprintf(daemon, sizeof daemon, "%s:%d",
                   snap[endpoint_idx].host, snap[endpoint_idx].port);
          rime_wallet_set_daemon(w, daemon);
        }
        disconnect_count = 0;
      }
    } else {
      pthread_mutex_lock(&g_lock);
      g_stats.wallet_connected = 0;
      pthread_mutex_unlock(&g_lock);
    }
    /* v1.1.3: bumped 4s -> 20s. Block time is ~120s so a 20s refresh
       still gives a live-feeling UI while cutting /getblocks.bin traffic
       to the public proxy ~5x. Send/sweep paths can still drive an
       immediate refresh through the same thread if needed. */
    usleep(20000000);  /* poll every ~20s */
  }
  return NULL;
}

/* background body for miner_open_wallet -- recover/open can take a while */
static void *open_wallet_thread(void *arg) {
  char *seed = (char *)arg;            /* heap copy; freed here */
  char path[1024], host[128]; int port;
  pthread_mutex_lock(&g_lock);
  strncpy(path, g_wallet_path, sizeof path); path[sizeof path-1] = 0;
  strncpy(host, g_node_host, sizeof host);   host[sizeof host-1] = 0;
  port = g_node_port;
  RimeWallet *old = g_wallet;
  g_wallet = NULL;
  g_wallet_addr[0] = 0;
  g_stats.wallet_connected = 0;
  pthread_mutex_unlock(&g_lock);
  if (old) rime_wallet_close(old);
  char daemon[200];
  snprintf(daemon, sizeof daemon, "%s:%d", host, port);
  RimeWallet *w = rime_wallet_recover(path, seed ? seed : "", daemon, 0);
  pthread_mutex_lock(&g_lock);
  g_wallet = w;
  pthread_mutex_unlock(&g_lock);
  free(seed);
  return NULL;
}
static NSData *hex2data(NSString *hex) {
  if (![hex isKindOfClass:NSString.class] || hex.length%2) return nil;
  NSMutableData *d = [NSMutableData dataWithLength:hex.length/2];
  unsigned char *p = d.mutableBytes;
  const char *c = hex.UTF8String;
  for (NSUInteger i=0;i<hex.length/2;i++) {
    unsigned v; if (sscanf(c+i*2,"%2x",&v)!=1) return nil; p[i]=(unsigned char)v;
  }
  return d;
}
static NSString *data2hex(NSData *d) {
  const unsigned char *b = d.bytes;
  NSMutableString *s = [NSMutableString stringWithCapacity:d.length*2];
  for (NSUInteger i=0;i<d.length;i++) [s appendFormat:@"%02x",b[i]];
  return s;
}

static void worker_set(int connected, double hr, uint64_t total, uint64_t height,
                       uint64_t diff, uint64_t blocks, int best, double up,
                       const char *lh) {
  (void)blocks;  /* v1.1.10: blocks_found is now owned by the submit/solo paths
                  * (incremented under g_lock on successful block submission);
                  * worker_set no longer overwrites it, which would clobber
                  * any concurrent increment from the async share-submitter. */
  pthread_mutex_lock(&g_lock);
  g_stats.daemon_connected=connected; g_stats.hashrate=hr;
  g_stats.total_hashes=total; g_stats.height=height; g_stats.difficulty=diff;
  g_stats.best_bits=best; g_stats.uptime_s=up;
  if(lh) memcpy(g_lasthash,lh,65);
  pthread_mutex_unlock(&g_lock);
}

/* The mining worker: pulls block templates and hashes nonces across all CPU
   cores via dispatch_apply with the Lattice PoW. */
static void *worker(void *arg) {
  (void)arg;
  double t0=now_s(), hr=0;
  uint64_t total=0, blocks=0; int best=0;
  u8 cur_seed[32]={0}; int have_ds=0;
  u64 *ds = malloc(DATASET_WORDS*sizeof(u64));   /* v2 epoch dataset */
  /* v1.1.10: nonce base lives the LIFETIME of the worker.
   *
   * Previously this was re-initialized to `now_s() * 997` on every outer
   * iteration. In pool mode the pool re-issues the SAME job_id whenever
   * the upstream template hasn't refreshed (~every ~120s real block),
   * but the inner loop only mines for template_budget=2.5s before
   * exiting and re-fetching. The new `base = now_s() * 997` then sat
   * only ~2.5 s of "997-units" further on -- overlapping ~25% of the
   * previous batch's nonce range. The pool's per-job dedupe set then
   * dropped 20%+ of submissions as "duplicate", which read as pool
   * hashrate trailing the miner by the same amount.
   *
   * Initializing once at worker start (with entropy so concurrent
   * miners on the same pool start at different offsets) and letting
   * base monotonically advance by BATCH per iteration guarantees a
   * unique 32-bit nonce range for every batch in the session
   * (8.4M batches before wrap -- months of continuous mining). */
  uint32_t base = (uint32_t)(now_s() * 997.0);

  while(g_running) {
    char wa[160];
    pthread_mutex_lock(&g_lock);
    memcpy(wa,g_wallet_addr,sizeof(wa));
    pthread_mutex_unlock(&g_lock);
    /* mine coinbase rewards to the app's embedded wallet. With no wallet open
       the miner refuses to mine -- no fallback address. */
    const char *chosen = wa[0] ? wa : NULL;
    if(!chosen){
      worker_set(0,hr,total,0,0,blocks,best,now_s()-t0,NULL);
      pthread_mutex_lock(&g_lock); g_stats.no_address=1; pthread_mutex_unlock(&g_lock);
      for(int i=0;i<10 && g_running;i++) usleep(100000);
      continue;
    }
    pthread_mutex_lock(&g_lock); g_stats.no_address=0; pthread_mutex_unlock(&g_lock);
    NSString *mine_to = [NSString stringWithUTF8String:chosen];

    /* v1.1.10: if the async submitter saw >=N consecutive invalid-share
     * rejections from the pool, drop the cached dataset so we rebuild
     * from the next job's seed_hash. The OUTER iteration also runs
     * pool_get_job below, so the combination of (have_ds = 0 + a fresh
     * job fetch) guarantees we re-sync against whatever the pool now
     * thinks the current epoch is.
     * v1.1.11: lock-free check via atomic_exchange. The flag is rare
     * (only ever set when something has genuinely gone wrong), so the
     * atomic exchange has no contention cost in the common path. */
    if (atomic_exchange(&g_force_refresh, 0)) {
      atomic_store(&g_consecutive_invalid, 0);
      have_ds = 0;
      memset(cur_seed, 0, sizeof cur_seed);
    }

    /* v1.1.6: in pool mode, fetch the job from the pool instead of
     * calling get_block_template on the daemon. The pool injects ITS
     * wallet as the coinbase recipient and tracks our share contribution
     * against `mine_to`, paying out proportionally on block-find. */
    int pool_mode_now = g_pool_enabled;
    NSDictionary *tpl;
    NSString *job_id = nil;
    uint64_t share_diff = 0;     /* only set in pool mode */
    if (pool_mode_now) {
      tpl = pool_get_job(mine_to);
      if (tpl) {
        job_id     = tpl[@"job_id"];
        share_diff = [tpl[@"share_difficulty"] unsignedLongLongValue];
      }
    } else {
      tpl = json_rpc(@"get_block_template",
          @{@"wallet_address":mine_to, @"reserve_size":@(8)});
    }
    if(!tpl || ![tpl[@"blockhashing_blob"] isKindOfClass:NSString.class]) {
      worker_set(0,hr,total,0,0,blocks,best,now_s()-t0,NULL);
      for(int i=0;i<10 && g_running;i++) usleep(100000);
      continue;
    }
    NSData *hbD=hex2data(tpl[@"blockhashing_blob"]);
    NSData *tbD=hex2data(tpl[@"blocktemplate_blob"]);
    NSData *seedD=hex2data(tpl[@"seed_hash"]);
    uint64_t height=[tpl[@"height"] unsignedLongLongValue];
    /* In pool mode the daemon's "difficulty" field is replaced by the
     * pool's share_difficulty; we keep network_difficulty separately to
     * know when a share ALSO solves the actual block. */
    uint64_t diff = pool_mode_now
      ? share_diff
      : [tpl[@"difficulty"] unsignedLongLongValue];
    uint64_t net_diff = pool_mode_now
      ? [tpl[@"network_difficulty"] unsignedLongLongValue]
      : diff;
    if(!hbD || !tbD || hbD.length<1 || hbD.length>250) { usleep(200000); continue; }
    const u8 *hb=hbD.bytes; int hb_len=(int)hbD.length;
    int noff=nonce_offset(hb,hb_len);
    if(noff+4>hb_len) { usleep(200000); continue; }

    u8 seed[32]={0};
    if(seedD && seedD.length==32) memcpy(seed,seedD.bytes,32);
    if(!have_ds || memcmp(seed,cur_seed,32)!=0){
      lattice_build_dataset(seed, ds);              /* once per epoch */
      memcpy(cur_seed,seed,32); have_ds=1;
    }

    /* mine this template (~1.5s budget, or until a block is found).
     * v1.1.10: `base` is now hoisted out of this loop -- see the comment
     * at worker() top. It advances monotonically across all batches and
     * all outer iterations, so we never re-walk a nonce range we've
     * already submitted (which the pool would dedup). */
    /* v1.1.9: collect ALL nonces that meet share_difficulty in this
     * batch, not just the first. Previously we lost ~30% of mined
     * shares because a batch often had 2-3 winning nonces and we only
     * submitted one. Cap at 16 -- a 512-batch hitting >16 shares
     * implies share_difficulty is absurdly low; the cap keeps the
     * winners buffer bounded. */
    #define WIN_MAX 16
    /* Blocks can't capture raw C arrays, so the storage is stack-resident
     * and the block sees it via a pointer. Same lifetime as the outer
     * while-loop frame, which is what we want. */
    uint32_t winners_storage[WIN_MAX];
    memset(winners_storage, 0, sizeof winners_storage);
    __block uint32_t *winners = winners_storage;
    __block int n_winners = 0;
    __block int bbest=best;
    char lhbuf[65]={0}; char *lh=lhbuf;
    double tm=now_s();
    /* v1.1.3: template lifetime 1.5s -> 10s in solo mode (block time
     *   ~120s, ~8% wasted compute on stale templates is fine).
     * v1.1.6: in POOL mode, refresh every 2.5s instead. The pool
     *   already caches its upstream template, so fetching often is
     *   cheap; and a stale pool job whose share_difficulty changed
     *   would have its submissions rejected as "stale job" by the pool.
     *   Stay conservative.
     * v1.1.9: solo mode still breaks the outer loop on first share
     *   (we have the full block, no reason to keep mining the same
     *   template). Pool mode keeps mining until template_budget
     *   expires -- we WANT more shares per template. */
    double template_budget = pool_mode_now ? 2.5 : 10.0;
    int solo_short_circuit = 0;
    /* v1.1.10: wall-clock hashrate. We measure interval END-to-END so any
     * time the worker spends NOT inside dispatch_apply (submit prep,
     * pool_get_job, dispatch overhead) is naturally subtracted from the
     * effective rate. Previously inst = BATCH / dispatch_apply_time, which
     * counted compute only -- the resulting figure could exceed the pool's
     * wall-clock-grounded estimate by 30%+. */
    double prev_end = now_s();
    while(g_running && !solo_short_circuit && now_s()-tm < template_budget) {
      /* the whole batch is always hashed -- no early-out on a found block --
         so the hashrate reflects real compute. On a low-difficulty testnet a
         block is found almost every batch; early-out would make the rate
         (BATCH / partial-time) spike wildly. */
      /* v1.1.14+: thread-count picker. Two code paths so we don't pay the
       * P/E convoy tax on Apple Silicon when ALL cores are selected:
       *
       *   - n_threads >= max_cores (ALL): use the original
       *     dispatch_apply(BATCH, ...) pattern. GCD pulls tasks from a
       *     shared queue, so fast P-cores naturally pick up more work than
       *     E-cores and the batch finishes in ~max-core-speed time, not
       *     ~min-core-speed time.
       *
       *   - n_threads < max_cores (capped): submit BATCH/N small chunks
       *     through a dispatch_semaphore that throttles to n_threads
       *     in-flight. 8x oversubscription per budgeted thread keeps the
       *     load balancer happy across asymmetric cores.
       *
       * Re-read g_thread_count once per batch so a Settings change applies
       * within ~1s without restarting mining. */
      int n_threads = g_thread_count;
      if (n_threads < 1) n_threads = 1;
      if (n_threads > g_max_cores) n_threads = g_max_cores;

      if (n_threads >= g_max_cores) {
        /* Fast path -- bit-for-bit the v1.1.13 hot loop. No concurrency
         * throttle, GCD load-balances 512 tiny tasks across all cores. */
        dispatch_apply(BATCH, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH,0),
                       ^(size_t n){
          u8 blob[256]; memcpy(blob,hb,hb_len);
          uint32_t nn=base+(uint32_t)n;
          blob[noff]=(u8)nn; blob[noff+1]=(u8)(nn>>8);
          blob[noff+2]=(u8)(nn>>16); blob[noff+3]=(u8)(nn>>24);
          u8 mh[32];
          lattice_hash_ds(blob,hb_len,ds,mh);
          int z=lz_bits(mh);
          if(z>bbest){ pthread_mutex_lock(&g_lock); if(z>bbest) bbest=z;
                       pthread_mutex_unlock(&g_lock); }
          if(meets_target(mh,diff)){
            pthread_mutex_lock(&g_lock);
            if (n_winners < WIN_MAX) winners[n_winners++] = nn;
            pthread_mutex_unlock(&g_lock);
          }
          if(n==BATCH-1) for(int i=0;i<32;i++) sprintf(lh+i*2,"%02x",mh[i]);
        });
      } else {
        /* Capped path -- throttle to n_threads in-flight with semaphore.
         * Submit 8 chunks per thread so faster cores can pick up slack. */
        const int chunks_per_thread = 8;
        int total_chunks = n_threads * chunks_per_thread;
        if (total_chunks > BATCH) total_chunks = BATCH;
        int chunk_size = (BATCH + total_chunks - 1) / total_chunks;
        dispatch_semaphore_t sem = dispatch_semaphore_create(n_threads);
        dispatch_group_t      group = dispatch_group_create();
        dispatch_queue_t      gq    = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH,0);
        for (int c = 0; c < total_chunks; c++) {
          dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
          int chunk_idx = c;
          dispatch_group_async(group, gq, ^{
            size_t start = (size_t)chunk_idx * (size_t)chunk_size;
            size_t end   = start + (size_t)chunk_size;
            if (end > BATCH) end = BATCH;
            u8 blob[256]; memcpy(blob,hb,hb_len);
            u8 mh[32];
            for (size_t n = start; n < end; n++) {
              uint32_t nn=base+(uint32_t)n;
              blob[noff]=(u8)nn; blob[noff+1]=(u8)(nn>>8);
              blob[noff+2]=(u8)(nn>>16); blob[noff+3]=(u8)(nn>>24);
              lattice_hash_ds(blob,hb_len,ds,mh);
              int z=lz_bits(mh);
              if(z>bbest){ pthread_mutex_lock(&g_lock); if(z>bbest) bbest=z;
                           pthread_mutex_unlock(&g_lock); }
              if(meets_target(mh,diff)){
                pthread_mutex_lock(&g_lock);
                if (n_winners < WIN_MAX) winners[n_winners++] = nn;
                pthread_mutex_unlock(&g_lock);
              }
              if(n==BATCH-1) for(int i=0;i<32;i++) sprintf(lh+i*2,"%02x",mh[i]);
            }
            dispatch_semaphore_signal(sem);
          });
        }
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
      }
      double now = now_s();
      double interval = now - prev_end; if (interval < 1e-6) interval = 1e-6;
      double inst = BATCH / interval;
      hr = (hr <= 0) ? inst : 0.7*hr + 0.3*inst;
      prev_end = now;
      total+=BATCH; base+=BATCH; best=bbest;
      worker_set(1,hr,total,height,diff,blocks,best,now_s()-t0,lh);

      /* Submit every winner we found this batch. In pool mode we keep
       * mining the same template afterward; in solo mode we just need
       * the first one (it's a full block), and then we move on. */
      if (n_winners > 0) {
        if (pool_mode_now) {
          /* v1.1.10: fire-and-forget. We snapshot the winners under the
           * lock, then enqueue every one to the serial submit queue and
           * IMMEDIATELY continue mining. Previously this loop blocked on
           * the HTTP roundtrip per share, eating 40-80% of wall-clock
           * time. The async submitter increments g_stats.blocks_found
           * itself if the pool says we landed a full block. */
          uint32_t to_submit[WIN_MAX]; int nw;
          pthread_mutex_lock(&g_lock);
          nw = n_winners;
          memcpy(to_submit, winners, sizeof(uint32_t) * nw);
          n_winners = 0;
          pthread_mutex_unlock(&g_lock);
          for (int i = 0; i < nw; i++) {
            uint32_t w = to_submit[i];
            u8 blob[256]; memcpy(blob, hb, hb_len);
            blob[noff]=(u8)w; blob[noff+1]=(u8)(w>>8);
            blob[noff+2]=(u8)(w>>16); blob[noff+3]=(u8)(w>>24);
            u8 mh[32]; lattice_hash_ds(blob, hb_len, ds, mh);
            int is_full = (net_diff > 0 && meets_target(mh, net_diff)) ? 1 : 0;
            submit_share_async(job_id, mine_to, w, is_full);
          }
          /* Cheap publish; blocks_found is owned by submit_share_async. */
          worker_set(1,hr,total,height,diff,0,best,now_s()-t0,lh);
        } else {
          /* Solo: take the first winner (any one solves the block). */
          uint32_t w = winners[0];
          NSMutableData *block=[tbD mutableCopy];
          u8 *bb=block.mutableBytes;
          if(noff+4<=(int)block.length){
            bb[noff]=(u8)w; bb[noff+1]=(u8)(w>>8);
            bb[noff+2]=(u8)(w>>16); bb[noff+3]=(u8)(w>>24);
            NSDictionary *sr=json_rpc(@"submit_block",@[data2hex(block)]);
            if(sr && [sr[@"status"] isEqualToString:@"OK"]) {
              pthread_mutex_lock(&g_lock);
              g_stats.blocks_found++;
              pthread_mutex_unlock(&g_lock);
            }
          }
          n_winners = 0;
          solo_short_circuit = 1;   /* fetch a fresh template */
          worker_set(1,hr,total,height,diff,0,best,now_s()-t0,lh);
        }
      }
    }
  }
  free(ds);
  return NULL;
}

void miner_start(void) {
  if(g_running) return;
  ensure_cores_init();
  pthread_mutex_lock(&g_lock);
  /* v1.1.10: reset MINING counters only -- earlier versions zeroed the
   * entire g_stats struct, which also wiped balance / unlocked_balance /
   * wallet_height / target_height / wallet_connected / wallet_syncing.
   * The next blocking wallet_poll() refresh could take minutes to
   * republish those fields, so the UI flashed "balance 0" for the
   * duration. Leave wallet state alone -- wallet_poll owns it. */
  g_stats.daemon_connected = 0;
  g_stats.hashrate         = 0;
  g_stats.total_hashes     = 0;
  g_stats.height           = 0;
  g_stats.difficulty       = 0;
  g_stats.blocks_found     = 0;
  g_stats.best_bits        = 0;
  g_stats.uptime_s         = 0;
  g_stats.no_address       = 0;
  g_lasthash[0]            = 0;
  pthread_mutex_unlock(&g_lock);
  /* Tell macOS this is real work: opt out of App Nap (which throttles the
     CPU when the window is unfocused) and keep the system from idle-sleeping
     while mining. Released in miner_stop so the Mac sleeps normally when idle. */
  if(!g_activity)
    g_activity = [[NSProcessInfo processInfo]
                  beginActivityWithOptions:NSActivityUserInitiated
                                    reason:@"Mining"];
  g_running=1;
  pthread_create(&g_worker,NULL,worker,NULL);
}
void miner_stop(void) {
  if(!g_running) return;
  g_running=0;
  pthread_join(g_worker,NULL);
  if(g_activity){
    [[NSProcessInfo processInfo] endActivity:g_activity];
    g_activity=nil;
  }
}
MinerStats miner_get_stats(void) {
  static int wallet_started = 0;
  if (!wallet_started) {            /* lazily start wallet polling */
    wallet_started = 1;
    pthread_t wt; pthread_create(&wt,NULL,wallet_poll,NULL); pthread_detach(wt);
  }
  MinerStats s;
  pthread_mutex_lock(&g_lock);
  s=g_stats;
  pthread_mutex_unlock(&g_lock);
  s.running=g_running;
  if(!s.running) s.hashrate=0;   /* idle -> report 0, not the last reading */
  return s;
}
const char *miner_device(void) {
  if (g_device[0]=='-' || g_device[0]==0) {
    size_t len=sizeof g_device;
    if (sysctlbyname("machdep.cpu.brand_string", g_device, &len, NULL, 0)!=0)
      strcpy(g_device,"CPU");
  }
  return g_device;
}
const char *miner_last_hash(void) {
  static char buf[65];
  pthread_mutex_lock(&g_lock);
  memcpy(buf,g_lasthash,65);
  pthread_mutex_unlock(&g_lock);
  return buf;
}
const char *miner_wallet_address(void) {
  static char buf[160];
  pthread_mutex_lock(&g_lock);
  memcpy(buf,g_wallet_addr,sizeof(buf));
  pthread_mutex_unlock(&g_lock);
  return buf;
}

/* open the app's embedded wallet (recover for `seed`, or open existing if
   `seed` is empty). Returns immediately; the work runs on a background thread. */
void miner_open_wallet(const char *path, const char *seed) {
  if (!path || !path[0]) return;
  pthread_mutex_lock(&g_lock);
  strncpy(g_wallet_path, path, sizeof(g_wallet_path)-1);
  g_wallet_path[sizeof(g_wallet_path)-1] = 0;
  pthread_mutex_unlock(&g_lock);
  char *seedcopy = strdup(seed ? seed : "");
  pthread_t t;
  pthread_create(&t, NULL, open_wallet_thread, seedcopy);
  pthread_detach(t);
}

/* set the rimed node this app connects to; the UI persists it */
void miner_set_node(const char *host, int port) {
  pthread_mutex_lock(&g_lock);
  if (host && host[0]) {
    strncpy(g_node_host, host, sizeof(g_node_host)-1);
    g_node_host[sizeof(g_node_host)-1] = 0;
  }
  if (port > 0 && port < 65536) g_node_port = port;
  pthread_mutex_unlock(&g_lock);
}

void miner_set_pool_config(int enabled, const char *url) {
  pthread_mutex_lock(&g_lock);
  g_pool_enabled = enabled ? 1 : 0;
  if (url && url[0]) {
    /* Strip a trailing slash so we can append "/pool/job" cleanly. */
    size_t len = strlen(url);
    char tmp[sizeof(g_pool_url)];
    strncpy(tmp, url, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = 0;
    if (len > 0 && tmp[strlen(tmp)-1] == '/') tmp[strlen(tmp)-1] = 0;
    strcpy(g_pool_url, tmp);
  }
  pthread_mutex_unlock(&g_lock);
}

/* v1.1.14+: thread-count picker. Worker re-reads g_thread_count at the top
 * of every batch, so changes apply within ~1s without restarting mining. */
void miner_set_thread_count(int n) {
  ensure_cores_init();
  if (n < 1) n = 1;
  if (n > g_max_cores) n = g_max_cores;
  g_thread_count = n;
}
int miner_get_thread_count(void) {
  ensure_cores_init();
  return g_thread_count;
}
int miner_max_cores(void) {
  ensure_cores_init();
  return g_max_cores;
}

/* generate a fresh Rime wallet via the keygen library (Rime's real crypto) */
int miner_generate_address(char *addr_out, int addr_cap, char *seed_out, int seed_cap) {
  if (!addr_out || !seed_out) return 0;
  RimeKeypair k;
  if (!rime_generate_address(&k)) return 0;
  if ((int)strlen(k.address)  >= addr_cap) return 0;
  if ((int)strlen(k.mnemonic) >= seed_cap) return 0;
  strcpy(addr_out, k.address);
  strcpy(seed_out, k.mnemonic);
  return 1;
}

/* send `amount` GLAC to `address` from the embedded wallet. Blocking. */
const char *miner_send(const char *address, double amount) {
  static char result[256];
  RimeWallet *w;
  pthread_mutex_lock(&g_lock);
  w = g_wallet;
  pthread_mutex_unlock(&g_lock);
  if (!w) { strcpy(result, "Wallet not ready yet"); return result; }
  if (amount <= 0) { strcpy(result, "Enter an amount greater than 0"); return result; }
  unsigned long long atomic = (unsigned long long)(amount * 1e12 + 0.5);
  rime_wallet_send(w, address ? address : "", atomic, result, sizeof result);
  return result;
}

/* sweep unmixable (mostly coinbase) outputs so mined coins become spendable. */
const char *miner_sweep_unmixable(void) {
  static char result[256];
  RimeWallet *w;
  pthread_mutex_lock(&g_lock);
  w = g_wallet;
  pthread_mutex_unlock(&g_lock);
  if (!w) { strcpy(result, "Wallet not ready yet"); return result; }
  rime_wallet_sweep_unmixable(w, result, sizeof result);
  return result;
}

/* recent transaction history (sends/sweeps/receives), newest first. */
const char *miner_history(void) {
  static char result[8192];
  RimeWallet *w;
  pthread_mutex_lock(&g_lock);
  w = g_wallet;
  pthread_mutex_unlock(&g_lock);
  if (!w) { strcpy(result, "Wallet not ready yet"); return result; }
  rime_wallet_history(w, result, sizeof result);
  return result;
}
