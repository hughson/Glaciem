/*
 * wallet_jni.cpp -- JNI bridge for the Rime embedded wallet.
 *
 * Thin glue over pow/wallet/rime_wallet.h (the C ABI around Monero's
 * wallet_api). Kotlin object: com.hughson.rime.WalletNative.
 * A handle is the RimeWallet* returned as a jlong.
 */
#include <jni.h>
#include <cstdint>
#include "rime_wallet.h"

static inline RimeWallet *H(jlong h) { return (RimeWallet *)(uintptr_t)h; }

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_hughson_rime_WalletNative_recover(JNIEnv *env, jobject thiz,
    jstring path, jstring seed, jstring daemon, jlong restoreHeight) {
  (void)thiz;
  const char *p = env->GetStringUTFChars(path, nullptr);
  const char *s = env->GetStringUTFChars(seed, nullptr);
  const char *d = env->GetStringUTFChars(daemon, nullptr);
  RimeWallet *w = rime_wallet_recover(p, s, d, (unsigned long long)restoreHeight);
  env->ReleaseStringUTFChars(path, p);
  env->ReleaseStringUTFChars(seed, s);
  env->ReleaseStringUTFChars(daemon, d);
  return (jlong)(uintptr_t)w;
}

JNIEXPORT void JNICALL
Java_com_hughson_rime_WalletNative_close(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  rime_wallet_close(H(h));
}

JNIEXPORT jboolean JNICALL
Java_com_hughson_rime_WalletNative_refresh(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return rime_wallet_refresh(H(h)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_hughson_rime_WalletNative_store(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  rime_wallet_store(H(h));
}

JNIEXPORT jboolean JNICALL
Java_com_hughson_rime_WalletNative_connected(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return rime_wallet_connected(H(h)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_hughson_rime_WalletNative_isSynchronized(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return rime_wallet_synchronized(H(h)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_hughson_rime_WalletNative_balance(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return (jlong)rime_wallet_balance(H(h));
}

JNIEXPORT jlong JNICALL
Java_com_hughson_rime_WalletNative_unlockedBalance(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return (jlong)rime_wallet_unlocked_balance(H(h));
}

JNIEXPORT jlong JNICALL
Java_com_hughson_rime_WalletNative_height(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return (jlong)rime_wallet_height(H(h));
}

JNIEXPORT jlong JNICALL
Java_com_hughson_rime_WalletNative_daemonHeight(JNIEnv *env, jobject thiz, jlong h) {
  (void)env; (void)thiz;
  return (jlong)rime_wallet_daemon_height(H(h));
}

JNIEXPORT jstring JNICALL
Java_com_hughson_rime_WalletNative_address(JNIEnv *env, jobject thiz, jlong h) {
  (void)thiz;
  char buf[256]; buf[0] = 0;
  rime_wallet_address(H(h), buf, sizeof buf);
  return env->NewStringUTF(buf);
}

JNIEXPORT jstring JNICALL
Java_com_hughson_rime_WalletNative_send(JNIEnv *env, jobject thiz,
    jlong h, jstring address, jlong amountAtomic) {
  (void)thiz;
  const char *a = env->GetStringUTFChars(address, nullptr);
  char res[512]; res[0] = 0;
  rime_wallet_send(H(h), a, (unsigned long long)amountAtomic, res, sizeof res);
  env->ReleaseStringUTFChars(address, a);
  return env->NewStringUTF(res);
}

JNIEXPORT jstring JNICALL
Java_com_hughson_rime_WalletNative_sweepUnmixable(JNIEnv *env, jobject thiz,
    jlong h) {
  (void)thiz;
  char res[512]; res[0] = 0;
  rime_wallet_sweep_unmixable(H(h), res, sizeof res);
  return env->NewStringUTF(res);
}

JNIEXPORT jstring JNICALL
Java_com_hughson_rime_WalletNative_history(JNIEnv *env, jobject thiz, jlong h) {
  (void)thiz;
  char res[8192]; res[0] = 0;
  rime_wallet_history(H(h), res, sizeof res);
  return env->NewStringUTF(res);
}

}  /* extern "C" */
