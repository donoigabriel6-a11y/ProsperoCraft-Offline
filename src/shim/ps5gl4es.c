// ps5gl4es.c - fixed-function OpenGL (1.x / 2.x) for the console's GL 4.6 CORE context, through gl4es.
//
// Minecraft up to 1.16.5 (and everything older: LWJGL 2 era, alpha, beta, classic) draws with the fixed-function pipeline (glMatrixMode, glAlphaFunc, glBegin, glEnable(GL_LIGHTING), client-side
// vertex arrays, GL_LUMINANCE textures ...). The console only has a core profile, where none of that exists: LWJGL looks glAlphaFunc up, gets 0 and aborts the JVM.
// gl4es (MIT, github.com/ptitSeb/gl4es) translates the old API to GLES2-style shaders. This file makes it run on a desktop CORE context instead of GLES2 (built NOEGL / NO_LOADER / STATICLIB):
//   * gl4es asks for every GL function through a callback (set_getprocaddress): we hand out the console's real functions, except where core differs from ES2 (the overrides below)
//   * shaders: gl4es produces GLSL ES 1.00 text ("#version 100", attribute / varying / gl_FragColor / texture2D); sh_ShaderSource turns it into "#version 330 core"
//   * client-side vertex arrays and client-side index arrays do not exist in core: draws stream them through a ring buffer (vertex data) and an index ring
//   * GL_ALPHA / GL_LUMINANCE / GL_LUMINANCE_ALPHA textures do not exist in core: stored as R8 / RG8 with a texture swizzle
//   * a vertex array object must always be bound (one default VAO), and gl_PointSize needs GL_PROGRAM_POINT_SIZE
// The rest of gl4es (matrix stacks, lighting, fog, texgen, display lists, glBegin/glEnd, quads, ...) runs unchanged.
// Entry points used by ps5lwjgl.c:   ps5g4_lookup(name)  -> gl4es' function for a GL name (0 = gl4es does not know it)     ps5g4_pre_swap()  before each buffer swap
// Log lines start with "g4:".
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glx/hardext.h"
#include "gl/init.h"
#include "gl/glstate.h"
#include "gl/buffers.h"

extern void ps5_trace(const char *);
static void G4(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void G4(const char *fmt, ...) { char b[200]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); ps5_trace(b); }

// gl4es entry points that stay exported (everything else of gl4es is renamed g4_* by build_gl4es.sh)
extern void set_getprocaddress(void *(*)(const char *));
extern void set_getmainfbsize(void (*)(int *, int *));
extern void initialize_gl4es(void);
extern void *gl4es_GetProcAddress(const char *name);
extern void gl4es_pre_swap(void);
extern glstate_t *glstate;

typedef unsigned int u32;
typedef intptr_t isz;

// ---------------------------------------------------------------- the console's real GL functions
static void *(*g_real)(const char *);
#define REALFNS \
    FN(void, glBindBuffer, (u32, u32)) \
    FN(void, glBufferData, (u32, isz, const void *, u32)) \
    FN(void, glBufferSubData, (u32, isz, isz, const void *)) \
    FN(void, glGetBufferSubData, (u32, isz, isz, void *)) \
    FN(void, glDeleteBuffers, (int, const u32 *)) \
    FN(void, glGenBuffers, (int, u32 *)) \
    FN(void, glVertexAttribPointer, (u32, int, u32, unsigned char, int, const void *)) \
    FN(void, glVertexAttribIPointer, (u32, int, u32, int, const void *)) \
    FN(void, glEnableVertexAttribArray, (u32)) \
    FN(void, glDisableVertexAttribArray, (u32)) \
    FN(void, glDrawArrays, (u32, int, int)) \
    FN(void, glDrawElements, (u32, int, u32, const void *)) \
    FN(void, glTexImage2D, (u32, int, int, int, int, int, u32, u32, const void *)) \
    FN(void, glTexSubImage2D, (u32, int, int, int, int, int, u32, u32, const void *)) \
    FN(void, glCopyTexImage2D, (u32, int, u32, int, int, int, int, int)) \
    FN(void, glCopyTexSubImage2D, (u32, int, int, int, int, int, int, int)) \
    FN(void, glTexParameteri, (u32, u32, int)) \
    FN(void, glTexParameteriv, (u32, u32, const int *)) \
    FN(void, glBindTexture, (u32, u32)) \
    FN(void, glActiveTexture, (u32)) \
    FN(void, glUseProgram, (u32)) \
    FN(void, glDeleteTextures, (int, const u32 *)) \
    FN(void, glGenVertexArrays, (int, u32 *)) \
    FN(void, glBindVertexArray, (u32)) \
    FN(void, glEnable, (u32)) \
    FN(void, glDisable, (u32)) \
    FN(void, glShaderSource, (u32, int, const char *const *, const int *)) \
    FN(void, glGetShaderiv, (u32, u32, int *)) \
    FN(void, glGetShaderInfoLog, (u32, int, int *, char *)) \
    FN(void, glGetProgramiv, (u32, u32, int *)) \
    FN(void, glGetProgramInfoLog, (u32, int, int *, char *)) \
    FN(void, glGetIntegerv, (u32, int *)) \
    FN(void, glGetFloatv, (u32, float *)) \
    FN(u32, glGetError, (void)) \
    FN(const unsigned char *, glGetString, (u32))
#define FN(ret, name, args) static ret (*r_##name) args;
REALFNS
#undef FN
static void load_real(void) {
#define FN(ret, name, args) r_##name = (ret (*) args)g_real(#name);
    REALFNS
#undef FN
}

