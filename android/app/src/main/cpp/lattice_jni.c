/*
 * lattice_jni.c -- JNI bridge for the Rime Android miner.
 *
 * Mining core ported from pow/app/miner_core.m. The Lattice PoW reference
 * pow/lattice_ref.c is compiled as a separate unit (see cpp/CMakeLists.txt)
 * and linked here, so the Android arm64 build is bit-identical to daemon
 * consensus. Lattice is CPU-only.
 */
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

#include "keygen/rime_keygen.h"   /* rime_generate_address */

typedef uint64_t u64;
typedef uint8_t  u8;

/* epoch dataset size -- must match pow/lattice_ref.c */
#define DATASET_WORDS (4*1024*1024/8)

/* Lattice PoW (CPU-only) -- pow/lattice_ref.c, linked as a separate unit. */
extern void lattice_build_dataset(const u8 epoch_seed[32], u64 *ds);
extern void lattice_hash_ds(const u8 *in, size_t len, const u64 *ds, u8 out[32]);
extern void lattice_hash(const u8 *in, size_t len, const u8 epoch_seed[32], u8 out[32]);

/* The epoch dataset (4 MiB) depends only on the epoch seed, so it is built
   once per epoch and shared read-only by every worker thread. Rebuilt only
   via setEpochSeed, which the engine calls while no worker is hashing. */
static u64 *g_dataset = NULL;
static pthread_mutex_t g_ds_lock = PTHREAD_MUTEX_INITIALIZER;

/* leading zero bits of a little-endian 256-bit hash */
static int lz_bits(const u8 out[32]) {
  int n = 0;
  for (int i = 31; i >= 0; i--) {
    if (out[i] == 0) { n += 8; continue; }
    for (int b = 7; b >= 0; b--) { if (out[i] & (1 << b)) return n; n++; }
  }
  return n;
}

