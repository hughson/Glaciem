package com.hughson.rime

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/**
 * PeerCache -- Kotlin port of pow/wallet/peer_cache.[ch].
 *
 * Maintains a list of known Glaciem RPC endpoints (seeds + discovered peers)
 * shared between RpcClient (miner RPC) and MinerEngine (wallet failover).
 * Seeds are baked in at app start and never dropped; discovered peers come
 * from periodic /get_peer_list calls and persist across launches in
 * SharedPreferences. Thread-safe -- all mutations are @Synchronized.
 *
 * The same data shape is on disk on Mac (UserDefaults), Win (rime_peers.json
 * next to .exe), and Linux (QSettings); cross-platform peer state could
 * eventually be backed up / shared, though that's not done today.
 */
data class Peer(
    val host: String,
    val port: Int,
    val useSsl: Boolean,
    val isSeed: Boolean,
    var lastOk: Long = 0L,          // unix seconds; 0 = never
    var consecFailures: Int = 0,
    var avgLatencyMs: Int = 0,
)

class PeerCache(context: Context) {
    private val prefs = context.getSharedPreferences("rime", Context.MODE_PRIVATE)
    private val peers = mutableListOf<Peer>()
    private var rpcSinceDiscovery = 0

    companion object {
        private const val MAX_PEERS = 64
        private const val DISCOVERY_INTERVAL = 5
        private const val DROP_FAILURE_THRESHOLD = 5
        private const val STALE_LAST_OK_SECONDS = 7L * 24 * 3600
        private const val PREFS_KEY = "peerCache"
    }

    init {
        load()
    }

    @Synchronized
    fun addSeed(host: String, port: Int, useSsl: Boolean) {
        if (host.isEmpty() || port <= 0) return
        val idx = peers.indexOfFirst { it.host == host && it.port == port }
        if (idx < 0) {
            if (peers.size < MAX_PEERS) {
                peers.add(Peer(host, port, useSsl, isSeed = true))
            }
        } else {
            // upgrade existing entry to seed
            val p = peers[idx]
            peers[idx] = p.copy(isSeed = true, useSsl = useSsl)
        }
    }

    @Synchronized
    fun addDiscovered(host: String, port: Int) {
        if (host.isEmpty() || port <= 0) return
        if (peers.any { it.host == host && it.port == port }) return
        if (peers.size >= MAX_PEERS) return
        peers.add(Peer(host, port, useSsl = false, isSeed = false))
        save()
    }

    @Synchronized
    fun markSuccess(host: String, port: Int, latencyMs: Int) {
        val p = peers.firstOrNull { it.host == host && it.port == port } ?: return
        p.consecFailures = 0
        p.lastOk = System.currentTimeMillis() / 1000
        p.avgLatencyMs = if (p.avgLatencyMs == 0) latencyMs
                        else (p.avgLatencyMs * 7 + latencyMs * 3) / 10
        rpcSinceDiscovery++
        save()
    }

    @Synchronized
    fun markFailure(host: String, port: Int) {
        val idx = peers.indexOfFirst { it.host == host && it.port == port }
        if (idx < 0) return
        val p = peers[idx]
        p.consecFailures++
        val nowSec = System.currentTimeMillis() / 1000
        val stale = !p.isSeed && p.lastOk > 0 && (nowSec - p.lastOk) > STALE_LAST_OK_SECONDS
        val dead = !p.isSeed && p.consecFailures >= DROP_FAILURE_THRESHOLD
        if (stale || dead) {
            peers.removeAt(idx)
        }
        save()
    }

    /** Snapshot in attempt order: seeds first (insertion order), then
     *  discovered peers sorted by score (consec failures asc, last_ok desc,
     *  avg latency asc). */
    @Synchronized
    fun snapshot(): List<Peer> {
        val seeds = peers.filter { it.isSeed }
        val disc = peers.filter { !it.isSeed }
            .sortedWith(
                compareBy<Peer> { it.consecFailures }
                    .thenByDescending { it.lastOk }
                    .thenBy { it.avgLatencyMs }
            )
        return seeds + disc
    }

    @Synchronized
    fun shouldDiscover(): Boolean = rpcSinceDiscovery >= DISCOVERY_INTERVAL

    @Synchronized
    fun resetDiscoveryCounter() {
        rpcSinceDiscovery = 0
    }

    private fun load() {
        val blob = prefs.getString(PREFS_KEY, null) ?: return
        try {
            val obj = JSONObject(blob)
            val arr = obj.optJSONArray("peers") ?: return
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val host = o.optString("host")
                val port = o.optInt("port", 0)
                if (host.isEmpty() || port <= 0) continue
                // discard saved seeds; they get re-added on init so app updates
                // can change them without leaving stale entries.
                if (o.optInt("is_seed", 0) == 1) continue
                peers.add(
                    Peer(
                        host = host, port = port,
                        useSsl = o.optInt("use_ssl", 0) == 1,
                        isSeed = false,
                        lastOk = o.optLong("last_ok", 0),
                        consecFailures = o.optInt("consec_failures", 0),
                        avgLatencyMs = o.optInt("avg_latency_ms", 0),
                    )
                )
            }
        } catch (_: Throwable) { /* corrupt cache: fall back to seeds-only */ }
    }

    private fun save() {
        val arr = JSONArray()
        for (p in peers) {
            arr.put(
                JSONObject()
                    .put("host", p.host)
                    .put("port", p.port)
                    .put("use_ssl", if (p.useSsl) 1 else 0)
                    .put("is_seed", if (p.isSeed) 1 else 0)
                    .put("last_ok", p.lastOk)
                    .put("consec_failures", p.consecFailures)
                    .put("avg_latency_ms", p.avgLatencyMs)
            )
        }
        val obj = JSONObject().put("version", 1).put("peers", arr)
        prefs.edit().putString(PREFS_KEY, obj.toString()).apply()
    }
}
