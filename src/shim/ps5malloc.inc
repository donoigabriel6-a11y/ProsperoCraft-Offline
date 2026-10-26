// ps5malloc.inc - the title's own malloc (included by ps5lwjgl.c in the PS5_VK build; OWNMALLOC=0 at build time leaves it out).
//
// Why: in the Vulkan / Zink build malloc() is NOT redirected into the OpenGL SDK's big app heap (that heap belongs to the GL-only title), so every malloc of the JVM, LWJGL, Mesa and Zink lands in the console
// libc's own heap, which is bounded: its free capacity (the "pieces=" of the timeline, 256 KB each) is only ~12 MB at the start and is eaten by class loading / shader compiles / chunk building. At 0 the libc cannot
// allocate any more: pthread_mutex_init / cond_init / attr_init return ENOMEM (the "CLIFF") and the JVM then stalls for seconds to minutes inside native calls (logs 1b6f4b5a, fe8789f5: render thread stuck in
// ClassLoader.defineClass1 for 52 s with the pool at 0). This allocator serves everything the game mallocs from 64 MB anonymous mmap chunks instead, so the libc heap keeps its capacity for the libc's own needs.
//
// Design (small and boring on purpose):
//  * size classes: 16-byte steps up to 1 KB, then 4 classes per doubling up to 16 MB; blocks carry a 16-byte header {tag, info}; freed blocks go to a per-class free list (never returned to the OS).
//  * chunks are 64 MB, 64 MB-aligned, so "is this pointer ours?" is a single bit test (om_map); larger requests get their own mapping (table of at most OM_DEDIC).
//  * pointers that are not ours (the libc allocates them internally: scandir, asprintf, FILE buffers ...) are passed to the libc's free / realloc / malloc_usable_size.
//  * one global spin lock (no pthread objects: those are what runs out).
#include <sys/mman.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define OM_CHUNK (64ul << 20)
#define OM_BIG   (16ul << 20)
#define OM_PAGE  16384ul
#define OM_DEDIC 512
#define OM_ALIGNED 0x100ul
#define OM_TAG 0x9E3779B97F4A7C15ull

#ifndef OM_LOG
#define OM_LOG(...) ((void)0)
#endif
// Memory source: the console's anonymous mmap is served from the FLEXIBLE pool (only ~42 MB free in this title: a 64 MB chunk always fails), so chunks come from DIRECT memory (8+ GB free) like the JVM heap; plain mmap is the fallback.
extern long sceKernelGetDirectMemorySize(void) __attribute__((weak));
extern int sceKernelAllocateDirectMemory(long start, long end, size_t len, size_t align, int type, long *phys) __attribute__((weak));
extern int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, long phys, size_t align) __attribute__((weak));
extern int sceKernelReleaseDirectMemory(long phys, size_t len) __attribute__((weak));
static void om_report(void);
static volatile int om_note;                                   // 1 = log the first chunk's source, 2 = log a failure (done after the lock is released)
static volatile unsigned long om_note_len; static volatile int om_note_src, om_note_rc;
static unsigned char *om_raw(unsigned long len, unsigned long align, long *phys) {
    *phys = -1;
    if (sceKernelAllocateDirectMemory && sceKernelMapDirectMemory && sceKernelGetDirectMemorySize) {
        long tot = sceKernelGetDirectMemorySize(), ph = 0; int r = sceKernelAllocateDirectMemory(0, tot, len, align, 0, &ph);
        if (r == 0) {
            void *a = 0; int r2 = sceKernelMapDirectMemory(&a, len, 3, 0, ph, align);
            if (r2 == 0 && a && !((unsigned long)a & (align - 1))) { *phys = ph; om_note_src = 1; return (unsigned char *)a; }
            if (r2 == 0 && a) munmap(a, len);
            if (sceKernelReleaseDirectMemory) sceKernelReleaseDirectMemory(ph, len);
            om_note_rc = r2 ? r2 : -2;
        } else om_note_rc = r;
    }
    unsigned char *m = (unsigned char *)mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == (unsigned char *)MAP_FAILED) return 0;
    if ((unsigned long)m & (align - 1)) {   // not aligned: map len + align and give back the unaligned head / tail
        munmap(m, len);
        m = (unsigned char *)mmap(0, len + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == (unsigned char *)MAP_FAILED) return 0;
        unsigned char *a = (unsigned char *)(((unsigned long)m + align - 1) & ~(align - 1));
        if (a > m) munmap(m, (unsigned long)(a - m));
        unsigned char *tail = a + len, *mend = m + len + align; if (mend > tail) munmap(tail, (unsigned long)(mend - tail));
        m = a;
    }
    om_note_src = 2; return m;
}
extern void *__real_malloc(size_t) __attribute__((weak));
extern void __real_free(void *) __attribute__((weak));
extern void *__real_realloc(void *, size_t) __attribute__((weak));

