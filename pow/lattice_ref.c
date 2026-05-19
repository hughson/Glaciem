/*
 * lattice_ref.c -- Lattice, CPU-only reference implementation.
 *
 * Lattice is the "economic gravity well" proof-of-work (see POW_DESIGN.md
 * section 8): CPU-only, integer-only, latency-bound and branchy. It puts the
 * mining contest on CPUs -- the terrain where Apple Silicon's performance-per-
 * watt lead is largest and least contested. There is NO GPU phase: v1's
 * Y-phase existed only to feed the abandoned CPU<->GPU "moat", so v2 drops it.
 * No Metal, no OpenCL -- one algorithm, one kind of hardware.
 *
 * v2 is not a new algorithm. It is v1's X-phase promoted to the whole
 * pipeline. The sole cryptographic primitive is still arx_perm() -- the
 * BLAKE2b round function, a well-analysed 64-bit ARX permutation. The sponge,
 * the epoch dataset, and the walk are unchanged in shape from v1's structure.
 * What changed: no Y-phase, no stage alternation -- one long, serial,
 * latency-bound walk over a large per-nonce scratchpad.
 *
 * Why it favours Apple Silicon (economically, not structurally):
 *   - latency-bound serial dependency chain  -> rewards wide, high-IPC cores
 *   - large per-nonce scratchpad             -> collapses GPU occupancy
 *   - data-dependent branch every iteration  -> warp divergence on a GPU
 *   - integer-only, cheap CPU verification   -> any node validates, no GPU
 * The contest lands on CPUs; Apple's perf-per-watt makes a Mac the most
 * profitable box to mine on where electricity is normally priced.
 *
 * STATUS: v2 structural reference. SCRATCH_WORDS and L_WALK are PROVISIONAL --
 * tuned by measurement on real hardware (POW_DESIGN.md roadmap). NOT
 * cryptographically reviewed. Defines the algorithm shape, not the final
 * secure parameters.
 *
 * Build: cc -O2 -Wall -o lattice_ref lattice_ref.c
 * Run:   ./lattice_ref            (self-tests: determinism, avalanche, verify cost)
 *        ./lattice_ref --vectors  (regenerate test vectors)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint64_t u64;
typedef uint8_t  u8;

/* ---- parameters (v2; provisional, pending measurement + review) ---- */
#define ARX_ROUNDS    8
#define RATE_WORDS    8                 /* sponge rate; capacity = 16-8 = 8    */
#define BLOCK_WORDS   16                /* mixing block = 128 bytes            */
#define ST_WORDS      256               /* working state = 2 KiB / nonce       */
#define DATASET_WORDS (4*1024*1024/8)   /* epoch-global dataset = 4 MiB        */

/* The two tuning knobs. Overridable at build time (-DSCRATCH_WORDS=, -DL_WALK=)
   for the parameter sweep; the defaults are the provisional candidates.
   L_WALK is set high on purpose: the walk is the branch-divergent, random-
   access, CPU-favouring work; the scratchpad fill is blander sequential work.
   L_WALK must dominate so the per-nonce cost is mostly the walk (M3 tuning). */
#ifndef SCRATCH_WORDS
#define SCRATCH_WORDS (1024*1024/8)     /* per-nonce scratchpad = 1 MiB -- TUNABLE */
#endif
#ifndef L_WALK
#define L_WALK        32768             /* walk iterations (the work) -- TUNABLE   */
#endif

/* SCRATCH_WORDS and L_WALK are the two tuning knobs (POW_DESIGN.md roadmap):
 *   SCRATCH_WORDS -- sized so the live working set sits in a large CPU cache
 *                    (Apple's big per-cluster L2 + SLC) but thrashes small
 *                    caches and collapses GPU occupancy. 1 MiB is a starting
 *                    candidate; the tuned value is set by benchmark.
 *   L_WALK        -- the serial walk length; sets the cost of one evaluation.
 *                    Tuned so a single CPU verification is a few milliseconds. */

static inline u64 rotr64(u64 x, int n){ return (x >> n) | (x << (64 - n)); }
static inline void store64le(u8 *p, u64 v){ for(int i=0;i<8;i++) p[i]=(u8)(v>>(8*i)); }

/* BLAKE2b G mixing function */
#define G(a,b,c,d) do{ \
  a=a+b; d=rotr64(d^a,32); c=c+d; b=rotr64(b^c,24); \
  a=a+b; d=rotr64(d^a,16); c=c+d; b=rotr64(b^c,63); }while(0)

