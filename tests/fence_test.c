// sandbox test of the GL sync emulation in ps5lwjgl.c (gw_fence_sync / gw_client_wait / gw_get_synciv ...): a fence is a swap-count stamp; nothing is reported done early
#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { (void)m; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) { (void)x; }
static int fails; static int g_fin_calls;
#define CHECK(c, ...) do { if (!(c)) { dprintf(1, "FAIL line %d: ", __LINE__); dprintf(1, __VA_ARGS__); dprintf(1, "\n"); fails++; } } while (0)
static void fake_finish(void) { g_fin_calls++; }
void *eglGetProcAddress(const char *n) { (void)n; return (void *)fake_finish; }
int main(void) {
    typedef long (*fn)(long, long, long, long, long, long, long, long, long, long);
    fn mk = (fn)fs_lookup("glFenceSync"), cw = (fn)fs_lookup("glClientWaitSync"), del = (fn)fs_lookup("glDeleteSync"), iss = (fn)fs_lookup("glIsSync"), giv = (fn)fs_lookup("glGetSynciv"), ws = (fn)fs_lookup("glWaitSync");
    CHECK(mk && cw && del && iss && giv && ws && !fs_lookup("glDrawArrays"), "lookup");
    g_swap_n = 10;
    long a = mk(0x9117, 0, 0, 0, 0, 0, 0, 0, 0, 0); CHECK(a && iss(a, 0, 0, 0, 0, 0, 0, 0, 0, 0), "created");
    int st = 0, len = 0;
    giv(a, 0x9114, 1, (long)&len, (long)&st, 0, 0, 0, 0, 0); CHECK(len == 1 && st == 0x9118, "young fence is UNSIGNALED (%x)", st);
    CHECK(cw(a, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0x911B, "poll of a young fence: TIMEOUT_EXPIRED");
    g_swap_n = 12; CHECK(cw(a, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0x911B, "2 swaps old is still not done");
    g_swap_n = 13; CHECK(cw(a, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0x911A, "3 swaps old: ALREADY_SIGNALED");
    giv(a, 0x9114, 1, (long)&len, (long)&st, 0, 0, 0, 0, 0); CHECK(st == 0x9119, "old fence SIGNALED");
    long b = mk(0x9117, 0, 0, 0, 0, 0, 0, 0, 0, 0);          // young, waited with a timeout: one real finish, then done
    CHECK(cw(b, 1, 1000000, 0, 0, 0, 0, 0, 0, 0) == 0x911C, "blocking wait on a young fence: CONDITION_SATISFIED");
    CHECK(g_fs_forced == 1 && g_fin_calls == 1, "one forced finish (%lu, %d calls)", g_fs_forced, g_fin_calls);
    CHECK(cw(b, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0x911A, "signaled after the finish");
    CHECK(cw(0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 0x911D, "null sync: WAIT_FAILED");
    del(a, 0, 0, 0, 0, 0, 0, 0, 0, 0); del(b, 0, 0, 0, 0, 0, 0, 0, 0, 0); del(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    ws(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    dprintf(1, "fails=%d\n", fails); return fails;
}
