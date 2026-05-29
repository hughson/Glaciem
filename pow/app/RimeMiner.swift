// RimeMiner.swift -- SwiftUI front-end for the Lattice PoW miner.
// Wraps miner_core (CPU-only Lattice PoW). Dark UI, amber accent.
//
// Two structural rules keep SwiftUI's AttributeGraph from crashing on macOS 26:
//
//  1. Each panel is its OWN View struct, not a computed property on ContentView.
//     Computed-property panels inline into one monolithic `body` type, giving
//     AttributeGraph a single enormous layout descriptor it mis-walks.
//
//  2. The C struct MinerStats never enters the view graph. It is copied into a
//     native Swift `MinerSnapshot` at the tick boundary. AttributeGraph builds
//     layout descriptors by reflecting type metadata; the imported C struct's
//     layout was being mis-read (a scalar field walked as a retainable pointer,
//     then swift_retain'd -> EXC_BAD_ACCESS). A native Swift struct has a
//     layout AttributeGraph reflects correctly.

import SwiftUI
import CoreImage

// palette
private let bg      = Color(red: 0.05, green: 0.05, blue: 0.065)
private let card    = Color(red: 0.10, green: 0.10, blue: 0.125)
private let amber   = Color(red: 0.247, green: 0.757, blue: 0.878)
private let dimText = Color.white.opacity(0.42)

@main
struct RimeMinerApp: App {
    init() {
        // Upgrade users who saved the v1.0.0 direct-to-VM defaults. The
        // wallet now talks to the Cloudflare proxy so it benefits from
        // node failover; if the user customised the host, leave it alone.
        let ud = UserDefaults.standard
        if ud.string(forKey: "nodeHost") == "46.225.125.197"
           && (ud.object(forKey: "nodePort") as? Int) == 19081 {
            ud.set("glaciem-rpc.frostmine.workers.dev", forKey: "nodeHost")
            ud.set(443, forKey: "nodePort")
        }
    }
    var body: some Scene {
        WindowGroup("Glaciem Miner  ·  v\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?")") {
            ContentView().frame(width: 460, height: 880)
        }
        .windowResizability(.contentSize)
    }
}

// Native Swift mirror of the C MinerStats. Only this type flows through the
// SwiftUI view graph -- never the imported C struct (see file header).
struct MinerSnapshot: Equatable {
    var running         = false
    var daemonConnected = false
    var hashrate        = 0.0
    var totalHashes: UInt64 = 0
    var height: UInt64  = 0
    var difficulty: UInt64 = 0
    var blocksFound: UInt64 = 0
    var bestBits        = 0
    var uptime          = 0.0
    var walletConnected = false
    var balance: UInt64 = 0
    var unlockedBalance: UInt64 = 0
    var walletSyncing   = false
    var walletHeight: UInt64 = 0
    var targetHeight: UInt64 = 0
    var noAddress       = false

    init() {}
    init(_ c: MinerStats) {
        running         = c.running == 1
        daemonConnected = c.daemon_connected == 1
        hashrate        = c.hashrate
        totalHashes     = c.total_hashes
        height          = c.height
        difficulty      = c.difficulty
        blocksFound     = c.blocks_found
        bestBits        = Int(c.best_bits)
        uptime          = c.uptime_s
        walletConnected = c.wallet_connected == 1
        balance         = c.balance
        unlockedBalance = c.unlocked_balance
        walletSyncing   = c.wallet_syncing == 1
        walletHeight    = c.wallet_height
        targetHeight    = c.target_height
        noAddress       = c.no_address == 1
    }
}

struct ContentView: View {
    @State private var stats   = MinerSnapshot()
    @State private var device  = "-"
    @State private var lastHash = ""
    @State private var history: [Double] = []
    @State private var walletAddr      = "—"
    @State private var walletAddrFull  = ""
    @State private var walletConnected = false
    @State private var balanceStr      = "0.000000"
    @State private var nodeHost = UserDefaults.standard.string(forKey: "nodeHost") ?? "glaciem-rpc.frostmine.workers.dev"
    @State private var nodePort = (UserDefaults.standard.object(forKey: "nodePort") as? Int) ?? 443
    // v1.1.6: pool mode. Defaults to SOLO (false) so upgrade is a no-op
    // behaviorally for existing users. Pool URL defaults to the official
    // pool; the field is editable so users can point at any pool.
    @State private var poolEnabled = UserDefaults.standard.bool(forKey: "poolEnabled")
    @State private var poolURL     = UserDefaults.standard.string(forKey: "poolURL") ?? "https://glaciem-pool.frostmine.workers.dev"
    // v1.1.14+: thread-count picker. Default = (maxCores+1)/2 ("Recommended"),
    // computed in onAppear once miner_max_cores() is callable.
    @State private var threadCount = UserDefaults.standard.integer(forKey: "threadCount")
    @State private var showSettings = false