/* arx_perm: the sole primitive -- BLAKE2b column+diagonal mixing, ARX_ROUNDS.
   Unchanged from the v1 reference -- this is the one function review scrutinises. */
static void arx_perm(u64 v[16]){
  for(int r=0;r<ARX_ROUNDS;r++){
    G(v[0],v[4],v[ 8],v[12]); G(v[1],v[5],v[ 9],v[13]);
    G(v[2],v[6],v[10],v[14]); G(v[3],v[7],v[11],v[15]);
    G(v[0],v[5],v[10],v[15]); G(v[1],v[6],v[11],v[12]);
    G(v[2],v[7],v[ 8],v[13]); G(v[3],v[4],v[ 9],v[14]);
  }
}

/* Sponge hash over arx_perm: absorb in[inwords], squeeze out[outwords].
   `domain` separates call sites (seed / epoch / walk / dataset / final). */
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
  last[inwords-off] ^= 0x01;                       /* 10*1 padding start */
  last[RATE_WORDS-1] ^= 0x8000000000000000ULL;     /* padding end        */
  for(int i=0;i<RATE_WORDS;i++) s[i] ^= last[i];
  arx_perm(s);
  int got=0;
  while(got<outwords){
    for(int i=0;i<RATE_WORDS && got<outwords;i++) out[got++]=s[i];
    if(got<outwords) arx_perm(s);
  }
}

/* Fill `buf[n]` deterministically from an 8-word seed (arx_perm keystream). */
static void expand(const u64 seed[8], u64 domain, u64 *buf, size_t n){
  u64 s[16]; memset(s,0,sizeof s);
  for(int i=0;i<8;i++) s[i]=seed[i];
  s[8] ^= domain;
  for(size_t j=0;j<n;j+=RATE_WORDS){
    arx_perm(s);
    for(int i=0;i<RATE_WORDS && j+i<n;i++) buf[j+i]=s[i];
  }
}

/* The walk: a latency-bound, branchy traversal of a large per-nonce scratchpad.
   Each iteration derives scratchpad / dataset / state block indices from the
   accumulator, mixes those blocks in, applies arx_perm once or twice on a
   data-dependent bit (a branch -> warp divergence on a GPU), and writes the
   result back into the scratchpad and the working state. The dependency chain
   is serial -- latency-bound, a CPU strength -- and the 1 MiB working set
   collapses GPU occupancy (the proven RandomX mechanism). This is v1's X-phase,
   promoted from one stage to the whole algorithm. */
static void lattice_walk(u64 *state, u64 *scratch, const u64 *ds){
  u64 acc[16];
  sponge(state, ST_WORDS, acc, 16, 0x58 /*walk*/);  /* working set from state */
  expand(acc, 0x58, scratch, SCRATCH_WORDS);        /* acc[0..7] seed scratch */
  const size_t sblk  = SCRATCH_WORDS/BLOCK_WORDS;
  const size_t dblk  = DATASET_WORDS/BLOCK_WORDS;
  const size_t stblk = ST_WORDS/BLOCK_WORDS;
  for(int it=0; it<L_WALK; it++){
    size_t si = (size_t)(acc[0] % sblk)  * BLOCK_WORDS;   /* data-dependent  */
    size_t di = (size_t)(acc[1] % dblk)  * BLOCK_WORDS;
    size_t ti = (size_t)(acc[2] % stblk) * BLOCK_WORDS;
    for(int k=0;k<BLOCK_WORDS;k++){
      acc[k] ^= scratch[si+k];
      acc[k] += ds[di+k];
      acc[k] ^= state[ti+k];
    }
    if(acc[3] & 1ULL) arx_perm(acc);          /* data-dependent branch ->    */
    else { arx_perm(acc); arx_perm(acc); }    /* warp divergence on a GPU    */
    for(int k=0;k<BLOCK_WORDS;k++){
      scratch[si+k] = acc[k] ^ rotr64(acc[(k+1)&15], 1);   /* write back     */
      state[ti+k]   = acc[k] + rotr64(acc[(k+9)&15], 17);  /* stir the state */
    }
  }
}

/* Full Lattice.
 *   input       block hashing blob (header incl. nonce)
 *   epoch_seed  32 bytes keying the dataset; stable across an epoch -- in
 *               consensus, the hash of a past "seed" block (RandomX-style).
 *   out         32-byte PoW hash
 */
