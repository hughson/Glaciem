package com.hughson.rime

import android.content.Context
import android.graphics.Bitmap
import android.os.Bundle
import android.os.PowerManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// palette -- mirrors RimeMiner.swift
private val bg = Color(0xFF0D0D10)
private val card = Color(0xFF1A1A20)
private val amber = Color(0xFF3FC1E0)
private val dim = Color.White.copy(alpha = 0.42f)
private val mono = FontFamily.Monospace

class MainActivity : ComponentActivity() {

    private lateinit var rpc: RpcClient
    private lateinit var engine: MinerEngine
    private var wakeLock: PowerManager.WakeLock? = null

    // v1.1.12: settings state lives in Compose-tracked properties so the
    // Settings dialog always reflects the current saved values. Earlier
    // the Settings dialog was wired up with non-reactive snapshots read
    // once in setContent; saving worked (SharedPreferences + engine
    // both updated), but reopening the dialog read the stale snapshot
    // and showed the toggle in the old position -- making it look like
    // the change reverted unless the app was fully restarted.
    private var uiNodeHost     by mutableStateOf("")
    private var uiNodePort     by mutableStateOf(0)
    private var uiMiningMode   by mutableStateOf(MiningMode.MAX)
    private var uiPoolEnabled  by mutableStateOf(false)
    private var uiPoolUrl      by mutableStateOf("https://glaciem-pool.frostmine.workers.dev")

    /** The embedded wallet's cache file lives in app-private storage. */
    private fun walletPath() = filesDir.resolve("rime-wallet").absolutePath

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        rpc = RpcClient(this)
        engine = MinerEngine(rpc)
        loadSettings()                 // touches engine.setMiningMode -- must run after engine init

        // re-open the embedded wallet if one was already generated
        if (java.io.File(walletPath() + ".keys").exists()) {
            engine.openWallet(walletPath(), "")
        }

        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Rime::Mining")

        val selfTestOk = MinerNative.selfTest()
        android.util.Log.i("Rime", "native self-test: ${if (selfTestOk) "PASS" else "FAIL"}")

        setContent {
            // v1.1.12: read the reactive ui* properties so saveSettings()
            // updates flow through and reopening the dialog shows the
            // freshly-saved values.
            RimeScreen(
                engine = engine,
                selfTestOk = selfTestOk,
                walletPath = walletPath(),
                initialNodeHost = uiNodeHost,
                initialNodePort = uiNodePort,
                initialMiningMode = uiMiningMode,
                initialPoolEnabled = uiPoolEnabled,
                initialPoolUrl = uiPoolUrl,
                onToggleMining = ::toggleMining,
                onSaveSettings = ::saveSettings,
            )
        }
    }

    private fun toggleMining() {
        if (engine.isRunning()) {
            releaseWake()
            Thread { engine.stop() }.start()
        } else {
            acquireWake()
            engine.start()
        }
    }

    private fun acquireWake() {
        if (wakeLock?.isHeld != true) wakeLock?.acquire(12 * 60 * 60 * 1000L)
    }

    private fun releaseWake() {
        if (wakeLock?.isHeld == true) wakeLock?.release()
    }

    private fun loadSettings() {
        val p = getSharedPreferences("rime", Context.MODE_PRIVATE)
        // Upgrade users who saved the v1.0.0 direct-to-VM defaults. The wallet
        // now talks to the Cloudflare proxy so it benefits from node failover.
        // If the user customised the host, leave it alone.
        if (p.getString("nodeHost", null) == "46.225.125.197"
            && p.getInt("nodePort", 0) == 19081) {
            p.edit()
                .putString("nodeHost", "glaciem-rpc.frostmine.workers.dev")
                .putInt("nodePort", 443)
                .apply()
        }
        rpc.nodeHost = p.getString("nodeHost", rpc.nodeHost) ?: rpc.nodeHost
        rpc.nodePort = p.getInt("nodePort", rpc.nodePort)
        // Mining intensity (default MAX so the apparent hashrate matches what
        // the v1.0.0 patch shipped; users who notice heat can dial down).
        val modeName = p.getString("miningMode", MiningMode.MAX.name) ?: MiningMode.MAX.name
        val mode = runCatching { MiningMode.valueOf(modeName) }.getOrDefault(MiningMode.MAX)
        engine.setMiningMode(mode)
        // v1.1.6: pool mode. Default: off, official pool URL.
        val poolOn = p.getBoolean("poolEnabled", false)
        val poolUrlVal = p.getString("poolUrl", "https://glaciem-pool.frostmine.workers.dev")
            ?: "https://glaciem-pool.frostmine.workers.dev"
        engine.setPoolConfig(poolOn, poolUrlVal)
        // v1.1.12: mirror the loaded values into Compose-tracked state
        // so the Settings dialog opens with whatever's actually saved.
        uiNodeHost    = rpc.nodeHost
        uiNodePort    = rpc.nodePort
        uiMiningMode  = mode
        uiPoolEnabled = poolOn
        uiPoolUrl     = poolUrlVal
    }

    private fun saveSettings(nodeHost: String, nodePort: Int, mode: MiningMode,
                             poolEnabled: Boolean, poolUrl: String) {
        rpc.nodeHost = nodeHost.trim()
        rpc.nodePort = nodePort
        engine.setMiningMode(mode)
        engine.setPoolConfig(poolEnabled, poolUrl.trim())
        getSharedPreferences("rime", Context.MODE_PRIVATE).edit()
            .putString("nodeHost", rpc.nodeHost)
            .putInt("nodePort", rpc.nodePort)
            .putString("miningMode", mode.name)
            .putBoolean("poolEnabled", poolEnabled)
            .putString("poolUrl", poolUrl.trim())
            .apply()
        // v1.1.12: keep Compose state in sync so reopening Settings
        // shows the values the user just saved, not the launch-time
        // snapshot. The engine + SharedPreferences updates above were
        // already correct; this fixes the visual revert bug.
        uiNodeHost    = rpc.nodeHost
        uiNodePort    = rpc.nodePort
        uiMiningMode  = mode
        uiPoolEnabled = poolEnabled
        uiPoolUrl     = poolUrl.trim()
    }

    override fun onDestroy() {
        super.onDestroy()
        releaseWake()
        Thread { engine.stop() }.start()
    }
}