// ---------------------------------------------------------------- GL constants
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_COPY_WRITE_BUFFER 0x8F37   // the vertex ring is uploaded through this target, so the array binding is never disturbed
#define GL_STREAM_DRAW 0x88E0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#define GL_SHADER_TYPE 0x8B4F
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_ALPHA 0x1906
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_RGBA8 0x8058
#define GL_BGRA_EXT 0x80E1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405

static int g_ready, g_fail;
static int g_log_src;   // translated shader sources logged so far (compile failures)

// ---------------------------------------------------------------- state mirrored here: buffer bindings, client arrays
#define NATT 16
typedef struct { int en, client, integer, size; u32 type; unsigned char norm; int stride; const void *ptr; } Att;
static Att g_att[NATT];
static u32 g_arr_buf, g_elem_buf;
static u32 g_ring, g_iring; static isz g_ring_cap = 16 << 20, g_ring_pos, g_iring_cap = 4 << 20, g_iring_pos;
static unsigned long g_n_client_draws, g_n_idx_draws, g_n_ring_wraps, g_n_ring_bytes, g_n_ic_hits;
#define IC_MAX 2048
static double g_pf_prep, g_pf_idx, g_pf_draw; static unsigned long g_pf_n;   // sampled (every 8th index-ring draw) time spent in our GL calls per draw, seconds
static double pf_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

static int type_size(u32 t) {
    switch (t) {
    case 0x1400: case 0x1401: return 1;                        // BYTE, UNSIGNED_BYTE
    case 0x1402: case 0x1403: case 0x140B: return 2;           // SHORT, UNSIGNED_SHORT, HALF_FLOAT
    case 0x1404: case 0x1405: case 0x1406: case 0x8368: case 0x8D9F: return 4;   // INT, UNSIGNED_INT, FLOAT, UNSIGNED_INT_2_10_10_10_REV
    case 0x140A: return 8;                                     // DOUBLE
    default: return 4;
    }
}
static int comp_bytes(const Att *a) {   // bytes of one element of the attribute
    if (a->type == 0x8368 || a->type == 0x8D9F || a->type == 0x8363) return 4;   // packed 2_10_10_10 / 5_6_5 count as one unit (5_6_5 is 2 bytes, never a vertex type)
    return a->size * type_size(a->type);
}

// What is REALLY bound (g_arr_buf / g_elem_buf are what gl4es believes). A bind that changes nothing is dropped: every GL call costs several microseconds on the console and the
// panorama of Minecraft 1.12.2 makes ~400 draws per frame. There is exactly one VAO (gl4es emulates its own), so the element binding is only changed through rbind().
static u32 g_rb_arr, g_rb_elem, g_rb_cw, g_vao;
static void rbind(u32 target, u32 id) {
    u32 *sh = target == GL_ARRAY_BUFFER ? &g_rb_arr : target == GL_ELEMENT_ARRAY_BUFFER ? &g_rb_elem : target == GL_COPY_WRITE_BUFFER ? &g_rb_cw : 0;
    if (sh) { if (*sh == id) return; *sh = id; }
    r_glBindBuffer(target, id);
}
static void sh_BindBuffer(u32 target, u32 id) {
    if (target == GL_ARRAY_BUFFER) g_arr_buf = id; else if (target == GL_ELEMENT_ARRAY_BUFFER) g_elem_buf = id;
    rbind(target, id);
}
static void sh_DeleteBuffers(int n, const u32 *ids) {
    for (int i = 0; i < n; i++) {
        if (ids[i] == g_arr_buf) g_arr_buf = 0; if (ids[i] == g_elem_buf) g_elem_buf = 0;
        if (ids[i] == g_rb_arr) g_rb_arr = 0; if (ids[i] == g_rb_elem) g_rb_elem = 0; if (ids[i] == g_rb_cw) g_rb_cw = 0;   // GL unbinds a deleted buffer
    }
    r_glDeleteBuffers(n, ids);
}
static void sh_VertexAttribPointer(u32 idx, int size, u32 type, unsigned char norm, int stride, const void *ptr) {
    if (idx >= NATT) { r_glVertexAttribPointer(idx, size, type, norm, stride, ptr); return; }
    Att *a = &g_att[idx];
    a->size = size; a->type = type; a->norm = norm; a->stride = stride; a->ptr = ptr; a->integer = 0;
    if (g_arr_buf) { a->client = 0; r_glVertexAttribPointer(idx, size, type, norm, stride, ptr); } else a->client = 1;
}
static void sh_VertexAttribIPointer(u32 idx, int size, u32 type, int stride, const void *ptr) {
    if (idx >= NATT) { r_glVertexAttribIPointer(idx, size, type, stride, ptr); return; }
    Att *a = &g_att[idx];
    a->size = size; a->type = type; a->norm = 0; a->stride = stride; a->ptr = ptr; a->integer = 1;
    if (g_arr_buf) { a->client = 0; r_glVertexAttribIPointer(idx, size, type, stride, ptr); } else a->client = 1;
}
static void sh_EnableVA(u32 idx) { if (idx < NATT) g_att[idx].en = 1; r_glEnableVertexAttribArray(idx); }
static void sh_DisableVA(u32 idx) { if (idx < NATT) g_att[idx].en = 0; r_glDisableVertexAttribArray(idx); }

// ring allocation (orphaned when it wraps: a new store, so the GPU may still read the old one)
static unsigned long g_iring_gen;   // counts the orphanings of the index ring: cached index uploads of an older generation are gone
static isz ring_alloc(u32 *buf, isz *pos, isz cap, u32 target, isz n, unsigned long *wraps) {
    n = (n + 15) & ~(isz)15;
    if (*pos + n > cap) { rbind(target, *buf); r_glBufferData(target, cap, 0, GL_STREAM_DRAW); *pos = 0; if (wraps) (*wraps)++; if (buf == &g_iring) g_iring_gen++; }
    isz at = *pos; *pos += n; return at;
}