    private let tick = Timer.publish(every: 0.12, on: .main, in: .common).autoconnect()
    // mining pays coinbase rewards to the embedded wallet -- enabled once it is open.
    private var hasWallet: Bool { !walletAddrFull.isEmpty }

    var body: some View {
        ZStack {
            bg.ignoresSafeArea()
            VStack(spacing: 20) {
                HeaderView(stats: stats, onSettings: { showSettings = true })
                HashratePanel(hashrate: stats.hashrate, device: device)
                Sparkline(data: history)
                    .frame(height: 56)
                    .padding(.horizontal, 4)
                StatsRow(stats: stats)
                WalletPanel(stats: stats, walletConnected: walletConnected,
                            walletAddr: walletAddr, walletAddrFull: walletAddrFull,
                            balanceStr: balanceStr)
                HashPanel(lastHash: lastHash)
                Spacer(minLength: 0)
                StartButton(running: stats.running, hasWallet: hasWallet)
            }
            .padding(24)
        }
        .onAppear {
            miner_set_node(nodeHost, Int32(nodePort))
            miner_set_pool_config(poolEnabled ? 1 : 0, poolURL)
            // v1.1.14+: thread count picker. UserDefaults.integer returns 0
            // for "not set yet" -- on first run we adopt the engine's
            // built-in default (Recommended = (maxCores+1)/2).
            let maxCores = Int(miner_max_cores())
            if threadCount < 1 || threadCount > maxCores {
                threadCount = Int(miner_get_thread_count())
            }
            miner_set_thread_count(Int32(threadCount))
            // re-open the embedded wallet if one was already generated
            if FileManager.default.fileExists(atPath: walletPath() + ".keys") {
                miner_open_wallet(walletPath(), "")
            }
        }
        .sheet(isPresented: $showSettings) {
            SettingsSheet(host0: nodeHost, port0: nodePort,
                          poolEnabled0: poolEnabled, poolURL0: poolURL,
                          threadCount0: threadCount,
                          maxCores: Int(miner_max_cores()),
                          walletPath: walletPath(),
                onSave: { h, p, pe, pu, tc in
                    nodeHost = h; nodePort = p
                    poolEnabled = pe; poolURL = pu
                    threadCount = tc
                    UserDefaults.standard.set(h, forKey: "nodeHost")
                    UserDefaults.standard.set(p, forKey: "nodePort")
                    UserDefaults.standard.set(pe, forKey: "poolEnabled")
                    UserDefaults.standard.set(pu, forKey: "poolURL")
                    UserDefaults.standard.set(tc, forKey: "threadCount")
                    miner_set_node(h, Int32(p))
                    miner_set_pool_config(pe ? 1 : 0, pu)
                    miner_set_thread_count(Int32(tc))
                    showSettings = false
                },
                onCancel: { showSettings = false })
        }
        .onReceive(tick) { _ in
            let s = MinerSnapshot(miner_get_stats())
            stats    = s
            device   = String(cString: miner_device())
            lastHash = String(cString: miner_last_hash())
            walletConnected = s.walletConnected
            let a = String(cString: miner_wallet_address())
            walletAddrFull = a
            walletAddr = a.isEmpty ? "—"
                : (a.count > 24 ? String(a.prefix(11)) + "…" + String(a.suffix(11)) : a)
            balanceStr = String(format: "%.6f", Double(s.balance) / 1e12)
            if s.running {
                history.append(s.hashrate)
                if history.count > 90 { history.removeFirst() }
            }
        }
    }
}

// MARK: header
private struct HeaderView: View {
    let stats: MinerSnapshot
    let onSettings: () -> Void
    @State private var pulse = false

    private var statusText: String {
        if !stats.running { return "IDLE" }
        if stats.noAddress { return "NO ADDRESS" }
        return stats.daemonConnected ? "MINING" : "NO DAEMON"
    }
    private var statusColor: Color {
        if !stats.running { return dimText }
        if stats.noAddress { return amber }
        return stats.daemonConnected ? .green : amber
    }

    var body: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 2) {
                Text("GLACIEM")
                    .font(.system(size: 26, weight: .heavy, design: .rounded))
                    .foregroundColor(amber)
                Text("PROOF-OF-WORK MINER  ·  v\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?")")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundColor(dimText)
                    .tracking(2)
            }
            Spacer()
            HStack(spacing: 6) {
                Circle()
                    .fill(statusColor)
                    .frame(width: 8, height: 8)
                    .opacity(stats.running ? (pulse ? 0.35 : 1.0) : 1.0)
                    .animation(.easeInOut(duration: 0.7).repeatForever(autoreverses: true),
                               value: pulse)
                Text(statusText)
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(statusColor)
            }
            .onAppear { pulse = true }
            Button(action: onSettings) {
                Text("SETTINGS")
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(dimText)
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(card)
                    .clipShape(RoundedRectangle(cornerRadius: 7))
            }
            .buttonStyle(.plain)
            .padding(.leading, 10)
        }
    }
}