/* Build the epoch dataset from a 32-byte epoch seed. In consensus the dataset
   is stable across an epoch -- built once, reused for every nonce -- so this
   cost amortises and is NOT part of the per-nonce work. `ds` holds DATASET_WORDS. */
void lattice_build_dataset(const u8 epoch_seed[32], u64 *ds){
  u64 es[4];
  for(int i=0;i<4;i++){ u64 w=0; for(int b=0;b<8;b++) w|=(u64)epoch_seed[i*8+b]<<(8*b); es[i]=w; }
  u64 eseed[8]; sponge(es, 4, eseed, 8, 0x02 /*epoch*/);
  expand(eseed, 0xDA /*dataset*/, ds, DATASET_WORDS);
}

/* One PoW evaluation against a prebuilt epoch dataset -- this is the per-nonce
   work a miner repeats and a verifier runs exactly once. */
void lattice_hash_ds(const u8 *input, size_t len, const u64 *ds, u8 out[32]){
  /* seed the working state from the input (header|nonce) */
  int inwords = (int)((len+7)/8); if(inwords<1) inwords=1;
  u64 *iw = calloc(inwords, sizeof(u64));
  for(size_t i=0;i<len;i++) iw[i/8] |= (u64)input[i] << (8*(i%8));
  u64 *state = malloc(ST_WORDS*sizeof(u64));
  sponge(iw, inwords, state, ST_WORDS, 0x01 ^ ((u64)len<<16) /*seed|byte-len*/);
  free(iw);

  /* the CPU-bound walk -- the whole of the per-nonce work */
  u64 *scratch = malloc(SCRATCH_WORDS*sizeof(u64));
  lattice_walk(state, scratch, ds);
  free(scratch);

  u64 fh[4]; sponge(state, ST_WORDS, fh, 4, 0xF1 /*final*/);
  free(state);
  for(int i=0;i<4;i++) store64le(out+i*8, fh[i]);
}

/* Convenience one-shot: build the dataset, hash, free. For callers that do not
   cache the epoch dataset (tests, tools). Miners and the daemon cache it. */
void lattice_hash(const u8 *input, size_t len, const u8 epoch_seed[32], u8 out[32]){
  u64 *ds = malloc(DATASET_WORDS*sizeof(u64));
  lattice_build_dataset(epoch_seed, ds);
  lattice_hash_ds(input, len, ds, out);
  free(ds);
}

/* ------------------------------ self-tests ------------------------------ */
#ifndef LATTICE_NO_MAIN
static void hex(const u8 *b, int n){ for(int i=0;i<n;i++) printf("%02x",b[i]); }

static void emit_vec(const char *label, const u8 *in, size_t len, const u8 es[32]){
  u8 h[32]; lattice_hash(in,len,es,h);
  printf("%-14s ", label);
  if(len==0) printf("-"); else hex(in,(int)len);
  printf(" "); hex(h,32); printf("\n");
}

static void print_vectors(void){
  printf("# Lattice test vectors\n");
  printf("# source: pow/lattice_ref.c   regenerate: ./lattice_ref --vectors\n");
  printf("# v2 / provisional parameters -- vectors change if the algorithm changes.\n");
  u8 es[32]; for(int i=0;i<32;i++) es[i]=(u8)i;     /* fixed epoch seed 00..1f */
  printf("# epoch_seed: "); hex(es,32); printf("\n");
  printf("# format: <label> <input-hex|-> <pow-hash-hex>\n");
  u8 z32[32]={0};
  u8 hdr[76]; for(int i=0;i<76;i++) hdr[i]=(u8)(i*7+1);
  emit_vec("empty",       (const u8*)"", 0,  es);
  emit_vec("byte00",      z32, 1,  es);
  emit_vec("abc",         (const u8*)"abc", 3,  es);
  emit_vec("zeros32",     z32, 32, es);
  emit_vec("header76",    hdr, 76, es);
  hdr[39]=1; emit_vec("header76_n1", hdr, 76, es);
  hdr[39]=2; emit_vec("header76_n2", hdr, 76, es);
}

