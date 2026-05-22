/*
 * rime_miner_win.c -- Glaciem Miner for Windows (graphical, single .exe).
 *
 * A standalone CPU-only Lattice miner with a small Win32 dashboard (live
 * hashrate, status, blocks found, height). It mines real block templates
 * from rimed over JSON-RPC and carries an embedded wallet -- the Monero
 * wallet_api built from the Rime fork -- so it generates an address with
 * the keygen library and the dashboard shows its live balance and sync
 * state. The daemon host comes from a command-line argument or a
 * rime_host.txt file next to the .exe.
 *
 * Cross-built from macOS/Linux with MinGW-w64 -- see build_win.sh.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <windowsx.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LATTICE_NO_MAIN
#include "../lattice_ref.c"            /* lattice_build_dataset, lattice_hash_ds */
#include "../keygen/rime_keygen.h"     /* rime_generate_address */
#include "../wallet/rime_wallet.h"     /* embedded wallet (Monero wallet_api) */
#include "../wallet/peer_cache.h"      /* seed + discovered RPC endpoints */

/* Forward decls for the tiny JSON readers defined further down; the peer
   discovery helper uses them before their definition. */
static int  json_get_str(const char *json, const char *key, char *out, int outsz);
static int  json_get_u64(const char *json, const char *key, uint64_t *out);

/* =====================================================================
 * Daemon: JSON-RPC over WinHTTP
 * ===================================================================== */
/* Wallet RPC goes through the Cloudflare proxy (same path as the miner). The
   Worker fails over between nodes, so a Hetzner traffic-scrub on one VM
   doesn't take the wallet offline. wallet2 connects over TLS on :443 via
   its e_ssl_support_autodetect default. NODE_PORT only affects the wallet
   daemon string -- http_post() ignores its port argument and always hits the
   Worker on HTTPS. */
#define NODE_PORT   443
#define WALLET_FILE "rimewallet"       /* wallet files, next to the .exe */

/* Default daemon host -- the Cloudflare proxy. Override with a command-line
   argument or a rime_host.txt file next to the .exe. */
static char g_host[64] = "glaciem-rpc.frostmine.workers.dev";

static void load_host(LPSTR cmd) {
  char tmp[64] = {0};
  if (cmd) {                                   /* 1) command-line argument */
    int i = 0; while (cmd[i]==' '||cmd[i]=='\t') i++;
    int j = 0; while (cmd[i] && cmd[i]!=' ' && cmd[i]!='\t' && j<63) tmp[j++]=cmd[i++];
    tmp[j] = 0;
  }
  if (!tmp[0]) {                               /* 2) rime_host.txt        */
    FILE *f = fopen("rime_host.txt","r");
    if (f) {
      if (fgets(tmp,sizeof tmp,f)) {
        int j = 0;
        while (tmp[j] && tmp[j]!='\r' && tmp[j]!='\n' && tmp[j]!=' ') j++;
        tmp[j] = 0;
      }
      fclose(f);
    }
  }
  if (tmp[0]) snprintf(g_host,sizeof g_host,"%s",tmp);
}

/* ---- v1.1.7 pool mode -----------------------------------------------
 *
 * When pool mode is enabled, the mining loop fetches jobs from
 *   POST {pool_url}/pool/job   { "wallet": <payout addr> }
 * and submits shares to
 *   POST {pool_url}/pool/submit { "job_id", "wallet", "nonce", "full_block" }
 * instead of talking to a daemon directly.
 *
 * Settings are persisted to rime_pool.txt (one URL per line; empty file =
 * pool mode off). A Pool button in the header opens the config dialog.
 */
static int  g_pool_enabled    = 0;
static char g_pool_url[256]   = "https://glaciem-pool.frostmine.workers.dev";
/* Parsed once from g_pool_url. */
static char g_pool_host[128]  = "glaciem-pool.frostmine.workers.dev";
static int  g_pool_port       = 443;
static int  g_pool_use_ssl    = 1;
static char g_pool_path_prefix[64] = "";  /* if URL has a subpath, e.g. "/pool" */

/* Parse `g_pool_url` (or any url) -> g_pool_host/port/use_ssl/path_prefix.
 * Lenient -- doesn't reject malformed URLs hard, just falls back to defaults. */
static void parse_pool_url(void) {
  const char *u = g_pool_url;
  int use_ssl = 1; int port = 443;
  if (strncmp(u, "https://", 8) == 0) { use_ssl = 1; port = 443; u += 8; }
  else if (strncmp(u, "http://", 7) == 0) { use_ssl = 0; port = 80; u += 7; }
  /* host[:port][/path] -- copy host until ':' or '/' or end */
  char host[128] = {0}; int i = 0;
  while (*u && *u != ':' && *u != '/' && i < (int)sizeof(host)-1) host[i++] = *u++;
  host[i] = 0;
  if (*u == ':') {
    u++;
    char pbuf[8] = {0}; int j = 0;
    while (*u >= '0' && *u <= '9' && j < 7) pbuf[j++] = *u++;
    pbuf[j] = 0;
    if (pbuf[0]) port = atoi(pbuf);
  }
  /* path prefix (e.g. "/pool" if the user pasted .../pool by accident);
   * we'll suffix /pool/job ourselves, so strip a trailing /pool if present. */
  char path[64] = {0}; int k = 0;
  while (*u && k < (int)sizeof(path)-1) path[k++] = *u++;
  path[k] = 0;
  /* trim trailing slash */
  while (k > 0 && path[k-1] == '/') { path[--k] = 0; }
  /* if user pasted ".../pool", strip it -- we'll append /pool/job ourselves */
  if (k >= 5 && strcmp(path + k - 5, "/pool") == 0) { path[k - 5] = 0; }
  if (host[0]) snprintf(g_pool_host, sizeof g_pool_host, "%s", host);
  g_pool_port    = port;
  g_pool_use_ssl = use_ssl;
  snprintf(g_pool_path_prefix, sizeof g_pool_path_prefix, "%s", path);
}

static void load_pool_config(void) {
  FILE *f = fopen("rime_pool.txt","r");
  if (!f) { g_pool_enabled = 0; parse_pool_url(); return; }
  char line[256] = {0};
  if (fgets(line, sizeof line, f)) {
    /* trim trailing CR/LF/whitespace */
    int n = (int)strlen(line);
    while (n > 0 && (line[n-1]=='\n' || line[n-1]=='\r' || line[n-1]==' ' || line[n-1]=='\t'))
      line[--n] = 0;
    if (n > 0) {
      g_pool_enabled = 1;
      snprintf(g_pool_url, sizeof g_pool_url, "%s", line);
    } else {
      g_pool_enabled = 0;
    }
  }
  fclose(f);
  parse_pool_url();
}

/* ---- peer cache: seeds + discovered peers, persisted to rime_peers.json
   next to the .exe. Same cache feeds the miner-RPC path and the wallet
   failover. Seeds are always present and tried first; discovered peers
   come from periodic get_peer_list calls and survive across launches. ---- */
static PeerCache *g_peers = NULL;

static int peers_load_cb(char *out, int cap, void *ctx) {
  (void)ctx;
  FILE *f = fopen("rime_peers.json", "rb");
  if (!f) return 0;
  size_t n = fread(out, 1, (size_t)cap - 1, f);
  fclose(f);
  out[n] = 0;
  return n > 0;
}
static void peers_save_cb(const char *json, void *ctx) {
  (void)ctx;
  FILE *f = fopen("rime_peers.json", "wb");
  if (!f) return;
  fwrite(json, 1, strlen(json), f);
  fclose(f);
}
static void peers_init_once(void) {
  static volatile LONG initialized = 0;
  if (InterlockedCompareExchange(&initialized, 1, 0) != 0) return;
  g_peers = peer_cache_new(peers_load_cb, peers_save_cb, NULL);
  peer_cache_add_seed(g_peers, "glaciem-rpc.frostmine.workers.dev", 443, 1);
  peer_cache_add_seed(g_peers, "static.197.125.225.46.clients.your-server.de", 19081, 0);
  peer_cache_add_seed(g_peers, "static.34.142.105.178.clients.your-server.de", 19081, 0);
}

/* Convert UTF-8 host -> UTF-16 for WinHttpConnect. Lifetime: caller owns
   the returned buffer; pass it back to free_wide(). */
static wchar_t *host_to_wide(const char *host) {
  int n = MultiByteToWideChar(CP_UTF8, 0, host, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
  if (!w) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, host, -1, w, n);
  return w;
}
static void free_wide(wchar_t *w) { free(w); }

