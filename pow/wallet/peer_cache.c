/*
 * peer_cache.c -- portable C peer cache. See peer_cache.h.
 *
 * Tiny in-memory list (capped at MAX_PEERS) with a hand-rolled JSON
 * serializer for persistence. Pure C, no external deps; safe to link from
 * Objective-C (Mac), C (Win), and C++ (Linux, Android wallet_jni).
 */
#include "peer_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PEERS 64
/* How many successful RPCs between discovery attempts. ~5 means ~once a
   minute given the apps' polling cadence. */
#define DISCOVERY_INTERVAL 5
/* Drop a discovered peer after this many consecutive failures. */
#define DROP_FAILURE_THRESHOLD 5
/* Drop a discovered peer that hasn't been reachable for this long. */
#define STALE_LAST_OK_SECONDS (7 * 24 * 3600)

struct PeerCache {
  PeerEntry             peers[MAX_PEERS];
  int                   count;
  int                   rpc_since_discovery;
  pthread_mutex_t       lock;
  peer_cache_save_fn    save;
  void                 *save_ctx;
};

/* ---- JSON helpers (hand-rolled; the data shape is tiny + fixed) ---- */

static void emit_str(char **p, char *end, const char *s) {
  if (*p < end) **p = '"', ++*p;
  for (; *s && *p < end - 1; s++) {
    if (*s == '"' || *s == '\\') { if (*p < end - 1) { *(*p)++ = '\\'; } }
    *(*p)++ = *s;
  }
  if (*p < end) **p = '"', ++*p;
}
static void emit_int(char **p, char *end, long long v) {
  char tmp[32];
  int n = snprintf(tmp, sizeof tmp, "%lld", v);
  for (int i = 0; i < n && *p < end; i++) *(*p)++ = tmp[i];
}

static int serialize(const PeerCache *pc, char *out, int cap) {
  char *p = out, *end = out + cap;
  *p++ = '{';
  const char *vkey = "\"version\":1,";
  while (*vkey && p < end) *p++ = *vkey++;
  const char *pkey = "\"peers\":[";
  while (*pkey && p < end) *p++ = *pkey++;
  for (int i = 0; i < pc->count; i++) {
    if (i && p < end) *p++ = ',';
    if (p < end) *p++ = '{';
    const char *k;
    k = "\"host\":";            while (*k && p < end) *p++ = *k++; emit_str(&p, end, pc->peers[i].host);
    k = ",\"port\":";           while (*k && p < end) *p++ = *k++; emit_int(&p, end, pc->peers[i].port);
    k = ",\"use_ssl\":";        while (*k && p < end) *p++ = *k++; emit_int(&p, end, pc->peers[i].use_ssl);
    k = ",\"is_seed\":";        while (*k && p < end) *p++ = *k++; emit_int(&p, end, pc->peers[i].is_seed);
    k = ",\"last_ok\":";        while (*k && p < end) *p++ = *k++; emit_int(&p, end, pc->peers[i].last_ok);
    k = ",\"consec_failures\":"; while (*k && p < end) *p++ = *k++; emit_int(&p, end, pc->peers[i].consec_failures);
    k = ",\"avg_latency_ms\":";  while (*k && p < end) *p++ = *k++; emit_int(&p, end, pc->peers[i].avg_latency_ms);
    if (p < end) *p++ = '}';
  }
  if (p < end) *p++ = ']';
  if (p < end) *p++ = '}';
  if (p < end) *p = 0;
  return (int)(p - out);
}

/* tolerant scanner: extracts {"host":"...","port":N,"use_ssl":0,"last_ok":N,
   "consec_failures":N,"avg_latency_ms":N} entries. Ignores unknown fields. */
static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}
static const char *find_field(const char *obj_start, const char *obj_end,
                              const char *key, char *out, int outcap,
                              long long *outint) {
  /* searches within [obj_start, obj_end) for "key":VALUE; returns pointer
     past the value, or NULL if not found. out for strings, outint for ints. */
  char needle[64];
  snprintf(needle, sizeof needle, "\"%s\"", key);
  const char *p = strstr(obj_start, needle);
  if (!p || p >= obj_end) return NULL;
  p += strlen(needle);
  p = skip_ws(p);
  if (*p != ':') return NULL;
  p = skip_ws(p + 1);
  if (*p == '"' && out) {
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outcap - 1) out[i++] = *p++;
    out[i] = 0;
    if (*p == '"') p++;
    return p;
  }
  if ((*p == '-' || (*p >= '0' && *p <= '9')) && outint) {
    long long sign = 1;
    if (*p == '-') { sign = -1; p++; }
    long long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *outint = v * sign;
    return p;
  }
  return NULL;
}

