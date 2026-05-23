/*
 * pool_verify.c -- server-side Lattice share verification for the
 *                  Glaciem mining pool.
 *
 * Compile as a shared library on the pool VM (Linux x86-64) and call
 * via Python ctypes from pool-server.py. This closes the v1 trust-mode
 * gap: every submitted share is re-hashed and rejected if it doesn't
 * actually meet share_difficulty.
 *
 * Build (on glaciem-node):
 *   gcc -shared -fPIC -O3 -funroll-loops \
 *       -I/root/glaciem/pow \
 *       -o libpool_verify.so \
 *       pool_verify.c /root/glaciem/pow/lattice_ref.c
 *
 * Lattice keeps a 4 MiB epoch dataset keyed by the block template's
 * seed_hash. We expose pool_build_dataset() so Python can build one
 * lazily per seed it sees, and pool_free_dataset() when an old seed
 * rotates out. Datasets cost ~1-2s to build; verification itself is
 * a single hash (~microseconds per share).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* From pow/lattice_ref.c (linked as a sibling translation unit). */
extern void lattice_build_dataset(const uint8_t epoch_seed[32], uint64_t *ds);
extern void lattice_hash_ds(const uint8_t *input, size_t len,
                            const uint64_t *ds, uint8_t out[32]);

/* Same constant the miners use. Keep in sync with lattice_ref.c. */
#define DATASET_WORDS (4 * 1024 * 1024 / 8)
#define DATASET_BYTES (DATASET_WORDS * sizeof(uint64_t))

/* Build a new dataset for the given seed. Caller takes ownership;
 * release with pool_free_dataset(). Returns NULL on allocation
 * failure. */
uint64_t *pool_build_dataset(const uint8_t seed[32]) {
    uint64_t *ds = (uint64_t *)malloc(DATASET_BYTES);
    if (!ds) return NULL;
    lattice_build_dataset(seed, ds);
    return ds;
}

void pool_free_dataset(uint64_t *ds) {
    if (ds) free(ds);
}

/* Cryptonote-standard "hash meets difficulty" check.
 *
 *   target = 2^256 / difficulty
 *   share is valid iff hash <= target
 *   ⇔ hash * difficulty <= 2^256
 *
 * The hash is 32 bytes little-endian; only the top 64 bits (= the
 * last 8 bytes of the buffer) need to be looked at for a difficulty
 * that fits in 64 bits (which ours always does -- network diff is
 * ~700k right now and pool share diff is ~700). The hi-64 of
 * (top * difficulty) being non-zero means we wrapped past 2^64 in
 * the top word, which means hash * difficulty > 2^256 -- share
 * fails. Wrapping not happening = share meets target. */
static int meets_target(const uint8_t hash[32], uint64_t difficulty) {
    if (difficulty == 0) return 0;
    uint64_t top = 0;
    for (int i = 0; i < 8; i++) {
        top |= (uint64_t)hash[24 + i] << (i * 8);
    }
    __uint128_t prod = (__uint128_t)top * (__uint128_t)difficulty;
    /* Overflowed past 2^64 in the upper word = hash too big = FAILS.
     * Stayed within 64 bits = hash small enough = MEETS. */
    return ((uint64_t)(prod >> 64) == 0) ? 1 : 0;
}

/* Verify a submitted share.
 *
 *   blob          the blockhashing_blob the pool issued
 *   blob_len      its length in bytes
 *   nonce_offset  where to splice the nonce in the blob
 *   nonce         the value the miner found
 *   ds            a dataset built for this template's seed_hash
 *   difficulty    the pool's share_difficulty
 *
 * Returns:
 *    1 = hash meets the difficulty target (valid share)
 *    0 = hash does NOT meet (invalid; reject)
 *   -1 = bad arguments (malformed call) */
int pool_verify_share(const uint8_t *blob, int blob_len, int nonce_offset,
                      uint32_t nonce, const uint64_t *ds, uint64_t difficulty) {
    if (!blob || !ds) return -1;
    if (blob_len <= 0 || blob_len > 256) return -1;
    if (nonce_offset < 0 || nonce_offset + 4 > blob_len) return -1;

    uint8_t buf[256];
    memcpy(buf, blob, blob_len);
    buf[nonce_offset]     = (uint8_t)(nonce);
    buf[nonce_offset + 1] = (uint8_t)(nonce >> 8);
    buf[nonce_offset + 2] = (uint8_t)(nonce >> 16);
    buf[nonce_offset + 3] = (uint8_t)(nonce >> 24);

    uint8_t hash[32];
    lattice_hash_ds(buf, blob_len, ds, hash);
    return meets_target(hash, difficulty);
}

/* Like pool_verify_share, but also writes the resulting 32-byte hash to
 * `out_hash` so the caller can log it on failure for diagnostics.
 * v1.1.10: lets the pool log "share didn't meet target -- here's the
 * hash that came out, and the target it was checked against" so an
 * operator can tell the difference between (a) honest miner producing
 * the wrong hash from a stale dataset (hash is "real-looking" garbage
 * with no leading zeros), (b) actual cheating (hash is random / way
 * above target), or (c) a meets_target / endian / blob-mangling bug
 * (hash is suspiciously close to target). */
int pool_verify_share_v2(const uint8_t *blob, int blob_len, int nonce_offset,
                         uint32_t nonce, const uint64_t *ds, uint64_t difficulty,
                         uint8_t out_hash[32]) {
    if (!blob || !ds || !out_hash) return -1;
    if (blob_len <= 0 || blob_len > 256) return -1;
    if (nonce_offset < 0 || nonce_offset + 4 > blob_len) return -1;

    uint8_t buf[256];
    memcpy(buf, blob, blob_len);
    buf[nonce_offset]     = (uint8_t)(nonce);
    buf[nonce_offset + 1] = (uint8_t)(nonce >> 8);
    buf[nonce_offset + 2] = (uint8_t)(nonce >> 16);
    buf[nonce_offset + 3] = (uint8_t)(nonce >> 24);

    lattice_hash_ds(buf, blob_len, ds, out_hash);
    return meets_target(out_hash, difficulty);
}
