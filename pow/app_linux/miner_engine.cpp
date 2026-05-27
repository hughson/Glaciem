// miner_engine.cpp -- mining + wallet glue for the Linux Qt6 GUI.

#include "miner_engine.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <curl/curl.h>

extern "C" {
#include "rime_keygen.h"
#include "peer_cache.h"
// Lattice PoW (CPU-only) -- compiled as a separate unit (pow/lattice_ref.c).
extern void lattice_build_dataset(const uint8_t epoch_seed[32], uint64_t *ds);
extern void lattice_hash_ds(const uint8_t *in, size_t len, const uint64_t *ds,
                            uint8_t out[32]);
}

// epoch dataset size (must match pow/lattice_ref.c)
static constexpr int DATASET_WORDS = 4 * 1024 * 1024 / 8;
static constexpr int CHUNK = 64;  // nonces hashed per worker per batch

// --- small helpers ---------------------------------------------------------

namespace {

// HTTP POST to the Cloudflare RPC worker. Returns the response body or "".
QByteArray httpPost(const QString &url, const QByteArray &body) {
    CURL *c = curl_easy_init();
    if (!c) return {};
    QByteArray out;
    struct curl_slist *hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.toUtf8().constData());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.constData());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                     +[](char *p, size_t s, size_t n, void *u) -> size_t {
                         ((QByteArray *)u)->append(p, (int)(s * n));
                         return s * n;
                     });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? out : QByteArray{};
}

