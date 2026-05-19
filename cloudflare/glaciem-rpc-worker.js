/**
 * Glaciem RPC proxy (Cloudflare Worker).
 *
 * Sits in front of the rimed node and forwards daemon RPC calls to it.
 * Cloudflare absorbs connection storms (at difficulty 1, miners "find" a
 * block on the first nonce and hammer the node in a tight loop) so the
 * origin node only ever sees calm, pooled traffic.
 *
 * Origin: VM2, port 80 (socat) -> rimed RPC on :19081.
 * Workers fetch() needs a hostname, not a raw IP -- use VM2's Hetzner DNS name.
 */
const ORIGIN = "http://static.197.125.225.46.clients.your-server.de";

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const target = ORIGIN + url.pathname + url.search;

    const init = {
      method: request.method,
      headers: {
        "content-type":
          request.headers.get("content-type") || "application/octet-stream",
      },
    };
    if (request.method !== "GET" && request.method !== "HEAD") {
      init.body = await request.arrayBuffer();
    }

    try {
      const resp = await fetch(target, init);
      return new Response(resp.body, {
        status: resp.status,
        headers: {
          "content-type":
            resp.headers.get("content-type") || "application/json",
        },
      });
    } catch (e) {
      return new Response(
        JSON.stringify({ error: { code: -1, message: "node unreachable" } }),
        { status: 502, headers: { "content-type": "application/json" } }
      );
    }
  },
};