// Upload the client arrays of the enabled attributes for vertices [minv, maxv] and point the attributes at the copies. Returns a bit mask of attributes that were
// switched off for this draw (enabled, but with neither a buffer nor a pointer: core rejects those); bit 31 = the data did not fit, skip the draw.
// Arrays of equal stride whose memory overlaps (an interleaved vertex: position at +0, colour at +12 ...) are copied once.
typedef struct { const char *lo, *hi; int stride; isz at; } Grp;
static unsigned prep_client(long minv, long maxv) {
    unsigned off = 0; Grp g[NATT]; int gi[NATT]; int ng = 0;
    for (int i = 0; i < NATT; i++) {
        gi[i] = -1; Att *a = &g_att[i]; if (!a->en || !a->client) continue;
        if (!a->ptr) { r_glDisableVertexAttribArray(i); off |= 1u << i; continue; }
        int eb = comp_bytes(a), s = a->stride ? a->stride : eb;
        g[ng].lo = (const char *)a->ptr + minv * (long)s; g[ng].hi = (const char *)a->ptr + maxv * (long)s + eb; g[ng].stride = s; g[ng].at = 0; gi[i] = ng++;
    }
    if (!ng) return off;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int a = 0; a < ng; a++) {
            if (g[a].stride < 0) continue;
            for (int b = a + 1; b < ng; b++) {
                if (g[b].stride != g[a].stride) continue;
                if (g[b].lo < g[a].hi + g[a].stride && g[a].lo < g[b].hi + g[a].stride) {
                    if (g[b].lo < g[a].lo) g[a].lo = g[b].lo;
                    if (g[b].hi > g[a].hi) g[a].hi = g[b].hi;
                    g[b].stride = -1; changed = 1;
                    for (int i = 0; i < NATT; i++) if (gi[i] == b) gi[i] = a;
                }
            }
        }
    }
    for (int k = 0; k < ng; k++) {
        if (g[k].stride < 0) continue;
        isz len = (isz)(g[k].hi - g[k].lo), pre = (isz)minv * g[k].stride;   // pre: room in front, so that the pointer value (vertex 0's address) never goes below the buffer start
        if (pre > (1 << 20) || pre < 0) pre = 0;
        if (pre + len > g_ring_cap) { G4("g4: client array of %ld bytes does not fit the %ld byte ring: draw skipped", (long)len, (long)g_ring_cap); return off | 0x80000000u; }
        isz at = ring_alloc(&g_ring, &g_ring_pos, g_ring_cap, GL_COPY_WRITE_BUFFER, pre + len, &g_n_ring_wraps) + pre;
        rbind(GL_COPY_WRITE_BUFFER, g_ring);
        r_glBufferSubData(GL_COPY_WRITE_BUFFER, at, len, g[k].lo);
        g[k].at = at; g_n_ring_bytes += (unsigned long)len;
    }
    rbind(GL_ARRAY_BUFFER, g_ring);
    for (int i = 0; i < NATT; i++) {
        Att *a = &g_att[i]; if (gi[i] < 0) continue; Grp *q = &g[gi[i]];
        const char *lo0 = (const char *)a->ptr + minv * (long)q->stride;   // this attribute's first used byte
        uintptr_t ptr = (uintptr_t)q->at + (uintptr_t)(lo0 - q->lo) - (uintptr_t)(minv * (long)q->stride);   // where vertex 0 would be in the ring (vertex v is at ptr + v * stride)
        if (a->integer) r_glVertexAttribIPointer(i, a->size, a->type, a->stride, (const void *)ptr);
        else r_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, (const void *)ptr);
    }
    // gl4es believes g_arr_buf is bound: put it back when that is a real buffer. With 0 (client arrays only) nothing reads the array binding, so the ring just stays bound for the next draw.
    if (g_arr_buf) rbind(GL_ARRAY_BUFFER, g_arr_buf);
    return off;
}
static int any_client(void) { for (int i = 0; i < NATT; i++) if (g_att[i].en && g_att[i].client) return 1; return 0; }
static void restore_off(unsigned off) { for (int i = 0; i < NATT; i++) if (off & (1u << i)) r_glEnableVertexAttribArray(i); }

