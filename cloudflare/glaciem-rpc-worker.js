/**
 * Glaciem RPC proxy (Cloudflare Worker).
 *
 * Sits in front of the rimed nodes and forwards daemon RPC calls to them.
 * Cloudflare absorbs connection storms (at difficulty 1, miners "find" a block
 * on the first nonce and hammer the node in a tight loop) so the origin nodes
 * only ever see calm, pooled traffic.
 *
 * The apps point only at this Worker -- they never know which node is behind
 * it. To add, remove, or reorder nodes, edit ORIGINS and redeploy; the apps
 * never change.
 *
 * Failover: each request tries the origins in order. A transport error or a
 * 5xx moves on to the next; any node that actually answers (2xx/3xx/4xx,
 * including JSON-RPC error bodies) is used.
 *
 * Origins are VM port 80 (socat) -> rimed RPC on :19081, addressed by their
 * Hetzner DNS names (Workers fetch() needs a hostname, not a raw IP).
 *
 * ----------------------------------------------------------------------------
 * CACHE LAYER (2026-05-22)
 *
 * 12 miners each polling get_info every 1-2s sends ~30k+ identical calls/day
 * to the origin nodes. The chain only advances every 120s, so ~99% of those
 * polls return the same payload. We cache the cacheable read-only methods
 * with short TTLs and serve hits straight from the Worker edge.
 *
 * The cache is content-addressed: the key is method + sha256(params), with
 * the JSON-RPC `id` field stripped (since every client sends a different id
 * for the same logical query). On a cache hit we swap the requester's `id`
 * back into the cached body so they see their own id echoed.
 *
 * Methods that mutate state, or that depend on per-request context, are
 * passed through uncached. See CACHE_TTL below for the allowlist.
 */
const ORIGINS = [
  { id: "node2", url: "http://static.197.125.225.46.clients.your-server.de" },  // 46.225.125.197
  { id: "node1", url: "http://static.34.142.105.178.clients.your-server.de" },  // 178.105.142.34
];

// JSON-RPC methods we'll cache (parsed body, keyed by method+params).
// Anything not listed here passes through to origin every time.
const CACHE_TTL = {
  // Chain-tip queries -- change only when a new block lands.
  "get_info":                    8,
  "getinfo":                     8,
  "get_last_block_header":       8,
  "getlastblockheader":          8,
  "get_block_count":             8,
  "getblockcount":               8,
  "get_height":                  8,
  // By-height lookups can reorg on small chains, so keep short.
  "get_block_header_by_height":  5,
  "getblockheaderbyheight":      5,
  // By-hash and full-block lookups are content-addressed -- safe to cache
  // for the lifetime of the deployment effectively. We use 1h to stay
  // within typical Workers Cache reclamation policies.
  "get_block_header_by_hash":    3600,
  "getblockheaderbyhash":        3600,
  "get_block":                   3600,
};

// Binary endpoints we'll cache by raw request-body hash. These are wallet
// sync calls -- /getblocks.bin and friends -- that return MB-sized batches
// of blocks. They're the actual bandwidth eater on this chain (~46% of
// traffic, most of the bytes). Wallets at similar sync points will share
// cached responses. Short TTL is fine because the chain advances every
// 120s and most rapid retries are within seconds of each other.
const BIN_CACHE_TTL = {
  // Wallets typically poll getblocks.bin every ~8s; a 15s TTL means each
  // wallet's second poll within a window hits cache (and multiple wallets
  // at tip share the same response). Still well within block time (120s).
  "/getblocks.bin":          15,
  "/get_blocks.bin":         15,        // newer name on some forks
  "/getblocks_by_height.bin": 15,
  "/get_o_indexes.bin":      60,        // output index lookups -- mostly stable
  "/get_outs.bin":           60,
  "/get_transactions":       60,        // tx lookups; tx contents are immutable
  "/gettransactions":        60,
};

// Endpoints to refuse outright -- admin/debug endpoints that have no
// business being exposed via a public proxy.
const BLOCKED_PATHS = new Set([
  "/get_peer_list",
  "/get_public_nodes",
  "/get_connections",
  "/get_outputs.bin",   // huge ringsize lookups -- abuse vector
  "/save_bc",
  "/stop_save_graph",
  "/start_save_graph",
  "/update",
  "/set_log_level",
  "/set_log_categories",
  "/set_bootstrap_daemon",
  "/set_limit",
]);