static void deserialize(PeerCache *pc, const char *json) {
  if (!json || !*json) return;
  /* walk peer objects in the "peers":[...] array */
  const char *arr = strstr(json, "\"peers\"");
  if (!arr) return;
  arr = strchr(arr, '[');
  if (!arr) return;
  const char *end = strchr(arr, ']');
  if (!end) return;
  const char *p = arr + 1;
  while (p < end && pc->count < MAX_PEERS) {
    p = skip_ws(p);
    if (*p != '{') break;
    const char *obj_end = strchr(p, '}');
    if (!obj_end || obj_end > end) break;
    PeerEntry e; memset(&e, 0, sizeof e);
    long long v;
    find_field(p, obj_end, "host", e.host, (int)sizeof e.host, NULL);
    if (find_field(p, obj_end, "port", NULL, 0, &v)) e.port = (int)v;
    if (find_field(p, obj_end, "use_ssl", NULL, 0, &v)) e.use_ssl = (int)v;
    if (find_field(p, obj_end, "is_seed", NULL, 0, &v)) e.is_seed = (int)v;
    if (find_field(p, obj_end, "last_ok", NULL, 0, &v)) e.last_ok = v;
    if (find_field(p, obj_end, "consec_failures", NULL, 0, &v)) e.consec_failures = (int)v;
    if (find_field(p, obj_end, "avg_latency_ms", NULL, 0, &v)) e.avg_latency_ms = (int)v;
    /* discard seeds from the saved file -- seeds are re-added by the caller
       so we don't accidentally pin a stale seed across an app update. */
    if (e.host[0] && e.port > 0 && !e.is_seed) {
      pc->peers[pc->count++] = e;
    }
    p = obj_end + 1;
    p = skip_ws(p);
    if (*p == ',') p++;
  }
}

/* ---- public API ---- */

PeerCache *peer_cache_new(peer_cache_load_fn load, peer_cache_save_fn save, void *ctx) {
  PeerCache *pc = (PeerCache *)calloc(1, sizeof *pc);
  if (!pc) return NULL;
  pthread_mutex_init(&pc->lock, NULL);
  pc->save = save;
  pc->save_ctx = ctx;
  if (load) {
    char buf[16384];
    if (load(buf, (int)sizeof buf, ctx)) deserialize(pc, buf);
  }
  return pc;
}

void peer_cache_free(PeerCache *pc) {
  if (!pc) return;
  pthread_mutex_destroy(&pc->lock);
  free(pc);
}

static int find_idx(PeerCache *pc, const char *host, int port) {
  for (int i = 0; i < pc->count; i++) {
    if (pc->peers[i].port == port && strcmp(pc->peers[i].host, host) == 0) {
      return i;
    }
  }
  return -1;
}

static void persist_unlocked(PeerCache *pc) {
  if (!pc->save) return;
  char buf[16384];
  serialize(pc, buf, (int)sizeof buf);
  pc->save(buf, pc->save_ctx);
}

void peer_cache_add_seed(PeerCache *pc, const char *host, int port, int use_ssl) {
  if (!pc || !host || !host[0] || port <= 0) return;
  pthread_mutex_lock(&pc->lock);
  int idx = find_idx(pc, host, port);
  if (idx < 0 && pc->count < MAX_PEERS) {
    PeerEntry e; memset(&e, 0, sizeof e);
    strncpy(e.host, host, sizeof e.host - 1);
    e.port = port;
    e.use_ssl = use_ssl;
    e.is_seed = 1;
    pc->peers[pc->count++] = e;
  } else if (idx >= 0) {
    pc->peers[idx].is_seed = 1;
    pc->peers[idx].use_ssl = use_ssl;
  }
  pthread_mutex_unlock(&pc->lock);
}

void peer_cache_add_discovered(PeerCache *pc, const char *host, int port) {
  if (!pc || !host || !host[0] || port <= 0) return;
  pthread_mutex_lock(&pc->lock);
  if (find_idx(pc, host, port) < 0 && pc->count < MAX_PEERS) {
    PeerEntry e; memset(&e, 0, sizeof e);
    strncpy(e.host, host, sizeof e.host - 1);
    e.port = port;
    e.use_ssl = 0;
    e.is_seed = 0;
    pc->peers[pc->count++] = e;
    persist_unlocked(pc);
  }
  pthread_mutex_unlock(&pc->lock);
}

