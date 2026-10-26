#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { write(1, m, strlen(m)); write(1, "\n", 1); }
int scePadReadState(int h, PadState *s) { return -1; } int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}
/* ---- fake GL state machine ---- */
static struct { int dfbo, rfbo, vp[4], prog, vao, abuf, tex2d; unsigned char cm[4]; int en[4]; int bf[4]; int draws, lastn, uploads; long uploaded; int nprog, nvao, nbuf, nsh; } G;
static int ix(int c) { return c == 0x0BE2 ? 0 : c == 0x0B71 ? 1 : c == 0x0B44 ? 2 : c == 0x0C11 ? 3 : -1; }
static unsigned f_CreateShader(int t) { return ++G.nsh; } static void f_ShaderSource(unsigned s, int n, const char *const *p, const int *l) {} static void f_CompileShader(unsigned s) {}
static void f_GetShaderiv(unsigned s, int p, int *v) { *v = 1; } static void f_GetShaderInfoLog(unsigned s, int n, int *l, char *b) { b[0] = 0; }
static unsigned f_CreateProgram(void) { return ++G.nprog; } static void f_AttachShader(unsigned p, unsigned s) {} static void f_BindAttribLocation(unsigned p, unsigned i, const char *n) {}
static void f_LinkProgram(unsigned p) {} static void f_GetProgramiv(unsigned p, int q, int *v) { *v = 1; } static int f_GetUniformLocation(unsigned p, const char *n) { return 3; }
static int n_query;
static void f_GetIntegerv(int p, int *v) { n_query++; switch (p) { case 0x8CA6: *v = G.dfbo; break; case 0x8CAA: *v = G.rfbo; break; case 0x0BA2: memcpy(v, G.vp, 16); break; case 0x8B8D: *v = G.prog; break; case 0x85B5: *v = G.vao; break; case 0x8894: *v = G.abuf; break; case 0x8069: *v = G.tex2d; break;
    case 0x80C9: *v = G.bf[0]; break; case 0x80C8: *v = G.bf[1]; break; case 0x80CB: *v = G.bf[2]; break; case 0x80CA: *v = G.bf[3]; break; default: *v = 0; } }