// MARK: settings sheet (node + wallet + v1.1.6 pool mode)
private struct SettingsSheet: View {
    let walletPath: String
    let maxCores: Int
    /// Save callback: (host, port, poolEnabled, poolURL, threadCount)
    let onSave: (String, Int, Bool, String, Int) -> Void
    let onCancel: () -> Void
    @State private var host: String
    @State private var port: String
    @State private var poolEnabled: Bool
    @State private var poolURL: String
    @State private var threadCount: Int
    @State private var generated: GenWallet?
    @State private var showRestore = false

    init(host0: String, port0: Int,
         poolEnabled0: Bool, poolURL0: String,
         threadCount0: Int, maxCores: Int,
         walletPath: String,
         onSave: @escaping (String, Int, Bool, String, Int) -> Void,
         onCancel: @escaping () -> Void) {
        self.walletPath = walletPath
        self.maxCores = max(1, maxCores)
        self.onSave = onSave
        self.onCancel = onCancel
        _host = State(initialValue: host0)
        _port = State(initialValue: String(port0))
        _poolEnabled = State(initialValue: poolEnabled0)
        _poolURL = State(initialValue: poolURL0)
        _threadCount = State(initialValue: min(max(1, threadCount0), max(1, maxCores)))
    }

    private var recommendedThreads: Int { max(1, (maxCores + 1) / 2) }

    private func generateNewWallet() {
        let a = UnsafeMutablePointer<CChar>.allocate(capacity: 160)
        let s = UnsafeMutablePointer<CChar>.allocate(capacity: 600)
        defer { a.deallocate(); s.deallocate() }
        a[0] = 0; s[0] = 0
        if miner_generate_address(a, 160, s, 600) == 1 {
            generated = GenWallet(addr: String(cString: a), seed: String(cString: s))
        }
    }

    var body: some View {
        VStack(spacing: 12) {
            Text("SETTINGS")
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(amber).tracking(2)
            Text("The Glaciem node the wallet syncs from. The miner ignores this setting — it uses an automatic multi-node fallback (Cloudflare Worker + bootstrap nodes + discovered peers). To use your own local node, set 127.0.0.1 port 19081.")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
            TextField("node address", text: $host)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 12, design: .monospaced))
            TextField("node port", text: $port)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 12, design: .monospaced))

            // ---- v1.1.6: pool mode toggle + URL ----
            Toggle(isOn: $poolEnabled) {
                Text("MINING MODE — POOL")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(amber).tracking(1.5)
            }
            .toggleStyle(.switch)
            .padding(.top, 4)
            Text(poolEnabled
                 ? "Pool mode: shares submitted to the pool below; payouts arrive once your contribution crosses the pool's threshold."
                 : "Solo mode: this miner submits full blocks directly to the daemon — you keep 100% of any block you find but block-finds are rare with small hashrate.")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
            TextField("pool URL", text: $poolURL)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11, design: .monospaced))
                .disabled(!poolEnabled)
                .opacity(poolEnabled ? 1.0 : 0.5)

            // ---- v1.1.14+: thread-count picker ----
            Text("CPU THREADS — how many cores the miner uses. More threads = more hashrate, more heat & fan noise. Recommended is half your cores.")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 4)
            HStack(spacing: 8) {
                Button(action: { if threadCount > 1 { threadCount -= 1 } }) {
                    Text("−")
                        .font(.system(size: 18, weight: .heavy, design: .monospaced))
                        .foregroundColor(threadCount > 1 ? bg : dimText)
                        .frame(width: 40, height: 40)
                        .background(threadCount > 1 ? amber : card)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }.buttonStyle(.plain).disabled(threadCount <= 1)
                Text("\(threadCount) of \(maxCores)")
                    .font(.system(size: 14, weight: .bold, design: .monospaced))
                    .foregroundColor(amber)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10)
                    .background(card)
                    .clipShape(RoundedRectangle(cornerRadius: 7))
                Button(action: { if threadCount < maxCores { threadCount += 1 } }) {
                    Text("+")
                        .font(.system(size: 18, weight: .heavy, design: .monospaced))
                        .foregroundColor(threadCount < maxCores ? bg : dimText)
                        .frame(width: 40, height: 40)
                        .background(threadCount < maxCores ? amber : card)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }.buttonStyle(.plain).disabled(threadCount >= maxCores)
            }
            HStack(spacing: 8) {
                Button(action: { threadCount = recommendedThreads }) {
                    Text("RECOMMENDED (\(recommendedThreads))")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(threadCount == recommendedThreads ? bg : .white)
                        .frame(maxWidth: .infinity).padding(.vertical, 7)
                        .background(threadCount == recommendedThreads ? amber : card)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }.buttonStyle(.plain)
                Button(action: { threadCount = maxCores }) {
                    Text("ALL (\(maxCores))")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(threadCount == maxCores ? bg : .white)
                        .frame(maxWidth: .infinity).padding(.vertical, 7)
                        .background(threadCount == maxCores ? amber : card)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }.buttonStyle(.plain)
            }

            Text("WALLET — generate a wallet to mine to and hold GLAC. Balance and Send use this built-in wallet.")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
            Button(action: generateNewWallet) {
                Text("GENERATE NEW WALLET")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(bg)
                    .frame(maxWidth: .infinity).padding(.vertical, 9)
                    .background(amber).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)
            Button(action: { showRestore = true }) {
                Text("RESTORE FROM SEED")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(amber)
                    .frame(maxWidth: .infinity).padding(.vertical, 9)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)
            Button(action: {
                let h = host.trimmingCharacters(in: .whitespacesAndNewlines)
                let pu = poolURL.trimmingCharacters(in: .whitespacesAndNewlines)
                onSave(h.isEmpty ? "glaciem-rpc.frostmine.workers.dev" : h,
                       Int(port) ?? 443,
                       poolEnabled,
                       pu.isEmpty ? "https://glaciem-pool.frostmine.workers.dev" : pu,
                       min(max(1, threadCount), max(1, maxCores)))
            }) {
                Text("SAVE")
                    .font(.system(size: 13, weight: .heavy, design: .rounded))
                    .foregroundColor(bg)
                    .frame(maxWidth: .infinity).padding(.vertical, 10)
                    .background(amber).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)
            Button(action: onCancel) {
                Text("CANCEL")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity).padding(.vertical, 8)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain).keyboardShortcut(.cancelAction)
        }
        .padding(22)
        .frame(width: 380)
        .background(bg)
        .sheet(item: $generated) { g in
            GenerateSheet(genAddr: g.addr, genSeed: g.seed, walletPath: walletPath,
                          onUsed: onCancel)
        }
        .sheet(isPresented: $showRestore) {
            RestoreSheet(walletPath: walletPath, onUsed: onCancel)
        }
    }
}

