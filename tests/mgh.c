#define PS5_VFIX 1
#define PS5_MEGA 1
#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) { write(1, m, strlen(m)); write(1, "\n", 1); }
int scePadReadState(int h, PadState *s) { return -1; } int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}

/* ---------------- fake GL: records every REAL call and keeps buffer contents ---------------- */
enum { E_DRAW, E_BINDVAO, E_UNI3, E_UNI1I, E_COPY, E_BUFSUB, E_TEXSUB, E_USEPROG, E_BUFDATA } ;
typedef struct { int kind; long a, b, c, d; float f[3]; } Ev;
static Ev ev[100000]; static int nev;
static void rec(int k, long a, long b, long c, long d) { Ev *e = &ev[nev++]; e->kind = k; e->a = a; e->b = b; e->c = c; e->d = d; }
static int next_id = 1;
typedef struct { unsigned char *data; long size; } FB; static FB fb[4096];
static unsigned f_vao, f_prog, f_arr, f_copy_r, f_copy_w; static unsigned f_ebo_of_vao[4096];
static unsigned char *last_idx; static long last_idx_n;
static void r_GenBuffers(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_id++; }
static void r_GenVAO(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_id++; }
static void r_GenTex(int n, unsigned *o) { for (int i = 0; i < n; i++) o[i] = next_id++; }
static void r_UseProgram(unsigned p) { f_prog = p; rec(E_USEPROG, p, 0, 0, 0); }
static void r_BindVAO(unsigned v) { f_vao = v; rec(E_BINDVAO, v, 0, 0, 0); }
static unsigned f_texbuf_bound, f_pbuf; static void r_BindBuffer(int t, unsigned id) { if (t == 0x8892) f_arr = id; else if (t == 0x8893) f_ebo_of_vao[f_vao] = id; else if (t == 0x8F36) f_copy_r = id; else if (t == 0x8F37) f_copy_w = id; else if (t == 0x8C2A) f_texbuf_bound = id; }
static void r_TexBuffer(int target, int fmt, unsigned buf) { f_pbuf = buf; }
static void r_BufferData(int t, long size, const void *data, int u) { unsigned id = t == 0x8892 ? f_arr : t == 0x8893 ? f_ebo_of_vao[f_vao] : t == 0x8C2A ? f_texbuf_bound : 0; if (!id) return; free(fb[id].data); fb[id].data = (unsigned char *)calloc(1, (size_t)size + 1); fb[id].size = size; if (data) memcpy(fb[id].data, data, (size_t)size); rec(E_BUFDATA, id, size, 0, 0); }
static void r_BufferSubData(int t, long off, long size, const void *data) { unsigned id = t == 0x8893 ? f_ebo_of_vao[f_vao] : t == 0x8C2A ? f_texbuf_bound : f_arr; if (id && fb[id].data && off + size <= fb[id].size) memcpy(fb[id].data + off, data, (size_t)size); if (t == 0x8893) { free(last_idx); last_idx = (unsigned char *)malloc((size_t)size); memcpy(last_idx, data, (size_t)size); last_idx_n = size / 4; } rec(E_BUFSUB, t, off, size, 0); }
static int corrupt_next_copy; static void r_Copy(int rt, int wt, long ro, long wo, long size) { memcpy(fb[f_copy_w].data + wo, fb[f_copy_r].data + ro, (size_t)size); if (corrupt_next_copy) { fb[f_copy_w].data[wo + size / 2] ^= 0x55; corrupt_next_copy = 0; } rec(E_COPY, f_copy_r, f_copy_w, wo, size); }
static void r_GetBufSub(int t, long off, long size, void *out) { memcpy(out, fb[f_copy_r].data + off, (size_t)size); }
static float g_ptab[64][256 * 256 * 4]; static unsigned f_tex7, f_tex_cur; static int f_unit; static int g_mode;
#define g_pagetex ((float *)fb[f_pbuf].data)
static int g_last_rows; static long g_texels;
static int n_partial_row;
static void r_TexSub(int t, int l, int x, int y, int w, int h, int fmt, int ty, const void *p) {
    n_partial_row++; return;   /* on this console TexSubImage2D into the offset table loses earlier writes: the harness forbids it entirely */
    if (0) {
    const float *src = (const float *)p; float *dst = g_ptab[f_tex7 % 64];
    for (int r = 0; r < h; r++) memcpy(dst + (size_t)((y + r) * 256 + x) * 4, src + (size_t)r * w * 4, (size_t)w * 16);
    g_texels += (long)w * h; g_last_rows = h; rec(E_TEXSUB, f_tex7, 0, 0, 0); } }
