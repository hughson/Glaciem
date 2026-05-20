package com.hughson.rime

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import kotlin.random.Random

/** Mining intensity selector -- how many cores the miner uses per batch.
 *  ECO = quiet & cool (1 core), BALANCED = compromise (half cores),
 *  MAX = full hashrate (all cores). Persisted in SharedPreferences. */
enum class MiningMode { ECO, BALANCED, MAX }

/** Snapshot of miner + wallet state for the UI. Mirrors MinerStats in miner_core.h. */
data class MinerStats(
    val running: Boolean = false,
    val daemonConnected: Boolean = false,
    val hashrate: Double = 0.0,
    val totalHashes: Long = 0,
    val height: Long = 0,
    val difficulty: Long = 0,
    val blocksFound: Long = 0,
    val bestBits: Int = 0,
    val uptimeSec: Double = 0.0,
    val walletConnected: Boolean = false,
    val balance: Long = 0,
    val unlockedBalance: Long = 0,
    val lastHash: String = "",
    val walletAddress: String = "",
    val walletSyncing: Boolean = false,
    val walletHeight: Long = 0,
    val targetHeight: Long = 0,
    val noAddress: Boolean = false,
)

/**
 * The Rime mining engine. Ports pow/app/miner_core.m: a coordinator thread
 * pulls block templates from rimed, hashes nonces across CPU worker threads
 * (the GPU Y-phase of the Mac app runs on the CPU here -- v1 is CPU-only), and
 * submits blocks that meet the target. A separate thread polls the wallet RPC.
 */
class MinerEngine(private val rpc: RpcClient) {

    private val _stats = MutableStateFlow(MinerStats())
    val stats: StateFlow<MinerStats> = _stats.asStateFlow()

    @Volatile private var running = false
    private var minerThread: Thread? = null
    private var walletThread: Thread? = null

    private val maxCores = Runtime.getRuntime().availableProcessors().coerceIn(1, 8)
    private val pool = Executors.newFixedThreadPool(maxCores)

    // Mining intensity -- read at the top of each batch so a mode switch takes
    // effect within ~1 batch (sub-second). Idle pool threads in ECO/BALANCED
    // cost effectively nothing.
    @Volatile private var miningMode: MiningMode = MiningMode.MAX
    private fun activeCores(): Int = when (miningMode) {
        MiningMode.ECO      -> 1
        MiningMode.BALANCED -> ((maxCores + 1) / 2).coerceAtLeast(1)
        MiningMode.MAX      -> maxCores
    }
    fun setMiningMode(m: MiningMode) { miningMode = m }
    fun getMiningMode(): MiningMode = miningMode

    // mining stats (written by the miner thread)
    @Volatile private var daemonConnected = false
    @Volatile private var hashrate = 0.0
    @Volatile private var totalHashes = 0L
    @Volatile private var height = 0L
    @Volatile private var difficulty = 0L
    @Volatile private var blocksFound = 0L
    @Volatile private var bestBits = 0
    @Volatile private var startTimeNs = 0L
    @Volatile private var lastHash = ""
    @Volatile private var noAddress = false

    // embedded wallet (librime_wallet.so -- Monero wallet_api). The handle is
    // an opaque native pointer; 0 means "no wallet open yet".
    @Volatile private var walletHandle = 0L

    // wallet state (written by the wallet poll thread)
    @Volatile private var walletAddr = ""
    @Volatile private var walletOk = false
    @Volatile private var balance = 0L
    @Volatile private var unlocked = 0L
    @Volatile private var walletSyncing = false
    @Volatile private var walletHeight = 0L
    @Volatile private var targetHeight = 0L

    init {
        startWalletPoll()
    }

    /** Open the embedded wallet for [seed] (a 25-word seed) at [path], or open
     *  the existing wallet file there when [seed] is blank. Runs on its own
     *  thread -- recover/scan can take a while. */
    fun openWallet(path: String, seed: String) {
        Thread {
            val daemon = "${rpc.nodeHost}:${rpc.nodePort}"
            val h = WalletNative.recover(path, seed, daemon, 0L)
            walletHandle = h
            android.util.Log.i(TAG, "openWallet ${if (h != 0L) "ok" else "FAILED"}")
        }.apply { isDaemon = true }.start()
    }

    fun hasWallet() = walletHandle != 0L

    fun start() {
        if (running) return
        running = true
        minerThread = Thread { mineLoop() }.apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        minerThread?.join(8000)
        minerThread = null
        hashrate = 0.0          // idle -> show 0, not the last reading
        publish()
    }

    fun isRunning() = running

    /** Convert RME to atomic units and send from the embedded wallet.
     *  Blocking -- call off the UI thread. */
    fun send(address: String, amountTmpr: Double): String {
        if (address.isBlank()) return "Enter a recipient address"
        if (amountTmpr <= 0.0) return "Enter an amount greater than 0"
        val h = walletHandle
        if (h == 0L) return "Wallet not ready yet"
        val atomic = (amountTmpr * 1e12 + 0.5).toLong()
        return WalletNative.send(h, address.trim(), atomic)
    }

