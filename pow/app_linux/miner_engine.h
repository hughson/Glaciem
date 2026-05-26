// miner_engine.h -- QObject backend for the Linux Qt6 GUI.
//
// Wraps the same wallet C ABI (rime_wallet) and Lattice PoW (lattice_ref.c)
// the Mac/Win/Android apps use, exposes state as Q_PROPERTYs so the QML UI
// can bind to balance / hashrate / sync / address etc., and surfaces start /
// stop / generate / restore / settings as Q_INVOKABLE slots.
//
// The mining loop runs on its own QThread; the wallet poll runs on another.
// Stats are updated under QMutex and a 5 Hz QTimer on the main thread fires
// statsChanged() so QML bindings refresh.

#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <atomic>

extern "C" {
#include "rime_wallet.h"
}

class MinerEngine; // forward
class WorkerThread; // mining loop
class WalletPollThread; // wallet refresh loop

class MinerEngine : public QObject {
    Q_OBJECT

    // --- mining stats ---
    Q_PROPERTY(bool mining READ mining NOTIFY statsChanged)
    Q_PROPERTY(bool daemonConnected READ daemonConnected NOTIFY statsChanged)
    Q_PROPERTY(double hashrate READ hashrate NOTIFY statsChanged)
    Q_PROPERTY(quint64 height READ height NOTIFY statsChanged)
    Q_PROPERTY(quint64 difficulty READ difficulty NOTIFY statsChanged)
    Q_PROPERTY(quint64 blocksFound READ blocksFound NOTIFY statsChanged)
    Q_PROPERTY(int bestBits READ bestBits NOTIFY statsChanged)
    Q_PROPERTY(QString lastHash READ lastHash NOTIFY statsChanged)
    Q_PROPERTY(QString device READ device CONSTANT)

    // --- wallet state ---
    Q_PROPERTY(bool walletConnected READ walletConnected NOTIFY statsChanged)
    Q_PROPERTY(bool walletSyncing READ walletSyncing NOTIFY statsChanged)
    Q_PROPERTY(quint64 balance READ balance NOTIFY statsChanged)
    Q_PROPERTY(quint64 unlockedBalance READ unlockedBalance NOTIFY statsChanged)
    Q_PROPERTY(quint64 walletHeight READ walletHeight NOTIFY statsChanged)
    Q_PROPERTY(quint64 targetHeight READ targetHeight NOTIFY statsChanged)
    Q_PROPERTY(QString walletAddress READ walletAddress NOTIFY statsChanged)
    Q_PROPERTY(bool hasWallet READ hasWallet NOTIFY statsChanged)

    // --- settings ---
    Q_PROPERTY(QString nodeHost READ nodeHost WRITE setNodeHost NOTIFY nodeChanged)
    Q_PROPERTY(int nodePort READ nodePort WRITE setNodePort NOTIFY nodeChanged)
    // v1.1.8: pool mode. When enabled, the mining loop fetches jobs from
    // {poolUrl}/pool/job and submits shares to /pool/submit instead of
    // talking to a daemon directly. Block rewards go to the pool wallet;
    // miners are credited proportionally to share contribution.
    Q_PROPERTY(bool    poolEnabled READ poolEnabled WRITE setPoolEnabled NOTIFY poolChanged)
    Q_PROPERTY(QString poolUrl     READ poolUrl     WRITE setPoolUrl     NOTIFY poolChanged)
    // v1.1.14+: thread-count picker. WorkerThread reads m_threadCount at the
    // top of every batch so a change takes effect within a few seconds. Range
    // is 1..maxCores; maxCores is exposed as a constant so QML can clamp.
    Q_PROPERTY(int threadCount READ threadCount WRITE setThreadCount NOTIFY threadCountChanged)
    Q_PROPERTY(int maxCores    READ maxCores    CONSTANT)

public:
    explicit MinerEngine(QObject *parent = nullptr);
    ~MinerEngine() override;

    // property getters (thread-safe via internal mutex)
    bool mining() const;
    bool daemonConnected() const;
    double hashrate() const;
    quint64 height() const;
    quint64 difficulty() const;
    quint64 blocksFound() const;
    int bestBits() const;
    QString lastHash() const;
    QString device() const;

