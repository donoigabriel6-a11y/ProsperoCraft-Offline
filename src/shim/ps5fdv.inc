// ps5fdv.inc - virtual file descriptors for .jar/.zip files (included by ps5lwjgl.c).
// This console lets a process hold only ~264 descriptors (open() fails with EMFILE; getrlimit is faked by the JDK shim). A Fabric pack of 50 mods keeps 2-3 per jar open
// (class loader + zip file system + nested jars) and ran out ("Too many open files", then the launcher could not even read its own files).
// The first FDV_FREE jars stay ordinary descriptors (nothing changes for small setups). Further read-only .jar/.zip files get a virtual number (FDV_BASE..) and only FDV_RES of
// them hold a real descriptor at a time: the least recently used one is closed (offset remembered) and reopened by path on its next read/pread/lseek/fstat.
// open() that fails with EMFILE also evicts one and retries. Linked in through --wrap=open/pread/lseek/fstat (+ the read/readv/close/fcntl wrappers in ps5lwjgl.c).
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#ifndef FDV_FREE
#define FDV_FREE 90      // jars that stay real descriptors
#endif
#ifndef FDV_RES
#define FDV_RES 56       // virtual jars with a real descriptor at the same time
#endif
#define FDV_BASE 8192
#define FDV_MAX 4096
#ifndef PS5_HAVE_VPATH   /* ps5lwjgl.c defines vpath (relative paths against the game folder); the stand-alone test does not */
static const char *vpath(const char *p, char *b, size_t n) { (void)b; (void)n; return p; }
#endif
extern int __real_open(const char *, int, int);
extern long __real_pread(int, void *, size_t, long);
extern long __real_lseek(int, long, int);
extern int __real_fstat(int, struct stat *);
extern int __real_close(int);
extern int __real_dup2(int, int);
extern long __real_read(int, void *, size_t);
extern long __real_readv(int, const struct iovec *, int);
extern int __real_fcntl(int, int, long);
typedef struct { char *path; int real; long pos, last; int busy, used; } fdv_ent;
static fdv_ent g_fdv[FDV_MAX];
static volatile int g_fdv_lk, g_fdv_res, g_fdv_jars, g_fdv_used;
static long g_fdv_clock; static unsigned char g_fdv_isjar[4096];
static int g_fdv_ev, g_fdv_reopen, g_fdv_virt;   // counters for the log
#define FDV_LK() while (__sync_lock_test_and_set(&g_fdv_lk, 1)) sched_yield()
#define FDV_UNLK() __sync_lock_release(&g_fdv_lk)
#define FDV_IS(fd) ((unsigned)((fd) - FDV_BASE) < (unsigned)FDV_MAX)
static int fdv_jarname(const char *p, int flags) {
    if (!p || (flags & 3) != 0 || (flags & (O_CREAT | O_TRUNC | O_APPEND))) return 0;
    size_t n = strlen(p); if (n < 5) return 0; const char *e = p + n - 4;
    return (e[0] == '.' && (e[1] == 'j' || e[1] == 'J') && (e[2] == 'a' || e[2] == 'A') && (e[3] == 'r' || e[3] == 'R')) || (e[0] == '.' && (e[1] == 'z' || e[1] == 'Z') && (e[2] == 'i' || e[2] == 'I') && (e[3] == 'p' || e[3] == 'P'));
}
static int fdv_evict_lru_locked(void) {   // closes the real descriptor of the least recently used idle virtual file
    int best = -1; long bl = 0;
    for (int i = 0; i < FDV_MAX; i++) { fdv_ent *e = &g_fdv[i]; if (e->used && e->real >= 0 && e->busy == 0 && (best < 0 || e->last < bl)) { best = i; bl = e->last; } }
    if (best < 0) return 0;
    fdv_ent *e = &g_fdv[best]; long p = __real_lseek(e->real, 0, SEEK_CUR); if (p >= 0) e->pos = p;
    __real_close(e->real); e->real = -1; g_fdv_res--; g_fdv_ev++; return 1;
}
static int fdv_evict_one(void) { FDV_LK(); int r = fdv_evict_lru_locked(); FDV_UNLK(); return r; }
static int fdv_acquire(int fd) {   // real descriptor behind a virtual one (reopened when it was parked); pair with fdv_release
    FDV_LK(); fdv_ent *e = &g_fdv[fd - FDV_BASE];
    if (!e->used) { FDV_UNLK(); errno = EBADF; return -1; }
    e->busy++; e->last = ++g_fdv_clock;
    if (e->real < 0) {
        while (g_fdv_res >= FDV_RES && fdv_evict_lru_locked()) {}
        int r = __real_open(e->path, O_RDONLY, 0);
        if (r < 0 && errno == EMFILE && fdv_evict_lru_locked()) r = __real_open(e->path, O_RDONLY, 0);
        if (r < 0) { int er = errno; e->busy--; FDV_UNLK(); errno = er; return -1; }
        if (e->pos > 0) __real_lseek(r, e->pos, SEEK_SET);
        e->real = r; g_fdv_res++; g_fdv_reopen++;
    }
    int r = e->real; FDV_UNLK(); return r;
}
static void fdv_release(int fd) { FDV_LK(); g_fdv[fd - FDV_BASE].busy--; FDV_UNLK(); }
static int fdv_close(int fd) {   // 0 or -1/EBADF
    FDV_LK(); fdv_ent *e = &g_fdv[fd - FDV_BASE];
    if (!e->used) { FDV_UNLK(); errno = EBADF; return -1; }
    int r = e->real; char *p = e->path; if (r >= 0) g_fdv_res--; e->real = -1; e->path = 0; e->used = 0; g_fdv_used--; FDV_UNLK();
    if (r >= 0) __real_close(r);
    free(p); return 0;
}
static void fdv_forget_real(int fd) { if ((unsigned)fd < sizeof g_fdv_isjar && g_fdv_isjar[fd]) { g_fdv_isjar[fd] = 0; __sync_fetch_and_sub(&g_fdv_jars, 1); } }
int __wrap_open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    char vb_[1100]; path = vpath(path, vb_, sizeof vb_);   // a relative path is redone against the game folder (ps5_set_vcwd)
    int fd = __real_open(path, flags, mode);
    if (fd < 0 && errno == EMFILE && fdv_evict_one()) fd = __real_open(path, flags, mode);
    if (fd < 0 || !fdv_jarname(path, flags)) return fd;
    if (g_fdv_jars < FDV_FREE && (unsigned)fd < sizeof g_fdv_isjar) { g_fdv_isjar[fd] = 1; __sync_fetch_and_add(&g_fdv_jars, 1); return fd; }
    char *cp = strdup(path); if (!cp) return fd;
    FDV_LK();
    int slot = -1; for (int i = 0; i < FDV_MAX; i++) if (!g_fdv[i].used) { slot = i; break; }
    if (slot < 0) { FDV_UNLK(); free(cp); return fd; }   // table full: stays a real descriptor
    fdv_ent *e = &g_fdv[slot]; e->path = cp; e->real = fd; e->pos = 0; e->busy = 0; e->used = 1; e->last = ++g_fdv_clock; g_fdv_res++; g_fdv_used++; g_fdv_virt++;
    while (g_fdv_res > FDV_RES && fdv_evict_lru_locked()) {}
    FDV_UNLK();
    return FDV_BASE + slot;
}
extern void ps5_wb_flush(void) __attribute__((weak));   // small-write coalescing (ps5lwjgl.c): pending bytes go out before anything that looks at the file
extern void ps5_io_note(int, long) __attribute__((weak));   // I/O timing (ps5lwjgl.c)
#define FDV_WB() do { if (ps5_wb_flush) ps5_wb_flush(); } while (0)
extern void ps5_wb_flush_for(int) __attribute__((weak));   // same, but only when the pending bytes belong to the file behind this descriptor (reads / seeks of other files leave them alone)
#define FDV_WBF(fd) do { if (ps5_wb_flush_for) ps5_wb_flush_for(fd); else FDV_WB(); } while (0)
long __wrap_pread(int fd, void *b, size_t n, long o) {
    FDV_WBF(fd);
    if (!FDV_IS(fd)) { if (!ps5_io_note) return __real_pread(fd, b, n, o); struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); long r = __real_pread(fd, b, n, o); clock_gettime(CLOCK_MONOTONIC, &t1); ps5_io_note(3, (long)((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec))); return r; }
    int r = fdv_acquire(fd); if (r < 0) return -1; long x = __real_pread(r, b, n, o); int er = errno; fdv_release(fd); errno = er; return x;
}
long __wrap_lseek(int fd, long o, int w) {
    FDV_WBF(fd);
    if (!FDV_IS(fd)) { if (!ps5_io_note) return __real_lseek(fd, o, w); struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); long r = __real_lseek(fd, o, w); clock_gettime(CLOCK_MONOTONIC, &t1); ps5_io_note(2, (long)((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec))); return r; }
    int r = fdv_acquire(fd); if (r < 0) return -1; long x = __real_lseek(r, o, w); int er = errno; fdv_release(fd); errno = er; return x;
}
int __wrap_fstat(int fd, struct stat *st) {
    FDV_WBF(fd);
    if (!FDV_IS(fd)) return __real_fstat(fd, st);
    int r = fdv_acquire(fd); if (r < 0) return -1; int x = __real_fstat(r, st); int er = errno; fdv_release(fd); errno = er; return x;
}
long fdv_read(int fd, void *b, size_t n) { int r = fdv_acquire(fd); if (r < 0) return -1; long x = __real_read(r, b, n); int er = errno; fdv_release(fd); errno = er; return x; }
long fdv_readv(int fd, const struct iovec *v, int c) { int r = fdv_acquire(fd); if (r < 0) return -1; long x = __real_readv(r, v, c); int er = errno; fdv_release(fd); errno = er; return x; }
int fdv_fcntl(int fd, int cmd, long arg) { int r = fdv_acquire(fd); if (r < 0) return -1; int x = __real_fcntl(r, cmd, arg); int er = errno; fdv_release(fd); errno = er; return x; }
int __wrap_dup2(int oldfd, int newfd) {   // JDK 8's FileChannel.close does dup2(preCloseFD, fd) first: for a virtual descriptor nothing is pending, the close that follows frees it
    FDV_WB();
    if (FDV_IS(newfd)) return newfd;
    return __real_dup2(oldfd, newfd);
}