@Composable
private fun RimeScreen(
    engine: MinerEngine,
    selfTestOk: Boolean,
    walletPath: String,
    initialNodeHost: String,
    initialNodePort: Int,
    initialMiningMode: MiningMode,
    initialPoolEnabled: Boolean,
    initialPoolUrl: String,
    onToggleMining: () -> Unit,
    onSaveSettings: (String, Int, MiningMode, Boolean, String) -> Unit,
) {
    val stats by engine.stats.collectAsState()
    val running = stats.running

    val history = remember { mutableStateListOf<Float>() }
    LaunchedEffect(stats.totalHashes) {
        if (running) {
            history.add(stats.hashrate.toFloat())
            if (history.size > 90) history.removeAt(0)
        }
    }

    var showSend by remember { mutableStateOf(false) }
    var showReceive by remember { mutableStateOf(false) }
    var showHistory by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }
    // mining needs the embedded wallet's address -- without it the miner refuses.
    val hasAddress = stats.walletAddress.isNotBlank()

    val threads = remember { Runtime.getRuntime().availableProcessors().coerceIn(1, 8) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(bg)
            .verticalScroll(rememberScrollState())
            .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Header(running, stats.daemonConnected, stats.noAddress) { showSettings = true }

        if (!selfTestOk) {
            Text(
                "WARNING: native self-test FAILED -- this build does not match consensus",
                color = amber, fontFamily = mono, fontSize = 10.sp,
            )
        }

        HashratePanel(stats.hashrate, threads)
        Sparkline(history)
        StatsRow(stats)
        WalletPanel(
            stats = stats,
            onSend = { showSend = true },
            onReceive = { showReceive = true },
            onHistory = { showHistory = true },
        )
        HashPanel(stats.lastHash)
        Spacer(Modifier.height(4.dp))
        StartButton(running, hasAddress) { onToggleMining() }
        Spacer(Modifier.height(8.dp))
    }

    if (showSend) {
        SendDialog(engine, stats, onDismiss = { showSend = false })
    }
    if (showReceive) {
        ReceiveDialog(stats.walletAddress, onDismiss = { showReceive = false })
    }
    if (showHistory) {
        HistoryDialog(engine, onDismiss = { showHistory = false })
    }
    if (showSettings) {
        SettingsDialog(
            nodeHost0 = initialNodeHost,
            nodePort0 = initialNodePort,
            miningMode0 = initialMiningMode,
            poolEnabled0 = initialPoolEnabled,
            poolUrl0 = initialPoolUrl,
            engine = engine,
            walletPath = walletPath,
            onSave = { nh, np, mode, pe, pu ->
                onSaveSettings(nh, np, mode, pe, pu)
                showSettings = false
            },
            onDismiss = { showSettings = false },
        )
    }
}