/* POST to a single endpoint at a specific path; 1 = success. */
static int http_post_ep(const PeerEntry *ep, const wchar_t *path,
                        const char *body, char *resp, int resp_sz) {
  int ok = 0;
  wchar_t *whost = host_to_wide(ep->host);
  if (!whost) return 0;
  HINTERNET hs = WinHttpOpen(L"RimeMiner/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) { free_wide(whost); return 0; }
  WinHttpSetTimeouts(hs, 4000, 4000, 5000, 6000);
  HINTERNET hc = WinHttpConnect(hs, whost, (INTERNET_PORT)ep->port, 0);
  if (hc) {
    DWORD flags = ep->use_ssl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hr = WinHttpOpenRequest(hc, L"POST", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (hr) {
      DWORD body_len = body ? (DWORD)strlen(body) : 0;
      if (WinHttpSendRequest(hr, L"Content-Type: application/json\r\n", (DWORD)-1,
                             (void*)body, body_len, body_len, 0) &&
          WinHttpReceiveResponse(hr, NULL)) {
        DWORD status = 0, slen = sizeof status;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &status, &slen, NULL);
        if (status >= 200 && status < 400) {
          int got = 0; DWORD avail = 0;
          while (WinHttpQueryDataAvailable(hr,&avail) && avail > 0) {
            if (got + (int)avail > resp_sz-1) avail = (DWORD)(resp_sz-1-got);
            if ((int)avail <= 0) break;
            DWORD rd = 0;
            if (!WinHttpReadData(hr, resp+got, avail, &rd) || rd == 0) break;
            got += (int)rd;
          }
          resp[got] = 0;
          ok = (got > 0);
        }
      }
      WinHttpCloseHandle(hr);
    }
    WinHttpCloseHandle(hc);
  }
  WinHttpCloseHandle(hs);
  free_wide(whost);
  return ok;
}

/* Periodic peer discovery: ask the endpoint that just answered for its
   peer list, add any peers advertising an rpc_port to the cache. */
static void try_discover_peers(const PeerEntry *source) {
  if (!peer_cache_should_discover(g_peers)) return;
  char resp[16384] = {0};
  if (!http_post_ep(source, L"/get_peer_list", "", resp, (int)sizeof resp)) {
    peer_cache_reset_discovery_counter(g_peers);
    return;
  }
  /* Hand-walk the white_list: {"host":"...","rpc_port":N,...} per entry. */
  const char *p = strstr(resp, "\"white_list\"");
  int added = 0;
  while (p && added < 4) {
    p = strstr(p, "{");
    if (!p) break;
    const char *end = strstr(p, "}");
    if (!end) break;
    char host[128] = {0};
    int rpc_port = 0;
    json_get_str(p, "host", host, (int)sizeof host);
    uint64_t v = 0;
    if (json_get_u64(p, "rpc_port", &v)) rpc_port = (int)v;
    if (host[0] && rpc_port > 0 && rpc_port <= 65535) {
      peer_cache_add_discovered(g_peers, host, rpc_port);
      added++;
    }
    p = end + 1;
  }
  peer_cache_reset_discovery_counter(g_peers);
}

/* JSON-RPC: snapshot the cache, try endpoints in order, update scores. */
static int http_post(int port, const char *body, char *resp, int resp_sz) {
  (void)port;
  peers_init_once();
  PeerEntry snap[64];
  int n = peer_cache_snapshot(g_peers, snap, 64);
  for (int i = 0; i < n; i++) {
    DWORD t0 = GetTickCount();
    if (http_post_ep(&snap[i], L"/json_rpc", body, resp, resp_sz)) {
      peer_cache_mark_success(g_peers, snap[i].host, snap[i].port,
                               (int)(GetTickCount() - t0));
      /* v1.1.4: try_discover_peers() removed -- the public proxy now
         403s /get_peer_list (admin/debug endpoint that shouldn't be
         exposed), so this call always failed silently and was wasted
         bandwidth. Peers come from seeded endpoints only now. */
      return 1;
    }
    peer_cache_mark_failure(g_peers, snap[i].host, snap[i].port);
  }
  return 0;
}

/* JSON-RPC call -> full response body in `resp`. 1 on success. */
static int rpc_call(int port, const char *method, const char *params,
                    char *resp, int resp_sz) {
  char body[8192];
  snprintf(body,sizeof body,
    "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"%s\",\"params\":%s}",
    method, params ? params : "{}");
  return http_post(port, body, resp, resp_sz);
}

/* ---- v1.1.7 pool HTTP helpers --------------------------------------------
 *
 * Pool endpoints take a flat JSON body (not JSON-RPC envelope). We
 * synthesize a one-shot PeerEntry from the parsed g_pool_host/port/ssl
 * and reuse http_post_ep for the actual TLS + POST work. */

static int http_post_pool(const char *path_suffix, const char *body,
                          char *resp, int resp_sz) {
  PeerEntry ep;
  memset(&ep, 0, sizeof ep);
  snprintf(ep.host, sizeof ep.host, "%s", g_pool_host);
  ep.port    = g_pool_port;
  ep.use_ssl = g_pool_use_ssl;

  /* Build the full path: prefix (if any) + suffix ("/pool/job" etc) */
  char path_a[160];
  snprintf(path_a, sizeof path_a, "%s%s", g_pool_path_prefix, path_suffix);
  /* WinHTTP wants wide-string paths. */
  wchar_t wpath[160];
  int wn = MultiByteToWideChar(CP_UTF8, 0, path_a, -1, wpath,
                                sizeof(wpath)/sizeof(wpath[0]));
  if (wn <= 0) return 0;

  return http_post_ep(&ep, wpath, body, resp, resp_sz);
}

/* POST /pool/job -> response body in `resp`. 1 on success.
 * Caller parses share_difficulty, network_difficulty, job_id, etc. */
static int pool_get_job(const char *wallet, char *resp, int resp_sz) {
  char body[512];
  snprintf(body, sizeof body, "{\"wallet\":\"%s\"}", wallet ? wallet : "");
  return http_post_pool("/pool/job", body, resp, resp_sz);
}

/* POST /pool/submit. full_block=1 tells the pool the nonce also meets
 * network difficulty and it should forward as submitblock. */
static int pool_submit_share(const char *job_id, const char *wallet,
                             uint32_t nonce, int full_block,
                             char *resp, int resp_sz) {
  char body[512];
  snprintf(body, sizeof body,
    "{\"job_id\":\"%s\",\"wallet\":\"%s\",\"nonce\":%u,\"full_block\":%s}",
    job_id ? job_id : "",
    wallet ? wallet : "",
    (unsigned)nonce,
    full_block ? "true" : "false");
  return http_post_pool("/pool/submit", body, resp, resp_sz);
}

/* tiny JSON field readers -- the Monero RPC replies we parse are flat. */
static int json_get_str(const char *json, const char *key, char *out, int outsz) {
  char needle[64];
  snprintf(needle,sizeof needle,"\"%s\"",key);
  const char *p = strstr(json,needle);
  if (!p) return 0;
  p += strlen(needle);
  while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
  if (*p++ != ':') return 0;
  while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
  if (*p++ != '"') return 0;
  int i = 0;
  while (*p && *p!='"' && i<outsz-1) out[i++]=*p++;
  out[i] = 0;
  return 1;
}
static int json_get_u64(const char *json, const char *key, uint64_t *out) {
  char needle[64];
  snprintf(needle,sizeof needle,"\"%s\"",key);
  const char *p = strstr(json,needle);
  if (!p) return 0;
  p += strlen(needle);
  while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
  if (*p++ != ':') return 0;
  while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
  uint64_t v = 0; int any = 0;
  while (*p>='0' && *p<='9') { v = v*10 + (uint64_t)(*p-'0'); p++; any = 1; }
  if (!any) return 0;
  *out = v;
  return 1;
}

/* =====================================================================
 * Hex / target helpers
 * ===================================================================== */
static int hexval(char c) {
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return c-'a'+10;
  if (c>='A'&&c<='F') return c-'A'+10;
  return -1;
}
static int hex2bin(const char *hex, u8 *out, int maxlen) {
  int n = (int)strlen(hex);
  if (n & 1) return -1;
  n /= 2;
  if (n > maxlen) return -1;
  for (int i=0;i<n;i++) {
    int hi=hexval(hex[i*2]), lo=hexval(hex[i*2+1]);
    if (hi<0||lo<0) return -1;
    out[i] = (u8)((hi<<4)|lo);
  }
  return n;
}
static void bin2hex(const u8 *b, int n, char *out) {
  static const char H[] = "0123456789abcdef";
  for (int i=0;i<n;i++) { out[i*2]=H[b[i]>>4]; out[i*2+1]=H[b[i]&15]; }
  out[n*2] = 0;
}

/* offset of the 4-byte nonce in a block blob: 3 leading varints + 32B prev_id */
static int blob_nonce_off(const u8 *blob, int len) {
  int p = 0;
  for (int v=0; v<3 && p<len; v++) {
    int n = 1;
    while (p+n-1<len && (blob[p+n-1]&0x80)) n++;
    p += n;
  }
  return p + 32;
}
/* hash (32B little-endian 256-bit) meets target iff hash*difficulty < 2^256 */
static int meets_target(const u8 h[32], uint64_t difficulty) {
  if (difficulty <= 1) return 1;
  uint64_t w[4];
  for (int i=0;i<4;i++) {
    uint64_t x=0; for(int b=0;b<8;b++) x|=(uint64_t)h[i*8+b]<<(8*b);
    w[i]=x;
  }
  unsigned __int128 carry = 0;
  for (int i=0;i<4;i++) {
    unsigned __int128 pr = (unsigned __int128)w[i]*difficulty + carry;
    carry = pr >> 64;
  }
  return carry == 0;
}
/* leading-zero bits of a 32-byte little-endian hash -- a mining-progress gauge */
static int lz_bits(const u8 out[32]) {
  int n=0;
  for (int i=31;i>=0;i--) {
    if (out[i]==0) { n+=8; continue; }
    for (int b=7;b>=0;b--) { if (out[i]&(1<<b)) return n; n++; }
  }
  return n;
}

static double now_s(void) {
  static LARGE_INTEGER f; static int init = 0;
  LARGE_INTEGER c;
  if (!init) { QueryPerformanceFrequency(&f); init = 1; }
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
}

/* =====================================================================
 * CPU-only Lattice mining
 * ===================================================================== */
#define MAX_THREADS 32
#define CHUNK       64          /* nonces hashed per thread per batch */

static u64 *g_dataset;          /* 4 MiB epoch dataset, shared read-only */
static int  g_threads;

/* one worker hashes a contiguous run of nonces with Lattice */
typedef struct {
  const u8 *hb; int hb_len; int noff;
  uint64_t difficulty;
  uint32_t nonce_start; int count;
  int      winner;          /* 1 if a nonce met target, else 0 */
  uint32_t winner_nonce;
  int      best_bits;
} mjob_t;

static unsigned __stdcall mine_worker(void *arg) {
  mjob_t *j = (mjob_t*)arg;
  u8 blob[256];
  memcpy(blob, j->hb, (size_t)j->hb_len);
  j->winner = 0; j->winner_nonce = 0; j->best_bits = 0;
  for (int i=0;i<j->count;i++) {
    uint32_t nn = j->nonce_start + (uint32_t)i;
    blob[j->noff+0]=(u8)nn;        blob[j->noff+1]=(u8)(nn>>8);
    blob[j->noff+2]=(u8)(nn>>16);  blob[j->noff+3]=(u8)(nn>>24);
    u8 h[32];
    lattice_hash_ds(blob, (size_t)j->hb_len, g_dataset, h);
    int z = lz_bits(h);
    if (z > j->best_bits) j->best_bits = z;
    if (!j->winner && meets_target(h, j->difficulty)) {
      j->winner = 1; j->winner_nonce = nn;
    }
  }
  return 0;
}

/* =====================================================================
 * Shared state between the worker thread and the UI
 * ===================================================================== */
typedef struct {
  volatile LONG running;     /* 1 while the user wants to mine */
  CRITICAL_SECTION cs;
  double   hashrate;
  uint64_t total;
  int      best_bits;
  char     device[64];       /* "CPU - N threads" */
  char     status[200];
  /* daemon mining */
  int      daemon_connected;
  uint64_t blocks_found;
  uint64_t height;
  uint64_t difficulty;
  /* wallet */
  int      wallet_connected;
  int      wallet_syncing;
  uint64_t balance;
  uint64_t wallet_height;
  uint64_t target_height;
  char     wallet_addr[160];
} Shared;
static Shared g_sh;
static HANDLE g_worker;

/* Embedded wallet -- opened, refreshed and closed only by the wallet poll
   thread, so the handle itself needs no locking. The generate flow hands a
   fresh 25-word seed across via g_pending_seed; the poll thread does the
   (re)open, which keeps all wallet_api calls on one thread. */
static RimeWallet   *g_wallet;
static char          g_pending_seed[640];
static volatile LONG g_seed_pending;

/* main window handle (for worker-thread dialogs) + a pending Send, which the
   wallet poll thread executes so wallet_api is only ever touched by it. */
static HWND          g_hwnd;
static char          g_send_addr[200];
static double        g_send_amount;
static volatile LONG g_send_pending;
static volatile LONG g_sweep_pending;
static volatile LONG g_history_pending;

static void sh_status(const char *s) {
  EnterCriticalSection(&g_sh.cs);
  snprintf(g_sh.status,sizeof g_sh.status,"%s",s);
  LeaveCriticalSection(&g_sh.cs);
}

static unsigned __stdcall mine_thread(void *arg) {
  (void)arg;

  SYSTEM_INFO si; GetSystemInfo(&si);
  g_threads = (int)si.dwNumberOfProcessors;
  if (g_threads < 1) g_threads = 1;
  if (g_threads > MAX_THREADS) g_threads = MAX_THREADS;
  EnterCriticalSection(&g_sh.cs);
  snprintf(g_sh.device,sizeof g_sh.device,"CPU - %d threads",g_threads);
  LeaveCriticalSection(&g_sh.cs);

  if (!g_dataset) g_dataset = malloc(DATASET_WORDS*sizeof(u64));
  if (!g_dataset) { sh_status("out of memory"); g_sh.running=0; return 0; }

  sh_status("Mining");
  double hr = 0;
  uint64_t total = 0, blocks = 0;
  int best = 0;
  u8 cur_seed[32]; int have_seed = 0;
  uint32_t base = (uint32_t)(now_s()*131.0);
  char resp[16384];

  while (g_sh.running) {
    /* mine to the embedded wallet's address. No fallback -- with no wallet
       generated yet, the miner refuses to mine. */
    char waddr[160];
    EnterCriticalSection(&g_sh.cs);
    snprintf(waddr,sizeof waddr,"%s",g_sh.wallet_addr);
    LeaveCriticalSection(&g_sh.cs);

    if (!waddr[0]) {
      EnterCriticalSection(&g_sh.cs);
      g_sh.daemon_connected=0; g_sh.hashrate=0;
      LeaveCriticalSection(&g_sh.cs);
      sh_status("Generate an address to mine");
      for (int i=0;i<12 && g_sh.running;i++) Sleep(100);
      continue;
    }

    /* v1.1.7: branch on pool mode. Pool jobs use share_difficulty (much
     * easier than network) and we track network_difficulty separately so
     * we can flag full_block=true when a share also solves the block. */
    int pool_mode_now = g_pool_enabled;
    char hbHex[1024], tbHex[8192], seedHex[80], job_id[64];
    job_id[0] = 0;
    int got_template = 0;
    if (pool_mode_now) {
      char body[256];
      snprintf(body, sizeof body, "{\"wallet\":\"%s\"}", waddr);
      if (http_post_pool("/pool/job", body, resp, sizeof resp) &&
          json_get_str(resp, "blockhashing_blob", hbHex, sizeof hbHex) &&
          json_get_str(resp, "blocktemplate_blob", tbHex, sizeof tbHex)) {
        json_get_str(resp, "job_id", job_id, sizeof job_id);
        got_template = 1;
      }
    } else {
      char params[256];
      snprintf(params,sizeof params,
        "{\"wallet_address\":\"%s\",\"reserve_size\":8}", waddr);
      if (rpc_call(NODE_PORT,"get_block_template",params,resp,sizeof resp) &&
          json_get_str(resp,"blockhashing_blob",hbHex,sizeof hbHex) &&
          json_get_str(resp,"blocktemplate_blob",tbHex,sizeof tbHex)) {
        got_template = 1;
      }
    }
    if (!got_template) {
      EnterCriticalSection(&g_sh.cs);
      g_sh.daemon_connected=0; g_sh.hashrate=0;
      LeaveCriticalSection(&g_sh.cs);
      sh_status(pool_mode_now ? "No pool" : "No daemon");
      for (int i=0;i<12 && g_sh.running;i++) Sleep(100);
      continue;
    }
    uint64_t height=0, diff=0, net_diff=0;
    json_get_u64(resp,"height",&height);
    if (pool_mode_now) {
      json_get_u64(resp,"share_difficulty",&diff);
      json_get_u64(resp,"network_difficulty",&net_diff);
    } else {
      json_get_u64(resp,"difficulty",&diff);
      net_diff = diff;
    }
    if (!json_get_str(resp,"seed_hash",seedHex,sizeof seedHex)) seedHex[0]=0;

    u8 hb[256], tb[4096];
    int hb_len = hex2bin(hbHex,hb,sizeof hb);
    int tb_len = hex2bin(tbHex,tb,sizeof tb);
    if (hb_len<=0 || tb_len<=0 || hb_len>250) { Sleep(200); continue; }
    int noff = blob_nonce_off(hb,hb_len);
    if (noff+4 > hb_len) { Sleep(200); continue; }

    EnterCriticalSection(&g_sh.cs);
    g_sh.daemon_connected=1; g_sh.height=height; g_sh.difficulty=diff;
    LeaveCriticalSection(&g_sh.cs);
    sh_status("Mining");

    /* (re)build the epoch dataset when the template's seed changes */
    u8 seed[32]; memset(seed,0,sizeof seed);
    if (seedHex[0]) hex2bin(seedHex,seed,32);
    if (!have_seed || memcmp(seed,cur_seed,32)!=0) {
      sh_status("Building dataset...");
      lattice_build_dataset(seed,g_dataset);
      memcpy(cur_seed,seed,32); have_seed=1;
      sh_status("Mining");
    }

    /* hash one batch -- g_threads workers, CHUNK nonces each */
    HANDLE th[MAX_THREADS]; mjob_t jb[MAX_THREADS];
    double t0 = now_s();
    for (int t=0;t<g_threads;t++) {
      jb[t].hb=hb; jb[t].hb_len=hb_len; jb[t].noff=noff;
      jb[t].difficulty=diff;
      jb[t].nonce_start = base + (uint32_t)(t*CHUNK);
      jb[t].count = CHUNK;
      th[t]=(HANDLE)_beginthreadex(NULL,0,mine_worker,&jb[t],0,NULL);
      /* below-normal so the all-core hashing never starves the UI thread */
      if (th[t]) SetThreadPriority(th[t],THREAD_PRIORITY_BELOW_NORMAL);
    }
    WaitForMultipleObjects(g_threads,th,TRUE,INFINITE);

    int have_winner=0; uint32_t winner_nonce=0;
    for (int t=0;t<g_threads;t++) {
      CloseHandle(th[t]);
      if (jb[t].best_bits>best) best=jb[t].best_bits;
      if (jb[t].winner) {
        if (!have_winner || jb[t].winner_nonce<winner_nonce) {
          have_winner=1; winner_nonce=jb[t].winner_nonce;
        }
      }
    }

    int batch = g_threads*CHUNK;
    double dt = now_s()-t0; if (dt<=0) dt=1e-6;
    double inst = batch/dt;
    hr = (hr<=0) ? inst : 0.8*hr + 0.2*inst;
    total += (uint64_t)batch;
    EnterCriticalSection(&g_sh.cs);
    g_sh.hashrate=hr; g_sh.total=total; g_sh.best_bits=best;
    LeaveCriticalSection(&g_sh.cs);

    /* submit the winning nonce */
    if (have_winner && noff+4<=tb_len) {
      if (pool_mode_now) {
        /* v1.1.7: re-hash with the winning nonce against network_diff to
         * decide whether to flag full_block=true (so the pool forwards
         * to the daemon as submitblock). */
        u8 hb_check[256]; memcpy(hb_check, hb, hb_len);
        hb_check[noff+0]=(u8)winner_nonce;
        hb_check[noff+1]=(u8)(winner_nonce>>8);
        hb_check[noff+2]=(u8)(winner_nonce>>16);
        hb_check[noff+3]=(u8)(winner_nonce>>24);
        u8 mh[32];
        lattice_hash_ds(hb_check, hb_len, g_dataset, mh);
        int is_full = (net_diff > 0 && meets_target(mh, net_diff)) ? 1 : 0;
        char sr[2048];
        if (pool_submit_share(job_id, waddr, winner_nonce, is_full, sr, sizeof sr)) {
          int got_block = strstr(sr, "\"block\":true") != NULL;
          if (got_block) blocks++;
        }
      } else {
        tb[noff+0]=(u8)winner_nonce;       tb[noff+1]=(u8)(winner_nonce>>8);
        tb[noff+2]=(u8)(winner_nonce>>16); tb[noff+3]=(u8)(winner_nonce>>24);
        char *blockHex = malloc((size_t)tb_len*2+1);
        char *sp = malloc((size_t)tb_len*2+8);
        if (blockHex && sp) {
          bin2hex(tb,tb_len,blockHex);
          snprintf(sp,(size_t)tb_len*2+8,"[\"%s\"]",blockHex);
          char sr[2048];
          if (rpc_call(NODE_PORT,"submit_block",sp,sr,sizeof sr)) {
            char stt[16];
            if (json_get_str(sr,"status",stt,sizeof stt) && strcmp(stt,"OK")==0)
              blocks++;
          }
        }
        free(blockHex); free(sp);
      }
      EnterCriticalSection(&g_sh.cs);
      g_sh.blocks_found = blocks;
      LeaveCriticalSection(&g_sh.cs);
    }
    base += (uint32_t)batch;
  }
  sh_status("Idle");
  return 0;
}

/* Wallet failover uses the same peer cache as the miner. Seeds first,
   discovered peers second (sorted by score). */
#define WALLET_FAILOVER_THRESHOLD 3

/* embedded-wallet poll: owns g_wallet -- opens/recovers it, refreshes it,
   and publishes address/balance/sync into g_sh. Runs for the app's
   lifetime so the wallet panel is live even while not mining. */
static unsigned __stdcall wallet_thread(void *arg) {
  (void)arg;
  peers_init_once();
  char daemon[96];
  snprintf(daemon,sizeof daemon,"%s:%d",g_host,NODE_PORT);
  int wallet_disconnects = 0, wallet_ep_idx = 0;

  /* open a previously-generated wallet, if one sits next to the .exe */
  g_wallet = rime_wallet_recover(WALLET_FILE, "", daemon, 0);

  while (1) {
    /* a freshly generated 25-word seed waiting to become the wallet */
    if (g_seed_pending) {
      char seed[640];
      EnterCriticalSection(&g_sh.cs);
      snprintf(seed,sizeof seed,"%s",g_pending_seed);
      g_seed_pending = 0;
      LeaveCriticalSection(&g_sh.cs);
      if (g_wallet) { rime_wallet_close(g_wallet); g_wallet = NULL; }
      remove(WALLET_FILE);
      remove(WALLET_FILE ".keys");
      remove(WALLET_FILE ".address.txt");
      g_wallet = rime_wallet_recover(WALLET_FILE, seed, daemon, 0);
    }

    /* a Send requested from the dialog -- executed here, on the wallet's
       owning thread, so wallet_api is never touched from two threads */
    if (g_send_pending) {
      char sa[200]; double amt;
      EnterCriticalSection(&g_sh.cs);
      snprintf(sa,sizeof sa,"%s",g_send_addr); amt=g_send_amount;
      g_send_pending = 0;
      LeaveCriticalSection(&g_sh.cs);
      char result[300] = "";
      if (g_wallet) {
        unsigned long long atomic=(unsigned long long)(amt*1e12+0.5);
        rime_wallet_send(g_wallet, sa, atomic, result, sizeof result);
      } else {
        snprintf(result,sizeof result,"No wallet open.");
      }
      MessageBoxA(g_hwnd, result, "Glaciem Miner - Send", MB_OK|MB_ICONINFORMATION);
    }

    /* a Sweep requested from the dialog -- run on the wallet's owning thread */
    if (g_sweep_pending) {
      g_sweep_pending = 0;
      char result[300] = "";
      if (g_wallet) {
        rime_wallet_sweep_unmixable(g_wallet, result, sizeof result);
      } else {
        snprintf(result,sizeof result,"No wallet open.");
      }
      MessageBoxA(g_hwnd, result, "Glaciem Miner - Sweep", MB_OK|MB_ICONINFORMATION);
    }

    /* a History request -- run on the wallet's owning thread */
    if (g_history_pending) {
      g_history_pending = 0;
      char hist[8192] = "";
      if (g_wallet) rime_wallet_history(g_wallet, hist, sizeof hist);
      else          snprintf(hist,sizeof hist,"No wallet open.");
      MessageBoxA(g_hwnd, hist, "Glaciem Miner - History", MB_OK|MB_ICONINFORMATION);
    }

    if (g_wallet) {
      char addr[160] = "";
      /* publish cached address + balance first (available immediately) */
      rime_wallet_address(g_wallet, addr, sizeof addr);
      EnterCriticalSection(&g_sh.cs);
      snprintf(g_sh.wallet_addr,sizeof g_sh.wallet_addr,"%s",addr);
      g_sh.balance = rime_wallet_balance(g_wallet);
      LeaveCriticalSection(&g_sh.cs);

      rime_wallet_refresh(g_wallet);                 /* blocking chain scan */
      int conn   = rime_wallet_connected(g_wallet);
      int synced = rime_wallet_synchronized(g_wallet);
      uint64_t bal = rime_wallet_balance(g_wallet);
      uint64_t wht = rime_wallet_height(g_wallet);
      uint64_t tgt = rime_wallet_daemon_height(g_wallet);
      /* stranded ahead of the daemon -> scanned a dead fork (testnet reset);
         a plain refresh can't roll that back, so force a rescan. */
      if (conn && tgt>0 && wht>tgt) {
        rime_wallet_rescan(g_wallet);
        synced = rime_wallet_synchronized(g_wallet);
        bal    = rime_wallet_balance(g_wallet);
        wht    = rime_wallet_height(g_wallet);
        tgt    = rime_wallet_daemon_height(g_wallet);
      }
      rime_wallet_store(g_wallet);
      rime_wallet_address(g_wallet, addr, sizeof addr);
      EnterCriticalSection(&g_sh.cs);
      g_sh.wallet_connected = conn;
      g_sh.balance          = bal;
      g_sh.wallet_height    = wht;
      g_sh.target_height    = tgt;
      g_sh.wallet_syncing   = (conn && !synced) ? 1 : 0;
      snprintf(g_sh.wallet_addr,sizeof g_sh.wallet_addr,"%s",addr);
      LeaveCriticalSection(&g_sh.cs);

      /* Failover: after N consecutive disconnects, snapshot the peer
         cache (seeds + discovered, in score order) and rotate. */
      if (conn) {
        wallet_disconnects = 0;
      } else if (++wallet_disconnects >= WALLET_FAILOVER_THRESHOLD) {
        PeerEntry snap[64];
        int n = peer_cache_snapshot(g_peers, snap, 64);
        if (n > 0) {
          wallet_ep_idx = (wallet_ep_idx + 1) % n;
          char d[200];
          snprintf(d, sizeof d, "%s:%d",
                   snap[wallet_ep_idx].host, snap[wallet_ep_idx].port);
          rime_wallet_set_daemon(g_wallet, d);
        }
        wallet_disconnects = 0;
      }
    } else {
      EnterCriticalSection(&g_sh.cs);
      g_sh.wallet_connected = 0;
      LeaveCriticalSection(&g_sh.cs);
    }
    /* v1.1.4: ~20s between refreshes (was 4s). Block time is ~120s so
       the wallet still feels live in the UI, but /getblocks.bin traffic
       to the public RPC proxy drops ~5x. Send/sweep/history paths still
       wake the loop immediately via the pending flags. */
    for (int i=0;i<200 && !g_send_pending && !g_sweep_pending && !g_history_pending && !g_seed_pending;i++) Sleep(100);
  }
  return 0;
}

/* =====================================================================
 * Win32 GUI
 *
 * The dashboard is drawn into a fixed DW x DH "design canvas" bitmap,
 * which is then scaled to fit a resizable window (StretchBlt, aspect-
 * preserved). So the whole UI is always fully visible at any window
 * size or display DPI -- it fits small tablet screens. The two buttons
 * are painted into the canvas and hit-tested on click.
 * ===================================================================== */
#define DW 440          /* design-canvas width  */
#define DH 690          /* design-canvas height */

static HFONT g_fTitle, g_fBig, g_fMid, g_fSmall;
static const COLORREF C_BG    = RGB(13,13,18);
static const COLORREF C_CARD  = RGB(26,26,33);
static const COLORREF C_AMBER = RGB(63,193,224);
static const COLORREF C_WHITE = RGB(240,240,245);
static const COLORREF C_DIM   = RGB(120,120,132);
static const COLORREF C_GREEN = RGB(70,200,110);
static const COLORREF C_RED   = RGB(230,80,80);
static const COLORREF C_BTN   = RGB(46,46,57);

/* button rects, in design-canvas coordinates */
static const RECT R_GEN     = {  40, 544, 230, 588 };
static const RECT R_RESTORE = { 244, 544, DW-40, 588 };
static const RECT R_MINE = { 40, 600, DW-40, 656 };
static const RECT R_RECV = {  40, 472, 153, 502 };      /* inside the wallet card */
static const RECT R_SEND = { 163, 472, 276, 502 };
static const RECT R_HIST = { 286, 472, DW-40, 502 };
static const RECT R_HOST = { DW-128, 18, DW-24, 48 };   /* header, top-right */
static const RECT R_POOL = { DW-238, 18, DW-138, 48 };  /* left of HOST */

static void draw_text(HDC dc, const char *s, int x, int y, int w,
                      HFONT f, COLORREF col, UINT align) {
  RECT r={x,y,x+w,y+200};
  SelectObject(dc,f);
  SetTextColor(dc,col);
  SetBkMode(dc,TRANSPARENT);
  DrawTextA(dc,s,-1,&r,align|DT_SINGLELINE|DT_NOPREFIX);
}

static void draw_button(HDC dc, const RECT *rc, COLORREF fill,
                        const char *text, HFONT f, COLORREF textcol) {
  RECT r=*rc;
  HBRUSH b=CreateSolidBrush(fill);
  FillRect(dc,&r,b); DeleteObject(b);
  SelectObject(dc,f);
  SetBkMode(dc,TRANSPARENT);
  SetTextColor(dc,textcol);
  DrawTextA(dc,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}

/* 1 if mining can proceed: a generated/configured address, or a wallet */
static int has_mining_address(void) {
  int ok;
  EnterCriticalSection(&g_sh.cs);
  ok = (g_sh.wallet_addr[0] != 0);
  LeaveCriticalSection(&g_sh.cs);
  return ok;
}

/* scale + offset that fits the DW x DH canvas into a W x H client area,
   preserving aspect ratio (letterboxed) */
static void fit_calc(int W, int H, double *scale, int *ox, int *oy) {
  if (W<1) W=1;
  if (H<1) H=1;
  double sx=(double)W/DW, sy=(double)H/DH;
  double s = sx<sy ? sx : sy;
  if (s<=0.0) s=0.0001;
  *scale=s;
  *ox=(int)((W - DW*s)/2.0);
  *oy=(int)((H - DH*s)/2.0);
}

static void paint(HWND hwnd) {
  PAINTSTRUCT ps; HDC wdc=BeginPaint(hwnd,&ps);
  RECT cr; GetClientRect(hwnd,&cr);
  int W=cr.right<1?1:cr.right, H=cr.bottom<1?1:cr.bottom;

  /* --- render the dashboard into the fixed DW x DH design canvas --- */
  HDC dc=CreateCompatibleDC(wdc);
  HBITMAP bm=CreateCompatibleBitmap(wdc,DW,DH);
  HBITMAP old=SelectObject(dc,bm);

  HBRUSH bg=CreateSolidBrush(C_BG);
  RECT full={0,0,DW,DH}; FillRect(dc,&full,bg); DeleteObject(bg);

  /* snapshot shared state */
  Shared s;
  EnterCriticalSection(&g_sh.cs); s=g_sh; LeaveCriticalSection(&g_sh.cs);
  int running=g_sh.running;
  char buf[256];

  /* header */
  draw_text(dc,"GLACIEM",28,16,DW-56,g_fTitle,C_AMBER,DT_LEFT);
  {
    char subtitle[64];
    snprintf(subtitle, sizeof subtitle, "PROOF-OF-WORK MINER  \xB7  v%s",
             GLACIEM_VERSION);
    draw_text(dc,subtitle,30,54,DW-60,g_fSmall,C_DIM,DT_LEFT);
  }
  draw_button(dc,&R_HOST,C_BTN,"HOST",g_fSmall,C_AMBER);
  /* v1.1.7: "POOL" toggle. Label flips to indicate current mode. */
  draw_button(dc,&R_POOL,C_BTN,
              g_pool_enabled ? "POOL ON" : "POOL",
              g_fSmall, g_pool_enabled ? C_AMBER : 0x8a8a99);

  /* hashrate card */
  RECT card1={24,84,DW-24,258};
  HBRUSH cb1=CreateSolidBrush(C_CARD); FillRect(dc,&card1,cb1); DeleteObject(cb1);
  snprintf(buf,sizeof buf,"%.0f", s.hashrate);
  draw_text(dc,buf,24,96,DW-48,g_fBig,C_WHITE,DT_CENTER);
  draw_text(dc,"HASHES / SECOND",24,182,DW-48,g_fSmall,C_DIM,DT_CENTER);
  draw_text(dc, s.device[0]?s.device:"CPU",
            24,206,DW-48,g_fSmall,C_AMBER,DT_CENTER);

  /* status */
  {
    const char *st = s.status[0] ? s.status : "Idle";
    COLORREF sc;
    if (running && strcmp(st,"Mining")==0) sc=C_GREEN;
    else if (!running) sc=C_DIM;
    else if (strstr(st,"FAIL")||strstr(st,"not found")||strstr(st,"failed")) sc=C_RED;
    else sc=C_AMBER;
    draw_text(dc,st,24,228,DW-48,g_fSmall,sc,DT_CENTER);
  }

  /* stats row: blocks found / testnet height / best bits */
  {
    int third=(DW-48)/3;
    snprintf(buf,sizeof buf,"%llu",(unsigned long long)s.blocks_found);
    draw_text(dc,buf,24,272,third,g_fMid,C_WHITE,DT_CENTER);
    draw_text(dc,"BLOCKS FOUND",24,302,third,g_fSmall,C_DIM,DT_CENTER);
    snprintf(buf,sizeof buf,"%llu",(unsigned long long)s.height);
    draw_text(dc,buf,24+third,272,third,g_fMid,C_WHITE,DT_CENTER);
    draw_text(dc,"BLOCK HEIGHT",24+third,302,third,g_fSmall,C_DIM,DT_CENTER);
    snprintf(buf,sizeof buf,"%d",s.best_bits);
    draw_text(dc,buf,24+2*third,272,third,g_fMid,C_WHITE,DT_CENTER);
    draw_text(dc,"BEST BITS",24+2*third,302,third,g_fSmall,C_DIM,DT_CENTER);
  }

  /* wallet card */
  RECT card2={24,350,DW-24,510};
  HBRUSH cb2=CreateSolidBrush(C_CARD); FillRect(dc,&card2,cb2); DeleteObject(cb2);
  draw_text(dc,"WALLET",40,362,DW-80,g_fSmall,C_DIM,DT_LEFT);
  {
    const char *wst = !s.wallet_connected ? "NO WALLET"
                    : (s.wallet_syncing ? "SYNCING" : "CONNECTED");
    COLORREF wsc = !s.wallet_connected ? C_DIM
                 : (s.wallet_syncing ? C_AMBER : C_GREEN);
    draw_text(dc,wst,40,362,DW-80,g_fSmall,wsc,DT_RIGHT);
  }
  if (s.wallet_syncing) {
    draw_text(dc,"catching up...",40,386,DW-80,g_fMid,C_AMBER,DT_LEFT);
    unsigned long long pct = s.target_height
        ? (unsigned long long)(s.wallet_height*100/s.target_height) : 0;
    if (pct>100) pct=100;
    snprintf(buf,sizeof buf,"block %llu / %llu  (%llu%%)",
             (unsigned long long)s.wallet_height,
             (unsigned long long)s.target_height, pct);
    draw_text(dc,buf,40,418,DW-80,g_fSmall,C_DIM,DT_LEFT);
  } else {
    snprintf(buf,sizeof buf,"%.6f GLAC", (double)s.balance/1e12);
    draw_text(dc,buf,40,392,DW-80,g_fMid,C_WHITE,DT_LEFT);
  }
  {
    int al=(int)strlen(s.wallet_addr);
    if (al==0)        snprintf(buf,sizeof buf,"-");
    else if (al>24)   snprintf(buf,sizeof buf,"%.11s...%s",s.wallet_addr,
                               s.wallet_addr+al-11);
    else              snprintf(buf,sizeof buf,"%s",s.wallet_addr);
    draw_text(dc,buf,40,448,DW-80,g_fSmall,C_AMBER,DT_LEFT);
  }

  /* wallet actions */
  draw_button(dc,&R_RECV,C_BTN,"RECEIVE",g_fSmall,C_AMBER);
  draw_button(dc,&R_SEND,C_BTN,"SEND",g_fSmall,C_AMBER);
  draw_button(dc,&R_HIST,C_BTN,"HISTORY",g_fSmall,C_AMBER);

  /* daemon host */
  snprintf(buf,sizeof buf,"node  %s:%d", g_host, NODE_PORT);
  draw_text(dc,buf,24,522,DW-48,g_fSmall,C_DIM,DT_CENTER);

  /* buttons -- painted into the canvas, hit-tested on click */
  draw_button(dc,&R_GEN,C_CARD,"NEW WALLET",g_fSmall,C_AMBER);
  draw_button(dc,&R_RESTORE,C_CARD,"RESTORE SEED",g_fSmall,C_AMBER);
  {
    int blocked = !running && !has_mining_address();
    COLORREF fill = blocked?C_CARD:(running?C_RED:C_AMBER);
    COLORREF tc   = blocked?C_DIM:(running?C_WHITE:C_BG);
    const char *t = blocked?"NEW WALLET OR RESTORE SEED FIRST"
                           :(running?"STOP":"START MINING");
    draw_button(dc,&R_MINE,fill,t,blocked?g_fSmall:g_fMid,tc);
  }

  /* --- scale the canvas to fit the window (aspect-preserved) --- */
  double scale; int ox,oy;
  fit_calc(W,H,&scale,&ox,&oy);
  int sw=(int)(DW*scale), sh=(int)(DH*scale);
  if (sw<1) sw=1;
  if (sh<1) sh=1;

  HDC out=CreateCompatibleDC(wdc);
  HBITMAP obm=CreateCompatibleBitmap(wdc,W,H);
  HBITMAP oold=SelectObject(out,obm);
  HBRUSH lb=CreateSolidBrush(C_BG);
  RECT fr={0,0,W,H}; FillRect(out,&fr,lb); DeleteObject(lb);
  SetStretchBltMode(out,HALFTONE);
  SetBrushOrgEx(out,0,0,NULL);
  StretchBlt(out,ox,oy,sw,sh,dc,0,0,DW,DH,SRCCOPY);
  BitBlt(wdc,0,0,W,H,out,0,0,SRCCOPY);

  SelectObject(out,oold); DeleteObject(obm); DeleteDC(out);
  SelectObject(dc,old); DeleteObject(bm); DeleteDC(dc);
  EndPaint(hwnd,&ps);
}

/* GENERATE: make a fresh Rime wallet with the keygen library (Rime's real
   crypto), show the address + 25-word seed; on confirm, hand the seed to
   the wallet poll thread, which opens it as the app's embedded wallet. */
static void do_generate_address(HWND hwnd) {
  RimeKeypair k;
  if (!rime_generate_address(&k)) {
    MessageBoxA(hwnd, "Address generation failed.", "Glaciem Miner",
                MB_OK | MB_ICONERROR);
    return;
  }
  char msg[1100];
  snprintf(msg, sizeof msg,
    "NEW WALLET\r\n\r\n"
    "Address:\r\n%s\r\n\r\n"
    "25-word recovery seed:\r\n%s\r\n\r\n"
    "IMPORTANT - write this seed down now and keep it safe. It is the only "
    "way to recover this wallet. It is not stored anywhere and CANNOT be "
    "recovered if you lose it.\r\n\r\n"
    "(Press Ctrl+C to copy this dialog.)\r\n\r\n"
    "Click OK to use this wallet, or Cancel to discard it.",
    k.address, k.mnemonic);
  if (MessageBoxA(hwnd, msg, "Glaciem Miner - New Wallet",
                  MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
    EnterCriticalSection(&g_sh.cs);
    snprintf(g_pending_seed, sizeof g_pending_seed, "%s", k.mnemonic);
    g_sh.wallet_addr[0] = 0;          /* clear stale state until reopened */
    g_sh.balance = 0;
    LeaveCriticalSection(&g_sh.cs);
    g_seed_pending = 1;               /* poll thread picks this up + opens it */
    InvalidateRect(hwnd, NULL, FALSE);
  }
}

/* ---- Restore dialog: recover an existing wallet from its 25-word seed.
   Hands the seed to the wallet poll thread via the same g_pending_seed /
   g_seed_pending path the generate flow uses (rime_wallet_recover). ---- */
#define IDC_RESTORE_EDIT 301
#define IDC_RESTORE_OK   302
#define IDC_RESTORE_CXL  303
static HWND g_restore_dlg;

static LRESULT CALLBACK RestoreProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_CREATE: {
    HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
    CreateWindowExA(0,"STATIC",
                    "Paste the 25-word recovery seed you saved when you created "
                    "the wallet. This replaces the wallet in this app.",
                    WS_CHILD|WS_VISIBLE, 16,12,400,34,h,NULL,hi,NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
                    WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
                    16,52,392,80,h,(HMENU)IDC_RESTORE_EDIT,hi,NULL);
    CreateWindowExA(0,"BUTTON","Restore",WS_CHILD|WS_VISIBLE,
                    218,144,92,30,h,(HMENU)IDC_RESTORE_OK,hi,NULL);
    CreateWindowExA(0,"BUTTON","Cancel",WS_CHILD|WS_VISIBLE,
                    316,144,92,30,h,(HMENU)IDC_RESTORE_CXL,hi,NULL);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp)==IDC_RESTORE_OK) {
      char seed[640]={0};
      GetWindowTextA(GetDlgItem(h,IDC_RESTORE_EDIT),seed,sizeof seed);
      /* normalise whitespace to single spaces and count words */
      for (char *p=seed; *p; ++p) if (*p=='\r'||*p=='\n'||*p=='\t') *p=' ';
      int words=0; for (char *p=seed; *p; ) {
        while (*p==' ') ++p;
        if (*p) { ++words; while (*p && *p!=' ') ++p; }
      }
      if (words < 24) {
        MessageBoxA(h,"Enter the full recovery seed (25 words).",
                    "Glaciem Miner - Restore",MB_OK|MB_ICONWARNING);
        return 0;
      }
      EnterCriticalSection(&g_sh.cs);
      snprintf(g_pending_seed,sizeof g_pending_seed,"%s",seed);
      g_sh.wallet_addr[0]=0;
      g_sh.balance=0;
      LeaveCriticalSection(&g_sh.cs);
      g_seed_pending=1;                 /* the wallet poll thread recovers it */
      DestroyWindow(h);
    } else if (LOWORD(wp)==IDC_RESTORE_CXL) {
      DestroyWindow(h);
    }
    return 0;
  case WM_CLOSE: DestroyWindow(h); return 0;
  case WM_DESTROY:
    g_restore_dlg=NULL;
    EnableWindow(g_hwnd,TRUE);
    SetActiveWindow(g_hwnd);
    return 0;
  }
  return DefWindowProcA(h,m,wp,lp);
}