// MARK: restore-wallet sheet — recover an existing wallet from its 25-word seed
private struct RestoreSheet: View {
    let walletPath: String
    let onUsed: () -> Void
    @State private var seedInput = ""
    @State private var status = ""

    var body: some View {
        VStack(spacing: 12) {
            Text("RESTORE WALLET")
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(amber).tracking(2)
            Text("Paste the 25-word recovery seed you saved when you created the wallet. This replaces the wallet currently in this app.")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
            TextEditor(text: $seedInput)
                .font(.system(size: 11, design: .monospaced))
                .frame(height: 96)
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(card, lineWidth: 1))
            if !status.isEmpty {
                Text(status)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(amber)
            }
            Button(action: {
                let words = seedInput
                    .split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" || $0 == "\r" })
                if words.count < 24 {
                    status = "Enter the full recovery seed (25 words)."
                    return
                }
                let clean = words.joined(separator: " ")
                let fm = FileManager.default
                try? fm.removeItem(atPath: walletPath)
                try? fm.removeItem(atPath: walletPath + ".keys")
                try? fm.removeItem(atPath: walletPath + ".address.txt")
                miner_open_wallet(walletPath, clean)
                onUsed()
            }) {
                Text("RESTORE")
                    .font(.system(size: 13, weight: .heavy, design: .rounded))
                    .foregroundColor(bg)
                    .frame(maxWidth: .infinity).padding(.vertical, 10)
                    .background(amber).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)
            Button(action: onUsed) {
                Text("CANCEL")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity).padding(.vertical, 8)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain).keyboardShortcut(.cancelAction)
        }
        .padding(22)
        .frame(width: 380)
        .background(bg)
    }
}