static void r_ActTex(int u) { f_unit = u - 0x84C0; }
static void r_BindTex(int t, unsigned id) { f_tex_cur = id; if (f_unit == 7) f_tex7 = id; }
static int n_fence, n_fence_del, n_wait; static int g_wait_result = 0x911A; static long fence_ids[1000];
static void *r_FenceSync(int c, int f) { n_fence++; return (void *)(long)(1000 + n_fence); }
static int r_ClientWait(void *s, int f, unsigned long long t) { n_wait++; return g_wait_result; }
static void r_DelSync(void *s) { n_fence_del++; }
static float g_base[3]; static int g_base_set;
static void r_Uni3(int loc, int c, const float *v) { if (loc == 13) { memcpy(g_base, v, 12); g_base_set = 1; return; } Ev *e = &ev[nev++]; e->kind = E_UNI3; e->a = loc; memcpy(e->f, v, 12); }
static void r_Uni1i(int loc, int v) { rec(E_UNI1I, loc, v, f_prog, 0); if (loc == 11) g_mode = v; }
static unsigned f_err = 0; static unsigned r_GetError(void) { unsigned e = f_err; f_err = 0; return e; }
static float last_draw_offsets_ok; 
static unsigned g_draw_ebo[100000], g_draw_tex7[100000];
/* GPU-lag model: a merged draw is 'executed' by the GPU HAZ_LAG draws after it was issued, reading the buffers as they are THEN. Its signature (index list, page-table texels and arena bytes it depends on) must be unchanged. */
static int haz_lag = -1; static unsigned long haz_checks, haz_bad; typedef struct { unsigned ebo, tex; long first, count; uint64_t sig; int due; } Haz; static Haz haz_q[64]; static int haz_n, haz_clock;
static uint64_t haz_sig(unsigned ebo, unsigned tex, long first, long count) {
    uint64_t h = 1469598103934665603ULL; unsigned *ix = (unsigned *)fb[ebo].data + first; static unsigned char seen[MG_PAGES]; memset(seen, 0, sizeof seen);
    for (long i = 0; i < count; i++) { h = (h ^ ix[i]) * 1099511628211ULL; unsigned pg = ix[i] / 128; if (!seen[pg]) { seen[pg] = 1; float *w = (float *)fb[f_pbuf].data + (size_t)pg * 4; for (int k = 0; k < 3; k++) { unsigned u; memcpy(&u, &w[k], 4); h = (h ^ u) * 1099511628211ULL; } for (int b = 0; b < 4096; b += 8) { uint64_t v; memcpy(&v, fb[g_mg_arena].data + (long)pg * 4096 + b, 8); h = (h ^ v) * 1099511628211ULL; } } }
    return h;
}
static void haz_step(void) {   /* called at every draw: execute due draws */
    haz_clock++; for (int i = 0; i < haz_n;) { if (haz_q[i].due <= haz_clock) { haz_checks++; if (haz_sig(haz_q[i].ebo, haz_q[i].tex, haz_q[i].first, haz_q[i].count) != haz_q[i].sig) haz_bad++; haz_q[i] = haz_q[--haz_n]; } else i++; } }

static long g_draw_off[100000];
static void r_Draw(int mode, int count, int type, const void *idx) { g_draw_ebo[nev] = f_ebo_of_vao[f_vao]; g_draw_tex7[nev] = f_tex7; g_draw_off[nev] = (long)idx; rec(E_DRAW, count, type, f_vao, g_mode);
    if (haz_lag >= 0) { haz_step(); if (type == 0x1405 && g_mode == 1 && haz_n < 64) { Haz *z = &haz_q[haz_n++]; z->ebo = f_ebo_of_vao[f_vao]; z->tex = f_tex7; z->first = (long)idx / 4; z->count = count; z->sig = haz_sig(z->ebo, z->tex, z->first, count); z->due = haz_clock + haz_lag; } } }
static void nop0(void) {} static void nop1(int a) {} static void nop2(int a, unsigned b) {} static void nopS(unsigned a, unsigned b) {}
static void r_TexImg(int a, int b, int c, int d, int e, int f, int g, int h, const void *p) {} static void r_TexPar(int a, int b, int c) {}
static void r_VAP(unsigned i, int s, int t, unsigned char n, int st, const void *p) { rec(100, i, s, t, (long)p); } static void r_VAIP(unsigned i, int s, int t, int st, const void *p) { rec(101, i, s, t, (long)p); }
static void r_EnVAA(unsigned i) {} static void r_DelB(int n, const unsigned *ids) { for (int i = 0; i < n; i++) { free(fb[ids[i]].data); fb[ids[i]].data = 0; } } static void r_DelV(int n, const unsigned *ids) {}
static int g_uloc_off = 7, g_uloc_mode = 11, g_uloc_pages = 12;
static int r_GetUloc(unsigned p, const char *name) { if (!strcmp(name, "ChunkOffset")) return g_uloc_off; if (!strcmp(name, "ArenaMode_")) return g_uloc_mode; if (!strcmp(name, "ChunkPages_")) return g_uloc_pages; if (!strcmp(name, "ArenaBase_")) return 13; return 3; }
static void r_Attach(unsigned p, unsigned s) {} static void r_Link(unsigned p) {} static void r_Enable(int c) {} static void r_Disable(int c) {}
static long r_ShaderSource(long s, long n, long c, long d) { return 0; }
static const char *g_last_src;
static void r_ShaderSource2(unsigned s, int n, const char *const *st, const int *ln) { static char buf[20000]; buf[0] = 0; for (int i = 0; i < n; i++) strcat(buf, st[i]); g_last_src = buf; }
static int n_finish; static void r_Finish(void) { n_finish++; }
static void r_GetTexImage(int t, int l, int f, int ty, void *out) { memcpy(out, g_ptab[f_tex7 % 64], (size_t)256 * MG_ROWS * 16); }
static void r_Viewport(int a, int b, int c, int d) {} static void r_DepthMask(unsigned char m) {} static void r_DrawArrays(int a, int b, int c) { rec(E_DRAW, -1, 0, 0, 0); }
void *eglGetProcAddress(const char *n) {
#define M(name, fn) if (!strcmp(n, name)) return (void *)fn
    M("glGenBuffers", r_GenBuffers); M("glGenVertexArrays", r_GenVAO); M("glGenTextures", r_GenTex); M("glUseProgram", r_UseProgram); M("glBindVertexArray", r_BindVAO); M("glBindBuffer", r_BindBuffer);
    M("glBufferData", r_BufferData); M("glBufferSubData", r_BufferSubData); M("glCopyBufferSubData", r_Copy); M("glGetBufferSubData", r_GetBufSub); M("glTexSubImage2D", r_TexSub);
    M("glUniform3fv", r_Uni3); M("glUniform1i", r_Uni1i); M("glGetError", r_GetError); M("glDrawElements", r_Draw); M("glTexImage2D", r_TexImg); M("glTexParameteri", r_TexPar);
    M("glVertexAttribPointer", r_VAP); M("glVertexAttribIPointer", r_VAIP); M("glEnableVertexAttribArray", r_EnVAA); M("glDisableVertexAttribArray", r_EnVAA); M("glDeleteBuffers", r_DelB); M("glDeleteVertexArrays", r_DelV);
    M("glGetUniformLocation", r_GetUloc); M("glAttachShader", r_Attach); M("glLinkProgram", r_Link); M("glActiveTexture", r_ActTex); M("glEnable", r_Enable); M("glDisable", r_Disable);
    M("glBindFramebuffer", nop2); M("glBindTexture", r_BindTex); M("glFenceSync", r_FenceSync); M("glClientWaitSync", r_ClientWait); M("glDeleteSync", r_DelSync); M("glClear", nop1); M("glDrawArrays", r_DrawArrays); M("glViewport", r_Viewport); M("glDepthMask", r_DepthMask); M("glShaderSource", r_ShaderSource2);
    M("glBindAttribLocation", nop2); M("glFinish", r_Finish); M("glGetTexImage", r_GetTexImage); M("glTexBuffer", r_TexBuffer);
    return (void *)nop0; }