void peer_cache_mark_success(PeerCache *pc, const char *host, int port, int latency_ms) {
  if (!pc) return;
  pthread_mutex_lock(&pc->lock);
  int idx = find_idx(pc, host, port);
  if (idx >= 0) {
    pc->peers[idx].consec_failures = 0;
    pc->peers[idx].last_ok = (long long)time(NULL);
    /* EMA: 0.7*old + 0.3*new (initial sample replaces zero) */
    if (pc->peers[idx].avg_latency_ms == 0) pc->peers[idx].avg_latency_ms = latency_ms;
    else pc->peers[idx].avg_latency_ms = (pc->peers[idx].avg_latency_ms * 7 + latency_ms * 3) / 10;
    pc->rpc_since_discovery++;
    persist_unlocked(pc);
  }
  pthread_mutex_unlock(&pc->lock);
}

void peer_cache_mark_failure(PeerCache *pc, const char *host, int port) {
  if (!pc) return;
  pthread_mutex_lock(&pc->lock);
  int idx = find_idx(pc, host, port);
  if (idx >= 0) {
    pc->peers[idx].consec_failures++;
    /* drop non-seed peers that fail too often OR have gone stale */
    int now = (int)time(NULL);
    int stale = !pc->peers[idx].is_seed
                && pc->peers[idx].last_ok > 0
                && (now - pc->peers[idx].last_ok) > STALE_LAST_OK_SECONDS;
    int dead  = !pc->peers[idx].is_seed
                && pc->peers[idx].consec_failures >= DROP_FAILURE_THRESHOLD;
    if (stale || dead) {
      /* remove by shifting */
      for (int i = idx; i + 1 < pc->count; i++) pc->peers[i] = pc->peers[i + 1];
      pc->count--;
    }
    persist_unlocked(pc);
  }
  pthread_mutex_unlock(&pc->lock);
}

static int score_cmp(const void *a, const void *b) {
  const PeerEntry *pa = (const PeerEntry *)a;
  const PeerEntry *pb = (const PeerEntry *)b;
  /* seeds first, in insertion order */
  if (pa->is_seed != pb->is_seed) return pb->is_seed - pa->is_seed;
  if (pa->is_seed && pb->is_seed) return 0;  /* preserve insertion order */
  /* among discovered: fewer failures first, then most recent last_ok, then
     lower latency */
  if (pa->consec_failures != pb->consec_failures)
    return pa->consec_failures - pb->consec_failures;
  if (pa->last_ok != pb->last_ok)
    return (pa->last_ok < pb->last_ok) ? 1 : -1;
  return pa->avg_latency_ms - pb->avg_latency_ms;
}

int peer_cache_snapshot(PeerCache *pc, PeerEntry *out, int cap) {
  if (!pc || !out || cap <= 0) return 0;
  pthread_mutex_lock(&pc->lock);
  int n = pc->count < cap ? pc->count : cap;
  /* separate seeds (preserving order) and discovered (sortable) */
  int seed_n = 0, disc_n = 0;
  PeerEntry seeds[MAX_PEERS], disc[MAX_PEERS];
  for (int i = 0; i < pc->count; i++) {
    if (pc->peers[i].is_seed) seeds[seed_n++] = pc->peers[i];
    else                       disc[disc_n++]  = pc->peers[i];
  }
  qsort(disc, disc_n, sizeof *disc, score_cmp);
  int k = 0;
  for (int i = 0; i < seed_n && k < cap; i++) out[k++] = seeds[i];
  for (int i = 0; i < disc_n && k < cap; i++) out[k++] = disc[i];
  pthread_mutex_unlock(&pc->lock);
  (void)n;
  return k;
}

int peer_cache_should_discover(PeerCache *pc) {
  if (!pc) return 0;
  pthread_mutex_lock(&pc->lock);
  int ok = pc->rpc_since_discovery >= DISCOVERY_INTERVAL;
  pthread_mutex_unlock(&pc->lock);
  return ok;
}

void peer_cache_reset_discovery_counter(PeerCache *pc) {
  if (!pc) return;
  pthread_mutex_lock(&pc->lock);
  pc->rpc_since_discovery = 0;
  pthread_mutex_unlock(&pc->lock);
}
