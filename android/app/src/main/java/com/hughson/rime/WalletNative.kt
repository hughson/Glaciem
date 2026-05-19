package com.hughson.rime

/**
 * JNI bridge to the embedded Rime wallet (librime_wallet.so -- Monero
 * wallet_api built from the Rime fork). A handle is an opaque Long.
 *
 * All calls are blocking; refresh() scans the chain, so call it off the UI
 * thread. A handle of 0L means "no wallet".
 */
object WalletNative {
    init { System.loadLibrary("rime_wallet") }

    /** Recover/open the testnet wallet for [seed] at [path], pointed at
     *  [daemon] (host:port). Returns a handle, or 0L on failure. */
    external fun recover(path: String, seed: String, daemon: String,
                         restoreHeight: Long): Long

    external fun close(handle: Long)
    external fun refresh(handle: Long): Boolean

    /** Persist the wallet cache to disk so the balance survives a relaunch. */
    external fun store(handle: Long)
    external fun connected(handle: Long): Boolean
    external fun isSynchronized(handle: Long): Boolean
    external fun balance(handle: Long): Long
    external fun unlockedBalance(handle: Long): Long
    external fun height(handle: Long): Long
    external fun daemonHeight(handle: Long): Long
    external fun address(handle: Long): String
    external fun send(handle: Long, address: String, amountAtomic: Long): String

    /** Sweep unmixable (mostly mined/coinbase) outputs into spendable ones. */
    external fun sweepUnmixable(handle: Long): String

    /** Recent non-coinbase transaction history, newest first, one tx per line. */
    external fun history(handle: Long): String
}