typedef long (*f10)(long, long, long, long, long, long, long, long, long, long);
#define GP(n) gl_proc(n)
static void (*gDrawElements)(int, int, int, const void *); static void (*gBindVAO)(unsigned); static void (*gUni3)(int, int, const float *); static void (*gUse)(unsigned);
static void (*gBufData)(int, long, const void *, int); static void (*gBindBuf)(int, unsigned); static void (*gVAP)(unsigned, int, int, unsigned char, int, const void *); static void (*gVAIP)(unsigned, int, int, int, const void *);
static void (*gEnVAA)(unsigned); static int (*gGetUloc)(unsigned, const char *); static void (*gAttach)(unsigned, unsigned); static void (*gLink)(unsigned); static void (*gEnable)(int); static void (*gDisable)(int);
static void (*gGenBuf)(int, unsigned *); static void (*gGenVAO)(int, unsigned *); static void (*gDelBuf)(int, const unsigned *); static f10 gShSrc;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { dprintf(1, "FAIL line %d: %s  ", __LINE__, #c); dprintf(1, __VA_ARGS__); dprintf(1, "\n"); fails++; } } while (0)
static int count_ev(int k) { int n = 0; for (int i = 0; i < nev; i++) n += ev[i].kind == k; return n; }
static int count_draw_big(void) { int n = 0; for (int i = 0; i < nev; i++) n += ev[i].kind == E_DRAW && ev[i].b == 0x1405; return n; }

#define NCH 20
static unsigned vbo[NCH], vao[NCH]; static int quads[NCH]; static float offs[NCH][3]; static unsigned ebo16, ebo_bad;
static void make_chunk(int i, int q, float x) {
    quads[i] = q; offs[i][0] = x; offs[i][1] = 2.0f * i; offs[i][2] = -5.0f;
    gGenBuf(1, &vbo[i]); gGenVAO(1, &vao[i]);
    gBindVAO(vao[i]); gBindBuf(0x8892, vbo[i]);
    unsigned char *data = (unsigned char *)malloc((size_t)q * 4 * 32); for (long k = 0; k < (long)q * 4 * 32; k++) data[k] = (unsigned char)(i * 31 + k);
    gBufData(0x8892, (long)q * 4 * 32, data, 0x88E4); free(data);
    for (unsigned a = 0; a < 5; a++) gEnVAA(a);
    gVAP(0, 3, 0x1406, 0, 32, (const void *)0); gVAP(1, 4, 0x1401, 1, 32, (const void *)12); gVAP(2, 2, 0x1406, 0, 32, (const void *)16);
    gVAIP(3, 2, 0x1402, 32, (const void *)24);            /* two shorts: VFIX turns it into one uint */
    gVAP(4, 3, 0x1400, 1, 32, (const void *)28);          /* 3-byte normal: VFIX turns it into 4 */
    gBindBuf(0x8893, ebo16);
}
static void draw_layer(const unsigned *list, int n, unsigned prog, int zero_after) {
    gUse(prog);
    for (int k = 0; k < n; k++) { int i = list[k]; gUni3(7, 1, offs[i]); gBindVAO(vao[i]); gDrawElements(4, quads[i] * 6, 0x1403, 0); }
    if (zero_after) { float z[3] = { 0, 0, 0 }; gUni3(7, 1, z); }
    gUse(0); gBindVAO(0);
}
int main(void) {
    g_created = 1;
    gDrawElements = (void *)GP("glDrawElements"); gBindVAO = (void *)GP("glBindVertexArray"); gUni3 = (void *)GP("glUniform3fv"); gUse = (void *)GP("glUseProgram");
    gBufData = (void *)GP("glBufferData"); gBindBuf = (void *)GP("glBindBuffer"); gVAP = (void *)GP("glVertexAttribPointer"); gVAIP = (void *)GP("glVertexAttribIPointer"); gEnVAA = (void *)GP("glEnableVertexAttribArray");
    gGetUloc = (void *)GP("glGetUniformLocation"); gAttach = (void *)GP("glAttachShader"); gLink = (void *)GP("glLinkProgram"); gEnable = (void *)GP("glEnable"); gDisable = (void *)GP("glDisable");
    gGenBuf = (void *)GP("glGenBuffers"); gGenVAO = (void *)GP("glGenVertexArrays"); gDelBuf = (void *)GP("glDeleteBuffers"); gShSrc = (f10)GP("glShaderSource");
    /* ---- shader: VFIX + chunk rewrite ---- */
    const char *vsh = "#version 150\n\nin vec3 Position;\nin vec4 Color;\nin vec2 UV0;\nin ivec2 UV2;\nin vec3 Normal;\n\nuniform mat4 ModelViewMat;\nuniform mat4 ProjMat;\nuniform vec3 ChunkOffset;\n\nvoid main() {\n    vec3 pos = Position + ChunkOffset;\n    gl_Position = ProjMat * ModelViewMat * vec4(pos, 1.0);\n    ivec2 u = UV2;\n}\n";
    const char *one = vsh; gShSrc(5, 1, (long)&one, 0, 0, 0, 0, 0, 0, 0);
    CHECK(strstr(g_last_src, "ArenaMode_") && strstr(g_last_src, "#define ChunkOffset ChunkOffsetEff_()") && strstr(g_last_src, "in uint UV2_raw_;"), "shader rewrite missing");
    { const char *p = strstr(g_last_src, "uniform vec3 ChunkOffset;"); CHECK(p && strstr(g_last_src, "Position + ChunkOffset;") > p, "declaration must precede the macro and the use"); }
    const char *other = "#version 150\nin vec3 Position;\nvoid main() { gl_Position = vec4(Position, 1.0); }\n"; one = other; gShSrc(6, 1, (long)&one, 0, 0, 0, 0, 0, 0, 0);
    /* ---- programs: 10 = chunk program (solid), 20 = translucent chunk program (same shader), 30 = some other program ---- */
    gAttach(10, 5); gLink(10); gGetUloc(10, "ChunkOffset"); gAttach(20, 5); gLink(20); gGetUloc(20, "ChunkOffset"); gAttach(30, 6); gLink(30);
    /* ---- EBO: shared sequential quad index buffer (16-bit), created while a chunk VAO is bound, as Minecraft does ---- */
    gGenBuf(1, &ebo16); gGenBuf(1, &ebo_bad);
    for (int i = 0; i < NCH; i++) make_chunk(i, 100 + 37 * i, 16.0f * i);
    { gBindVAO(vao[0]); gBindBuf(0x8893, ebo16); static unsigned short idx[6 * 2000]; for (int q = 0; q < 2000; q++) { static const int pat[6] = { 0, 1, 2, 2, 3, 0 }; for (int k = 0; k < 6; k++) idx[q * 6 + k] = (unsigned short)(q * 4 + pat[k]); } gBufData(0x8893, sizeof idx, idx, 0x88E4); gBindVAO(0); }
    nev = 0;
    /* helper: find the (single) merged draw of the current event log and decode its list from the EBO and page table the DRAW really used */
    #define MERGED_DRAW_INDEX() ({ int _d = -1; for (int i = 0; i < nev; i++) if (ev[i].kind == E_DRAW && ev[i].b == 0x1405) _d = i; _d; })
    /* ================= frame 1: 12 visible chunks, solid program ================= */
    unsigned list1[12]; for (int k = 0; k < 12; k++) list1[k] = (unsigned)k;
    draw_layer(list1, 12, 10, 1);
    int ndraw = count_ev(E_DRAW), nbig = count_draw_big(); long total = 0; for (int k = 0; k < 12; k++) total += quads[list1[k]] * 6L;
#if MEGA_IDXCACHE
    CHECK(nbig >= 1 && nbig == ndraw, "every real draw must be a run draw (%d draws, %d merged)", ndraw, nbig);
    CHECK(count_ev(E_COPY) == 12, "12 arena copies expected on the first frame, got %d", count_ev(E_COPY));
    {   /* every visible chunk's quads must be covered exactly once by the run draws, with correct indices and offsets, and nothing else may be drawn */
        unsigned *ix = (unsigned *)fb[f_ebo_of_vao[f_vao] ? f_ebo_of_vao[f_vao] : 0].data; (void)ix;
        int covered[NCH]; memset(covered, 0, sizeof covered); int bad = 0; long drawn_real = 0;
        for (int i = 0; i < nev; i++) {
            if (ev[i].kind != E_DRAW || ev[i].b != 0x1405) continue;
            unsigned *ib = (unsigned *)fb[g_draw_ebo[i]].data; long first = g_draw_off[i] / 4, cnt = ev[i].a;
            for (long p = 0; p < cnt; p += 6) {
                unsigned a0 = ib[first + p], a1 = ib[first + p + 1], a2 = ib[first + p + 2];
                if (a0 == a1 && a1 == a2) continue;                      /* degenerate filler */
                drawn_real += 6;
                int page = (int)(a0 / 128), found = -1;
                for (int c = 0; c < NCH; c++) { MgBuf *b = mg_buf(vbo[c]); if (b && b->valid && page >= b->page && page < b->page + b->pages) { found = c; break; } }
                if (found < 0) { bad++; continue; }
                covered[found]++;
                float *pt = (float *)fb[f_pbuf].data + (size_t)page * 4;
                if (fabsf(g_base[0] + pt[0] - offs[found][0]) > 0.01f || fabsf(g_base[1] + pt[1] - offs[found][1]) > 0.01f) bad++;
                if (a1 != a0 + 1 || a2 != a0 + 2) bad++;
            }
        }
        CHECK(bad == 0, "%d bad quads in the run draws", bad);
        CHECK(drawn_real == total, "the run draws must cover exactly the visible quads (%ld of %ld indices)", drawn_real, total);
        for (int k = 0; k < 12; k++) CHECK(covered[list1[k]] == quads[list1[k]], "chunk %d: %d of %d quads drawn", k, covered[list1[k]], quads[list1[k]]);
        MgBuf *b = mg_buf(vbo[3]); CHECK(b && b->valid && !memcmp(fb[g_mg_arena].data + (long)b->page * 128 * 32, fb[vbo[3]].data, (size_t)quads[3] * 128), "arena copy of chunk 3 differs");
    }
#else
    CHECK(ndraw == 1 && nbig == 1, "expected ONE real draw (INT indices), got %d draws, %d INT", ndraw, nbig);
    { int di = MERGED_DRAW_INDEX(); CHECK(di >= 0 && ev[di].a == total, "draw count %ld expected %ld", di >= 0 ? ev[di].a : -1, total); }
    CHECK(count_ev(E_COPY) == 12, "12 arena copies expected on the first frame, got %d", count_ev(E_COPY));
#endif
    { int on = -1, off_after = -1, dlast = MERGED_DRAW_INDEX(), dfirst = -1;
      for (int i = 0; i < nev; i++) { if (ev[i].kind == E_DRAW && ev[i].b == 0x1405 && dfirst < 0) dfirst = i; if (ev[i].kind == E_UNI1I && ev[i].a == 11 && ev[i].b == 1) on = i; }
      for (int i = dlast; i < nev; i++) if (ev[i].kind == E_UNI1I && ev[i].a == 11 && ev[i].b == 0) { off_after = i; break; }
      CHECK(on >= 0 && on < dfirst && off_after > dlast, "ArenaMode_ must be 1 before the first run draw and 0 after the last (on=%d first=%d last=%d off=%d)", on, dfirst, dlast, off_after); }
    CHECK(f_vao == 0 && f_prog == 0, "final real state: vao %u prog %u expected 0 0", f_vao, f_prog);
#ifdef MEGA_FENCE
    CHECK(n_fence >= 1, "a fence must follow every merged draw (%d created)", n_fence);
#else
    CHECK(n_fence == 0 && n_wait == 0, "default build must not use fences (%d fences, %d waits)", n_fence, n_wait);
#endif
#if MEGA_IDXCACHE
    /* with the persistent index buffer there is no per-frame list: a redraw of the same chunks must upload nothing at all, and a changed visible set must not upload either */
    { nev = 0; draw_layer(list1, 12, 10, 1); int up = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB || ev[i].kind == E_BUFDATA) up++;
      CHECK(up == 0 && count_ev(E_COPY) == 0, "a repeated frame must upload nothing (%d uploads, %d copies)", up, count_ev(E_COPY)); }
    { nev = 0; for (int i = 0; i < NCH; i++) offs[i][0] += 0.5f; unsigned l3[12]; for (int k = 0; k < 11; k++) l3[k] = (unsigned)k; l3[11] = 12; draw_layer(l3, 12, 10, 0);
      int up = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB) up++;
      CHECK(up <= 2, "a changed visible set may only upload the newly placed chunk (%d uploads)", up);
      long drawn = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_DRAW && ev[i].b == 0x1405) { unsigned *ib = (unsigned *)fb[g_draw_ebo[i]].data; long first = g_draw_off[i] / 4;
          for (long p = 0; p < ev[i].a; p += 6) if (!(ib[first + p] == ib[first + p + 1] && ib[first + p + 1] == ib[first + p + 2])) drawn += 6; }
      long want = 0; for (int k = 0; k < 12; k++) want += quads[l3[k]] * 6L;
      CHECK(drawn == want, "changed set: %ld indices drawn, %ld expected", drawn, want); }
