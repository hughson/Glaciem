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

    // Optional stats readout: GET /stats returns today's per-node hit counts.
    // Cheap to expose since it's just one KV read per origin.
    const url = new URL(request.url);
    if (url.pathname === "/stats" && request.method === "GET") {
      const day = new Date().toISOString().slice(0, 10);
      const out = { day };
      for (const o of ORIGINS) {
        out[o.id] = parseInt(
          (await env.STATS.get(`nodehit:${o.id}:${day}`)) || "0", 10) || 0;
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
        // Stats bump runs after the response is on its way -- ctx.waitUntil
        // lets the KV write complete without delaying the user's response.
        if (env && ctx) ctx.waitUntil(bumpHit(env, origin.id));
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