static void sh_DrawArrays(u32 mode, int first, int count) {
    if (count <= 0) return;
    unsigned off = 0;
    if (any_client()) { g_n_client_draws++; off = prep_client(first, (long)first + count - 1); if (off & 0x80000000u) { restore_off(off & 0x7FFFFFFFu); return; } }
    r_glDrawArrays(mode, first, count);
    restore_off(off);
}
static void index_range(const void *idx, int count, u32 type, long *mn, long *mx) {
    long lo = 0x7FFFFFFF, hi = 0;
    if (type == GL_UNSIGNED_BYTE) { const unsigned char *p = idx; for (int i = 0; i < count; i++) { long v = p[i]; if (v < lo) lo = v; if (v > hi) hi = v; } }
    else if (type == GL_UNSIGNED_SHORT) { const unsigned short *p = idx; for (int i = 0; i < count; i++) { long v = p[i]; if (v < lo) lo = v; if (v > hi) hi = v; } }
    else { const unsigned *p = idx; for (int i = 0; i < count; i++) { long v = p[i]; if (v < lo) lo = v; if (v > hi) hi = v; } }
    *mn = lo; *mx = hi;
}
static void sh_DrawElements(u32 mode, int count, u32 type, const void *indices) {
    if (count <= 0) return;
    int isz_ = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    unsigned off = 0;
    if (g_elem_buf) {   // the indices are an offset into the bound element buffer
        if (any_client()) {
            long mn = 0, mx = 0; int got = 0;
            glbuffer_t *eb = glstate && glstate->vao ? glstate->vao->elements : 0;   // gl4es keeps a CPU copy of every buffer
            if (eb && eb->data && eb->real_buffer == g_elem_buf && (isz)indices + (isz)count * isz_ <= eb->size) { index_range((const char *)eb->data + (isz)indices, count, type, &mn, &mx); got = 1; }
            if (!got) {
                void *tmp = malloc((size_t)count * isz_);
                if (tmp) { r_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (isz)indices, (isz)count * isz_, tmp); index_range(tmp, count, type, &mn, &mx); free(tmp); got = 1; }
                static int logged; if (!logged++) G4("g4: client vertex arrays with an element buffer and no CPU copy: reading the indices back from the GPU");
            }
            if (!got) return;
            g_n_client_draws++; off = prep_client(mn, mx); if (off & 0x80000000u) { restore_off(off & 0x7FFFFFFFu); return; }
        }
        r_glDrawElements(mode, count, type, indices);
        restore_off(off);
        return;
    }
    // client indices: scan for the vertex range, copy the indices into the index ring
    long mn, mx; index_range(indices, count, type, &mn, &mx);
    int pf = (g_n_idx_draws & 7) == 0; double t0 = pf ? pf_now() : 0, t1, t2;
    if (any_client()) { g_n_client_draws++; off = prep_client(mn, mx); if (off & 0x80000000u) { restore_off(off & 0x7FFFFFFFu); return; } }
    t1 = pf ? pf_now() : 0;
    isz bytes = (isz)count * isz_;
    if (bytes > g_iring_cap) { G4("g4: %d indices do not fit the index ring: draw skipped", count); restore_off(off & 0x7FFFFFFFu); return; }
    // the same small index list again and again (GL_QUADS become 0,1,2,0,2,3,...): reuse the copy that is already in the ring
    static struct { isz at, bytes; unsigned long gen; unsigned char d[IC_MAX]; } ic[4]; static unsigned ic_next;
    isz at = -1;
    if (bytes <= IC_MAX) for (int i = 0; i < 4; i++) if (ic[i].bytes == bytes && ic[i].gen == g_iring_gen + 1 && !memcmp(ic[i].d, indices, (size_t)bytes)) { at = ic[i].at; g_n_ic_hits++; break; }
    if (at < 0) {
        at = ring_alloc(&g_iring, &g_iring_pos, g_iring_cap, GL_ELEMENT_ARRAY_BUFFER, bytes, &g_n_ring_wraps);
        rbind(GL_ELEMENT_ARRAY_BUFFER, g_iring);
        r_glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, at, bytes, indices);
        if (bytes <= IC_MAX) { unsigned k = ic_next++ & 3; ic[k].at = at; ic[k].bytes = bytes; ic[k].gen = g_iring_gen + 1; memcpy(ic[k].d, indices, (size_t)bytes); }
    }
    rbind(GL_ELEMENT_ARRAY_BUFFER, g_iring);   // stays bound afterwards: gl4es binds its own element buffer through sh_BindBuffer before it uses one, and with g_elem_buf == 0 nothing reads the binding
    t2 = pf ? pf_now() : 0;
    r_glDrawElements(mode, count, type, (const void *)at);
    if (pf) { double t3 = pf_now(); g_pf_n++; g_pf_prep += t1 - t0; g_pf_idx += t2 - t1; g_pf_draw += t3 - t2; }
    g_n_idx_draws++;
    restore_off(off);
}
static void sh_MultiDrawArrays(u32 mode, const int *first, const int *count, int n) { for (int i = 0; i < n; i++) sh_DrawArrays(mode, first[i], count[i]); }
static void sh_MultiDrawElements(u32 mode, const int *count, u32 type, const void *const *idx, int n) { for (int i = 0; i < n; i++) sh_DrawElements(mode, count[i], type, idx[i]); }

