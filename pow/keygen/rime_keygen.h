/*
 * rime_keygen.h -- one C entry point to generate a fresh Rime wallet.
 *
 * Built from Rime's own crypto (src/crypto, src/mnemonics, src/common/base58)
 * -- no hand-rolled key or address crypto. Shared by the macOS, Android and
 * Windows miner apps.
 */
#ifndef RIME_KEYGEN_H
#define RIME_KEYGEN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char address[160];   /* base58 testnet address, NUL-terminated ('A'..., ~95 chars) */
  char mnemonic[600];  /* 25-word Electrum seed, single-space separated, NUL-terminated */
} RimeKeypair;

/* Generate a fresh Rime testnet wallet.
   Returns 1 on success (out->address and out->mnemonic filled), 0 on failure. */
int rime_generate_address(RimeKeypair *out);

#ifdef __cplusplus
}
#endif

#endif