// Per-node hit counter -- writes nodehit:<id>:<YYYY-MM-DD> in KV. Best-effort:
// read-modify-write isn't atomic so a few counts may be lost under load, but
// the signal (which node serves most miners?) survives just fine. We also
// bump a synthetic "cache" id on cache hits so /stats can show how many
// calls never hit origin.
async function bumpHit(env, nodeId) {
  if (!env || !env.STATS) return;
  const day = new Date().toISOString().slice(0, 10);   // YYYY-MM-DD UTC
  const key = `nodehit:${nodeId}:${day}`;
  try {
    const cur = parseInt((await env.STATS.get(key)) || "0", 10) || 0;
    // 7-day TTL keeps the namespace from filling with old daily keys.
    await env.STATS.put(key, String(cur + 1), { expirationTtl: 7 * 86400 });
  } catch (_e) {
    // never block the user's RPC for stats
  }
}

// Privacy-preserving unique-miner counter. (Unchanged from previous version.)
//
// Goal: a single aggregate number -- "N unique miners today".
// Anti-goal: ever store or expose anything that could identify a miner,
// including coarse aggregates like country distribution. For a privacy
// coin, "1 miner from country X" can still uniquely identify someone,
// especially in combination with what a person might self-disclose
// elsewhere. So: no country, no region, no per-miner detail of any kind.
//
// Strategy: hash (daily_salt || IP) -> 16 hex chars, write that as a key.
// The salt rotates daily so yesterday's fingerprints can't be correlated
// to today's. Raw IPs never touch KV. The cf-ipcountry header is read
// only to discard (we deliberately don't propagate it). Wallet addresses
// in the RPC body are never read or stored either -- they ride through
// the proxy untouched.
//
// /stats exposes only aggregate counts. Even we can't reverse a
// fingerprint back to an IP: salt regenerates daily, hash is SHA-256,
// no raw input is retained.
async function bumpMiner(env, request) {
  if (!env || !env.STATS) return;
  const ip = request.headers.get("cf-connecting-ip");
  if (!ip) return;
  const day = new Date().toISOString().slice(0, 10);
  const enc = new TextEncoder();
  const digest = await crypto.subtle.digest(
    "SHA-256",
    enc.encode(`glaciem-rpc-salt-${day}::${ip}`)
  );
  const fp = Array.from(new Uint8Array(digest).slice(0, 8))
    .map(b => b.toString(16).padStart(2, "0")).join("");
  try {
    await env.STATS.put(`miner:${day}:${fp}`, "1",
      { expirationTtl: 7 * 86400 });
  } catch (_e) {
    // never block the RPC for stats
  }
}

// ---- cache helpers -------------------------------------------------------

/** Parse a JSON-RPC request body. Returns the parsed object or null. */
function parseRpc(buf) {
  try {
    const text = new TextDecoder().decode(buf);
    const obj = JSON.parse(text);
    if (!obj || typeof obj !== "object") return null;
    // Accept Monero's restricted RPC envelope: { jsonrpc, method, params, id }.
    if (typeof obj.method !== "string") return null;
    return obj;
  } catch {
    return null;
  }
}

/** Build a stable cache key Request from method + params (id stripped). */
async function cacheKeyFor(method, params) {
  const enc = new TextEncoder();
  const payload = method + ":" + JSON.stringify(params ?? null);
  const digest = await crypto.subtle.digest("SHA-256", enc.encode(payload));
  const hex = Array.from(new Uint8Array(digest).slice(0, 16))
    .map(b => b.toString(16).padStart(2, "0")).join("");
  // Cloudflare Cache API keys by URL. Use a synthetic .local URL so cache
  // entries are isolated from any real-domain caching.
  return new Request(`https://rpc-cache.local/${method}/${hex}`, { method: "GET" });
}

/** Override the JSON-RPC `id` in a cached response body so each client
 *  sees their own id echoed back. If parsing fails, return as-is. */
function swapId(text, newId) {
  try {
    const obj = JSON.parse(text);
    obj.id = newId;
    return JSON.stringify(obj);
  } catch {
    return text;
  }
}

// ---- CORS ----------------------------------------------------------------

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Max-Age": "86400",
};