// JSON-RPC call -> result object (or empty on failure).
QJsonObject jsonRpc(const QString &nodeUrl, const QString &method,
                    const QJsonValue &params) {
    QJsonObject body{{"jsonrpc", "2.0"},
                     {"id", "0"},
                     {"method", method},
                     {"params", params.isUndefined() ? QJsonValue(QJsonObject{})
                                                     : params}};
    auto resp = httpPost(nodeUrl, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (resp.isEmpty()) return {};
    auto doc = QJsonDocument::fromJson(resp);
    if (!doc.isObject()) return {};
    return doc.object().value("result").toObject();
}

// Miner-RPC endpoints, tried in order. The Cloudflare Worker is primary
// (it absorbs the connection storm a difficulty-1 miner produces); the
// direct-node URLs are pure resilience fallbacks for when the Worker is
// unreachable (rate-limited, CF outage, etc.) so the network keeps mining.
static const QStringList kMinerEndpoints = {
    QStringLiteral("https://glaciem-rpc.frostmine.workers.dev/json_rpc"),
    QStringLiteral("http://static.197.125.225.46.clients.your-server.de:19081/json_rpc"),
    QStringLiteral("http://static.34.142.105.178.clients.your-server.de:19081/json_rpc"),
};

// --- peer cache: seeds + discovered peers, shared by miner + wallet --------

static PeerCache *g_peers = nullptr;
static QMutex g_peers_init_lock;

// Persist the JSON blob via QSettings (so we get Linux's standard config
// location automatically, ~/.config/Glaciem/GlaciemMiner.conf).
static int peers_load_cb(char *out, int cap, void *) {
    QSettings s("Glaciem", "GlaciemMiner");
    QString blob = s.value("peerCache").toString();
    if (blob.isEmpty()) return 0;
    QByteArray b = blob.toUtf8();
    int n = qMin(b.size(), cap - 1);
    memcpy(out, b.constData(), n);
    out[n] = 0;
    return 1;
}
static void peers_save_cb(const char *json, void *) {
    QSettings s("Glaciem", "GlaciemMiner");
    s.setValue("peerCache", QString::fromUtf8(json));
}
static void peers_init_once() {
    QMutexLocker lk(&g_peers_init_lock);
    if (g_peers) return;
    g_peers = peer_cache_new(peers_load_cb, peers_save_cb, nullptr);
    peer_cache_add_seed(g_peers, "glaciem-rpc.frostmine.workers.dev", 443, 1);
    peer_cache_add_seed(g_peers, "static.197.125.225.46.clients.your-server.de", 19081, 0);
    peer_cache_add_seed(g_peers, "static.34.142.105.178.clients.your-server.de", 19081, 0);
}

// Periodic peer discovery: hit /get_peer_list on the endpoint that just
// answered, look for peers advertising an RPC port, add them to the cache.
static void tryDiscoverPeers(const PeerEntry &source_peer) {
    if (!peer_cache_should_discover(g_peers)) return;
    QString url = QStringLiteral("%1://%2:%3/get_peer_list")
                      .arg(source_peer.use_ssl ? "https" : "http")
                      .arg(source_peer.host)
                      .arg(source_peer.port);
    QByteArray resp = httpPost(url, QByteArray());
    if (resp.isEmpty()) { peer_cache_reset_discovery_counter(g_peers); return; }
    auto doc = QJsonDocument::fromJson(resp);
    if (!doc.isObject()) { peer_cache_reset_discovery_counter(g_peers); return; }
    auto white = doc.object().value("white_list").toArray();
    int added = 0;
    for (const auto &v : white) {
        auto p = v.toObject();
        int rp = p.value("rpc_port").toInt();
        QString host = p.value("host").toString();
        if (rp <= 0 || rp > 65535 || host.isEmpty()) continue;
        peer_cache_add_discovered(g_peers, host.toUtf8().constData(), rp);
        if (++added >= 4) break;
    }
    peer_cache_reset_discovery_counter(g_peers);
}

QJsonObject jsonRpcAny(const QString &method, const QJsonValue &params) {
    peers_init_once();
    PeerEntry snap[64];
    int n = peer_cache_snapshot(g_peers, snap, 64);
    for (int i = 0; i < n; i++) {
        QString url = QStringLiteral("%1://%2:%3/json_rpc")
                          .arg(snap[i].use_ssl ? "https" : "http")
                          .arg(snap[i].host)
                          .arg(snap[i].port);
        qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        QJsonObject r = jsonRpc(url, method, params);
        int latency_ms = (int)(QDateTime::currentMSecsSinceEpoch() - t0);
        if (!r.isEmpty()) {
            peer_cache_mark_success(g_peers, snap[i].host, snap[i].port, latency_ms);
            // v1.1.4: tryDiscoverPeers() removed -- the public proxy
            // now 403s /get_peer_list, so this call was wasted. Peers
            // come from seeded endpoints only.
            return r;
        }
        peer_cache_mark_failure(g_peers, snap[i].host, snap[i].port);
    }
    return {};
}

QByteArray hex2bin(const QString &hex) {
    return QByteArray::fromHex(hex.toLatin1());
}
QString bin2hex(const QByteArray &b) { return QString::fromLatin1(b.toHex()); }

// --- v1.1.8: pool HTTP helpers ---------------------------------------------
//
// Pool endpoints take a flat JSON body (not the JSON-RPC envelope), so we
// have dedicated wrappers around httpPost. The pool returns flat JSON too.
// poolUrl is expected without a trailing slash (setPoolUrl trims it).

QJsonObject poolGetJob(const QString &poolUrl, const QString &wallet) {
    QString url = poolUrl + "/pool/job";
    QJsonObject body{{"wallet", wallet}};
    QByteArray resp = httpPost(url, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (resp.isEmpty()) return {};
    auto doc = QJsonDocument::fromJson(resp);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

QJsonObject poolSubmitShare(const QString &poolUrl, const QString &jobId,
                            const QString &wallet, quint32 nonce, bool isFullBlock) {
    QString url = poolUrl + "/pool/submit";
    QJsonObject body{
        {"job_id",    jobId},
        {"wallet",    wallet},
        {"nonce",     (qint64)nonce},
        {"full_block", isFullBlock},
    };
    QByteArray resp = httpPost(url, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (resp.isEmpty()) return {};
    auto doc = QJsonDocument::fromJson(resp);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

// nonce offset in a block blob: 3 leading varints + 32-byte prev_id.
int nonceOffset(const QByteArray &blob) {
    int p = 0;
    for (int v = 0; v < 3 && p < blob.size(); v++) {
        int n = 1;
        while (p + n - 1 < blob.size() && ((uint8_t)blob[p + n - 1] & 0x80))
            n++;
        p += n;
    }
    return p + 32;
}

// hash * difficulty < 2^256 ?
bool meetsTarget(const uint8_t h[32], uint64_t difficulty) {
    if (difficulty <= 1) return true;
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        uint64_t x = 0;
        for (int b = 0; b < 8; b++) x |= (uint64_t)h[i * 8 + b] << (8 * b);
        w[i] = x;
    }
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 p = (unsigned __int128)w[i] * difficulty + carry;
        carry = p >> 64;
    }
    return carry == 0;
}

int leadingZeros(const uint8_t h[32]) {
    int n = 0;
    for (int i = 31; i >= 0; i--) {
        if (h[i] == 0) { n += 8; continue; }
        for (int b = 7; b >= 0; b--) {
            if (h[i] & (1 << b)) return n;
            n++;
        }
    }
    return n;
}

double nowSec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

}  // namespace

// === WorkerThread: mining loop ============================================

class WorkerThread : public QThread {
public:
    WorkerThread(MinerEngine *e) : m_e(e) {}
    void requestStop() { m_stop = true; }

protected:
    void run() override;

private:
    MinerEngine *m_e;
    std::atomic<bool> m_stop{false};
};

void WorkerThread::run() {
    // v1.1.14+: re-read m_threadCount at the top of every batch (inside the
    // while loop below) so a settings change takes effect within ~1 batch
    // without restarting the miner. m_maxCores is the upper bound (set in
    // ctor from std::thread::hardware_concurrency, capped at 16).
    int cores = m_e->m_threadCount.load();
    if (cores < 1) cores = 1;
    if (cores > m_e->m_maxCores) cores = m_e->m_maxCores;

    std::vector<uint64_t> ds(DATASET_WORDS);
    uint8_t curSeed[32] = {0};
    bool haveDataset = false;

    double t0 = nowSec();
    double hr = 0;
    uint64_t total = 0, blocks = 0;
    int best = 0;
    // v1.1.10: nonce base is initialized ONCE and advances by cores*CHUNK
    // per batch. Previously it was re-seeded as `nowSec() * 131` on every
    // outer iteration; the pool re-issues the same job_id whenever the
    // upstream template hasn't refreshed, so consecutive batches with the
    // same job_id walked overlapping nonce ranges and the pool dedupe set
    // dropped 20%+ of submissions as duplicates -- showing up as a real
    // pool/miner hashrate gap. Monotonic base avoids that entirely.
    uint32_t base = (uint32_t)(nowSec() * 131.0);

    // v1.1.10: defensive refresh on consecutive invalid-share
    // rejections. The pool will ban a wallet after 25 in a row;
    // detecting it at 3 and forcing a dataset+template rebuild
    // self-corrects whatever drift (usually a missed epoch transition)
    // caused the mismatch.
    int consecutiveInvalid = 0;
    constexpr int LINUX_CONSECUTIVE_INVALID_THRESHOLD = 3;

    while (!m_stop) {
        // v1.1.14+: pick up Settings-dialog changes to the thread-count
        // picker without restarting the miner. Atomic load -- no lock.
        {
            int t = m_e->m_threadCount.load();
            if (t < 1) t = 1;
            if (t > m_e->m_maxCores) t = m_e->m_maxCores;
            cores = t;
        }
        if (consecutiveInvalid >= LINUX_CONSECUTIVE_INVALID_THRESHOLD) {
            // Drop cached dataset so the next iteration rebuilds it
            // against whatever seed_hash the fresh job carries.
            haveDataset = false;
            std::memset(curSeed, 0, sizeof(curSeed));
            consecutiveInvalid = 0;
        }
        // Mine to the embedded wallet's address; refuse to mine without one.
        QString addr;
        {
            QMutexLocker lk(&m_e->m_lock);
            addr = m_e->m_walletAddress;
        }
        if (addr.isEmpty()) {
            QMutexLocker lk(&m_e->m_lock);
            m_e->m_daemonConnected = false;
            m_e->m_hashrate = 0;
            msleep(500);
            continue;
        }

        // v1.1.8: branch on pool mode. Read poolEnabled/poolUrl under the
        // lock so a Settings dialog "Save" takes effect at the start of
        // the next batch (no app restart required).
        bool    poolModeNow;
        QString poolUrlNow;
        {
            QMutexLocker lk(&m_e->m_lock);
            poolModeNow = m_e->m_poolEnabled;
            poolUrlNow  = m_e->m_poolUrl;
        }
        QJsonObject tpl;
        QString jobId;
        if (poolModeNow) {
            tpl   = poolGetJob(poolUrlNow, addr);
            jobId = tpl.value("job_id").toString();
        } else {
            tpl = jsonRpcAny("get_block_template",
                             QJsonObject{{"wallet_address", addr},
                                         {"reserve_size", 8}});
        }
        QString hbHex = tpl.value("blockhashing_blob").toString();
        QString tbHex = tpl.value("blocktemplate_blob").toString();
        if (hbHex.isEmpty() || tbHex.isEmpty()) {
            QMutexLocker lk(&m_e->m_lock);
            m_e->m_daemonConnected = false;
            m_e->m_hashrate = 0;
            msleep(1000);
            continue;
        }
        QByteArray hb = hex2bin(hbHex);
        QByteArray tb = hex2bin(tbHex);
        QByteArray seedB = hex2bin(tpl.value("seed_hash").toString());
        uint64_t height = (uint64_t)tpl.value("height").toDouble();
        // In pool mode the daemon's "difficulty" is replaced by share_difficulty
        // (much easier); network_difficulty is reported separately so we can
        // detect shares that ALSO solve the actual block.
        uint64_t diff = poolModeNow
                          ? (uint64_t)tpl.value("share_difficulty").toDouble()
                          : (uint64_t)tpl.value("difficulty").toDouble();
        uint64_t netDiff = poolModeNow
                          ? (uint64_t)tpl.value("network_difficulty").toDouble()
                          : diff;
        if (hb.isEmpty() || hb.size() > 250) { msleep(200); continue; }
        int noff = nonceOffset(hb);
        if (noff + 4 > hb.size()) { msleep(200); continue; }

        uint8_t seed[32] = {0};
        if (seedB.size() == 32) std::memcpy(seed, seedB.constData(), 32);
        if (!haveDataset || std::memcmp(seed, curSeed, 32) != 0) {
            lattice_build_dataset(seed, ds.data());
            std::memcpy(curSeed, seed, 32);
            haveDataset = true;
        }

        // hash one batch -- N workers, CHUNK nonces each, parallel
        // v1.1.9: collect ALL winners (not just first). Honest miners
        // often produce 2-3 shares per batch at pool share-difficulty;
        // submitting all of them closes most of the miner/pool hashrate
        // gap and means more shares = more proportional payout.
        // v1.1.10: `base` is now hoisted (see top of run()) so it
        // advances monotonically across iterations and never re-uses
        // a nonce range against the same job.
        constexpr int WIN_MAX = 32;
        uint32_t winners[WIN_MAX] = {0};
        std::atomic<int> n_winners{0};
        std::atomic<int> bestBitsBatch{best};
        uint8_t lastHashBuf[32] = {0};
        QMutex lhMu;
        QMutex winMu;

        double tb_start = nowSec();
        std::vector<std::thread> workers;
        workers.reserve(cores);
        for (int t = 0; t < cores; t++) {
            workers.emplace_back([&, t]() {
                uint8_t blob[256];
                std::memcpy(blob, hb.constData(), hb.size());
                uint32_t start = base + (uint32_t)(t * CHUNK);
                for (int i = 0; i < CHUNK; i++) {
                    uint32_t nn = start + (uint32_t)i;
                    blob[noff] = (uint8_t)nn;
                    blob[noff + 1] = (uint8_t)(nn >> 8);
                    blob[noff + 2] = (uint8_t)(nn >> 16);
                    blob[noff + 3] = (uint8_t)(nn >> 24);
                    uint8_t h[32];
                    lattice_hash_ds(blob, hb.size(), ds.data(), h);
                    int z = leadingZeros(h);
                    int prev = bestBitsBatch.load();
                    while (z > prev && !bestBitsBatch.compare_exchange_weak(prev, z))
                        ;
                    if (meetsTarget(h, diff)) {
                        QMutexLocker lk(&winMu);
                        if (n_winners.load() < WIN_MAX) {
                            winners[n_winners.fetch_add(1)] = nn;
                        }
                    }
                    if (i == CHUNK - 1) {
                        QMutexLocker lk(&lhMu);
                        std::memcpy(lastHashBuf, h, 32);
                    }
                }
            });
        }
        for (auto &th : workers) th.join();

        int batch = cores * CHUNK;
        base += (uint32_t)batch;   // v1.1.10: advance for the next iteration
        double dt = nowSec() - tb_start;
        if (dt <= 0) dt = 1e-6;
        double inst = batch / dt;
        hr = (hr <= 0) ? inst : 0.8 * hr + 0.2 * inst;
        total += batch;
        if (bestBitsBatch > best) best = bestBitsBatch;

        char lhHex[65];
        for (int i = 0; i < 32; i++)
            std::snprintf(lhHex + i * 2, 3, "%02x", lastHashBuf[i]);

        {
            QMutexLocker lk(&m_e->m_lock);
            m_e->m_daemonConnected = true;
            m_e->m_hashrate = hr;
            m_e->m_height = height;
            m_e->m_difficulty = diff;
            m_e->m_bestBits = best;
            m_e->m_lastHash = QString::fromLatin1(lhHex, 64);
        }

        int nw = n_winners.load();
        if (nw > 0 && noff + 4 <= tb.size()) {
            if (poolModeNow) {
                // Submit every share. Re-hash each nonce against
                // network_difficulty so the pool gets the right full_block
                // flag (any one of them might also solve the block).
                for (int i = 0; i < nw; i++) {
                    uint32_t win = winners[i];
                    QByteArray hbCheck = hb;
                    hbCheck[noff]     = (char)win;
                    hbCheck[noff + 1] = (char)(win >> 8);
                    hbCheck[noff + 2] = (char)(win >> 16);
                    hbCheck[noff + 3] = (char)(win >> 24);
                    uint8_t mh[32];
                    lattice_hash_ds((const uint8_t*)hbCheck.constData(),
                                    hbCheck.size(), ds.data(), mh);
                    bool isFull = (netDiff > 0) && meetsTarget(mh, netDiff);
                    auto sr = poolSubmitShare(poolUrlNow, jobId, addr,
                                              (quint32)win, isFull);
                    bool isBlock    = sr.value("block").toBool();
                    bool isAccepted = sr.value("accepted").toBool();
                    QString reason  = sr.value("reason").toString();
                    if (isBlock) {
                        blocks++;
                        QMutexLocker lk(&m_e->m_lock);
                        m_e->m_blocksFound = blocks;
                    }
                    // v1.1.10: self-heal counter -- accepted clears it,
                    // "invalid share" rejections accumulate toward the
                    // force-refresh threshold at the top of the loop.
                    if (isAccepted || isBlock) {
                        consecutiveInvalid = 0;
                    } else if (reason == QStringLiteral("invalid share")) {
                        consecutiveInvalid++;
                    }
                }
            } else {
                // Solo: any winning nonce solves the block; take the first.
                int win = winners[0];
                tb[noff]     = (char)win;
                tb[noff + 1] = (char)(win >> 8);
                tb[noff + 2] = (char)(win >> 16);
                tb[noff + 3] = (char)(win >> 24);
                auto sr = jsonRpcAny("submit_block",
                                     QJsonArray{QString::fromLatin1(tb.toHex())});
                if (sr.value("status").toString() == "OK") {
                    blocks++;
                    QMutexLocker lk(&m_e->m_lock);
                    m_e->m_blocksFound = blocks;
                }
            }
        }

        (void)t0;  // uptime not surfaced yet
    }

    QMutexLocker lk(&m_e->m_lock);
    m_e->m_hashrate = 0;
}

// === WalletPollThread: open + refresh wallet ===============================

class WalletPollThread : public QThread {
public:
    WalletPollThread(MinerEngine *e) : m_e(e) {}
    void requestStop() { m_stop = true; }
    // signal that a pending generate/restore needs to be picked up
    void wake() {}  // (the loop polls every 200ms for pending state)

protected:
    void run() override;

private:
    MinerEngine *m_e;
    std::atomic<bool> m_stop{false};
    int m_idle = 0;
};

static constexpr int kWalletFailoverThreshold = 3;

void WalletPollThread::run() {
    peers_init_once();
    int disconnectCount = 0;
    int endpointIdx = 0;
    while (!m_stop) {
        RimeWallet *w;
        {
            QMutexLocker lk(&m_e->m_lock);
            w = m_e->m_wallet;
        }

        // -- drain any pending wallet op (Send / Sweep / History) --
        MinerEngine::WalletReq req = MinerEngine::ReqNone;
        QString sendAddr;
        double sendAmt = 0.0;
        {
            QMutexLocker lk(&m_e->m_lock);
            req = m_e->m_pendingReq;
            sendAddr = m_e->m_pendingSendAddr;
            sendAmt = m_e->m_pendingSendAmount;
            m_e->m_pendingReq = MinerEngine::ReqNone;
        }
        if (req == MinerEngine::ReqSend) {
            char buf[400] = {0};
            if (!w) {
                std::snprintf(buf, sizeof buf, "Wallet not ready yet");
            } else if (sendAddr.isEmpty() || sendAmt <= 0.0) {
                std::snprintf(buf, sizeof buf,
                              "Enter a recipient address and an amount above 0");
            } else {
                unsigned long long atomic =
                    (unsigned long long)(sendAmt * 1e12 + 0.5);
                rime_wallet_send(w, sendAddr.toUtf8().constData(), atomic, buf,
                                 sizeof buf);
            }
            QMetaObject::invokeMethod(m_e, "sendResult", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromUtf8(buf)));
        } else if (req == MinerEngine::ReqSweep) {
            char buf[400] = {0};
            if (!w) {
                std::snprintf(buf, sizeof buf, "Wallet not ready yet");
            } else {
                rime_wallet_sweep_unmixable(w, buf, sizeof buf);
            }
            QMetaObject::invokeMethod(m_e, "sweepResult", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromUtf8(buf)));
        } else if (req == MinerEngine::ReqHistory) {
            char buf[8192] = {0};
            if (!w) {
                std::snprintf(buf, sizeof buf, "Wallet not ready yet");
            } else {
                rime_wallet_history(w, buf, sizeof buf);
            }
            QMetaObject::invokeMethod(m_e, "historyResult", Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromUtf8(buf)));
        }

        if (w) {
            // publish cached address + balance first (instant, no chain scan)
            char addr[160] = {0};
            rime_wallet_address(w, addr, sizeof addr);
            {
                QMutexLocker lk(&m_e->m_lock);
                m_e->m_walletAddress = QString::fromLatin1(addr);
                m_e->m_balance = rime_wallet_balance(w);
                m_e->m_unlockedBalance = rime_wallet_unlocked_balance(w);
            }

            rime_wallet_refresh(w);  // blocking chain scan
            int conn = rime_wallet_connected(w);
            int synced = rime_wallet_synchronized(w);
            uint64_t bal = rime_wallet_balance(w);
            uint64_t unl = rime_wallet_unlocked_balance(w);
            uint64_t wht = rime_wallet_height(w);
            uint64_t tgt = rime_wallet_daemon_height(w);
            // stranded ahead of the daemon -> rescan
            if (conn && tgt > 0 && wht > tgt) {
                rime_wallet_rescan(w);
                synced = rime_wallet_synchronized(w);
                bal = rime_wallet_balance(w);
                unl = rime_wallet_unlocked_balance(w);
                wht = rime_wallet_height(w);
                tgt = rime_wallet_daemon_height(w);
            }
            rime_wallet_store(w);
            {
                QMutexLocker lk(&m_e->m_lock);
                m_e->m_walletConnected = conn != 0;
                m_e->m_balance = bal;
                m_e->m_unlockedBalance = unl;
                m_e->m_walletHeight = wht;
                m_e->m_targetHeight = tgt;
                m_e->m_walletSyncing = (conn && !synced);
            }

            // Failover: after N consecutive disconnects, snapshot the peer
            // cache (seeds + discovered, in score order) and rotate to the
            // next entry. Wallet keys/balance/height survive the swap.
            if (conn) {
                disconnectCount = 0;
            } else if (++disconnectCount >= kWalletFailoverThreshold) {
                PeerEntry snap[64];
                int n = peer_cache_snapshot(g_peers, snap, 64);
                if (n > 0) {
                    endpointIdx = (endpointIdx + 1) % n;
                    QString daemon = QStringLiteral("%1:%2")
                                         .arg(snap[endpointIdx].host)
                                         .arg(snap[endpointIdx].port);
                    rime_wallet_set_daemon(w, daemon.toUtf8().constData());
                }
                disconnectCount = 0;
            }
        } else {
            QMutexLocker lk(&m_e->m_lock);
            m_e->m_walletConnected = false;
        }
        // v1.1.4: ~20s between refreshes (was 4s). Block time is ~120s
        // so a 20s refresh keeps the UI live-feeling while cutting
        // /getblocks.bin traffic to the public RPC proxy ~5x.
        for (int i = 0; i < 200 && !m_stop; i++) msleep(100);
    }
}