@Composable
private fun Header(
    running: Boolean,
    daemonConnected: Boolean,
    noAddress: Boolean,
    onSettings: () -> Unit,
) {
    val statusText = when {
        !running -> "IDLE"
        noAddress -> "NO ADDRESS"
        daemonConnected -> "MINING"
        else -> "NO DAEMON"
    }
    val statusColor = when {
        !running -> dim
        noAddress -> amber
        daemonConnected -> Color(0xFF35C759)
        else -> amber
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column {
            Text("GLACIEM", color = amber, fontSize = 26.sp, fontWeight = FontWeight.Black)
            Text(
                "PROOF-OF-WORK MINER  ·  v${BuildConfig.GLACIEM_VERSION}",
                color = dim, fontFamily = mono,
                fontSize = 10.sp, fontWeight = FontWeight.SemiBold,
            )
        }
        Spacer(Modifier.weight(1f))
        Box(
            Modifier.size(8.dp).clip(RoundedCornerShape(4.dp)).background(statusColor),
        )
        Spacer(Modifier.width(6.dp))
        Text(
            statusText, color = statusColor, fontFamily = mono,
            fontSize = 11.sp, fontWeight = FontWeight.Bold,
        )
        Spacer(Modifier.width(12.dp))
        Box(
            modifier = Modifier
                .clip(RoundedCornerShape(7.dp))
                .background(card)
                .clickable { onSettings() }
                .padding(horizontal = 10.dp, vertical = 6.dp),
        ) {
            Text("HOST", color = dim, fontFamily = mono,
                fontSize = 10.sp, fontWeight = FontWeight.Bold)
        }
    }
}

@Composable
private fun HashratePanel(hashrate: Double, threads: Int) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(card)
            .padding(vertical = 18.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            "%.0f".format(hashrate), color = Color.White,
            fontFamily = mono, fontSize = 60.sp, fontWeight = FontWeight.Bold,
        )
        Text(
            "HASHES / SECOND", color = dim, fontFamily = mono,
            fontSize = 11.sp, fontWeight = FontWeight.SemiBold,
        )
        Text(
            "CPU · $threads threads", color = amber.copy(alpha = 0.8f),
            fontFamily = mono, fontSize = 11.sp,
            modifier = Modifier.padding(top = 2.dp),
        )
    }
}

@Composable
private fun Sparkline(data: List<Float>) {
    Canvas(
        modifier = Modifier
            .fillMaxWidth()
            .height(56.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(card),
    ) {
        if (data.size < 2) return@Canvas
        val w = size.width
        val h = size.height
        val mx = (data.maxOrNull() ?: 1f).coerceAtLeast(1f)
        val pts = data.mapIndexed { i, v ->
            Offset(
                x = w * i / (data.size - 1),
                y = h - h * 0.9f * (v / mx),
            )
        }
        val fill = Path().apply {
            moveTo(0f, h)
            pts.forEach { lineTo(it.x, it.y) }
            lineTo(w, h)
            close()
        }
        drawPath(fill, amber.copy(alpha = 0.22f))
        val line = Path().apply {
            moveTo(pts[0].x, pts[0].y)
            pts.drop(1).forEach { lineTo(it.x, it.y) }
        }
        drawPath(line, amber, style = androidx.compose.ui.graphics.drawscope.Stroke(width = 4f))
    }
}

@Composable
private fun StatsRow(stats: MinerStats) {
    // stats.height is only written while mining; fall back to the daemon tip
    // the wallet poll reports so the height shows even when idle.
    val chainHeight = if (stats.height > 0L) stats.height else stats.targetHeight
    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        StatTile("BLOCKS FOUND", stats.blocksFound.toString(), Modifier.weight(1f))
        StatTile("BLOCK HEIGHT", chainHeight.toString(), Modifier.weight(1f))
        StatTile("UPTIME", fmtTime(stats.uptimeSec), Modifier.weight(1f))
    }
}

@Composable
private fun StatTile(label: String, value: String, modifier: Modifier) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(card)
            .padding(vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            value, color = Color.White, fontFamily = mono,
            fontSize = 16.sp, fontWeight = FontWeight.Bold, maxLines = 1,
        )
        Spacer(Modifier.height(4.dp))
        Text(label, color = dim, fontFamily = mono, fontSize = 9.sp)
    }
}