/* RESTORE: open the recover-from-seed dialog */
static void do_restore(HWND parent) {
  if (g_restore_dlg) { SetForegroundWindow(g_restore_dlg); return; }
  HINSTANCE hi=(HINSTANCE)GetWindowLongPtrA(parent,GWLP_HINSTANCE);
  RECT pr; GetWindowRect(parent,&pr);
  EnableWindow(parent,FALSE);
  g_restore_dlg=CreateWindowExA(WS_EX_DLGMODALFRAME,"RimeRestoreWnd","Restore Wallet",
      WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,
      pr.left+40,pr.top+70,440,232,parent,NULL,hi,NULL);
  if (!g_restore_dlg) EnableWindow(parent,TRUE);
}

/* start or stop the miner */
static void toggle_mining(HWND hwnd) {
  if (!g_sh.running) {
    if (!has_mining_address()) return;   /* refuse: no address set */
    g_sh.running=1;
    g_worker=(HANDLE)_beginthreadex(NULL,0,mine_thread,NULL,0,NULL);
  } else {
    g_sh.running=0;
    if (g_worker) {
      WaitForSingleObject(g_worker,12000);
      CloseHandle(g_worker); g_worker=NULL;
    }
    g_sh.hashrate=0;   /* idle -> show 0, not the last reading */
  }
  InvalidateRect(hwnd,NULL,FALSE);
}