    /** Sweep unmixable outputs (mostly mined/coinbase rewards) so they become
     *  spendable. Blocking -- call off the UI thread. */
    fun sweepUnmixable(): String {
        val h = walletHandle
        if (h == 0L) return "Wallet not ready yet"
        return WalletNative.sweepUnmixable(h)
    }

    /** Recent transaction history (sends/sweeps/receives), newest first.
     *  Blocking -- call off the UI thread. */
    fun history(): String {
        val h = walletHandle
        if (h == 0L) return "Wallet not ready yet"
        return WalletNative.history(h)
    }

    @Synchronized
    private fun startWalletPoll() {
        if (walletThread != null) return
        walletThread = Thread { walletLoop() }.apply { isDaemon = true; start() }
    }

    private fun publish() {
        _stats.value = MinerStats(
            running = running,
            daemonConnected = daemonConnected,
            hashrate = if (running) hashrate else 0.0,
            totalHashes = totalHashes,
            height = height,
            difficulty = difficulty,
            blocksFound = blocksFound,
            bestBits = bestBits,
            uptimeSec = if (startTimeNs > 0L) (System.nanoTime() - startTimeNs) / 1e9 else 0.0,
            walletConnected = walletOk,
            balance = balance,
            unlockedBalance = unlocked,
            lastHash = lastHash,
            walletAddress = walletAddr,
            walletSyncing = walletSyncing,
            walletHeight = walletHeight,
            targetHeight = targetHeight,
            noAddress = noAddress,
        )
    }

    private fun walletLoop() {
        while (true) {
            val h = walletHandle
            if (h != 0L) {
                try {
                    // Publish cached state FIRST -- the address is derived from the
                    // keys and the balance was persisted by store(), so both are
                    // available the instant the wallet file opens. Without this the
                    // UI shows "NO WALLET" / 0 for the whole blocking re-scan below.
                    walletAddr    = WalletNative.address(h)
                    balance       = WalletNative.balance(h)
                    unlocked      = WalletNative.unlockedBalance(h)
                    walletHeight  = WalletNative.height(h)
                    publish()

                    WalletNative.refresh(h)               // blocking chain scan
                    walletOk      = WalletNative.connected(h)
                    balance       = WalletNative.balance(h)
                    unlocked      = WalletNative.unlockedBalance(h)
                    walletAddr    = WalletNative.address(h)
                    walletHeight  = WalletNative.height(h)
                    targetHeight  = WalletNative.daemonHeight(h)
                    walletSyncing = walletOk && !WalletNative.isSynchronized(h)
                    WalletNative.store(h)                 // persist so balance survives a relaunch
                } catch (e: Throwable) {
                    walletOk = false
                    android.util.Log.w(TAG, "wallet poll: ${e.message}")
                }
            } else {
                walletOk = false
            }
            publish()
            try { Thread.sleep(4000) } catch (e: InterruptedException) { return }
        }
    }