@Composable
private fun WalletPanel(stats: MinerStats, onSend: () -> Unit, onReceive: () -> Unit,
                        onHistory: () -> Unit) {
    val connected = stats.walletConnected
    val syncing = stats.walletSyncing
    val addr = stats.walletAddress
    val shortAddr = when {
        addr.isEmpty() -> "—"
        addr.length > 24 -> addr.take(11) + "…" + addr.takeLast(11)
        else -> addr
    }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(card)
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("WALLET", color = dim, fontFamily = mono, fontSize = 9.sp)
            Spacer(Modifier.weight(1f))
            Text(
                when {
                    !connected -> "NO WALLET"
                    syncing -> "SYNCING"
                    else -> "CONNECTED"
                },
                color = when {
                    !connected -> dim
                    syncing -> amber
                    else -> Color(0xFF35C759)
                },
                fontFamily = mono, fontSize = 9.sp, fontWeight = FontWeight.Bold,
            )
        }
        if (syncing) {
            // wallet still scanning the chain -- show progress, not a partial balance
            Text(
                "catching up…", color = amber,
                fontFamily = mono, fontSize = 20.sp, fontWeight = FontWeight.Bold,
            )
            val target = stats.targetHeight.coerceAtLeast(1)
            val pct = (stats.walletHeight * 100 / target).coerceIn(0, 100)
            Text(
                "block ${stats.walletHeight} / ${stats.targetHeight}  ($pct%)",
                color = dim, fontFamily = mono, fontSize = 11.sp,
            )
        } else {
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    "%.6f".format(stats.balance / 1e12), color = Color.White,
                    fontFamily = mono, fontSize = 26.sp, fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.width(6.dp))
                Text("GLAC", color = dim, fontFamily = mono, fontSize = 11.sp)
            }
        }
        Text(
            shortAddr, color = amber.copy(alpha = 0.8f),
            fontFamily = mono, fontSize = 11.sp,
        )
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            PillButton("SEND", connected && !syncing, onSend)
            PillButton("RECEIVE", connected, onReceive)
            PillButton("HISTORY", connected, onHistory)
        }
    }
}

@Composable
private fun PillButton(label: String, enabled: Boolean, onClick: () -> Unit) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(7.dp))
            .background(if (enabled) amber else dim)
            .let { if (enabled) it.clickable { onClick() } else it }
            .padding(horizontal = 12.dp, vertical = 6.dp),
    ) {
        Text(label, color = bg, fontFamily = mono, fontSize = 10.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun HashPanel(lastHash: String) {
    val h = lastHash.ifEmpty { "-".repeat(64) }
    val zeros = h.takeWhile { it == '0' }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(card)
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text("LATEST HASH", color = dim, fontFamily = mono, fontSize = 9.sp)
        Text(
            buildAnnotatedHash(h, zeros.length),
            fontFamily = mono, fontSize = 12.sp,
        )
    }
}

@Composable
private fun buildAnnotatedHash(h: String, zeroCount: Int): AnnotatedString =
    AnnotatedString.Builder().apply {
        pushStyle(androidx.compose.ui.text.SpanStyle(color = amber))
        append(h.take(zeroCount))
        pop()
        pushStyle(androidx.compose.ui.text.SpanStyle(color = Color.White.copy(alpha = 0.55f)))
        append(h.drop(zeroCount))
        pop()
    }.toAnnotatedString()

@Composable
private fun StartButton(running: Boolean, hasAddress: Boolean, onClick: () -> Unit) {
    val blocked = !running && !hasAddress
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(13.dp))
            .background(if (blocked) card else if (running) Color(0xFFD93B3B) else amber)
            .let { if (blocked) it else it.clickable { onClick() } }
            .padding(vertical = 15.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            when {
                blocked -> "SET UP A WALLET IN SETTINGS"
                running -> "STOP"
                else -> "START MINING"
            },
            color = when {
                blocked -> dim
                running -> Color.White
                else -> bg
            },
            fontSize = 15.sp, fontWeight = FontWeight.Black,
        )
    }
}

// ---- dialogs ----