static void f_GetBooleanv(int p, unsigned char *v) { memcpy(v, G.cm, 4); }
static unsigned char f_IsEnabled(int c) { n_query++; return G.en[ix(c)]; } static void f_Enable(int c) { G.en[ix(c)] = 1; } static void f_Disable(int c) { G.en[ix(c)] = 0; }
static void f_GenVertexArrays(int n, unsigned *o) { o[0] = ++G.nvao; } static void f_GenBuffers(int n, unsigned *o) { o[0] = ++G.nbuf; }
static void f_BindVertexArray(unsigned v) { G.vao = v; } static void f_BindBuffer(int t, unsigned b) { G.abuf = b; }
static void f_BufferData(int t, long sz, const void *d, int u) {} static void f_BufferSubData(int t, long off, long sz, const void *d) { G.uploads++; G.uploaded = sz; }
static void f_EnableVertexAttribArray(unsigned i) {} static void f_VertexAttribPointer(unsigned i, int n, int t, int nn, int st, const void *p) {}
static void f_BindFramebuffer(int t, unsigned f) { if (t == 0x8D40) { G.dfbo = G.rfbo = f; } else if (t == 0x8CA9) G.dfbo = f; else if (t == 0x8CA8) G.rfbo = f; }
static void f_Viewport(int x, int y, int w, int h) { G.vp[0] = x; G.vp[1] = y; G.vp[2] = w; G.vp[3] = h; } static void f_ColorMask(int r, int g, int b, int a) { G.cm[0] = r; G.cm[1] = g; G.cm[2] = b; G.cm[3] = a; }
static void f_BlendFuncSeparate(int a, int b, int c, int d) { G.bf[0] = a; G.bf[1] = b; G.bf[2] = c; G.bf[3] = d; }
static void f_UseProgram(unsigned p) { G.prog = p; } static void f_Uniform2f(int l, float a, float b) {}
static int bad_draw; static void f_DrawArrays(int m, int f, int c) { G.draws++; G.lastn = c; if (G.dfbo != 0 || G.en[1] || G.en[2] || G.en[3] || !G.en[0] || G.cm[0] != 1 || G.vp[2] != 1920 || G.bf[0] != 0x0302 || G.bf[1] != 0x0303) bad_draw++; }
static void f_ReadPixels(int x, int y, int w, int h, int fmt, int ty, void *p) { ((unsigned char *)p)[0] = 10; ((unsigned char *)p)[1] = 20; ((unsigned char *)p)[2] = 30; ((unsigned char *)p)[3] = 255; }
static void f_BindTexture(int t, unsigned id) { G.tex2d = id; }
static unsigned f_GetError(void) { return 0; }
static void f_GetTexLevelParameteriv(int t, int l, int p, int *v) { *v = p == 0x1000 ? 256 : p == 0x1001 ? 256 : 0x8058; } static void f_GetTexParameteriv(int t, int p, int *v) { *v = 0x2600; }
static const struct { const char *n; void *f; } T[] = {
#define E(x) { "gl" #x, (void *)f_##x }
E(CreateShader), E(ShaderSource), E(CompileShader), E(GetShaderiv), E(GetShaderInfoLog), E(CreateProgram), E(AttachShader), E(BindAttribLocation), E(LinkProgram), E(GetProgramiv), E(GetUniformLocation),
E(GetIntegerv), E(GetBooleanv), E(IsEnabled), E(Enable), E(Disable), E(GenVertexArrays), E(GenBuffers), E(BindVertexArray), E(BindBuffer), E(BufferData), E(BufferSubData), E(EnableVertexAttribArray), E(VertexAttribPointer),
E(BindFramebuffer), E(Viewport), E(ColorMask), E(BlendFuncSeparate), E(UseProgram), E(Uniform2f), E(DrawArrays), E(ReadPixels), E(BindTexture), E(GetTexLevelParameteriv), E(GetTexParameteriv), E(GetError), { 0, 0 } };
void *eglGetProcAddress(const char *n) { for (int i = 0; T[i].n; i++) if (!strcmp(T[i].n, n)) return T[i].f; dprintf(1, "MISSING GL FUNCTION %s\n", n); return 0; }
static int same(typeof(G) *a) { return G.dfbo == a->dfbo && G.rfbo == a->rfbo && !memcmp(G.vp, a->vp, 16) && G.prog == a->prog && G.vao == a->vao && G.abuf == a->abuf && !memcmp(G.cm, a->cm, 4) && !memcmp(G.en, a->en, sizeof G.en) && !memcmp(G.bf, a->bf, 16) && G.tex2d == a->tex2d; }
int main(void) {
    int fails = 0; g_w = 1920; g_h = 1080;
    for (int variant = 0; variant < 4; variant++) {
        memset(&G, 0, sizeof G); G.dfbo = 7; G.rfbo = 9; G.vp[2] = 640; G.vp[3] = 360; G.prog = 42; G.vao = 5; G.abuf = 3; G.tex2d = 11; G.cm[0] = 1; G.cm[1] = 0; G.cm[2] = 1; G.cm[3] = 1;
        /* set the state the way Minecraft does: through the functions gl_proc hands out, so the shim's shadow sees every change */
        {   typedef void (*v1)(int); typedef void (*v2)(int, unsigned); typedef void (*v4i)(int, int, int, int);
            v1 en = (v1)gl_proc("glEnable"), di = (v1)gl_proc("glDisable");
            v2 bfb = (v2)gl_proc("glBindFramebuffer"), bb = (v2)gl_proc("glBindBuffer");
            v4i vpf = (v4i)gl_proc("glViewport"), cmf = (v4i)gl_proc("glColorMask"), bfs = (v4i)gl_proc("glBlendFuncSeparate");
            void (*up)(unsigned) = (void (*)(unsigned))gl_proc("glUseProgram");
            void (*bva)(unsigned) = (void (*)(unsigned))gl_proc("glBindVertexArray");
            (variant & 1) ? en(0x0BE2) : di(0x0BE2);
            en(0x0B71);
            ((variant >> 1) & 1) ? en(0x0B44) : di(0x0B44);
            en(0x0C11);
            bfs(0x0302, 0x0001, 0x0001, 0x0303);
            cmf(1, 1, variant & 1, 1);
            bfb(0x8D40, 7u + (unsigned)variant); vpf(0, 0, 640 + variant, 360);
            up(40u + (unsigned)variant); bva(50u + (unsigned)variant); bb(0x8892, 60u + (unsigned)variant);
        }
        n_query = 0; typeof(G) before = G; g_osk_open = variant != 1; g_cur_vis = (variant != 2); g_osk_top = variant & 1;
        for (int k = 0; k < 3; k++) cursor_draw();
        dprintf(1, "variant %d: queries=%d osk=%d draws=%d vertices=%d (limit %d) upload=%ldKB state restored=%s bad draw states=%d\n", variant, n_query, g_osk_open, G.draws, G.lastn, OV_MAX, G.uploaded / 1024, same(&before) ? "YES" : "NO", bad_draw);
        if (!same(&before) || G.draws != 3 || bad_draw || G.lastn >= OV_MAX) fails++;
    }
    for (int pos = 0; pos < 6; pos++) {   /* stats overlay alone (in game: no cursor, no keyboard), every corner */
        typeof(G) before = G; g_osk_open = 0; g_cur_vis = 0; g_ov_menu = 0; g_hud_on = 1; g_hud_pos = pos; G.draws = 0;
        cursor_draw();
        float minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9; for (int i = 0; i < g_ov_n; i++) { if (g_ov[i].x < minx) minx = g_ov[i].x; if (g_ov[i].x > maxx) maxx = g_ov[i].x; if (g_ov[i].y < miny) miny = g_ov[i].y; if (g_ov[i].y > maxy) maxy = g_ov[i].y; }
        int inside = minx >= 0 && miny >= 0 && maxx <= g_w && maxy <= g_h;
        dprintf(1, "hud pos %d: draws=%d vertices=%d box %.0f,%.0f..%.0f,%.0f inside=%d restored=%s\n", pos, G.draws, g_ov_n, minx, miny, maxx, maxy, inside, same(&before) ? "YES" : "NO");
        if (G.draws != 1 || g_ov_n < 100 || !inside || !same(&before)) fails++;
    }
    g_hud_on = 0; g_ov_menu = 1;
    g_vp_w = 1920; g_vp_h = 1080; g_cur_fbo = 1; G.tex2d = 11; typeof(G) b2 = G; cap_probe(1, 42); cap_probe(6, 42); cap_probe(12, 43);
    dprintf(1, "cap_probe leaves texture binding: %s\n", same(&b2) ? "unchanged" : "CHANGED"); if (!same(&b2)) fails++;
    dprintf(1, "fails=%d\n", fails); return fails;
}
