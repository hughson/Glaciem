/*
 * randomx_stub.c -- linker stubs for the RandomX API.
 *
 * The embedded wallet links cryptonote_core, whose proof-of-work path
 * (rx-slow-hash.c) references the RandomX API. RandomX itself is NOT linked:
 * its hand-written aarch64 JIT assembly has 1 MB-range branches that cannot
 * span this large shared library.
 *
 * This is safe: the wallet runs against a TRUSTED daemon and never verifies
 * proof-of-work, so none of these are ever called. If one ever were, abort()
 * makes that loud rather than silent.
 */
#include <stdlib.h>

void         *randomx_get_flags(void)                                    { abort(); }
void         *randomx_alloc_cache(void *flags)                           { abort(); }
void          randomx_init_cache(void *c, const void *k, unsigned long n){ abort(); }
void         *randomx_alloc_dataset(void *flags)                         { abort(); }
unsigned long randomx_dataset_item_count(void)                           { abort(); }
void          randomx_init_dataset(void *d, void *c,
                                   unsigned long s, unsigned long n)      { abort(); }
void         *randomx_create_vm(void *flags, void *cache, void *dataset)  { abort(); }
void          randomx_vm_set_cache(void *vm, void *cache)                 { abort(); }
void          randomx_destroy_vm(void *vm)                                { abort(); }
void          randomx_calculate_hash(void *vm, const void *in,
                                     unsigned long n, void *out)          { abort(); }
