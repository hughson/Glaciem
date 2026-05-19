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
#include <sys/sysctl.h>

#include "miner_core.h"
#include "rime_keygen.h"        /* rime_generate_address */
#include "rime_wallet.h"        /* embedded wallet (Monero wallet_api) */

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
static char            g_node_host[128]   = "178.105.142.34";  /* rimed node host */
static int             g_node_port        = 19081;        /* rimed RPC port  */
static char            g_wallet_path[1024]= "";   /* embedded wallet file path */
static RimeWallet   *g_wallet           = NULL; /* embedded wallet handle    */

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
/* node RPC (rimed) -- URL built from the runtime-settable host/port */
static NSDictionary *json_rpc(NSString *method, id params) {
  pthread_mutex_lock(&g_lock);
  NSString *url = [NSString stringWithFormat:@"http://%s:%d/json_rpc",
                   g_node_host, g_node_port];
  pthread_mutex_unlock(&g_lock);
  return json_rpc_url(url, method, params);
}

/* ---- embedded-wallet poll: address + balance + sync state ---- */
static void *wallet_poll(void *arg) {
  (void)arg;
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
    } else {
      pthread_mutex_lock(&g_lock);
      g_stats.wallet_connected = 0;
      pthread_mutex_unlock(&g_lock);
    }
    usleep(4000000);   /* poll every ~4s */
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
  pthread_mutex_lock(&g_lock);
  g_stats.daemon_connected=connected; g_stats.hashrate=hr;
  g_stats.total_hashes=total; g_stats.height=height; g_stats.difficulty=diff;
  g_stats.blocks_found=blocks; g_stats.best_bits=best; g_stats.uptime_s=up;
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
    NSDictionary *tpl = json_rpc(@"get_block_template",
        @{@"wallet_address":mine_to, @"reserve_size":@(8)});
    if(!tpl || ![tpl[@"blockhashing_blob"] isKindOfClass:NSString.class]) {
      worker_set(0,hr,total,0,0,blocks,best,now_s()-t0,NULL);   /* no daemon */
      for(int i=0;i<10 && g_running;i++) usleep(100000);
      continue;
    }
    NSData *hbD=hex2data(tpl[@"blockhashing_blob"]);
    NSData *tbD=hex2data(tpl[@"blocktemplate_blob"]);
    NSData *seedD=hex2data(tpl[@"seed_hash"]);
    uint64_t height=[tpl[@"height"] unsignedLongLongValue];
    uint64_t diff=[tpl[@"difficulty"] unsignedLongLongValue];
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

    /* mine this template (~1.5s budget, or until a block is found) */
    uint32_t base=(uint32_t)(now_s()*997.0);
    __block int found=0; __block uint32_t win=0; __block int bbest=best;
    char lhbuf[65]={0}; char *lh=lhbuf;
    double tm=now_s();
    while(g_running && !found && now_s()-tm < 1.5) {
      double tb=now_s();
      /* the whole batch is always hashed -- no early-out on a found block --
         so the hashrate reflects real compute. On a low-difficulty testnet a
         block is found almost every batch; early-out would make the rate
         (BATCH / partial-time) spike wildly. */
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
          if(!found){ found=1; win=nn; }
          pthread_mutex_unlock(&g_lock);
        }
        if(n==BATCH-1) for(int i=0;i<32;i++) sprintf(lh+i*2,"%02x",mh[i]);
      });
      double dt=now_s()-tb; if(dt<=0) dt=1e-6;
      double inst=BATCH/dt; hr=(hr<=0)?inst:0.7*hr+0.3*inst;
      total+=BATCH; base+=BATCH; best=bbest;
      worker_set(1,hr,total,height,diff,blocks,best,now_s()-t0,lh);
    }

    if(found){
      NSMutableData *block=[tbD mutableCopy];
      u8 *bb=block.mutableBytes;
      if(noff+4<=(int)block.length){
        bb[noff]=(u8)win; bb[noff+1]=(u8)(win>>8);
        bb[noff+2]=(u8)(win>>16); bb[noff+3]=(u8)(win>>24);
        NSDictionary *sr=json_rpc(@"submit_block",@[data2hex(block)]);
        if(sr && [sr[@"status"] isEqualToString:@"OK"]) blocks++;
      }
      worker_set(1,hr,total,height,diff,blocks,best,now_s()-t0,lh);
    }
  }
  free(ds);
  return NULL;
}

void miner_start(void) {
  if(g_running) return;
  pthread_mutex_lock(&g_lock);
  memset(&g_stats,0,sizeof g_stats); g_lasthash[0]=0;
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

/* send `amount` RME to `address` from the embedded wallet. Blocking. */
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