/* hash (32B, little-endian 256-bit) meets target iff hash*difficulty < 2^256 */
static int meets_target(const u8 h[32], uint64_t difficulty) {
  if (difficulty <= 1) return 1;
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

/* ---- JNI surface (Kotlin: object com.hughson.rime.MinerNative) ---- */

/* selfTest(): lattice_hash("abc", epoch_seed 00..1f) must equal the frozen
   "abc" vector -- proves the arm64 build == daemon consensus. */
JNIEXPORT jboolean JNICALL
Java_com_hughson_rime_MinerNative_selfTest(JNIEnv *env, jobject thiz) {
  (void)env; (void)thiz;
  u8 es[32]; for (int i = 0; i < 32; i++) es[i] = (u8)i;
  u8 out[32];
  lattice_hash((const u8 *)"abc", 3, es, out);
  static const u8 expect[32] = {
    0xf8,0xe1,0xd3,0x51,0xfe,0x34,0x70,0x48,0x73,0x58,0x0b,0x5a,0x42,0x70,0xc3,0x5a,
    0xad,0x81,0xb8,0x7b,0x18,0xa7,0x9a,0x19,0x16,0xed,0x38,0x07,0x57,0x0b,0x02,0x70 };
  return memcmp(out, expect, 32) == 0 ? JNI_TRUE : JNI_FALSE;
}

/* setEpochSeed(seed): (re)build the shared 4 MiB epoch dataset. */
JNIEXPORT void JNICALL
Java_com_hughson_rime_MinerNative_setEpochSeed(JNIEnv *env, jobject thiz,
                                               jbyteArray seed) {
  (void)thiz;
  u8 s[32] = {0};
  jsize n = (*env)->GetArrayLength(env, seed);
  if (n > 32) n = 32;
  if (n > 0) (*env)->GetByteArrayRegion(env, seed, 0, n, (jbyte *)s);
  pthread_mutex_lock(&g_ds_lock);
  if (!g_dataset) g_dataset = malloc(DATASET_WORDS * sizeof(u64));
  if (g_dataset) lattice_build_dataset(s, g_dataset);
  pthread_mutex_unlock(&g_ds_lock);
}

/* hash(): hash all `count` nonces starting at `startNonce`, each patched into
 * the 4-byte nonce field of `blob` at `nonceOffset`. Returns the absolute
 * nonce of the FIRST hash that meets `difficulty`, or -1 if none did. The
 * whole batch is always computed (it does not stop at the first winner) so
 * the caller's hashrate reflects real compute even on a difficulty-1 testnet.
 * outLastHash[32] receives the most recent hash; outBestBits[0] is updated
 * in place with the best leading-zero-bit count seen. Thread-safe: only reads
 * the shared epoch dataset, which must have been built via setEpochSeed. */
JNIEXPORT jlong JNICALL
Java_com_hughson_rime_MinerNative_hash(JNIEnv *env, jobject thiz,
    jbyteArray blob_, jint nonceOffset, jlong startNonce, jint count,
    jlong difficulty, jbyteArray outLastHash_, jintArray outBestBits_) {
  (void)thiz;
  jsize blen = (*env)->GetArrayLength(env, blob_);
  if (blen <= 0 || blen > 250 || nonceOffset < 0 || nonceOffset + 4 > blen)
    return -1;

  u8 blob[256];
  (*env)->GetByteArrayRegion(env, blob_, 0, blen, (jbyte *)blob);

  const u64 *ds = g_dataset;
  if (!ds) return -1;

  int best = 0;
  jint *bb = outBestBits_ ? (*env)->GetIntArrayElements(env, outBestBits_, NULL)
                          : NULL;
  if (bb) best = bb[0];

  jlong winner = -1;
  u8 lasth[32] = {0};
  for (jint k = 0; k < count; k++) {
    uint32_t nn = (uint32_t)(startNonce + k);
    blob[nonceOffset]     = (u8)nn;
    blob[nonceOffset + 1] = (u8)(nn >> 8);
    blob[nonceOffset + 2] = (u8)(nn >> 16);
    blob[nonceOffset + 3] = (u8)(nn >> 24);

    u8 h[32];
    lattice_hash_ds(blob, (size_t)blen, ds, h);

    int z = lz_bits(h); if (z > best) best = z;
    memcpy(lasth, h, 32);
    if (winner < 0 && meets_target(h, (uint64_t)difficulty))
      winner = startNonce + k;
  }

  if (bb) { bb[0] = best; (*env)->ReleaseIntArrayElements(env, outBestBits_, bb, 0); }
  if (outLastHash_) (*env)->SetByteArrayRegion(env, outLastHash_, 0, 32, (jbyte *)lasth);
  return winner;
}

/* v1.1.9: hashMulti -- like hash(), but collects EVERY nonce that meets
 * the target, not just the first. outWinners is a long[] the caller
 * provides; this writes up to outWinners.length winning nonces and
 * returns the count actually written. Closes the miner/pool hashrate
 * gap caused by under-reporting batches with 2+ winning nonces. */
JNIEXPORT jint JNICALL
Java_com_hughson_rime_MinerNative_hashMulti(JNIEnv *env, jobject thiz,
    jbyteArray blob_, jint nonceOffset, jlong startNonce, jint count,
    jlong difficulty, jbyteArray outLastHash_, jintArray outBestBits_,
    jlongArray outWinners_) {
  (void)thiz;
  jsize blen = (*env)->GetArrayLength(env, blob_);
  if (blen <= 0 || blen > 250 || nonceOffset < 0 || nonceOffset + 4 > blen)
    return 0;

  u8 blob[256];
  (*env)->GetByteArrayRegion(env, blob_, 0, blen, (jbyte *)blob);

  const u64 *ds = g_dataset;
  if (!ds) return 0;

  int best = 0;
  jint *bb = outBestBits_ ? (*env)->GetIntArrayElements(env, outBestBits_, NULL)
                          : NULL;
  if (bb) best = bb[0];

  jsize cap = outWinners_ ? (*env)->GetArrayLength(env, outWinners_) : 0;
  jlong *wbuf = outWinners_ ? (*env)->GetLongArrayElements(env, outWinners_, NULL)
                            : NULL;
  jint n_winners = 0;

  u8 lasth[32] = {0};
  for (jint k = 0; k < count; k++) {
    uint32_t nn = (uint32_t)(startNonce + k);
    blob[nonceOffset]     = (u8)nn;
    blob[nonceOffset + 1] = (u8)(nn >> 8);
    blob[nonceOffset + 2] = (u8)(nn >> 16);
    blob[nonceOffset + 3] = (u8)(nn >> 24);

    u8 h[32];
    lattice_hash_ds(blob, (size_t)blen, ds, h);

    int z = lz_bits(h); if (z > best) best = z;
    memcpy(lasth, h, 32);
    if (wbuf && n_winners < cap && meets_target(h, (uint64_t)difficulty)) {
      wbuf[n_winners++] = (jlong)(startNonce + k);
    }
  }

  if (bb) { bb[0] = best; (*env)->ReleaseIntArrayElements(env, outBestBits_, bb, 0); }
  if (wbuf) (*env)->ReleaseLongArrayElements(env, outWinners_, wbuf, 0);
  if (outLastHash_) (*env)->SetByteArrayRegion(env, outLastHash_, 0, 32, (jbyte *)lasth);
  return n_winners;
}

/* generateAddress(): generate a fresh Rime wallet via the keygen library
 * (Rime's own crypto). Returns String[2] = { testnet address, 25-word seed },
 * or null on failure. */
JNIEXPORT jobjectArray JNICALL
Java_com_hughson_rime_MinerNative_generateAddress(JNIEnv *env, jobject thiz) {
  (void)thiz;
  RimeKeypair k;
  if (!rime_generate_address(&k)) return NULL;
  jclass strCls = (*env)->FindClass(env, "java/lang/String");
  if (!strCls) return NULL;
  jobjectArray arr = (*env)->NewObjectArray(env, 2, strCls, NULL);
  if (!arr) return NULL;
  (*env)->SetObjectArrayElement(env, arr, 0, (*env)->NewStringUTF(env, k.address));
  (*env)->SetObjectArrayElement(env, arr, 1, (*env)->NewStringUTF(env, k.mnemonic));
  return arr;
}