#else
    int di = MERGED_DRAW_INDEX(); unsigned ebo_frame1 = g_draw_ebo[di], tex_frame1 = g_draw_tex7[di];
    /* ================= frame 2: identical visible set and offsets (e.g. paused) -> NO new index upload, the list is reused, texture slot rotates ================= */
    nev = 0; draw_layer(list1, 12, 10, 1); di = MERGED_DRAW_INDEX();
    CHECK(count_ev(E_COPY) == 0 && count_draw_big() == 1 && count_ev(E_DRAW) == 1, "frame 2: copies %d draws %d", count_ev(E_COPY), count_ev(E_DRAW));
    { int idxup = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8893) idxup++; CHECK(idxup == 0, "unchanged list must not re-upload indices (%d uploads)", idxup); }
    CHECK(g_draw_ebo[di] == ebo_frame1, "unchanged list must draw from the same index buffer (%u vs %u)", g_draw_ebo[di], ebo_frame1);
    CHECK(f_pbuf != 0, "the offset table buffer must exist");
    { unsigned *ix = (unsigned *)fb[g_draw_ebo[di]].data; float *pt = (float *)fb[f_pbuf].data; int bad = 0; long n = 0; for (int k = 0; k < 12; k++) { int i = (int)list1[k]; unsigned pb = ix[n]; if (fabsf(g_base[0] + pt[(pb / 128) * 4] - offs[i][0]) > 0.01f) bad++; n += quads[i] * 6; } CHECK(bad == 0, "reused list: wrong page table (%d)", bad); }
    /* ================= frame 2b: same list but the camera moved (offsets changed) -> still no index upload, new offsets in the texture ================= */
    nev = 0; for (int i = 0; i < NCH; i++) offs[i][0] += 0.5f; draw_layer(list1, 12, 10, 1); di = MERGED_DRAW_INDEX();
    { int idxup = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8893) idxup++; CHECK(idxup == 0, "moved camera, same list: no index upload expected (%d)", idxup); }
    { unsigned *ix = (unsigned *)fb[g_draw_ebo[di]].data; float *pt = (float *)fb[f_pbuf].data; int bad = 0; long n = 0; for (int k = 0; k < 12; k++) { int i = (int)list1[k]; unsigned pb = ix[n]; if (fabsf(g_base[0] + pt[(pb / 128) * 4] - offs[i][0]) > 0.01f) bad++; n += quads[i] * 6; } CHECK(bad == 0, "moved camera: stale offsets (%d)", bad); }
    /* ================= frame 3: different visible set (drops chunk 11, adds chunk 12) -> new list in the NEXT index slot ================= */
    nev = 0; { unsigned l3[12]; for (int k = 0; k < 11; k++) l3[k] = (unsigned)k; l3[11] = 12; draw_layer(l3, 12, 10, 0); di = MERGED_DRAW_INDEX();
      CHECK(g_draw_ebo[di] != ebo_frame1, "a changed list must use another index slot"); int idxup = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8893) idxup++; CHECK(idxup == 1, "changed list: one index upload expected (%d)", idxup); }