    private fun mineLoop() {
        startTimeNs = System.nanoTime()
        totalHashes = 0; blocksFound = 0; bestBits = 0; hashrate = 0.0
        var curSeed = ByteArray(0)
        val rnd = Random(System.nanoTime())
        var prevDaemon = -1
        android.util.Log.i(TAG, "mining started (mode=$miningMode, ${activeCores()}/$maxCores threads)")

        while (running) {
            // mine to the embedded wallet's own address. No fallback -- with no
            // wallet open yet, the miner refuses to mine.
            val mineTo = walletAddr
            if (mineTo.isBlank()) {
                noAddress = true
                daemonConnected = false
                hashrate = 0.0
                publish()
                if (!sleepWhileRunning(1000)) break
                continue
            }
            noAddress = false
            val tpl = rpc.getBlockTemplate(mineTo)
            val hbHex = tpl?.optString("blockhashing_blob")
            val tbHex = tpl?.optString("blocktemplate_blob")
            if (tpl == null || hbHex.isNullOrEmpty() || tbHex.isNullOrEmpty()) {
                if (prevDaemon != 0) {
                    android.util.Log.w(TAG, "daemon unreachable at ${rpc.nodeHost}:${rpc.nodePort}")
                    prevDaemon = 0
                }
                daemonConnected = false
                hashrate = 0.0
                publish()
                if (!sleepWhileRunning(1000)) break
                continue
            }
            daemonConnected = true
            if (prevDaemon != 1) {
                android.util.Log.i(TAG, "daemon connected at ${rpc.nodeHost}:${rpc.nodePort}, " +
                    "height=${tpl.optLong("height")} difficulty=${tpl.optLong("difficulty")}")
                prevDaemon = 1
            }

            val hb = hexToBytes(hbHex)
            val tb = hexToBytes(tbHex)
            if (hb == null || tb == null || hb.size > 250) {
                if (!sleepWhileRunning(300)) break
                continue
            }
            height = tpl.optLong("height")
            difficulty = tpl.optLong("difficulty")
            val nonceOff = nonceOffset(hb)
            if (nonceOff + 4 > hb.size) {
                if (!sleepWhileRunning(300)) break
                continue
            }

            val seed = hexToBytes(tpl.optString("seed_hash") ?: "")
            val epochSeed = if (seed != null && seed.size == 32) seed else ByteArray(32)
            if (!epochSeed.contentEquals(curSeed)) {
                MinerNative.setEpochSeed(epochSeed)
                curSeed = epochSeed
            }

            // one parallel batch: `n` cores hash CHUNK contiguous nonces each.
            // Snapshot `n` once so task fan-out and hashesThisBatch stay in sync
            // if the mode changes mid-batch.
            val n = activeCores()
            val base = rnd.nextInt().toLong() and 0xFFFFFFFFL
            val lastHashBuf = ByteArray(32)
            val t0 = System.nanoTime()
            val tasks = (0 until n).map { idx ->
                Callable {
                    val lh = ByteArray(32)
                    val bb = intArrayOf(bestBits)
                    val start = (base + idx.toLong() * CHUNK) and 0xFFFFFFFFL
                    val w = MinerNative.hash(hb, nonceOff, start, CHUNK, difficulty, lh, bb)
                    if (bb[0] > bestBits) bestBits = bb[0]
                    synchronized(lastHashBuf) { System.arraycopy(lh, 0, lastHashBuf, 0, 32) }
                    w
                }
            }
            var winner = -1L
            for (f in pool.invokeAll(tasks)) {
                val w = try { f.get() } catch (e: Exception) { -1L }
                if (w >= 0L && winner < 0L) winner = w
            }

            val dt = (System.nanoTime() - t0) / 1e9
            val hashesThisBatch = CHUNK.toLong() * n
            totalHashes += hashesThisBatch
            val inst = if (dt > 0.0) hashesThisBatch / dt else 0.0
            hashrate = if (hashrate <= 0.0) inst else 0.7 * hashrate + 0.3 * inst
            lastHash = bytesToHex(lastHashBuf)
            publish()

            if (winner >= 0L) {
                val block = tb.copyOf()
                val w = winner.toInt()
                block[nonceOff] = (w and 0xFF).toByte()
                block[nonceOff + 1] = ((w ushr 8) and 0xFF).toByte()
                block[nonceOff + 2] = ((w ushr 16) and 0xFF).toByte()
                block[nonceOff + 3] = ((w ushr 24) and 0xFF).toByte()
                val ok = rpc.submitBlock(bytesToHex(block))
                if (ok) blocksFound++
                android.util.Log.i(TAG, "submit_block nonce=$w -> ${if (ok) "OK (height ~$height)" else "rejected"}")
                publish()
            }
        }
        hashrate = 0.0
        running = false
        publish()
    }

    /** Sleep up to [ms], waking early if mining is stopped. Returns false if stopped. */
    private fun sleepWhileRunning(ms: Int): Boolean {
        var left = ms
        while (left > 0 && running) {
            val step = minOf(left, 100)
            try { Thread.sleep(step.toLong()) } catch (e: InterruptedException) { return false }
            left -= step
        }
        return running
    }

    companion object {
        private const val TAG = "Rime"

        /** nonces hashed per core per batch. Matches the Windows miner's value;
         *  small enough to keep UI updates responsive, large enough to amortise
         *  the pool's invokeAll() barrier across many hashes. */
        private const val CHUNK = 64

        /** Offset of the 4-byte nonce in a block blob: 3 leading varints
         *  (major, minor, timestamp) + 32-byte prev_id. From miner_core.m. */
        fun nonceOffset(blob: ByteArray): Int {
            var p = 0
            var v = 0
            while (v < 3 && p < blob.size) {
                var n = 1
                while (p + n - 1 < blob.size && (blob[p + n - 1].toInt() and 0x80) != 0) n++
                p += n
                v++
            }
            return p + 32
        }

        fun hexToBytes(hex: String): ByteArray? {
            if (hex.length % 2 != 0) return null
            val out = ByteArray(hex.length / 2)
            for (i in out.indices) {
                val hi = Character.digit(hex[i * 2], 16)
                val lo = Character.digit(hex[i * 2 + 1], 16)
                if (hi < 0 || lo < 0) return null
                out[i] = ((hi shl 4) or lo).toByte()
            }
            return out
        }

        fun bytesToHex(bytes: ByteArray): String {
            val sb = StringBuilder(bytes.size * 2)
            for (b in bytes) {
                val v = b.toInt() and 0xFF
                sb.append("0123456789abcdef"[v ushr 4])
                sb.append("0123456789abcdef"[v and 0xF])
            }
            return sb.toString()
        }
    }
}
