#define PS5_MDBENCH 1
#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { write(1, m, strlen(m)); write(1, "\n", 1); }
int scePadReadState(int h, PadState *s) { return -1; } int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}
static int ids = 100, ndraw, nmulti, nind, nbv_calls, bad;
static unsigned gen(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = ++ids; return 0; }
static void f_Gen(int n, unsigned *o) { gen(n, o); }
static unsigned f_CreateShader(int t) { return ++ids; } static void f_Ok(unsigned a, int p, int *v) { *v = 1; } static void f_ShaderSource(unsigned s, int n, const char *const *p, const int *l) {}
static void f_Nop(void) {} static unsigned f_CreateProgram(void) { return ++ids; } static int f_GetUniformLocation(unsigned p, const char *n) { return 3; }
static void f_DrawElements(int m, int c, int t, const void *i) { ndraw++; if (c != 3000 || t != 0x1403) bad++; }
static void f_MDBV(int m, const int *c, int t, const void *const *i, int n, const int *b) { nmulti++; if (n != 500 || c[499] != 3000 || b[499] != 499 * 2000 || t != 0x1403) bad++; }
static void f_MDI(int m, int t, const void *i, int n, int s) { nind++; if (n != 500 || t != 0x1403) bad++; }
static unsigned f_GetError(void) { return 0; }
static void *anyf = (void *)f_Nop;
void *eglGetProcAddress(const char *n) {
    if (!strncmp(n, "glGen", 5)) return (void *)f_Gen; if (!strcmp(n, "glCreateShader")) return (void *)f_CreateShader; if (!strcmp(n, "glGetShaderiv") || !strcmp(n, "glGetProgramiv")) return (void *)f_Ok;
    if (!strcmp(n, "glShaderSource")) return (void *)f_ShaderSource; if (!strcmp(n, "glCreateProgram")) return (void *)f_CreateProgram; if (!strcmp(n, "glGetUniformLocation")) return (void *)f_GetUniformLocation;
    if (!strcmp(n, "glDrawElements")) return (void *)f_DrawElements; if (!strcmp(n, "glMultiDrawElementsBaseVertex")) return (void *)f_MDBV; if (!strcmp(n, "glMultiDrawElementsIndirect")) return (void *)f_MDI;
    if (!strcmp(n, "glGetError")) return (void *)f_GetError; return anyf; }
int main(void) { md_bench(); dprintf(1, "stub run: individual draws=%d (expect 500*24=12000) multi=%d (24) indirect=%d (24) bad-args=%d\n", ndraw, nmulti, nind, bad); return bad || ndraw != 12000 || nmulti != 24 || nind != 24; }
