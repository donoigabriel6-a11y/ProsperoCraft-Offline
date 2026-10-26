// ps5al.inc - a small software OpenAL 1.1 for LWJGL on the PS5 (included by ps5lwjgl.c).
// LWJGL's OpenAL binding has no natives of its own: it calls function pointers it gets from dlsym()/alcGetProcAddress()/alGetProcAddress(), which the
// dlsym() replacement answers with pal_lookup() below. Sources are mixed (linear resampling, pitch, gain, distance attenuation, stereo pan) into one
// 48 kHz S16 stereo stream written to sceAudioOut (user 0xFF, MAIN port; found by the sndprobe build) by a mixer thread, one 1024-frame grain at a time.
// Only mono/stereo 8/16-bit buffers, no extensions (LWJGL then asks for no extension functions), no capture.
// Log lines are tagged "sndal:" (Select-String 'sndal|sndprobe').
#include <strings.h>
#ifndef LPB
#define LPB(site) (void)0
#define LPA(site) (void)0
#endif
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned len, unsigned freq, unsigned param);
int sceAudioOutOutput(int handle, const void *ptr);

#define AL_MAXSRC 256
#define AL_MAXBUF 2048
#define AL_MAXQ 32
#define AL_RATE 48000
#ifndef AL_GRAIN
#define AL_GRAIN 512   // frames per sceAudioOut grain (valid: 256/512/768/1024...). The console queues ~3 grains (measured), so output latency ~ 3 * grain: 1024 = 64 ms, 512 = 32 ms, 256 = 16 ms
#endif
enum { A_INITIAL = 0x1011, A_PLAYING = 0x1012, A_PAUSED = 0x1013, A_STOPPED = 0x1014, A_STATIC = 0x1028, A_STREAMING = 0x1029, A_UNDET = 0x1030 };

typedef struct { int16_t *pcm; int frames, ch, freq, used; } AlBuf;
typedef struct {
    int used, state, relative, looping, type, q[AL_MAXQ], qn, qcur;
    int dmodel, lat_pending;
    double t_play;
    float gain, pitch, refd, maxd, roll, ming, maxg, pos[3], vel[3], dir[3];
    double posf;
} AlSrc;
static AlBuf g_albuf[AL_MAXBUF + 1];
static AlSrc g_alsrc[AL_MAXSRC + 1];
static pthread_mutex_t g_al_mx = PTHREAD_MUTEX_INITIALIZER;
static float g_lgain = 1, g_lpos[3], g_lvel[3], g_lori[6] = { 0, 0, -1, 0, 1, 0 }, g_doppler = 1, g_dopplerv = 1, g_speed = 343.3f;
static int g_srcdist, g_dmodel = 0xD002, g_al_err, g_alc_err, g_ctx_made, g_dev_open, g_mixer_started;
static int g_dev_tag, g_ctx_tag;   // addresses are the ALCdevice / ALCcontext handles
static volatile int g_al_grains, g_al_clip;
static double g_lat_sum, g_lat_max, g_out_sum, g_out_max; static int g_lat_n, g_out_n;   // latency probe: alSourcePlay -> first mix, and sceAudioOutOutput blocking time

#define PAL_BUILD "ps5al b5 (grain 512, latency probe)"
static void al_seterr_(int e, const char *fn) { if (!g_al_err) g_al_err = e; static int n; if (n++ < 40) TR("sndal: AL error 0x%x raised in %s", e, fn); }
#define al_seterr(e) al_seterr_((e), __func__)
static int g_alc_trace;   // number of ALC calls traced so far (first 120 are logged)
#define ALC_TR(...) do { if (g_alc_trace++ < 40) TR(__VA_ARGS__); } while (0)
static void al_src_defaults(AlSrc *s) {
    memset(s, 0, sizeof *s); s->used = 1; s->state = A_INITIAL; s->type = A_UNDET; s->gain = 1; s->pitch = 1; s->refd = 1; s->maxd = 3.4028235e38f;
    s->roll = 1; s->ming = 0; s->maxg = 1; s->dir[2] = 0; s->dmodel = 0xD002;   // AL_INVERSE_DISTANCE_CLAMPED
}

// ---- mixing ----
static void al_gains(const AlSrc *s, float *gm_l, float *gm_r, float *gs) {
    float rel[3] = { s->pos[0], s->pos[1], s->pos[2] };
    if (!s->relative) for (int i = 0; i < 3; i++) rel[i] -= g_lpos[i];
    float d = (float)sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    float at[3] = { g_lori[0], g_lori[1], g_lori[2] }, up[3] = { g_lori[3], g_lori[4], g_lori[5] };
    float right[3] = { at[1] * up[2] - at[2] * up[1], at[2] * up[0] - at[0] * up[2], at[0] * up[1] - at[1] * up[0] };
    float rl = (float)sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    float x = (rl > 1e-6f) ? (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / rl : rel[0];
    float pan = d > 1e-6f ? x / d : 0; if (pan > 1) pan = 1; if (pan < -1) pan = -1;
    float att = 1, ref = s->refd, mx = s->maxd, dd = d;
    int model = g_srcdist ? s->dmodel : g_dmodel;   // AL_EXT_source_distance_model: per-source model once alEnable(AL_SOURCE_DISTANCE_MODEL) was called
    int clamped = (model == 0xD002 || model == 0xD004 || model == 0xD006);
    if (clamped) { if (dd < ref) dd = ref; if (dd > mx) dd = mx; }
    switch (model) {
        case 0xD001: case 0xD002: att = (ref + s->roll * (dd - ref)) > 0 ? ref / (ref + s->roll * (dd - ref)) : 1; break;
        case 0xD003: case 0xD004: att = (mx > ref) ? 1 - s->roll * (dd - ref) / (mx - ref) : 1; break;
        case 0xD005: case 0xD006: att = (dd > 0 && ref > 0) ? (float)pow(dd / ref, -s->roll) : 1; break;
        default: att = 1;
    }
    if (att < 0) att = 0; if (att > 1e3f) att = 1e3f;
    float g = s->gain * att; if (g < s->ming) g = s->ming; if (g > s->maxg) g = s->maxg; g *= g_lgain;
    double ang = (pan + 1) * 0.78539816339;   // equal-power pan, centre = 0.707 per side
    *gm_l = g * (float)cos(ang); *gm_r = g * (float)sin(ang); *gs = g;
}
static int g_al_playing, g_al_nsrc, g_al_nbuf;
static void al_mix_grain(int16_t *out) {
    static float mix[AL_GRAIN * 2];
    memset(mix, 0, sizeof mix);
    pthread_mutex_lock(&g_al_mx);
    int playing = 0, nsrc = 0;
    for (int n = 1; n <= AL_MAXSRC; n++) {
        AlSrc *S = &g_alsrc[n]; if (!S->used) continue; nsrc++;
        if (S->state != A_PLAYING) continue;
        playing++;
        if (S->lat_pending) {
            S->lat_pending = 0; double ms = (now_s() - S->t_play) * 1000.0; g_lat_sum += ms; g_lat_n++; if (ms > g_lat_max) g_lat_max = ms;
            if (g_lat_n <= 8) TR("sndal: latency play->mix %.1f ms", ms);
        }
        float ml, mr, sg; al_gains(S, &ml, &mr, &sg);
        for (int i = 0; i < AL_GRAIN; i++) {
            if (S->qcur >= S->qn) { S->state = A_STOPPED; break; }
            AlBuf *B = &g_albuf[S->q[S->qcur]];
            if (!B->used || !B->pcm || B->frames <= 0) { S->qcur++; S->posf = 0; i--; continue; }
            int idx = (int)S->posf; if (idx >= B->frames) idx = B->frames - 1;
            float fr = (float)(S->posf - idx); int idx2 = idx + 1 < B->frames ? idx + 1 : idx; float l, r;
            if (B->ch == 1) { float a = B->pcm[idx], b = B->pcm[idx2], v = (a + (b - a) * fr) * (1.0f / 32768.0f); l = v * ml; r = v * mr; }
            else {
                float a0 = B->pcm[2 * idx], b0 = B->pcm[2 * idx2], a1 = B->pcm[2 * idx + 1], b1 = B->pcm[2 * idx2 + 1];
                l = (a0 + (b0 - a0) * fr) * (1.0f / 32768.0f) * sg; r = (a1 + (b1 - a1) * fr) * (1.0f / 32768.0f) * sg;
            }
            mix[2 * i] += l; mix[2 * i + 1] += r;
            S->posf += (double)S->pitch * B->freq / AL_RATE;
            if (S->posf >= B->frames) {
                S->posf = 0; S->qcur++;
                if (S->qcur >= S->qn) { if (S->looping && S->qn > 0) S->qcur = 0; else { S->state = A_STOPPED; break; } }
            }
        }
    }
    g_al_playing = playing; g_al_nsrc = nsrc;
    pthread_mutex_unlock(&g_al_mx);
    for (int i = 0; i < AL_GRAIN * 2; i++) {
        float v = mix[i] * 32767.0f;
        if (v > 32767.0f) { v = 32767.0f; g_al_clip++; } else if (v < -32768.0f) { v = -32768.0f; g_al_clip++; }
        out[i] = (int16_t)v;
    }
}
static void *al_mixer_thread(void *arg) {
    (void)arg;
    static int16_t buf[AL_GRAIN * 2] __attribute__((aligned(256)));
    TR("sndal: mixer thread: sceAudioOutInit rc=0x%x", sceAudioOutInit());
    int h = sceAudioOutOpen(0xFF, 0, 0, AL_GRAIN, AL_RATE, 1);   // user 0xFF (system), MAIN port, S16 stereo: the combination the sndprobe build opened
    TR("sndal: mixer thread: sceAudioOutOpen(grain %d) = 0x%x%s", AL_GRAIN, h, h > 0 ? "" : " (FAILED: running a silent clock instead)");
    int errs = 0;
    for (;;) {
        al_mix_grain(buf);
        if (h > 0) {
            double t0 = now_s(); LPB(2); int r = sceAudioOutOutput(h, buf); LPA(2); double ms = (now_s() - t0) * 1000.0;
            if (r < 0 && errs++ < 5) TR("sndal: sceAudioOutOutput rc=0x%x", r);
            g_out_sum += ms; g_out_n++; if (ms > g_out_max) g_out_max = ms;
            static double first[14]; static int nf; if (nf < 14) { first[nf++] = ms; if (nf == 14) TR("sndal: first 14 sceAudioOutOutput blocking times (ms): %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7], first[8], first[9], first[10], first[11], first[12], first[13]); }
        }
        else usleep(AL_GRAIN * 1000000 / AL_RATE);
        int g = ++g_al_grains;
        if (g % (250 * 1024 / AL_GRAIN) == 0 && (g_al_playing > 0 || g == 250 * 1024 / AL_GRAIN)) TR("sndal: grains=%d sources=%d playing=%d buffers=%d clipped=%d | output call avg %.1f max %.1f ms | play->mix avg %.1f max %.1f ms (n=%d)", g, g_al_nsrc, g_al_playing, g_al_nbuf, g_al_clip, g_out_n ? g_out_sum / g_out_n : 0.0, g_out_max, g_lat_n ? g_lat_sum / g_lat_n : 0.0, g_lat_max, g_lat_n);
    }
    return 0;
}
static void al_start_mixer(void) {
    if (g_mixer_started) return; g_mixer_started = 1;
    pthread_t t; if (pthread_create(&t, 0, al_mixer_thread, 0) == 0) pthread_detach(t); else TR("sndal: pthread_create failed");
}

// ---- ALC ----
static void *pal_alcOpenDevice(const char *name) { TR("sndal: %s", PAL_BUILD); TR("sndal: alcOpenDevice(%s)", name ? name : "default"); g_dev_open = 1; return &g_dev_tag; }
static unsigned char pal_alcCloseDevice(void *d) { (void)d; g_dev_open = 0; return 1; }
static void *pal_alcCreateContext(void *d, const int *attrs) { (void)d; (void)attrs; TR("sndal: alcCreateContext"); return &g_ctx_tag; }
static unsigned char pal_alcMakeContextCurrent(void *c) { g_ctx_made = c != 0; if (c) al_start_mixer(); TR("sndal: alcMakeContextCurrent(%s)", c ? "ctx" : "null"); return 1; }
static void pal_alcProcessContext(void *c) { (void)c; }
static void pal_alcSuspendContext(void *c) { (void)c; }
static void pal_alcDestroyContext(void *c) { (void)c; g_ctx_made = 0; }
static void *pal_alcGetCurrentContext(void) { return g_ctx_made ? &g_ctx_tag : 0; }
static void *pal_alcGetContextsDevice(void *c) { (void)c; return &g_dev_tag; }
static const char *g_alc_ext = "ALC_ENUMERATE_ALL_EXT ALC_SOFT_system_events";
static unsigned char pal_alcIsExtensionPresent(void *d, const char *n) { (void)d; unsigned char r = n && (!strcasecmp(n, "ALC_ENUMERATE_ALL_EXT") || !strcasecmp(n, "ALC_SOFT_system_events")); ALC_TR("sndal: alcIsExtensionPresent(%s) -> %d", n ? n : "(null)", r); return r; }
static int pal_alcGetEnumValue(void *d, const char *n);
static void *pal_alcGetProcAddress(void *d, const char *n);
static int pal_alcGetError(void *d) { (void)d; int e = g_alc_err; g_alc_err = 0; ALC_TR("sndal: alcGetError(%p) -> 0x%x", d, e); return e; }
static const char *pal_alcGetString_(void *d, int p) {
    (void)d;
    switch (p) {
        case 0x1004: case 0x1005: case 0x1012: return "PS5 Audio";
        case 0x1013: { static const char l[] = "PS5 Audio\0"; return l; }   // double-NUL terminated list
        case 0x1006: return g_alc_ext;
        case 0x310: case 0x311: return "";
        case 0: return "";
        default: ALC_TR("sndal: alcGetString: unknown param 0x%x (answered with an empty string, no error)", p); return "";
    }
}
static const char *pal_alcGetString(void *d, int p) { const char *r = pal_alcGetString_(d, p); ALC_TR("sndal: alcGetString(%p, 0x%x) -> %.40s", d, p, r ? r : "(null)"); return r; }
static void pal_alcGetIntegerv_(void *d, int p, int n, int *v) {
    (void)d; if (!v || n < 1) return;
    switch (p) {
        case 0x1000: v[0] = 1; break;                 // ALC_MAJOR_VERSION
        case 0x1001: v[0] = 1; break;                 // ALC_MINOR_VERSION
        case 0x1010: v[0] = 64; break;                // ALC_MONO_SOURCES
        case 0x1011: v[0] = 1; break;                 // ALC_STEREO_SOURCES
        case 0x1007: v[0] = AL_RATE; break;           // ALC_FREQUENCY
        case 0x1008: v[0] = 50; break;                // ALC_REFRESH
        case 0x1009: v[0] = 0; break;                 // ALC_SYNC
        case 0x313: v[0] = 1; break;                  // ALC_CONNECTED
        case 0x1002: v[0] = 5; break;                 // ALC_ATTRIBUTES_SIZE (MC: 4 ints of attributes + terminating 0)
        case 0x1003: { static const int a[5] = { 0x1010, 64, 0x1011, 1, 0 }; for (int i = 0; i < n && i < 5; i++) v[i] = a[i]; break; }   // ALC_ALL_ATTRIBUTES: MONO_SOURCES=64, STEREO_SOURCES=1
        case 0x312: v[0] = 0; break;                  // ALC_CAPTURE_SAMPLES
        default: ALC_TR("sndal: alcGetIntegerv: unknown param 0x%x (answered with 0, no error)", p); v[0] = 0; break;
    }
}
static void pal_alcGetIntegerv(void *d, int p, int n, int *v) { pal_alcGetIntegerv_(d, p, n, v); ALC_TR("sndal: alcGetIntegerv(%p, 0x%x, n=%d) -> %d", d, p, n, (v && n > 0) ? v[0] : -1); }
static void *pal_alcCaptureOpenDevice(const char *n, unsigned f, int fmt, int sz) { (void)n; (void)f; (void)fmt; (void)sz; return 0; }
static unsigned char pal_alcCaptureCloseDevice(void *d) { (void)d; return 0; }
static void pal_alcCaptureStart(void *d) { (void)d; }
static void pal_alcCaptureStop(void *d) { (void)d; }
static void pal_alcCaptureSamples(void *d, void *b, int n) { (void)d; (void)b; (void)n; }

// ---- AL: globals ----
static int pal_alGetError(void) { int e = g_al_err; g_al_err = 0; return e; }
static void pal_alEnable(int c) { if (c == 0x200) g_srcdist = 1; else al_seterr(0xA002); }
static void pal_alDisable(int c) { if (c == 0x200) g_srcdist = 0; else al_seterr(0xA002); }
static unsigned char pal_alIsEnabled(int c) { return c == 0x200 ? (unsigned char)g_srcdist : 0; }
static const char *pal_alGetString(int p) {
    switch (p) {
        case 0xB001: return "PS5 mini OpenAL";
        case 0xB002: return "1.1 PS5MINI";
        case 0xB003: return "PS5 sceAudioOut mixer";
        case 0xB004: return "AL_EXT_source_distance_model AL_EXT_LINEAR_DISTANCE";   // MC 1.20.1 refuses to start its sound system without both
        case 0xA001: return "Invalid Name"; case 0xA002: return "Invalid Enum"; case 0xA003: return "Invalid Value";
        case 0xA004: return "Invalid Operation"; case 0xA005: return "Out of Memory"; case 0: return "No Error";
        default: al_seterr(0xA002); return 0;
    }
}
static int pal_alGetInteger(int p) { switch (p) { case 0xD000: return g_dmodel; default: return 0; } }
static float pal_alGetFloat(int p) { switch (p) { case 0xC000: return g_doppler; case 0xC001: return g_dopplerv; case 0xC003: return g_speed; default: return 0; } }
static double pal_alGetDouble(int p) { return pal_alGetFloat(p); }
static unsigned char pal_alGetBoolean(int p) { (void)p; return 0; }
static void pal_alGetBooleanv(int p, unsigned char *v) { if (v) v[0] = pal_alGetBoolean(p); }
static void pal_alGetIntegerv(int p, int *v) { if (v) v[0] = pal_alGetInteger(p); }
static void pal_alGetFloatv(int p, float *v) { if (v) v[0] = pal_alGetFloat(p); }
static void pal_alGetDoublev(int p, double *v) { if (v) v[0] = pal_alGetDouble(p); }
static void pal_alDistanceModel(int m) { if (m == 0 || (m >= 0xD001 && m <= 0xD006)) g_dmodel = m; else al_seterr(0xA003); }
static void pal_alDopplerFactor(float v) { if (v < 0) al_seterr(0xA003); else g_doppler = v; }
static void pal_alDopplerVelocity(float v) { if (v <= 0) al_seterr(0xA003); else g_dopplerv = v; }
static void pal_alSpeedOfSound(float v) { if (v <= 0) al_seterr(0xA003); else g_speed = v; }

// ---- AL: listener ----
static void pal_alListenerfv(int p, const float *v) {
    pthread_mutex_lock(&g_al_mx);
    switch (p) {
        case 0x100A: if (v[0] < 0) al_seterr(0xA003); else g_lgain = v[0]; break;
        case 0x1004: memcpy(g_lpos, v, 12); break;
        case 0x1006: memcpy(g_lvel, v, 12); break;
        case 0x100F: memcpy(g_lori, v, 24); break;
        default: al_seterr(0xA002);
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alListenerf(int p, float v) { if (p == 0x100A) pal_alListenerfv(p, &v); else al_seterr(0xA002); }
static void pal_alListener3f(int p, float a, float b, float c) { float v[3] = { a, b, c }; if (p == 0x1004 || p == 0x1006) pal_alListenerfv(p, v); else al_seterr(0xA002); }
static void pal_alListeneri(int p, int v) { (void)p; (void)v; al_seterr(0xA002); }
static void pal_alListener3i(int p, int a, int b, int c) { pal_alListener3f(p, (float)a, (float)b, (float)c); }
static void pal_alListeneriv(int p, const int *v) {
    if (p == 0x1004 || p == 0x1006) pal_alListener3f(p, (float)v[0], (float)v[1], (float)v[2]);
    else if (p == 0x100F) { float f[6]; for (int i = 0; i < 6; i++) f[i] = (float)v[i]; pal_alListenerfv(p, f); } else al_seterr(0xA002);
}
static void pal_alGetListenerfv(int p, float *v) {
    pthread_mutex_lock(&g_al_mx);
    switch (p) {
        case 0x100A: v[0] = g_lgain; break;
        case 0x1004: memcpy(v, g_lpos, 12); break;
        case 0x1006: memcpy(v, g_lvel, 12); break;
        case 0x100F: memcpy(v, g_lori, 24); break;
        default: al_seterr(0xA002);
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alGetListenerf(int p, float *v) { if (v) pal_alGetListenerfv(p, v); }
static void pal_alGetListener3f(int p, float *a, float *b, float *c) { float v[6] = { 0 }; pal_alGetListenerfv(p, v); if (a) *a = v[0]; if (b) *b = v[1]; if (c) *c = v[2]; }
static void pal_alGetListeneri(int p, int *v) { (void)p; if (v) *v = 0; al_seterr(0xA002); }
static void pal_alGetListeneriv(int p, int *v) { float f[6] = { 0 }; pal_alGetListenerfv(p, f); int n = p == 0x100F ? 6 : (p == 0x100A ? 1 : 3); for (int i = 0; i < n; i++) v[i] = (int)f[i]; }

// ---- AL: buffers ----
static void pal_alGenBuffers(int n, unsigned *out) {
    if (n < 0) { al_seterr(0xA003); return; }
    pthread_mutex_lock(&g_al_mx);
    for (int i = 0; i < n; i++) {
        unsigned id = 0; for (unsigned k = 1; k <= AL_MAXBUF; k++) if (!g_albuf[k].used) { id = k; break; }
        if (!id) { al_seterr(0xA005); break; }
        memset(&g_albuf[id], 0, sizeof(AlBuf)); g_albuf[id].used = 1; out[i] = id; g_al_nbuf++;
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alDeleteBuffers(int n, const unsigned *ids) {
    if (n < 0) { al_seterr(0xA003); return; }
    pthread_mutex_lock(&g_al_mx);
    for (int i = 0; i < n; i++) {
        unsigned id = ids[i]; if (!id) continue;
        if (id > AL_MAXBUF || !g_albuf[id].used) { al_seterr(0xA001); continue; }
        for (int s = 1; s <= AL_MAXSRC; s++) if (g_alsrc[s].used) for (int k = 0; k < g_alsrc[s].qn; k++) if ((unsigned)g_alsrc[s].q[k] == id) { al_seterr(0xA004); goto next; }
        free(g_albuf[id].pcm); memset(&g_albuf[id], 0, sizeof(AlBuf)); g_al_nbuf--;
        next:;
    }
    pthread_mutex_unlock(&g_al_mx);
}
static unsigned char pal_alIsBuffer(unsigned id) { return id > 0 && id <= AL_MAXBUF && g_albuf[id].used; }
static void pal_alBufferData(unsigned id, int fmt, const void *data, int size, int freq) {
    if (id == 0 || id > AL_MAXBUF || !g_albuf[id].used) { al_seterr(0xA001); return; }
    int ch, bits;
    switch (fmt) { case 0x1100: ch = 1; bits = 8; break; case 0x1101: ch = 1; bits = 16; break; case 0x1102: ch = 2; bits = 8; break; case 0x1103: ch = 2; bits = 16; break;
                   default: { static int w; if (w++ < 5) TR("sndal: alBufferData unsupported format 0x%x", fmt); al_seterr(0xA002); return; } }
    if (size < 0 || freq <= 0 || (size && !data)) { al_seterr(0xA003); return; }
    int frames = size / (ch * bits / 8);
    int16_t *pcm = (int16_t *)malloc((size_t)(frames > 0 ? frames : 1) * ch * sizeof(int16_t));
    if (!pcm) { al_seterr(0xA005); return; }
    if (bits == 16) memcpy(pcm, data, (size_t)frames * ch * 2);
    else for (int i = 0; i < frames * ch; i++) pcm[i] = (int16_t)((((const uint8_t *)data)[i] - 128) << 8);
    pthread_mutex_lock(&g_al_mx);
    if (id > 0) { free(g_albuf[id].pcm); g_albuf[id].pcm = pcm; g_albuf[id].frames = frames; g_albuf[id].ch = ch; g_albuf[id].freq = freq; }
    pthread_mutex_unlock(&g_al_mx);
    static int logged; if (logged++ < 3) TR("sndal: alBufferData id=%u fmt=0x%x %d frames %d Hz", id, fmt, frames, freq);
}
static void pal_alBufferfv(unsigned id, int p, const float *v) { (void)id; (void)p; (void)v; al_seterr(0xA002); }
static void pal_alBufferf(unsigned id, int p, float v) { (void)id; (void)p; (void)v; al_seterr(0xA002); }
static void pal_alBuffer3f(unsigned id, int p, float a, float b, float c) { (void)id; (void)p; (void)a; (void)b; (void)c; al_seterr(0xA002); }
static void pal_alBufferi(unsigned id, int p, int v) { (void)id; (void)p; (void)v; al_seterr(0xA002); }
static void pal_alBuffer3i(unsigned id, int p, int a, int b, int c) { (void)id; (void)p; (void)a; (void)b; (void)c; al_seterr(0xA002); }
static void pal_alBufferiv(unsigned id, int p, const int *v) { (void)id; (void)p; (void)v; al_seterr(0xA002); }
static void pal_alGetBufferi(unsigned id, int p, int *v) {
    if (id == 0 || id > AL_MAXBUF || !g_albuf[id].used) { al_seterr(0xA001); return; }
    AlBuf *b = &g_albuf[id];
    switch (p) { case 0x2001: *v = b->freq; break; case 0x2002: *v = 16; break; case 0x2003: *v = b->ch; break; case 0x2004: *v = b->frames * b->ch * 2; break; default: al_seterr(0xA002); }
}
static void pal_alGetBufferiv(unsigned id, int p, int *v) { pal_alGetBufferi(id, p, v); }
static void pal_alGetBufferf(unsigned id, int p, float *v) { int i = 0; pal_alGetBufferi(id, p, &i); *v = (float)i; }
static void pal_alGetBufferfv(unsigned id, int p, float *v) { pal_alGetBufferf(id, p, v); }

// ---- AL: sources ----
static void pal_alGenSources(int n, unsigned *out) {
    if (n < 0) { al_seterr(0xA003); return; }
    pthread_mutex_lock(&g_al_mx);
    for (int i = 0; i < n; i++) {
        unsigned id = 0; for (unsigned k = 1; k <= AL_MAXSRC; k++) if (!g_alsrc[k].used) { id = k; break; }
        if (!id) { al_seterr(0xA005); break; }
        al_src_defaults(&g_alsrc[id]); out[i] = id;
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alDeleteSources(int n, const unsigned *ids) {
    if (n < 0) { al_seterr(0xA003); return; }
    pthread_mutex_lock(&g_al_mx);
    for (int i = 0; i < n; i++) { unsigned id = ids[i]; if (!id) continue; if (id > AL_MAXSRC || !g_alsrc[id].used) { al_seterr(0xA001); continue; } g_alsrc[id].used = 0; g_alsrc[id].state = A_STOPPED; }
    pthread_mutex_unlock(&g_al_mx);
}
static unsigned char pal_alIsSource(unsigned id) { return id > 0 && id <= AL_MAXSRC && g_alsrc[id].used; }
#define SRC_OR_ERR(id, S) if ((id) == 0 || (id) > AL_MAXSRC || !g_alsrc[id].used) { al_seterr(0xA001); return; } AlSrc *S = &g_alsrc[id];
static void al_src_stop(AlSrc *S) { if (S->state == A_PLAYING || S->state == A_PAUSED) { S->state = A_STOPPED; S->qcur = S->qn; S->posf = 0; } }
static void pal_alSourcef(unsigned id, int p, float v) {
    SRC_OR_ERR(id, S);
    pthread_mutex_lock(&g_al_mx);
    switch (p) {
        case 0x1003: if (v <= 0) al_seterr(0xA003); else S->pitch = v; break;
        case 0x100A: if (v < 0) al_seterr(0xA003); else S->gain = v; break;
        case 0x100D: S->ming = v; break;
        case 0x100E: S->maxg = v; break;
        case 0x1020: if (v < 0) al_seterr(0xA003); else S->refd = v; break;
        case 0x1021: if (v < 0) al_seterr(0xA003); else S->roll = v; break;
        case 0x1023: if (v < 0) al_seterr(0xA003); else S->maxd = v; break;
        case 0x1001: case 0x1002: case 0x1022: break;   // cone: accepted, ignored
        case 0x1024: { AlBuf *B = S->qcur < S->qn ? &g_albuf[S->q[S->qcur]] : 0; if (B && B->freq > 0) S->posf = v * B->freq; break; }   // AL_SEC_OFFSET
        case 0x1025: S->posf = v; break;                                                                                                 // AL_SAMPLE_OFFSET
        default: al_seterr(0xA002);
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alSourcefv(unsigned id, int p, const float *v) {
    SRC_OR_ERR(id, S);
    if (p == 0x1004 || p == 0x1005 || p == 0x1006) {
        pthread_mutex_lock(&g_al_mx); memcpy(p == 0x1004 ? S->pos : p == 0x1005 ? S->dir : S->vel, v, 12); pthread_mutex_unlock(&g_al_mx);
    } else pal_alSourcef(id, p, v[0]);
}
static void pal_alSource3f(unsigned id, int p, float a, float b, float c) { float v[3] = { a, b, c }; if (p == 0x1004 || p == 0x1005 || p == 0x1006) pal_alSourcefv(id, p, v); else { SRC_OR_ERR(id, S); (void)S; al_seterr(0xA002); } }
static void pal_alSourcei(unsigned id, int p, int v) {
    SRC_OR_ERR(id, S);
    pthread_mutex_lock(&g_al_mx);
    switch (p) {
        case 0x202: S->relative = v != 0; break;
        case 0x1007: S->looping = v != 0; break;
        case 0x1009:   // AL_BUFFER: replace the queue with one buffer (0 clears it)
            if (v != 0 && (v < 0 || v > AL_MAXBUF || !g_albuf[v].used)) { al_seterr(0xA001); break; }
            if (S->state == A_PLAYING || S->state == A_PAUSED) { al_seterr(0xA004); break; }
            S->qn = v ? 1 : 0; S->q[0] = v; S->qcur = 0; S->posf = 0; S->state = A_INITIAL; S->type = v ? A_STATIC : A_UNDET; break;
        case 0x1020: case 0x1021: case 0x1023: pthread_mutex_unlock(&g_al_mx); pal_alSourcef(id, p, (float)v); return;
        case 0x1025: S->posf = v; break;
        case 0xD000: if (v == 0 || (v >= 0xD001 && v <= 0xD006)) S->dmodel = v; else al_seterr(0xA003); break;
        case 0x1001: case 0x1002: break;
        default: al_seterr(0xA002);
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alSource3i(unsigned id, int p, int a, int b, int c) { pal_alSource3f(id, p, (float)a, (float)b, (float)c); }
static void pal_alSourceiv(unsigned id, int p, const int *v) {
    if (p == 0x1004 || p == 0x1005 || p == 0x1006) pal_alSource3f(id, p, (float)v[0], (float)v[1], (float)v[2]); else pal_alSourcei(id, p, v[0]);
}
static void pal_alGetSourcei(unsigned id, int p, int *v) {
    SRC_OR_ERR(id, S);
    pthread_mutex_lock(&g_al_mx);
    switch (p) {
        case 0x1010: *v = S->state; break;
        case 0x1015: *v = S->qn; break;
        case 0x1016: *v = (S->state == A_PLAYING || S->state == A_PAUSED) ? S->qcur : (S->state == A_STOPPED ? S->qn : 0); break;   // AL_BUFFERS_PROCESSED
        case 0x1009: *v = S->qcur < S->qn ? S->q[S->qcur] : 0; break;
        case 0x1007: *v = S->looping; break;
        case 0x202: *v = S->relative; break;
        case 0x1027: *v = S->type; break;
        case 0x1025: *v = (int)S->posf; break;
        case 0xD000: *v = S->dmodel; break;
        default: al_seterr(0xA002);
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alGetSourceiv(unsigned id, int p, int *v) { pal_alGetSourcei(id, p, v); }
static void pal_alGetSourcef(unsigned id, int p, float *v) {
    SRC_OR_ERR(id, S);
    pthread_mutex_lock(&g_al_mx);
    switch (p) {
        case 0x1003: *v = S->pitch; break; case 0x100A: *v = S->gain; break; case 0x100D: *v = S->ming; break; case 0x100E: *v = S->maxg; break;
        case 0x1020: *v = S->refd; break; case 0x1021: *v = S->roll; break; case 0x1023: *v = S->maxd; break;
        case 0x1024: { AlBuf *B = S->qcur < S->qn ? &g_albuf[S->q[S->qcur]] : 0; *v = (B && B->freq > 0) ? (float)(S->posf / B->freq) : 0; break; }
        case 0x1025: *v = (float)S->posf; break;
        default: al_seterr(0xA002);
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alGetSourcefv(unsigned id, int p, float *v) {
    SRC_OR_ERR(id, S);
    if (p == 0x1004 || p == 0x1005 || p == 0x1006) { pthread_mutex_lock(&g_al_mx); memcpy(v, p == 0x1004 ? S->pos : p == 0x1005 ? S->dir : S->vel, 12); pthread_mutex_unlock(&g_al_mx); }
    else pal_alGetSourcef(id, p, v);
}
static void pal_alGetSource3f(unsigned id, int p, float *a, float *b, float *c) { float v[3] = { 0 }; pal_alGetSourcefv(id, p, v); if (a) *a = v[0]; if (b) *b = v[1]; if (c) *c = v[2]; }
static void pal_alSourceQueueBuffers(unsigned id, int n, const unsigned *bufs) {
    SRC_OR_ERR(id, S);
    if (n < 0) { al_seterr(0xA003); return; }
    pthread_mutex_lock(&g_al_mx);
    if (S->qn + n > AL_MAXQ) al_seterr(0xA003);
    else {
        int ok = 1; for (int i = 0; i < n; i++) if (bufs[i] == 0 || bufs[i] > AL_MAXBUF || !g_albuf[bufs[i]].used) ok = 0;
        if (!ok) al_seterr(0xA001);
        else if (S->type == A_STATIC) al_seterr(0xA004);
        else { for (int i = 0; i < n; i++) S->q[S->qn++] = (int)bufs[i]; S->type = A_STREAMING; }
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void pal_alSourceUnqueueBuffers(unsigned id, int n, unsigned *bufs) {
    SRC_OR_ERR(id, S);
    if (n < 0) { al_seterr(0xA003); return; }
    pthread_mutex_lock(&g_al_mx);
    int processed = (S->state == A_PLAYING || S->state == A_PAUSED) ? S->qcur : (S->state == A_STOPPED ? S->qn : 0);
    if (n > processed) al_seterr(0xA003);
    else {
        for (int i = 0; i < n; i++) bufs[i] = (unsigned)S->q[i];
        memmove(S->q, S->q + n, (size_t)(S->qn - n) * sizeof(int)); S->qn -= n; S->qcur -= n; if (S->qcur < 0) S->qcur = 0;
        if (S->qn == 0 && S->type == A_STREAMING) S->type = A_UNDET;
    }
    pthread_mutex_unlock(&g_al_mx);
}
static void al_play(AlSrc *S) {
    if (S->state == A_PAUSED) { S->state = A_PLAYING; return; }
    S->t_play = now_s(); S->lat_pending = 1;
    S->qcur = 0; S->posf = 0; S->state = S->qn > 0 ? A_PLAYING : A_INITIAL;   // INITIAL/STOPPED/PLAYING: (re)start from the first queued buffer
    if (S->qn == 0) S->state = A_STOPPED;
}
static void pal_alSourcePlay(unsigned id) { SRC_OR_ERR(id, S); pthread_mutex_lock(&g_al_mx); al_play(S); pthread_mutex_unlock(&g_al_mx); }
static void pal_alSourcePause(unsigned id) { SRC_OR_ERR(id, S); pthread_mutex_lock(&g_al_mx); if (S->state == A_PLAYING) S->state = A_PAUSED; pthread_mutex_unlock(&g_al_mx); }
static void pal_alSourceStop(unsigned id) { SRC_OR_ERR(id, S); pthread_mutex_lock(&g_al_mx); al_src_stop(S); pthread_mutex_unlock(&g_al_mx); }
static void pal_alSourceRewind(unsigned id) { SRC_OR_ERR(id, S); pthread_mutex_lock(&g_al_mx); S->state = A_INITIAL; S->qcur = 0; S->posf = 0; pthread_mutex_unlock(&g_al_mx); }
static void pal_alSourcePlayv(int n, const unsigned *ids) { for (int i = 0; i < n; i++) pal_alSourcePlay(ids[i]); }
static void pal_alSourcePausev(int n, const unsigned *ids) { for (int i = 0; i < n; i++) pal_alSourcePause(ids[i]); }
static void pal_alSourceStopv(int n, const unsigned *ids) { for (int i = 0; i < n; i++) pal_alSourceStop(ids[i]); }
static void pal_alSourceRewindv(int n, const unsigned *ids) { for (int i = 0; i < n; i++) pal_alSourceRewind(ids[i]); }

// ---- lookup ----
static int pal_alGetEnumValue(const char *n) {
    static const struct { const char *n; int v; } e[] = { {"AL_NONE", 0}, {"AL_FALSE", 0}, {"AL_TRUE", 1}, {"AL_SOURCE_RELATIVE", 0x202}, {"AL_PITCH", 0x1003}, {"AL_POSITION", 0x1004},
        {"AL_GAIN", 0x100A}, {"AL_SOURCE_STATE", 0x1010}, {"AL_PLAYING", 0x1012}, {"AL_STOPPED", 0x1014}, {"AL_BUFFERS_PROCESSED", 0x1016}, {"AL_FORMAT_MONO16", 0x1101},
        {"AL_FORMAT_STEREO16", 0x1103}, {"AL_NO_ERROR", 0}, {0, 0} };
    for (int i = 0; e[i].n; i++) if (n && !strcmp(e[i].n, n)) return e[i].v;
    return 0;
}
static int pal_alcGetEnumValue(void *d, const char *n) { (void)d; return pal_alGetEnumValue(n); }
static unsigned char pal_alIsExtensionPresent(const char *n) { return n && (!strcasecmp(n, "AL_EXT_source_distance_model") || !strcasecmp(n, "AL_EXT_LINEAR_DISTANCE")); }
// ALC_SOFT_system_events: Minecraft 26.1 calls alcEventIsSupportedSOFT without asking for the extension first (NullPointerException "Initializing game" in Checks.check: the function address was 0).
// This device has no device-change events: "not supported" makes the game use its polling fallback.
static int pal_alcEventIsSupportedSOFT(int eventType, int deviceType) { (void)eventType; (void)deviceType; return 0x19D7; /* ALC_EVENT_NOT_SUPPORTED_SOFT */ }
static unsigned char pal_alcEventControlSOFT(int count, const int *events, unsigned char enable) { (void)count; (void)events; (void)enable; return 0; }
static void pal_alcEventCallbackSOFT(void *cb, void *user) { (void)cb; (void)user; }
typedef struct { const char *name; void *fn; } AlSym;
#define ALSYM_(n) { #n, (void *)pal_##n }
static const AlSym g_alsyms[] = {
    ALSYM_(alcOpenDevice), ALSYM_(alcCloseDevice), ALSYM_(alcCreateContext), ALSYM_(alcMakeContextCurrent), ALSYM_(alcProcessContext), ALSYM_(alcSuspendContext), ALSYM_(alcDestroyContext),
    ALSYM_(alcGetCurrentContext), ALSYM_(alcGetContextsDevice), ALSYM_(alcIsExtensionPresent), ALSYM_(alcGetProcAddress), ALSYM_(alcGetEnumValue), ALSYM_(alcGetError), ALSYM_(alcGetString),
    ALSYM_(alcGetIntegerv), ALSYM_(alcCaptureOpenDevice), ALSYM_(alcCaptureCloseDevice), ALSYM_(alcCaptureStart), ALSYM_(alcCaptureStop), ALSYM_(alcCaptureSamples),
    ALSYM_(alGetError), ALSYM_(alEnable), ALSYM_(alDisable), ALSYM_(alIsEnabled), ALSYM_(alGetBoolean), ALSYM_(alGetInteger), ALSYM_(alGetFloat), ALSYM_(alGetDouble), ALSYM_(alGetBooleanv), ALSYM_(alGetIntegerv),
    ALSYM_(alGetFloatv), ALSYM_(alGetDoublev), ALSYM_(alGetString), ALSYM_(alDistanceModel), ALSYM_(alDopplerFactor), ALSYM_(alDopplerVelocity), ALSYM_(alSpeedOfSound),
    ALSYM_(alListenerf), ALSYM_(alListeneri), ALSYM_(alListener3f), ALSYM_(alListenerfv), ALSYM_(alListener3i), ALSYM_(alListeneriv), ALSYM_(alGetListenerf), ALSYM_(alGetListeneri), ALSYM_(alGetListener3f),
    ALSYM_(alGetListenerfv), ALSYM_(alGetListeneriv), ALSYM_(alGenSources), ALSYM_(alDeleteSources), ALSYM_(alIsSource), ALSYM_(alSourcef), ALSYM_(alSource3f), ALSYM_(alSourcefv), ALSYM_(alSourcei),
    ALSYM_(alSource3i), ALSYM_(alSourceiv), ALSYM_(alGetSourcef), ALSYM_(alGetSource3f), ALSYM_(alGetSourcefv), ALSYM_(alGetSourcei), ALSYM_(alGetSourceiv), ALSYM_(alSourceQueueBuffers),
    ALSYM_(alSourceUnqueueBuffers), ALSYM_(alSourcePlay), ALSYM_(alSourcePause), ALSYM_(alSourceStop), ALSYM_(alSourceRewind), ALSYM_(alSourcePlayv), ALSYM_(alSourcePausev), ALSYM_(alSourceStopv),
    ALSYM_(alSourceRewindv), ALSYM_(alGenBuffers), ALSYM_(alDeleteBuffers), ALSYM_(alIsBuffer), ALSYM_(alBufferData), ALSYM_(alBufferf), ALSYM_(alBuffer3f), ALSYM_(alBufferfv), ALSYM_(alBufferi),
    ALSYM_(alBuffer3i), ALSYM_(alBufferiv), ALSYM_(alGetBufferf), ALSYM_(alGetBufferi), ALSYM_(alGetBufferfv), ALSYM_(alGetBufferiv), ALSYM_(alGetEnumValue), ALSYM_(alIsExtensionPresent),
    ALSYM_(alcEventIsSupportedSOFT), ALSYM_(alcEventControlSOFT), ALSYM_(alcEventCallbackSOFT),
    { "alGetProcAddress", 0 }, { 0, 0 } };
static void *pal_alGetProcAddress(const char *n);
static void *pal_lookup(const char *n) {
    if (!n) return 0;
    if (!strcmp(n, "alGetProcAddress")) return (void *)pal_alGetProcAddress;
    for (const AlSym *s = g_alsyms; s->name; s++) if (!strcmp(s->name, n)) return s->fn;
    return 0;
}
static void *pal_alGetProcAddress(const char *n) { return pal_lookup(n); }
static void *pal_alcGetProcAddress(void *d, const char *n) { (void)d; return pal_lookup(n); }