    bool walletConnected() const;
    bool walletSyncing() const;
    quint64 balance() const;
    quint64 unlockedBalance() const;
    quint64 walletHeight() const;
    quint64 targetHeight() const;
    QString walletAddress() const;
    bool hasWallet() const;

    QString nodeHost() const;
    int nodePort() const;
    void setNodeHost(const QString &host);
    void setNodePort(int port);

    bool    poolEnabled() const;
    QString poolUrl() const;
    void    setPoolEnabled(bool on);
    void    setPoolUrl(const QString &url);

    int  threadCount() const;
    int  maxCores() const;
    void setThreadCount(int n);

public slots:
    // --- mining control ---
    void start();
    void stop();
    void toggleMining();

    // --- wallet ---
    // Generate a fresh wallet; the address and 25-word mnemonic are returned
    // via the generatedWallet(addr, seed) signal so QML can show the seed
    // dialog. Caller can then call useGeneratedWallet() to commit, or discard.
    void generateWallet();
    void useGeneratedWallet();        // adopt the just-generated wallet
    void discardGeneratedWallet();    // throw it away

    // Restore an existing wallet from a 25-word seed (space-separated).
    void restoreWallet(const QString &seed);

    void copyAddressToClipboard();    // utility for the UI

    // --- wallet ops (Send / Sweep / History) ---
    // wallet_api is single-threaded -- these enqueue a request that the poll
    // thread picks up next iteration, then emits the matching *Result signal.
    // Use the signals (not return values) from QML.
    Q_INVOKABLE void requestSend(const QString &address, double amountGlac);
    Q_INVOKABLE void requestSweep();
    Q_INVOKABLE void requestHistory();

    // --- settings persistence ---
    void loadSettings();
    void saveSettings();

signals:
    void statsChanged();
    void nodeChanged();
    void poolChanged();
    void threadCountChanged();
    // address + 25-word seed of a freshly generated wallet, waiting for the
    // user to confirm before it becomes the app's embedded wallet
    void generatedWallet(QString address, QString seed);

    // wallet op results (emitted from the poll thread via queued connection)
    void sendResult(QString line);
    void sweepResult(QString line);
    void historyResult(QString text);

private slots:
    void tick();   // 5 Hz UI refresh

private:
    friend class WorkerThread;
    friend class WalletPollThread;

    void openWallet(const QString &seed);  // (re)open wallet on the poll thread
    QString walletFilePath() const;

    // -- mutable state guarded by m_lock --
    mutable QMutex m_lock;

    bool m_daemonConnected = false;
    double m_hashrate = 0.0;
    quint64 m_height = 0, m_difficulty = 0, m_blocksFound = 0;
    int m_bestBits = 0;
    QString m_lastHash;

    bool m_walletConnected = false, m_walletSyncing = false;
    quint64 m_balance = 0, m_unlockedBalance = 0;
    quint64 m_walletHeight = 0, m_targetHeight = 0;
    QString m_walletAddress;

    QString m_nodeHost;
    int m_nodePort = 0;
    // v1.1.8: pool mode state
    bool    m_poolEnabled = false;
    QString m_poolUrl = QStringLiteral("https://glaciem-pool.frostmine.workers.dev");
    // v1.1.14+: thread count picker. Atomic so WorkerThread can read it
    // without holding m_lock at the top of every batch.
    std::atomic<int> m_threadCount{0};   // 0 = "not yet set, use recommended"
    int              m_maxCores = 1;     // populated in ctor from hardware_concurrency

    // pending generated wallet awaiting user confirm
    QString m_pendingAddr, m_pendingSeed;

    // request queue (drained by the poll thread)
    enum WalletReq { ReqNone, ReqSend, ReqSweep, ReqHistory };
    WalletReq m_pendingReq = ReqNone;
    QString m_pendingSendAddr;
    double m_pendingSendAmount = 0.0;

    // -- threads + handles --
    WorkerThread *m_worker = nullptr;
    WalletPollThread *m_poll = nullptr;
    QTimer m_tick;

    // wallet handle is owned by the poll thread; this pointer is shared by
    // pointer-value-only across threads (set/cleared under m_lock).
    RimeWallet *m_wallet = nullptr;
};