// ---------------------------------------------------------------- textures: GL_ALPHA / GL_LUMINANCE / GL_LUMINANCE_ALPHA -> R8 / RG8 + swizzle
static u32 g_tex2d[32]; static int g_unit;
static unsigned char g_swz[65536];   // per texture id: 0 identity, 1 alpha, 2 luminance, 3 luminance-alpha
// glGetError: Minecraft 1.0-1.5 asks once per frame (Minecraft.checkGLError) and the profile put 50-90% of the game thread in that call, i.e. the driver call waits for the queued frame. Nothing here consumes the error
// (gl4es keeps its own shim errors; the game only logs it), so the driver is not asked; one call in 64 is still timed to show what it would have cost ("g4: glGetError").
static unsigned long g_ge_n, g_ge_s; static double g_ge_t, g_ge_max;
static u32 sh_GetError(void) {
    if ((++g_ge_n & 63) != 1) return 0;
    double t0 = pf_now(); u32 e = r_glGetError(); double d = pf_now() - t0; g_ge_s++; g_ge_t += d; if (d > g_ge_max) g_ge_max = d; return e;
}
static void sh_ActiveTexture(u32 t) { g_unit = (int)(t - GL_TEXTURE0) & 31; r_glActiveTexture(t); }
static void sh_BindTexture(u32 target, u32 id) { if (target == GL_TEXTURE_2D) g_tex2d[g_unit] = id; r_glBindTexture(target, id); }
static void sh_DeleteTextures(int n, const u32 *ids) { for (int i = 0; i < n; i++) { if (ids[i] < 65536) g_swz[ids[i]] = 0; for (int u = 0; u < 32; u++) if (g_tex2d[u] == ids[i]) g_tex2d[u] = 0; } r_glDeleteTextures(n, ids); }
static int legacy_kind(u32 f) { return f == GL_ALPHA ? 1 : f == GL_LUMINANCE ? 2 : f == GL_LUMINANCE_ALPHA ? 3 : 0; }
static void set_swizzle(u32 target, int kind) {
    int v[4];
    switch (kind) {
    case 1: v[0] = GL_ZERO; v[1] = GL_ZERO; v[2] = GL_ZERO; v[3] = GL_RED; break;   // alpha: (0,0,0,r)
    case 2: v[0] = GL_RED; v[1] = GL_RED; v[2] = GL_RED; v[3] = GL_ONE; break;      // luminance: (r,r,r,1)
    case 3: v[0] = GL_RED; v[1] = GL_RED; v[2] = GL_RED; v[3] = 0x1904; break;      // luminance-alpha: (r,r,r,g)
    default: v[0] = 0x1903; v[1] = 0x1904; v[2] = 0x1905; v[3] = 0x1906; break;     // identity: R G B A
    }
    r_glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, v);
}
static void track_swizzle(u32 target, int level, int kind) {   // the texture bound to the active unit was (re)specified with the given legacy kind (0 = a normal format)
    (void)level;
    if (target != GL_TEXTURE_2D) return;
    u32 id = g_tex2d[g_unit]; if (id == 0 || id >= 65536) return;
    if (g_swz[id] == kind) return;
    set_swizzle(target, kind); g_swz[id] = (unsigned char)kind;
}
static void sh_TexImage2D(u32 target, int level, int ifmt, int w, int h, int border, u32 fmt, u32 type, const void *data) {
    int kind = legacy_kind((u32)ifmt);
    if (kind) { ifmt = kind == 3 ? GL_RG8 : GL_R8; fmt = kind == 3 ? GL_RG : GL_RED; }
    else { if (ifmt == GL_BGRA_EXT) ifmt = GL_RGBA8; kind = legacy_kind(fmt) ? legacy_kind(fmt) : 0; if (kind) { fmt = kind == 3 ? GL_RG : GL_RED; ifmt = kind == 3 ? GL_RG8 : GL_R8; } }
    r_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
    track_swizzle(target, level, kind);
}
static void sh_TexSubImage2D(u32 target, int level, int x, int y, int w, int h, u32 fmt, u32 type, const void *data) {
    int kind = legacy_kind(fmt);
    if (kind) fmt = kind == 3 ? GL_RG : GL_RED;
    r_glTexSubImage2D(target, level, x, y, w, h, fmt, type, data);
}
static void sh_CopyTexImage2D(u32 target, int level, u32 ifmt, int x, int y, int w, int h, int border) {
    int kind = legacy_kind(ifmt);
    if (kind) ifmt = kind == 3 ? GL_RG8 : GL_R8; else if (ifmt == GL_BGRA_EXT) ifmt = GL_RGBA8;
    r_glCopyTexImage2D(target, level, ifmt, x, y, w, h, border);
    track_swizzle(target, level, kind);
}

// glCopyTexSubImage2D, timed: Minecraft's title screen copies the 256x256 panorama render into a texture 7 times per frame (1.12.2 rotateAndBlurSkybox); with GLALL the real call cost ~7 ms each
static unsigned long g_cp_n, g_cp_n_all; static double g_cp_t, g_cp_max;
static double g4_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sh_CopyTexSubImage2D(u32 target, int level, int xo, int yo, int x, int y, int w, int h) {
    double t0 = g4_now(); r_glCopyTexSubImage2D(target, level, xo, yo, x, y, w, h); double dt = g4_now() - t0;
    g_cp_n++; g_cp_n_all++; g_cp_t += dt; if (dt > g_cp_max) g_cp_max = dt;
    if (g_cp_n_all <= 3) { int rf = -1; r_glGetIntegerv(0x8CAA, &rf); G4("g4: glCopyTexSubImage2D #%lu: %dx%d from (%d,%d) to (%d,%d) level %d, read framebuffer %d, %.2f ms", g_cp_n_all, w, h, x, y, xo, yo, level, rf, dt * 1e3); }
}

