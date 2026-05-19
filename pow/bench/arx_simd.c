/* arx_simd.c -- prototype #7: SIMD arx_perm for Lattice.
 *
 * arx_perm is BLAKE2b's round function on a 16-word state. The 4 column G's
 * are independent, as are the 4 diagonal G's -- so the state is held as four
 * 4-wide row vectors and one vectorised G does all 4 columns at once; the
 * diagonal step reuses it after a lane "diagonalisation" (standard BLAKE2 SIMD).
 *
 * On the M4 (128-bit NEON, no 64-bit rotate) this measured 0.70x -- SLOWER.
 * Re-measured here on x86: AVX2 is native 256-bit / 4-wide u64, so the row
 * layout maps to one register per row and diagonalisation is a single vpermq.
 *
 * Output MUST be bit-identical to scalar arx_perm -- verified below.
 * Uses Clang vector extensions + __builtin_shufflevector (build with clang).
 *
 * Build: clang -O3 -mavx2 -mbmi2 -funroll-loops -o arx arx_simd.c
 */
#define LATTICE_NO_MAIN
#include "../lattice_ref.c"

typedef unsigned long long u64x4 __attribute__((vector_size(32)));

static inline u64x4 rotr_v(u64x4 x, int n){
  return (x >> n) | (x << (64 - n));
}

#define GV(a,b,c,d) do{ \
  a=a+b; d=rotr_v(d^a,32); c=c+d; b=rotr_v(b^c,24); \
  a=a+b; d=rotr_v(d^a,16); c=c+d; b=rotr_v(b^c,63); }while(0)

static void arx_perm_simd(u64 v[16]){
  u64x4 r0,r1,r2,r3;
  memcpy(&r0, v+0,  32);
  memcpy(&r1, v+4,  32);
  memcpy(&r2, v+8,  32);
  memcpy(&r3, v+12, 32);
  for(int r=0;r<ARX_ROUNDS;r++){
    GV(r0,r1,r2,r3);
    r1 = __builtin_shufflevector(r1,r1, 1,2,3,0);
    r2 = __builtin_shufflevector(r2,r2, 2,3,0,1);
    r3 = __builtin_shufflevector(r3,r3, 3,0,1,2);
    GV(r0,r1,r2,r3);
    r1 = __builtin_shufflevector(r1,r1, 3,0,1,2);
    r2 = __builtin_shufflevector(r2,r2, 2,3,0,1);
    r3 = __builtin_shufflevector(r3,r3, 1,2,3,0);
  }
  memcpy(v+0,  &r0, 32);
  memcpy(v+4,  &r1, 32);
  memcpy(v+8,  &r2, 32);
  memcpy(v+12, &r3, 32);
}

static void arx_perm_scalar(u64 v[16]){
  for(int r=0;r<ARX_ROUNDS;r++){
    G(v[0],v[4],v[ 8],v[12]); G(v[1],v[5],v[ 9],v[13]);
    G(v[2],v[6],v[10],v[14]); G(v[3],v[7],v[11],v[15]);
    G(v[0],v[5],v[10],v[15]); G(v[1],v[6],v[11],v[12]);
    G(v[2],v[7],v[ 8],v[13]); G(v[3],v[4],v[ 9],v[14]);
  }
}

static double now_ms(void){ return 1000.0*(double)clock()/CLOCKS_PER_SEC; }

int main(void){
  u64 seed[16]; for(int i=0;i<16;i++) seed[i]=0x9E3779B97F4A7C15ULL*(i+1);
  int fails=0;
  for(int t=0;t<100000;t++){
    u64 a[16], b[16];
    for(int i=0;i<16;i++){ seed[i^7] ^= (seed[i]<<13)|(seed[i]>>51); a[i]=b[i]=seed[i]; }
    arx_perm_scalar(a);
    arx_perm_simd(b);
    if(memcmp(a,b,sizeof a)) fails++;
  }
  printf("correctness (SIMD == scalar, 100k random states) = %s\n", fails? "FAIL":"PASS");
  if(fails) return 1;

  const long N = 20000000;
  u64 v[16];
  double best_s=1e18, best_v=1e18;
  volatile u64 sink=0;
  for(int run=0;run<8;run++){
    for(int i=0;i<16;i++) v[i]=0x0123456789ABCDEFULL ^ (i*0x100);
    double t0=now_ms();
    for(long i=0;i<N;i++){ arx_perm_scalar(v); v[0]^=i; }
    double s=(now_ms()-t0)*1e6/N; if(s<best_s) best_s=s;
    sink ^= v[0]^v[7]^v[15];

    for(int i=0;i<16;i++) v[i]=0x0123456789ABCDEFULL ^ (i*0x100);
    t0=now_ms();
    for(long i=0;i<N;i++){ arx_perm_simd(v); v[0]^=i; }
    double w=(now_ms()-t0)*1e6/N; if(w<best_v) best_v=w;
    sink ^= v[0]^v[7]^v[15];
  }
  printf("scalar arx_perm : %.2f ns/call\n", best_s);
  printf("SIMD   arx_perm : %.2f ns/call\n", best_v);
  printf("speedup         : %.2fx\n", best_s/best_v);
  return (int)(sink & 0);
}
