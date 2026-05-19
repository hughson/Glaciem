/*
 * Minimal stand-in for libsodium's <sodium/crypto_verify_32.h>.
 *
 * Rime's crypto/generic-ops.h #includes this header to declare the
 * constant-time 32-byte compare used by public-key (in)equality operators.
 * The keygen library never instantiates those operators, so crypto_verify_32
 * is declared but never called or linked -- verified: `nm` shows no undefined
 * crypto_verify symbol in librimekeygen.a. Vendoring this declaration lets
 * keygen build for macOS / Android / Windows with no libsodium dependency.
 */
#ifndef crypto_verify_32_H
#define crypto_verify_32_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define crypto_verify_32_BYTES 32U

size_t crypto_verify_32_bytes(void);

int crypto_verify_32(const unsigned char *x, const unsigned char *y);

#ifdef __cplusplus
}
#endif

#endif