static volatile int om_lk;
static unsigned char *om_cur, *om_end;
static void *om_freel[128];
static unsigned char om_map[1u << 18];                       // 1 bit per 64 MB region of a 47-bit address space
static struct { unsigned char *base; unsigned long len; long phys; } om_dtab[OM_DEDIC];
static volatile int om_dlive;                                // dedicated mappings alive (almost always 0)
volatile unsigned long om_used, om_peak, om_chunks, om_dedic_n, om_fail, om_foreign;   // statistics for the timeline

static inline void om_lock(void) { int n = 0; while (__atomic_exchange_n(&om_lk, 1, __ATOMIC_ACQUIRE)) { if (++n > 100) { sched_yield(); n = 0; } } }
static inline void om_unlock(void) { __atomic_store_n(&om_lk, 0, __ATOMIC_RELEASE); }
static inline int om_region(unsigned long a) { unsigned long r = a >> 26; return (om_map[r >> 3] >> (r & 7)) & 1; }

// block size and class index of a payload request (sz <= OM_BIG)
static unsigned long om_bs(unsigned long sz, int *idx) {
    if (sz == 0) sz = 1;
    if (sz <= 1024) { unsigned long c = (sz + 15) >> 4; *idx = (int)c - 1; return c << 4; }
    unsigned long n = sz - 1; int hb = 63 - __builtin_clzl(n); unsigned long step = 1ul << (hb - 2), bs = (n / step + 1) * step;
    int hb2 = 63 - __builtin_clzl(bs - 1); *idx = 64 + (hb2 - 10) * 4 + (int)(((bs - 1) >> (hb2 - 2)) & 3);
    return bs;
}
static unsigned long om_bs_of_idx(int idx) {
    if (idx < 64) return (unsigned long)(idx + 1) << 4;
    int j = idx - 64, hb2 = 10 + j / 4, q = j % 4; return (unsigned long)(4 + q + 1) << (hb2 - 2);
}
static inline unsigned long *om_hdr(const void *p) { return (unsigned long *)((char *)p - 16); }

static unsigned char *om_new_chunk(void) {   // 64 MB, 64 MB-aligned (direct memory is mapped with that alignment)
    long ph; unsigned char *m = om_raw(OM_CHUNK, OM_CHUNK, &ph);
    if (!m) { om_note = 2; om_note_len = OM_CHUNK; return 0; }
    if (!om_chunks) om_note = 1;
    return m;
}

static void *om_dedic_alloc(unsigned long sz) {   // bigger than OM_BIG: its own mapping, payload 64 bytes in
    unsigned long len = (sz + 64 + OM_PAGE - 1) & ~(OM_PAGE - 1);
    long ph; unsigned char *m = om_raw(len, OM_PAGE, &ph);
    if (!m) { om_note = 2; om_note_len = len; om_report(); return 0; }
    int slot = -1; om_lock();
    for (int i = 0; i < OM_DEDIC; i++) if (!om_dtab[i].base) { slot = i; om_dtab[i].base = m; om_dtab[i].len = len; om_dtab[i].phys = ph; break; }
    if (slot >= 0) { om_dlive++; om_dedic_n++; om_used += len; if (om_used > om_peak) om_peak = om_used; }
    om_unlock();
    if (slot < 0) { munmap(m, len); if (ph >= 0 && sceKernelReleaseDirectMemory) sceKernelReleaseDirectMemory(ph, len); return 0; }
    unsigned long *h = om_hdr(m + 64); h[0] = OM_TAG ^ (unsigned long)(m + 64); h[1] = 255;
    return m + 64;
}
static int om_dedic_find(unsigned long a, unsigned char **base, unsigned long *len, int *slot) {   // caller holds the lock
    for (int i = 0; i < OM_DEDIC; i++) if (om_dtab[i].base && a >= (unsigned long)om_dtab[i].base && a < (unsigned long)om_dtab[i].base + om_dtab[i].len) { *base = om_dtab[i].base; *len = om_dtab[i].len; *slot = i; return 1; }
    return 0;
}
// 1 if p belongs to a dedicated mapping (and returns its extent), 0 if not
static int om_is_dedic(unsigned long a, unsigned long *usable) {
    if (!__atomic_load_n(&om_dlive, __ATOMIC_ACQUIRE)) return 0;
    unsigned char *b; unsigned long l; int s, r; om_lock(); r = om_dedic_find(a, &b, &l, &s); om_unlock();
    if (r && usable) *usable = (unsigned long)(b + l) - a;
    return r;
}

