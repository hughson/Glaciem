/*
 * rime_keygen.cpp -- generate a fresh Glaciem mainnet wallet.
 *
 * Mirrors cryptonote::account_base::generate() exactly, using Rime's real
 * crypto: a random spend keypair, then a view keypair derived deterministically
 * from keccak(spend_secret). The 25-word seed encodes the spend secret key, so
 * it restores the identical wallet in rime-wallet-cli.
 */
#include "rime_keygen.h"

#include <string>
#include <cstring>

#include "crypto/crypto.h"
#include "common/base58.h"
#include "mnemonics/electrum-words.h"
#include "wipeable_string.h"

extern "C" {
#include "crypto/keccak.h"
}

/* Glaciem mainnet public-address prefix -- config (mainnet) in
   cryptonote_config.h. Encodes to base58 addresses beginning with 'R'. */
#define RIME_ADDRESS_PREFIX 144

int rime_generate_address(RimeKeypair *out)
{
  if (!out) return 0;
  std::memset(out, 0, sizeof(*out));

  try {
    crypto::public_key spend_pub, view_pub;
    crypto::secret_key spend_sec, view_sec, second;

    /* spend keypair: a fresh random secret key, reduced mod l */
    crypto::generate_keys(spend_pub, spend_sec);

    /* view keypair: deterministic, second seed = keccak(spend_secret).
       recover=true so the seed alone restores the whole wallet. */
    keccak((const uint8_t *)&spend_sec, sizeof(crypto::secret_key),
           (uint8_t *)&second, sizeof(crypto::secret_key));
    crypto::generate_keys(view_pub, view_sec, second, true);

    /* address = base58( prefix , spend_pub || view_pub || keccak-checksum ).
       encode_addr appends the 4-byte checksum; this is byte-identical to
       cryptonote::get_account_address_as_str for a standard address. */
    std::string blob;
    blob.append((const char *)&spend_pub, sizeof(spend_pub));
    blob.append((const char *)&view_pub, sizeof(view_pub));
    std::string addr = tools::base58::encode_addr(RIME_ADDRESS_PREFIX, blob);

    /* 25-word Electrum seed encodes the spend secret key */
    epee::wipeable_string words;
    if (!crypto::ElectrumWords::bytes_to_words(spend_sec, words, "English"))
      return 0;

    if (addr.size() >= sizeof(out->address)) return 0;
    if (words.size() >= sizeof(out->mnemonic)) return 0;
    std::memcpy(out->address, addr.data(), addr.size());
    std::memcpy(out->mnemonic, words.data(), words.size());
    return 1;
  } catch (...) {
    return 0;
  }
}