/* RECEIVE: copy the embedded wallet's address to the clipboard and show it */
static void do_receive(HWND hwnd) {
  char addr[200];
  EnterCriticalSection(&g_sh.cs);
  snprintf(addr,sizeof addr,"%s",g_sh.wallet_addr);
  LeaveCriticalSection(&g_sh.cs);
  if (!addr[0]) {
    MessageBoxA(hwnd,"Generate a wallet first.","Glaciem Miner - Receive",
                MB_OK|MB_ICONINFORMATION);
    return;
  }
  if (OpenClipboard(hwnd)) {
    EmptyClipboard();
    size_t n=strlen(addr)+1;
    HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,n);
    if (hg) {
      void *p=GlobalLock(hg);
      if (p) { memcpy(p,addr,n); GlobalUnlock(hg); SetClipboardData(CF_TEXT,hg); }
    }
    CloseClipboard();
  }
  char msg[320];
  snprintf(msg,sizeof msg,
    "Your Rime address (copied to the clipboard):\r\n\r\n%s",addr);
  MessageBoxA(hwnd,msg,"Glaciem Miner - Receive",MB_OK|MB_ICONINFORMATION);
}

/* ---- Send dialog: a small popup; the main message loop dispatches to it,
   and the parent window is disabled while it is open (modal-style). ---- */