static void om_report(void) {
    int n = om_note; if (!n || !__sync_bool_compare_and_swap(&om_note, n, 0)) return;
    static int fails; if (n == 1) OM_LOG("om: first chunk from %s (64 MB, aligned)", om_note_src == 1 ? "DIRECT memory" : "mmap (flexible)");
    else if (fails++ < 5) OM_LOG("om: chunk of %lu MB FAILED (direct rc 0x%x, mmap errno %d): falling back to the libc heap", (unsigned long)om_note_len >> 20, (unsigned)om_note_rc, errno);
}
static void *om_alloc(size_t sz0) {
    unsigned long sz = sz0;
    if (sz > OM_BIG) return om_dedic_alloc(sz);
    int idx; unsigned long bs = om_bs(sz, &idx), raw = bs + 16; void *p;
    om_lock();
    p = om_freel[idx];
    if (p) om_freel[idx] = *(void **)p;
    else {
        if ((unsigned long)(om_end - om_cur) < raw) {
            unsigned char *c = om_new_chunk();
            if (!c) { om_fail++; om_unlock(); om_report(); return 0; }
            unsigned long r = (unsigned long)c >> 26; om_map[r >> 3] |= (unsigned char)(1u << (r & 7));
            om_cur = c; om_end = c + OM_CHUNK; om_chunks++;
        }
        unsigned char *b = om_cur; om_cur += raw; p = b + 16;
        unsigned long *h = (unsigned long *)b; h[0] = OM_TAG ^ (unsigned long)p; h[1] = (unsigned long)idx;
    }
    om_used += bs; if (om_used > om_peak) om_peak = om_used;
    om_unlock();
    if (om_note) om_report();
    return p;
}
static void *om_payload(void *p) { unsigned long *h = om_hdr(p); return (h[1] & OM_ALIGNED) ? (char *)p - (h[1] >> 32) : p; }   // start of the block an aligned pointer lives in
static unsigned long om_usable(void *p) {   // bytes usable from p
    unsigned long a = (unsigned long)p;
    if (om_region(a)) { void *b = om_payload(p); unsigned long bs = om_bs_of_idx((int)(om_hdr(b)[1] & 0xff)); return bs - (unsigned long)((char *)p - (char *)b); }
    unsigned long u = 0; om_is_dedic(a, &u); return u;
}
static void om_release(void *p) {
    unsigned long a = (unsigned long)p;
    if (om_region(a)) {
        void *b = om_payload(p); int idx = (int)(om_hdr(b)[1] & 0xff); unsigned long bs = om_bs_of_idx(idx);
        om_lock(); *(void **)b = om_freel[idx]; om_freel[idx] = b; om_used -= bs; om_unlock();
        return;
    }
    om_lock();
    unsigned char *base; unsigned long len; int slot;
    if (om_dedic_find(a, &base, &len, &slot)) { long ph = om_dtab[slot].phys; om_dtab[slot].base = 0; om_dtab[slot].len = 0; om_dlive--; om_used -= len; om_unlock(); munmap(base, len); if (ph >= 0 && sceKernelReleaseDirectMemory) sceKernelReleaseDirectMemory(ph, len); return; }
    om_unlock();
}
static int om_ours(void *p) { unsigned long a = (unsigned long)p; return om_region(a) || om_is_dedic(a, 0); }

static void *om_aligned(size_t al, size_t sz) {
    if (al <= 16) return om_alloc(sz);
    void *b = om_alloc(sz + al + 16); if (!b) return 0;
    unsigned long q = ((unsigned long)b + 16 + al - 1) & ~(unsigned long)(al - 1);
    unsigned long *h = om_hdr((void *)q); h[0] = OM_TAG ^ q; h[1] = OM_ALIGNED | ((q - (unsigned long)b) << 32);
    return (void *)q;
}