// === MinerEngine ===========================================================

MinerEngine::MinerEngine(QObject *parent) : QObject(parent) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // v1.1.14+: detect physical cores once so the QML spinner can clamp + so
    // the default thread count is sensible before loadSettings overrides it.
    {
        int hw = (int)std::thread::hardware_concurrency();
        if (hw < 1) hw = 1;
        if (hw > 16) hw = 16;     // matches WorkerThread::run's old hard cap
        m_maxCores = hw;
        m_threadCount.store((m_maxCores + 1) / 2);
    }

    loadSettings();

    m_poll = new WalletPollThread(this);
    m_poll->start();

    // re-open any existing wallet file on disk
    if (QFile::exists(walletFilePath() + ".keys")) {
        QMutexLocker lk(&m_lock);
        m_wallet = rime_wallet_recover(
            walletFilePath().toUtf8().constData(), "",
            QStringLiteral("https://%1:%2").arg(m_nodeHost).arg(m_nodePort)
                .toUtf8()
                .constData(),
            0);
    }

    connect(&m_tick, &QTimer::timeout, this, &MinerEngine::tick);
    m_tick.start(200);  // 5 Hz UI refresh
}

MinerEngine::~MinerEngine() {
    stop();
    if (m_poll) {
        m_poll->requestStop();
        m_poll->wait(2000);
        delete m_poll;
    }
    if (m_wallet) rime_wallet_close(m_wallet);
    curl_global_cleanup();
}