// ---------------------------------------------------------------- shaders: GLSL ES 1.00 -> GLSL 3.30 core
static int is_id(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
typedef struct { const char *from, *to; } Rep;
static const Rep k_common[] = { { "texture2D", "texture" }, { "texture2DProj", "textureProj" }, { "texture2DLod", "textureLod" }, { "texture2DProjLod", "textureProjLod" },
    { "texture2DLodEXT", "textureLod" }, { "texture2DProjLodEXT", "textureProjLod" }, { "texture2DGradEXT", "textureGrad" }, { "textureCube", "texture" }, { "textureCubeLod", "textureLod" },
    { "textureCubeLodEXT", "textureLod" }, { "texture3D", "texture" }, { "gl_FragDepthEXT", "gl_FragDepth" }, { 0, 0 } };
// returns a malloc'ed GLSL 330 core shader; vertex: attribute -> in, varying -> out; fragment: varying -> in, gl_FragColor / gl_FragData -> declared outputs
static char *translate_shader(const char *src, int vertex) {
    size_t n = strlen(src); char *out = malloc(n * 2 + 1024); if (!out) return 0;
    size_t o = 0; const char *p = src;
    int uses_color = strstr(src, "gl_FragColor") != 0, uses_data = strstr(src, "gl_FragData") != 0;
    // version line
    const char *v = strstr(src, "#version");
    if (v && (v == src || v[-1] == '\n')) {
        o += (size_t)(v - src); memcpy(out, src, (size_t)(v - src)); p = v; while (*p && *p != '\n') p++;
    }
    o += (size_t)sprintf(out + o, "#version 330 core\n");
    if (!vertex) {
        if (uses_color) o += (size_t)sprintf(out + o, "out vec4 g4_FragColor;\n");
        if (uses_data) o += (size_t)sprintf(out + o, "layout(location = 0) out vec4 g4_FragData[8];\n");
    }
    while (*p) {
        if (*p == '#' && (p == src || p[-1] == '\n')) {   // preprocessor line: drop #extension (core has what ES asked for), keep the others
            const char *e = p; while (*e && *e != '\n') e++;
            if (!strncmp(p, "#extension", 10)) { p = e; continue; }
            if (!strncmp(p, "#version", 8)) { p = e; continue; }
            memcpy(out + o, p, (size_t)(e - p)); o += (size_t)(e - p); p = e; continue;
        }
        if (is_id(*p) && !(*p >= '0' && *p <= '9') && (p == src || !is_id(p[-1]))) {
            const char *e = p; while (is_id(*e)) e++;
            size_t len = (size_t)(e - p); const char *to = 0;
            #define W(s) (len == sizeof(s) - 1 && !memcmp(p, s, len))
            if (W("attribute")) to = vertex ? "in" : "attribute";
            else if (W("varying")) to = vertex ? "out" : "in";
            else if (W("gl_FragColor")) to = "g4_FragColor";
            else if (W("gl_FragData")) to = "g4_FragData";
            else for (int i = 0; k_common[i].from; i++) if (len == strlen(k_common[i].from) && !memcmp(p, k_common[i].from, len)) { to = k_common[i].to; break; }
            #undef W
            if (to) { size_t l = strlen(to); memcpy(out + o, to, l); o += l; } else { memcpy(out + o, p, len); o += len; }
            p = e; continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0; return out;
}

static char *g_src_cache[64]; static u32 g_src_id[64]; static int g_src_n;   // translated sources by shader id, for the compile-failure report
static void sh_ShaderSource(u32 shader, int count, const char *const *str, const int *len) {
    size_t total = 1; for (int i = 0; i < count; i++) total += str[i] ? (len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i])) : 0;
    char *cat = malloc(total); if (!cat) return; size_t o = 0;
    for (int i = 0; i < count; i++) if (str[i]) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(cat + o, str[i], l); o += l; }
    cat[o] = 0;
    int type = 0; r_glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    char *t = translate_shader(cat, type == GL_VERTEX_SHADER); free(cat);
    if (!t) return;
    const char *tp = t; r_glShaderSource(shader, 1, &tp, 0);
    int slot = g_src_n % 64; free(g_src_cache[slot]); g_src_cache[slot] = t; g_src_id[slot] = shader; g_src_n++;
}
static void log_text(const char *tag, const char *s, int maxlines) {
    int ln = 0; const char *p = s;
    while (*p && ln < maxlines) { const char *e = p; while (*e && *e != '\n' && e - p < 150) e++; char b[160]; size_t l = (size_t)(e - p); memcpy(b, p, l); b[l] = 0; G4("g4: %s%s", tag, b); ln++; p = e; if (*p == '\n') p++; }
}
static void report_compile_fail(u32 shader) {
    static int reported; if (reported >= 4) return; reported++;
    char log[512]; int l = 0; r_glGetShaderInfoLog(shader, sizeof log - 1, &l, log); log[l > 0 ? l : 0] = 0;
    G4("g4: shader %u failed to compile:", shader); log_text("   log: ", log, 6);
    for (int i = 0; i < 64; i++) if (g_src_id[i] == shader && g_src_cache[i]) { log_text("   src: ", g_src_cache[i], 60); break; }
}
static void sh_GetShaderiv(u32 shader, u32 pname, int *params) {
    r_glGetShaderiv(shader, pname, params);
    if (pname == GL_COMPILE_STATUS && params && *params == 0) report_compile_fail(shader);
}
static void sh_GetProgramiv(u32 prog, u32 pname, int *params) {
    r_glGetProgramiv(prog, pname, params);
    if (pname == GL_LINK_STATUS && params && *params == 0) {
        static int reported; if (reported++ < 4) { char log[512]; int l = 0; r_glGetProgramInfoLog(prog, sizeof log - 1, &l, log); log[l > 0 ? l : 0] = 0; G4("g4: program %u failed to link:", prog); log_text("   log: ", log, 6); }
    }
}

