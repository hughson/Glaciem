package com.hughson.rime

import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/**
 * JSON-RPC client for rimed (node) and rime-wallet-rpc (wallet).
 * Ports the exact calls used by pow/app/miner_core.m.
 */
class RpcClient {
    // nodeHost/nodePort are the embedded wallet's daemon. Points at the
    // Cloudflare proxy so the wallet rides the same failover the miner does;
    // wallet2 picks up TLS on :443 via its e_ssl_support_autodetect default.
    @Volatile var nodeHost: String = "glaciem-rpc.frostmine.workers.dev"
    @Volatile var nodePort: Int = 443
    @Volatile var walletHost: String = "46.225.125.197"
    @Volatile var walletPort: Int = 29083

    // Miner RPC goes through the Cloudflare proxy, which absorbs the connection
    // storm a difficulty-1 miner produces; the node is never hit directly.
    private fun nodeUrl() = "https://glaciem-rpc.frostmine.workers.dev/json_rpc"
    private fun walletUrl() = "http://$walletHost:$walletPort/json_rpc"

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
        call(
            nodeUrl(), "get_block_template",
            JSONObject().put("wallet_address", walletAddress).put("reserve_size", 8),
        )

    fun submitBlock(blockHex: String): Boolean {
        val r = call(nodeUrl(), "submit_block", JSONArray().put(blockHex))
        return r?.optString("status") == "OK"
    }

    /** Current blockchain height of the node, or null if unreachable. */
    fun getNodeHeight(): Long? {
        val r = call(nodeUrl(), "get_info", JSONObject()) ?: return null
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
