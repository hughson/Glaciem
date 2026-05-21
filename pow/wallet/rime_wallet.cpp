/*
 * rime_wallet.cpp -- C ABI over Rime's real wallet (Monero wallet_api,
 * src/wallet/api). Backed by libwallet_api built from the Rime fork.
 *
 * Every entry point catches all exceptions: wallet2 throws C++ exceptions, and
 * a C ABI must never let one escape across the boundary (it would terminate
 * the process).
 */
#include "rime_wallet.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "wallet/api/wallet2_api.h"

#ifdef __ANDROID__
#include <android/log.h>
#define TLOG(...) __android_log_print(ANDROID_LOG_INFO, "RimeWallet", __VA_ARGS__)
#else
#define TLOG(...) ((void)0)
#endif

using namespace Monero;

struct RimeWallet {
  WalletManager *wm = nullptr;
  Wallet        *w  = nullptr;
};

static void copy_out(const std::string &s, char *out, int cap) {
  if (!out || cap <= 0) return;
  int n = (int)s.size();
  if (n > cap - 1) n = cap - 1;
  if (n > 0) std::memcpy(out, s.data(), (size_t)n);
  out[n] = 0;
}

/* common tail of recover/open: verify status, connect to the daemon, wrap */
static RimeWallet *finish_open(WalletManager *wm, Wallet *w,
                                 const char *daemon_address) {
  if (!w) return nullptr;
  {
    int st = 0; std::string err;
    w->statusWithErrorString(st, err);
    TLOG("wallet opened: status=%d err='%s'", st, err.c_str());
  }
  if (w->status() != Wallet::Status_Ok) { wm->closeWallet(w, false); return nullptr; }
  if (daemon_address && daemon_address[0]) {
    // doInit() inside init() flags a LAN daemon as trusted (no PoW checks).
    // No startRefresh(): the app drives refresh() on its own thread so every
    // wallet call crosses this try/catch'd boundary.
    // explicit std::string -> picks the daemon init overload, not the static
    // logging init Wallet::init(const char*, const char*, ...)
    bool ok = w->init(std::string(daemon_address), (uint64_t)0);
    int st = 0; std::string err;
    w->statusWithErrorString(st, err);
    TLOG("init('%s') -> %d  status=%d err='%s'", daemon_address, (int)ok, st, err.c_str());
  }
  RimeWallet *tw = new (std::nothrow) RimeWallet();
  if (!tw) { wm->closeWallet(w, false); return nullptr; }
  tw->wm = wm;
  tw->w  = w;
  return tw;
}

extern "C" {

RimeWallet *rime_wallet_recover(const char *path, const char *seed,
                                    const char *daemon_address,
                                    unsigned long long restore_height) {
  try {
    if (!path || !seed) return nullptr;
    WalletManager *wm = WalletManagerFactory::getWalletManager();
    if (!wm) return nullptr;
    Wallet *w = wm->walletExists(path)
                  ? wm->openWallet(path, "", MAINNET)
                  : wm->recoveryWallet(path, "", seed, MAINNET,
                                       (uint64_t)restore_height);
    return finish_open(wm, w, daemon_address);
  } catch (...) { return nullptr; }
}

RimeWallet *rime_wallet_open(const char *path, const char *password,
                                 const char *daemon_address) {
  try {
    if (!path) return nullptr;
    WalletManager *wm = WalletManagerFactory::getWalletManager();
    if (!wm) return nullptr;
    Wallet *w = wm->openWallet(path, password ? password : "", MAINNET);
    return finish_open(wm, w, daemon_address);
  } catch (...) { return nullptr; }
}

void rime_wallet_close(RimeWallet *tw) {
  try {
    if (!tw) return;
    if (tw->wm && tw->w) tw->wm->closeWallet(tw->w, true);
    delete tw;
  } catch (...) {}
}

/* Re-point an open wallet at a new daemon (failover). Wallet::init() is the
   only swap path wallet_api exposes -- calling it again post-construction
   re-runs wallet2::set_daemon() which closes the existing connection and
   sets the new address. Keys, balance, scanned-height, and history are
   preserved; only the HTTP connection state is swapped. */
int rime_wallet_set_daemon(RimeWallet *tw, const char *daemon_address) {
  try {
    if (!tw || !tw->w || !daemon_address || !daemon_address[0]) return 0;
    bool ok = tw->w->init(std::string(daemon_address), (uint64_t)0);
    int st = 0; std::string err;
    tw->w->statusWithErrorString(st, err);
    TLOG("set_daemon('%s') -> %d  status=%d err='%s'", daemon_address,
         (int)ok, st, err.c_str());
    return ok ? 1 : 0;
  } catch (...) { return 0; }
}

int rime_wallet_refresh(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    bool r = tw->w->refresh();
    TLOG("refresh ok=%d  walletH=%llu daemonH=%llu bal=%llu sync=%d conn=%d",
         (int)r,
         (unsigned long long)tw->w->blockChainHeight(),
         (unsigned long long)tw->w->daemonBlockChainHeight(),
         (unsigned long long)tw->w->balance(0),
         (int)tw->w->synchronized(),
         (int)tw->w->connected());
    return r ? 1 : 0;
  } catch (const std::exception &e) {
    TLOG("refresh exception: %s", e.what());
    return 0;
  } catch (...) {
    TLOG("refresh exception: unknown");
    return 0;
  }
}