#define IDC_SEND_ADDR  101
#define IDC_SEND_AMT   102
#define IDC_SEND_OK    103
#define IDC_SEND_CXL   104
#define IDC_SEND_SWEEP 105
static HWND g_send_dlg;

static LRESULT CALLBACK SendProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_CREATE: {
    HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
    CreateWindowExA(0,"STATIC","Recipient address:",WS_CHILD|WS_VISIBLE,
                    16,12,380,18,h,NULL,hi,NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
                    WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                    16,32,392,24,h,(HMENU)IDC_SEND_ADDR,hi,NULL);
    CreateWindowExA(0,"STATIC","Amount (GLAC):",WS_CHILD|WS_VISIBLE,
                    16,66,380,18,h,NULL,hi,NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
                    WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                    16,86,150,24,h,(HMENU)IDC_SEND_AMT,hi,NULL);
    CreateWindowExA(0,"BUTTON","Send",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                    218,124,92,30,h,(HMENU)IDC_SEND_OK,hi,NULL);
    CreateWindowExA(0,"BUTTON","Cancel",WS_CHILD|WS_VISIBLE,
                    316,124,92,30,h,(HMENU)IDC_SEND_CXL,hi,NULL);
    CreateWindowExA(0,"STATIC",
                    "Mined coins won't send (\"not enough outputs\")? "
                    "Sweep them into spendable form:",
                    WS_CHILD|WS_VISIBLE, 16,172,392,32,h,NULL,hi,NULL);
    CreateWindowExA(0,"BUTTON","Sweep Unmixable",WS_CHILD|WS_VISIBLE,
                    16,206,200,30,h,(HMENU)IDC_SEND_SWEEP,hi,NULL);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp)==IDC_SEND_OK) {
      char addr[200]={0}, amt[64]={0};
      GetWindowTextA(GetDlgItem(h,IDC_SEND_ADDR),addr,sizeof addr);
      GetWindowTextA(GetDlgItem(h,IDC_SEND_AMT),amt,sizeof amt);
      double a=atof(amt);
      if (!addr[0] || a<=0.0) {
        MessageBoxA(h,"Enter a recipient address and an amount above 0.",
                    "Glaciem Miner - Send",MB_OK|MB_ICONWARNING);
        return 0;
      }
      EnterCriticalSection(&g_sh.cs);
      snprintf(g_send_addr,sizeof g_send_addr,"%s",addr);
      g_send_amount=a;
      LeaveCriticalSection(&g_sh.cs);
      g_send_pending=1;                  /* the wallet poll thread sends it */
      DestroyWindow(h);
    } else if (LOWORD(wp)==IDC_SEND_SWEEP) {
      g_sweep_pending=1;                 /* the wallet poll thread sweeps it */
      DestroyWindow(h);
    } else if (LOWORD(wp)==IDC_SEND_CXL) {
      DestroyWindow(h);
    }
    return 0;
  case WM_CLOSE: DestroyWindow(h); return 0;
  case WM_DESTROY:
    g_send_dlg=NULL;
    EnableWindow(g_hwnd,TRUE);
    SetActiveWindow(g_hwnd);
    return 0;
  }
  return DefWindowProcA(h,m,wp,lp);
}

