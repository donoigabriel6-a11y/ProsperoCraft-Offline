// fdv_test.c: ps5fdv.inc (virtual descriptors for jars) under a low real descriptor limit. Linux, no console needed.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <pthread.h>
#define FDV_FREE 10
#define FDV_RES 20
#include "ps5fdv.inc"
int __real_open(const char *p, int f, int m) { return open(p, f, m); }
long __real_pread(int fd, void *b, size_t n, long o) { return pread(fd, b, n, o); }
long __real_lseek(int fd, long o, int w) { return lseek(fd, o, w); }
int __real_fstat(int fd, struct stat *st) { return fstat(fd, st); }
int __real_close(int fd) { return close(fd); }
int __real_dup2(int a, int b) { return dup2(a, b); }
long __real_read(int fd, void *b, size_t n) { return read(fd, b, n); }
long __real_readv(int fd, const struct iovec *v, int c) { return readv(fd, v, c); }
int __real_fcntl(int fd, int c, long a) { return fcntl(fd, c, a); }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NF 400
#define SZ 1000
static unsigned char at(int i, int k) { return (unsigned char)(i * 7 + k * 3 + k / 256); }
static int tfd[100]; static volatile int tbad;
static void *tw(void *a) { unsigned seed = (unsigned)(long)a; unsigned char b[64]; for (int n = 0; n < 6000; n++) { int i = rand_r(&seed) % 100, off = rand_r(&seed) % (SZ - 64); long r = __wrap_pread(tfd[i], b, 64, off); if (r != 64) { tbad++; continue; } for (int k = 0; k < 64; k++) if (b[k] != at(i, off + k)) { tbad++; break; } } return 0; }
int main(void) {
    struct rlimit rl = { 120, 120 }; CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0, "setrlimit");
    char dir[] = "/tmp/fdvXXXXXX"; CHECK(mkdtemp(dir) != 0, "mkdtemp"); static char path[NF][80]; static int fd[NF]; static long expect[NF];
    for (int i = 0; i < NF; i++) { snprintf(path[i], 80, "%s/m%d.jar", dir, i); FILE *f = fopen(path[i], "wb"); for (int k = 0; k < SZ; k++) fputc(at(i, k), f); fclose(f); }
    // 1. 400 jars with a 120-descriptor limit
    for (int i = 0; i < NF; i++) { fd[i] = __wrap_open(path[i], O_RDONLY); CHECK(fd[i] >= 0, "open %d failed (errno %d)", i, errno); }
    int virt = 0; for (int i = 0; i < NF; i++) virt += FDV_IS(fd[i]);
    CHECK(virt == NF - FDV_FREE, "virtualised %d, expected %d", virt, NF - FDV_FREE);
    CHECK(g_fdv_res <= FDV_RES, "resident %d > %d", g_fdv_res, FDV_RES);
    // 2. random access: pread, lseek+read, sequential read, readv, fstat, fcntl
    srand(7); unsigned char b[200];
    for (int n = 0; n < 20000; n++) {
        int i = rand() % NF, op = rand() % 5, off = rand() % (SZ - 100), len = 1 + rand() % 90;
        if (op == 0) { long r = __wrap_pread(fd[i], b, len, off); CHECK(r == len, "pread %ld", r); for (int k = 0; k < len; k++) CHECK(b[k] == at(i, off + k), "pread data file %d off %d", i, off + k); }
        else if (op == 1) { long p = __wrap_lseek(fd[i], off, SEEK_SET); CHECK(p == off, "lseek %ld", p); long r = FDV_IS(fd[i]) ? fdv_read(fd[i], b, len) : read(fd[i], b, len); CHECK(r == len, "read %ld", r); for (int k = 0; k < len; k++) CHECK(b[k] == at(i, off + k), "read data"); expect[i] = off + len; }
        else if (op == 2) { long p = __wrap_lseek(fd[i], 0, SEEK_CUR); long want = expect[i]; CHECK(p == want, "position of %d is %ld, expected %ld", i, p, want); }
        else if (op == 3) { struct stat st; CHECK(__wrap_fstat(fd[i], &st) == 0 && st.st_size == SZ && S_ISREG(st.st_mode), "fstat"); }
        else { struct iovec v[2] = { { b, 5 }, { b + 5, 7 } }; long p = __wrap_lseek(fd[i], 100, SEEK_SET); long r = FDV_IS(fd[i]) ? fdv_readv(fd[i], v, 2) : readv(fd[i], v, 2); CHECK(p == 100 && r == 12, "readv %ld", r); for (int k = 0; k < 12; k++) CHECK(b[k] == at(i, 100 + k), "readv data"); expect[i] = 112; }
    }
    printf("parked %d, reopened %d, resident now %d\n", g_fdv_ev, g_fdv_reopen, g_fdv_res);
    CHECK(g_fdv_reopen > 1000, "expected plenty of reopens, got %d", g_fdv_reopen);
    // 3. eight threads reading 100 jars at once
    for (int i = 0; i < 100; i++) tfd[i] = fd[i];
    { pthread_t t[8]; for (long j = 0; j < 8; j++) pthread_create(&t[j], 0, tw, (void *)(j + 1)); for (int j = 0; j < 8; j++) pthread_join(t[j], 0); CHECK(tbad == 0, "%d wrong reads under threads", tbad); }
    CHECK(__wrap_dup2(0, fd[100]) == fd[100] && __wrap_lseek(fd[100], 0, SEEK_CUR) >= 0, "dup2 on a virtual descriptor must be a no-op");
    // 4. close everything: nothing left
    for (int i = 0; i < NF; i++) { int r = FDV_IS(fd[i]) ? fdv_close(fd[i]) : close(fd[i]); if (!FDV_IS(fd[i])) fdv_forget_real(fd[i]); CHECK(r == 0, "close %d", i); }
    CHECK(g_fdv_used == 0 && g_fdv_res == 0 && g_fdv_jars == 0, "left over: used %d res %d jars %d", g_fdv_used, g_fdv_res, g_fdv_jars);
    CHECK(fdv_close(FDV_BASE + 5) == -1 && errno == EBADF, "double close");
    // 5. EMFILE on an ordinary open frees a parked-able jar slot and retries
    for (int i = 0; i < 60; i++) fd[i] = __wrap_open(path[i], O_RDONLY);
    int z[200], nz = 0; for (;;) { int d = __wrap_open("/dev/null", O_RDONLY); if (d < 0) break; z[nz++] = d; if (nz >= 200) break; }
    CHECK(nz > 0 && nz < 200, "ordinary opens ended at %d", nz);
    CHECK(g_fdv_res == 0, "all virtual files should be parked now, resident %d", g_fdv_res);
    for (int i = 0; i < nz; i++) close(z[i]);
    for (int i = 10; i < 60; i++) { long r = __wrap_pread(fd[i], b, 4, 20); CHECK(r == 4 && b[0] == at(i, 20), "after parking, file %d", i); }
    for (int i = 0; i < 60; i++) { if (FDV_IS(fd[i])) fdv_close(fd[i]); else { fdv_forget_real(fd[i]); close(fd[i]); } }
    printf("fails=%d\n", fails); return fails != 0;
}
