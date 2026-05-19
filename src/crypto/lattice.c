/* lattice.c -- Lattice, embedded in the daemon for v18+ block hashing
 * and verification. Algorithm-only copy of the canonical reference
 * pow/lattice_ref.c -- KEEP THE TWO IN SYNC.
 *
 * v2: CPU-only. v1's X-phase promoted to the whole pipeline -- one long,
 * latency-bound, branchy walk over a per-nonce scratchpad. No Y-phase, no
 * GPU code. The sole primitive (arx_perm, the BLAKE2b round function) and
 * the sponge are unchanged from v1. Provisional parameters, not yet
 * cryptographically reviewed. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint8_t  u8;

/* ---- parameters (v2; provisional, pending review) ---- */
#define ARX_ROUNDS    8
#define RATE_WORDS    8                 /* sponge rate; capacity = 16-8 = 8 */
#define BLOCK_WORDS   16                /* mixing block = 128 bytes         */
#define ST_WORDS      256               /* working state = 2 KiB / nonce    */
#define SCRATCH_WORDS (1024*1024/8)     /* per-nonce scratchpad = 1 MiB      */
#define DATASET_WORDS (4*1024*1024/8)   /* epoch-global dataset = 4 MiB      */
#define L_WALK        32768             /* walk iterations (the work)        */

static inline u64 rotr64(u64 x, int n){ return (x >> n) | (x << (64 - n)); }
static inline void store64le(u8 *p, u64 v){ for(int i=0;i<8;i++) p[i]=(u8)(v>>(8*i)); }

/* BLAKE2b G mixing function */
#define G(a,b,c,d) do{ \
  a=a+b; d=rotr64(d^a,32); c=c+d; b=rotr64(b^c,24); \
  a=a+b; d=rotr64(d^a,16); c=c+d; b=rotr64(b^c,63); }while(0)

/* arx_perm: the sole primitive -- BLAKE2b column+diagonal mixing, ARX_ROUNDS. */
static void arx_perm(u64 v[16]){
  for(int r=0;r<ARX_ROUNDS;r++){
    G(v[0],v[4],v[ 8],v[12]); G(v[1],v[5],v[ 9],v[13]);
    G(v[2],v[6],v[10],v[14]); G(v[3],v[7],v[11],v[15]);
    G(v[0],v[5],v[10],v[15]); G(v[1],v[6],v[11],v[12]);
    G(v[2],v[7],v[ 8],v[13]); G(v[3],v[4],v[ 9],v[14]);
  }
}

/* Sponge hash over arx_perm: absorb in[inwords], squeeze out[outwords]. */
static void sponge(const u64 *in, int inwords, u64 *out, int outwords, u64 domain){
  u64 s[16]; memset(s,0,sizeof s);
  s[15] ^= domain ^ ((u64)inwords << 8);
  int off=0;
  while(off + RATE_WORDS <= inwords){
    for(int i=0;i<RATE_WORDS;i++) s[i] ^= in[off+i];
    arx_perm(s); off += RATE_WORDS;
  }
  u64 last[RATE_WORDS]; memset(last,0,sizeof last);
  for(int i=0; off+i<inwords; i++) last[i]=in[off+i];
  last[inwords-off] ^= 0x01;
  last[RATE_WORDS-1] ^= 0x8000000000000000ULL;
  for(int i=0;i<RATE_WORDS;i++) s[i] ^= last[i];
  arx_perm(s);
  int got=0;
  while(got<outwords){
    for(int i=0;i<RATE_WORDS && got<outwords;i++) out[got++]=s[i];
    if(got<outwords) arx_perm(s);
  }
}

/* Fill buf[n] deterministically from an 8-word seed (arx_perm keystream). */
static void expand(const u64 seed[8], u64 domain, u64 *buf, size_t n){
  u64 s[16]; memset(s,0,sizeof s);
  for(int i=0;i<8;i++) s[i]=seed[i];
  s[8] ^= domain;
  for(size_t j=0;j<n;j+=RATE_WORDS){
    arx_perm(s);
    for(int i=0;i<RATE_WORDS && j+i<n;i++) buf[j+i]=s[i];
  }
}

/* The walk: a latency-bound, branchy traversal of a large per-nonce scratchpad. */
static void lattice_walk(u64 *state, u64 *scratch, const u64 *ds){
  u64 acc[16];
  sponge(state, ST_WORDS, acc, 16, 0x58 /*walk*/);
  expand(acc, 0x58, scratch, SCRATCH_WORDS);
  const size_t sblk  = SCRATCH_WORDS/BLOCK_WORDS;
  const size_t dblk  = DATASET_WORDS/BLOCK_WORDS;
  const size_t stblk = ST_WORDS/BLOCK_WORDS;
  for(int it=0; it<L_WALK; it++){
    size_t si = (size_t)(acc[0] % sblk)  * BLOCK_WORDS;
    size_t di = (size_t)(acc[1] % dblk)  * BLOCK_WORDS;
    size_t ti = (size_t)(acc[2] % stblk) * BLOCK_WORDS;
    for(int k=0;k<BLOCK_WORDS;k++){
      acc[k] ^= scratch[si+k];
      acc[k] += ds[di+k];
      acc[k] ^= state[ti+k];
    }
    if(acc[3] & 1ULL) arx_perm(acc);
    else { arx_perm(acc); arx_perm(acc); }
    for(int k=0;k<BLOCK_WORDS;k++){
      scratch[si+k] = acc[k] ^ rotr64(acc[(k+1)&15], 1);
      state[ti+k]   = acc[k] + rotr64(acc[(k+9)&15], 17);
    }
  }
}

/* Build the epoch dataset from a 32-byte epoch seed (stable across an epoch). */
static void lattice_build_dataset(const u8 epoch_seed[32], u64 *ds){
  u64 es[4];
  for(int i=0;i<4;i++){ u64 w=0; for(int b=0;b<8;b++) w|=(u64)epoch_seed[i*8+b]<<(8*b); es[i]=w; }
  u64 eseed[8]; sponge(es, 4, eseed, 8, 0x02 /*epoch*/);
  expand(eseed, 0xDA /*dataset*/, ds, DATASET_WORDS);
}

/* One PoW evaluation against a prebuilt epoch dataset. */
static void lattice_hash_ds(const u8 *input, size_t len, const u64 *ds, u8 out[32]){
  int inwords = (int)((len+7)/8); if(inwords<1) inwords=1;
  u64 *iw = calloc(inwords, sizeof(u64));
  for(size_t i=0;i<len;i++) iw[i/8] |= (u64)input[i] << (8*(i%8));
  u64 *state = malloc(ST_WORDS*sizeof(u64));
  sponge(iw, inwords, state, ST_WORDS, 0x01 ^ ((u64)len<<16));
  free(iw);

  u64 *scratch = malloc(SCRATCH_WORDS*sizeof(u64));
  lattice_walk(state, scratch, ds);
  free(scratch);

  u64 fh[4]; sponge(state, ST_WORDS, fh, 4, 0xF1 /*final*/);
  free(state);
  for(int i=0;i<4;i++) store64le(out+i*8, fh[i]);
}

/* Full Lattice: build the epoch dataset, hash, free. */
void lattice_hash(const uint8_t *input, size_t len,
                  const uint8_t epoch_seed[32], uint8_t out[32]){
  u64 *ds = malloc(DATASET_WORDS*sizeof(u64));
  lattice_build_dataset(epoch_seed, ds);
  lattice_hash_ds(input, len, ds, out);
  free(ds);
}