/* SEND: open the send dialog (disables the main window while it is up) */
static void do_send(HWND parent) {
  if (g_send_dlg) { SetForegroundWindow(g_send_dlg); return; }
  if (!has_mining_address()) {
    MessageBoxA(parent,"Generate a wallet first.","Glaciem Miner - Send",
                MB_OK|MB_ICONINFORMATION);
    return;
  }
  HINSTANCE hi=(HINSTANCE)GetWindowLongPtrA(parent,GWLP_HINSTANCE);
  RECT pr; GetWindowRect(parent,&pr);
  EnableWindow(parent,FALSE);
  g_send_dlg=CreateWindowExA(WS_EX_DLGMODALFRAME,"RimeSendWnd","Send GLAC",
      WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,
      pr.left+40,pr.top+70,440,290,parent,NULL,hi,NULL);
  if (!g_send_dlg) EnableWindow(parent,TRUE);
}

/* HISTORY: ask the wallet poll thread to fetch the tx history (wallet_api is
   single-threaded) -- it pops up a message box once the list is ready. */
static void do_history(HWND hwnd) {
  (void)hwnd;
  g_history_pending = 1;
}

/* ---- Host dialog: edit the node address; persisted to rime_host.txt ---- */
#define IDC_HOST_EDIT 201
#define IDC_HOST_OK   202
#define IDC_HOST_CXL  203
static HWND g_host_dlg;

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_CREATE: {
    HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
    CreateWindowExA(0,"STATIC",
                    "Node the wallet syncs from. The miner ignores this -- "
                    "it uses an automatic multi-node fallback. Set 127.0.0.1 "
                    "to point the wallet at a local rimed.",
                    WS_CHILD|WS_VISIBLE, 16,12,400,48,h,NULL,hi,NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",g_host,
                    WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                    16,64,392,24,h,(HMENU)IDC_HOST_EDIT,hi,NULL);
    CreateWindowExA(0,"STATIC",
                    "Port is fixed at 19081. Saved to rime_host.txt -- "
                    "takes effect when you restart Glaciem Miner.",
                    WS_CHILD|WS_VISIBLE, 16,92,392,34,h,NULL,hi,NULL);
    CreateWindowExA(0,"BUTTON","Save",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                    218,140,92,30,h,(HMENU)IDC_HOST_OK,hi,NULL);
    CreateWindowExA(0,"BUTTON","Cancel",WS_CHILD|WS_VISIBLE,
                    316,140,92,30,h,(HMENU)IDC_HOST_CXL,hi,NULL);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp)==IDC_HOST_OK) {
      char raw[96]={0}, host[64]={0};
      GetWindowTextA(GetDlgItem(h,IDC_HOST_EDIT),raw,sizeof raw);
      int i=0; while (raw[i]==' '||raw[i]=='\t') i++;
      int j=0; while (raw[i] && raw[i]!=' ' && raw[i]!='\t' && raw[i]!=':'
                      && raw[i]!='\r' && raw[i]!='\n' && j<63) host[j++]=raw[i++];
      host[j]=0;
      if (!host[0]) {
        MessageBoxA(h,"Enter a node host (e.g. 5.161.207.149).",
                    "Glaciem Miner - Host",MB_OK|MB_ICONWARNING);
        return 0;
      }
      FILE *f=fopen("rime_host.txt","w");
      if (!f) {
        MessageBoxA(h,"Could not write rime_host.txt -- is the folder writable?",
                    "Glaciem Miner - Host",MB_OK|MB_ICONWARNING);
        return 0;
      }
      fprintf(f,"%s\n",host);
      fclose(f);
      MessageBoxA(h,"Saved. Restart Glaciem Miner for the new node to take effect.",
                  "Glaciem Miner - Host",MB_OK|MB_ICONINFORMATION);
      DestroyWindow(h);
    } else if (LOWORD(wp)==IDC_HOST_CXL) {
      DestroyWindow(h);
    }
    return 0;
  case WM_CLOSE: DestroyWindow(h); return 0;
  case WM_DESTROY:
    g_host_dlg=NULL;
    EnableWindow(g_hwnd,TRUE);
    SetActiveWindow(g_hwnd);
    return 0;
  }
  return DefWindowProcA(h,m,wp,lp);
}