bool MinerEngine::mining() const { return m_worker && m_worker->isRunning(); }

#define GETTER_LOCKED(name, type, member) \
    type MinerEngine::name() const { QMutexLocker lk(&m_lock); return member; }

GETTER_LOCKED(daemonConnected, bool, m_daemonConnected)
GETTER_LOCKED(hashrate, double, m_hashrate)
GETTER_LOCKED(height, quint64, m_height)
GETTER_LOCKED(difficulty, quint64, m_difficulty)
GETTER_LOCKED(blocksFound, quint64, m_blocksFound)
GETTER_LOCKED(bestBits, int, m_bestBits)
GETTER_LOCKED(lastHash, QString, m_lastHash)
GETTER_LOCKED(walletConnected, bool, m_walletConnected)
GETTER_LOCKED(walletSyncing, bool, m_walletSyncing)
GETTER_LOCKED(balance, quint64, m_balance)
GETTER_LOCKED(unlockedBalance, quint64, m_unlockedBalance)
GETTER_LOCKED(walletHeight, quint64, m_walletHeight)
GETTER_LOCKED(targetHeight, quint64, m_targetHeight)
GETTER_LOCKED(walletAddress, QString, m_walletAddress)
GETTER_LOCKED(nodeHost, QString, m_nodeHost)