// ---------------------------------------------------------------- lookup callback given to gl4es
static void *real_lookup(const char *name) {
    // fixed-function entry points the console's GL exports although they are invalid in a core context (glGetError 0x502 on every call: Minecraft 1.12.2 raised one per gradient, every frame). gl4es keeps the state itself and treats a missing function as "OpenGL ES 2 has none".
    if (!strcmp(name, "glShadeModel") || !strcmp(name, "glClientActiveTexture")) return 0;
    void *p = g_real(name);
    if (!p) {   // ES-style names: glBlendEquationOES, glDrawBuffersEXT ...
        size_t l = strlen(name);
        if (l > 3 && (!strcmp(name + l - 3, "OES") || !strcmp(name + l - 3, "EXT") || !strcmp(name + l - 3, "KHR"))) { char b[96]; if (l - 3 < sizeof b) { memcpy(b, name, l - 3); b[l - 3] = 0; p = g_real(b); } }
    }
    return p;
}
static void *g4_getproc(const char *name) {
    static const struct { const char *n; void *f; } ov[] = {
        { "glGetError", (void *)sh_GetError }, { "glBindBuffer", (void *)sh_BindBuffer }, { "glDeleteBuffers", (void *)sh_DeleteBuffers },
        { "glVertexAttribPointer", (void *)sh_VertexAttribPointer }, { "glVertexAttribIPointer", (void *)sh_VertexAttribIPointer },
        { "glEnableVertexAttribArray", (void *)sh_EnableVA }, { "glDisableVertexAttribArray", (void *)sh_DisableVA },
        { "glDrawArrays", (void *)sh_DrawArrays }, { "glDrawElements", (void *)sh_DrawElements },
        { "glMultiDrawArrays", (void *)sh_MultiDrawArrays }, { "glMultiDrawElements", (void *)sh_MultiDrawElements },
        { "glTexImage2D", (void *)sh_TexImage2D }, { "glTexSubImage2D", (void *)sh_TexSubImage2D }, { "glCopyTexImage2D", (void *)sh_CopyTexImage2D }, { "glCopyTexSubImage2D", (void *)sh_CopyTexSubImage2D },
        { "glBindTexture", (void *)sh_BindTexture }, { "glDeleteTextures", (void *)sh_DeleteTextures }, { "glActiveTexture", (void *)sh_ActiveTexture },
        { "glShaderSource", (void *)sh_ShaderSource }, { "glGetShaderiv", (void *)sh_GetShaderiv }, { "glGetProgramiv", (void *)sh_GetProgramiv }, { 0, 0 } };
    for (int i = 0; ov[i].n; i++) if (!strcmp(name, ov[i].n)) return ov[i].f;
    return real_lookup(name);
}
static void g4_mainfb(int *w, int *h) { *w = 1920; *h = 1080; }

// ---------------------------------------------------------------- init
static int g_glver = 21;
int ps5g4_init(void *(*real)(const char *), int glver) {
    if (g_ready) return 1; if (g_fail) return 0;
    g_real = real; if (glver > 0) g_glver = glver;
    load_real();
    if (!r_glBindBuffer || !r_glGenVertexArrays || !r_glDrawElements) { G4("g4: init failed: core GL functions missing"); g_fail = 1; return 0; }
    // core needs a vertex array object; one is enough, gl4es keeps its own VAO emulation
    u32 vao = 0; r_glGenVertexArrays(1, &vao); r_glBindVertexArray(vao); g_vao = vao;
    r_glEnable(GL_PROGRAM_POINT_SIZE);
    r_glGenBuffers(1, &g_ring); r_glBindBuffer(GL_ARRAY_BUFFER, g_ring); r_glBufferData(GL_ARRAY_BUFFER, g_ring_cap, 0, GL_STREAM_DRAW); r_glBindBuffer(GL_ARRAY_BUFFER, 0);
    r_glGenBuffers(1, &g_iring); r_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_iring); r_glBufferData(GL_ELEMENT_ARRAY_BUFFER, g_iring_cap, 0, GL_STREAM_DRAW); r_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    int maxtex = 8, maxsize = 4096, maxvary = 15, maxvattr = 16, maxdb = 8, maxca = 8, maxtu = 16; float aniso = 0;
    r_glGetIntegerv(0x0D33, &maxsize);        // GL_MAX_TEXTURE_SIZE
    r_glGetIntegerv(0x8872, &maxtex);         // GL_MAX_TEXTURE_IMAGE_UNITS
    r_glGetIntegerv(0x8B4B, &maxtu);          // GL_MAX_VARYING_FLOATS (unused)
    r_glGetIntegerv(0x8869, &maxvattr);       // GL_MAX_VERTEX_ATTRIBS
    r_glGetIntegerv(0x8824, &maxdb);          // GL_MAX_DRAW_BUFFERS
    r_glGetIntegerv(0x8CDF, &maxca);          // GL_MAX_COLOR_ATTACHMENTS
    r_glGetIntegerv(0x8DFC, &maxvary);        // GL_MAX_VARYING_VECTORS
    r_glGetFloatv(0x84FF, &aniso);            // GL_MAX_TEXTURE_MAX_ANISOTROPY
    r_glGetError();                           // clear errors from the probes above
    if (maxtex < 8) maxtex = 8; if (maxtex > 16) maxtex = 16; if (maxvattr > 16) maxvattr = 16; if (maxdb < 1) maxdb = 1; if (maxdb > 8) maxdb = 8; if (maxca < 1) maxca = 1;
    set_getmainfbsize(g4_mainfb);
    set_getprocaddress(g4_getproc);   // set BEFORE initialize: gl4es then probes nothing itself (build_gl4es.sh makes the probe step a no-op) and all its GL calls come through here
    initialize_gl4es();
    globals4es.silentstub = 1; globals4es.nobanner = 1; globals4es.nointovlhack = 1;   /* the int/float overloads gl4es adds for pow, min, max, clamp, mod HIDE the built-ins in a #version 330 shader: every post shader of 1.12.2 failed to compile */ globals4es.gl = g_glver; globals4es.es = 2;
    hardext.esversion = 2;
    hardext.npot = 2; hardext.maxtex = maxtex; hardext.maxsize = maxsize; hardext.maxlights = 8; hardext.maxplanes = 6;
    hardext.blendsub = hardext.blendfunc = hardext.blendeq = hardext.blendminmax = hardext.blendcolor = 1;
    hardext.pointsprite = 1; hardext.pointsize = 0; hardext.elementuint = 1; hardext.fbo = 1; hardext.depthstencil = 1; hardext.depth24 = 1; hardext.rgba8 = 1;
    hardext.multidraw = 0; hardext.bgra8888 = 1; hardext.depthtex = 1; hardext.stenciltex = 0; hardext.cubemap = 1; hardext.drawtex = 0; hardext.rgtex = 1;
    hardext.floattex = 1; hardext.halffloattex = 1; hardext.floatfbo = 1; hardext.halffloatfbo = 1; hardext.mirrored = 1; hardext.aniso = (int)aniso; hardext.mapbuffer = 0; hardext.drawbuffers = 1;
    hardext.maxvattrib = maxvattr; hardext.maxteximage = maxtex; hardext.maxvarying = maxvary; hardext.maxcolorattach = maxca; hardext.maxdrawbuffers = maxdb;
    hardext.highp = 1; hardext.fragdepth = 1; hardext.derivatives = 1; hardext.shaderlod = 0; hardext.cubelod = 0;
    hardext.prgbinary = 0; hardext.prgbin_n = 0; hardext.shader_fbfetch = 0; hardext.glsl120 = 0; hardext.glsl300es = 0; hardext.glsl310es = 0;
    g_ready = 1;
    G4("g4: gl4es ready on %s (GL %d.%d reported to the game, texture units %d, vertex attribs %d, max texture %d)", (const char *)r_glGetString(0x1F01) ? (const char *)r_glGetString(0x1F01) : "?", g_glver / 10, g_glver % 10, maxtex, maxvattr, maxsize);
    (void)maxtu;
    return 1;
}

