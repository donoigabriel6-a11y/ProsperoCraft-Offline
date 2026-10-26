#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { write(1, m, strlen(m)); write(1, "\n", 1); }
int scePadReadState(int h, PadState *s) { return -1; } int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}
static char *g_src; static char g_names[16][32]; static int g_nn; static long g_ip[6], g_p[6];
static long r_ShaderSource(long s, long n, long c, long d) { const char *const *st = (const char *const *)c; size_t t = 0; for (int i = 0; i < n; i++) t += strlen(st[i]); g_src = malloc(t + 1); g_src[0] = 0; for (int i = 0; i < n; i++) strcat(g_src, st[i]); return 0; }
static long r_BindAttrib(long p, long i, long name) { snprintf(g_names[g_nn++], 32, "%ld:%s", i, (const char *)name); return 0; }
static long r_VAI(long a, long b, long c, long d, long e) { g_ip[0] = a; g_ip[1] = b; g_ip[2] = c; g_ip[3] = d; g_ip[4] = e; return 0; }
static long r_VA(long a, long b, long c, long d, long e, long f) { g_p[0] = a; g_p[1] = b; g_p[2] = c; g_p[3] = d; g_p[4] = e; g_p[5] = f; return 0; }
void *eglGetProcAddress(const char *n) {
    if (!strcmp(n, "glShaderSource")) return (void *)r_ShaderSource; if (!strcmp(n, "glBindAttribLocation")) return (void *)r_BindAttrib;
    if (!strcmp(n, "glVertexAttribIPointer")) return (void *)r_VAI; if (!strcmp(n, "glVertexAttribPointer")) return (void *)r_VA;
    return (void *)r_VA; }
static char *slurp(const char *f) { FILE *fp = fopen(f, "rb"); fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp); char *b = malloc(n + 1); fread(b, 1, n, fp); b[n] = 0; fclose(fp); return b; }
int main(int argc, char **argv) {
    typedef long (*f10)(long, long, long, long, long, long, long, long, long, long);
    f10 shsrc = (f10)gl_proc("glShaderSource"), bind = (f10)gl_proc("glBindAttribLocation"), vai = (f10)gl_proc("glVertexAttribIPointer"), va = (f10)gl_proc("glVertexAttribPointer");
    for (int k = 1; k < argc; k++) { char *s = slurp(argv[k]); const char *one = s; shsrc(7, 1, (long)&one, 0, 0, 0, 0, 0, 0, 0);
        char path[64]; snprintf(path, sizeof path, "%s.out", argv[k]); FILE *o = fopen(path, "w"); fputs(g_src, o); fclose(o); }
    bind(1, 3, (long)"UV2", 0, 0, 0, 0, 0, 0, 0); bind(1, 4, (long)"Position", 0, 0, 0, 0, 0, 0, 0);
    dprintf(1, "bind calls:"); for (int i = 0; i < g_nn; i++) dprintf(1, " %s", g_names[i]); dprintf(1, "\n");
    vai(3, 2, 0x1402, 32, 24, 0, 0, 0, 0, 0); dprintf(1, "IPointer(2xSHORT) -> size=%ld type=0x%lx stride=%ld\n", g_ip[1], g_ip[2], g_ip[3]);
    vai(3, 4, 0x1401, 32, 24, 0, 0, 0, 0, 0); dprintf(1, "IPointer(4xUBYTE) untouched -> size=%ld type=0x%lx\n", g_ip[1], g_ip[2]);
    va(5, 3, 0x1400, 1, 32, 28, 0, 0, 0, 0); dprintf(1, "Pointer(3xBYTE norm) -> size=%ld\n", g_p[1]);
    va(5, 3, 0x1400, 0, 32, 28, 0, 0, 0, 0); dprintf(1, "Pointer(3xBYTE not normalized) untouched -> size=%ld\n", g_p[1]);
    va(0, 3, 0x1406, 0, 32, 0, 0, 0, 0, 0); dprintf(1, "Pointer(3xFLOAT) untouched -> size=%ld\n", g_p[1]);
    return 0; }
