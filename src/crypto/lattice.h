/* lattice.h -- Lattice proof-of-work, CPU-only, used for v18+ block hashing.
 * Implementation: lattice.c (synced from pow/lattice_ref.c -- KEEP IN SYNC). */
#ifndef LATTICE_H
#define LATTICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute the Lattice hash (CPU-only, integer-only, deterministic).
 *   input       block hashing blob
 *   len         its length in bytes
 *   epoch_seed  32 bytes keying the dataset (the seed-block hash)
 *   out         receives the 32-byte PoW hash
 */
void lattice_hash(const uint8_t *input, size_t len,
                  const uint8_t epoch_seed[32], uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif
