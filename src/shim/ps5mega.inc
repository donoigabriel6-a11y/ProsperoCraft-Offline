// ps5mega.inc - chunk "mega-draw" for the PS5 GL shim (included by ps5lwjgl.c when built with MEGA=1; needs VFIX).
//
// Why: on this SDK every glDrawElements costs ~30 us of fixed overhead whatever its size (measured: 60 vs 3000 indices, state changes, multi-draw all the same), while ONE draw
// over a million indices costs ~1.5 ms. Minecraft issues one draw per visible chunk section and layer (hundreds per frame). This layer lets those draws go through unchanged
// from Minecraft's point of view but replaces them by ONE glDrawElements per layer:
//   * every chunk vertex buffer is copied (GPU side, glCopyBufferSubData) once into a shared "arena" buffer, in 128-vertex pages;
//   * the per-chunk camera-relative offset (Minecraft's ChunkOffset uniform) is written into a page table texture (one RGBA32F texel per page) and the chunk vertex shader
//     is rewritten to read it with texelFetch(gl_VertexID >> 7) when ArenaMode_ = 1 (otherwise it uses the uniform as before);
//   * the chunk draws of a layer are recorded (visibility/culling/order stay Minecraft's), and when Minecraft does anything else (the first call that is not
//     glUniform3fv(ChunkOffset) / glBindVertexArray(chunk VAO) / glDrawElements(chunk)) the recorded draws are issued as one indexed draw over a freshly built index list.
// Index lists are PERSISTENT (MEGA_IDXCACHE, the default): rebuilding the visible-chunk index list every frame cost 6-13 ms of upload, because the visible set changes as soon as the camera
// turns. Instead every chunk's indices are written ONCE, when it is placed in the arena, into the slot of the shared index buffer that mirrors its arena pages (page p owns indices
// [p * MG_IDX_PER_PAGE, ...)), and the unused tail of its last page is filled with degenerate triangles so a draw may run straight over it. A frame then draws RUNS: maximal spans of
// page-adjacent visible chunks, one glDrawElements each, with no upload at all. The number of runs per frame is logged; each run costs the usual ~30 us, so this wins as long as the visible
// chunks stay reasonably clustered in the arena. MEGA_IDXCACHE=0 restores the per-frame list.
// The offset table lives in a BUFFER read through a samplerBuffer, not in a 2D texture: on this console every TexSubImage2D into the table behaved like a fresh, zero-filled allocation, so rows
// written in earlier flushes read back as zeros (first as sub-rectangles, then as whole rows) -- that was the chunk flicker. Buffer uploads are the same path the index list already uses and do
// keep their contents; only the changed range is sent, so a frame costs a few hundred bytes.
// The offset table is ONE texture shared by every chunk program (Minecraft has three: solid, cutout and cutout-mipped). It describes the arena, which they all share; a per-program table meant
// each program only ever wrote the entries of the chunks IT drew and kept stale values for the rest, which is exactly the chunk flicker this had.
// Offsets: Minecraft's ChunkOffset is the chunk origin minus the camera, so it changes every frame -- uploading a page table of them per frame was both the dominant
// cost (5-24 ms) and unreliable here (verified: the texture kept zeros or a previous frame's values while the CPU table was correct, which is the chunk flicker). The difference of two
// ChunkOffsets cancels the camera, so what is stored per page is the chunk's offset RELATIVE to one reference chunk, which is constant until the chunk moves inside the arena, and the
// reference's own ChunkOffset goes in a uniform once per flush. Per frame that is one vec3 uniform instead of a megabyte, and a page's texel is written only when its chunk is (re)placed.
// Buffer reuse: every chunk program owns MEGA_RING (6) index-buffer slots (each with its own VAO) and page-table textures, used round robin (a slot is rewritten that many flushes after its last
// draw), and freed arena pages are quarantined for 4 x MEGA_RING flushes. With MEGA_FENCE=1 a GL fence is additionally waited on before a slot is rewritten (off by default: with glthread every ClientWaitSync drains the
// whole GL queue, which measured 3-13 ms per frame). An unchanged visible set (e.g. while paused) reuses the previous index list without any upload.
// Anything unexpected (other vertex layout, non-sequential index buffer, blending = translucent layer, arena full, GL error, ...) falls back to the normal draw call.
// Log lines are tagged "mega:". Counters are printed with the perf line.
#include <stdint.h>
#include <math.h>

#ifndef MEGA_ARENA_MB
#define MEGA_ARENA_MB 256                   // vertex arena size (build knob ARENA_MB=128/256/512): 128 filled up around render distance 11
#endif
#define MG_PAGE_V 128                       // vertices per page
#define MG_STRIDE 32                        // MC BLOCK vertex format
#define MG_PAGES ((int)((long)MEGA_ARENA_MB * 1024 * 1024 / (MG_PAGE_V * MG_STRIDE)))   // 4 KB pages; the page table texture is 256 wide and MG_PAGES / 256 high
#define MG_ROWS (MG_PAGES / 256)
#define MG_ARENA_BYTES ((long)MG_PAGES * MG_PAGE_V * MG_STRIDE)
#define MG_IDX_CAP (8L * 1024 * 1024)       // indices per merged draw (32 MB scratch); anything beyond is drawn individually
#ifndef MEGA_IDXCACHE
#define MEGA_IDXCACHE 1
#endif
#define MG_IDX_PER_PAGE (MG_PAGE_V / 4 * 6)                      // 192: indices of one page's worth of quads
#define MG_IDXBUF_BYTES ((long)MG_PAGES * MG_IDX_PER_PAGE * 4)   // the persistent index buffer mirrors the arena
#define MG_PEND_CAP 8192
#ifndef MEGA_RING
#define MEGA_RING 6                         // slots per chunk program (index buffer + VAO, page-table texture): a slot is rewritten MEGA_RING flushes after its last draw
#endif
#define MG_RING MEGA_RING

typedef struct { unsigned buf; int size, type, stride; long ptr; unsigned char norm, en, integer; } MgAttr;
typedef struct { MgAttr a[8]; unsigned ebo; signed char layout; } MgVao;                 // layout: -1 unknown, 0 not the chunk layout, 1 chunk layout
typedef struct { long size; int page, pages, idx_quads; unsigned char valid, ebo_state, rel_set; int ebo_type; float rel[3]; } MgBuf;   // ebo_state: 0 unknown, 1 sequential quads, 2 not
typedef struct { unsigned vao, vbo; int count, type; float off[3]; } MgPend;
#define MG_RUN_CAP 4096
typedef struct { unsigned char chunk, init, slots_ready, list_valid; int loc_off, loc_mode, loc_pages, loc_base; int run_n, run_first[MG_RUN_CAP], run_pages[MG_RUN_CAP]; unsigned vao[MG_RING], ebo[MG_RING]; void *fe_ebo[MG_RING]; long ebo_cap[MG_RING]; int cur_ebo, cur_tex; uint64_t list_hash; long list_n; } MgProg;

// real GL entry points (set by the lookup table below for wrapped names, by mg_load for the rest)
static struct {
    void (*UseProgram)(unsigned); void (*BindVertexArray)(unsigned); void (*BindBuffer)(int, unsigned); void (*DrawElements)(int, int, int, const void *);
    void (*Uniform3fv)(int, int, const float *); void (*BufferData)(int, long, const void *, int); void (*BufferSubData)(int, long, long, const void *);
    void (*DeleteBuffers)(int, const unsigned *); void (*DeleteVertexArrays)(int, const unsigned *);
    void (*VertexAttribPointer)(unsigned, int, int, unsigned char, int, const void *); void (*VertexAttribIPointer)(unsigned, int, int, int, const void *);
    void (*EnableVAA)(unsigned); void (*DisableVAA)(unsigned); int (*GetUniformLocation)(unsigned, const char *); void (*AttachShader)(unsigned, unsigned); void (*LinkProgram)(unsigned);
    void (*ActiveTexture)(int); void (*Enable)(int); void (*Disable)(int); void (*BindFramebuffer)(int, unsigned); void (*BindTexture)(int, unsigned); void (*Clear)(int);
    void (*DrawArrays)(int, int, int); void (*Viewport)(int, int, int, int); void (*DepthMask)(unsigned char);
    // not wrapped
    void (*GenBuffers)(int, unsigned *); void (*GenVertexArrays)(int, unsigned *); void (*GenTextures)(int, unsigned *); void (*CopyBufferSubData)(int, int, long, long, long);
    void (*GetBufferSubData)(int, long, long, void *); void (*TexImage2D)(int, int, int, int, int, int, int, int, const void *); void (*TexSubImage2D)(int, int, int, int, int, int, int, int, const void *);
    void (*TexParameteri)(int, int, int); void (*Uniform1i)(int, int); unsigned (*GetError)(void);
    void (*Finish)(void); void (*GetTexImage)(int, int, int, int, void *); void (*TexBuffer)(int, int, unsigned);
    void *(*FenceSync)(int, int); int (*ClientWaitSync)(void *, int, unsigned long long); void (*DeleteSync)(void *);   // only used with MEGA_FENCE
} mr;

static MgBuf *g_mg_buf; static unsigned g_mg_buf_n; static MgVao *g_mg_vao; static unsigned g_mg_vao_n; static MgProg *g_mg_prog; static unsigned g_mg_prog_n;
static unsigned char *g_mg_shader_chunk; static unsigned g_mg_shader_n;
static unsigned g_mg_cur_vao, g_mg_cur_prog, g_mg_array, g_mg_ebo0; static int g_mg_active_tex = 0x84C0, g_mg_blend, g_mg_lazy_vao, g_mg_off_valid;
static float g_mg_off[3];
static MgPend g_mg_pend[MG_PEND_CAP]; static unsigned char g_mg_mflag[MG_PEND_CAP]; static int g_mg_npend;
static int g_mg_level = 1, g_mg_soft_err;
static int g_mg_ref_ok; static float g_mg_ref_rel[3];   // the reference chunk: its relative offset is 0 by definition; g_mg_ref_ok says one has been chosen
static unsigned long g_mg_st_texel, g_mg_st_rebase, g_mg_st_rows; static double g_mg_t_off;
static int g_mg_dmin = MG_PAGES, g_mg_dmax = -1;   // range of offset-table entries changed in this flush   // debug levels (touchpad cycles OFF -> 1 -> 2 -> 3): 1 normal, 2 never reuse the index list, 3 = 2 + full page-table upload + glFinish after every merged draw
static int g_mg_on = 1, g_mg_user_off, g_mg_ready, g_mg_loaded, g_mg_nflush, g_mg_in_flush, g_mg_fail_n = 0x7fffffff; static unsigned long g_mg_st_scan;
static int mg_env_off(void) { static int v = -1; if (v < 0) v = getenv("PS5_NOMEGA") != 0; return v; }   // launcher setting "Chunk merging: off" (jvm_host.cpp sets PS5_NOMEGA)
#define MG_ACTIVE() (g_mg_on && !g_mg_user_off && !mg_env_off())
static unsigned g_mg_arena, g_mg_ptex, g_mg_pbuf, g_mg_idxbuf, g_mg_runvao; static unsigned long g_mg_flush_seq;
static float g_mg_pages[MG_PAGES * 4]; static unsigned char g_mg_pagefree[MG_PAGES]; static int g_mg_hint; static unsigned *g_mg_idx;
#define MG_QCAP 8192
#define MG_QFLUSHES (4 * MG_RING)                   // freed arena pages stay unused for this many flushes (24 = 8 frames)
static struct { int page, pages; unsigned long seq; } g_mg_q[MG_QCAP]; static unsigned g_mg_qh, g_mg_qt;   // quarantine of freed arena pages
// statistics (reset every perf window by mg_report)
static unsigned long g_mg_st_flush, g_mg_st_chunks, g_mg_st_idx, g_mg_st_copy, g_mg_st_single, g_mg_st_noalloc, g_mg_st_badvao, g_mg_st_badebo, g_mg_st_blend, g_mg_st_other, g_mg_st_rewritten;
static double g_mg_st_ms, g_mg_t_copy, g_mg_t_gen, g_mg_t_up, g_mg_t_wait, g_mg_t_draw; static unsigned long g_mg_st_runs, g_mg_st_runidx, g_mg_st_idxwrite, g_mg_st_partial; static unsigned long g_mg_st_reuse, g_mg_st_wait; static int g_mg_vao_reason_logged;

#define MG_MAXID (1u << 22)
static MgBuf *mg_buf(unsigned id) {
    if (!id || id >= MG_MAXID) return 0;
    if (id >= g_mg_buf_n) { unsigned n = g_mg_buf_n ? g_mg_buf_n : 1024; while (n <= id) n *= 2; MgBuf *p = (MgBuf *)realloc(g_mg_buf, sizeof(MgBuf) * n); if (!p) return 0; memset(p + g_mg_buf_n, 0, sizeof(MgBuf) * (n - g_mg_buf_n)); g_mg_buf = p; g_mg_buf_n = n; }
    return &g_mg_buf[id];
}
static MgVao *mg_vao(unsigned id) {
    if (!id || id >= MG_MAXID) return 0;
    if (id >= g_mg_vao_n) { unsigned n = g_mg_vao_n ? g_mg_vao_n : 1024; while (n <= id) n *= 2; MgVao *p = (MgVao *)realloc(g_mg_vao, sizeof(MgVao) * n); if (!p) return 0; memset(p + g_mg_vao_n, 0, sizeof(MgVao) * (n - g_mg_vao_n)); g_mg_vao = p; g_mg_vao_n = n; }
    return &g_mg_vao[id];
}
static MgProg *mg_prog(unsigned id) {
    if (!id || id >= MG_MAXID) return 0;
    if (id >= g_mg_prog_n) { unsigned n = g_mg_prog_n ? g_mg_prog_n : 256; while (n <= id) n *= 2; MgProg *p = (MgProg *)realloc(g_mg_prog, sizeof(MgProg) * n); if (!p) return 0; memset(p + g_mg_prog_n, 0, sizeof(MgProg) * (n - g_mg_prog_n)); g_mg_prog = p; g_mg_prog_n = n; }
    if (!g_mg_prog[id].init) { g_mg_prog[id].init = 1; g_mg_prog[id].loc_off = -1; g_mg_prog[id].loc_mode = -1; g_mg_prog[id].loc_pages = -1; g_mg_prog[id].loc_base = -1; }
    return &g_mg_prog[id];
}
static void mg_mark_shader(unsigned id, int chunk) {
    if (!id || id >= MG_MAXID) return;
    if (id >= g_mg_shader_n) { unsigned n = g_mg_shader_n ? g_mg_shader_n : 256; while (n <= id) n *= 2; unsigned char *p = (unsigned char *)realloc(g_mg_shader_chunk, n); if (!p) return; memset(p + g_mg_shader_n, 0, n - g_mg_shader_n); g_mg_shader_chunk = p; g_mg_shader_n = n; }
    g_mg_shader_chunk[id] = (unsigned char)chunk;
}

typedef struct { const char *name; void *wrap; void **real; } MgFn;
static const MgFn g_mg_fns[];
static void mg_load(void) {
    if (g_mg_loaded) return; g_mg_loaded = 1;
    for (const MgFn *f = g_mg_fns; f->name; f++) if (!*f->real) *f->real = (void *)eglGetProcAddress(f->name);   // wrapped functions LWJGL has not asked for yet
#define L(f) *(void **)&mr.f = (void *)eglGetProcAddress("gl" #f)
    L(GenBuffers); L(GenVertexArrays); L(GenTextures); L(CopyBufferSubData); L(GetBufferSubData); L(TexImage2D); L(TexSubImage2D); L(TexParameteri); L(Uniform1i); L(GetError); L(Finish); L(GetTexImage); L(TexBuffer);   // GetTexImage only for MEGA_VERIFY
#ifdef MEGA_FENCE
    L(FenceSync); L(ClientWaitSync); L(DeleteSync);
#endif
#undef L
    if (!mr.GenBuffers || !mr.GenVertexArrays || !mr.GenTextures || !mr.CopyBufferSubData || !mr.GetBufferSubData || !mr.TexImage2D || !mr.TexSubImage2D || !mr.TexParameteri || !mr.Uniform1i || !mr.GetError || !mr.Finish || !mr.TexBuffer
#ifdef MEGA_FENCE
        || !mr.FenceSync || !mr.ClientWaitSync || !mr.DeleteSync
#endif
        ) {
        g_mg_on = 0; TR("mega: DISABLED, a required GL function is missing (copy %p getsub %p texsub %p fence %p wait %p)", (void *)mr.CopyBufferSubData, (void *)mr.GetBufferSubData, (void *)mr.TexSubImage2D, (void *)mr.FenceSync, (void *)mr.ClientWaitSync);
    }
}
static void mg_err_soft(const char *what) { unsigned e = mr.GetError(); if (e && g_mg_soft_err++ < 10) TR("mega: GL error 0x%x after %s (not fatal, merging stays on)", e, what); }
static int mg_err(const char *what) { unsigned e = mr.GetError(); if (e) { TR("mega: GL error 0x%x after %s -> merging disabled", e, what); g_mg_on = 0; return 1; } return 0; }

// ---- one-time creation of the arena (on the first flush); per chunk program the slots are created by mg_slots() ----
static int mg_init(void) {
    if (g_mg_ready) return 1; if (!g_mg_on) return 0;
    mg_load(); if (!g_mg_on) return 0;
    g_mg_idx = (unsigned *)malloc(sizeof(unsigned) * (size_t)MG_IDX_CAP);
    if (!g_mg_idx) { g_mg_on = 0; TR("mega: DISABLED, out of memory for the index scratch"); return 0; }
    memset(g_mg_pagefree, 1, sizeof g_mg_pagefree);
    mr.GetError();   // clear stale errors from Minecraft's own calls
    mr.GenBuffers(1, &g_mg_arena);
    mr.BindBuffer(0x8892, g_mg_arena); mr.BufferData(0x8892, MG_ARENA_BYTES, 0, 0x88E4);
#if MEGA_IDXCACHE
    mr.GenBuffers(1, &g_mg_idxbuf); mr.GenVertexArrays(1, &g_mg_runvao);
    mr.BindVertexArray(g_mg_runvao);
    for (unsigned a = 0; a < 5; a++) mr.EnableVAA(a);
    mr.VertexAttribPointer(0, 3, 0x1406, 0, MG_STRIDE, (const void *)0);
    mr.VertexAttribPointer(1, 4, 0x1401, 1, MG_STRIDE, (const void *)12);
    mr.VertexAttribPointer(2, 2, 0x1406, 0, MG_STRIDE, (const void *)16);
    mr.VertexAttribIPointer(3, 1, 0x1405, MG_STRIDE, (const void *)24);
    mr.VertexAttribPointer(4, 4, 0x1400, 1, MG_STRIDE, (const void *)28);
    mr.BindBuffer(0x8893, g_mg_idxbuf); mr.BufferData(0x8893, MG_IDXBUF_BYTES, 0, 0x88E4);
    mr.BindVertexArray(0);
#endif
    mr.BindBuffer(0x8892, g_mg_array);                      // give Minecraft its array-buffer binding back
    if (mg_err("arena creation")) return 0;
    g_mg_ready = 1; TR("mega: ready (arena %ld MB in %d pages of %d vertices, persistent index buffer %ld MB, arena buffer %u)", MG_ARENA_BYTES >> 20, MG_PAGES, MG_PAGE_V, MG_IDXBUF_BYTES >> 20, g_mg_arena);
    return 1;
}
static int mg_slots(MgProg *pr) {   // MG_RING index-buffer slots (VAO + EBO) and page-table textures for this chunk program
    if (pr->slots_ready) return 1;
    mr.GenVertexArrays(MG_RING, pr->vao); mr.GenBuffers(MG_RING, pr->ebo);
    mr.BindBuffer(0x8892, g_mg_arena);
    for (int s = 0; s < MG_RING; s++) {
        mr.BindVertexArray(pr->vao[s]);
        for (unsigned a = 0; a < 5; a++) mr.EnableVAA(a);
        mr.VertexAttribPointer(0, 3, 0x1406, 0, MG_STRIDE, (const void *)0);
        mr.VertexAttribPointer(1, 4, 0x1401, 1, MG_STRIDE, (const void *)12);
        mr.VertexAttribPointer(2, 2, 0x1406, 0, MG_STRIDE, (const void *)16);
        mr.VertexAttribIPointer(3, 1, 0x1405, MG_STRIDE, (const void *)24);
        mr.VertexAttribPointer(4, 4, 0x1400, 1, MG_STRIDE, (const void *)28);
        mr.BindBuffer(0x8893, pr->ebo[s]); pr->ebo_cap[s] = (long)sizeof(unsigned) * 262144; mr.BufferData(0x8893, pr->ebo_cap[s], 0, 0x88E0);
    }
    mr.BindVertexArray(0); mr.BindBuffer(0x8892, g_mg_array);
    if (!g_mg_ptex) {   // one offset table for all chunk programs: it describes the shared arena. GL_TEXTURE_BUFFER (0x8C2A) over a plain buffer, fetched with texelFetch(samplerBuffer, page)
        mr.GenBuffers(1, &g_mg_pbuf); mr.BindBuffer(0x8C2A, g_mg_pbuf); mr.BufferData(0x8C2A, (long)MG_PAGES * 16, g_mg_pages, 0x88E8);
        mr.GenTextures(1, &g_mg_ptex); mr.ActiveTexture(0x84C7); mr.BindTexture(0x8C2A, g_mg_ptex);
        mr.TexBuffer(0x8C2A, 0x8814, g_mg_pbuf);   // RGBA32F view of the buffer
        mr.ActiveTexture(g_mg_active_tex); mr.BindBuffer(0x8C2A, 0);
    }
    if (mg_err("slot creation")) return 0;
    pr->slots_ready = 1; pr->cur_ebo = 0; pr->list_valid = 0; return 1;
}
#ifndef MEGA_FENCE
static void mg_fence_wait(void **f) { (void)f; }
static void mg_fence_set(void **f) { (void)f; }
#else
static void mg_fence_wait(void **f) {   // wait until the GPU has finished the last draw that used a slot, then drop the fence
    if (!*f) return; double t0 = now_s();
    int r = mr.ClientWaitSync(*f, 1, 200000000ULL); mr.DeleteSync(*f); *f = 0; double dt = now_s() - t0; g_mg_t_wait += dt * 1e3;
    if (dt > 0.0002) g_mg_st_wait++;
    if (r == 0x911B || r == 0x911D) { static int logged; if (logged++ < 5) TR("mega: fence wait returned 0x%x after %.1f ms (continuing)", r, dt * 1e3); }
}
static void mg_fence_set(void **f) { if (*f) mr.DeleteSync(*f); *f = mr.FenceSync(0x9117, 0); }
#endif

// ---- page allocator (first fit, byte per page) ----
static void mg_q_release(int force) {   // return quarantined pages whose last possible use is long done (MG_QFLUSHES flushes = the GPU cannot be that far behind)
    while (g_mg_qh != g_mg_qt && (force || g_mg_q[g_mg_qh % MG_QCAP].seq + MG_QFLUSHES <= g_mg_flush_seq)) {
        int p = g_mg_q[g_mg_qh % MG_QCAP].page, n = g_mg_q[g_mg_qh % MG_QCAP].pages; memset(g_mg_pagefree + p, 1, (size_t)n); if (p < g_mg_hint) g_mg_hint = p; g_mg_qh++; force = 0; g_mg_fail_n = 0x7fffffff;
    }
}
static int mg_alloc_pages(int n) {
    if (n <= 0 || n > MG_PAGES) return -1;
    mg_q_release(0);
    if (n >= g_mg_fail_n) return -1;   // an allocation this big failed and nothing was freed since: do not rescan the whole arena for every chunk of every frame
    for (int pass = 0; pass < 2; pass++) {
        int start = pass ? 0 : g_mg_hint, end = pass ? g_mg_hint + n : MG_PAGES, run = 0;
        if (end > MG_PAGES) end = MG_PAGES;
        g_mg_st_scan += (unsigned long)(end - start);
        for (int i = start; i < end; i++) {
            if (g_mg_pagefree[i]) { if (++run == n) { int first = i - n + 1; memset(g_mg_pagefree + first, 0, (size_t)n); g_mg_hint = first + n; if (g_mg_hint >= MG_PAGES) g_mg_hint = 0; return first; } } else run = 0;
        }
    }
    if (n < g_mg_fail_n) g_mg_fail_n = n;
    return -1;
}
static void mg_free_slot(MgBuf *b) {
    if (b->pages > 0 && b->page >= 0) {   // not reusable yet: the GPU may still be reading these pages for a draw already submitted
        if (g_mg_qt - g_mg_qh >= MG_QCAP) mg_q_release(1);
        g_mg_q[g_mg_qt % MG_QCAP].page = b->page; g_mg_q[g_mg_qt % MG_QCAP].pages = b->pages; g_mg_q[g_mg_qt % MG_QCAP].seq = g_mg_flush_seq; g_mg_qt++;
    }
    b->pages = 0; b->page = 0; b->valid = 0; b->rel_set = 0;   // its texel belongs to whatever takes these pages next
}

// ---- layout / index-buffer validation ----
static int mg_check_layout(const MgVao *v) {
    static const struct { int size, type; unsigned char norm, integer; long ptr; } want[5] = { { 3, 0x1406, 0, 0, 0 }, { 4, 0x1401, 1, 0, 12 }, { 2, 0x1406, 0, 0, 16 }, { 1, 0x1405, 0, 1, 24 }, { 4, 0x1400, 1, 0, 28 } };
    unsigned B = v->a[0].buf; if (!B) return 0;
    for (int i = 0; i < 5; i++) { const MgAttr *a = &v->a[i]; if (!a->en || a->buf != B || a->stride != MG_STRIDE || a->size != want[i].size || a->type != want[i].type || a->norm != want[i].norm || a->integer != want[i].integer || a->ptr != want[i].ptr) return 0; }
    for (int i = 5; i < 8; i++) if (v->a[i].en) return 0;
    return 1;
}
static int mg_verify_ebo(unsigned ebo, int type, long size) {
    int sz = type == 0x1403 ? 2 : 4; if (size < 24 * sz) return 2;
    unsigned char tmp[96]; mr.BindBuffer(0x8F36, ebo); mr.GetBufferSubData(0x8F36, 0, 24 * sz, tmp); mr.BindBuffer(0x8F36, 0);
    for (int i = 0; i < 24; i++) {
        static const int pat[6] = { 0, 1, 2, 2, 3, 0 }; unsigned want = (unsigned)((i / 6) * 4 + pat[i % 6]); unsigned got = sz == 2 ? (unsigned)((unsigned short *)tmp)[i] : ((unsigned *)tmp)[i];
        if (got != want) { TR("mega: index buffer %u (type 0x%x) is not the sequential quad pattern (index %d = %u, expected %u): not merged", ebo, type, i, got, want); return 2; }
    }
    return 1;
}

#ifdef MEGA_VERIFY
// Data check of a merged draw (build knob VERIFY=1, every 30th flush): reads back the arena pages, the index slot and the page-table texture the draw just used and compares them with the
// source chunk buffers and the recorded offsets. Mismatches are logged ("mega-verify:"). A clean run means the data is right and any artifact comes from how the GPU consumes it.
static unsigned long g_mg_v_runs, g_mg_v_bad;
static void mg_verify(MgProg *pr, int s, int ts, long used) {
    (void)ts;
    int picks[4], np = 0, cand[MG_PEND_CAP], nc = 0;
    for (int i = 0; i < g_mg_npend; i++) if (g_mg_mflag[i]) cand[nc++] = i;
    if (nc == 0) return;
    picks[np++] = cand[0]; if (nc > 1) picks[np++] = cand[nc - 1]; if (nc > 3) { picks[np++] = cand[nc / 3]; picks[np++] = cand[2 * nc / 3]; }
    int bad_arena = 0, bad_idx = 0, bad_off = 0; g_mg_v_runs++;
    static float *tex; static unsigned char *a, *b; static size_t acap; static int tex_ok = 1;
    if (!tex) tex = (float *)malloc(sizeof(float) * 4 * 256 * MG_ROWS);
    if (tex && tex_ok) {   // the offset table can only be checked if this SDK really reads buffers back
        memset(tex, 0xCD, sizeof(float) * 4 * 256 * MG_ROWS); mr.GetError();
        mr.BindBuffer(0x8F36, g_mg_pbuf); mr.GetBufferSubData(0x8F36, 0, (long)MG_PAGES * 16, tex); mr.BindBuffer(0x8F36, 0);
        unsigned e = mr.GetError(); int untouched = 1; unsigned char *raw = (unsigned char *)tex;
        for (size_t k = 0; k < sizeof(float) * 4 * 256 * MG_ROWS && untouched; k += 4096) if (raw[k] != 0xCD) untouched = 0;
        if (e || untouched) { tex_ok = 0; TR("mega-verify: the offset buffer cannot be read back on this SDK (error 0x%x, buffer %s): offsets are no longer checked, arena and index list still are", e, untouched ? "untouched" : "written"); }
    }
    if (!tex_ok) tex = 0;
    long idxpos = 0; int first_bad = -1;
    for (int k = 0; k < g_mg_npend; k++) {   // position of each picked chunk inside the index list
        if (!g_mg_mflag[k]) continue;
        for (int pi = 0; pi < np; pi++) if (picks[pi] == k) {
            MgPend *e = &g_mg_pend[k]; MgBuf *bf = mg_buf(e->vbo); long bytes = bf->size; if ((size_t)bytes > acap) { acap = (size_t)bytes * 2; a = (unsigned char *)realloc(a, acap); b = (unsigned char *)realloc(b, acap); }
            if (a && b) {
                mr.BindBuffer(0x8F36, g_mg_arena); mr.GetBufferSubData(0x8F36, (long)bf->page * MG_PAGE_V * MG_STRIDE, bytes, a);
                mr.BindBuffer(0x8F36, e->vbo); mr.GetBufferSubData(0x8F36, 0, bytes, b); mr.BindBuffer(0x8F36, 0);
                if (memcmp(a, b, (size_t)bytes)) { bad_arena++; if (first_bad < 0) first_bad = k; }
            }
            int q = e->count / 6; static unsigned ix[12];
#if MEGA_IDXCACHE
            long base_off = (long)bf->page * MG_IDX_PER_PAGE;   // the chunk owns the index block that mirrors its arena pages
            mr.BindBuffer(0x8F36, g_mg_idxbuf); mr.GetBufferSubData(0x8F36, base_off * 4, 12 * 4, ix); mr.GetBufferSubData(0x8F36, (base_off + (long)(q - 1) * 6) * 4, 6 * 4, ix + 6); mr.BindBuffer(0x8F36, 0);
#else
            mr.BindBuffer(0x8F36, pr->ebo[s]); mr.GetBufferSubData(0x8F36, idxpos * 4, 12 * 4, ix); mr.GetBufferSubData(0x8F36, (idxpos + (long)(q - 1) * 6) * 4, 6 * 4, ix + 6); mr.BindBuffer(0x8F36, 0);
#endif
            { static const int pat[6] = { 0, 1, 2, 2, 3, 0 }; unsigned base = (unsigned)bf->page * MG_PAGE_V; for (int j = 0; j < 12; j++) { unsigned want = base + (unsigned)((j / 6) * 4 + pat[j % 6]); if (j >= 6) want = base + (unsigned)((q - 1) * 4 + pat[j % 6]); if (q == 1 && j >= 6) want = base + (unsigned)pat[j % 6]; if (ix[j] != want) { bad_idx++; break; } } }
            if (tex && bf->rel_set) for (int p = 0; p < bf->pages; p++) { float *w = tex + ((size_t)bf->page + p) * 4; if (fabsf(w[0] - bf->rel[0]) > 0.01f || fabsf(w[1] - bf->rel[1]) > 0.01f || fabsf(w[2] - bf->rel[2]) > 0.01f) { bad_off++; break; } }
        }
        idxpos += g_mg_pend[k].count;
    }
    if (bad_arena || bad_idx || bad_off) { g_mg_v_bad++; TR("mega-verify: flush %lu (%d chunks, %ld idx): MISMATCH arena %d index %d offsets %d (of %d sampled)", g_mg_flush_seq, nc, used, bad_arena, bad_idx, bad_off, np);
        if (bad_off && tex && g_mg_v_bad <= 8) {   // what exactly differs: the value in the texture, the value that was recorded, and what the CPU-side page table says (so we can tell a bad upload from a stale page entry)
            for (int pi = 0; pi < np; pi++) { int k = picks[pi]; if (k < 0 || !g_mg_mflag[k]) continue; MgPend *e = &g_mg_pend[k]; MgBuf *bf = mg_buf(e->vbo); if (!bf) continue;
                float *w = tex + (size_t)bf->page * 4, *c = g_mg_pages + (size_t)bf->page * 4;
                if (bf->rel_set && (fabsf(w[0] - bf->rel[0]) > 0.01f || fabsf(w[1] - bf->rel[1]) > 0.01f || fabsf(w[2] - bf->rel[2]) > 0.01f))
                    TR("mega-verify:   chunk vbo %u page %d (%d pages): offset table (%.2f %.2f %.2f) vs relative offset (%.2f %.2f %.2f) vs CPU table (%.2f %.2f %.2f)", e->vbo, bf->page, bf->pages, w[0], w[1], w[2], bf->rel[0], bf->rel[1], bf->rel[2], c[0], c[1], c[2]);
            }
        } }
    else if ((g_mg_v_runs & 15) == 1) TR("mega-verify: %lu checks so far, %lu with mismatches (last: %d sampled chunks all consistent)", g_mg_v_runs, g_mg_v_bad, np);
}
#endif
// ---- flush: build the index list + page table of the recorded chunk draws and issue ONE draw ----
static void mg_single_draw(const MgPend *e, const MgProg *pr) {   // normal draw of one recorded chunk
    mr.BindVertexArray(e->vao); if (pr && pr->loc_off >= 0) mr.Uniform3fv(pr->loc_off, 1, e->off); mr.DrawElements(4, e->count, e->type, 0); g_mg_st_single++;
}
static void mg_flush(void) {
    if (g_mg_npend == 0 || g_mg_in_flush) return;
    g_mg_in_flush = 1; double t0 = now_s(); MgProg *pr = mg_prog(g_mg_cur_prog);
    int ok = pr && MG_ACTIVE() && mg_init() && pr->loc_mode >= 0 && pr->loc_pages >= 0 && mg_slots(pr);
    long used = 0; int merged = 0; unsigned char *mflag = g_mg_mflag;
    if (ok) {
        double tc = now_s();
        for (int i = 0; i < g_mg_npend; i++) {
            MgPend *e = &g_mg_pend[i]; MgBuf *b = mg_buf(e->vbo); mflag[i] = 0; if (!b) continue;
            if (!b->valid) {   // (re)allocate a slot and copy the vertex data into the arena, GPU side
                mg_free_slot(b); int pages = (int)((b->size / MG_STRIDE + MG_PAGE_V - 1) / MG_PAGE_V); int first = mg_alloc_pages(pages);
                if (first < 0) { g_mg_st_noalloc++; continue; }
                b->page = first; b->pages = pages; mr.BindBuffer(0x8F36, e->vbo); mr.BindBuffer(0x8F37, g_mg_arena); mr.CopyBufferSubData(0x8F36, 0x8F37, 0, (long)first * MG_PAGE_V * MG_STRIDE, b->size);
                mr.BindBuffer(0x8F36, 0); mr.BindBuffer(0x8F37, 0); b->valid = 1; g_mg_st_copy++;
#if MEGA_IDXCACHE
                {   // the chunk's indices, written once: its quads, then degenerate triangles filling the rest of its last page so a run may sweep over the gap
                    int quads = (int)(b->size / MG_STRIDE / 4), slots = pages * (MG_PAGE_V / 4); unsigned bse = (unsigned)first * MG_PAGE_V; unsigned *o = g_mg_idx;
                    static const int pat[6] = { 0, 1, 2, 2, 3, 0 };
                    for (int j = 0; j < quads; j++, o += 6) { unsigned v = bse + (unsigned)j * 4; for (int k = 0; k < 6; k++) o[k] = v + (unsigned)pat[k]; }
                    for (int j = quads; j < slots; j++, o += 6) for (int k = 0; k < 6; k++) o[k] = bse;   // degenerate: all three corners the same vertex
                    mr.BindVertexArray(g_mg_runvao);
                    mr.BufferSubData(0x8893, (long)first * MG_IDX_PER_PAGE * 4, (long)slots * 6 * 4, g_mg_idx);
                    mr.BindVertexArray(g_mg_cur_vao); g_mg_st_idxwrite += (unsigned long)slots * 6;
                    b->idx_quads = quads;
                }
#endif
            }
#if MEGA_IDXCACHE
            // the persistent indices cover the whole chunk buffer, so a draw that uses only part of it cannot go through a run: let it fall back to its own draw
            if (e->count / 6 != b->idx_quads) { g_mg_st_partial++; continue; }
#endif
            mflag[i] = 1;
        }
        g_mg_t_copy += 1e3 * (now_s() - tc);
        // which chunks go into this draw, how many indices, and a hash of the list (pages + counts, in order): an unchanged list needs no new index buffer
        long cap = MG_IDX_CAP, n = 0; uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < g_mg_npend; i++) {
            if (!mflag[i]) continue; MgPend *e = &g_mg_pend[i]; MgBuf *b = mg_buf(e->vbo); if (n + (long)(e->count) > cap) { mflag[i] = 0; continue; }
            n += e->count; merged++; h = (h ^ (uint64_t)(unsigned)b->page) * 1099511628211ULL; h = (h ^ (uint64_t)(unsigned)e->count) * 1099511628211ULL;
        }
        used = n;
        if (used > 0) {
            double tg = now_s();
            // base: the ChunkOffset the reference chunk would have this frame, derived from any merged chunk whose relative offset is already known
            float base[3]; int have_base = 0;
            for (int i = 0; i < g_mg_npend && !have_base; i++) { if (!mflag[i]) continue; MgBuf *b2 = mg_buf(g_mg_pend[i].vbo);
                if (b2 && b2->rel_set) { base[0] = g_mg_pend[i].off[0] - b2->rel[0]; base[1] = g_mg_pend[i].off[1] - b2->rel[1]; base[2] = g_mg_pend[i].off[2] - b2->rel[2]; have_base = 1; } }
            if (!have_base) { for (int i = 0; i < g_mg_npend; i++) if (mflag[i]) { base[0] = g_mg_pend[i].off[0]; base[1] = g_mg_pend[i].off[1]; base[2] = g_mg_pend[i].off[2]; have_base = 1; break; } }
            // every merged chunk: make sure its texel holds (its ChunkOffset - base), writing only the ones that are new or have drifted (a relocated chunk, or Minecraft re-centring its coordinates)
            for (int i = 0; i < g_mg_npend; i++) {
                if (!mflag[i]) continue; MgPend *e = &g_mg_pend[i]; MgBuf *b2 = mg_buf(e->vbo); if (!b2) continue;
                float r[3] = { e->off[0] - base[0], e->off[1] - base[1], e->off[2] - base[2] };
                int drift = !b2->rel_set || fabsf(r[0] - b2->rel[0]) > 0.01f || fabsf(r[1] - b2->rel[1]) > 0.01f || fabsf(r[2] - b2->rel[2]) > 0.01f;
                if (drift) {
                    b2->rel[0] = r[0]; b2->rel[1] = r[1]; b2->rel[2] = r[2]; b2->rel_set = 1;
                    for (int p = 0; p < b2->pages; p++) { float *w = g_mg_pages + ((size_t)b2->page + p) * 4; w[0] = r[0]; w[1] = r[1]; w[2] = r[2]; w[3] = 0; }
                    if (b2->page < g_mg_dmin) g_mg_dmin = b2->page;                       // widen the range to upload after this loop
                    if (b2->page + b2->pages - 1 > g_mg_dmax) g_mg_dmax = b2->page + b2->pages - 1;
                    g_mg_st_texel += (unsigned long)b2->pages;
                }
            }
            if (g_mg_dmax >= g_mg_dmin) {   // one buffer update covering everything that changed in this flush
                mr.BindBuffer(0x8C2A, g_mg_pbuf);
                mr.BufferSubData(0x8C2A, (long)g_mg_dmin * 16, (long)(g_mg_dmax - g_mg_dmin + 1) * 16, g_mg_pages + (size_t)g_mg_dmin * 4);
                mr.BindBuffer(0x8C2A, 0); g_mg_st_rows += (unsigned long)(g_mg_dmax - g_mg_dmin + 1);
                g_mg_dmin = MG_PAGES; g_mg_dmax = -1;
            }
            g_mg_t_off += 1e3 * (now_s() - tg);
#if MEGA_IDXCACHE
            // runs: the merged chunks sorted by arena page, then maximal spans whose pages touch. Each run is one draw over the persistent index buffer, with nothing uploaded.
            {   int n = 0; static int ord[MG_PEND_CAP];
                for (int i = 0; i < g_mg_npend; i++) if (mflag[i]) ord[n++] = i;
                for (int i = 1; i < n; i++) { int t = ord[i], j = i - 1; MgBuf *bt = mg_buf(g_mg_pend[t].vbo);   // insertion sort: the list is nearly sorted frame to frame
                    while (j >= 0 && mg_buf(g_mg_pend[ord[j]].vbo)->page > bt->page) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = t; }
                pr->run_n = 0;
                for (int i = 0; i < n; ) {
                    MgBuf *b0 = mg_buf(g_mg_pend[ord[i]].vbo); int first = b0->page, end = b0->page + b0->pages; int j = i + 1;
                    while (j < n) { MgBuf *bj = mg_buf(g_mg_pend[ord[j]].vbo); if (bj->page != end) break; end = bj->page + bj->pages; j++; }
                    if (pr->run_n < MG_RUN_CAP) { pr->run_first[pr->run_n] = first; pr->run_pages[pr->run_n] = end - first; pr->run_n++; }
                    i = j;
                }
                g_mg_st_runs += (unsigned long)pr->run_n;
            }
#else
            int reuse = g_mg_level == 1 && pr->list_valid && pr->list_hash == h && pr->list_n == used;
            if (!reuse) {
                pr->cur_ebo = (pr->cur_ebo + 1) % MG_RING; int s = pr->cur_ebo; mg_fence_wait(&pr->fe_ebo[s]);
                double tg2 = now_s();
                unsigned *o = g_mg_idx;
                for (int i = 0; i < g_mg_npend; i++) {
                    if (!mflag[i]) continue; MgPend *e = &g_mg_pend[i]; MgBuf *b2 = mg_buf(e->vbo); int q = e->count / 6; unsigned bse = (unsigned)b2->page * MG_PAGE_V;
                    for (int j = 0; j < q; j++, o += 6) { unsigned v = bse + (unsigned)j * 4; o[0] = v; o[1] = v + 1; o[2] = v + 2; o[3] = v + 2; o[4] = v + 3; o[5] = v; }
                }
                g_mg_t_gen += 1e3 * (now_s() - tg2); double tu = now_s();
                mr.BindVertexArray(pr->vao[s]);
                if ((long)sizeof(unsigned) * used > pr->ebo_cap[s]) { long nc = (long)sizeof(unsigned) * used; nc += nc / 4; mr.BufferData(0x8893, nc, 0, 0x88E0); pr->ebo_cap[s] = nc; }
                mr.BufferSubData(0x8893, 0, (long)sizeof(unsigned) * used, g_mg_idx);
                g_mg_t_up += 1e3 * (now_s() - tu); pr->list_hash = h; pr->list_n = used; pr->list_valid = 1; g_mg_st_idx += (unsigned long)used;
            } else g_mg_st_reuse++;
#endif
            if (have_base && pr->loc_base >= 0) mr.Uniform3fv(pr->loc_base, 1, base);
            (void)g_mg_ref_ok; (void)g_mg_ref_rel; (void)g_mg_st_rebase;   // the only per-frame offset data: one vec3
        }
    }
    for (int i = 0; i < g_mg_npend; i++) if (!ok || !mflag[i]) mg_single_draw(&g_mg_pend[i], pr);   // anything that could not be merged: the normal draw
    if (ok && used > 0) {
        double td = now_s(); int s = pr->cur_ebo;
        mr.ActiveTexture(0x84C7); mr.BindTexture(0x8C2A, g_mg_ptex); mr.ActiveTexture(g_mg_active_tex);   // the offset table must be the buffer texture on unit 7 for this draw
#if MEGA_IDXCACHE
        mr.BindVertexArray(g_mg_runvao);
#else
        mr.BindVertexArray(pr->vao[s]);
#endif
#ifdef PS5_GPUTIME
        gt_mark(10 + 2 * (int)(pr - g_mg_prog) % 8);   // timestamp before the merged draw (tag by program)
#endif
        mr.Uniform1i(pr->loc_mode, 1);
#if MEGA_IDXCACHE
        for (int r = 0; r < pr->run_n; r++) { long off = (long)pr->run_first[r] * MG_IDX_PER_PAGE * 4; int cnt = pr->run_pages[r] * MG_IDX_PER_PAGE;
            mr.DrawElements(4, cnt, 0x1405, (const void *)off); g_mg_st_runidx += (unsigned long)cnt; }
#else
        mr.DrawElements(4, (int)used, 0x1405, 0);
#endif
        mr.Uniform1i(pr->loc_mode, 0);
#ifdef PS5_GPUTIME
        gt_mark(11 + 2 * (int)(pr - g_mg_prog) % 8);   // ... and after it
#endif
        if (g_mg_level >= 3) mr.Finish();   // debug level 3: the GPU is completely done with this draw before anything is reused
        mg_fence_set(&pr->fe_ebo[s]); g_mg_flush_seq++;
        if ((g_mg_flush_seq & 63) == 0) mg_err_soft("merged draws (periodic check)");
        g_mg_t_draw += 1e3 * (now_s() - td); g_mg_st_chunks += (unsigned long)merged;
#ifdef MEGA_VERIFY
        if (g_mg_flush_seq % 30 == 0) mg_verify(pr, s, 0, used);
#endif
        if (g_mg_nflush < 12) { g_mg_nflush++; TR("mega: flush #%d: %d chunk draws merged into one draw of %ld indices (%d drawn individually)", g_mg_nflush, merged, used, g_mg_npend - merged); mg_err("first merged draws"); }
    }
    g_mg_st_flush++; g_mg_npend = 0;
    mr.BindVertexArray(g_mg_cur_vao); g_mg_lazy_vao = 0;      // the real VAO binding is now what Minecraft expects
    g_mg_st_ms += 1e3 * (now_s() - t0); g_mg_in_flush = 0;
}
static void mg_sync(void) {   // called before any call that is not part of the chunk draw loop: finish recorded draws, bring the real GL state up to date
    if (g_mg_npend) mg_flush();
    if (g_mg_lazy_vao) { mr.BindVertexArray(g_mg_cur_vao); g_mg_lazy_vao = 0; }
}
static int mg_chunk_mode(void) { MgProg *p = (MG_ACTIVE() && g_mg_cur_prog) ? mg_prog(g_mg_cur_prog) : 0; return p && p->chunk && p->loc_off >= 0 && p->loc_mode >= 0; }

// ---- wrappers ----
static void mw_UseProgram(unsigned p) { mg_sync(); mr.UseProgram(p); g_mg_cur_prog = p; }
static void mw_BindVertexArray(unsigned a) {
    if (a && mg_chunk_mode() && !g_mg_blend) { g_mg_cur_vao = a; g_mg_lazy_vao = 1; return; }   // lazy: only needed if this chunk ends up drawn individually
    mg_sync(); mr.BindVertexArray(a); g_mg_cur_vao = a;
}
static void mw_BindBuffer(int t, unsigned id) {
    mg_sync(); mr.BindBuffer(t, id);
    if (t == 0x8892) g_mg_array = id; else if (t == 0x8893) { if (g_mg_cur_vao) { MgVao *v = mg_vao(g_mg_cur_vao); if (v) { v->ebo = id; v->layout = -1; } } else g_mg_ebo0 = id; }
}
static unsigned mg_cur_ebo(void) { if (g_mg_cur_vao) { MgVao *v = mg_vao(g_mg_cur_vao); return v ? v->ebo : 0; } return g_mg_ebo0; }
static void mw_BufferData(int t, long size, const void *data, int usage) {
    mg_sync(); unsigned id = t == 0x8892 ? g_mg_array : t == 0x8893 ? mg_cur_ebo() : 0;
    if (id) { MgBuf *b = mg_buf(id); if (b) { mg_free_slot(b); b->size = size; b->ebo_state = 0; } }
    mr.BufferData(t, size, data, usage);
}
static void mw_BufferSubData(int t, long off, long size, const void *data) {
    mg_sync(); unsigned id = t == 0x8892 ? g_mg_array : t == 0x8893 ? mg_cur_ebo() : 0;
    if (id) { MgBuf *b = mg_buf(id); if (b) { b->valid = 0; b->ebo_state = 0; } }
    mr.BufferSubData(t, off, size, data);
}
static void mw_DeleteBuffers(int n, const unsigned *ids) {
    mg_sync(); for (int i = 0; i < n; i++) { MgBuf *b = mg_buf(ids[i]); if (b) { mg_free_slot(b); memset(b, 0, sizeof *b); } if (ids[i] == g_mg_array) g_mg_array = 0; }
    mr.DeleteBuffers(n, ids);
}
static void mw_DeleteVertexArrays(int n, const unsigned *ids) {
    mg_sync(); for (int i = 0; i < n; i++) { MgVao *v = mg_vao(ids[i]); if (v) memset(v, 0, sizeof *v), v->layout = -1; if (ids[i] == g_mg_cur_vao) g_mg_cur_vao = 0; }
    mr.DeleteVertexArrays(n, ids);
}
static void mg_note_attr(unsigned idx, int size, int type, unsigned char norm, int stride, const void *ptr, int integer) {
    MgVao *v = mg_vao(g_mg_cur_vao); if (!v || idx >= 8) return; MgAttr *a = &v->a[idx]; a->buf = g_mg_array; a->size = size; a->type = type; a->norm = norm; a->stride = stride; a->ptr = (long)ptr; a->integer = (unsigned char)integer; v->layout = -1;
}
static void mw_VertexAttribPointer(unsigned i, int size, int type, unsigned char norm, int stride, const void *ptr) {
    vf_log_attr("glVertexAttribPointer", i, size, type, norm, stride, (long)ptr);
    if (sod_fptr(i, size, type, norm, stride, ptr, mr.VertexAttribIPointer)) return;
    mg_sync(); if (size == 3 && type == 0x1400 && norm) size = 4;   // VFIX: 3-byte normal -> 4 bytes
    mg_note_attr(i, size, type, norm, stride, ptr, 0); mr.VertexAttribPointer(i, size, type, norm, stride, ptr);
}
static void mw_VertexAttribIPointer(unsigned i, int size, int type, int stride, const void *ptr) {
    vf_log_attr("glVertexAttribIPointer", i, size, type, 0, stride, (long)ptr);
    mg_sync(); if (size == 2 && type == 0x1402) { size = 1; type = 0x1405; }   // VFIX: two shorts -> one uint
    else if (sod_iptr(i, &size, &type, stride, ptr, mr.VertexAttribIPointer)) return;   // Sodium's integer attributes -> R32_UINT (this hook is in front of gw_G_VATTRI when MEGA=1)
    mg_note_attr(i, size, type, 0, stride, ptr, 1); mr.VertexAttribIPointer(i, size, type, stride, ptr);
}
static void mw_EnableVAA(unsigned i) { mg_sync(); MgVao *v = mg_vao(g_mg_cur_vao); if (v && i < 8) { v->a[i].en = 1; v->layout = -1; } mr.EnableVAA(i); }
static void mw_DisableVAA(unsigned i) { mg_sync(); MgVao *v = mg_vao(g_mg_cur_vao); if (v && i < 8) { v->a[i].en = 0; v->layout = -1; } mr.DisableVAA(i); }
static int mw_GetUniformLocation(unsigned p, const char *name) {
    int r = mr.GetUniformLocation(p, name);
    if (name && !strcmp(name, "ChunkOffset")) { MgProg *pr = mg_prog(p); if (pr) pr->loc_off = r; }
    return r;
}
static void mw_AttachShader(unsigned p, unsigned s) { mr.AttachShader(p, s); if (s < g_mg_shader_n && g_mg_shader_chunk[s]) { MgProg *pr = mg_prog(p); if (pr) pr->chunk = 1; } }
static void mw_LinkProgram(unsigned p) {
    mg_sync(); mg_load(); mr.LinkProgram(p); MgProg *pr = mg_prog(p);
    if (pr && pr->chunk && g_mg_on) {   // our injected uniforms: sampler of the page table on unit 7, ArenaMode_ off
        pr->loc_mode = mr.GetUniformLocation(p, "ArenaMode_"); pr->loc_pages = mr.GetUniformLocation(p, "ChunkPages_"); pr->loc_base = mr.GetUniformLocation(p, "ArenaBase_");
        if (pr->loc_mode >= 0 && pr->loc_pages >= 0) { mr.UseProgram(p); mr.Uniform1i(pr->loc_pages, 7); mr.Uniform1i(pr->loc_mode, 0); mr.UseProgram(g_mg_cur_prog); }
        else TR("mega: program %u has a rewritten chunk shader but the injected uniforms were not found (mode %d pages %d base %d): not merged", p, pr->loc_mode, pr->loc_pages, pr->loc_base);
    }
}
static void mw_Uniform3fv(int loc, int count, const float *v) {
    MgProg *pr = g_mg_cur_prog ? mg_prog(g_mg_cur_prog) : 0;
    if (pr && pr->chunk && loc == pr->loc_off && count == 1 && MG_ACTIVE()) { g_mg_off[0] = v[0]; g_mg_off[1] = v[1]; g_mg_off[2] = v[2]; g_mg_off_valid = 1; return; }   // swallowed; forwarded only for individual draws
    if (pr && pr->chunk && loc == pr->loc_off) g_mg_off_valid = 0;   // Minecraft's own upload reaches GL: the remembered (swallowed) value is stale
    mg_sync(); mr.Uniform3fv(loc, count, v);
}
static void mw_ActiveTexture(int t) { mg_sync(); mr.ActiveTexture(t); g_mg_active_tex = t; }
static void mw_Enable(int c) { if (c == 0x0BE2) g_mg_blend = 1; mg_sync(); mr.Enable(c); }
static void mw_Disable(int c) { if (c == 0x0BE2) g_mg_blend = 0; mg_sync(); mr.Disable(c); }
static void mw_BindFramebuffer(int t, unsigned f) { mg_sync(); mr.BindFramebuffer(t, f); }
static void mw_BindTexture(int t, unsigned id) { mg_sync(); mr.BindTexture(t, id); }
static void mw_Clear(int m) { mg_sync(); mr.Clear(m); }
static void mw_DrawArrays(int m, int f, int c) { mg_sync(); mr.DrawArrays(m, f, c); }
static void mw_Viewport(int x, int y, int w, int h) { mg_sync(); mr.Viewport(x, y, w, h); }
static void mw_DepthMask(unsigned char m) { mg_sync(); mr.DepthMask(m); }
static void mw_DrawElements(int mode, int count, int type, const void *idx) {
    MgProg *pr = g_mg_cur_prog ? mg_prog(g_mg_cur_prog) : 0;
    if (pr && pr->chunk && g_mg_cur_vao) {
        int why = 0; MgVao *v = mg_vao(g_mg_cur_vao); MgBuf *b = 0, *e = 0;
        if (!MG_ACTIVE()) why = 5;
        else if (mode != 4 || idx != 0 || count <= 0 || count % 6 || (type != 0x1403 && type != 0x1405)) why = 4;
        else if (g_mg_blend) why = 3;
        else if (!v) why = 1;
        else {
            if (v->layout < 0) v->layout = (signed char)mg_check_layout(v);
            if (v->layout != 1) why = 1;
            else { b = mg_buf(v->a[0].buf); e = mg_buf(v->ebo); if (!b || !e || b->size % MG_STRIDE || (long)(count / 6 * 4) * MG_STRIDE > b->size) why = 4; }
        }
        if (!why) {
            if (e->ebo_state == 0 || e->ebo_type != type) { e->ebo_state = (unsigned char)mg_verify_ebo(v->ebo, type, e->size); e->ebo_type = type; }
            if (e->ebo_state != 1 || (long)count * (type == 0x1403 ? 2 : 4) > e->size) why = 2;
        }
        if (!why && g_mg_off_valid) {
            if (g_mg_npend >= MG_PEND_CAP) mg_flush();
            MgPend *p = &g_mg_pend[g_mg_npend++]; p->vao = g_mg_cur_vao; p->vbo = v->a[0].buf; p->count = count; p->type = type; memcpy(p->off, g_mg_off, sizeof p->off); return;
        }
        if (why == 1) g_mg_st_badvao++; else if (why == 2) g_mg_st_badebo++; else if (why == 3) g_mg_st_blend++; else if (why != 5) g_mg_st_other++;
        if (why == 1 && v && !g_mg_vao_reason_logged) { g_mg_vao_reason_logged = 1; TR("mega: a chunk-program draw uses a VAO whose layout is not the expected one: a0 buf %u size %d type 0x%x stride %d, a3 int=%d size %d type 0x%x, a4 size %d type 0x%x norm %d, enabled %d%d%d%d%d%d", v->a[0].buf, v->a[0].size, v->a[0].type, v->a[0].stride, v->a[3].integer, v->a[3].size, v->a[3].type, v->a[4].size, v->a[4].type, v->a[4].norm, v->a[0].en, v->a[1].en, v->a[2].en, v->a[3].en, v->a[4].en, v->a[5].en); }
        // individual draw: restore the real state Minecraft believes it has (VAO and the ChunkOffset uniform), then the normal call
        mg_flush(); if (g_mg_lazy_vao) { mr.BindVertexArray(g_mg_cur_vao); g_mg_lazy_vao = 0; }
        if (g_mg_off_valid && pr->loc_off >= 0) mr.Uniform3fv(pr->loc_off, 1, g_mg_off);
        mr.DrawElements(mode, count, type, idx); g_mg_st_single++; return;
    }
    mg_sync(); mr.DrawElements(mode, count, type, idx);
}

// ---- shader rewrite: chunk vertex shaders read their offset from the page table when ArenaMode_ != 0 ----
static const char mg_inject[] =
    "uniform vec3 ChunkOffset;\nuniform int ArenaMode_;\nuniform samplerBuffer ChunkPages_;\nuniform vec3 ArenaBase_;\n"
    "vec3 ChunkOffsetEff_() { return ArenaMode_ != 0 ? ArenaBase_ + texelFetch(ChunkPages_, gl_VertexID >> 7).xyz : ChunkOffset; }\n"
    "#define ChunkOffset ChunkOffsetEff_()\n";
// returns the new length of the rewritten source in out (which must have 600 spare bytes) or -1 when the shader has no "uniform vec3 ChunkOffset;"
static int mg_rewrite_chunk(const char *in, char *out) {
    const char *p = strstr(in, "uniform vec3 ChunkOffset;"); if (!p) return -1;
    size_t head = (size_t)(p - in); memcpy(out, in, head); memcpy(out + head, mg_inject, sizeof mg_inject - 1);
    strcpy(out + head + sizeof mg_inject - 1, p + strlen("uniform vec3 ChunkOffset;")); g_mg_st_rewritten++; return 0;
}

// ---- lookup table used by gl_proc(): wrapper for a GL function name, remembering the real pointer ----
static const MgFn g_mg_fns[] = {
    { "glUseProgram", (void *)mw_UseProgram, (void **)&mr.UseProgram }, { "glBindVertexArray", (void *)mw_BindVertexArray, (void **)&mr.BindVertexArray },
    { "glBindBuffer", (void *)mw_BindBuffer, (void **)&mr.BindBuffer }, { "glDrawElements", (void *)mw_DrawElements, (void **)&mr.DrawElements },
    { "glUniform3fv", (void *)mw_Uniform3fv, (void **)&mr.Uniform3fv }, { "glBufferData", (void *)mw_BufferData, (void **)&mr.BufferData },
    { "glBufferSubData", (void *)mw_BufferSubData, (void **)&mr.BufferSubData }, { "glDeleteBuffers", (void *)mw_DeleteBuffers, (void **)&mr.DeleteBuffers },
    { "glDeleteVertexArrays", (void *)mw_DeleteVertexArrays, (void **)&mr.DeleteVertexArrays }, { "glVertexAttribPointer", (void *)mw_VertexAttribPointer, (void **)&mr.VertexAttribPointer },
    { "glVertexAttribIPointer", (void *)mw_VertexAttribIPointer, (void **)&mr.VertexAttribIPointer }, { "glEnableVertexAttribArray", (void *)mw_EnableVAA, (void **)&mr.EnableVAA },
    { "glDisableVertexAttribArray", (void *)mw_DisableVAA, (void **)&mr.DisableVAA }, { "glGetUniformLocation", (void *)mw_GetUniformLocation, (void **)&mr.GetUniformLocation },
    { "glAttachShader", (void *)mw_AttachShader, (void **)&mr.AttachShader }, { "glLinkProgram", (void *)mw_LinkProgram, (void **)&mr.LinkProgram },
    { "glActiveTexture", (void *)mw_ActiveTexture, (void **)&mr.ActiveTexture }, { "glEnable", (void *)mw_Enable, (void **)&mr.Enable }, { "glDisable", (void *)mw_Disable, (void **)&mr.Disable },
    { "glBindFramebuffer", (void *)mw_BindFramebuffer, (void **)&mr.BindFramebuffer }, { "glBindTexture", (void *)mw_BindTexture, (void **)&mr.BindTexture },
    { "glClear", (void *)mw_Clear, (void **)&mr.Clear }, { "glDrawArrays", (void *)mw_DrawArrays, (void **)&mr.DrawArrays }, { "glViewport", (void *)mw_Viewport, (void **)&mr.Viewport },
    { "glDepthMask", (void *)mw_DepthMask, (void **)&mr.DepthMask }, { 0, 0, 0 } };
static void *mg_lookup(const char *n, void *real) {
    for (const MgFn *f = g_mg_fns; f->name; f++) if (!strcmp(n, f->name)) { *f->real = real; return f->wrap; }
    return 0;
}
static void mg_toggle(void) {   // controller (touchpad in gameplay): cycle OFF -> merge level 1 -> 2 -> 3 -> OFF, outside rendering so nothing is pending
    mg_sync(); g_mg_off_valid = 0;
    if (g_mg_user_off) { g_mg_user_off = 0; g_mg_level = 1; }
    else if (g_mg_level < 3) g_mg_level++;
    else g_mg_user_off = 1;
    TR("mega: merging mode now %s", g_mg_user_off ? "OFF" : g_mg_level == 1 ? "ON (level 1: normal)" : g_mg_level == 2 ? "ON (level 2: index list rebuilt every frame, never reused)" : "ON (level 3: level 2 + full page-table upload + glFinish after every merged draw)");
}
static void mg_report(int frames) {   // several short lines: the trace channel cuts lines at ~220 characters
    if (!g_mg_st_flush && !g_mg_st_single && !g_mg_st_other && !g_mg_st_badvao && !g_mg_st_badebo && !g_mg_st_blend) { g_mg_st_ms = 0; return; }
    int used = 0; for (int i = 0; i < MG_PAGES; i++) used += !g_mg_pagefree[i]; double f = frames ? 1.0 / frames : 0;
    TR("mega: /frame %.1f flushes, %.1f chunks merged in %.1f runs (%.0fK indices drawn, %.0fK written), %.1f single, %.1f copies", g_mg_st_flush * f, g_mg_st_chunks * f, g_mg_st_runs * f, g_mg_st_runidx / 1e3 * f, g_mg_st_idxwrite / 1e3 * f, g_mg_st_single * f, g_mg_st_copy * f);
    TR("mega: ms/frame total %.2f = copy %.2f gen %.2f index-upload %.2f offset-upload %.2f fence-wait %.2f draw %.2f; %.1f offset entries in %.1f uploaded per frame", g_mg_st_ms * f, g_mg_t_copy * f, g_mg_t_gen * f, g_mg_t_up * f, g_mg_t_off * f, g_mg_t_wait * f, g_mg_t_draw * f, g_mg_st_texel * f, g_mg_st_rows * f);
    TR("mega: arena %d/%d pages; not merged: layout %lu ebo %lu blend %lu other %lu noalloc %lu partial %lu (arena scan %luK)%s", used, MG_PAGES, g_mg_st_badvao, g_mg_st_badebo, g_mg_st_blend, g_mg_st_other, g_mg_st_noalloc, g_mg_st_partial, g_mg_st_scan / 1000, g_mg_user_off ? "  [OFF by controller]" : g_mg_on ? (g_mg_level == 1 ? "" : "  [debug level raised]") : "  [MERGING DISABLED]");
    g_mg_st_flush = g_mg_st_chunks = g_mg_st_idx = g_mg_st_copy = g_mg_st_single = g_mg_st_noalloc = g_mg_st_badvao = g_mg_st_badebo = g_mg_st_blend = g_mg_st_other = g_mg_st_reuse = g_mg_st_wait = g_mg_st_scan = g_mg_st_texel = g_mg_st_rows = g_mg_st_runs = g_mg_st_runidx = g_mg_st_idxwrite = g_mg_st_partial = 0; g_mg_t_off = 0; g_mg_st_ms = g_mg_t_copy = g_mg_t_gen = g_mg_t_up = g_mg_t_wait = g_mg_t_draw = 0;
}