static void *(*g_init_real)(const char *); static int g_init_ver;
void ps5g4_configure(void *(*real)(const char *), int glver) { g_init_real = real; g_init_ver = glver; }
// the lookup LWJGL (and everything else) goes through when fixed-function mode is on
void *ps5g4_lookup(const char *name) {
    if (!g_ready && !ps5g4_init(g_init_real, g_init_ver)) return 0;
    void *p = gl4es_GetProcAddress(name);
    if (!p) { static int miss; if (miss < 60) { miss++; G4("g4: %s is not known to gl4es", name); } }
    return p;
}
void ps5g4_report(void);
void ps5g4_pre_swap(void) {
    static unsigned long frames; frames++;
    if (!g_ready) return;
    gl4es_pre_swap();
    if (frames % 600 == 0) ps5g4_report();
}
int ps5g4_ready(void) { return g_ready; }
// Something else (the native launcher) used the same GL context since gl4es last ran: its VAO and buffer bindings replaced ours, but g_rb_* still say "ring bound", so the next rbind() was dropped,
// glVertexAttribPointer(ring offset) hit another buffer (or none: the SDK then reads the offset as a client address and crashes). Put the real bindings back to what gl4es believes.
void ps5g4_resync(void) {
    if (!g_ready || !g_vao) return;
    // gl4es caches the GLES program that is current (gleshard->program) and the active texture unit and skips the real call when its wish equals the cache. The launcher (or another game's shim) changed the real
    // ones in between: gl4es then never bound its fixed-function program again and every glUniformMatrix / glUniform went to the launcher's program ("GL_INVALID_OPERATION in glUniformMatrix(non-matrix uniform)",
    // "glUseProgram INVALID_VALUE" for a program the launcher had deleted): the second and later fixed-function game of a session started with a GL error flood. Make cache and driver agree on "none".
    if (glstate && glstate->gleshard) { r_glUseProgram(0); glstate->gleshard->program = 0; glstate->gleshard->glprogram = 0; r_glActiveTexture(GL_TEXTURE0); glstate->gleshard->active = 0; g_unit = 0; }
    r_glBindVertexArray(g_vao);   // the element binding and the attribute set-up live in this VAO and are untouched
    r_glBindBuffer(GL_ARRAY_BUFFER, g_rb_arr); r_glBindBuffer(GL_COPY_WRITE_BUFFER, g_rb_cw);
    G4("g4: bindings restored (vao %u, array %u, copy-write %u, element %u)", g_vao, g_rb_arr, g_rb_cw, g_rb_elem);
}
void ps5g4_report(void) {
    G4("g4: client-array draws=%lu index-ring draws=%lu (%lu reused an earlier index upload) ring wraps=%lu uploaded=%lu KB", g_n_client_draws, g_n_idx_draws, g_n_ic_hits, g_n_ring_wraps, g_n_ring_bytes / 1024);
    if (g_pf_n) { G4("g4: per index-ring draw (%lu sampled): %.1f us vertex upload + attribute pointers, %.1f us index upload, %.1f us glDrawElements", g_pf_n, 1e6 * g_pf_prep / g_pf_n, 1e6 * g_pf_idx / g_pf_n, 1e6 * g_pf_draw / g_pf_n); g_pf_n = 0; g_pf_prep = g_pf_idx = g_pf_draw = 0; }
    if (g_ge_s) { G4("g4: glGetError: %lu calls skipped the driver; %lu sampled took %.3f ms each on average, %.3f ms the slowest", g_ge_n - g_ge_s, g_ge_s, 1e3 * g_ge_t / g_ge_s, 1e3 * g_ge_max); g_ge_s = 0; g_ge_n = 0; g_ge_t = g_ge_max = 0; }
    if (g_cp_n) { G4("g4: glCopyTexSubImage2D: %lu calls, %.2f ms each on average, %.2f ms the slowest (%.0f ms in total in this window)", g_cp_n, 1e3 * g_cp_t / g_cp_n, 1e3 * g_cp_max, 1e3 * g_cp_t); g_cp_n = 0; g_cp_t = g_cp_max = 0; }
}