int rime_wallet_rescan(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    bool r = tw->w->rescanBlockchain();
    TLOG("rescanBlockchain ok=%d  walletH=%llu daemonH=%llu",
         (int)r,
         (unsigned long long)tw->w->blockChainHeight(),
         (unsigned long long)tw->w->daemonBlockChainHeight());
    return r ? 1 : 0;
  } catch (const std::exception &e) {
    TLOG("rescan exception: %s", e.what());
    return 0;
  } catch (...) {
    TLOG("rescan exception: unknown");
    return 0;
  }
}

void rime_wallet_store(RimeWallet *tw) {
  try {
    if (tw && tw->w) tw->w->store("");   // "" -> save to the wallet's own file
  } catch (...) {}
}

int rime_wallet_connected(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    return tw->w->connected() == Wallet::ConnectionStatus_Connected ? 1 : 0;
  } catch (...) { return 0; }
}

int rime_wallet_synchronized(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    return tw->w->synchronized() ? 1 : 0;
  } catch (...) { return 0; }
}

unsigned long long rime_wallet_balance(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    return (unsigned long long)tw->w->balance(0);
  } catch (...) { return 0; }
}

unsigned long long rime_wallet_unlocked_balance(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    return (unsigned long long)tw->w->unlockedBalance(0);
  } catch (...) { return 0; }
}

unsigned long long rime_wallet_height(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    return (unsigned long long)tw->w->blockChainHeight();
  } catch (...) { return 0; }
}

unsigned long long rime_wallet_daemon_height(RimeWallet *tw) {
  try {
    if (!tw || !tw->w) return 0;
    return (unsigned long long)tw->w->daemonBlockChainHeight();
  } catch (...) { return 0; }
}

void rime_wallet_address(RimeWallet *tw, char *out, int cap) {
  if (out && cap > 0) out[0] = 0;
  try {
    if (tw && tw->w) copy_out(tw->w->address(0, 0), out, cap);
  } catch (...) {}
}

void rime_wallet_seed(RimeWallet *tw, char *out, int cap) {
  if (out && cap > 0) out[0] = 0;
  try {
    if (tw && tw->w) copy_out(tw->w->seed(""), out, cap);
  } catch (...) {}
}