@Composable
private fun SendDialog(engine: MinerEngine, stats: MinerStats, onDismiss: () -> Unit) {
    var addr by remember { mutableStateOf("") }
    var amount by remember { mutableStateOf("") }
    var result by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    // v1.1.12: QR scanner for the recipient address. ZXing's
    // ScanContract returns the decoded text in the result; we then
    // validate it looks like an R-address before populating the field
    // so a stray QR (a URL, vCard, etc.) doesn't silently overwrite
    // what the user typed.
    val rAddrRegex = remember { Regex("^R[1-9A-HJ-NP-Za-km-z]{94,105}$") }
    val scanLauncher = rememberLauncherForActivityResult(ScanContract()) { scan ->
        val raw = scan?.contents ?: return@rememberLauncherForActivityResult
        // Some wallets encode their address as a "glaciem:R..." or
        // "monero:R..." URI. Strip a protocol prefix and any trailing
        // query string before validating.
        val cleaned = raw
            .substringAfter("://")
            .substringAfter(":")
            .substringBefore("?")
            .trim()
        if (rAddrRegex.matches(cleaned)) {
            addr = cleaned
            result = ""
        } else {
            result = "scanned QR isn't a Glaciem address"
        }
    }
    fun launchScanner() {
        val opts = ScanOptions().apply {
            setDesiredBarcodeFormats(ScanOptions.QR_CODE)
            setPrompt("Point camera at the recipient's Glaciem QR")
            setBeepEnabled(false)
            setOrientationLocked(false)
        }
        scanLauncher.launch(opts)
    }
    // Camera permission. zxing-android-embedded prompts automatically
    // when the scanner activity launches, but on first-grant we still
    // need a runtime permission flow.
    val cameraPermLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (granted) launchScanner()
        else result = "camera permission denied"
    }

    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(14.dp))
                .background(bg)
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("SEND GLAC", color = amber, fontFamily = mono,
                fontSize = 13.sp, fontWeight = FontWeight.Bold)
            Text(
                "unlocked balance: %.6f GLAC".format(stats.unlockedBalance / 1e12),
                color = dim, fontFamily = mono, fontSize = 10.sp,
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                OutlinedTextField(
                    value = addr, onValueChange = { addr = it },
                    label = { Text("recipient address") },
                    singleLine = true,
                    textStyle = TextStyle(color = Color.White, fontFamily = mono, fontSize = 11.sp),
                    colors = darkTextFieldColors(),
                    modifier = Modifier.weight(1f),
                )
                // v1.1.12: scan-QR button. We rely on the launcher's
                // permission contract to request CAMERA on first use.
                val ctx = androidx.compose.ui.platform.LocalContext.current
                TextButton(
                    onClick = {
                        val hasPerm = androidx.core.content.ContextCompat
                            .checkSelfPermission(ctx, android.Manifest.permission.CAMERA) ==
                            android.content.pm.PackageManager.PERMISSION_GRANTED
                        if (hasPerm) launchScanner()
                        else cameraPermLauncher.launch(android.Manifest.permission.CAMERA)
                    },
                ) {
                    Text("SCAN", color = amber, fontFamily = mono,
                        fontSize = 11.sp, fontWeight = FontWeight.Bold)
                }
            }
            OutlinedTextField(
                value = amount, onValueChange = { amount = it },
                label = { Text("amount (GLAC)") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                textStyle = TextStyle(color = Color.White, fontFamily = mono, fontSize = 12.sp),
                colors = darkTextFieldColors(),
            )
            if (result.isNotEmpty()) {
                Text(
                    result, fontFamily = mono, fontSize = 10.sp,
                    color = if (result.startsWith("sent") || result.startsWith("swept"))
                        Color(0xFF35C759) else amber,
                )
            }
            TextButton(
                onClick = {
                    if (sending) return@TextButton
                    sending = true
                    result = "sending…"
                    scope.launch {
                        val r = withContext(Dispatchers.IO) {
                            engine.send(addr, amount.toDoubleOrNull() ?: 0.0)
                        }
                        result = r
                        sending = false
                        // Clear the recipient + amount on a successful
                        // send so the user can't accidentally double-pay
                        // by hitting SEND again. Keep them on failure so
                        // the user can fix the issue and retry without
                        // re-typing.
                        if (r.startsWith("sent")) {
                            addr = ""
                            amount = ""
                        }
                    }
                },
            ) { Text("SEND", color = amber, fontWeight = FontWeight.Bold) }
            TextButton(
                onClick = {
                    if (sending) return@TextButton
                    sending = true
                    result = "sweeping…"
                    scope.launch {
                        val r = withContext(Dispatchers.IO) { engine.sweepUnmixable() }
                        result = r
                        sending = false
                    }
                },
            ) { Text("SWEEP UNMIXABLE", color = amber, fontWeight = FontWeight.Bold) }
            Text(
                "use if a send fails with \"not enough outputs\" — " +
                    "consolidates mined coins so they can be spent",
                color = dim, fontFamily = mono, fontSize = 8.sp,
            )
            TextButton(onClick = onDismiss) { Text("CLOSE", color = Color.White) }
        }
    }
}