// MARK: hashrate hero
private struct HashratePanel: View {
    let hashrate: Double
    let device: String
    var body: some View {
        VStack(spacing: 2) {
            Text(String(format: "%.0f", hashrate))
                .font(.system(size: 68, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
                .contentTransition(.numericText())
                .animation(.default, value: hashrate)
            Text("HASHES / SECOND")
                .font(.system(size: 11, weight: .semibold, design: .monospaced))
                .foregroundColor(dimText).tracking(2)
            Text(device)
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(amber.opacity(0.8))
                .padding(.top, 2)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 18)
        .background(card)
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }
}

// MARK: stat tiles
private struct StatsRow: View {
    let stats: MinerSnapshot
    var body: some View {
        HStack(spacing: 12) {
            StatTile(label: "BLOCKS FOUND", value: "\(stats.blocksFound)")
            StatTile(label: "BLOCK HEIGHT", value: "\(stats.height)")
            StatTile(label: "UPTIME", value: fmtTime(stats.uptime))
        }
    }
}

private struct StatTile: View {
    let label: String
    let value: String
    var body: some View {
        VStack(spacing: 5) {
            Text(value)
                .font(.system(size: 16, weight: .bold, design: .monospaced))
                .foregroundColor(.white).lineLimit(1).minimumScaleFactor(0.6)
            Text(label)
                .font(.system(size: 9, weight: .semibold, design: .monospaced))
                .foregroundColor(dimText).tracking(1)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 12)
        .background(card)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
}

// a freshly generated address + seed, presented to the sheet as one item so
// the values can never lag the sheet (the .sheet(isPresented:) race).
private struct GenWallet: Identifiable {
    let id = UUID()
    let addr: String
    let seed: String
}

// MARK: generated-wallet sheet
private struct GenerateSheet: View {
    let genAddr: String
    let genSeed: String
    let walletPath: String
    let onUsed: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 14) {
            Text("NEW WALLET")
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(amber).tracking(2)

            VStack(alignment: .leading, spacing: 4) {
                Text("ADDRESS")
                    .font(.system(size: 9, weight: .semibold, design: .monospaced))
                    .foregroundColor(dimText).tracking(1)
                Text(genAddr)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(.white)
                    .textSelection(.enabled)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            VStack(alignment: .leading, spacing: 4) {
                Text("25-WORD RECOVERY SEED")
                    .font(.system(size: 9, weight: .semibold, design: .monospaced))
                    .foregroundColor(dimText).tracking(1)
                Text(genSeed)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundColor(amber)
                    .textSelection(.enabled)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(10)
            .background(card)
            .clipShape(RoundedRectangle(cornerRadius: 8))

            Text("IMPORTANT — write this seed down now and keep it safe. It is the only way to recover this wallet. It is not stored anywhere and CANNOT be recovered if you lose it.")
                .font(.system(size: 10, weight: .semibold, design: .monospaced))
                .foregroundColor(amber)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)

            Button(action: {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(genSeed, forType: .string)
            }) {
                Text("COPY SEED")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity).padding(.vertical, 9)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)

            Button(action: {
                // a freshly generated wallet replaces any previous one
                let fm = FileManager.default
                try? fm.removeItem(atPath: walletPath)
                try? fm.removeItem(atPath: walletPath + ".keys")
                try? fm.removeItem(atPath: walletPath + ".address.txt")
                miner_open_wallet(walletPath, genSeed)
                dismiss()
                onUsed()
            }) {
                Text("I SAVED IT — USE THIS WALLET")
                    .font(.system(size: 13, weight: .heavy, design: .rounded))
                    .foregroundColor(bg)
                    .frame(maxWidth: .infinity).padding(.vertical, 11)
                    .background(amber).clipShape(RoundedRectangle(cornerRadius: 10))
            }.buttonStyle(.plain)

            Button(action: { dismiss() }) {
                Text("CANCEL")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(dimText)
            }.buttonStyle(.plain).keyboardShortcut(.cancelAction)
        }
        .padding(24)
        .frame(width: 420)
        .background(bg)
    }
}

// MARK: wallet
private struct WalletPanel: View {
    let stats: MinerSnapshot
    let walletConnected: Bool
    let walletAddr: String
    let walletAddrFull: String
    let balanceStr: String
    @State private var showReceive = false
    @State private var showSend = false
    @State private var showHistory = false

    var body: some View {
        // A wallet exists the moment its address is published (key derivation,
        // no chain scan). "connecting" = open but not yet reached the daemon;
        // "syncing" = connected but still scanning. These let us show real
        // progress instead of a misleading "NO WALLET" during startup.
        let hasWallet  = !walletAddrFull.isEmpty
        let syncing    = stats.walletSyncing
        let connecting = hasWallet && !walletConnected
        let pct = stats.targetHeight > 0
            ? min(UInt64(100), stats.walletHeight * 100 / stats.targetHeight) : 0
        let statusText  = !hasWallet ? "NO WALLET"
                        : connecting ? "CONNECTING…"
                        : syncing    ? "SYNCING"
                        :              "CONNECTED"
        let statusColor: Color = !hasWallet ? dimText
                        : (connecting || syncing) ? amber : .green
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("WALLET")
                    .font(.system(size: 9, weight: .semibold, design: .monospaced))
                    .foregroundColor(dimText).tracking(1)
                Spacer()
                Text(statusText)
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .foregroundColor(statusColor)
            }
            if connecting {
                Text("connecting…")
                    .font(.system(size: 20, weight: .bold, design: .monospaced))
                    .foregroundColor(amber)
            } else if syncing {
                // wallet still scanning the chain -- show progress, not a partial balance
                Text("catching up…")
                    .font(.system(size: 20, weight: .bold, design: .monospaced))
                    .foregroundColor(amber)
                Text("block \(stats.walletHeight) / \(stats.targetHeight)  (\(pct)%)")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(dimText)
            } else {
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(balanceStr)
                        .font(.system(size: 26, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                    Text("GLAC")
                        .font(.system(size: 11, weight: .semibold, design: .monospaced))
                        .foregroundColor(dimText)
                }
            }
            HStack {
                Text(walletAddr)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(amber.opacity(0.8))
                Spacer()
                Button(action: { showSend = true }) {
                    Text("SEND")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(bg)
                        .padding(.horizontal, 12).padding(.vertical, 5)
                        .background((walletConnected && !syncing) ? amber : dimText)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }
                .buttonStyle(.plain).disabled(!walletConnected || syncing)
                Button(action: { showReceive = true }) {
                    Text("RECEIVE")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(bg)
                        .padding(.horizontal, 12).padding(.vertical, 5)
                        .background(hasWallet ? amber : dimText)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }
                .buttonStyle(.plain).disabled(!hasWallet)
                Button(action: { showHistory = true }) {
                    Text("HISTORY")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundColor(bg)
                        .padding(.horizontal, 12).padding(.vertical, 5)
                        .background(hasWallet ? amber : dimText)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                }
                .buttonStyle(.plain).disabled(!hasWallet)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .background(card)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .sheet(isPresented: $showReceive) {
            ReceiveSheet(walletAddrFull: walletAddrFull, showReceive: $showReceive)
        }
        .sheet(isPresented: $showSend) {
            SendSheet(balanceStr: balanceStr, showSend: $showSend)
        }
        .sheet(isPresented: $showHistory) {
            HistorySheet(showHistory: $showHistory)
        }
    }
}

private struct HistorySheet: View {
    @Binding var showHistory: Bool
    @State private var history = "loading…"

