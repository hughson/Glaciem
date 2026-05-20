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

QByteArray hex2bin(const QString &hex) {
    return QByteArray::fromHex(hex.toLatin1());
}
QString bin2hex(const QByteArray &b) { return QString::fromLatin1(b.toHex()); }

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
    int cores = (int)std::thread::hardware_concurrency();
    if (cores < 1) cores = 1;
    if (cores > 16) cores = 16;

    std::vector<uint64_t> ds(DATASET_WORDS);
    uint8_t curSeed[32] = {0};
    bool haveDataset = false;

    double t0 = nowSec();
    double hr = 0;
    uint64_t total = 0, blocks = 0;
    int best = 0;

    QString nodeUrl;
    {
        QMutexLocker lk(&m_e->m_lock);
        nodeUrl = QStringLiteral("https://%1:%2/json_rpc")
                      .arg(m_e->m_nodeHost)
                      .arg(m_e->m_nodePort);
    }

    while (!m_stop) {
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

        // get_block_template
        auto tpl = jsonRpc(nodeUrl, "get_block_template",
                           QJsonObject{{"wallet_address", addr},
                                       {"reserve_size", 8}});
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
        uint64_t diff = (uint64_t)tpl.value("difficulty").toDouble();
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
        uint32_t base = (uint32_t)(nowSec() * 131.0);
        std::atomic<int> winner{-1};
        std::atomic<int> bestBitsBatch{best};
        uint8_t lastHashBuf[32] = {0};
        QMutex lhMu;

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
                    if (winner.load() < 0 && meetsTarget(h, diff)) {
                        int expected = -1;
                        winner.compare_exchange_strong(expected, (int)nn);
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

        int win = winner.load();
        if (win >= 0 && noff + 4 <= tb.size()) {
            tb[noff] = (char)win;
            tb[noff + 1] = (char)(win >> 8);
            tb[noff + 2] = (char)(win >> 16);
            tb[noff + 3] = (char)(win >> 24);
            auto sr = jsonRpc(nodeUrl, "submit_block",
                              QJsonArray{QString::fromLatin1(tb.toHex())});
            if (sr.value("status").toString() == "OK") {
                blocks++;
                QMutexLocker lk(&m_e->m_lock);
                m_e->m_blocksFound = blocks;
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

void WalletPollThread::run() {
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
        } else {
            QMutexLocker lk(&m_e->m_lock);
            m_e->m_walletConnected = false;
        }
        // ~4s between refreshes
        for (int i = 0; i < 40 && !m_stop; i++) msleep(100);
    }
}

// === MinerEngine ===========================================================

MinerEngine::MinerEngine(QObject *parent) : QObject(parent) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

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
    int n = (int)std::thread::hardware_concurrency();
    return QStringLiteral("CPU - %1 threads").arg(n);
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
}

void MinerEngine::saveSettings() {
    QSettings s("Glaciem", "GlaciemMiner");
    s.setValue("nodeHost", m_nodeHost);
    s.setValue("nodePort", m_nodePort);
}

void MinerEngine::tick() { emit statsChanged(); }