#endif
    /* ================= frame 3b: chunk 2 re-uploaded by Minecraft (new size) ================= */
    nev = 0;
    gBindVAO(vao[2]); gBindBuf(0x8892, vbo[2]); { unsigned char *d = (unsigned char *)calloc(1, 60 * 128); gBufData(0x8892, 60 * 128, d, 0x88E4); free(d); quads[2] = 60; } gBindVAO(0);
    draw_layer(list1, 12, 10, 0);
    CHECK(count_ev(E_COPY) == 1 && count_draw_big() >= 1, "expected 1 re-copy, got %d (big draws %d)", count_ev(E_COPY), count_draw_big());
    /* ================= quarantine: pages freed by a re-upload are not handed out again right away ================= */
    { MgBuf *b2 = mg_buf(vbo[2]); int old_page = -1; (void)old_page; int p_before = b2->page;
      /* chunk 2 again re-uploaded with the same size: its old pages go to quarantine, the new slot must differ */
      gBindVAO(vao[2]); gBindBuf(0x8892, vbo[2]); { unsigned char *d = (unsigned char *)calloc(1, 60 * 128); gBufData(0x8892, 60 * 128, d, 0x88E4); free(d); } gBindVAO(0);
      draw_layer(list1, 12, 10, 0); b2 = mg_buf(vbo[2]); CHECK(b2->page != p_before, "pages of a re-uploaded chunk were reused immediately (page %d)", b2->page); }
    /* ================= controller toggle: merging OFF -> individual draws with correct offsets, ON -> merged again ================= */
    nev = 0; mg_toggle(); mg_toggle(); mg_toggle(); CHECK(g_mg_user_off == 1, "three presses from level 1 must reach OFF"); nev = 0; draw_layer(list1, 12, 10, 1);
    CHECK(count_ev(E_DRAW) == 12 && count_draw_big() == 0, "toggled off: %d draws, %d merged (expect 12, 0)", count_ev(E_DRAW), count_draw_big());
    { int k = 0, bad = 0; float last[3] = { 0, 0, 0 }; for (int i = 0; i < nev; i++) { if (ev[i].kind == E_UNI3) memcpy(last, ev[i].f, 12); if (ev[i].kind == E_DRAW) { int c = (int)list1[k++]; if (last[0] != offs[c][0] || last[1] != offs[c][1]) bad++; } } CHECK(bad == 0 && k == 12, "toggled off: %d wrong offsets (k=%d)", bad, k); }
    nev = 0; mg_toggle(); draw_layer(list1, 12, 10, 1); CHECK(count_draw_big() >= 1 && count_draw_big() == count_ev(E_DRAW), "toggled back on: %d draws, %d merged", count_ev(E_DRAW), count_draw_big());
    /* a camera move alone must not touch the page table any more */
    { long t0 = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8C2A) t0++; for (int i = 0; i < NCH; i++) offs[i][0] += 0.37f; nev = 0; draw_layer(list1, 12, 10, 1); long t1 = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8C2A) t1++; (void)t0; CHECK(t1 == 0, "a camera move must not touch the offset table (%ld uploads)", t1); }
    /* ================= GPU-lag hazard simulation: random camera, visible sets and chunk re-uploads over many frames, two chunk programs ================= */
    { const char *lagenv = getenv("HAZARD_LAG"), *expect = getenv("HAZARD_EXPECT"); int lag = lagenv ? atoi(lagenv) : 10;
      gAttach(40, 5); gLink(40); gGetUloc(40, "ChunkOffset");
      haz_lag = lag; haz_checks = haz_bad = 0; haz_n = 0; unsigned seed = 12345; unsigned vis[NCH];
      for (int it = 0; it < 400; it++) {
          seed = seed * 1103515245u + 12345u; int nv = 6 + (int)((seed >> 8) % 12); if (nv > NCH) nv = NCH;
          for (int k = 0; k < nv; k++) vis[k] = (unsigned)((seed >> 4) % 3 == 0 ? (k + (seed >> 12) % 3) % NCH : k);
          for (int i = 0; i < NCH; i++) offs[i][0] += 0.01f * (float)(seed % 7);                       /* camera moves every frame */
          if (it % 3 == 0) { int c = (int)((seed >> 16) % NCH); int q = 30 + (int)((seed >> 20) % 150); gBindVAO(vao[c]); gBindBuf(0x8892, vbo[c]); unsigned char *d = (unsigned char *)malloc((size_t)q * 128); for (long k = 0; k < (long)q * 128; k++) d[k] = (unsigned char)(it + k); gBufData(0x8892, (long)q * 128, d, 0x88E4); free(d); quads[c] = q; gBindVAO(0); }   /* a chunk is rebuilt */
          draw_layer(vis, nv, it % 2 ? 10 : 40, 0);
      }
      haz_lag = -1;
      dprintf(1, "hazard simulation (GPU lag %d draws, ring %d): %lu merged draws checked, %lu read changed data\n", lag, MG_RING, haz_checks, haz_bad);
      if (!expect) CHECK(haz_bad == 0 && haz_checks > 100, "a draw executed %d draws late saw changed data (%lu of %lu)", lag, haz_bad, haz_checks);
      else CHECK(haz_bad > 0, "the hazard simulation must be able to detect an undersized ring (0 mismatches)");
    }
