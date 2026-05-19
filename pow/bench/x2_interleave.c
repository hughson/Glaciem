/* x2_interleave.c -- prototype #1: N=2 multi-nonce interleaving for Lattice.
 *
 * Steps two independent nonces in lockstep through the walk so one core's
 * out-of-order window can overlap nonce B's compute with nonce A's outstanding
 * scratchpad/dataset loads (the walk is latency-bound on cache-starved CPUs).
 *
 * On the M4 this measured ~1.03x -- the M4's large L2 absorbs the working set,
 * so it is compute-bound, not latency-bound. Re-measured here on x86 (smaller
 * L2, latency-bound) where the technique should actually pay off.
 *
 * Output MUST be bit-identical to scalar lattice_hash_ds -- verified below.
 *
 * Build: cc -O3 -mavx2 -mbmi2 -funroll-loops -o x2 x2_interleave.c
 */
#define LATTICE_NO_MAIN
#include "../lattice_ref.c"

static void seed_state(const u8 *input, size_t len, u64 *state){
  int inwords = (int)((len+7)/8); if(inwords<1) inwords=1;
  u64 *iw = calloc(inwords, sizeof(u64));
  for(size_t i=0;i<len;i++) iw[i/8] |= (u64)input[i] << (8*(i%8));
  sponge(iw, inwords, state, ST_WORDS, 0x01 ^ ((u64)len<<16));
  free(iw);
}

/* interleaved walk: two nonces stepped together, bit-identical per-nonce */
static void lattice_walk_x2(u64 *st0,u64 *sc0, u64 *st1,u64 *sc1, const u64 *ds){
  u64 acc0[16], acc1[16];
  sponge(st0, ST_WORDS, acc0, 16, 0x58);
  sponge(st1, ST_WORDS, acc1, 16, 0x58);
  expand(acc0, 0x58, sc0, SCRATCH_WORDS);
  expand(acc1, 0x58, sc1, SCRATCH_WORDS);
  const size_t sblk  = SCRATCH_WORDS/BLOCK_WORDS;
  const size_t dblk  = DATASET_WORDS/BLOCK_WORDS;
  const size_t stblk = ST_WORDS/BLOCK_WORDS;
  for(int it=0; it<L_WALK; it++){
    size_t si0=(size_t)(acc0[0]%sblk)*BLOCK_WORDS;
    size_t di0=(size_t)(acc0[1]%dblk)*BLOCK_WORDS;
    size_t ti0=(size_t)(acc0[2]%stblk)*BLOCK_WORDS;
    size_t si1=(size_t)(acc1[0]%sblk)*BLOCK_WORDS;
    size_t di1=(size_t)(acc1[1]%dblk)*BLOCK_WORDS;
    size_t ti1=(size_t)(acc1[2]%stblk)*BLOCK_WORDS;
    for(int k=0;k<BLOCK_WORDS;k++){          /* both nonces' loads issue together */
      acc0[k]^=sc0[si0+k]; acc0[k]+=ds[di0+k]; acc0[k]^=st0[ti0+k];
      acc1[k]^=sc1[si1+k]; acc1[k]+=ds[di1+k]; acc1[k]^=st1[ti1+k];
    }
    if(acc0[3]&1ULL) arx_perm(acc0); else { arx_perm(acc0); arx_perm(acc0); }
    if(acc1[3]&1ULL) arx_perm(acc1); else { arx_perm(acc1); arx_perm(acc1); }
    for(int k=0;k<BLOCK_WORDS;k++){
      sc0[si0+k]=acc0[k]^rotr64(acc0[(k+1)&15],1);
      st0[ti0+k]=acc0[k]+rotr64(acc0[(k+9)&15],17);
      sc1[si1+k]=acc1[k]^rotr64(acc1[(k+1)&15],1);
      st1[ti1+k]=acc1[k]+rotr64(acc1[(k+9)&15],17);
    }
  }
}

static void lattice_hash_ds_x2(const u8 *in0,size_t l0, const u8 *in1,size_t l1,
                               const u64 *ds, u8 o0[32], u8 o1[32]){
  u64 *st0=malloc(ST_WORDS*8), *st1=malloc(ST_WORDS*8);
  u64 *sc0=malloc(SCRATCH_WORDS*8), *sc1=malloc(SCRATCH_WORDS*8);
  seed_state(in0,l0,st0); seed_state(in1,l1,st1);
  lattice_walk_x2(st0,sc0, st1,sc1, ds);
  u64 fh0[4], fh1[4];
  sponge(st0,ST_WORDS,fh0,4,0xF1);
  sponge(st1,ST_WORDS,fh1,4,0xF1);
  for(int i=0;i<4;i++){ store64le(o0+i*8,fh0[i]); store64le(o1+i*8,fh1[i]); }
  free(st0);free(st1);free(sc0);free(sc1);
}

static double now_ms(void){ return 1000.0*(double)clock()/CLOCKS_PER_SEC; }

int main(void){
  u8 es[32]; for(int i=0;i<32;i++) es[i]=(u8)i;
  u64 *ds = malloc(DATASET_WORDS*sizeof(u64));
  lattice_build_dataset(es, ds);

  int fails=0;
  for(int n=0;n<8;n+=2){
    u8 h0[76], h1[76];
    for(int i=0;i<76;i++){ h0[i]=(u8)(i*7+1); h1[i]=(u8)(i*7+1); }
    h0[39]=(u8)n; h1[39]=(u8)(n+1);
    u8 sa[32], sb[32], xa[32], xb[32];
    lattice_hash_ds(h0,76,ds,sa);
    lattice_hash_ds(h1,76,ds,sb);
    lattice_hash_ds_x2(h0,76,h1,76,ds,xa,xb);
    if(memcmp(sa,xa,32)||memcmp(sb,xb,32)) fails++;
  }
  printf("correctness (x2 == scalar, 8 nonces) = %s\n", fails? "FAIL":"PASS");
  if(fails){ free(ds); return 1; }

  const int REPS=64;
  u8 hdr[76]; for(int i=0;i<76;i++) hdr[i]=(u8)(i*7+1);
  u8 h[32], hb[32];
  lattice_hash_ds(hdr,76,ds,h);

  double best_s=1e9, best_x=1e9;
  for(int run=0; run<12; run++){
    double t0=now_ms();
    for(int k=0;k<REPS;k++){ hdr[39]=(u8)k; lattice_hash_ds(hdr,76,ds,h); }
    double s_per=(now_ms()-t0)/REPS; if(s_per<best_s) best_s=s_per;

    t0=now_ms();
    for(int k=0;k<REPS;k+=2){
      u8 a[76],b[76];
      for(int i=0;i<76;i++){ a[i]=hdr[i]; b[i]=hdr[i]; }
      a[39]=(u8)k; b[39]=(u8)(k+1);
      lattice_hash_ds_x2(a,76,b,76,ds,h,hb);
    }
    double x_per=(now_ms()-t0)/REPS; if(x_per<best_x) best_x=x_per;
  }
  free(ds);
  printf("scalar : %.3f ms/nonce  -> %.1f H/s/core\n", best_s, 1000.0/best_s);
  printf("x2     : %.3f ms/nonce  -> %.1f H/s/core\n", best_x, 1000.0/best_x);
  printf("speedup: %.2fx\n", best_s/best_x);
  return 0;
}