int MinerEngine::nodePort() const { QMutexLocker lk(&m_lock); return m_nodePort; }
bool MinerEngine::hasWallet() const {
    QMutexLocker lk(&m_lock);
    return !m_walletAddress.isEmpty();
}

QString MinerEngine::device() const {
    return QStringLiteral("CPU - %1 threads").arg(m_maxCores);
}

int MinerEngine::threadCount() const { return m_threadCount.load(); }
int MinerEngine::maxCores()    const { return m_maxCores; }

void MinerEngine::setThreadCount(int n) {
    if (n < 1) n = 1;
    if (n > m_maxCores) n = m_maxCores;
    if (m_threadCount.exchange(n) == n) return;
    saveSettings();
    emit threadCountChanged();
}

void MinerEngine::setNodeHost(const QString &host) {
    {
        QMutexLocker lk(&m_lock);
        if (m_nodeHost == host) return;
        m_nodeHost = host;
    }
    saveSettings();
    emit nodeChanged();
}

void MinerEngine::setNodePort(int port) {
    {
        QMutexLocker lk(&m_lock);
        if (m_nodePort == port) return;
        m_nodePort = port;
    }
    saveSettings();
    emit nodeChanged();
}

// v1.1.8: pool config accessors + setters. Worker thread re-reads
// m_poolEnabled / m_poolUrl at the start of each batch under m_lock,
// so a Settings dialog "Save" takes effect within ~1 second.
bool MinerEngine::poolEnabled() const {
    QMutexLocker lk(&m_lock);
    return m_poolEnabled;
}
QString MinerEngine::poolUrl() const {
    QMutexLocker lk(&m_lock);
    return m_poolUrl;
}
void MinerEngine::setPoolEnabled(bool on) {
    {
        QMutexLocker lk(&m_lock);
        if (m_poolEnabled == on) return;
        m_poolEnabled = on;
    }
    saveSettings();
    emit poolChanged();
}
void MinerEngine::setPoolUrl(const QString &url) {
    QString trimmed = url.trimmed();
    while (trimmed.endsWith('/')) trimmed.chop(1);
    {
        QMutexLocker lk(&m_lock);
        if (m_poolUrl == trimmed) return;
        m_poolUrl = trimmed;
    }
    saveSettings();
    emit poolChanged();
}