#ifdef MEGA_VERIFY
    /* ================= verify mode: clean data -> no mismatch; a corrupted arena copy is reported ================= */
    { unsigned long bad0 = g_mg_v_bad, runs0 = g_mg_v_runs; unsigned l[4] = { 0, 1, 2, 3 };
      for (int rep = 0; rep < 70; rep++) draw_layer(l, 4, 10, 0);
      CHECK(g_mg_v_runs > runs0 && g_mg_v_bad == bad0, "verify on clean data: %lu runs, %lu mismatches", g_mg_v_runs - runs0, g_mg_v_bad - bad0);
      MgBuf *b = mg_buf(vbo[1]); mg_free_slot(b); corrupt_next_copy = 1; g_mg_flush_seq = (g_mg_flush_seq / 30 + 1) * 30 - 1;   /* the next flush is a verify flush, and chunk 1 is copied (corrupted) in it */
      bad0 = g_mg_v_bad; draw_layer(l, 4, 10, 0);
      CHECK(g_mg_v_bad > bad0, "verify must report the corrupted arena copy (mismatches %lu -> %lu)", bad0, g_mg_v_bad); }
#endif
    /* ================= two chunk programs sharing the arena (solid + cutout) must both get correct offsets ================= */
    { unsigned l[8]; for (int k = 0; k < 8; k++) l[k] = (unsigned)k;
      for (int rep = 0; rep < 3; rep++) { for (int i = 0; i < NCH; i++) offs[i][0] += 0.25f; draw_layer(l, 8, 10, 0); draw_layer(l, 8, 40, 0); }
      nev = 0; draw_layer(l, 8, 40, 0);
      CHECK(count_draw_big() >= 1, "the second chunk program must also merge");
      { int bad = 0; long drawn = 0, want = 0; for (int k = 0; k < 8; k++) want += quads[l[k]] * 6L;
        for (int i = 0; i < nev; i++) { if (ev[i].kind != E_DRAW || ev[i].b != 0x1405) continue;
            unsigned *ib = (unsigned *)fb[g_draw_ebo[i]].data; long first = g_draw_off[i] / 4; float *pt = (float *)fb[f_pbuf].data;
            for (long p = 0; p < ev[i].a; p += 6) { unsigned a0 = ib[first + p]; if (a0 == ib[first + p + 1] && a0 == ib[first + p + 2]) continue; drawn += 6;
                int page = (int)(a0 / 128), found = -1;
                for (int c = 0; c < NCH; c++) { MgBuf *b = mg_buf(vbo[c]); if (b && b->valid && page >= b->page && page < b->page + b->pages) { found = c; break; } }
                if (found < 0) { bad++; continue; }
                float *w = pt + (size_t)page * 4;
                if (fabsf(g_base[0] + w[0] - offs[found][0]) > 0.01f || fabsf(g_base[1] + w[1] - offs[found][1]) > 0.01f) bad++; } }
        CHECK(bad == 0 && drawn == want, "second program: %d bad quads, %ld of %ld indices", bad, drawn, want); } }
    /* ================= debug levels (touchpad cycles OFF -> 1 -> 2 -> 3): level 2 never reuses the list, level 3 adds glFinish + full upload ================= */
    mg_toggle(); /* the harness is currently ON level 1 -> level 2 */
    CHECK(g_mg_level == 2 && !g_mg_user_off, "level after the first cycle: %d off=%d", g_mg_level, g_mg_user_off);
    nev = 0; draw_layer(list1, 12, 10, 1); draw_layer(list1, 12, 10, 1);