    var body: some View {
        VStack(spacing: 12) {
            Text("TRANSACTION HISTORY")
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(amber).tracking(2)
            Text("sends, sweeps & received transfers — mining rewards omitted")
                .font(.system(size: 9, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.center)
            ScrollView {
                Text(history)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(.white.opacity(0.85))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .frame(height: 300)
            .padding(8)
            .background(card)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            HStack(spacing: 10) {
                Button(action: { history = String(cString: miner_history()) }) {
                    Text("REFRESH")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(amber)
                        .frame(maxWidth: .infinity).padding(.vertical, 8)
                        .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
                }.buttonStyle(.plain)
                Button(action: { showHistory = false }) {
                    Text("DONE")
                        .font(.system(size: 12, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                        .frame(maxWidth: .infinity).padding(.vertical, 8)
                        .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
                }.buttonStyle(.plain).keyboardShortcut(.cancelAction)
            }
        }
        .padding(22)
        .frame(width: 430)
        .background(bg)
        .onAppear { history = String(cString: miner_history()) }
    }
}

// MARK: receive sheet
private struct ReceiveSheet: View {
    let walletAddrFull: String
    @Binding var showReceive: Bool

    var body: some View {
        VStack(spacing: 14) {
            Text("RECEIVE GLAC")
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(amber).tracking(2)
            if let qr = qrImage(walletAddrFull) {
                Image(nsImage: qr)
                    .interpolation(.none)
                    .resizable().frame(width: 190, height: 190)
                    .background(Color.white)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
            }
            Text(walletAddrFull.isEmpty ? "no wallet connected" : walletAddrFull)
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(.white.opacity(0.8))
                .multilineTextAlignment(.center)
                .textSelection(.enabled)
                .frame(maxWidth: 320)
            Button(action: {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(walletAddrFull, forType: .string)
            }) {
                Text("COPY ADDRESS")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity).padding(.vertical, 9)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)
            Button(action: { showReceive = false }) {
                Text("DONE")
                    .font(.system(size: 13, weight: .heavy, design: .rounded))
                    .foregroundColor(bg)
                    .frame(maxWidth: .infinity).padding(.vertical, 11)
                    .background(amber).clipShape(RoundedRectangle(cornerRadius: 10))
            }.buttonStyle(.plain).keyboardShortcut(.cancelAction)
        }
        .padding(24)
        .frame(width: 360)
        .background(bg)
    }
}

// MARK: send sheet
// A .glac name that resolved and is awaiting the user's explicit confirmation
// before we broadcast — so the user always sees the real destination address.
private struct PendingGlacSend {
    let address: String
    let amount: Double
    let name: String
}

private struct SendSheet: View {
    let balanceStr: String
    @Binding var showSend: Bool
    @State private var sendAddr = ""
    @State private var sendAmount = ""
    @State private var sendResult = ""
    @State private var resolving = false
    @State private var pending: PendingGlacSend? = nil

    var body: some View {
        VStack(spacing: 12) {
            Text("SEND GLAC")
                .font(.system(size: 13, weight: .bold, design: .monospaced))
                .foregroundColor(amber).tracking(2)
            Text("unlocked balance: \(balanceStr) GLAC")
                .font(.system(size: 10, design: .monospaced))
                .foregroundColor(dimText)
            TextField("recipient address or name.glac", text: $sendAddr)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11, design: .monospaced))
            TextField("amount (GLAC)", text: $sendAmount)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 12, design: .monospaced))
            if !sendResult.isEmpty {
                Text(sendResult)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor((sendResult.hasPrefix("sent") || sendResult.hasPrefix("swept")) ? .green : amber)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 330)
            }
            if let p = pending {
                // A .glac name resolved -- show exactly where the money will go
                // and require an explicit confirm before broadcasting.
                VStack(spacing: 7) {
                    Text("\(p.name) resolves to")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(dimText)
                    Text(p.address)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(amber)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 320)
                        .textSelection(.enabled)
                    Text("Send \(String(format: "%.6f", p.amount)) GLAC to this address?")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                    HStack(spacing: 10) {
                        Button(action: { pending = nil; sendResult = "send cancelled" }) {
                            Text("CANCEL")
                                .font(.system(size: 11, weight: .bold, design: .monospaced))
                                .foregroundColor(.white)
                                .frame(maxWidth: .infinity).padding(.vertical, 8)
                                .background(card).clipShape(RoundedRectangle(cornerRadius: 8))
                        }.buttonStyle(.plain)
                        Button(action: {
                            let p2 = p; pending = nil
                            doSend(to: p2.address, amount: p2.amount, name: p2.name)
                        }) {
                            Text("CONFIRM SEND")
                                .font(.system(size: 11, weight: .bold, design: .monospaced))
                                .foregroundColor(bg)
                                .frame(maxWidth: .infinity).padding(.vertical, 8)
                                .background(amber).clipShape(RoundedRectangle(cornerRadius: 8))
                        }.buttonStyle(.plain)
                    }
                }
                .padding(11)
                .frame(maxWidth: 340)
                .background(card)
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(amber.opacity(0.5), lineWidth: 1))
                .clipShape(RoundedRectangle(cornerRadius: 10))
            }
            Button(action: { performSend() }) {
                Text(resolving ? "RESOLVING…" : (pending != nil ? "AWAITING CONFIRM" : "SEND"))
                    .font(.system(size: 13, weight: .heavy, design: .rounded))
                    .foregroundColor(bg)
                    .frame(maxWidth: .infinity).padding(.vertical, 10)
                    .background((resolving || pending != nil) ? dimText : amber)
                    .clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain).disabled(resolving || pending != nil)
            Button(action: {
                sendResult = String(cString: miner_sweep_unmixable())
            }) {
                Text("SWEEP UNMIXABLE")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(amber)
                    .frame(maxWidth: .infinity).padding(.vertical, 8)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain)
            Text("use if a send fails with \"not enough outputs\" \u{2014} consolidates mined coins so they can be spent")
                .font(.system(size: 8, design: .monospaced))
                .foregroundColor(dimText)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 330)
            Button(action: { showSend = false }) {
                Text("DONE")
                    .font(.system(size: 12, weight: .bold, design: .monospaced))
                    .foregroundColor(.white)
                    .frame(maxWidth: .infinity).padding(.vertical, 8)
                    .background(card).clipShape(RoundedRectangle(cornerRadius: 9))
            }.buttonStyle(.plain).keyboardShortcut(.cancelAction)
        }
        .padding(22)
        .frame(width: 380)
        .background(bg)
    }

    // Send to a typed address, or resolve a *.glac name first. The name service
    // maps name.glac -> a GLAC address; the wallet itself doesn't understand
    // names, so we resolve here and hand the wallet the resolved address.
    private func performSend() {
        let raw = sendAddr.trimmingCharacters(in: .whitespacesAndNewlines)
        let amt = Double(sendAmount) ?? 0
        if raw.lowercased().hasSuffix(".glac") {
            resolving = true
            sendResult = "resolving \(raw)…"
            resolveGlac(raw.lowercased()) { addr in
                DispatchQueue.main.async {
                    self.resolving = false
                    guard let addr = addr else {
                        self.sendResult = "couldn't resolve \(raw) — name not found"
                        return
                    }
                    // Don't broadcast yet -- stage it for an explicit confirm so
                    // the user sees the resolved address before any money moves.
                    self.sendResult = ""
                    self.pending = PendingGlacSend(address: addr, amount: amt, name: raw)
                }
            }
        } else {
            doSend(to: raw, amount: amt, name: nil)
        }
    }

    private func doSend(to address: String, amount: Double, name: String?) {
        let r = String(cString: miner_send(address, amount))
        // show the resolved name alongside the result for clarity
        sendResult = (r.hasPrefix("sent") && name != nil) ? "\(r)  → \(name!)" : r
        // clear on success so a second tap can't double-pay; keep on failure
        if r.hasPrefix("sent") { sendAddr = ""; sendAmount = "" }
    }

    // GET <resolver>/resolve/<name>.glac -> { "address": "R..." } | { "error" }.
    // Resolver base is RIME_GLAC_RESOLVER (defaults to the production host).
    private func resolveGlac(_ name: String, completion: @escaping (String?) -> Void) {
        let base = ProcessInfo.processInfo.environment["RIME_GLAC_RESOLVER"]
            ?? "https://names.glaciem.io"
        guard let url = URL(string: base + "/resolve/" + name) else { completion(nil); return }
        var req = URLRequest(url: url)
        req.timeoutInterval = 10
        URLSession.shared.dataTask(with: req) { data, _, _ in
            guard let data = data,
                  let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let addr = obj["address"] as? String, !addr.isEmpty else {
                completion(nil); return
            }
            completion(addr)
        }.resume()
    }
}

