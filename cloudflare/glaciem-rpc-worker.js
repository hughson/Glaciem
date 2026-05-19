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
const ORIGINS = [
  "http://static.197.125.225.46.clients.your-server.de",  // VM2 (46.225.125.197)
  "http://static.34.142.105.178.clients.your-server.de",  // VM1 (178.105.142.34)
];

// CORS so the Glaciem website can read live chain stats from the browser.
const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
  "Access-Control-Max-Age": "86400",
};

export default {
  async fetch(request) {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS });
    }

    const url = new URL(request.url);
    const path = url.pathname + url.search;
    const method = request.method;
    const ct = request.headers.get("content-type") || "application/octet-stream";
    let body;
    if (method !== "GET" && method !== "HEAD") {
      body = await request.arrayBuffer();
    }

    for (const origin of ORIGINS) {
      try {
        const resp = await fetch(origin + path, {
          method,
          headers: { "content-type": ct },
          body,
        });
        // 5xx -> node is failing, try the next one. Anything else (incl.
        // JSON-RPC error bodies, which come back as HTTP 200) is a real answer.
        if (resp.status >= 500) continue;
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