void MinerEngine::start() {
    if (m_worker && m_worker->isRunning()) return;
    if (m_worker) { delete m_worker; m_worker = nullptr; }
    m_worker = new WorkerThread(this);
    m_worker->start();
    emit statsChanged();
}

void MinerEngine::stop() {
    if (!m_worker) return;
    m_worker->requestStop();
    m_worker->wait(8000);
    delete m_worker;
    m_worker = nullptr;
    emit statsChanged();
}

void MinerEngine::toggleMining() { mining() ? stop() : start(); }

void MinerEngine::generateWallet() {
    RimeKeypair k;
    if (!rime_generate_address(&k)) return;
    m_pendingAddr = QString::fromLatin1(k.address);
    m_pendingSeed = QString::fromLatin1(k.mnemonic);
    emit generatedWallet(m_pendingAddr, m_pendingSeed);
}

void MinerEngine::useGeneratedWallet() {
    if (m_pendingSeed.isEmpty()) return;
    QString seed = m_pendingSeed;
    m_pendingAddr.clear();
    m_pendingSeed.clear();
    openWallet(seed);
}

void MinerEngine::discardGeneratedWallet() {
    m_pendingAddr.clear();
    m_pendingSeed.clear();
}

void MinerEngine::restoreWallet(const QString &seed) {
    openWallet(seed.trimmed());
}