int main(int argc, char **argv){
  if(argc>1 && strcmp(argv[1],"--vectors")==0){ print_vectors(); return 0; }
  if(argc>1 && strcmp(argv[1],"--bench")==0){
    /* machine-readable timing for the SCRATCH_WORDS / L_WALK sweep. The epoch
       dataset is built ONCE (amortised in consensus) and excluded from the
       per-nonce figure -- only the real per-nonce work is timed. */
    u8 hdr[76]; for(int i=0;i<76;i++) hdr[i]=(u8)(i*7+1);
    u8 es[32]; for(int i=0;i<32;i++) es[i]=(u8)i;
    u8 h[32];
    u64 *ds = malloc(DATASET_WORDS*sizeof(u64));
    clock_t d0=clock();
    lattice_build_dataset(es, ds);
    double dms=1000.0*(double)(clock()-d0)/CLOCKS_PER_SEC;
    lattice_hash_ds(hdr,sizeof hdr,ds,h);               /* warm up */
    const int REPS=32;
    clock_t t0=clock();
    for(int k=0;k<REPS;k++){ hdr[39]=(u8)k; lattice_hash_ds(hdr,sizeof hdr,ds,h); }
    double ms=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC/REPS;
    free(ds);
    printf("scratch=%6dKiB  L_WALK=%-6d  per-nonce=%7.3f ms  %8.1f H/s/core"
           "   (dataset build %.1f ms, amortised)\n",
           SCRATCH_WORDS*8/1024, L_WALK, ms, 1000.0/ms, dms);
    return 0;
  }
  printf("Lattice reference -- CPU-only -- self-test\n");
  printf("params: ARX_ROUNDS=%d L_WALK=%d state=%dB scratch=%dKiB dataset=%dMiB\n\n",
         ARX_ROUNDS, L_WALK, ST_WORDS*8, SCRATCH_WORDS*8/1024,
         DATASET_WORDS*8/(1024*1024));

  u8 hdr[76]; for(int i=0;i<76;i++) hdr[i]=(u8)(i*7+1);  /* mock header */
  u8 eseed[32]; for(int i=0;i<32;i++) eseed[i]=(u8)i;    /* fixed epoch seed */
  u8 a[32], b[32], c[32];

  /* build the epoch dataset once -- amortised across every nonce */
  u64 *ds = malloc(DATASET_WORDS*sizeof(u64));
  lattice_build_dataset(eseed, ds);

  lattice_hash_ds(hdr,sizeof hdr,ds,a);
  lattice_hash_ds(hdr,sizeof hdr,ds,b);
  printf("hash(header)        = "); hex(a,32); printf("\n");
  printf("determinism (a==b)  = %s\n", memcmp(a,b,32)==0 ? "PASS" : "FAIL");

  hdr[40] ^= 1;                               /* flip one bit (a nonce byte) */
  lattice_hash_ds(hdr,sizeof hdr,ds,c);
  int diff=0; for(int i=0;i<32;i++) for(int k=0;k<8;k++) diff += ((c[i]>>k)&1) ^ ((a[i]>>k)&1);
  printf("hash(header')       = "); hex(c,32); printf("\n");
  printf("avalanche (1-bit)   = %d/256 bits flipped %s\n",
         diff, (diff>90 && diff<166) ? "PASS" : "WEAK");
  hdr[40] ^= 1;                               /* restore */

  /* tiny mining demo: search a nonce for 8 leading zero bits (~256 expected) */
  u8 m[76]; memcpy(m,hdr,76); u8 h[32];
  unsigned nonce=0, found=0;
  for(; nonce<2000; nonce++){
    m[39]=(u8)nonce; m[40]=(u8)(nonce>>8);
    lattice_hash_ds(m,sizeof m,ds,h);
    if(h[31]==0){ found=1; break; }            /* 8 leading zero bits */
  }
  if(found){ printf("mining demo         = nonce %u -> ", nonce); hex(h,32); printf("\n"); }
  else      printf("mining demo         = no nonce < 2000 (rare)\n");

  /* per-nonce cost: the work a verifier runs once per block (dataset excluded
     -- it is built once per epoch and reused). Must stay cheap. */
  {
    const int REPS=24;
    clock_t t0=clock();
    for(int k=0;k<REPS;k++){ m[39]=(u8)k; lattice_hash_ds(m,sizeof m,ds,h); }
    double ms = 1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC/REPS;
    printf("per-nonce cost      = %.2f ms / evaluation (CPU, integer-only)\n", ms);
  }
  free(ds);
  return 0;
}
#endif