// MARK: latest hash ("watch it hash")
private struct HashPanel: View {
    let lastHash: String
    private var hashText: Text {
        let h = lastHash.isEmpty ? String(repeating: "-", count: 64) : lastHash
        let zeros = h.prefix(while: { $0 == "0" })
        let rest  = h.dropFirst(zeros.count)
        return (Text(String(zeros)).foregroundColor(amber)
              + Text(String(rest)).foregroundColor(.white.opacity(0.55)))
            .font(.system(size: 12.5, design: .monospaced))
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("LATEST HASH")
                .font(.system(size: 9, weight: .semibold, design: .monospaced))
                .foregroundColor(dimText).tracking(1)
            hashText
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(14)
        .background(card)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
}

// MARK: start / stop
private struct StartButton: View {
    let running: Bool
    let hasWallet: Bool
    var body: some View {
        let blocked = !running && !hasWallet
        return Button(action: {
            if blocked { return }
            running ? miner_stop() : miner_start()
        }) {
            Text(blocked ? "SET UP A WALLET IN HOST"
                         : (running ? "STOP" : "START MINING"))
                .font(.system(size: 15, weight: .heavy, design: .rounded))
                .foregroundColor(blocked ? dimText : (running ? .white : bg))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 14)
                .background(blocked ? card : (running ? Color.red.opacity(0.85) : amber))
                .clipShape(RoundedRectangle(cornerRadius: 13))
        }
        .buttonStyle(.plain)
        .disabled(blocked)
    }
}