void MinerEngine::openWallet(const QString &seed) {
    // Tear down the existing wallet, delete its files, recover/open with the
    // new seed. Done on the main thread; the poll thread picks it up next tick.
    QString path = walletFilePath();
    QString daemon =
        QStringLiteral("https://%1:%2").arg(m_nodeHost).arg(m_nodePort);

    RimeWallet *old;
    {
        QMutexLocker lk(&m_lock);
        old = m_wallet;
        m_wallet = nullptr;
        m_walletAddress.clear();
        m_balance = 0;
        m_unlockedBalance = 0;
        m_walletConnected = false;
    }
    if (old) rime_wallet_close(old);
    if (!seed.isEmpty()) {
        QFile::remove(path);
        QFile::remove(path + ".keys");
        QFile::remove(path + ".address.txt");
    }
    RimeWallet *w = rime_wallet_recover(
        path.toUtf8().constData(),
        seed.toUtf8().constData(),
        daemon.toUtf8().constData(), 0);
    {
        QMutexLocker lk(&m_lock);
        m_wallet = w;
    }
    emit statsChanged();
}

void MinerEngine::requestSend(const QString &address, double amountGlac) {
    QMutexLocker lk(&m_lock);
    m_pendingReq = ReqSend;
    m_pendingSendAddr = address.trimmed();
    m_pendingSendAmount = amountGlac;
}