int rime_wallet_send(RimeWallet *tw, const char *address,
                       unsigned long long amount_atomic,
                       char *result, int result_cap) {
  if (result && result_cap > 0) result[0] = 0;
  try {
    if (!tw || !tw->w)           { copy_out("wallet not open", result, result_cap); return 0; }
    if (!address || !address[0]) { copy_out("enter a recipient address", result, result_cap); return 0; }
    if (amount_atomic == 0)      { copy_out("enter an amount greater than 0", result, result_cap); return 0; }

    PendingTransaction *pt = tw->w->createTransaction(
        address, "", optional<uint64_t>((uint64_t)amount_atomic), 0,
        PendingTransaction::Priority_Low, 0, std::set<uint32_t>());
    if (!pt) { copy_out("could not create transaction", result, result_cap); return 0; }

    int ok = 0;
    if (pt->status() != PendingTransaction::Status_Ok) {
      copy_out("send failed: " + pt->errorString(), result, result_cap);
    } else {
      // read amount/fee BEFORE commit() -- commit() clears the pending-tx
      // data, so reading them afterwards yields 0.
      uint64_t amt = pt->amount(), fee = pt->fee();
      if (!pt->commit()) {
        copy_out("broadcast failed: " + pt->errorString(), result, result_cap);
      } else {
        char line[256];
        std::snprintf(line, sizeof line, "sent %.6f RME  (fee %.6f)",
                      (double)amt / 1e12, (double)fee / 1e12);
        copy_out(line, result, result_cap);
        ok = 1;
      }
    }
    tw->w->disposeTransaction(pt);
    return ok;
  } catch (...) {
    copy_out("send failed (internal error)", result, result_cap);
    return 0;
  }
}

int rime_wallet_sweep_unmixable(RimeWallet *tw, char *result, int result_cap) {
  if (result && result_cap > 0) result[0] = 0;
  try {
    if (!tw || !tw->w) { copy_out("wallet not open", result, result_cap); return 0; }

    PendingTransaction *pt = tw->w->createSweepUnmixableTransaction();
    if (!pt) { copy_out("could not create sweep transaction", result, result_cap); return 0; }

    int ok = 0;
    if (pt->status() != PendingTransaction::Status_Ok) {
      copy_out("sweep failed: " + pt->errorString(), result, result_cap);
    } else if (pt->amount() == 0 && pt->fee() == 0) {
      copy_out("nothing to sweep -- no unmixable outputs", result, result_cap);
    } else {
      // read amount/fee BEFORE commit() -- commit() clears the pending-tx
      // data, so reading them afterwards yields 0.
      uint64_t amt = pt->amount(), fee = pt->fee();
      if (!pt->commit()) {
        copy_out("broadcast failed: " + pt->errorString(), result, result_cap);
      } else {
        char line[256];
        std::snprintf(line, sizeof line,
                      "swept %.6f RME  (fee %.6f) -- mined coins are now spendable",
                      (double)amt / 1e12, (double)fee / 1e12);
        copy_out(line, result, result_cap);
        ok = 1;
      }
    }
    tw->w->disposeTransaction(pt);
    return ok;
  } catch (...) {
    copy_out("sweep failed (internal error)", result, result_cap);
    return 0;
  }
}

void rime_wallet_history(RimeWallet *tw, char *out, int cap) {
  if (out && cap > 0) out[0] = 0;
  try {
    if (!tw || !tw->w) return;
    TransactionHistory *h = tw->w->history();
    if (!h) return;
    h->refresh();
    std::vector<TransactionInfo*> all = h->getAll();
    std::string s;
    int shown = 0;
    // getAll() is oldest-first -- walk in reverse for newest-first
    for (auto it = all.rbegin(); it != all.rend() && shown < 50; ++it) {
      TransactionInfo *t = *it;
      if (!t || t->isCoinbase()) continue;          // skip mining rewards
      bool is_out = (t->direction() == TransactionInfo::Direction_Out);
      const char *status = t->isFailed()  ? "failed"
                         : t->isPending() ? "pending"
                         :                  "confirmed";
      char date[24] = "";
      std::time_t ts = t->timestamp();
      if (ts > 0) {
        std::tm tmv;
#ifdef _WIN32
        bool ok = (localtime_s(&tmv, &ts) == 0);   // MSVC/mingw: no POSIX localtime_r
#else
        bool ok = (localtime_r(&ts, &tmv) != nullptr);
#endif
        if (ok) std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tmv);
      }
      char line[200];
      std::snprintf(line, sizeof line, "%s  %s%.6f RME  %s  %s\n",
                    is_out ? "out" : "in ",
                    is_out ? "-" : "+",
                    (double)t->amount() / 1e12,
                    status, date);
      s += line;
      ++shown;
    }
    if (s.empty()) s = "no sends or receives yet";
    copy_out(s, out, cap);
  } catch (...) {}
}

}  /* extern "C" */