/* HOST: open the node-address dialog (disables the main window while up) */
static void do_host(HWND parent) {
  if (g_host_dlg) { SetForegroundWindow(g_host_dlg); return; }
  HINSTANCE hi=(HINSTANCE)GetWindowLongPtrA(parent,GWLP_HINSTANCE);
  RECT pr; GetWindowRect(parent,&pr);
  EnableWindow(parent,FALSE);
  g_host_dlg=CreateWindowExA(WS_EX_DLGMODALFRAME,"RimeHostWnd","Node Host",
      WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,
      pr.left+40,pr.top+70,440,250,parent,NULL,hi,NULL);
  if (!g_host_dlg) EnableWindow(parent,TRUE);
}

/* ---- v1.1.7 Pool settings dialog ----
 *
 * One checkbox (enabled) + one URL field. Persists to rime_pool.txt
 * next to the .exe: empty file = disabled, single URL line = enabled.
 * Takes effect at next batch (~1s) -- no restart required. */
#define IDC_POOL_ENABLE 401
#define IDC_POOL_URL    402
#define IDC_POOL_OK     403
#define IDC_POOL_CXL    404
static HWND g_pool_dlg;

static LRESULT CALLBACK PoolProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_CREATE: {
    HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
    CreateWindowExA(0,"STATIC",
                    "POOL MODE -- when ON, shares get submitted to the pool "
                    "URL below; payouts arrive once your contribution crosses "
                    "the pool's threshold. When OFF, this miner submits full "
                    "blocks directly (you keep 100% of any block you find, "
                    "but finds are rare with a CPU).",
                    WS_CHILD|WS_VISIBLE, 16,12,408,64,h,NULL,hi,NULL);
    CreateWindowExA(0,"BUTTON","Enable pool mode",
                    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                    16,80,260,24,h,(HMENU)IDC_POOL_ENABLE,hi,NULL);
    SendMessageA(GetDlgItem(h,IDC_POOL_ENABLE),BM_SETCHECK,
                 g_pool_enabled?BST_CHECKED:BST_UNCHECKED,0);
    CreateWindowExA(0,"STATIC","Pool URL:",
                    WS_CHILD|WS_VISIBLE, 16,114,80,18,h,NULL,hi,NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",g_pool_url,
                    WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                    16,134,408,24,h,(HMENU)IDC_POOL_URL,hi,NULL);
    CreateWindowExA(0,"STATIC",
                    "Default: https://glaciem-pool.frostmine.workers.dev "
                    "-- works with any compatible pool. Saved to "
                    "rime_pool.txt; takes effect at the next mining iteration.",
                    WS_CHILD|WS_VISIBLE, 16,162,408,40,h,NULL,hi,NULL);
    CreateWindowExA(0,"BUTTON","Save",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                    232,210,92,30,h,(HMENU)IDC_POOL_OK,hi,NULL);
    CreateWindowExA(0,"BUTTON","Cancel",WS_CHILD|WS_VISIBLE,
                    332,210,92,30,h,(HMENU)IDC_POOL_CXL,hi,NULL);
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(wp)==IDC_POOL_OK) {
      LRESULT chk = SendMessageA(GetDlgItem(h,IDC_POOL_ENABLE),BM_GETCHECK,0,0);
      char url[256] = {0};
      GetWindowTextA(GetDlgItem(h,IDC_POOL_URL),url,sizeof url);
      /* trim */
      int n = (int)strlen(url);
      while (n>0 && (url[n-1]==' '||url[n-1]=='\t'||url[n-1]=='\r'||url[n-1]=='\n'))
        url[--n]=0;
      int new_enabled = (chk==BST_CHECKED) ? 1 : 0;
      if (new_enabled && !url[0]) {
        MessageBoxA(h,"Pool mode is enabled but the URL is empty. Either "
                      "untick the box or paste a pool URL.",
                    "Glaciem Miner - Pool",MB_OK|MB_ICONWARNING);
        return 0;
      }
      FILE *f = fopen("rime_pool.txt","w");
      if (!f) {
        MessageBoxA(h,"Could not write rime_pool.txt -- is the folder writable?",
                    "Glaciem Miner - Pool",MB_OK|MB_ICONWARNING);
        return 0;
      }
      if (new_enabled) fprintf(f,"%s\n",url);
      fclose(f);
      g_pool_enabled = new_enabled;
      if (url[0]) snprintf(g_pool_url,sizeof g_pool_url,"%s",url);
      parse_pool_url();
      InvalidateRect(g_hwnd,NULL,FALSE);   /* repaint header button label */
      DestroyWindow(h);
    } else if (LOWORD(wp)==IDC_POOL_CXL) {
      DestroyWindow(h);
    }
    return 0;
  case WM_CLOSE: DestroyWindow(h); return 0;
  case WM_DESTROY:
    g_pool_dlg=NULL;
    EnableWindow(g_hwnd,TRUE);
    SetActiveWindow(g_hwnd);
    return 0;
  }
  return DefWindowProcA(h,m,wp,lp);
}