@Composable
private fun HistoryDialog(engine: MinerEngine, onDismiss: () -> Unit) {
    var text by remember { mutableStateOf("loading…") }
    val scope = rememberCoroutineScope()
    fun reload() {
        scope.launch { text = withContext(Dispatchers.IO) { engine.history() } }
    }
    LaunchedEffect(Unit) { reload() }

    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(14.dp))
                .background(bg)
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("TRANSACTION HISTORY", color = amber, fontFamily = mono,
                fontSize = 13.sp, fontWeight = FontWeight.Bold)
            Text("sends, sweeps & received transfers — mining rewards omitted",
                color = dim, fontFamily = mono, fontSize = 9.sp)
            Column(
                modifier = Modifier
                    .height(320.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                Text(text, color = Color.White.copy(alpha = 0.85f),
                    fontFamily = mono, fontSize = 10.sp)
            }
            Row {
                TextButton(onClick = { reload() }) {
                    Text("REFRESH", color = amber, fontWeight = FontWeight.Bold)
                }
                TextButton(onClick = onDismiss) {
                    Text("CLOSE", color = Color.White)
                }
            }
        }
    }
}

@Composable
private fun ReceiveDialog(address: String, onDismiss: () -> Unit) {
    val clipboard = LocalClipboardManager.current
    val qr = remember(address) { qrBitmap(address) }
    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(14.dp))
                .background(bg)
                .padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text("RECEIVE GLAC", color = amber, fontFamily = mono,
                fontSize = 13.sp, fontWeight = FontWeight.Bold)
            if (qr != null) {
                androidx.compose.foundation.Image(
                    bitmap = qr.asImageBitmap(),
                    contentDescription = "address QR",
                    modifier = Modifier.size(200.dp).clip(RoundedCornerShape(8.dp)),
                )
            }
            Text(
                address.ifEmpty { "no wallet connected" },
                color = Color.White.copy(alpha = 0.85f),
                fontFamily = mono, fontSize = 11.sp,
            )
            TextButton(
                onClick = { clipboard.setText(AnnotatedString(address)) },
            ) { Text("COPY ADDRESS", color = amber, fontWeight = FontWeight.Bold) }
            TextButton(onClick = onDismiss) { Text("CLOSE", color = Color.White) }
        }
    }
}