#if !MEGA_IDXCACHE
    { int idxup = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8893) idxup++; CHECK(idxup == 2, "level 2 must rebuild the list every flush (%d uploads for 2 flushes)", idxup); }
#endif
    CHECK(n_finish == 0, "level 2 must not glFinish (%d)", n_finish);
    mg_toggle(); CHECK(g_mg_level == 3, "level 3 expected, got %d", g_mg_level);
    nev = 0; n_finish = 0; draw_layer(list1, 12, 10, 1);
    CHECK(n_finish == 1 && "level 3: one glFinish per merged draw (finish %d)", n_finish);
    mg_toggle(); CHECK(g_mg_user_off == 1, "fourth press must switch merging off");
    mg_toggle(); CHECK(!g_mg_user_off && g_mg_level == 1, "fifth press must be level 1 again (off=%d level=%d)", g_mg_user_off, g_mg_level);
    nev = 0; draw_layer(list1, 12, 10, 1); draw_layer(list1, 12, 10, 1);
#if !MEGA_IDXCACHE
    { int idxup = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_BUFSUB && ev[i].a == 0x8893) idxup++; CHECK(idxup <= 1, "level 1 reuses the list (%d uploads)", idxup); }
#endif
    /* ================= translucent program (blend on) -> every chunk individually, no merged draw ================= */
    nev = 0; gEnable(0x0BE2); draw_layer(list1, 12, 20, 1); gDisable(0x0BE2);
    CHECK(count_ev(E_DRAW) == 12 && count_draw_big() == 0, "translucent: %d draws, %d merged (expect 12, 0)", count_ev(E_DRAW), count_draw_big());
    { int uni = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == E_UNI3) uni++; CHECK(uni >= 12, "individual draws must still receive their ChunkOffset (%d uniform uploads)", uni); }
    /* ================= a non-chunk program draw passes straight through ================= */
    nev = 0; gUse(30); gBindVAO(vao[0]); gDrawElements(4, 600, 0x1403, 0); gUse(0);
    CHECK(count_ev(E_DRAW) == 1 && ev[0].kind == E_USEPROG, "non-chunk draw: %d draws", count_ev(E_DRAW));
    /* ================= EBO that is not the sequential pattern -> individual draws ================= */
    nev = 0; { unsigned ebo2; gGenBuf(1, &ebo2); gBindVAO(vao[7]); gBindBuf(0x8893, ebo2); static unsigned short bad[300]; for (int i = 0; i < 300; i++) bad[i] = (unsigned short)(i % 7); gBufData(0x8893, sizeof bad, bad, 0x88E4); gBindVAO(0); }
    { unsigned l[1] = { 7 }; draw_layer(l, 1, 10, 0); }
    CHECK(count_ev(E_DRAW) == 1 && count_draw_big() == 0, "bad EBO: %d draws, %d merged", count_ev(E_DRAW), count_draw_big());
    /* ================= arena exhaustion -> graceful fallback ================= */
    nev = 0; for (int p = 0; p < MG_PAGES; p++) g_mg_pagefree[p] = 0; g_mg_qh = g_mg_qt = 0; g_mg_fail_n = 0x7fffffff;   /* the arena is full and nothing is quarantined */
    for (int i = 0; i < NCH; i++) { MgBuf *b = mg_buf(vbo[i]); if (b) { b->valid = 0; b->pages = 0; } }
    draw_layer(list1, 12, 10, 0);
    CHECK(count_ev(E_DRAW) == 12 && count_draw_big() == 0, "arena full: %d draws (expect 12 individual)", count_ev(E_DRAW));
    { unsigned long scan1 = g_mg_st_scan; nev = 0; draw_layer(list1, 12, 10, 0); CHECK(g_mg_st_scan - scan1 < 100000, "a failed allocation must not rescan the arena for every chunk of every frame (%lu entries scanned)", g_mg_st_scan - scan1); }
    memset(g_mg_pagefree, 1, sizeof g_mg_pagefree); g_mg_qh = g_mg_qt = 0; g_mg_fail_n = 0x7fffffff;
    /* ================= a pending fence that never signals must not hang the renderer ================= */
    nev = 0; g_wait_result = 0x911B; { unsigned l[3] = { 0, 1, 3 }; for (int rep = 0; rep < 4; rep++) draw_layer(l, 3, 10, 0); } g_wait_result = 0x911A;
    CHECK(count_draw_big() >= 1, "rendering must continue when a fence wait times out");
    /* ================= GL error during the first merged draws disables merging ================= */
    nev = 0; for (int i = 0; i < NCH; i++) { MgBuf *b = mg_buf(vbo[i]); if (b) mg_free_slot(b); } g_mg_nflush = 0; f_err = 0x502;
    draw_layer(list1, 3, 10, 0);
    nev = 0; draw_layer(list1, 12, 10, 0);
    CHECK(g_mg_on == 0 && count_ev(E_DRAW) == 12 && count_draw_big() == 0, "after a GL error merging must be off and draws individual: on=%d draws=%d", g_mg_on, count_ev(E_DRAW));
    { int k = 0, bad = 0; float last[3] = { 0, 0, 0 }; for (int i = 0; i < nev; i++) { if (ev[i].kind == E_UNI3) memcpy(last, ev[i].f, 12); if (ev[i].kind == E_DRAW) { int c = (int)list1[k++]; if (last[0] != offs[c][0] || last[1] != offs[c][1]) bad++; } } CHECK(bad == 0 && k == 12, "%d individual draws with a wrong ChunkOffset (k=%d)", bad, k); }
#ifdef MEGA_FENCE
    CHECK(n_fence_del >= n_fence - 2 * MG_RING * 3, "fences must be released (%d created, %d deleted)", n_fence, n_fence_del);
#endif
    CHECK(n_partial_row == 0, "the offset table must not go through TexSubImage2D at all (%d calls)", n_partial_row);
    dprintf(1, "mega harness: fails=%d\n", fails);
    return fails;
}