// ---- fetch handler -------------------------------------------------------

export default {
  async fetch(request, env, ctx) {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS });
    }

    const url = new URL(request.url);

    // /stats: aggregate counters. We add "cache" (hits served from edge)
    // and "origin_total" (calls that actually reached a backend node) to
    // make the cache effectiveness visible in the dashboard.
    if (url.pathname === "/stats" && request.method === "GET") {
      const day = new Date().toISOString().slice(0, 10);
      const out = { day, nodes: {}, cache: 0, miners: { unique: 0 } };
      for (const o of ORIGINS) {
        out.nodes[o.id] = parseInt(
          (await env.STATS.get(`nodehit:${o.id}:${day}`)) || "0", 10) || 0;
      }
      out.cache = parseInt(
        (await env.STATS.get(`nodehit:cache:${day}`)) || "0", 10) || 0;
      const totalOrigin = Object.values(out.nodes).reduce((a, b) => a + b, 0);
      const totalAll    = totalOrigin + out.cache;
      out.cache_hit_rate = totalAll > 0
        ? `${(out.cache * 100 / totalAll).toFixed(1)}%` : "0%";
      const minerList = await env.STATS.list(
        { prefix: `miner:${day}:`, limit: 1000 });
      out.miners.unique = minerList.keys.length;
      return new Response(JSON.stringify(out, null, 2), {
        headers: { "content-type": "application/json", ...CORS },
      });
    }

    const path = url.pathname + url.search;
    const method = request.method;
    const ct = request.headers.get("content-type") || "application/octet-stream";
    let body;
    if (method !== "GET" && method !== "HEAD") {
      body = await request.arrayBuffer();
    }

    // Refuse admin/debug endpoints. They never need to be reachable from a
    // public proxy; exposing them is a needless attack/abuse surface.
    if (BLOCKED_PATHS.has(url.pathname)) {
      return new Response(
        JSON.stringify({ error: { code: -32600, message: "endpoint not exposed" } }),
        { status: 403, headers: { "content-type": "application/json", ...CORS } }
      );
    }

    // ---- binary-endpoint cache (e.g. /getblocks.bin, the wallet-sync
    // bandwidth eater). Key the cache by raw request-body bytes so two
    // wallets at the same sync point share the same cached response. ----
    let binCacheReq = null;
    let binTtl = 0;
    if (BIN_CACHE_TTL[url.pathname] && body) {
      binTtl = BIN_CACHE_TTL[url.pathname];
      // Hash the raw body bytes -- these endpoints take binary, not JSON,
      // and we can't introspect parameters portably. Body-hash is a stable
      // key as long as two wallets send the same bytes.
      const digest = await crypto.subtle.digest("SHA-256", body);
      const hex = Array.from(new Uint8Array(digest).slice(0, 20))
        .map(b => b.toString(16).padStart(2, "0")).join("");
      binCacheReq = new Request(
        `https://rpc-cache.local/bin${url.pathname}/${hex}`,
        { method: "GET" }
      );
      const hit = await caches.default.match(binCacheReq);
      if (hit) {
        const bytes = await hit.arrayBuffer();
        if (env && ctx) {
          ctx.waitUntil(bumpHit(env, "cache"));
          ctx.waitUntil(bumpMiner(env, request));
        }
        return new Response(bytes, {
          status: 200,
          headers: {
            "content-type":
              hit.headers.get("content-type") || "application/octet-stream",
            "x-rpc-cache": "HIT",
            ...CORS,
          },
        });
      }
    }

    // ---- cache lookup (cacheable JSON-RPC methods only) ----
    // Monero's JSON-RPC endpoint is /json_rpc. The "fast" endpoints like
    // /get_height, /get_info are also cacheable but their bodies are empty,
    // so we'd cache them by URL alone -- handled separately below.
    let rpc = null;
    let cacheReq = null;
    let ttl = 0;
    const isJsonRpc = method === "POST" && url.pathname.endsWith("/json_rpc") && body;
    if (isJsonRpc) {
      rpc = parseRpc(body);
      if (rpc && CACHE_TTL[rpc.method]) {
        ttl = CACHE_TTL[rpc.method];
        cacheReq = await cacheKeyFor(rpc.method, rpc.params);
        const hit = await caches.default.match(cacheReq);
        if (hit) {
          const cachedText = await hit.text();
          const out = swapId(cachedText, rpc.id);
          if (env && ctx) {
            ctx.waitUntil(bumpHit(env, "cache"));
            ctx.waitUntil(bumpMiner(env, request));
          }
          return new Response(out, {
            status: 200,
            headers: {
              "content-type": "application/json",
              "x-rpc-cache": "HIT",
              ...CORS,
            },
          });
        }
      }
    }

    // Also cache the bare-path read endpoints (/get_info, /get_height,
    // /get_block_count). These take no body so a single URL key works.
    let pathCacheReq = null;
    let pathTtl = 0;
    if (method === "GET" || (method === "POST" && (!body || body.byteLength === 0))) {
      const tail = url.pathname.replace(/^\/+/, "");
      // Map URL-style endpoints to the same TTL table.
      const urlMethod = tail.toLowerCase();
      if (CACHE_TTL[urlMethod] !== undefined) {
        pathTtl = CACHE_TTL[urlMethod];
        pathCacheReq = new Request(`https://rpc-cache.local/path/${urlMethod}`, { method: "GET" });
        const hit = await caches.default.match(pathCacheReq);
        if (hit) {
          const cachedText = await hit.text();
          if (env && ctx) {
            ctx.waitUntil(bumpHit(env, "cache"));
            ctx.waitUntil(bumpMiner(env, request));
          }
          return new Response(cachedText, {
            status: 200,
            headers: {
              "content-type": "application/json",
              "x-rpc-cache": "HIT",
              ...CORS,
            },
          });
        }
      }
    }

    // ---- pass through to origin ----
    for (const origin of ORIGINS) {
      try {
        const resp = await fetch(origin.url + path, {
          method,
          headers: { "content-type": ct },
          body,
        });
        // 5xx -> node is failing, try the next one. Anything else (incl.
        // JSON-RPC error bodies, which come back as HTTP 200) is a real answer.
        if (resp.status >= 500) continue;
        if (env && ctx) {
          ctx.waitUntil(bumpHit(env, origin.id));
          ctx.waitUntil(bumpMiner(env, request));
        }

        // Binary cacheable responses use arrayBuffer (we can't parse them).
        // JSON cacheable responses go through .text() so we can id-swap
        // on hits. Pass-through traffic streams the body verbatim.
        const isBinCacheable = resp.status < 400 && binCacheReq;
        const isJsonCacheable = resp.status < 400 && (cacheReq || pathCacheReq);
        const respContentType =
          resp.headers.get("content-type") ||
          (isBinCacheable ? "application/octet-stream" : "application/json");

        if (isBinCacheable) {
          const bytes = await resp.arrayBuffer();
          const cacheResp = new Response(bytes, {
            status: 200,
            headers: {
              "content-type": respContentType,
              "cache-control": `public, max-age=${binTtl}`,
            },
          });
          ctx.waitUntil(caches.default.put(binCacheReq, cacheResp));
          return new Response(bytes, {
            status: resp.status,
            headers: {
              "content-type": respContentType,
              "x-rpc-cache": "MISS",
              ...CORS,
            },
          });
        }

        if (isJsonCacheable) {
          const respText = await resp.text();
          const storeReq = cacheReq || pathCacheReq;
          const storeTtl = ttl || pathTtl;
          const cacheResp = new Response(respText, {
            status: 200,
            headers: {
              "content-type": "application/json",
              "cache-control": `public, max-age=${storeTtl}`,
            },
          });
          ctx.waitUntil(caches.default.put(storeReq, cacheResp));
          return new Response(respText, {
            status: resp.status,
            headers: {
              "content-type": "application/json",
              "x-rpc-cache": "MISS",
              ...CORS,
            },
          });
        }

        // Uncacheable -- stream the response body straight through.
        return new Response(resp.body, {
          status: resp.status,
          headers: {
            "content-type": respContentType,
            "x-rpc-cache": "BYPASS",
            ...CORS,
          },
        });
      } catch (e) {
        // transport error -> try the next origin
      }
    }

    return new Response(
      JSON.stringify({ error: { code: -1, message: "all nodes unreachable" } }),
      { status: 502, headers: { "content-type": "application/json", ...CORS } }
    );
  },
};