@Composable
private fun SettingsDialog(
    nodeHost0: String,
    nodePort0: Int,
    miningMode0: MiningMode,
    poolEnabled0: Boolean,
    poolUrl0: String,
    engine: MinerEngine,
    walletPath: String,
    onSave: (String, Int, MiningMode, Boolean, String) -> Unit,
    onDismiss: () -> Unit,
) {
    var nodeHost by remember { mutableStateOf(nodeHost0) }
    var nodePort by remember { mutableStateOf(nodePort0.toString()) }
    var miningMode by remember { mutableStateOf(miningMode0) }
    var poolEnabled by remember { mutableStateOf(poolEnabled0) }
    var poolUrl by remember { mutableStateOf(poolUrl0) }
    var showGenerated by remember { mutableStateOf(false) }
    var showRestore by remember { mutableStateOf(false) }
    var genAddr by remember { mutableStateOf("") }
    var genSeed by remember { mutableStateOf("") }

    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(14.dp))
                .background(bg)
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("SETTINGS", color = amber, fontFamily = mono,
                fontSize = 13.sp, fontWeight = FontWeight.Bold)
            Text(
                "The Glaciem node the wallet syncs from. The miner ignores " +
                    "this — it uses an automatic multi-node fallback. Set " +
                    "127.0.0.1 port 19081 to point the wallet at a local rimed.",
                color = dim, fontFamily = mono, fontSize = 9.sp,
            )
            SettingField("node address", nodeHost) { nodeHost = it }
            SettingField("node port", nodePort, numeric = true) { nodePort = it }
            Text(
                "MINING INTENSITY — Eco uses 1 core (quiet & cool), Balanced uses " +
                    "half, Max uses all cores (full hashrate, more heat & battery).",
                color = dim, fontFamily = mono, fontSize = 9.sp,
            )
            IntensityPicker(selected = miningMode) { miningMode = it }

            // ---- v1.1.6: pool mode toggle + URL ----
            Spacer(Modifier.height(4.dp))
            Text(
                if (poolEnabled)
                    "MINING MODE — POOL. Shares get submitted to the pool below; payouts arrive once your contribution crosses the pool's threshold."
                else
                    "MINING MODE — SOLO. Direct daemon submission; you keep 100% of any block you find but block-finds are rare on a phone.",
                color = dim, fontFamily = mono, fontSize = 9.sp,
            )
            androidx.compose.foundation.layout.Row(
                verticalAlignment = androidx.compose.ui.Alignment.CenterVertically
            ) {
                Text("POOL MODE",
                    color = if (poolEnabled) amber else dim,
                    fontFamily = mono, fontSize = 11.sp,
                    fontWeight = FontWeight.Bold)
                Spacer(Modifier.weight(1f))
                androidx.compose.material3.Switch(
                    checked = poolEnabled,
                    onCheckedChange = { poolEnabled = it },
                )
            }
            if (poolEnabled) {
                SettingField("pool URL", poolUrl) { poolUrl = it }
            }

            Text(
                "WALLET — generate a wallet to mine to and hold GLAC. Balance and " +
                    "Send use the built-in wallet; no rime-wallet-rpc needed.",
                color = dim, fontFamily = mono, fontSize = 9.sp,
            )
            TextButton(
                onClick = {
                    val kp = MinerNative.generateAddress()
                    if (kp != null && kp.size == 2) {
                        genAddr = kp[0]; genSeed = kp[1]; showGenerated = true
                    }
                },
            ) { Text("GENERATE NEW WALLET", color = amber, fontWeight = FontWeight.Bold) }
            TextButton(
                onClick = { showRestore = true },
            ) { Text("RESTORE FROM SEED", color = amber, fontWeight = FontWeight.Bold) }
            TextButton(
                onClick = {
                    val pu = poolUrl.trim().ifBlank { "https://glaciem-pool.frostmine.workers.dev" }
                    onSave(nodeHost, nodePort.toIntOrNull() ?: 19081, miningMode, poolEnabled, pu)
                },
            ) { Text("SAVE", color = amber, fontWeight = FontWeight.Bold) }
            TextButton(onClick = onDismiss) { Text("CANCEL", color = Color.White) }
        }
    }

    if (showGenerated) {
        GeneratedAddressDialog(
            address = genAddr,
            seed = genSeed,
            onUse = {
                // a freshly generated wallet replaces any previous one
                java.io.File(walletPath).delete()
                java.io.File("$walletPath.keys").delete()
                java.io.File("$walletPath.address.txt").delete()
                engine.openWallet(walletPath, genSeed)   // create the embedded wallet
                showGenerated = false
                onDismiss()
            },
            onDismiss = { showGenerated = false },
        )
    }
    if (showRestore) {
        RestoreWalletDialog(
            engine = engine,
            walletPath = walletPath,
            onDone = { showRestore = false; onDismiss() },
            onDismiss = { showRestore = false },
        )
    }
}

/** Recover an existing wallet from its 25-word seed -- replaces the wallet
 *  currently in the app and re-opens it via the same recover path. */
@Composable
private fun RestoreWalletDialog(
    engine: MinerEngine,
    walletPath: String,
    onDone: () -> Unit,
    onDismiss: () -> Unit,
) {
    var seed by remember { mutableStateOf("") }
    var err by remember { mutableStateOf("") }
    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(14.dp))
                .background(bg)
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("RESTORE WALLET", color = amber, fontFamily = mono,
                fontSize = 13.sp, fontWeight = FontWeight.Bold)
            Text(
                "Paste the 25-word recovery seed you saved when you created the " +
                    "wallet. This replaces the wallet currently in this app.",
                color = dim, fontFamily = mono, fontSize = 9.sp,
            )
            OutlinedTextField(
                value = seed, onValueChange = { seed = it },
                label = { Text("25-word recovery seed") },
                textStyle = TextStyle(color = Color.White, fontFamily = mono, fontSize = 11.sp),
                colors = darkTextFieldColors(),
                minLines = 3, maxLines = 4,
            )
            if (err.isNotEmpty()) {
                Text(err, color = amber, fontFamily = mono, fontSize = 10.sp)
            }
            TextButton(onClick = {
                val words = seed.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
                if (words.size < 24) { err = "Enter the full recovery seed (25 words)."; return@TextButton }
                java.io.File(walletPath).delete()
                java.io.File("$walletPath.keys").delete()
                java.io.File("$walletPath.address.txt").delete()
                engine.openWallet(walletPath, words.joinToString(" "))
                onDone()
            }) { Text("RESTORE", color = amber, fontWeight = FontWeight.Bold) }
            TextButton(onClick = onDismiss) { Text("CANCEL", color = Color.White) }
        }
    }
}

