package com.hughson.rime

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/**
 * JSON-RPC client for rimed (node) and rime-wallet-rpc (wallet).
 * Ports the exact calls used by pow/app/miner_core.m.
 */
class RpcClient(context: Context) {
    // nodeHost/nodePort are the embedded wallet's daemon. Points at the
    // Cloudflare proxy so the wallet rides the same failover the miner does;
    // wallet2 picks up TLS on :443 via its e_ssl_support_autodetect default.
    @Volatile var nodeHost: String = "glaciem-rpc.frostmine.workers.dev"
    @Volatile var nodePort: Int = 443
    @Volatile var walletHost: String = "46.225.125.197"
    @Volatile var walletPort: Int = 29083

    // Shared peer cache (seeds + discovered peers, persisted across launches).
    // Both the miner-RPC path here AND the wallet failover in MinerEngine
    // snapshot from this cache.
    val peerCache = PeerCache(context).apply {
        // The Cloudflare Worker stays primary -- absorbs the connection storm
        // a difficulty-1 miner produces. The direct-node hostnames are seed
        // fallbacks for when the Worker is unreachable.
        addSeed("glaciem-rpc.frostmine.workers.dev", 443, useSsl = true)
        addSeed("static.197.125.225.46.clients.your-server.de", 19081, useSsl = false)
        addSeed("static.34.142.105.178.clients.your-server.de", 19081, useSsl = false)
    }

    private fun walletUrl() = "http://$walletHost:$walletPort/json_rpc"

    /** POST a JSON-RPC request via the peer cache: snapshot, try in order,
     *  update scores, opportunistically call /get_peer_list. */
    private fun callAny(method: String, params: Any?): JSONObject? {
        for (peer in peerCache.snapshot()) {
            val scheme = if (peer.useSsl) "https" else "http"
            val url = "$scheme://${peer.host}:${peer.port}/json_rpc"
            val t0 = System.currentTimeMillis()
            val r = call(url, method, params)
            val latency = (System.currentTimeMillis() - t0).toInt()
            if (r != null) {
                peerCache.markSuccess(peer.host, peer.port, latency)
                // v1.1.4: tryDiscoverPeers() removed -- the public proxy
                // now 403s /get_peer_list, so this call was wasted.
                // Peers come from seeded endpoints only.
                return r
            }
            peerCache.markFailure(peer.host, peer.port)
        }
        return null
    }

    /** After a successful RPC, occasionally ask the working node for its
     *  peer list; add any peers advertising an rpc_port to the cache. */
    private fun tryDiscoverPeers(source: Peer) {
        if (!peerCache.shouldDiscover()) return
        peerCache.resetDiscoveryCounter()
        val scheme = if (source.useSsl) "https" else "http"
        val url = "$scheme://${source.host}:${source.port}/get_peer_list"
        try {
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "POST"
                connectTimeout = 5000
                readTimeout = 6000
                doOutput = true
                setRequestProperty("Content-Type", "application/json")
            }
            conn.outputStream.use { it.write(ByteArray(0)) }
            if (conn.responseCode != 200) { conn.disconnect(); return }
            val text = conn.inputStream.bufferedReader().use { it.readText() }
            conn.disconnect()
            val white = JSONObject(text).optJSONArray("white_list") ?: return
            var added = 0
            for (i in 0 until white.length()) {
                val p = white.optJSONObject(i) ?: continue
                val host = p.optString("host")
                val rpcPort = p.optInt("rpc_port", 0)
                if (host.isEmpty() || rpcPort <= 0 || rpcPort > 65535) continue
                peerCache.addDiscovered(host, rpcPort)
                if (++added >= 4) break
            }
        } catch (_: Throwable) { /* discovery is best-effort */ }
    }

    /** POST a JSON-RPC request, return its `result` object (or null on failure). */
    private fun call(urlStr: String, method: String, params: Any?): JSONObject? {
        return try {
            val body = JSONObject()
                .put("jsonrpc", "2.0")
                .put("id", "0")
                .put("method", method)
                .put("params", params ?: JSONObject())
            val conn = (URL(urlStr).openConnection() as HttpURLConnection).apply {
                requestMethod = "POST"
                connectTimeout = 5000
                readTimeout = 6000
                doOutput = true
                setRequestProperty("Content-Type", "application/json")
            }
            conn.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
            if (conn.responseCode != 200) {
                conn.disconnect()
                return null
            }
            val text = conn.inputStream.bufferedReader().use { it.readText() }
            conn.disconnect()
            JSONObject(text).optJSONObject("result")
        } catch (e: Exception) {
            null
        }
    }

    // ---- node (rimed) ----

    fun getBlockTemplate(walletAddress: String): JSONObject? =
        callAny(
            "get_block_template",
            JSONObject().put("wallet_address", walletAddress).put("reserve_size", 8),
        )

    fun submitBlock(blockHex: String): Boolean {
        val r = callAny("submit_block", JSONArray().put(blockHex))
        return r?.optString("status") == "OK"
    }

    /** Current blockchain height of the node, or null if unreachable. */
    fun getNodeHeight(): Long? {
        val r = callAny("get_info", JSONObject()) ?: return null
        return if (r.has("height")) r.optLong("height") else null
    }

    // ---- wallet (rime-wallet-rpc) ----

    /** Primary address, or null if the wallet RPC is unreachable. */
    fun getAddress(): String? =
        call(walletUrl(), "get_address", JSONObject().put("account_index", 0))
            ?.optString("address")
            ?.takeIf { it.isNotEmpty() }

    /** Height the wallet has scanned to, or null if the wallet RPC is unreachable. */
    fun getWalletHeight(): Long? {
        val r = call(walletUrl(), "get_height", JSONObject()) ?: return null
        return if (r.has("height")) r.optLong("height") else null
    }

    /** (balance, unlockedBalance) in atomic units, or null if unreachable. */
    fun getBalance(): Pair<Long, Long>? {
        val r = call(walletUrl(), "get_balance", JSONObject().put("account_index", 0))
            ?: return null
        return r.optLong("balance") to r.optLong("unlocked_balance")
    }

    /** Transfer `atomic` units to `address`. Returns a human-readable result line. */
    fun transfer(address: String, atomic: Long): String {
        val destinations = JSONArray().put(
            JSONObject().put("amount", atomic).put("address", address),
        )
        val params = JSONObject()
            .put("destinations", destinations)
            .put("account_index", 0)
            .put("priority", 0)
            .put("get_tx_key", true)
        val r = call(walletUrl(), "transfer", params)
            ?: return "Send failed -- check the address, amount, and unlocked balance"
        val tx = r.optString("tx_hash")
        if (tx.isNullOrEmpty()) {
            return "Send failed -- check the address, amount, and unlocked balance"
        }
        val fee = r.optLong("fee")
        return "Sent  (fee %.6f GLAC)  tx %.12s...".format(fee / 1e12, tx)
    }
}
