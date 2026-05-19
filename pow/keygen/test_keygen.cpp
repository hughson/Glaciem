/* Standalone keygen check: generate a wallet, print address + seed,
   sanity-check the address shape. Not shipped in any app. */
#include "rime_keygen.h"
#include <cstdio>
#include <cstring>

int main(void)
{
  RimeKeypair k;
  if (!rime_generate_address(&k)) {
    std::fprintf(stderr, "rime_generate_address FAILED\n");
    return 1;
  }
  std::printf("address : %s\n", k.address);
  std::printf("length  : %zu\n", std::strlen(k.address));
  std::printf("mnemonic: %s\n", k.mnemonic);

  int words = k.mnemonic[0] ? 1 : 0;
  for (const char *p = k.mnemonic; *p; ++p) if (*p == ' ') words++;
  std::printf("words   : %d\n", words);

  int ok = 1;
  if (k.address[0] != 'A') { std::fprintf(stderr, "FAIL: address does not start with 'A'\n"); ok = 0; }
  size_t L = std::strlen(k.address);
  if (L < 90 || L > 100)   { std::fprintf(stderr, "FAIL: address length %zu out of range\n", L); ok = 0; }
  if (words != 25)         { std::fprintf(stderr, "FAIL: expected 25 seed words, got %d\n", words); ok = 0; }
  std::printf(ok ? "OK\n" : "CHECK FAILED\n");
  return ok ? 0 : 1;
}