/** Shows a freshly generated address + its 25-word recovery seed, with a
 *  back-it-up warning. "Use this address" fills the settings field. */
@Composable
private fun GeneratedAddressDialog(
    address: String,
    seed: String,
    onUse: () -> Unit,
    onDismiss: () -> Unit,
) {
    val clipboard = LocalClipboardManager.current
    Dialog(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(14.dp))
                .background(bg)
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text("NEW MINING ADDRESS", color = amber, fontFamily = mono,
                fontSize = 13.sp, fontWeight = FontWeight.Bold)

            Text("ADDRESS", color = dim, fontFamily = mono, fontSize = 9.sp)
            Text(address, color = Color.White, fontFamily = mono, fontSize = 11.sp)

            Text("25-WORD RECOVERY SEED", color = dim, fontFamily = mono, fontSize = 9.sp)
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(8.dp))
                    .background(card)
                    .padding(10.dp),
            ) {
                Text(seed, color = amber, fontFamily = mono, fontSize = 12.sp)
            }

            Text(
                "IMPORTANT — write this seed down now and keep it safe. It is the " +
                    "only way to recover this wallet. It is not stored anywhere and " +
                    "CANNOT be recovered if you lose it.",
                color = amber, fontFamily = mono, fontSize = 10.sp,
                fontWeight = FontWeight.SemiBold,
            )

            TextButton(onClick = { clipboard.setText(AnnotatedString(seed)) }) {
                Text("COPY SEED", color = Color.White, fontWeight = FontWeight.Bold)
            }
            TextButton(onClick = onUse) {
                Text("I SAVED IT — USE THIS ADDRESS", color = amber,
                    fontWeight = FontWeight.Bold)
            }
            TextButton(onClick = onDismiss) { Text("CANCEL", color = Color.White) }
        }
    }
}

/** White-text-on-dark-bg colours for OutlinedTextField. The Material3 default
 *  uses surface-tinted text which is unreadable on this app's dark palette. */
@Composable
private fun darkTextFieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = Color.White,
    unfocusedTextColor = Color.White,
    cursorColor = amber,
    focusedBorderColor = amber,
    unfocusedBorderColor = dim,
    focusedLabelColor = amber,
    unfocusedLabelColor = dim,
)

@Composable
private fun SettingField(
    label: String,
    value: String,
    numeric: Boolean = false,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        keyboardOptions = if (numeric) {
            KeyboardOptions(keyboardType = KeyboardType.Number)
        } else {
            KeyboardOptions.Default
        },
        textStyle = TextStyle(color = Color.White, fontFamily = mono, fontSize = 12.sp),
        colors = darkTextFieldColors(),
    )
}

/** 3-button segmented control for ECO / BALANCED / MAX mining intensity. */
@Composable
private fun IntensityPicker(
    selected: MiningMode,
    onSelect: (MiningMode) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        for (mode in MiningMode.values()) {
            val isSel = mode == selected
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(6.dp))
                    .background(if (isSel) amber else card)
                    .clickable { onSelect(mode) }
                    .padding(vertical = 8.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    mode.name,
                    color = if (isSel) bg else Color.White,
                    fontFamily = mono,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
    }
}

// ---- helpers ----

private fun fmtTime(seconds: Double): String {
    val t = seconds.toInt()
    return "%02d:%02d:%02d".format(t / 3600, (t / 60) % 60, t % 60)
}

private fun qrBitmap(text: String): Bitmap? {
    if (text.isEmpty()) return null
    return try {
        val size = 420
        val matrix = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, size, size)
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        for (x in 0 until size) {
            for (y in 0 until size) {
                bmp.setPixel(x, y, if (matrix[x, y]) android.graphics.Color.BLACK else android.graphics.Color.WHITE)
            }
        }
        bmp
    } catch (e: Exception) {
        null
    }
}