private func fmtTime(_ s: Double) -> String {
    let t = Int(s)
    return String(format: "%02d:%02d:%02d", t/3600, (t/60)%60, t%60)
}

// the embedded wallet's cache file -- lives in the app's Application Support dir
private func walletPath() -> String {
    let fm = FileManager.default
    // Testing override: when RIME_WALLET_DIR is set, keep the wallet there
    // instead of the normal Application Support location. Lets a debug build
    // exercise generate/restore in an isolated dir without touching the real
    // mining wallet. Unset in normal/release use, so behavior is unchanged.
    if let override = ProcessInfo.processInfo.environment["RIME_WALLET_DIR"],
       !override.isEmpty {
        let odir = URL(fileURLWithPath: override, isDirectory: true)
        try? fm.createDirectory(at: odir, withIntermediateDirectories: true)
        return odir.appendingPathComponent("wallet").path
    }
    let dir = (fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
               ?? URL(fileURLWithPath: NSTemporaryDirectory()))
        .appendingPathComponent("RimeMiner", isDirectory: true)
    try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir.appendingPathComponent("wallet").path
}

// MARK: sparkline
struct Sparkline: View {
    let data: [Double]
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            let mx = max(data.max() ?? 1, 1)
            ZStack {
                if data.count > 1 {
                    let pts = data.enumerated().map { (i, v) in
                        CGPoint(x: w * CGFloat(i) / CGFloat(data.count - 1),
                                y: h - h * 0.92 * CGFloat(v / mx))
                    }
                    Path { p in
                        p.move(to: CGPoint(x: 0, y: h))
                        for pt in pts { p.addLine(to: pt) }
                        p.addLine(to: CGPoint(x: w, y: h))
                    }
                    .fill(LinearGradient(colors: [amber.opacity(0.28), .clear],
                                         startPoint: .top, endPoint: .bottom))
                    Path { p in
                        p.move(to: pts[0])
                        for pt in pts.dropFirst() { p.addLine(to: pt) }
                    }
                    .stroke(amber, style: StrokeStyle(lineWidth: 2, lineJoin: .round))
                }
            }
        }
        .background(card)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }
}

// render a string as a QR code image
private func qrImage(_ s: String) -> NSImage? {
    guard !s.isEmpty, let filter = CIFilter(name: "CIQRCodeGenerator") else { return nil }
    filter.setValue(Data(s.utf8), forKey: "inputMessage")
    filter.setValue("M", forKey: "inputCorrectionLevel")
    guard let out = filter.outputImage else { return nil }
    let scaled = out.transformed(by: CGAffineTransform(scaleX: 10, y: 10))
    guard let cg = CIContext().createCGImage(scaled, from: scaled.extent) else { return nil }
    return NSImage(cgImage: cg, size: NSSize(width: 220, height: 220))
}