void *__wrap_malloc(size_t n) { void *p = om_alloc(n); if (p) return p; return __real_malloc ? __real_malloc(n) : 0; }
void *__wrap_calloc(size_t a, size_t b) {
    if (b && a > (size_t)-1 / b) { errno = ENOMEM; return 0; }
    size_t n = a * b; void *p = om_alloc(n); if (p) { memset(p, 0, n); return p; }
    p = __real_malloc ? __real_malloc(n) : 0; if (p) memset(p, 0, n); return p;
}
void __wrap_free(void *p) {
    if (!p) return;
    if (om_ours(p)) { om_release(p); return; }
    om_foreign++; if (__real_free) __real_free(p);
}
void *__wrap_realloc(void *p, size_t n) {
    if (!p) return __wrap_malloc(n);
    if (!om_ours(p)) return __real_realloc ? __real_realloc(p, n) : 0;
    if (n == 0) n = 1;
    unsigned long u = om_usable(p); if (n <= u && n > u / 2) return p;   // fits, and not wastefully
    void *q = __wrap_malloc(n); if (!q) return 0;
    memcpy(q, p, u < n ? u : n); om_release(p); return q;
}
size_t __wrap_malloc_usable_size(void *p) {
    if (!p) return 0;
    if (om_ours(p)) return om_usable(p);
    return 0;   /* the libc's own block: its size is not known here (malloc_usable_size is deliberately not referenced: a symbol the console libc does not export makes the link refuse the title) */
}
int __wrap_posix_memalign(void **mp, size_t al, size_t sz) {
    if (al < sizeof(void *) || (al & (al - 1))) return EINVAL;
    void *p = om_aligned(al, sz); if (!p) return ENOMEM;
    *mp = p; return 0;
}
void *__wrap_memalign(size_t al, size_t sz) { if (al & (al - 1)) { errno = EINVAL; return 0; } void *p = om_aligned(al, sz); if (!p) errno = ENOMEM; return p; }
void *__wrap_aligned_alloc(size_t al, size_t sz) { return __wrap_memalign(al, sz); }

// C++ operator new / delete: the console libc's own operator new takes its memory from the libc's private heap, not through malloc, so every C++ allocation of HotSpot / Mesa (ACO) / Zink-side C++ would drain the pool this file exists to protect.
// vk_relink.sh turns each __wrap_X defined here into --wrap=X, so the (mangled) names below redirect every undefined reference to them. Throwing forms abort() when memory is truly gone (no exception can be thrown from C); nothrow forms return 0.
// delete of a pointer that is not ours goes to the libc's free through __wrap_free (the libc's operator new is malloc-backed on this platform when it is used by a system library, and those pointers never cross to us).
volatile unsigned long om_new_n;                               // operator new calls served (timeline statistic: nonzero = the wrap is hit)
#include <stdlib.h>
static void *om_new(size_t n, size_t al, int nothrow) {
    __sync_fetch_and_add(&om_new_n, 1);
    if (!n) n = 1;
    void *p = al > 16 ? om_aligned(al, n) : __wrap_malloc(n);
    if (!p && !nothrow) abort();
    return p;
}
void *__wrap__Znwm(size_t n) { return om_new(n, 0, 0); }
void *__wrap__Znam(size_t n) { return om_new(n, 0, 0); }
void *__wrap__ZnwmRKSt9nothrow_t(size_t n, const void *t) { (void)t; return om_new(n, 0, 1); }
void *__wrap__ZnamRKSt9nothrow_t(size_t n, const void *t) { (void)t; return om_new(n, 0, 1); }
void *__wrap__ZnwmSt11align_val_t(size_t n, size_t al) { return om_new(n, al, 0); }
void *__wrap__ZnamSt11align_val_t(size_t n, size_t al) { return om_new(n, al, 0); }
void *__wrap__ZnwmSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, const void *t) { (void)t; return om_new(n, al, 1); }
void *__wrap__ZnamSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, const void *t) { (void)t; return om_new(n, al, 1); }
void __wrap__ZdlPv(void *p) { __wrap_free(p); }
void __wrap__ZdaPv(void *p) { __wrap_free(p); }
void __wrap__ZdlPvm(void *p, size_t n) { (void)n; __wrap_free(p); }
void __wrap__ZdaPvm(void *p, size_t n) { (void)n; __wrap_free(p); }
void __wrap__ZdlPvRKSt9nothrow_t(void *p, const void *t) { (void)t; __wrap_free(p); }
void __wrap__ZdaPvRKSt9nothrow_t(void *p, const void *t) { (void)t; __wrap_free(p); }
void __wrap__ZdlPvSt11align_val_t(void *p, size_t al) { (void)al; __wrap_free(p); }
void __wrap__ZdaPvSt11align_val_t(void *p, size_t al) { (void)al; __wrap_free(p); }
void __wrap__ZdlPvmSt11align_val_t(void *p, size_t n, size_t al) { (void)n; (void)al; __wrap_free(p); }
void __wrap__ZdaPvmSt11align_val_t(void *p, size_t n, size_t al) { (void)n; (void)al; __wrap_free(p); }
void __wrap__ZdlPvSt11align_val_tRKSt9nothrow_t(void *p, size_t al, const void *t) { (void)al; (void)t; __wrap_free(p); }
void __wrap__ZdaPvSt11align_val_tRKSt9nothrow_t(void *p, size_t al, const void *t) { (void)al; (void)t; __wrap_free(p); }
