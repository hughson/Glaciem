/**
 * Glaciem pool -- Cloudflare Worker.
 *
 * Two responsibilities:
 *   1. Proxy /pool/* requests to the pool-server on glaciem-node
 *      (Hetzner VM, port 8088). CORS headers added for browser clients
 *      (the stats page on this same domain calls /pool/stats from JS).
 *   2. Serve /public/ static assets for anything else (the stats UI).
 *
 * Read-only GETs are edge-cached for a few seconds (the pool's data
 * doesn't change faster than block time -- ~120s -- so even 5-10s of
 * cache is cheap insurance against traffic spikes). POSTs always go
 * to origin.
 */

const ORIGIN = "http://static.34.142.105.178.clients.your-server.de:8088";

const CORS = {
  "Access-Control-Allow-Origin":  "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Max-Age":       "86400",
};

// GET endpoints worth caching at the edge (and for how long, seconds).
// /pool/job is POST and inherently per-miner; not cached.
const GET_CACHE_TTL = {
  "/pool/stats":   5,
  "/pool/blocks":  10,
  "/pool/payouts": 10,
};

export default {
  async fetch(request, env, ctx) {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS });
    }

    const url = new URL(request.url);

    // /pool/* requests proxy to the pool server. Anything else is
    // a static asset.
    if (!url.pathname.startsWith("/pool/")) {
      // Fall back to the asset binding -- defined as ASSETS in wrangler.
      // (run_worker_first selects when the worker runs vs assets serve.)
      return env.ASSETS.fetch(request);
    }

    const isGet = request.method === "GET";
    const ttl   = GET_CACHE_TTL[url.pathname];

    // ---- edge cache lookup for cacheable GETs ----
    let cacheKey = null;
    if (isGet && ttl) {
      // Include the search string so different ?wallet=X stay isolated.
      cacheKey = new Request(
        `https://pool-cache.local${url.pathname}${url.search}`,
        { method: "GET" }
      );
      const hit = await caches.default.match(cacheKey);
      if (hit) {
        // Add CORS on the way out -- the cached response was stored
        // without CORS headers (so other origins don't get a false-positive).
        const out = new Response(hit.body, hit);
        for (const [k, v] of Object.entries(CORS)) out.headers.set(k, v);
        out.headers.set("x-pool-cache", "HIT");
        return out;
      }
    }

    // ---- forward to origin ----
    const originUrl = ORIGIN + url.pathname + url.search;
    let body = undefined;
    if (request.method === "POST") {
      body = await request.arrayBuffer();
    }
    let resp;
    try {
      resp = await fetch(originUrl, {
        method:  request.method,
        headers: { "content-type": request.headers.get("content-type") || "application/json" },
        body,
      });
    } catch (e) {
      return new Response(
        JSON.stringify({ error: "pool server unreachable: " + e.message }),
        { status: 502, headers: { "content-type": "application/json", ...CORS } }
      );
    }

    const respBody = await resp.arrayBuffer();
    const respCT   = resp.headers.get("content-type") || "application/json";

    // Cache successful GETs.
    if (cacheKey && resp.status < 400) {
      const cacheResp = new Response(respBody, {
        status: 200,
        headers: {
          "content-type":  respCT,
          "cache-control": `public, max-age=${ttl}`,
        },
      });
      ctx.waitUntil(caches.default.put(cacheKey, cacheResp));
    }

    return new Response(respBody, {
      status:  resp.status,
      headers: {
        "content-type": respCT,
        "x-pool-cache": cacheKey ? "MISS" : "BYPASS",
        ...CORS,
      },
    });
  },
};