static void do_pool(HWND parent) {
  if (g_pool_dlg) { SetForegroundWindow(g_pool_dlg); return; }
  HINSTANCE hi=(HINSTANCE)GetWindowLongPtrA(parent,GWLP_HINSTANCE);
  RECT pr; GetWindowRect(parent,&pr);
  EnableWindow(parent,FALSE);
  g_pool_dlg=CreateWindowExA(WS_EX_DLGMODALFRAME,"RimePoolWnd","Pool Mode",
      WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,
      pr.left+40,pr.top+70,460,310,parent,NULL,hi,NULL);
  if (!g_pool_dlg) EnableWindow(parent,TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    g_hwnd = hwnd;
    g_fTitle=CreateFontA(34,0,0,0,FW_HEAVY,0,0,0,DEFAULT_CHARSET,0,0,
                         CLEARTYPE_QUALITY,0,"Segoe UI");
    g_fBig  =CreateFontA(78,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,
                         CLEARTYPE_QUALITY,0,"Segoe UI Semibold");
    g_fMid  =CreateFontA(22,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,
                         CLEARTYPE_QUALITY,0,"Segoe UI");
    g_fSmall=CreateFontA(17,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,
                         CLEARTYPE_QUALITY,0,"Segoe UI");
    SetTimer(hwnd,1,200,NULL);
    /* wallet poll runs for the app's lifetime, independent of mining */
    {
      HANDLE wt=(HANDLE)_beginthreadex(NULL,0,wallet_thread,NULL,0,NULL);
      if (wt) CloseHandle(wt);
    }
    return 0;
  }
  case WM_ERASEBKGND: return 1;
  case WM_PAINT: paint(hwnd); return 0;
  case WM_SIZE:
    InvalidateRect(hwnd,NULL,FALSE);
    return 0;
  case WM_GETMINMAXINFO: {
    MINMAXINFO *mmi=(MINMAXINFO*)lp;
    mmi->ptMinTrackSize.x=260;
    mmi->ptMinTrackSize.y=210;
    return 0;
  }
  case WM_TIMER:
    InvalidateRect(hwnd,NULL,FALSE);
    return 0;

  case WM_LBUTTONDOWN: {
    /* map the click from window pixels back into design-canvas coords */
    RECT cr; GetClientRect(hwnd,&cr);
    double scale; int ox,oy;
    fit_calc(cr.right,cr.bottom,&scale,&ox,&oy);
    POINT p;
    p.x=(LONG)(((double)GET_X_LPARAM(lp)-ox)/scale);
    p.y=(LONG)(((double)GET_Y_LPARAM(lp)-oy)/scale);
    if (PtInRect(&R_GEN,p))       do_generate_address(hwnd);
    else if (PtInRect(&R_RESTORE,p)) do_restore(hwnd);
    else if (PtInRect(&R_MINE,p)) toggle_mining(hwnd);
    else if (PtInRect(&R_RECV,p)) do_receive(hwnd);
    else if (PtInRect(&R_SEND,p)) do_send(hwnd);
    else if (PtInRect(&R_HIST,p)) do_history(hwnd);
    else if (PtInRect(&R_HOST,p)) do_host(hwnd);
    else if (PtInRect(&R_POOL,p)) do_pool(hwnd);
    return 0;
  }

  case WM_CLOSE:
    g_sh.running=0;
    if (g_worker) { WaitForSingleObject(g_worker,12000); CloseHandle(g_worker); g_worker=NULL; }
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    KillTimer(hwnd,1);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
  (void)hPrev;
  SetProcessDPIAware();          /* crisp on high-DPI tablet displays */
  load_host(cmd);
  load_pool_config();   /* v1.1.7: read rime_pool.txt for pool mode + URL */
  InitializeCriticalSection(&g_sh.cs);

  WNDCLASSA wc={0};
  wc.lpfnWndProc=WndProc;
  wc.hInstance=hInst;
  wc.hIcon=LoadIconA(hInst,MAKEINTRESOURCEA(1));
  wc.hCursor=LoadCursor(NULL,IDC_ARROW);
  wc.hbrBackground=NULL;
  wc.lpszClassName="RimeMinerWnd";
  RegisterClassA(&wc);

  /* the Send dialog's window class (default grey, standard controls) */
  WNDCLASSA sc={0};
  sc.lpfnWndProc=SendProc;
  sc.hInstance=hInst;
  sc.hCursor=LoadCursor(NULL,IDC_ARROW);
  sc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
  sc.lpszClassName="RimeSendWnd";
  RegisterClassA(&sc);

  /* the Host dialog's window class */
  WNDCLASSA hc={0};
  hc.lpfnWndProc=HostProc;
  hc.hInstance=hInst;
  hc.hCursor=LoadCursor(NULL,IDC_ARROW);
  hc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
  hc.lpszClassName="RimeHostWnd";
  RegisterClassA(&hc);

  /* the Restore-wallet dialog's window class */
  WNDCLASSA rc={0};
  rc.lpfnWndProc=RestoreProc;
  rc.hInstance=hInst;
  rc.hCursor=LoadCursor(NULL,IDC_ARROW);
  rc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
  rc.lpszClassName="RimeRestoreWnd";
  RegisterClassA(&rc);

  /* v1.1.7: the Pool-config dialog's window class */
  WNDCLASSA pc={0};
  pc.lpfnWndProc=PoolProc;
  pc.hInstance=hInst;
  pc.hCursor=LoadCursor(NULL,IDC_ARROW);
  pc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
  pc.lpszClassName="RimePoolWnd";
  RegisterClassA(&pc);

  /* open large: the client area fills ~90% of the screen work area
     (aspect-preserved), so the UI is comfortably sized on tablets and
     high-DPI displays. The window is resizable; the UI scales to fit. */
  RECT wa; SystemParametersInfoA(SPI_GETWORKAREA,0,&wa,0);
  int waW=wa.right-wa.left, waH=wa.bottom-wa.top;
  int ch=(int)(waH*0.90);
  int cw=ch*DW/DH;
  if (cw > (int)(waW*0.95)) { cw=(int)(waW*0.95); ch=cw*DH/DW; }
  if (cw<200) cw=200;
  if (ch<300) ch=300;

  RECT r={0,0,cw,ch};
  AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
  int winW=r.right-r.left, winH=r.bottom-r.top;
  int px=wa.left+(waW-winW)/2, py=wa.top+(waH-winH)/2;
  if (px<wa.left) px=wa.left;
  if (py<wa.top)  py=wa.top;
  HWND hwnd=CreateWindowExA(0,"RimeMinerWnd","Glaciem Miner",
      WS_OVERLAPPEDWINDOW,
      px,py,winW,winH,
      NULL,NULL,hInst,NULL);
  if (!hwnd) return 1;
  ShowWindow(hwnd,show);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessage(&msg,NULL,0,0)>0) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return 0;
}
