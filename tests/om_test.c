// Stress test of ps5malloc.inc (the title's own malloc): threads, all size classes, realloc, aligned blocks, calloc, dedicated mappings, pointers that belong to the "libc" (here glibc), usable size.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <malloc.h>
void *__real_malloc(size_t n) { return malloc(n); }
void __real_free(void *p) { free(p); }
void *__real_realloc(void *p, size_t n) { return realloc(p, n); }
#include "../ps5malloc.inc"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { __atomic_add_fetch(&fails, 1, __ATOMIC_RELAXED); if (fails < 10) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)
static unsigned rnd(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
typedef struct { unsigned char *p; size_t n; unsigned char tag; int foreign; } Blk;
static void fill(Blk *b) { memset(b->p, b->tag, b->n); }
static int verify(Blk *b, size_t upto) { for (size_t i = 0; i < upto; i += (upto > 4096 ? 509 : 1)) if (b->p[i] != b->tag) return 0; return b->n == 0 || b->p[upto - 1] == b->tag; }
static size_t pick(unsigned *s) {
    unsigned r = rnd(s) % 100;
    if (r < 60) return 1 + rnd(s) % 300;
    if (r < 85) return 1 + rnd(s) % 4000;
    if (r < 96) return 1 + rnd(s) % 200000;
    if (r < 99) return 1 + rnd(s) % (4 << 20);
    return (17ul << 20) + rnd(s) % (3 << 20);   // dedicated mapping
}
static void *worker(void *a) {
    unsigned s = (unsigned)(long)a * 7919u + 1; enum { N = 400 }; Blk b[N]; memset(b, 0, sizeof b);
    for (int it = 0; it < 60000; it++) {
        int i = rnd(&s) % N; Blk *k = &b[i]; int op = rnd(&s) % 10;
        if (k->p) {
            CHECK(verify(k, k->n), "content of block %zu bytes (foreign %d) damaged", k->n, k->foreign);
            if (op < 4) { size_t nn = pick(&s); unsigned char *q = k->foreign ? __wrap_realloc(k->p, nn) : __wrap_realloc(k->p, nn); CHECK(q, "realloc"); if (!q) return 0; size_t keep = nn < k->n ? nn : k->n; k->p = q; k->n = nn; (void)keep; k->tag = (unsigned char)rnd(&s); fill(k); }
            else { __wrap_free(k->p); k->p = 0; }
        } else {
            size_t n = pick(&s); int kind = rnd(&s) % 10; k->foreign = 0;
            if (kind == 0) { k->p = __wrap_calloc(n, 1); CHECK(k->p, "calloc"); if (k->p) for (size_t j = 0; j < n; j += 997) CHECK(k->p[j] == 0, "calloc not zero"); }
            else if (kind == 1) { size_t al = (size_t)16 << (rnd(&s) % 9); void *p = 0; int r = __wrap_posix_memalign(&p, al, n); CHECK(r == 0 && p && ((unsigned long)p % al) == 0, "memalign %zu -> %p rc %d", al, p, r); k->p = p; }
            else if (kind == 2) { k->p = malloc(n); k->foreign = 1; }   // a pointer the libc made
            else k->p = __wrap_malloc(n);
            if (!k->p) { CHECK(0, "alloc %zu failed", n); return 0; }
            CHECK(!k->foreign ? __wrap_malloc_usable_size(k->p) >= n : 1, "usable size %zu < %zu", __wrap_malloc_usable_size(k->p), n);
            CHECK(((unsigned long)k->p & 15) == 0, "not 16-aligned %p", (void *)k->p);
            k->n = n; k->tag = (unsigned char)rnd(&s); fill(k);
        }
    }
    for (int i = 0; i < N; i++) if (b[i].p) { CHECK(verify(&b[i], b[i].n), "final content"); __wrap_free(b[i].p); }
    return 0;
}
int main(void) {
    pthread_t t[8]; for (long i = 0; i < 8; i++) pthread_create(&t[i], 0, worker, (void *)(i + 1));
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    CHECK(om_used == 0, "bytes still in use after everything was freed: %lu", om_used);
    // class table sanity: every size maps to a block >= size, the class index round-trips to the same block size
    for (unsigned long sz = 1; sz < (16ul << 20); sz += (sz < 4096 ? 1 : 977)) { int idx; unsigned long bs = om_bs(sz, &idx); CHECK(bs >= sz && idx >= 0 && idx < 128 && om_bs_of_idx(idx) == bs, "class of %lu: bs %lu idx %d back %lu", sz, bs, idx, om_bs_of_idx(idx)); }
    printf("om_test: peak %lu MB, %lu chunks, %lu dedicated, %lu foreign frees, fails=%d\n", om_peak >> 20, om_chunks, om_dedic_n, om_foreign, fails);
    return fails ? 1 : 0;
}
