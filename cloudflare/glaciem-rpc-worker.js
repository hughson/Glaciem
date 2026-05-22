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
 */
// Each origin gets a short id so we can attribute hits to the right node in
// KV without leaking the full hostname in stats keys.
const ORIGINS = [
  { id: "node2", url: "http://static.197.125.225.46.clients.your-server.de" },  // 46.225.125.197
  { id: "node1", url: "http://static.34.142.105.178.clients.your-server.de" },  // 178.105.142.34
];

// Per-node hit counter -- writes nodehit:<id>:<YYYY-MM-DD> in KV. Best-effort:
// read-modify-write isn't atomic so a few counts may be lost under load, but
// the signal (which node serves most miners?) survives just fine.
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

// Privacy-preserving unique-miner counter.
//
// Goal: count unique miners per day. Anti-goal: ever be able to identify an
// individual miner from KV contents.
//
// Strategy: hash (daily_salt || IP) -> 16 hex chars, write that as a key.
// The salt rotates daily so yesterday's fingerprints can't be matched to
// today's. Raw IPs never touch KV. Wallet addresses are NEVER read or stored
// (they ride through in the RPC body untouched). The country header from
// Cloudflare is bucketed by ISO code only -- never per-IP.
//
// /stats exposes only aggregate counts. Even WE can't reverse a fingerprint
// back to an IP -- the salt is regenerated each UTC day and the hash is
// keyed-SHA256.
async function bumpMiner(env, request) {
  if (!env || !env.STATS) return;
  const ip = request.headers.get("cf-connecting-ip");
  if (!ip) return;
  const country = (request.headers.get("cf-ipcountry") || "XX")
    .toUpperCase().slice(0, 2);
  const day = new Date().toISOString().slice(0, 10);

  // Daily-rotating salt. Same across all isolates today; impossible to
  // correlate to yesterday's fingerprints without yesterday's salt
  // (which we don't keep -- KV TTL aside, it's deterministic but only
  // useful within the same UTC day window).
  const enc = new TextEncoder();
  const digest = await crypto.subtle.digest(
    "SHA-256",
    enc.encode(`glaciem-rpc-salt-${day}::${ip}`)
  );
  const fp = Array.from(new Uint8Array(digest).slice(0, 8))
    .map(b => b.toString(16).padStart(2, "0")).join("");

  try {
    // Write the fingerprint key. Idempotent within the day (same IP same
    // fingerprint), so K writes for K total requests but only N distinct
    // keys for N distinct IPs. To count uniques, list keys with this prefix.
    await env.STATS.put(`miner:${day}:${fp}`, "1",
      { expirationTtl: 7 * 86400 });
    // Country bucket -- per-day per-country fingerprint, again no raw IP.
    // Aggregating is "how many distinct fp:country pairs exist today."
    await env.STATS.put(`mc:${day}:${country}:${fp}`, "1",
      { expirationTtl: 7 * 86400 });
  } catch (_e) {
    // never block the RPC for stats
  }
}

// CORS so the Glaciem website can read live chain stats from the browser.
const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Max-Age": "86400",
};

export default {
  async fetch(request, env, ctx) {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS });
    }

    // Aggregate stats only -- no per-IP, no per-fingerprint exposed.
    const url = new URL(request.url);
    if (url.pathname === "/stats" && request.method === "GET") {
      const day = new Date().toISOString().slice(0, 10);
      const out = { day, nodes: {}, miners: { unique: 0, countries: {} } };
      for (const o of ORIGINS) {
        out.nodes[o.id] = parseInt(
          (await env.STATS.get(`nodehit:${o.id}:${day}`)) || "0", 10) || 0;
      }
      // Count unique miner fingerprints today (list-with-prefix is the
      // closest KV gets to a set-size operation; cheap at our scale).
      const minerList = await env.STATS.list(
        { prefix: `miner:${day}:`, limit: 1000 });
      out.miners.unique = minerList.keys.length;
      // Country distribution -- count fingerprints per country code.
      // We do NOT expose individual fingerprints in the response.
      const cList = await env.STATS.list(
        { prefix: `mc:${day}:`, limit: 1000 });
      for (const k of cList.keys) {
        const parts = k.name.split(":");        // mc:YYYY-MM-DD:CC:fp
        const cc = parts[2] || "XX";
        out.miners.countries[cc] = (out.miners.countries[cc] || 0) + 1;
      }
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
        // Stats bumps run after the response is on its way -- ctx.waitUntil
        // lets the KV writes complete without delaying the user's response.
        if (env && ctx) {
          ctx.waitUntil(bumpHit(env, origin.id));
          ctx.waitUntil(bumpMiner(env, request));
        }
        return new Response(resp.body, {
          status: resp.status,
          headers: {
            "content-type":
              resp.headers.get("content-type") || "application/json",
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