void MinerEngine::requestSweep() {
    QMutexLocker lk(&m_lock);
    m_pendingReq = ReqSweep;
}

void MinerEngine::requestHistory() {
    QMutexLocker lk(&m_lock);
    m_pendingReq = ReqHistory;
}

void MinerEngine::copyAddressToClipboard() {
    QString addr = walletAddress();
    if (addr.isEmpty()) return;
    auto *cb = QGuiApplication::clipboard();
    if (cb) cb->setText(addr);
}

QString MinerEngine::pasteFromClipboard() const {
    auto *cb = QGuiApplication::clipboard();
    if (!cb) return {};
    // Trim leading/trailing whitespace -- Glaciem addresses are 95
    // chars of base58 and any newline a clipboard manager added
    // would otherwise make wallet2 reject the input.
    return cb->text().trimmed();
}

QString MinerEngine::walletFilePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.glaciem";
    QDir().mkpath(dir);
    return dir + "/glaciem-wallet";
}

void MinerEngine::loadSettings() {
    QSettings s("Glaciem", "GlaciemMiner");
    m_nodeHost = s.value("nodeHost", "glaciem-rpc.frostmine.workers.dev").toString();
    m_nodePort = s.value("nodePort", 443).toInt();
    m_poolEnabled = s.value("poolEnabled", false).toBool();
    m_poolUrl     = s.value("poolUrl",
        QStringLiteral("https://glaciem-pool.frostmine.workers.dev")).toString();
    // v1.1.14+: thread count picker. Default to the recommended value (half
    // the cores) set in the ctor. Clamp aggressively in case prefs were
    // written from a build with a different m_maxCores.
    {
        int n = s.value("threadCount", (m_maxCores + 1) / 2).toInt();
        if (n < 1) n = 1;
        if (n > m_maxCores) n = m_maxCores;
        m_threadCount.store(n);
    }
}

void MinerEngine::saveSettings() {
    QSettings s("Glaciem", "GlaciemMiner");
    s.setValue("nodeHost", m_nodeHost);
    s.setValue("nodePort", m_nodePort);
    s.setValue("poolEnabled", m_poolEnabled);
    s.setValue("poolUrl",     m_poolUrl);
    s.setValue("threadCount", m_threadCount.load());
}

void MinerEngine::tick() { emit statsChanged(); }
