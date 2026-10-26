// jvm_host.cpp: boots the statically linked HotSpot inside a PS5 native title and runs assets/hello/Hello.
// Log goes to stdout and, if ps5_conf.h defines LOG_HOST, to UDP (listen with the PowerShell listener).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#include "jni.h"
#include "ps5_conf.h"
#ifdef GL_BARE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif
#ifdef LOG_HOST
extern "C" { const char *ps5_log_host_str = LOG_HOST; }   // ps5lwjgl.c (LAN diagnostics) sends the LAN announcement to this PC as well
#endif
#ifdef LOG_HOST
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

extern "C" {
#ifndef JDK_MASK
#define JDK_MASK 1   // bit 0 = Java 17, bit 1 = Java 21, bit 2 = Java 25, bit 3 = Java 8: the JDKs linked into this title (make_jvm_title.sh renames each one's JNI_CreateJavaVM / ps5_set_java_home / ps5_trace_hook with a _<version> suffix)
#endif
#ifdef GL_SELFTEST_ONLY
#undef JDK_MASK
#define JDK_MASK 1   // no JDK in this build: stubs for Java 17 only
void (*ps5_trace_hook_17)(const char *) = 0;
void ps5_trace(const char *m) { if (ps5_trace_hook_17) ps5_trace_hook_17(m); }
void ps5_set_java_home_17(const char *) {}
jint JNI_CreateJavaVM_17(JavaVM **, void **, void *) { return -1; }
void *ps5_dll_handle_17(void) { return 0; }
void *ps5_dlsym_17(const char *) { return 0; }
#else
#define JDK_DECL(V) jint JNI_CreateJavaVM_##V(JavaVM **, void **, void *); void ps5_set_java_home_##V(const char *); extern void (*ps5_trace_hook_##V)(const char *); void *ps5_dll_handle_##V(void); void *ps5_dlsym_##V(const char *);
#if JDK_MASK & 1
JDK_DECL(17)
#endif
#if JDK_MASK & 2
JDK_DECL(21)
#endif
#if JDK_MASK & 4
JDK_DECL(25)
#endif
#if JDK_MASK & 8
JDK_DECL(8)
#endif
#endif
}
extern "C" void ps5_gl_probe(void);
extern "C" void ps5_calib(void) __attribute__((weak));   // ps5lwjgl.c: pthread-object budget probe, run before the VM thread
#ifdef ZPROBE
extern "C" void ps5_zink_probe(void);
#endif
extern "C" void ps5_set_renderer(int gl);
extern "C" void ps5_set_hz(int hz);
static int g_renderer_gl, g_renderer_zink;   // zink: OpenGL games over Mesa Zink on RADV (g_renderer_gl stays 0: the window is a Vulkan display)
static volatile int g_main_done;   // Hello.main returned
static int g_jdkv = 17;   // the Java this run uses, picked in vm_main from launcher.cfg java=<major the chosen version needs>
extern "C" int ps5_running_jdk(void) { return g_jdkv; }
#ifdef AIO_RENDERER
static const char *g_rsel_name = "vulkan";
extern "C" int ps5_running_renderer(void) { return g_renderer_zink ? 2 : g_renderer_gl ? 1 : 0; }   // 0 Vulkan, 1 OpenGL (SDK), 2 OpenGL via Zink
extern "C" int ps5_renderer_have(int r) {   // 1 when this build carries the renderer
#ifdef PS5_ZINK
    return 1;
#else
    return r != 2;
#endif
}
#endif
static int g_jneed = 0;   // the Java the instance's version needs (0 = unknown), set at boot
   // the Java of this run, shown in the launcher settings
static bool jdk_have(int v) { return v == 17 ? (JDK_MASK & 1) : v == 21 ? (JDK_MASK & 2) : v == 25 ? (JDK_MASK & 4) : v == 8 ? (JDK_MASK & 8) : 0; }
static int jdk_pick(int need) {   // the smallest linked Java that is at least `need` (a newer Java runs every older version); none that new: the newest there is (McBoot then says which Java the version needs)
    static const int t[4] = { 8, 17, 21, 25 };
    if (need <= 0) { for (int i = 1; i < 4; i++) if (jdk_have(t[i])) return t[i]; }   // unknown (first start): never Java 8 by default, the version picked next would usually need a newer one
    for (int i = 0; i < 4; i++) if (jdk_have(t[i]) && t[i] >= need) return t[i];
    for (int i = 3; i >= 0; i--) if (jdk_have(t[i])) return t[i];
    return 17;
}
extern "C" int ps5_java_for(int forced, int need)   // the Java a start boots for a version that needs `need` (0 = unknown): launcher.cfg jdk= (0 = automatic) if that Java is linked and not older than needed, else the automatic pick
{
    const int g = jdk_pick(need);
    return forced && jdk_have(forced) && forced >= need ? forced : g;
}
extern "C" int ps5_java_expect(int forced) { return ps5_java_for(forced, g_jneed); }   // ... for the version this run was started for
static jint jdk_create(JavaVM **vm, void **env, void *a) {
#if JDK_MASK & 1
    if (g_jdkv == 17) return JNI_CreateJavaVM_17(vm, env, a);
#endif
#if JDK_MASK & 2
    if (g_jdkv == 21) return JNI_CreateJavaVM_21(vm, env, a);
#endif
#if JDK_MASK & 4
    if (g_jdkv == 25) return JNI_CreateJavaVM_25(vm, env, a);
#endif
#if JDK_MASK & 8
    if (g_jdkv == 8) return JNI_CreateJavaVM_8(vm, env, a);
#endif
    return -1;
}
extern "C" { extern void *(*ps5_dll_handle_fn)(void); extern void *(*ps5_dlsym_fn)(const char *); }   // ps5lwjgl.c: dlsym() of the JDK libraries goes to the active JDK
static void jdk_set_dl() {
#if JDK_MASK & 1
    if (g_jdkv == 17) { ps5_dll_handle_fn = ps5_dll_handle_17; ps5_dlsym_fn = ps5_dlsym_17; }
#endif
#if JDK_MASK & 2
    if (g_jdkv == 21) { ps5_dll_handle_fn = ps5_dll_handle_21; ps5_dlsym_fn = ps5_dlsym_21; }
#endif
#if JDK_MASK & 4
    if (g_jdkv == 25) { ps5_dll_handle_fn = ps5_dll_handle_25; ps5_dlsym_fn = ps5_dlsym_25; }
#endif
#if JDK_MASK & 8
    if (g_jdkv == 8) { ps5_dll_handle_fn = ps5_dll_handle_8; ps5_dlsym_fn = ps5_dlsym_8; }
#endif
}
static void jdk_set_home(const char *p) {
#if JDK_MASK & 1
    if (g_jdkv == 17) ps5_set_java_home_17(p);
#endif
#if JDK_MASK & 2
    if (g_jdkv == 21) ps5_set_java_home_21(p);
#endif
#if JDK_MASK & 4
    if (g_jdkv == 25) ps5_set_java_home_25(p);
#endif
#if JDK_MASK & 8
    if (g_jdkv == 8) ps5_set_java_home_8(p);
#endif
}
static void jdk_set_trace(void (*f)(const char *)) {   // every linked JDK has its own hook variable
#if JDK_MASK & 1
    ps5_trace_hook_17 = f;
#endif
#if JDK_MASK & 2
    ps5_trace_hook_21 = f;
#endif
#if JDK_MASK & 4
    ps5_trace_hook_25 = f;
#endif
#if JDK_MASK & 8
    ps5_trace_hook_8 = f;
#endif
}

#ifndef LOG_PORT
#define LOG_PORT 9999
#endif
#ifndef XMX_MB
#define XMX_MB 1536   // Java heap max; override with XMX=<mb> when running make_jvm_title.sh
#endif
#ifndef JAVA_HOME
#define JAVA_HOME "/app0/assets/jdk17"   // only the early file probe; the real java.home is /app0/assets/jdk<version> of the Java picked at start
#endif
#ifndef CLASS_PATH
#define CLASS_PATH "/app0/assets/hello"
#endif
#ifdef USE_GL
#undef CLASS_PATH
#define CLASS_PATH "/app0/assets/hello:/app0/assets/lib/lwjgl.jar:/app0/assets/lib/lwjgl-glfw.jar:/app0/assets/lib/lwjgl-opengl.jar:/app0/assets/lib/lwjgl-stb.jar"
#ifdef MC_CP   // "mc" build: Minecraft client + libraries (list generated by make_jvm_title.sh)
#undef CLASS_PATH
#define CLASS_PATH "/app0/assets/hello:/app0/assets/lib/lwjgl.jar:/app0/assets/lib/lwjgl-glfw.jar:/app0/assets/lib/lwjgl-opengl.jar:/app0/assets/lib/lwjgl-stb.jar:" MC_CP
#endif
// the OpenGL SDK routes malloc into an app-owned heap (Mesa needs it); the JVM's native memory and LWJGL's buffers live there too
#ifndef GL_SELFTEST
extern "C" const size_t ps5_opengl_heap_size = 1536ull << 20;   // selftest keeps the SDK default (128 MiB), exactly like the SDK sample
#endif
#endif

int main();
static int sk = -1;
#ifdef LOG_HOST
static sockaddr_in dst;
#endif

static void sendraw(const char *b, int n) {
#ifdef LOG_HOST
    if (sk >= 0 && n > 0) { sendto(sk, b, n, 0, (sockaddr *)&dst, sizeof dst); usleep(300); }   // throttle: bursts overflow the listener
#else
    (void)b; (void)n;
#endif
}
#ifdef NATIVE_UI
extern "C" int kit_stage_run(const char *root, const char *assets);
#endif
static void out(const char *fmt, ...) {
    char b[1024];
    va_list v; va_start(v, fmt);
    int n = vsnprintf(b, sizeof b, fmt, v);
    va_end(v);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = sizeof b - 1;
    sendraw(b, n);
}
#ifdef VKPROBE
extern "C" int vkprobe_run(const char *tag, void (*logf)(const char *), int secs);   // vkprobe.c (make_vk_title.sh): Vulkan through the RADV linked into the title
static void vkp_log(const char *m) { out("%s\n", m); }
#endif
extern "C" void kit_log(const char *l) { out("%s\n", l); }
#ifndef LCFG_ROOT
#define LCFG_ROOT "/app0/mc"
#endif
#ifdef NATIVE_UI
// Kit.run(String root): the native launcher (PS5 UI kit) runs here, on the Java main thread, until the player presses Play.
static jint JNICALL native_kit_run(JNIEnv *e, jclass, jstring root) {
    const char *r = root ? e->GetStringUTFChars(root, 0) : 0;
    int rc = kit_stage_run(r ? r : LCFG_ROOT, "/app0/assets");
    if (r) e->ReleaseStringUTFChars(root, r);
    return rc;
}
extern "C" unsigned ps5_pad_peek(int ms);
static jint JNICALL native_kit_held(JNIEnv *, jclass, jint ms) { return (jint)ps5_pad_peek(ms); }
#endif

// HotSpot writes its messages (and hs_err reports) to fd 1/2. Point both at the connected UDP socket
// so every write() becomes a datagram to the listener (no pipe or reader thread needed).
// Beacons: sent paced (the start-up log lost most of the early ones), and the last 40 are kept so the exit / abort / SIGSYS paths can replay them ("[trace-last]"): whatever the listener dropped, the final steps before a silent exit arrive.
static char g_tring[40][160]; static volatile unsigned g_tseq; static volatile int g_treplayed;
static void trace_cb(const char *m) {
    char b[200]; int n = snprintf(b, sizeof b, "[trace] %s\n", m);
    unsigned i = __sync_fetch_and_add(&g_tseq, 1); snprintf(g_tring[i % 40], sizeof g_tring[0], "%s", m);
    sendraw(b, n < (int)sizeof b ? n : (int)sizeof b - 1);
    if (g_jdkv == 8 && i < 1500) usleep(2000);   // Java 8 bring-up only: the other JDKs' beacons arrived intact unpaced
}
static void trace_replay() {
    if (__sync_lock_test_and_set(&g_treplayed, 1)) return;
    unsigned e = g_tseq, b0 = e > 40 ? e - 40 : 0;
    for (unsigned i = b0; i < e; i++) { char b[200]; int n = snprintf(b, sizeof b, "[trace-last %u] %s\n", i, g_tring[i % 40]); sendraw(b, n < (int)sizeof b ? n : (int)sizeof b - 1); usleep(5000); }
}
static int redirect_fd(int s, int fd) {
    // NOTE: never close(fd) here. A failed redirect used to leave fd 1/2 closed, so the next handles the process opened
    // (GPU, video, event queues) became fd 1/2 and every stray write to stdout/stderr landed in them.
    if (dup2(s, fd) == fd) return 0;
    if (fcntl(s, 10 /* F_DUP2FD */, fd) == fd) return 1;
    return -1;
}
// Make sure fds 0-2 are always taken, so nothing else can ever be assigned them.
static void ensure_std_fds() {
    for (int i = 0; i <= 2; i++) {
        if (fcntl(i, F_GETFD) != -1) continue;
        int fd = open("/dev/null", O_RDWR);
        if (fd != i && fd >= 0) { if (dup2(fd, i) != i) { /* leave it */ } if (fd > 2) close(fd); }
    }
}
static void capture_stdio() {
#ifdef LOG_HOST
    // a second, connected socket: sendto() on a connected socket fails with EISCONN, so sk stays unconnected
    int s2 = socket(AF_INET, SOCK_DGRAM, 0);
    int c = connect(s2, (sockaddr *)&dst, sizeof dst);
    int r1 = redirect_fd(s2, 1), r2 = redirect_fd(s2, 2);
    out("capture connect=%d redirect(stdout)=%d redirect(stderr)=%d (0=dup2 1=F_DUP2FD 2=close+dup -1=failed)\n", c, r1, r2);
#endif
}

// file system checks: does the title's data folder look the way HotSpot expects?
static void ls(const char *p) {
    DIR *d = opendir(p);
    if (!d) { out("ls %s: opendir failed errno=%d\n", p, errno); return; }
    char line[600]; int n = snprintf(line, sizeof line, "ls %s:", p);
    struct dirent *e; int cnt = 0;
    while ((e = readdir(d)) && cnt++ < 40 && n < (int)sizeof line - 40) n += snprintf(line + n, sizeof line - n, " %s", e->d_name);
    out("%s\n", line);
    closedir(d);
}
static void probe_file(const char *p) {
    struct stat st; errno = 0;
    int r = stat(p, &st); int e1 = errno; errno = 0;
    int fd = open(p, O_RDONLY); int e2 = errno;
    out("file %s: stat=%d errno=%d size=%ld open=%d errno=%d\n", p, r, r ? e1 : 0, r == 0 ? (long)st.st_size : -1L, fd, fd < 0 ? e2 : 0);
    if (fd >= 0) { char b[8]; ssize_t n = read(fd, b, 4); out("  read=%ld first=%02x%02x%02x%02x\n", (long)n, b[0] & 255, b[1] & 255, b[2] & 255, b[3] & 255); close(fd); }
}

#ifdef MC_CP
// Diagnostics for the EPERM failures seen from Java: opendir() on any directory, and non-blocking sockets (NIO configureBlocking).
#include <sys/ioctl.h>
#include <netinet/in.h>
// declared under another C name (asm label) so it cannot clash with the SDK/libc prototype; weak, so the link works even if the symbol is absent
extern "C" long ps5_getdirentries(int, char *, long, long *) __asm__("getdirentries") __attribute__((weak));
static void probe_dir(const char *p) {
    struct { const char *n; int fl; } t[] = { { "RDONLY", O_RDONLY }, { "RDONLY|DIRECTORY", O_RDONLY | O_DIRECTORY }, { "RDONLY|NONBLOCK|DIRECTORY|CLOEXEC", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC } };
    int first = -1;
    for (auto &x : t) {
        errno = 0; int fd = open(p, x.fl); int e = errno;
        out("dirprobe %s open(%s) = %d errno=%d\n", p, x.n, fd, fd < 0 ? e : 0);
        if (fd >= 0) { if (first < 0) first = fd; else close(fd); }
    }
    if (first >= 0) {
        char buf[1024]; long base = 0; errno = 0;
        if (ps5_getdirentries) { int n = (int)ps5_getdirentries(first, buf, sizeof buf, &base); out("dirprobe getdirentries = %d errno=%d first=%.12s\n", n, n < 0 ? errno : 0, n > 8 ? buf + 8 : ""); }
        else out("dirprobe getdirentries: not linked\n");
        close(first);
    }
}
// ---- Which way of removing/renaming works? libc unlink/rmdir/rename fail with EPERM on /app0 and /download0 (McBoot's fsprobe). Try the alternatives. ----
extern "C" int sceKernelUnlink(const char *) __attribute__((weak));
extern "C" int sceKernelRename(const char *, const char *) __attribute__((weak));
extern "C" int sceKernelRmdir(const char *) __attribute__((weak));
static int exists_(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void touch_(const char *p) { int fd = open(p, O_WRONLY | O_CREAT, 0666); if (fd >= 0) { write(fd, "x", 1); close(fd); } }
static void rm_report(const char *name, long rc, const char *p) { out("rmprobe %-22s rc=%ld gone=%d\n", name, rc, !exists_(p)); }
static void probe_rm(const char *base, const char *phase) {
    char root[220], p[300], q[300];
    mkdir(base, 0777);
    snprintf(root, sizeof root, "%s/_rm%d_%s", base, (int)getpid(), phase);
    errno = 0; int mk = mkdir(root, 0777);
    out("rmprobe[%s] base=%s mkdir(root)=%d errno=%d\n", phase, base, mk, mk ? errno : 0);
    if (!exists_(root)) return;
#define NEWFILE(tag) do { snprintf(p, sizeof p, "%s/f_" tag, root); touch_(p); } while (0)
#define NEWDIR(tag) do { snprintf(p, sizeof p, "%s/d_" tag, root); mkdir(p, 0777); } while (0)
    NEWFILE("libc"); errno = 0; rm_report("unlink(libc)", unlink(p) ? -errno : 0, p);
    NEWFILE("remove"); errno = 0; rm_report("remove(libc)", remove(p) ? -errno : 0, p);
    NEWFILE("sce"); if (sceKernelUnlink) rm_report("sceKernelUnlink", sceKernelUnlink(p), p); else out("rmprobe sceKernelUnlink: not linked\n");
    NEWDIR("libc"); errno = 0; rm_report("rmdir(libc)", rmdir(p) ? -errno : 0, p);
    NEWDIR("sce"); if (sceKernelRmdir) rm_report("sceKernelRmdir", sceKernelRmdir(p), p); else out("rmprobe sceKernelRmdir: not linked\n");
    NEWFILE("r1"); snprintf(q, sizeof q, "%s/f_r1_to", root); errno = 0; rm_report("rename(libc)", rename(p, q) ? -errno : 0, p);
    NEWFILE("r2"); snprintf(q, sizeof q, "%s/f_r2_to", root); if (sceKernelRename) rm_report("sceKernelRename", sceKernelRename(p, q), p); else out("rmprobe sceKernelRename: not linked\n");
    NEWFILE("t"); { int fd = open(p, O_WRONLY | O_TRUNC); out("rmprobe %-22s fd=%d errno=%d\n", "open(O_TRUNC)", fd, fd < 0 ? errno : 0); if (fd >= 0) close(fd); }
    { errno = 0; int r = truncate(p, 0); out("rmprobe %-22s rc=%d errno=%d\n", "truncate()", r, r ? errno : 0); }
}
static void probe_stat_kinds(const char *base) {
    struct stat st; errno = 0; int a = stat(base, &st); int ea = errno; errno = 0; int b = lstat(base, &st); int eb = errno;
    out("rmprobe stat(%s)=%d errno=%d | lstat=%d errno=%d (lstat may already be the stat-based replacement from the shim)\n", base, a, a ? ea : 0, b, b ? eb : 0);
}
static void probe_rm_all(const char *phase) { probe_stat_kinds("/app0/mc"); probe_rm("/app0/mc", phase); probe_rm("/download0/mc", phase); }
// called from Java (McBoot) so the same operations are tried from inside the running JVM: Hello.rmprobe()
static void JNICALL native_rmprobe(JNIEnv *, jclass) { probe_rm_all("in-java"); }
static void probe_sock() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    errno = 0; int fl = fcntl(s, F_GETFL); out("sockprobe socket=%d F_GETFL=%d errno=%d\n", s, fl, fl < 0 ? errno : 0);
    errno = 0; int r = fcntl(s, F_SETFL, fl | O_NONBLOCK); out("sockprobe F_SETFL|O_NONBLOCK = %d errno=%d\n", r, r < 0 ? errno : 0);
    int on = 1; errno = 0; r = ioctl(s, FIONBIO, &on); out("sockprobe ioctl(FIONBIO) = %d errno=%d\n", r, r < 0 ? errno : 0);
    close(s);
}
#ifdef NETPROBE
// ---- NETPROBE=1: native network probe (plain POSIX calls, own thread so a hanging call cannot stall the boot) ----
// Why native: the JDK's getaddrinfo is a stub that always fails (ps5_jdk_patch.py), so Java-level DNS/HTTP "failures" say nothing about the console, and any Java socket with a
// timeout is switched to non-blocking by NIO, which this console refuses (fcntl EPERM). Here nothing is made non-blocking. SO_RCVTIMEO/SO_SNDTIMEO bound the waits (the set result is logged).
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
static long np_ms(const struct timespec &a) { struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b); return (b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L; }
static void np_timeouts(int s, int sec) {
    struct timeval tv; tv.tv_sec = sec; tv.tv_usec = 0;
    errno = 0; int a = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); int ea = a ? errno : 0;
    errno = 0; int b = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); int eb = b ? errno : 0;
    out("net: c: SO_RCVTIMEO=%d(errno %d) SO_SNDTIMEO=%d(errno %d)\n", a, ea, b, eb);
}
static int np_addr(sockaddr_in *a, const char *ip, int port) { memset(a, 0, sizeof *a); a->sin_family = AF_INET; a->sin_port = htons(port); return inet_pton(AF_INET, ip, &a->sin_addr); }
static int np_skip_name(const unsigned char *p, int n, int i) {      // DNS name (labels or compression pointer) -> index after it, or -1
    while (i < n) { int l = p[i]; if (l == 0) return i + 1; if ((l & 0xC0) == 0xC0) return i + 2 <= n ? i + 2 : -1; i += l + 1; }
    return -1;
}
// A-record lookup by hand over UDP (the only transport proven on this console): fills ip[4] with the first A record
static int np_dns(const char *server, const char *name, unsigned char ip[4]) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { out("net: c: dns %s: socket() = %d errno=%d\n", server, s, errno); return -1; }
    np_timeouts(s, 3);
    unsigned char q[300]; int n = 0;
    q[n++] = 0x5a; q[n++] = 0x17; q[n++] = 0x01; q[n++] = 0x00; q[n++] = 0; q[n++] = 1; for (int k = 0; k < 6; k++) q[n++] = 0;
    for (const char *p = name; *p;) { const char *d = strchr(p, '.'); int l = d ? (int)(d - p) : (int)strlen(p); q[n++] = (unsigned char)l; memcpy(q + n, p, l); n += l; p += l; if (d) p++; }
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    sockaddr_in a; np_addr(&a, server, 53);
    errno = 0; ssize_t sr = sendto(s, q, n, 0, (sockaddr *)&a, sizeof a);
    if (sr != n) { out("net: c: dns %s: sendto = %ld errno=%d\n", server, (long)sr, errno); close(s); return -1; }
    unsigned char r[600]; errno = 0; ssize_t rr = recvfrom(s, r, sizeof r, 0, 0, 0); int er = errno;
    close(s);
    if (rr < 12) { out("net: c: dns %s: recvfrom = %ld errno=%d (%ld ms): no reply\n", server, (long)rr, er, np_ms(t0)); return -1; }
    int an = (r[6] << 8) | r[7], rcode = r[3] & 15, i = 12;
    i = np_skip_name(r, (int)rr, i); i = i < 0 ? -1 : i + 4;
    int got = 0;
    for (int k = 0; k < an && i > 0 && i < (int)rr; k++) {
        i = np_skip_name(r, (int)rr, i); if (i < 0 || i + 10 > (int)rr) break;
        int type = (r[i] << 8) | r[i + 1], rdlen = (r[i + 8] << 8) | r[i + 9]; i += 10;
        if (type == 1 && rdlen == 4 && i + 4 <= (int)rr) { if (!got) memcpy(ip, r + i, 4); got++; }
        i += rdlen;
    }
    out("net: c: dns %s: %ld bytes in %ld ms, rcode=%d answers=%d A-records=%d%s\n", server, (long)rr, np_ms(t0), rcode, an, got, got ? "" : " (none)");
    if (got) out("net: c: dns %s: %s -> %u.%u.%u.%u\n", server, name, ip[0], ip[1], ip[2], ip[3]);
    return got ? 0 : -1;
}
// blocking connect (no fcntl/ioctl), then the non-blocking-style calls we might fall back on (recv MSG_DONTWAIT, poll), then optionally a plain HTTP request
static int np_tcp(const char *ip, int port, const char *host, int http) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { out("net: c: tcp %s:%d socket() = %d errno=%d\n", ip, port, s, errno); return -1; }
    np_timeouts(s, 5);
    sockaddr_in a; np_addr(&a, ip, port);
    out("net: c: tcp %s:%d connect() ...\n", ip, port);
    errno = 0; int c = connect(s, (sockaddr *)&a, sizeof a); int ec = errno;
    out("net: c: tcp %s:%d connect() = %d errno=%d (%ld ms)\n", ip, port, c, c ? ec : 0, np_ms(t0));
    if (c) { close(s); return -1; }
    char b[512];
    errno = 0; ssize_t d = recv(s, b, 1, MSG_DONTWAIT); out("net: c: tcp recv(MSG_DONTWAIT) before any data = %ld errno=%d (EAGAIN is %d)\n", (long)d, d < 0 ? errno : 0, EAGAIN);
    struct pollfd pf; pf.fd = s; pf.events = POLLIN; pf.revents = 0;
    errno = 0; int pr = poll(&pf, 1, 0); out("net: c: tcp poll(POLLIN, 0 ms) = %d revents=0x%x errno=%d\n", pr, pf.revents, pr < 0 ? errno : 0);
    if (http) {
        int n = snprintf(b, sizeof b, "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: ps5-mc\r\nConnection: close\r\n\r\n", host);
        errno = 0; ssize_t w = send(s, b, n, 0); out("net: c: tcp send(%d) = %ld errno=%d\n", n, (long)w, w < 0 ? errno : 0);
        pf.revents = 0; errno = 0; pr = poll(&pf, 1, 5000); out("net: c: tcp poll(POLLIN, 5000 ms) = %d revents=0x%x errno=%d\n", pr, pf.revents, pr < 0 ? errno : 0);
        errno = 0; ssize_t r = recv(s, b, sizeof b - 1, 0);
        if (r > 0) { b[r] = 0; for (char *p = b; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; } out("net: c: tcp reply %ld bytes, first line: %.100s\n", (long)r, b); }
        else out("net: c: tcp recv = %ld errno=%d\n", (long)r, errno);
    }
    close(s);
    return 0;
}
// can a socket be non-blocking without fcntl/ioctl (both refused)? SOCK_NONBLOCK at creation, then connect/poll/read/accept behaviour on such sockets; plus whether a listening socket works at all
static void np_nonblock() {
#ifdef SOCK_NONBLOCK
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0; int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); int e0 = errno;
    int fl = s >= 0 ? fcntl(s, F_GETFL) : -1;
    out("net: c: socket(SOCK_NONBLOCK) = %d errno=%d, F_GETFL=0x%x (O_NONBLOCK=0x%x -> %s)\n", s, s < 0 ? e0 : 0, fl, O_NONBLOCK, fl >= 0 && (fl & O_NONBLOCK) ? "set" : "NOT set");
    if (s >= 0) {
        sockaddr_in a; np_addr(&a, "1.1.1.1", 80);
        errno = 0; int c = connect(s, (sockaddr *)&a, sizeof a); int ec = errno;
        out("net: c: nonblocking connect() = %d errno=%d (EINPROGRESS is %d) (%ld ms)\n", c, c ? ec : 0, EINPROGRESS, np_ms(t0));
        if (c < 0 && ec == EINPROGRESS) {
            struct pollfd pf; pf.fd = s; pf.events = POLLOUT; pf.revents = 0;
            errno = 0; int pr = poll(&pf, 1, 5000); int soerr = -1; socklen_t sl = sizeof soerr; getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            out("net: c: poll(POLLOUT, 5000 ms) = %d revents=0x%x SO_ERROR=%d (%ld ms)\n", pr, pf.revents, soerr, np_ms(t0));
            char b[16]; errno = 0; ssize_t r = read(s, b, 1); out("net: c: read() on the non-blocking socket, no data yet = %ld errno=%d (EAGAIN is %d)\n", (long)r, r < 0 ? errno : 0, EAGAIN);
        }
        close(s);
    }
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ls >= 0) {
        sockaddr_in a; np_addr(&a, "0.0.0.0", 0);
        errno = 0; int b = bind(ls, (sockaddr *)&a, sizeof a); int eb = errno;
        errno = 0; int l = b ? -1 : listen(ls, 5); int el = errno;
        socklen_t al = sizeof a; sockaddr_in got; memset(&got, 0, sizeof got); int gs = getsockname(ls, (sockaddr *)&got, &al);
        out("net: c: listen socket: bind=%d errno=%d listen=%d errno=%d getsockname=%d port=%d\n", b, b ? eb : 0, l, l ? el : 0, gs, ntohs(got.sin_port));
        if (!b && !l) {
            { struct pollfd pf; pf.fd = ls; pf.events = POLLIN; pf.revents = 0; errno = 0; int pr = poll(&pf, 1, 0); out("net: c: poll(listener, 0 ms) with nobody connecting = %d revents=0x%x (expect 0; accept() is NOT called here: on this console it blocks even on a SOCK_NONBLOCK socket)\n", pr, pf.revents); }
            int cs = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in lo; np_addr(&lo, "127.0.0.1", ntohs(got.sin_port));
            errno = 0; int cc = connect(cs, (sockaddr *)&lo, sizeof lo); out("net: c: loopback connect to our own listener = %d errno=%d\n", cc, cc ? errno : 0);
            if (!cc) {
                struct pollfd pf; pf.fd = ls; pf.events = POLLIN; pf.revents = 0; int pr = poll(&pf, 1, 2000);
                errno = 0; int ac2 = accept(ls, 0, 0); int f2 = ac2 >= 0 ? fcntl(ac2, F_GETFL) : -1;
                out("net: c: poll(listener) = %d revents=0x%x, accept() = %d errno=%d, accepted socket F_GETFL=0x%x (non-blocking inherited: %s)\n", pr, pf.revents, ac2, ac2 < 0 ? errno : 0, f2, f2 >= 0 && (f2 & O_NONBLOCK) ? "yes" : "no");
                if (ac2 >= 0) close(ac2);
            }
            close(cs);
        }
        // what the JDK Selector relies on: poll() on a pipe (its wake-up pipe) and on several fds at once
        int pp[2];
        if (pipe(pp) == 0) {
            struct pollfd pf[2]; pf[0].fd = pp[0]; pf[0].events = POLLIN; pf[0].revents = 0; pf[1].fd = ls; pf[1].events = POLLIN; pf[1].revents = 0;
            errno = 0; int r1 = poll(pf, 1, 0); out("net: c: poll(pipe read end, 0 ms) = %d revents=0x%x errno=%d\n", r1, pf[0].revents, r1 < 0 ? errno : 0);
            errno = 0; int r2 = poll(pf, 2, 0); out("net: c: poll(pipe + listener, 0 ms) = %d revents=0x%x/0x%x errno=%d\n", r2, pf[0].revents, pf[1].revents, r2 < 0 ? errno : 0);
            errno = 0; int w = (int)write(pp[1], "x", 1); pf[0].revents = 0; errno = 0; int r3 = poll(pf, 2, 100); out("net: c: pipe write=%d, poll(pipe + listener, 100 ms) = %d revents=0x%x/0x%x errno=%d\n", w, r3, pf[0].revents, pf[1].revents, r3 < 0 ? errno : 0);
            errno = 0; int f1 = fcntl(pp[0], F_SETFL, O_NONBLOCK); out("net: c: fcntl(pipe, F_SETFL, O_NONBLOCK) = %d errno=%d\n", f1, f1 < 0 ? errno : 0);
            close(pp[0]); close(pp[1]);
        } else out("net: c: pipe() failed errno=%d\n", errno);
        close(ls);
    } else out("net: c: listen socket: socket() = %d errno=%d\n", ls, errno);
#else
    out("net: c: SOCK_NONBLOCK is not defined in these headers\n");
#endif
}
// Which bind() calls does the console allow? Java's "Open to LAN" does ServerSocket(0) -> port P, then binds the wildcard address on P and got BindException "Permission denied" while bind to port 0 works.
// One line per (family, address, SO_REUSEADDR): port=ok or port=errno, ports: 0, a fresh ephemeral port E (learned from bind(0), then closed), E again, and a few fixed ones.
static void np_bind_row(int family, const char *addr, int reuse, int eph) {
    int ports[] = { 0, eph, eph, 25565, 8080, 1025, 3000, 20000, 40000, 49152, 55555, 65000 };
    char line[400]; int n = snprintf(line, sizeof line, "net: c: bind %s %s reuse=%d:", family == AF_INET ? "inet" : "inet6", addr, reuse);
    for (unsigned i = 0; i < sizeof ports / sizeof *ports && n < (int)sizeof line - 24; i++) {
        int s = socket(family, SOCK_STREAM, 0); if (s < 0) { n += snprintf(line + n, sizeof line - n, " socket()=errno %d", errno); break; }
        if (reuse) { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
        int r; errno = 0;
        if (family == AF_INET) { sockaddr_in a; np_addr(&a, addr, ports[i]); r = bind(s, (sockaddr *)&a, sizeof a); }
        else { sockaddr_in6 a; memset(&a, 0, sizeof a); a.sin6_family = AF_INET6; a.sin6_port = htons(ports[i]); inet_pton(AF_INET6, addr, &a.sin6_addr); r = bind(s, (sockaddr *)&a, sizeof a); }
        int e = errno; int l = -2, el = 0;
        if (!r) { errno = 0; l = listen(s, 5); el = errno; }
        if (r) n += snprintf(line + n, sizeof line - n, " %d=errno%d", ports[i], e);
        else if (l) n += snprintf(line + n, sizeof line - n, " %d=bind-ok/listen-errno%d", ports[i], el);
        else n += snprintf(line + n, sizeof line - n, " %d=ok", ports[i]);
        close(s);
    }
    out("%s\n", line);
}
static void np_bind_matrix() {
    int eph = 0;
    { int s = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a; np_addr(&a, "0.0.0.0", 0); if (s >= 0 && !bind(s, (sockaddr *)&a, sizeof a)) { socklen_t l = sizeof a; getsockname(s, (sockaddr *)&a, &l); eph = ntohs(a.sin_port); } if (s >= 0) close(s); }
    out("net: c: bind matrix, fresh ephemeral port E = %d (listed twice: right after it was released, and again)\n", eph);
    np_bind_row(AF_INET, "0.0.0.0", 1, eph);
    np_bind_row(AF_INET, "0.0.0.0", 0, eph);
    np_bind_row(AF_INET, "127.0.0.1", 1, eph);
    np_bind_row(AF_INET6, "::", 1, eph);
    np_bind_row(AF_INET6, "::ffff:127.0.0.1", 1, eph);
}
// where exactly is the explicit-bind boundary? bind 0.0.0.0:P for many ports and print the runs of ok / errno
static void np_bind_scan() {
    char line[600]; int n = snprintf(line, sizeof line, "net: c: bind scan 0.0.0.0:"); int prev = -999, start = 0, last = 0;
    static const int fixed[] = { 1, 22, 80, 443, 1000, 1023, 1024 };
    for (int pass = 0; pass < 2; pass++) {
        int count = pass == 0 ? (int)(sizeof fixed / sizeof *fixed) : (65000 - 1100) / 250 + 1;
        for (int i = 0; i < count; i++) {
            int port = pass == 0 ? fixed[i] : 1100 + i * 250;
            int s = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a; np_addr(&a, "0.0.0.0", port); errno = 0; int r = bind(s, (sockaddr *)&a, sizeof a); int code = r ? errno : 0; close(s);
            if (code != prev) { if (prev != -999) n += snprintf(line + n, sizeof line - n, " %d-%d=%s%d", start, last, prev ? "errno" : "ok", prev); prev = code; start = port; }
            last = port;
        }
    }
    snprintf(line + n, sizeof line - n, " %d-%d=%s%d", start, last, prev ? "errno" : "ok", prev);
    out("%s\n", line);
}
static void *np_thread(void *) {
    out("net: c: native probe start (blocking POSIX calls, own thread)\n");
    unsigned char ip[4] = { 0, 0, 0, 0 }; char sip[32] = "";
    const char *servers[] = { "1.1.1.1", "8.8.8.8" };
    for (int i = 0; i < 2 && !sip[0]; i++) if (np_dns(servers[i], "piston-meta.mojang.com", ip) == 0) snprintf(sip, sizeof sip, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    np_tcp("1.1.1.1", 80, "1.1.1.1", 1);                       // internet TCP without any DNS
    if (sip[0]) np_tcp(sip, 80, "piston-meta.mojang.com", 1);   // the name we actually need, via the hand-made lookup
    np_nonblock();
    np_bind_matrix();
    np_bind_scan();
    out("net: c: native probe done\n");
    return 0;
}
static void netprobe_start() { pthread_t t; if (pthread_create(&t, 0, np_thread, 0) == 0) pthread_detach(t); else out("net: c: probe thread FAILED\n"); }
#else
static void netprobe_start() {}
#endif
#endif
// HotSpot 21 (and libc++abi with it) registers destructors of thread_local objects through the glibc entry point __cxa_thread_atexit_impl, which the SDK stubs do not export:
// the title converter stops with "no public SDK stub exports required symbol __cxa_thread_atexit_impl". Provide it. The destructors are not run at thread exit (a few small per-thread objects leak
// when a thread ends; the JVM's threads live as long as the process).
extern "C" int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso_handle) { (void)dtor; (void)obj; (void)dso_handle; return 0; }
// ---- launcher settings read BEFORE the JVM exists: <root>/launcher.cfg names the current instance, <root>/instances/<id>/instance.cfg holds its JVM settings (plain key=value lines, written by the launcher).
// Keys: xmx, xms (MB), cpus (processor count the JVM reports), jitThreads, gc (serial|parallel). Anything missing or out of range keeps the build-time default. Everything else in the file is read by Launcher.java.
#ifndef LCFG_ROOT
#define LCFG_ROOT "/app0/mc"
#endif
#ifndef JIT_THREADS
#define JIT_THREADS 1
#endif
struct LCfg { int java = 0; int xmx = 0, xms = 0, cpus = 0, jit = 0; int merge = -1, uncap = -1, glthread = -1; char gc[16] = ""; char renderer[8] = ""; int hz = 0, zglt = 0, vsync = -1, res = 0; char inst[64] = ""; char ver[64] = ""; int jdk = 0; };   // jdk: launcher.cfg jdk=17|21|25 (the global Java setting) forces that Java unless the version needs a newer one (0 = automatic)   // merge / uncap / glthread: -1 = build default, 0 = off, 1 = on
static void lcfg_file(const char *path, LCfg *c, bool top) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char ln[256];
    while (fgets(ln, sizeof ln, f)) {
        char *e = strchr(ln, '='); if (!e || ln[0] == '#') continue;
        *e++ = 0; for (char *t = e + strlen(e); t > e && (t[-1] == '\n' || t[-1] == '\r' || t[-1] == ' '); ) *--t = 0;
        while (*e == ' ') e++;
        if (top) { if (!strcmp(ln, "instance")) snprintf(c->inst, sizeof c->inst, "%s", e); else if (!strcmp(ln, "java")) c->java = atoi(e); else if (!strcmp(ln, "jdk")) c->jdk = atoi(e); else if (!strcmp(ln, "renderer")) snprintf(c->renderer, sizeof c->renderer, "%s", e); else if (!strcmp(ln, "hz")) c->hz = atoi(e); else if (!strcmp(ln, "resolution")) c->res = atoi(e); else if (!strcmp(ln, "vsync")) c->vsync = !strcmp(e, "always") || !strcmp(e, "on") || !strcmp(e, "1"); else if (!strcmp(ln, "zglthread")) c->zglt = !strcmp(e, "on") || !strcmp(e, "1"); else if (!strncmp(ln, "env.", 4) && ln[4]) { setenv(ln + 4, e, 1); out("launcher: env %s=%s (launcher.cfg env.%s)\n", ln + 4, e, ln + 4); } continue; }   // env.NAME=value: any environment variable for the process (Mesa / Zink / RADV switches can be tried without a rebuild, e.g. env.ZINK_DEBUG=nobgc)   // java=<major Java the last chosen version needs>, written by McBoot
        if (!strcmp(ln, "version")) snprintf(c->ver, sizeof c->ver, "%s", e); else if (top && !strcmp(ln, "xmx")) c->xmx = atoi(e); else if (top && !strcmp(ln, "xms")) c->xms = atoi(e);   /* memory is a launcher-wide setting (launcher.cfg): an old xmx= in an instance.cfg is ignored */ else if (!strcmp(ln, "cpus")) c->cpus = atoi(e);
        else if (!strcmp(ln, "jitThreads")) c->jit = atoi(e); else if (!strcmp(ln, "gc")) snprintf(c->gc, sizeof c->gc, "%s", e);
        else if (!strcmp(ln, "merge") || !strcmp(ln, "uncap") || !strcmp(ln, "glthread")) {   // on|off (also 1|0)
            int on = !strcmp(e, "on") || !strcmp(e, "1") ? 1 : !strcmp(e, "off") || !strcmp(e, "0") ? 0 : -1;
            if (ln[0] == 'm') c->merge = on; else if (ln[0] == 'u') c->uncap = on; else c->glthread = on;
        }
    }
    fclose(f);
}
// Java the instance's Minecraft version needs: javaVersion.majorVersion of <root>/versions/<id>/<id>.json, following inheritsFrom (Fabric/Quilt profiles have none of their own); 0 = unknown (not installed, no such field)
static int ver_java_need(const char *id, int depth) {
    if (!id[0] || depth > 5) return 0;
    for (const char *t = id; *t; t++) if (!((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || *t == '.' || *t == '_' || *t == '-' || *t == '+')) return 0;
    char path[200]; snprintf(path, sizeof path, LCFG_ROOT "/versions/%s/%s.json", id, id);
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n <= 0 || n > (8 << 20)) { fclose(f); return 0; }
    char *b = (char *)malloc(n + 1); if (!b) { fclose(f); return 0; }
    n = (long)fread(b, 1, n, f); fclose(f); b[n] = 0;
    int r = 0;
    if (char *j = strstr(b, "\"javaVersion\"")) { if (char *m = strstr(j, "\"majorVersion\"")) { m = strchr(m + 14, ':'); if (m) r = atoi(m + 1); } }
    else if (char *h = strstr(b, "\"inheritsFrom\"")) { h = strchr(h + 14, '"'); if (h) { char par[64]; int i = 0; for (h++; *h && *h != '"' && i < 63; h++) par[i++] = *h; par[i] = 0; r = ver_java_need(par, depth + 1); } }
    else r = 8;   // older version JSONs have no javaVersion: Java 8 era
    free(b); return r;
}
extern "C" int ps5_java_need_of(const char *id) { return id && *id ? ver_java_need(id, 0) : 0; }   // the Java major version <root>/versions/<id>/<id>.json asks for (0 = unknown), for the launcher's Java notes
static void lcfg_load(LCfg *c) {
    lcfg_file(LCFG_ROOT "/launcher.cfg", c, true);
    for (char *t = c->inst; *t; t++) if (!((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || *t == '.' || *t == '_' || *t == '-')) { c->inst[0] = 0; break; }   // an id is a folder name: no '/'
    if (!c->inst[0] || c->inst[0] == '.') snprintf(c->inst, sizeof c->inst, "default");
    char path[200]; snprintf(path, sizeof path, LCFG_ROOT "/instances/%s/instance.cfg", c->inst);
    lcfg_file(path, c, false);
    if (c->vsync >= 0) c->uncap = c->vsync ? 0 : 1;   // launcher.cfg vsync=always|minecraft is launcher-wide and wins over an old per-instance uncap=
    { int need = ver_java_need(c->ver, 0); if (need > 0) c->java = need; }   // the Java is chosen from the instance's version itself; launcher.cfg java= (recorded at Play) is the fallback
    if (c->xmx && (c->xmx < 768 || c->xmx > 8192)) { out("launcher: xmx=%d is outside 768..8192 MB, ignored\n", c->xmx); c->xmx = 0; }   // above 4096 only the reservation grows (see heap_start_mb)
    if (c->xms && (c->xms < 128 || c->xms > 8192 || (c->xmx && c->xms > c->xmx))) c->xms = 0;
    if (c->cpus && (c->cpus < 1 || c->cpus > 8)) c->cpus = 0;
    if (c->jit && (c->jit < 1 || c->jit > 4)) c->jit = 0;
    if (c->gc[0] && strcmp(c->gc, "serial") && strcmp(c->gc, "parallel")) c->gc[0] = 0;
}
// ---- back to the launcher: when Minecraft quits (or the VM crashes) the title starts itself again, so the launcher shows instead of a frozen screen ----
// sceSystemServiceLoadExec replaces this process by a fresh start of the same eboot (Kit.show then runs the launcher as always). Every step is logged, because which of
// these calls the console accepts for this title is only known from a real run. Guards: never when the launcher is off (ui=off), never twice within 20 s (a crash at boot
// must not loop). If nothing works the title ends (back to the PS5 home screen) instead of freezing.
static volatile int g_in_rtl;   // set while the launcher return runs (see crash())
static volatile int g_deliberate;   // set by native_quit: Hello.quit from Java is a user action, never a boot crash
#ifdef MC_CP
#ifndef TITLE_ID
#define TITLE_ID "PPSA27272"
#endif
extern "C" int sceSystemServiceLoadExec(const char *path, char *const argv[]) __attribute__((weak));
extern "C" int sceSystemServiceLaunchApp(const char *titleId, char *const argv[], void *param) __attribute__((weak));
extern "C" int sceSystemServiceGetAppIdOfRunningBigApp(void) __attribute__((weak));
extern "C" int sceSystemServiceKillApp(int appId, int how, int reason, int core_dump) __attribute__((weak));
extern "C" int sceSystemServiceNavigateToGoHome(void) __attribute__((weak));
extern "C" int sceUserServiceGetInitialUser(int *) __attribute__((weak));
static void rtl_write(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, text, strlen(text)); close(fd);
}
// Every step is announced BEFORE it runs: a call the kernel rejects (SIGSYS) or that never returns still leaves its name in the log.
// Logging of this path: the UDP listener drops lines that arrive in bursts (a crash dump right before swallowed several of them), so every line goes (1) to UDP with a pause after it
// and (2) into <root>/.kit/exitlog.txt, which the NEXT start prints again (prefix "[prev]") and deletes.
// g_in_rtl: while the steps run, a SIGSYS (the kernel refusing one of these calls) is answered with an error and the next step is tried (outside of it the handler parks the thread).
static long g_t0_ms;   // process start (set in main) for the crash-loop guard
static long mono_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static void exitlog(const char *fmt, ...) {
    char b[400]; va_list v; va_start(v, fmt); int n = vsnprintf(b, sizeof b, fmt, v); va_end(v); if (n < 0) return; if (n >= (int)sizeof b) n = sizeof b - 1;
    int fd = open(LCFG_ROOT "/.kit/exitlog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { char l[420]; int m = snprintf(l, sizeof l, "%s\n", b); write(fd, l, m); close(fd); }
    out("[jvm] %s\n", b); usleep(120000);
}
static void *exitlog_replay_thread(void *) {   // a few seconds after start (the boot burst drops lines): the previous run's exit log, kept as exitlog.last until the next exit writes a new one
    sleep(8);
    const char *p = LCFG_ROOT "/.kit/exitlog.txt"; FILE *f = fopen(p, "r"); int fresh = f != 0;
    if (!f) { p = LCFG_ROOT "/.kit/exitlog.last"; f = fopen(p, "r"); }
    if (!f) { out("[prev] no exit log of the previous run (neither exitlog.txt nor exitlog.last exists)\n"); return 0; }
    char ln[420]; out("[prev] ---- exit log of the previous run (%s) ----\n", fresh ? "new" : "seen before"); usleep(200000);
    while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; out("[prev] %s\n", ln); usleep(150000); }
    fclose(f); if (fresh) rename(LCFG_ROOT "/.kit/exitlog.txt", LCFG_ROOT "/.kit/exitlog.last"); out("[prev] ---- end ----\n");
    return 0;
}
static void exitlog_replay() { pthread_t t; if (pthread_create(&t, 0, exitlog_replay_thread, 0) == 0) pthread_detach(t); }
typedef int (*fn_loadexec)(const char *, char *const[]); typedef int (*fn_launch)(const char *, char *const[], void *); typedef int (*fn_appid)(void); typedef int (*fn_kill)(int, int, int, int); typedef int (*fn_home)(void);
extern "C" int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, const void *, int *) __attribute__((weak));
extern "C" int sceKernelDlsym(int, const char *, void **) __attribute__((weak));
extern "C" int sceKernelGetModuleInfo(int, void *) __attribute__((weak));
static fn_loadexec p_loadexec; static fn_launch p_launch; static fn_appid p_appid; static fn_kill p_kill; static fn_home p_home;
static void rtl_resolve() {   // linked weak symbols first; if the title does not link libSceSystemService, load the module and look the functions up by name
    p_loadexec = (fn_loadexec)sceSystemServiceLoadExec; p_launch = (fn_launch)sceSystemServiceLaunchApp; p_appid = (fn_appid)sceSystemServiceGetAppIdOfRunningBigApp; p_kill = (fn_kill)sceSystemServiceKillApp; p_home = (fn_home)sceSystemServiceNavigateToGoHome;
    if ((!p_loadexec || !p_launch || !p_kill || !p_home || !p_appid) && sceKernelLoadStartModule && sceKernelDlsym) {
        int res = 0; int h = sceKernelLoadStartModule("libSceSystemService.sprx", 0, 0, 0, 0, &res); exitlog("LoadStartModule(libSceSystemService.sprx) = %d (res %d)", h, res);
        if (h >= 0) {
            // The console's system libraries export by hashed name (NID = base64 of the first 8 bytes of SHA1(name + fixed salt)): a lookup by the plain name fails with 0x80020003, the NID is tried second.
            struct { const char *name, *nid; void **dst; } T[] = {
                { "sceSystemServiceLoadExec", "IMt1C0lqgCY", (void **)&p_loadexec }, { "sceSystemServiceLaunchApp", "B-xaA99BgZc", (void **)&p_launch },
                { "sceSystemServiceGetAppIdOfRunningBigApp", "--BomUf2BDc", (void **)&p_appid }, { "sceSystemServiceKillApp", "0BZ7mMhkhDc", (void **)&p_kill },
                { "sceSystemServiceNavigateToGoHome", "ld1w4PXob8c", (void **)&p_home } };
            // The module's exports are stored as "<NID>#<lib id>#<module id>"; dlsym needs that complete string. Look for it in the module's own mapped segments (only pages the kernel says are mapped are read).
            struct { size_t size; char name[256]; struct { void *addr; unsigned size; int prot; } seg[4]; unsigned nseg; unsigned char fp[20]; } mi;
            memset(&mi, 0, sizeof mi); mi.size = sizeof mi; int gr = sceKernelGetModuleInfo ? sceKernelGetModuleInfo(h, &mi) : -1;
            exitlog("module info rc 0x%x, %u segments", gr, gr == 0 ? mi.nseg : 0);
            for (auto &t : T) {
                if (*t.dst) continue;
                void *a = 0; int r = sceKernelDlsym(h, t.name, &a); exitlog("dlsym %s = 0x%x -> %p", t.name + 16, r, a);
                if (r != 0 || !a) { a = 0; r = sceKernelDlsym(h, t.nid, &a); exitlog("dlsym by NID %s = 0x%x -> %p", t.nid, r, a); }
                if ((r != 0 || !a) && gr == 0) {
                    char key[16]; snprintf(key, sizeof key, "%s#", t.nid); size_t kl = strlen(key); char full[64] = "";
                    for (unsigned i = 0; i < mi.nseg && i < 4 && !full[0]; i++) {
                        char *base = (char *)mi.seg[i].addr; size_t sz = mi.seg[i].size; if (!base || sz < kl || sz > (256u << 20)) continue;
                        for (size_t off = 0; off + kl <= sz && !full[0]; off += 4096) {   // page by page: unmapped / unreadable pages are skipped
                            char vec[2]; size_t pl = sz - off < 4096 + 64 ? sz - off : 4096 + 64;
                            if (mincore(base + off, pl > 4096 ? 8192 : pl, vec) != 0) continue;
                            for (size_t k = 0; k + kl <= pl; k++) if (!memcmp(base + off + k, key, kl)) { size_t n = 0; while (n < 40 && off + k + n < sz && base[off + k + n]) { full[n] = base[off + k + n]; n++; } full[n] = 0; break; }
                        }
                    }
                    if (full[0]) { a = 0; r = sceKernelDlsym(h, full, &a); exitlog("dlsym by export name %s = 0x%x -> %p", full, r, a); }
                    else exitlog("export name of %s not found in the module", t.nid);
                }
                if (r == 0 && a) *t.dst = a;
            }
        }
    }
}
// prepare: remembers why, decides whether a restart is allowed. Returns a bit mask: bit 0 LoadExec, 1 LaunchApp, 2 KillApp, 3 GoHome exist; 0x1000 = do not restart (ui=off / the process lived < 12 s = crashed at boot).
static int rtl_prepare(const char *what) {
    g_in_rtl = 1;
    exitlog("return to launcher (%s), build back2l-14, process age %ld s", what, (mono_ms() - g_t0_ms) / 1000);
    char buf[160]; snprintf(buf, sizeof buf, "%s\n", what);
    rtl_write(LCFG_ROOT "/.kit/lastexit.txt", buf);   // the launcher shows what happened
    bool ui_off = false;
    if (FILE *f = fopen(LCFG_ROOT "/launcher.cfg", "r")) { char ln[200]; while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "ui=off", 6)) ui_off = true; fclose(f); }
    rtl_resolve();
    int mask = (p_loadexec ? 1 : 0) | (p_launch ? 2 : 0) | (p_kill ? 4 : 0) | (p_home ? 8 : 0);
    if (strstr(what, "app exit")) { exitlog("launcher.cfg onexit=app: the game is over, the title ends"); mask |= 0x1000; }
    else if (ui_off) { exitlog("launcher is off (ui=off): the title ends"); mask |= 0x1000; }
    else if (!g_deliberate && mono_ms() - g_t0_ms < 12000) { exitlog("the process is younger than 12 s (crashed at boot?): not restarting, the title ends"); mask |= 0x1000; }
    exitlog("symbols mask 0x%x", mask);
    return mask;
}
static int rtl_step(int n) {
    g_in_rtl = 1; int rc = -1;
    switch (n) {
    case 1: if (p_loadexec) { exitlog("step 1: sceSystemServiceLoadExec(/app0/eboot.bin) ..."); char *argv[] = { (char *)"/app0/eboot.bin", 0 }; rc = p_loadexec("/app0/eboot.bin", argv); exitlog("step 1 returned 0x%x (it only returns when it failed)", rc); } else exitlog("step 1: no LoadExec"); break;
    case 2: if (p_launch) {
            struct { unsigned size; int user; int attr; unsigned crash; unsigned long long flag; } param = { sizeof param, 0, 0, 0, 0 };
            int uid = 0; if (sceUserServiceGetInitialUser && sceUserServiceGetInitialUser(&uid) == 0) param.user = uid;
#ifdef RETURN_TITLE_ID
#define RTL_TITLE RETURN_TITLE_ID
#else
#define RTL_TITLE TITLE_ID
#endif
            exitlog("step 2: sceSystemServiceLaunchApp(%s, user %d) ...", RTL_TITLE, param.user);
            rc = p_launch(RTL_TITLE, 0, &param); exitlog("step 2 returned 0x%x", rc);
            for (int i = 0; i < 40 && rc == 0; i++) usleep(100000);   // the new instance takes over; this one is closed by the system
        } else exitlog("step 2: no LaunchApp"); break;
    case 3: if (p_kill && p_appid) {
            int id = p_appid(); exitlog("step 3: sceSystemServiceKillApp(appId 0x%x) ...", id);
            rc = p_kill(id, -1, 0, 0); exitlog("step 3 returned 0x%x", rc);
            for (int i = 0; i < 30; i++) usleep(100000);
        } else exitlog("step 3: no KillApp"); break;
    case 4: if (p_home) { exitlog("step 4: sceSystemServiceNavigateToGoHome ..."); rc = p_home(); exitlog("step 4 returned 0x%x", rc); for (int i = 0; i < 20; i++) usleep(100000); } else exitlog("step 4: no GoHome"); break;
    case 5: exitlog("step 5: kill(getpid(), SIGKILL) (the exit syscall itself is refused by the console, a signal is not) ..."); usleep(200000); kill(getpid(), SIGKILL); usleep(500000);
            exitlog("step 5b: _exit(0) ..."); _exit(0); exitlog("_exit returned: the title cannot end itself, close it from the home screen"); break;
    }
    return rc;
}
static void return_to_launcher(const char *what) {
    static volatile int once; if (once++) for (;;) usleep(1000000);
    int mask = rtl_prepare(what);
#ifdef RETURN_TITLE_ID   // the Vulkan title has no launcher of its own: back means starting the launcher title (step 2), never this one again (step 1)
    for (int n = (mask & 0x1000) ? 3 : 2; n <= 5; n++) rtl_step(n);
#else
    for (int n = (mask & 0x1000) ? 3 : 1; n <= 5; n++) rtl_step(n);
#endif
    for (;;) usleep(1000000);
}
#else
static void return_to_launcher(const char *) { for (;;) usleep(1000000); }
#endif
// The thread that hit a fault may be half-dead (its own exit was refused: stack / TLS already released), so the way back to the launcher runs on a FRESH thread
// and the faulting one just parks. If no thread can be created, run it right here.
// A thread that has started to die (or sits in a fault handler) cannot be trusted to sleep, create threads or log: the second log showed the handler going silent right
// after "CRASH end". So the faulting / exiting thread only RAISES A FLAG (no calls), and a monitor thread started in main() does the actual work.
static volatile int g_quit_req, g_quit_claimed; static char g_quit_what[64];
static void quit_request(const char *what) {
    if (__sync_lock_test_and_set(&g_quit_claimed, 1)) return;   // the first reason wins
    int i = 0; for (; what[i] && i < 63; i++) g_quit_what[i] = what[i]; g_quit_what[i] = 0;
    __sync_synchronize(); g_quit_req = 1;
}
extern "C" void ps5_host_quit(const char *why) { quit_request(why); }   // ps5lwjgl.c: the pad quit combo (L1+R1+Share+Options) when the game did not close by itself; the monitor thread returns to the launcher
static void rescue_launcher(const char *what) { quit_request(what); for (;;) usleep(1000000); }
static volatile int g_dump_done, g_dump_wanted;   // Java stack dump after a native crash: the Java thread ps5-crashwatch (McBoot) polls Hello.crashFlag(), prints the stacks, then calls Hello.crashAck()
static volatile int g_stall_dump;   // ps5lwjgl.c asks for the Java stacks while the game is frozen (no frame presented for seconds): the crashwatch thread prints them and goes on watching
extern "C" void ps5_request_java_dump(void) { g_stall_dump = 1; }
extern "C" int ps5_java_dump_pending(void) { return g_stall_dump; }
static jint JNICALL native_crashflag(JNIEnv *, jclass) { return g_dump_wanted ? g_dump_wanted : (g_stall_dump ? 2 : 0); }
static void JNICALL native_crashack(JNIEnv *, jclass) { if (g_stall_dump && !g_dump_wanted) g_stall_dump = 0; else g_dump_done = 1; }
static void *quit_monitor(void *) {
    out("[jvm] quit monitor running\n");
    while (!g_quit_req) usleep(50000);
    out("[jvm] quit request: %s\n", g_quit_what);
    if (!strncmp(g_quit_what, "crash 1", 7)) {   // SIGSEGV (11) / SIGBUS (10): the Java stacks first, bounded to 8 s (a Java thread parked in the handler while in Java state blocks the safepoint the dump needs)
        usleep(1500000);   // let the crash handler finish its own CRASH lines
        g_dump_wanted = 1; __sync_synchronize();
        for (int i = 0; i < 80 && !g_dump_done; i++) usleep(100000);
        out("[jvm] java stack dump %s\n", g_dump_done ? "done" : "timed out (no ps5-crashwatch answer: old McBoot/Hello, or the faulting thread was in Java state)");
    }
    return_to_launcher(g_quit_what);
    for (;;) usleep(1000000);
    return 0;
}
static jint JNICALL native_exitstep(JNIEnv *env, jclass, jint n, jstring s) {   // Hello.exitStep(0, why) prepares, 1..5 run one step each (Java logs around every call: that log line survives where a native one may not)
#ifdef MC_CP
    if (n < 0) { const char *c = s ? env->GetStringUTFChars(s, 0) : 0; if (c) { exitlog("[java] %s", c); env->ReleaseStringUTFChars(s, c); } return 0; }
    if (n == 0) { const char *c = s ? env->GetStringUTFChars(s, 0) : 0; char w[64]; snprintf(w, sizeof w, "%s", c ? c : "exit"); if (c) env->ReleaseStringUTFChars(s, c); static volatile int once; if (once++) return 0x2000; return rtl_prepare(w); }
    return rtl_step(n);
#else
    return -1;
#endif
}
extern "C" void ps5_vk_handover_probe(void) __attribute__((weak));   // ps5vk.inc (Zink builds)
extern "C" void ps5_vk_defer(int) __attribute__((weak));   // ps5lwjgl.c: the hand-over happens when the game asks for its window
extern "C" void ps5_vk_defer_done(void) { g_renderer_zink = 0; ps5_set_renderer(0); g_rsel_name = "vulkan"; }
extern "C" int ps5_vk_switch(void) __attribute__((weak));   // ps5vk.inc (Zink builds): 1 when the display could be handed to a plain Vulkan swapchain
static void JNICALL native_quit(JNIEnv *env, jclass, jstring s) {
    const char *c = s ? env->GetStringUTFChars(s, 0) : 0;
    char w[64]; snprintf(w, sizeof w, "%s", c ? c : "exit");
    if (c) env->ReleaseStringUTFChars(s, c);
    out("[jvm] Hello.quit(%s) called from Java\n", w);
    if (!strcmp(w, "vkdefer")) {   // McBoot: like vkswitch, but the display changes hands only when the game asks for its window: the launcher's loading screen stays up through the long start of the game
        if (ps5_vk_defer && g_renderer_zink) ps5_vk_defer(1);
        jclass sc = env->FindClass("java/lang/System"); jmethodID sm = sc ? env->GetStaticMethodID(sc, "setProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") : 0;
        if (sm) env->CallStaticObjectMethod(sc, sm, env->NewStringUTF("ps5.vkswitch"), env->NewStringUTF(ps5_vk_defer && g_renderer_zink ? "1" : "0"));
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    if (!strcmp(w, "vkswitch")) {   // McBoot: the launcher ran on Zink, the game wants Vulkan: hand the display over in this process and tell Java how it went (system property ps5.vkswitch = 1 / 0); no quit
        int ok = ps5_vk_switch ? ps5_vk_switch() : 0;
        if (ok) { g_renderer_zink = 0; ps5_set_renderer(0); g_rsel_name = "vulkan"; }
        out("[jvm] vkswitch: %s\n", ok ? "the display is Vulkan's now" : "failed");
        jclass sc = env->FindClass("java/lang/System"); jmethodID sm = sc ? env->GetStaticMethodID(sc, "setProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") : 0;
        if (sm) env->CallStaticObjectMethod(sc, sm, env->NewStringUTF("ps5.vkswitch"), env->NewStringUTF(ok ? "1" : "0"));
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    {   // opt-in experiment (empty file <root>/.kit/vkprobe): when the game ends the app to start on Vulkan, first try to open Vulkan from this Zink process (log lines "vkh:")
        const char *pk = LCFG_ROOT "/.kit/vkprobe";
        if (ps5_vk_handover_probe && !strncmp(w, "Open the app again", 18) && access(pk, F_OK) == 0) { unlink(pk); out("[jvm] vkprobe: running the Vulkan handover probe\n"); ps5_vk_handover_probe(); }
    }
    g_deliberate = 1;
    quit_request(w);
    return_to_launcher(w);   // this is a healthy Java thread (the shutdown hook): do the work right here, the monitor thread is only the backup
    for (;;) usleep(1000000);
}
// statvfs()/fstatvfs() of the console's libc jump through a garbage pointer (log 1fc9ba5f: Render thread, Legacy4J PlayGameScreen -> File.getTotalSpace -> UnixFileSystem.getSpace0 -> SIGSEGV rip=0x10002746a6, deterministic;
// the JDK only stubs statfs). These definitions win over the libc stub at link time (same mechanism as ps5lwjgl.c's dlsym) and report a fixed 64 GiB volume with 32 GiB free:
// the games only use the numbers for "low disk space" checks and a storage bar.
static void fake_statvfs(struct statvfs *s) {
    static int logged; if (!logged++) out("[jvm] statvfs/fstatvfs: answered with fixed 64 GiB / 32 GiB free (the console libc's statvfs crashes)\n");
    memset(s, 0, sizeof *s);
    s->f_bsize = 4096; s->f_frsize = 4096;
    s->f_blocks = (64ull << 30) / 4096; s->f_bfree = s->f_bavail = (32ull << 30) / 4096;
    s->f_files = 1u << 20; s->f_ffree = s->f_favail = 1u << 19; s->f_namemax = 255;
}
extern "C" int statvfs(const char *path, struct statvfs *s) { if (!path || !s) { errno = EFAULT; return -1; } fake_statvfs(s); return 0; }
extern "C" int fstatvfs(int fd, struct statvfs *s) { if (fd < 0 || !s) { errno = fd < 0 ? EBADF : EFAULT; return -1; } fake_statvfs(s); return 0; }
static unsigned long long g_stack_lo, g_stack_hi;   // stack of the VM thread, set in vm_main
static volatile int g_in_crash;
extern "C" volatile int ps5_sample_compact;   // set by ps5lwjgl.c's stall code while it sends SIGUSR1 to every thread: one short line per thread
volatile int ps5_sample_compact;
extern "C" volatile int ps5_sample_ack;
volatile int ps5_sample_ack;
// Condition variables a stalled thread was seen waiting on (umtx CV_WAIT, filled by the all-thread sample). Log c37cc77a / cf1c10b6: the VM thread sat in wait_for_operation while another thread's VM operation waited on the SAME condition,
// for minutes, with no timeout firing: a lost wake-up. HotSpot re-checks its predicate after every wake-up, so a spurious broadcast is harmless; ps5_cv_nudge issues one on each. libthr's struct pthread_cond starts with the u32
// __has_user_waiters and then the kernel's ucond, so the umtx address is the handle + 4; a zero flag with a thread waiting is the lost-wake-up state itself (pthread_cond_broadcast returns early on it), so it is set to 1 first.
static volatile unsigned long long g_cvw[24]; static volatile int g_cvw_n;
static volatile unsigned long long g_ulw[8]; static volatile int g_ulw_n;   // umtx lock words a thread sleeps on while the word says 'unlocked' (owner 0): a lost wake-up; ps5_cv_nudge wakes them with UMTX_OP_WAKE
extern "C" int ps5_cv_nudge(void) {
    int n = g_cvw_n, done = 0;
    for (int i = 0; i < n && i < 24; i++) {
        unsigned long long cv = g_cvw[i], base = cv - 4; volatile unsigned *flag = (volatile unsigned *)base; unsigned fl = *flag;
        out("nudge: cond %llx has_user_waiters=%u%s\n", cv, fl, fl ? "" : " (a thread waits on it: lost-wakeup state)");
        if (sizeof(pthread_cond_t) != 8) continue;
        if (!fl) *flag = 1;
        pthread_cond_t c; memcpy((void *)&c, &base, 8); int rc = pthread_cond_broadcast(&c); done++;
        if (rc) out("nudge: pthread_cond_broadcast rc=%d\n", rc);
    }
    for (int i = 0; i < g_ulw_n && i < 8; i++) out("nudge: umtx lock word %llx has owner 0 but a thread sleeps on it (lost wake-up; no wake syscall is reachable from here)\n", g_ulw[i]);
    return done;
}
static void crash(int sig, siginfo_t *si, void *ucv) {
    if (sig != SIGUSR1 && sig != SIGSYS) { char w0[24] = "crash "; w0[6] = '0' + (sig / 10) % 10; w0[7] = '0' + sig % 10; w0[8] = 0; quit_request(w0); }   // raise the flag first: nothing below is guaranteed to finish
    if (sig == SIGUSR1 && ps5_sample_compact) {   // all-thread sample (stall detector): where is this thread right now
        const unsigned long long *w = (const unsigned long long *)ucv;
        unsigned long long rip = w[28], rsp = w[31];
        char b[600]; int n = snprintf(b, sizeof b, "SAMPLE #%d thr=%p main=%p rip=%llx rax=%llx rdi=%llx rsi=%llx rdx=%llx stk:", ps5_sample_compact, (void *)pthread_self(), (void *)&main, rip, w[15], w[9], w[10], w[11]);
        if (rsp > 0x10000) {
            char r = 0; unsigned long long pg = rsp & ~4095ull, lim = (pg + 4096 - rsp) / 8;
            if (mincore((void *)(pg + 4096), 4096, &r) == 0) lim += 512;
            const unsigned long long *st = (const unsigned long long *)rsp; int found = 0;
            for (unsigned long long i = 0; i < lim && found < 16 && n < (int)sizeof b - 100; i++)
                if (st[i] >= 0x400000ull && st[i] < 0x10000000ull) { n += snprintf(b + n, sizeof b - n, " %llx", st[i]); found++; }
        }
        {   // who owns the umtx mutex this thread waits on (rsi 0x11 MUTEX_WAIT, 5 MUTEX_LOCK, 0 LOCK): rdi = struct umutex, first word = owner tid (bit 31 = contested). Same tid is printed for every thread, so the owner can be looked up.
            extern int pthread_getthreadid_np(void) __attribute__((weak));
            unsigned long long op = w[10], um = w[9]; char r2 = 0;
            n += snprintf(b + n, sizeof b - n, " tid=%x", pthread_getthreadid_np ? (unsigned)pthread_getthreadid_np() : 0u);
            if (op == 8 && (um & 3) == 0 && um >= 0x880000000ull && um < 0x8c0000000ull) {   // CV_WAIT: remember the condition so the stall watchdog can wake it (ps5_cv_nudge)
                int k = 0, have = g_cvw_n; for (; k < have && k < 24; k++) if (g_cvw[k] == um) break;
                if (k == have && have < 24) { g_cvw[have] = um; g_cvw_n = have + 1; } }
            if ((op == 0x11 || op == 5 || op == 0) && (um & 3) == 0 && ((um >= 0x880000000ull && um < 0x8c0000000ull) || (um >= 0x400000000ull && um < 0x440000000ull)) && n < (int)sizeof b - 24) { unsigned ow = *(const volatile unsigned *)um; n += snprintf(b + n, sizeof b - n, " umtx_owner=%x", ow);
                if (ow == 0 && (op == 0 || op == 5) && g_ulw_n < 8) { int k = 0, have = g_ulw_n; for (; k < have; k++) if (g_ulw[k] == um) break; if (k == have) { g_ulw[have] = um; g_ulw_n = have + 1; } } }
        }
        out("%s\n", b); ps5_sample_ack = ps5_sample_compact; return;
    }
    if (g_in_crash++ > 2) for (;;) usleep(1000000);   // fault inside the handler: stop recursing
    // The console's saved-context layout (calibrated from a kill() self-test): rip=w28 cs=w29 rflags=w30 rsp=w31
    const unsigned long long *w = (const unsigned long long *)ucv;
    unsigned long long rip = w[28], rsp = w[31];
    if ((sig == SIGSYS || sig == SIGILL) && (g_in_rtl || (rip & 0xfff) == 0x3ac || sig == SIGILL)) {   // the known exit-time refusals: one short line instead of an 18-line dump (bursts make the UDP listener drop the lines after them)
        out("CRASH short sig=%d rip=%llx rsp=%llx in_rtl=%d\n", sig, rip, rsp, g_in_rtl); usleep(100000);
        if (!g_in_rtl && !g_main_done) trace_replay();   // the VM died before Hello.main returned: show its last beacons again
        if (sig == SIGSYS) {
            ucontext_t *uc = (ucontext_t *)ucv;
            if (g_in_rtl) { uc->uc_mcontext.mc_rax = 78; uc->uc_mcontext.mc_rflags |= 1; g_in_crash = 0; return; }
            quit_request("exit syscall"); g_in_crash = 0; for (;;) usleep(1000000);
        }
        quit_request("crash 04"); g_in_crash = 0; for (;;) usleep(1000000);
    }
    // Every line goes out paced (3 ms): the 18-line burst used to overrun the UDP listener and drop the header (sig/code/addr/main) and w00..w20 lines (log 42ec5a13).
#define COUT(...) do { out(__VA_ARGS__); usleep(3000); } while (0)
    if (sig == SIGUSR1) COUT("CRASH note: SIGUSR1 is a stack sample (host watchdog / start-up self-test), not a crash: the game keeps running\n");
    COUT("CRASH sig=%d code=%d addr=%p\nCRASH main=%p rip=%llx rsp=%llx rflags=%llx\n", sig, si ? si->si_code : -1,
        si ? si->si_addr : 0, (void *)&main, rip, rsp, w[30]);
    COUT("CRASH thread=%p stack=%llx..%llx (the vm thread's stack; a crash on another thread still scans the mapped part of its own stack)\n", (void *)pthread_self(), g_stack_lo, g_stack_hi);
    for (int i = 0; i < 56; i += 4) COUT("CRASH w%02d: %llx %llx %llx %llx\n", i, w[i], w[i + 1], w[i + 2], w[i + 3]);
    if (sig == SIGBUS && w[9] > 0x10000000ull && w[9] < 0x4000000000ull && (w[9] & 7) == 0) {   // a mutex lock that faults: show the pthread_mutex_t and what it points to
        unsigned long long *m = (unsigned long long *)w[9]; unsigned long long m0 = m[0];
        COUT("CRASH mutex@%llx: %llx %llx %llx %llx\n", w[9], m[0], m[1], m[2], m[3]);
        if (m0 > 0x10000000ull && m0 < 0x4000000000ull && (m0 & 7) == 0) { unsigned long long *q = (unsigned long long *)m0; COUT("CRASH mutex->%llx: %llx %llx %llx %llx %llx %llx\n", m0, q[0], q[1], q[2], q[3], q[4], q[5]); }
    }
    if (rip >= 0x100000000ull && rip < 0x400000000ull) {      // JIT/stub code lives in readable memory; the executable image is execute-only
        const unsigned char *c = (const unsigned char *)(rip - 24);
        char line[200]; int n = 0;
        for (int i = 0; i < 64 && n < 190; i++) n += snprintf(line + n, sizeof line - n, "%02x", c[i]);
        COUT("CRASH code@rip-24: %s\n", line);
    }
    if (sig == SIGSEGV || sig == SIGBUS) {   // what is around the faulting pc/address: unmapped (ENOMEM) vs mapped-but-not-resident, in 64 KB steps +-512 MB
        char v = 0; int ok_sp = mincore((void *)(rsp & ~4095ull), 4096, &v) == 0;
        if (!ok_sp) COUT("CRASH map: mincore refused (no map scan)\n");
        else {
            unsigned long long fa = si ? (unsigned long long)si->si_addr : rip;
            unsigned long long lo = (fa > (512ull << 20) ? fa - (512ull << 20) : 0) & ~0xffffull, hi = fa + (512ull << 20);
            unsigned long long run0 = 0; int inrun = 0, runs = 0, res0 = 0;
            for (unsigned long long p = lo; p <= hi + 0x10000ull && runs < 24; p += 0x10000ull) {
                char r = 0; int m = p <= hi && mincore((void *)p, 4096, &r) == 0;
                if (m && !inrun) { inrun = 1; run0 = p; res0 = r & 1; }
                if (!m && inrun) { inrun = 0; runs++; COUT("CRASH map: %llx-%llx mapped (first page %s)%s\n", run0, p, res0 ? "resident" : "not resident", (fa >= run0 && fa < p) ? "  <== contains the fault address" : ""); }
            }
            { char r = 0; int m = mincore((void *)(fa & ~4095ull), 4096, &r) == 0; COUT("CRASH map: fault page %llx is %s\n", fa & ~4095ull, m ? (r & 1 ? "mapped+resident" : "mapped, NOT resident") : "UNMAPPED"); }
        }
    }
    unsigned long long lim = 0;
    if (g_stack_hi && rsp >= g_stack_lo && rsp < g_stack_hi) lim = (g_stack_hi - rsp) / 8;       // return addresses on the stack, never past its top
    else if (rsp > 0x10000) {                                                                      // another thread: only what is provably mapped (this page and the next one)
        char r = 0; unsigned long long pg = rsp & ~4095ull; lim = (pg + 4096 - rsp) / 8;
        if (mincore((void *)(pg + 4096), 4096, &r) == 0) lim += 512;
    }
    if (lim) {
        if (lim > 2400) lim = 2400;   // 19 KB: a fault deep inside a libc call has its first exe frame several KB up (the old 400-word cap hid it)
        const unsigned long long *st = (const unsigned long long *)rsp;
        int found = 0;
        for (int i = 0; i < 6 && i < (int)lim; i++) COUT("CRASH raw[%d]=%llx\n", i, st[i]);   // the word at [rsp] is the return address when the fault came from a call through a bad pointer
        for (unsigned long long i = 0; i < lim && found < 40; i++)
            if (st[i] >= 0x400000ull && st[i] < 0x10000000ull) {
                COUT("CRASH stk[%llu]=%llx\n", i, st[i]);
                found++;
            }
        found = 0;
        for (unsigned long long i = 0; i < lim && found < 12; i++)     // compiled-Java return addresses (same 256 MB window as the faulting pc)
            if ((st[i] >> 28) == (rip >> 28) && rip >= 0x10000000000ull / 16) { COUT("CRASH jstk[%llu]=%llx\n", i, st[i]); found++; }
    }
    if (sig == SIGSYS || sig == SIGILL) {   // which syscall did the kernel refuse, and who called it (return addresses of any thread, bounded scan)
        ucontext_t *uc2 = (ucontext_t *)ucv;
        out("CRASH syscall rax=%ld rdi=%lx rsi=%lx rdx=%lx thread=%p\n", (long)uc2->uc_mcontext.mc_rax, (unsigned long)uc2->uc_mcontext.mc_rdi, (unsigned long)uc2->uc_mcontext.mc_rsi, (unsigned long)uc2->uc_mcontext.mc_rdx, (void *)pthread_self());
    }
    COUT("CRASH end\n");
    g_in_crash = 0;
    if (sig == SIGUSR1) return;   // self-test only
    if (sig == SIGSYS) {          // the kernel rejected a syscall/sysctl: report it, make the call fail, keep running
        static int seen;
        ucontext_t *uc = (ucontext_t *)ucv;
        long sysno = (long)uc->uc_mcontext.mc_rax;
        if (g_in_rtl) { out("[jvm] a call of the launcher return was refused by the kernel (SIGSYS): trying the next step\n"); uc->uc_mcontext.mc_rax = 78; uc->uc_mcontext.mc_rflags |= 1; return; }
        if (sysno == 1 || sysno == 431 || (rip >= 0x800000000ull && rip < 0x800010000ull && (rip & 0xfff) == 0x3ac)) {   // exit() / thr_exit() (the saved rax reads 0 on this console, so the stub address 0x8000003ac, seen at every quit, counts too): the console refuses them
            quit_request("exit syscall");       // the game is ending; the monitor thread brings the launcher back. This thread parks (returning ENOSYS made libthr touch its freed stack)
            for (;;) usleep(1000000);
        }
        uc->uc_mcontext.mc_rax = 78;            // ENOSYS
        uc->uc_mcontext.mc_rflags |= 1;         // carry flag = syscall error (FreeBSD convention)
        if (++seen < 40) return;
    }
    for (;;) usleep(1000000);   // the monitor thread has the flag
}
static void install_crash_handler() {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT, SIGUSR1, SIGSYS };
    for (int g : sigs) sigaction(g, &sa, 0);
}

static jint JNICALL vm_vfprintf(FILE *, const char *fmt, va_list ap) {
    char b[1024];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > 0) out("[jvm] %s", b);
    return n;
}
static void JNICALL vm_exit(jint code) { char w[64]; snprintf(w, sizeof w, "exit %d", (int)code); out("[jvm] exit(%d)\n", (int)code); if (!g_main_done) trace_replay(); rescue_launcher(w); }
static void JNICALL vm_abort(void) { out("[jvm] abort()\n"); if (!g_main_done) trace_replay(); rescue_launcher("abort"); }

extern "C" __attribute__((weak)) int ps5_glcompat_level;   // ps5lwjgl.c: GL version gl4es reports (21), 0 = fixed-function mode off
extern "C" __attribute__((weak)) void ps5_gl_release(void);   // ps5lwjgl.c: let go of the EGL context on the calling thread
static void JNICALL native_glcompat(JNIEnv *, jclass, jint level) { if (level < 0) { if (ps5_gl_release) ps5_gl_release(); return; }   // Hello.glCompat(-1): the game thread hands the GL context back
    if (&ps5_glcompat_level) ps5_glcompat_level = (int)level; out("[jvm] fixed-function GL level %d%s\n", (int)level, &ps5_glcompat_level ? "" : " (not in this build)"); }
extern "C" void ps5_set_vcwd(const char *);
static jint JNICALL native_chdir(JNIEnv *env, jclass, jstring s) {
    const char *c = env->GetStringUTFChars(s, 0); int r = chdir(c), e = errno;
    if (r) ps5_set_vcwd(c);   // the platform refuses chdir: the shim redoes relative paths against this folder
    out("[jvm] chdir(%s) = %d%s\n", c, r, r ? " (failed)" : "");
    env->ReleaseStringUTFChars(s, c); return r ? e : 0;
}
static void JNICALL native_log(JNIEnv *env, jclass, jstring s) {
    const char *c = env->GetStringUTFChars(s, 0);
    out("[java] %s\n", c);
    env->ReleaseStringUTFChars(s, c);
}


// Watchdog: if Hello has not finished after 90 s the VM thread is stuck (e.g. in a native loop). Signal it so the crash reporter
// prints where it is (registers + return addresses on its stack), twice, then leave it alone.
#ifdef USE_GL
extern "C" volatile int ps5_gl_lookups;
extern "C" void ps5_gl_selftest(void);
#endif

#ifdef GL_BARE_TRACE
// Trace build: --wrap shims defined here (no LWJGL archive). Logs over UDP (port 9999) like every other build.
static int g_tn;
static void tlog(const char *fmt, ...) { if (g_tn++ > 600) return; char b[300]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v); out("[trace] %s\n", b); }   // UDP to the PowerShell listener
extern "C" {
int __real_sceVideoOutOpen(int, int, int, const void *);
int __real_sceVideoOutSetFlipRate(int, int);
int __real_sceVideoOutRegisterBuffers2(int, int, int, void *, int, void *, int, void *);
int __real_sceVideoOutSubmitFlip(int, int, unsigned, long);
int __real_sceVideoOutIsFlipPending(int);
int __real_sceVideoOutWaitVblank(int);
int __real_sceVideoOutGetFlipStatus(int, void *);
int __real_sceAgcDriverSubmitDcb(void *);
unsigned *__real_sceAgcDcbSetFlip(void *, unsigned, int, unsigned, long);
int __real_ps5_agc_gate2_prepare_present(void *, size_t);
int __real_ps5_agc_gate2_present(unsigned, unsigned);
int __real_ps5_agc_gate2_batch_begin(void);
int __real_ps5_agc_gate2_batch_end(void);
int __real_ps5_agc_gate2_batch_present(unsigned);
// SDK runtime entry points called from the Mesa side (6cb291a: present takes 1 arg; 2 are forwarded so a 2-arg build also works)
int __wrap_ps5_agc_gate2_prepare_present(void *fb, size_t sz) {
#ifdef GL_BARE_FIX
    // EXPERIMENT: the runtime registered the scanout with 0x1400000 bytes but the swap path asks with the 64 MiB pool size, so its
    // "already registered" shortcut (registered >= asked) fails and it tries to reopen the port. Ask with the registered size instead.
    size_t use = sz > 0x1400000u ? 0x1400000u : sz;
#else
    size_t use = sz;
#endif
    int r = __real_ps5_agc_gate2_prepare_present(fb, use); tlog("RT prepare_present(fb=%p size=%zu asked=%zu) = %d", fb, sz, use, r); return r;
}
int __wrap_ps5_agc_gate2_present(unsigned a, unsigned b) { int r = __real_ps5_agc_gate2_present(a, b); tlog("RT present(%u) = %d", a, r); return r; }
int __wrap_ps5_agc_gate2_batch_begin(void) { int r = __real_ps5_agc_gate2_batch_begin(); tlog("RT batch_begin = %d", r); return r; }
int __wrap_ps5_agc_gate2_batch_end(void) { int r = __real_ps5_agc_gate2_batch_end(); tlog("RT batch_end = %d", r); return r; }
int __wrap_ps5_agc_gate2_batch_present(unsigned a) { int r = __real_ps5_agc_gate2_batch_present(a); tlog("RT batch_present(%u) = %d", a, r); return r; }
int __wrap_sceVideoOutOpen(int u, int t, int i, const void *p) { int r = __real_sceVideoOutOpen(u, t, i, p); tlog("vo Open(user=%d type=%d idx=%d) = %x", u, t, i, r); return r; }
int __wrap_sceVideoOutSetFlipRate(int h, int rate) { int r = __real_sceVideoOutSetFlipRate(h, rate); tlog("vo SetFlipRate(%x,%d) = %x", h, rate, r); return r; }
int __wrap_sceVideoOutRegisterBuffers2(int h, int a, int b, void *bufs, int n, void *attr, int c, void *d) { int r = __real_sceVideoOutRegisterBuffers2(h, a, b, bufs, n, attr, c, d); tlog("vo RegisterBuffers2(%x set=%d start=%d n=%d) = %x", h, a, b, n, r); return r; }
int __wrap_sceVideoOutSubmitFlip(int h, int idx, unsigned mode, long arg) { int r = __real_sceVideoOutSubmitFlip(h, idx, mode, arg); tlog("vo SubmitFlip(%x buf=%d mode=%u arg=%lx) = %x", h, idx, mode, arg, r); return r; }
int __wrap_sceVideoOutIsFlipPending(int h) { int r = __real_sceVideoOutIsFlipPending(h); tlog("vo IsFlipPending(%x) = %d", h, r); return r; }
int __wrap_sceVideoOutWaitVblank(int h) { int r = __real_sceVideoOutWaitVblank(h); tlog("vo WaitVblank(%x) = %x", h, r); return r; }
int __wrap_sceVideoOutGetFlipStatus(int h, void *st) {
    int r = __real_sceVideoOutGetFlipStatus(h, st); const unsigned long long *q = (const unsigned long long *)st;
    tlog("vo GetFlipStatus(%x) = %x status[0..5]=%llx %llx %llx %llx %llx %llx", h, r, q[0], q[1], q[2], q[3], q[4], q[5]); return r;
}
int __wrap_sceAgcDriverSubmitDcb(void *p) { int r = __real_sceAgcDriverSubmitDcb(p); tlog("agc SubmitDcb = %x", r); return r; }
unsigned *__wrap_sceAgcDcbSetFlip(void *cb, unsigned h, int idx, unsigned mode, long marker) {
    unsigned *r = __real_sceAgcDcbSetFlip(cb, h, idx, mode, marker); tlog("agc DcbSetFlip(handle=%x buf=%d mode=%u marker=%lx) = %p", h, idx, mode, marker, (void *)r); return r;
}
}
#endif

#ifdef GL_BARE
// bare bisect: SDK-sample-style EGL clear loop straight in the host. No LWJGL archive, no shim, no libc replacements, no --wrap.
extern "C" volatile int ps5_gl_lookups = 0;
extern "C" void ps5_gl_selftest(void) {
#ifdef GL_BARE_46
    const EGLint ma = 4, mi = 6;
#else
    const EGLint ma = 3, mi = 3;
#endif
    const EGLint cfg[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    const EGLint cx[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, ma, EGL_CONTEXT_MINOR_VERSION_KHR, mi, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE };
    EGLConfig c = 0; EGLint n = 0;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    out("bare: GL %d.%d core, display=%p\n", (int)ma, (int)mi, (void *)d);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, 0, 0) || !eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(d, cfg, &c, 1, &n) || n != 1) { out("bare: init/config FAILED err=%x\n", eglGetError()); return; }
    EGLSurface s = eglCreateWindowSurface(d, c, (EGLNativeWindowType)0, 0);
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, cx);
    out("bare: surface=%p context=%p\n", (void *)s, (void *)x);
    if (s == EGL_NO_SURFACE || x == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, x)) { out("bare: makeCurrent FAILED err=%x\n", eglGetError()); return; }
    typedef void (*CC)(float, float, float, float); typedef void (*CL)(unsigned);
    CC cc = (CC)eglGetProcAddress("glClearColor"); CL cl = (CL)eglGetProcAddress("glClear");
    if (!cc || !cl) { out("bare: glClear missing\n"); return; }
#ifdef GL_BARE_MIN
    for (int i = 0;; i++) {   // endless: success = visibly cycling colors on the TV
        float t = (i % 120) / 120.0f;
#else
#ifdef GL_BARE_FIX
    for (int i = 0; i < 1200; i++) {   // ~20 s of color cycling
        float t = (i % 180) / 180.0f;
#else
    for (int i = 0; i < 180; i++) {
        float t = i / 180.0f;
#endif
#endif
        cc(t, 0.2f, 1.0f - t, 1.0f); cl(0x4000u | 0x100u | 0x400u);   // color|depth|stencil
        EGLBoolean ok = eglSwapBuffers(d, s);
#ifdef GL_BARE_TRACE
        { int e = ok ? 0 : (int)eglGetError(); if (i < 3 || !ok) tlog("bare: swap #%d ok=%d err=%x", i, (int)ok, e);
#ifndef GL_BARE_FIX
          return;
#endif
          if (!ok) return; continue; }
#endif
        if (i < 3 || !ok) out("bare: swap #%d ok=%d err=%x\n", i, (int)ok, ok ? 0 : (int)eglGetError());
        if (!ok) return;
    }
    out("bare: 180 frames presented\n");
}
#endif
static pthread_t g_vm_thr; static volatile int g_vm_thr_set;
static void *watchdog(void *) {
    for (int i = 0; i < 90 && !g_main_done; i++) sleep(1);
    for (int k = 0; k < 2 && !g_main_done; k++) {
        if (g_vm_thr_set) {
#ifdef USE_GL
            out("WATCHDOG: VM thread still busy (%d), gl lookups so far=%d\n", k + 1, ps5_gl_lookups);
#else
            out("WATCHDOG: VM thread still busy (%d)\n", k + 1);
#endif
            int rc = pthread_kill(g_vm_thr, SIGUSR1); out("WATCHDOG: pthread_kill rc=%d\n", rc);
        }
        for (int i = 0; i < 3 && !g_main_done; i++) sleep(1);
    }
    return 0;
}

static void *vm_main(void *) {
    g_vm_thr = pthread_self(); g_vm_thr_set = 1;
    {   pthread_attr_t pa; void *sa = 0; size_t ssz = 0;
        if (!pthread_attr_init(&pa) && !pthread_attr_get_np(pthread_self(), &pa) && !pthread_attr_getstack(&pa, &sa, &ssz)) {
            g_stack_lo = (unsigned long long)sa; g_stack_hi = g_stack_lo + ssz;
        }
    }
    static char altstack[256 * 1024];
    stack_t ss; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0;
    out("sigaltstack rc=%d\n", sigaltstack(&ss, 0));
    // self-tests: stdout capture, stderr capture, signal delivery with ucontext + backtrace
    write(1, "selftest stdout\n", 16); write(2, "selftest stderr\n", 16);
    printf("selftest printf\n"); fflush(stdout);
    kill(getpid(), SIGUSR1);
    out("selftest signal returned\n");
    ls("/app0"); ls("/app0/assets"); ls("/app0/assets/jdk"); ls("/app0/assets/jdk/lib");
#ifndef GL_SELFTEST_ONLY
    probe_file(JAVA_HOME "/lib/modules"); probe_file("/app0/assets/hello/Hello.class");
#endif
#ifdef MC_CP
    probe_dir("/app0/assets"); probe_dir("/download0"); probe_sock(); netprobe_start();
    probe_rm_all("pre-jvm");
#endif
    jdk_set_trace(trace_cb);
#if defined(USE_GL) && defined(GL_SELFTEST)
    out("GL_SELFTEST: native window and 180 frames, no JVM\n");
    ps5_gl_selftest();
    out("GL_SELFTEST done\n");
    g_main_done = 1;
    for (;;) sleep(1);
#endif
#if defined(MC_VK) && defined(ZPROBE)   // AIO probe: Mesa Zink (OpenGL on RADV) in the Vulkan title, native, no JVM
    out("ZPROBE: Zink over RADV, no JVM\n");
    ps5_zink_probe();
    out("ZPROBE done\n");
    g_main_done = 1;
    for (;;) sleep(1);
#endif
#if defined(MC_VK) && defined(GLPROBE)   // AIO probe: the OpenGL SDK linked next to RADV, native clear loop on the OpenGL path, no JVM
    out("GLPROBE: OpenGL clear loop in the Vulkan title, no JVM\n");
    ps5_gl_probe();
    out("GLPROBE done\n");
    g_main_done = 1;
    for (;;) sleep(1);
#endif
    out("BEGIN jvm_boot\n");
    LCfg lc; lcfg_load(&lc);
    g_jneed = lc.java;
#ifdef AIO_RENDERER   // one title, both renderers: launcher.cfg renderer=gl|vulkan (default vulkan), applied at start-up (a change needs a restart)
#ifndef DEFAULT_RENDERER
#define DEFAULT_RENDERER "vulkan"
#endif
    const char *rsel = lc.renderer[0] ? lc.renderer : DEFAULT_RENDERER;
#ifndef MC_VK   // merged launcher title: the launcher UI only draws on OpenGL (SDK or Zink), so "vulkan" is a one-game setting. The UI writes <root>/.kit/vk_once when Play is pressed with Vulkan selected and ends the app;
                // this start sees the marker, deletes it and plays that game on Vulkan. Without the marker (every other start, also after the game) a "vulkan" setting means the launcher on Zink (or the SDK without Zink).
    {
        const char *mk = LCFG_ROOT "/.kit/vk_once"; bool once = access(mk, F_OK) == 0;
        if (once) { unlink(mk); rsel = "vulkan"; out("renderer: one-shot Vulkan start (.kit/vk_once)\n"); }
        else if (!strcmp(rsel, "vulkan"))
#ifdef PS5_ZINK
            rsel = "zink";
#else
            rsel = "gl";
#endif
    }
#endif
    g_renderer_gl = !strcmp(rsel, "gl");
#ifdef PS5_ZINK
    g_renderer_zink = !strcmp(rsel, "zink");
#endif
    if (g_renderer_gl) lc.res = 0;   // the OpenGL SDK scans out 1080p only: asking Minecraft for a bigger window there put its mouse mapping and buttons off the picture
    if (lc.res == 1440 || lc.res == 2160) { char rb[8]; snprintf(rb, sizeof rb, "%d", lc.res); setenv("PS5_RES_H", rb, 1); } else unsetenv("PS5_RES_H");
    { FILE *ra = fopen(LCFG_ROOT "/.res_active", "w"); if (ra) { fprintf(ra, "%d\n", (lc.res == 1440 || lc.res == 2160) ? lc.res : 1080); fclose(ra); } }   // what THIS run of the app renders at: the launcher setting may change while it runs, the surface and the window size Minecraft is told must not   // launcher.cfg resolution=1440|2160: the Vulkan and Zink display mode (ps5vk.inc); the OpenGL (SDK) renderer stays 1080p
    if (lc.zglt) setenv("PS5_ZGLTHREAD", "1", 1); else unsetenv("PS5_ZGLTHREAD");   // Mesa's GL command thread on Zink: opt-in (launcher.cfg zglthread=on), off by default after a 1.20.1 crash on a worker thread
    ps5_set_renderer(g_renderer_zink ? 2 : g_renderer_gl);
    ps5_set_hz(lc.hz >= 120 || lc.hz == 0 ? 120 : 60);   // launcher.cfg hz=60|120 (default 120: the title declares it; the driver falls back to 59.94 when the output does not run it)
    g_rsel_name = g_renderer_zink ? "zink" : g_renderer_gl ? "gl" : "vulkan";
    out("renderer: %s (launcher.cfg renderer=%s, build default %s)\n", g_renderer_zink ? "OpenGL via Zink (Mesa on RADV)" : g_renderer_gl ? "OpenGL (SDK)" : "Vulkan (RADV)", lc.renderer[0] ? lc.renderer : "(unset)", DEFAULT_RENDERER);
#endif
    g_jdkv = ps5_java_expect(lc.jdk);   // the global Java setting (launcher.cfg jdk=) beats the automatic pick, but never picks a Java older than the version needs
    static char jdks_opt[48], jdk_opt[24]; snprintf(jdks_opt, sizeof jdks_opt, "-Dps5.jdks=%s%s%s%s", jdk_have(8) ? "8," : "", jdk_have(17) ? "17," : "", jdk_have(21) ? "21," : "", jdk_have(25) ? "25," : ""); snprintf(jdk_opt, sizeof jdk_opt, "-Dps5.jdk=%d", lc.jdk && jdk_have(lc.jdk) ? lc.jdk : 0);   // McBoot compares the running Java with the setting
    static char jhome[48], bpatch[64]; snprintf(jhome, sizeof jhome, "/app0/assets/jdk%d", g_jdkv); snprintf(bpatch, sizeof bpatch, "--patch-module=java.base=/app0/assets/lib/base-patch%d", g_jdkv);
    out("jdk: Java %d (launcher.cfg java=%d, linked mask %d)\n", g_jdkv, lc.java, JDK_MASK);
    jdk_set_home(jhome); jdk_set_dl();
    JavaVMOption o[192]; memset(o, 0, sizeof o); int n = 0;   // Forge adds ~40 module options   // was 24: the mc build passes 28 options, and writing past the array corrupts the stack (JNI_EINVAL)
    o[n++].optionString = (char *)"-Djava.class.path=" CLASS_PATH;
    o[n++].optionString = jdks_opt; o[n++].optionString = jdk_opt;
    if (g_jdkv >= 17) o[n++].optionString = (char *)"--add-opens=java.base/java.nio.file.spi=ALL-UNNAMED";
    if (g_jdkv >= 17) o[n++].optionString = (char *)"--add-opens=java.base/java.lang=ALL-UNNAMED";   // McBoot.openLwjglModules: Module.implAddOpens for the LWJGL modules of a Forge VM
    if (g_jdkv >= 17) o[n++].optionString = (char *)"--add-opens=java.base/sun.nio.fs=ALL-UNNAMED";   // McBoot.softChdir: relative paths of mods when chdir is refused
    if (g_jdkv >= 17) o[n++].optionString = (char *)"--add-opens=java.base/java.io=ALL-UNNAMED";
    if (g_jdkv >= 17) o[n++].optionString = (char *)"--add-opens=java.base/java.net=ALL-UNNAMED";   // McBoot.resetUrlFactory: Quilt's URL handler factory from an earlier game in this VM   // McBoot.registerFsProviders adds the file systems of the game's jars (Quilt) to the JDK's provider list
    if (g_jdkv >= 17 && g_jdkv < 24) o[n++].optionString = (char *)"-Djava.security.manager=allow";   // McBoot installs a SecurityManager that turns System.exit into "back to the launcher" (JDK 24+ has no SecurityManager at all; Java 8 takes System.setSecurityManager without a property, and "allow" there would be read as a class name)
    // Forge / NeoForge 1.17+ start through BootstrapLauncher on the module path: --module-path, --add-modules, --add-opens and --add-exports can only be given when the VM is created. McBoot (needsOtherJvmArgs) writes them,
    // one option per line, to <root>/.kit/jvmargs.txt (first line "#id <hash>" -> -Dps5.jvmargs.id, which McBoot compares to know the VM already has them) and ends the app; this start passes them. The file stays until McBoot replaces or deletes it (the VM keeps the flavour of the last game played).
    // Boot guard: <root>/.kit/vmboot exists from here until Hello.main runs (Java deletes it). If it is still there at the next start, the VM died before any Java ran (a module path or option it could not take):
    // the one-shot files that led there are dropped (jvmargs.txt, autoplay.txt) and McBoot clears its patched jars (-Dps5.safemode=1). Nothing a previous round left behind can keep the app from opening.
    static char fg_safe[] = "-Dps5.safemode=1";
    { struct stat bst;
      if (stat(LCFG_ROOT "/.kit/vmboot", &bst) == 0) {
        unlink(LCFG_ROOT "/.kit/jvmargs.txt"); unlink(LCFG_ROOT "/.kit/autoplay.txt");
        o[n++].optionString = fg_safe;
        out("boot guard: the last VM start died before Java ran: Forge / restart state dropped, plain start\n");
      }
      FILE *bf = fopen(LCFG_ROOT "/.kit/vmboot", "w"); if (bf) { fputs("1\n", bf); fclose(bf); } }
    static char fg_pool[32768]; static char fg_id[64];
    { FILE *jf = g_jdkv >= 17 ? fopen(LCFG_ROOT "/.kit/jvmargs.txt", "r") : NULL;   // Java 8 has no module options (it would refuse to start): the file waits for the next Java 17+ start
      if (jf) {
        size_t used = 0; int cnt = 0;
        while (n < 180 && used + 4 < sizeof fg_pool) {
            size_t room = sizeof fg_pool - used; if (room > 8192) room = 8192;
            if (!fgets(fg_pool + used, (int)room, jf)) break;
            char *l = fg_pool + used; size_t len = strlen(l); while (len && (l[len-1] == '\n' || l[len-1] == '\r')) l[--len] = 0;
            if (!len) continue;
            if (!strncmp(l, "#id ", 4)) { snprintf(fg_id, sizeof fg_id, "-Dps5.jvmargs.id=%.40s", l + 4); o[n++].optionString = fg_id; continue; }
            if (l[0] == '#') continue;
            o[n++].optionString = l; used += len + 1; cnt++;
        }
        fclose(jf);   // the file STAYS: the VM keeps the flavour (module path or none) of the last game played, so only a switch between Forge and Fabric / Quilt / vanilla restarts the app (McBoot.needsOtherJvmArgs rewrites or deletes it then; the boot guard above drops it when a start dies)
        out("launcher: %d extra JVM options from .kit/jvmargs.txt (Forge / NeoForge module path, kept until another kind of game is played)\n", cnt);
      } }
#ifdef USE_JIT
#ifdef HAS_C2   // C2=1 build: HotSpot server VM, tiered C1 + C2
    int jit_n = lc.jit > 0 ? lc.jit : (JIT_THREADS > 1 ? JIT_THREADS : 3); if (jit_n < 2) jit_n = 2;   // default 3 = 1 C1 + 2 C2 threads   // tiered needs at least one C1 and one C2 thread
    static char cic_opt[32]; snprintf(cic_opt, sizeof cic_opt, "-XX:CICompilerCount=%d", jit_n);
    o[n++].optionString = cic_opt;
    o[n++].optionString = (char *)"-XX:CompilerThreadStackSize=4096";   // C2 recurses deeply
#else
    o[n++].optionString = (char *)"-XX:TieredStopAtLevel=1";   // C1 only (this VM has no C2)
#ifndef JIT_THREADS
#define JIT_THREADS 1
#endif
    static char cic_opt[32]; snprintf(cic_opt, sizeof cic_opt, "-XX:CICompilerCount=%d", lc.jit > 0 ? lc.jit : JIT_THREADS);
    o[n++].optionString = cic_opt;
#endif
#else
    o[n++].optionString = (char *)"-Xint";
#endif
    bool par_gc =
#ifdef GC_PARALLEL
        true;
#else
        false;
#endif
    if (!strcmp(lc.gc, "parallel")) par_gc = true; else if (!strcmp(lc.gc, "serial")) par_gc = false;   // instance setting overrides the build default
    // The parallel collector's work-stealing termination livelocks here (log dbebac77, 1.20.1 on Zink: all 6 GC workers sit in TaskTerminator::offer_termination, the VM thread waits for them, every Java thread is parked at
    // the safepoint = a frozen game right after a young GC; reproducible, 1.21.1 survives it by luck). Serial until <root>/.kit/pargc exists (opt-in, for testing the collector).
    if (par_gc) { char pf[200]; snprintf(pf, sizeof pf, LCFG_ROOT "/.kit/pargc"); if (access(pf, F_OK) != 0) { par_gc = false; out("gc: parallel collector is off (it hangs in work-stealing termination); using the serial collector. Create .kit/pargc to turn it back on\n"); } }
    static char pgc_opt[40]; int cpus_for_gc = lc.cpus > 0 ? lc.cpus : 4;
    snprintf(pgc_opt, sizeof pgc_opt, "-XX:ParallelGCThreads=%d", cpus_for_gc > 2 ? (cpus_for_gc > 4 ? 3 : cpus_for_gc - 1) : 1);
#ifdef AIO_RENDERER
    snprintf(pgc_opt, sizeof pgc_opt, "-XX:ParallelGCThreads=%d", 6);   // stop-the-world pauses (scavenges ~27 ms, full GCs ~540 ms) were the stutter; the console has 16 hardware threads
#endif
    if (par_gc) {
        o[n++].optionString = (char *)"-XX:+UseParallelGC";           // the 1.5-2.4 s serial full GCs while a world loads are the main stall
        o[n++].optionString = pgc_opt;
    } else {
        o[n++].optionString = (char *)"-XX:+UseSerialGC";
    }
    // Graphics settings from the instance (the build only supplies the defaults): they are environment variables the GL layer reads when the first context is made, which is after this point.
    if (lc.glthread == 1) setenv("PS5_GLTHREAD", "1", 1); else if (lc.glthread == 0) unsetenv("PS5_GLTHREAD");
    if (lc.uncap == 0) setenv("PS5_NOUNCAP", "1", 1);
    if (lc.merge == 0) setenv("PS5_NOMEGA", "1", 1);
    out("launcher: graphics merge=%s uncap=%s glthread=%s (instance setting over the build default)\n", lc.merge < 0 ? "default" : lc.merge ? "on" : "off", lc.uncap < 0 ? "default" : lc.uncap ? "on" : "off", lc.glthread < 0 ? "default" : lc.glthread ? "on" : "off");
    // Memory is one launcher-wide setting (launcher.cfg xmx=, set in the launcher's Settings): the heap is exactly that, no renderer-specific or hidden caps (log fcfd6ad8: instances asked for more, 3 GB was used).
    // -Xms commits up to 4 GB at start; a larger heap reserves the rest and grows on demand.
    int heap_mb = lc.xmx > 0 ? lc.xmx : 3072, heap_start_mb = heap_mb > 4096 ? 4096 : heap_mb;
    if (lc.xmx > 4096) out("launcher: memory %d MB: 4096 MB committed at start, the rest is reserved (the console shares its 16 GB with the GPU: if the game runs out of video memory, lower it)\n", lc.xmx);
    // Heap: the old "-Xms128m" start made the JVM grow the heap through full collections while a world loads (log: MarkSweepCompact 1.5 s + 3.1 s, 5.7 fps).
    // Commit the whole heap up front and give the young generation a third of it, so young collections are rare and full ones do not happen during loading.
    static char xms_opt[24]; static char xmn_opt[24]; static char xmx_opt[24];
#ifdef MC_CP
    snprintf(xms_opt, sizeof xms_opt, "-Xms%dm", heap_start_mb); snprintf(xmn_opt, sizeof xmn_opt, "-Xmn%dm", heap_start_mb * 9 / 20);   // 45 % young (was a third) + SurvivorRatio=4 below: log 19afbe25 (26.2 + Vitrail, serial GC, 3 GB): ~10 MB of short-lived chunk data was promoted by every young collection (survivor spaces of 102 MB overflowed),
    // the old generation filled with garbage (882 MB -> 1.7 GB in 100 collections), every young collection cost 45 ms (card scanning of the big old generation), then one 783 ms full collection and the pauses fell to 6-17 ms.
#else
    snprintf(xms_opt, sizeof xms_opt, "-Xms128m"); snprintf(xmn_opt, sizeof xmn_opt, "-Xmn32m");   // plain GL/perf test builds keep the small start heap
#endif
    snprintf(xmx_opt, sizeof xmx_opt, "-Xmx%dm", heap_mb);
    o[n++].optionString = xms_opt;
    o[n++].optionString = xmn_opt;
    o[n++].optionString = xmx_opt;
    o[n++].optionString = (char *)"-Xss1m";
    static char maxram_opt[32]; snprintf(maxram_opt, sizeof maxram_opt, "-XX:MaxRAM=%dm", heap_mb + 1536 > 4096 ? heap_mb + 1536 : 4096);
    o[n++].optionString = maxram_opt;
#ifndef CPUS_N
#define CPUS_N 4
#endif
    static char cpu_opt[40]; snprintf(cpu_opt, sizeof cpu_opt, "-XX:ActiveProcessorCount=%d", lc.cpus > 0 ? lc.cpus : CPUS_N);
    o[n++].optionString = cpu_opt;
    out("launcher: instance=%s heap=%d/%d MB cpus=%d jit=%d gc=%s (memory from launcher.cfg; the rest launcher.cfg + instance.cfg over the build defaults)\n", lc.inst, heap_start_mb, heap_mb, lc.cpus > 0 ? lc.cpus : CPUS_N, lc.jit > 0 ? lc.jit : JIT_THREADS, par_gc ? "parallel" : "serial");
    o[n++].optionString = (char *)"-XX:-UseCompressedOops";
    o[n++].optionString = (char *)"-XX:-UseCompressedClassPointers";
#ifdef MC_CP
    o[n++].optionString = (char *)"-XX:ReservedCodeCacheSize=128m";   // 256m does not fit: "Could not reserve enough space for code cache (262144K)" (the console refuses a 256 MB executable mapping, log 2865cc4a: the app would not start); 128m is the size that works
    // "Pause Full (CodeCache GC Threshold)": every time the compiled code grows by SweeperThreshold (15 %) of the cache since the last class unloading the VM runs a FULL collection to unload code: 539 ms and 735 ms
    // freezes in the middle of play (log d7c0c1ee, 26.3 + Vitrail: Fabric, Mixin and a shader engine compile a lot). The cache is big and idle code is flushed by age anyway: the threshold is switched off.
    o[n++].optionString = (char *)"-XX:SweeperThreshold=100";
    o[n++].optionString = (char *)"-XX:SurvivorRatio=4";   // survivor spaces of 1/6 of the young generation each (was 1/10): chunk data that lives a few seconds stays young instead of being promoted and later cleaned by a full collection
    o[n++].optionString = (char *)"-XX:MaxMetaspaceSize=1024m";   // MC loads ~10-20k classes per round and the old rounds' loaders stay alive (threads of the old game): 75 MB/round on 26.x, 256m froze the 4th launch in endless full GCs
    o[n++].optionString = (char *)"-XX:MetaspaceSize=192m";       // start the metaspace high-water mark high: every step up used to trigger a full GC while the ~10k classes loaded (8 full GCs, 650 ms, before the menu)
    o[n++].optionString = (char *)"-XX:+DisableExplicitGC";        // ignore System.gc(): a full collection of a 900 MB heap took 1.5-2.8 s during world loading
    // The console has no locale: JDK 17 / 21 took US-ASCII for file.encoding and sun.jnu.encoding (JDK 8 and 25 already say UTF-8), so a resource pack called "NewGlowingOres\u00a76..." was an InvalidPathException and
    // non-ASCII chat, world and file names broke. UTF-8 for all of them.
    o[n++].optionString = (char *)"-Dfile.encoding=UTF-8"; o[n++].optionString = (char *)"-Dsun.jnu.encoding=UTF-8";
    // JDK 17 ignores -Dsun.jnu.encoding (the log of the 1440p run still says jnu=US-ASCII, and the pack still failed): it reads the C locale's charset. Ask for the UTF-8 one (the "jnu=" line of the Java log tells whether it took).
    if (!getenv("LC_ALL")) setenv("LC_ALL", "C.UTF-8", 1);
    if (!getenv("LANG")) setenv("LANG", "C.UTF-8", 1);
    o[n++].optionString = (char *)"-Dps5.mc=1";   // Hello runs McBoot instead of GLTest (the console keeps stale files from earlier deploys, so do not test for files)
    o[n++].optionString = (char *)"-Dlog4j.configurationFile=/app0/assets/mc/log4j2.xml";   // console only: chdir/relative log files do not work here
    o[n++].optionString = (char *)"-Dlog4j2.disable.jmx=true";
#ifdef NETPROBE
    o[n++].optionString = (char *)"-Dps5.netprobe=1";   // McBoot runs the DNS / TCP / HTTP / HTTPS probe before launching the game
#endif
    if (g_jdkv == 8) {   // Java 8: a classic jre (rt.jar has java.beans, PollingWatchService ...); only AWT-lite goes in front of it, and its headless Toolkit / GraphicsEnvironment are named by properties
        o[n++].optionString = (char *)"-Xbootclasspath/p:/app0/assets/lib/desktop-lite8.jar";   // BufferedImage / Image / ImageIO replacements (build_awtlite.sh 8)
        o[n++].optionString = (char *)"-Djava.awt.headless=true";
        o[n++].optionString = (char *)"-Dawt.toolkit=sun.awt.HToolkit";
        o[n++].optionString = (char *)"-Djava.awt.graphicsenv=sun.awt.PS5GraphicsEnvironment";
    } else {
        o[n++].optionString = bpatch;   // sun.nio.fs.PollingWatchService (missing from the image; needed by BsdFileSystem.newWatchService)
        o[n++].optionString = (char *)"-Xbootclasspath/a:/app0/assets/lib/desktop-lite.jar";   // java.beans etc. (java.desktop is not in the image)
    }
#else
    o[n++].optionString = (char *)"-XX:ReservedCodeCacheSize=32m";
    o[n++].optionString = (char *)"-XX:MaxMetaspaceSize=96m";
#endif
    o[n++].optionString = (char *)"-XX:-UsePerfData";
    o[n++].optionString = (char *)"-XX:+DisableAttachMechanism";
    o[n++].optionString = (char *)"-Xshare:off";
    o[n++].optionString = (char *)"-Xrs";
    o[n++].optionString = (char *)"-XX:-MaxFDLimit";
    o[n++].optionString = (char *)"-Xlog:all=warning:stdout";
    if (g_jdkv >= 9) { o[n++].optionString = (char *)"-Xlog:gc:stdout:none"; }
    if (g_jdkv >= 17) { o[n++].optionString = (char *)"-Xlog:handshake=info,safepoint=info:stdout:none"; }   // freeze after a render-distance change (log c37cc77a): the VM thread spun in VM_HandshakeAllThreads while the threads it needed sat on Heap_lock; this names the operation and the threads it was still waiting for   // one line per collection (a freeze right after a GC line = the collector; none = a thread that does not reach a safepoint)
    o[n++].optionString = (char *)"-XX:-UseDynamicNumberOfCompilerThreads";   // all compiler threads start at boot: no thread is created or retired while the game runs (every stall/abort so far followed a runtime compiler-thread change or thread start)
    o[n++].optionString = (char *)"-XX:+IgnoreUnrecognizedVMOptions";
#ifdef MC_CP
    o[n++].optionString = (char *)"-Djava.io.tmpdir=/download0";   // /app0 is read-only
#else
    o[n++].optionString = (char *)"-Djava.io.tmpdir=/app0/assets";
#endif
#if defined(USE_GL) || defined(MC_VK)
    o[n++].optionString = (char *)"-Dorg.lwjgl.system.allocator=system";
#endif
#ifdef MC_VK   // make_mcvk_title.sh: the Vulkan title has no native launcher UI (Minecraft is started straight from the instance in launcher.cfg) and asks the game for its Vulkan backend
    o[n++].optionString = (char *)"-Dps5.noui=1";
#ifdef AIO_RENDERER
    o[n++].optionString = (char *)((g_renderer_gl || g_renderer_zink) ? "-Dps5.vulkan=0" : "-Dps5.vulkan=1");
#else
    o[n++].optionString = (char *)"-Dps5.vulkan=1";
#endif
#endif
#if defined(AIO_RENDERER) && !defined(MC_VK)   // merged launcher title: the native UI needs OpenGL, so with the Vulkan renderer Minecraft is started straight from the instance
    { static char rn_opt[40]; snprintf(rn_opt, sizeof rn_opt, "-Dps5.renderer=%s", g_rsel_name); o[n++].optionString = rn_opt; }
    if (g_renderer_gl || g_renderer_zink) o[n++].optionString = (char *)"-Dps5.vulkan=0";
    else { o[n++].optionString = (char *)"-Dps5.vulkan=1"; o[n++].optionString = (char *)"-Dps5.noui=1"; }
#endif
    o[n].optionString = (char *)"vfprintf"; o[n++].extraInfo = (void *)vm_vfprintf;
    o[n].optionString = (char *)"exit"; o[n++].extraInfo = (void *)vm_exit;
    o[n].optionString = (char *)"abort"; o[n++].extraInfo = (void *)vm_abort;
    JavaVMInitArgs a; a.version = JNI_VERSION_1_8; a.nOptions = n; a.options = o; a.ignoreUnrecognized = JNI_TRUE;

    JavaVM *vm = 0; JNIEnv *env = 0;
    jint rc = jdk_create(&vm, (void **)&env, &a);
    out("PROBE jvm_create %s rc=%d\n", rc == 0 ? "PASS" : "FAIL", (int)rc);
    if (rc != 0) return 0;

    jclass c = env->FindClass("Hello");
    if (!c) { env->ExceptionDescribe(); out("PROBE find_hello FAIL\n"); return 0; }
#ifdef MC_CP
    probe_rm_all("post-jvm-create");   // same removal/rename tests right after the VM exists, still before any Java code runs
    JNINativeMethod nm[] = { { (char *)"log0", (char *)"(Ljava/lang/String;)V", (void *)native_log },
                             { (char *)"rmprobe", (char *)"()V", (void *)native_rmprobe } };
    if (env->RegisterNatives(c, nm, 2) != 0) { env->ExceptionClear(); env->RegisterNatives(c, nm, 1); out("Hello.rmprobe not registered (copy the new Hello.java too)\n"); }
#else
    JNINativeMethod nm[] = { { (char *)"log0", (char *)"(Ljava/lang/String;)V", (void *)native_log } };
    env->RegisterNatives(c, nm, 1);
#endif
#ifdef MC_CP
    {   JNINativeMethod em[] = { { (char *)"exitStep", (char *)"(ILjava/lang/String;)I", (void *)native_exitstep } };
        if (env->RegisterNatives(c, em, 1) != 0) { env->ExceptionClear(); out("Hello.exitStep not registered (copy the new Hello.java too)\n"); }
    }
    {   JNINativeMethod qm[] = { { (char *)"quit", (char *)"(Ljava/lang/String;)V", (void *)native_quit } };
        if (env->RegisterNatives(c, qm, 1) != 0) { env->ExceptionClear(); out("Hello.quit not registered (copy the new Hello.java too)\n"); }
    }
#endif
    {   JNINativeMethod cm[] = { { (char *)"crashFlag", (char *)"()I", (void *)native_crashflag }, { (char *)"crashAck", (char *)"()V", (void *)native_crashack } };
        if (env->RegisterNatives(c, cm, 2) != 0) { env->ExceptionClear(); out("Hello.crashFlag not registered (copy the new Hello.java too)\n"); }
    }
    {   // Hello.chdir; an older Hello.java simply lacks it
        JNINativeMethod dm[] = { { (char *)"chdir", (char *)"(Ljava/lang/String;)I", (void *)native_chdir } };
        if (env->RegisterNatives(c, dm, 1) != 0) { env->ExceptionClear(); out("Hello.chdir not registered (copy the new Hello.java too)\n"); }
    }
    {   // Hello.glCompat (Hello.java of this version); an older Hello.java simply lacks it
        JNINativeMethod gm[] = { { (char *)"glCompat", (char *)"(I)V", (void *)native_glcompat } };
        if (env->RegisterNatives(c, gm, 1) != 0) { env->ExceptionClear(); out("Hello.glCompat not registered (copy the new Hello.java too)\n"); }
    }
#ifdef NATIVE_UI
    {   // Kit.run is implemented by the native launcher (kit_stage.cpp)
        jclass kc = env->FindClass("Kit");
        if (kc) {
            JNINativeMethod km[] = { { (char *)"run", (char *)"(Ljava/lang/String;)I", (void *)native_kit_run }, { (char *)"held", (char *)"(I)I", (void *)native_kit_held } };
            if (env->RegisterNatives(kc, km, 2) != 0) { env->ExceptionClear(); out("[kit] Kit.run not registered\n"); }
        } else { env->ExceptionClear(); out("[kit] class Kit not found: copy Kit.java and rebuild\n"); }
    }
#endif
    jmethodID m = env->GetStaticMethodID(c, "main", "([Ljava/lang/String;)V");
    out("BEGIN hello_main\n");
    env->CallStaticVoidMethod(c, m, (jobjectArray)0);
    bool hello_failed = false;
    if (env->ExceptionCheck()) { hello_failed = true; env->ExceptionDescribe(); out("PROBE hello_main FAIL (exception)\n"); }
    else out("PROBE hello_main PASS\n");
#ifdef VKPROBE
    vkprobe_run("post-Hello, JVM alive", vkp_log, VKPROBE_SECS);
#endif
    g_main_done = 1;
#ifdef MC_CP
    rescue_launcher(hello_failed ? "crash java" : "exit 0");   // the game's main returned without System.exit: same as quitting, back to the launcher
#endif
    vm->DestroyJavaVM();
    out("DONE jvm\n");
    return 0;
}

int main() {
#ifdef GL_BARE_MIN
    // minimal: NOTHING from the host runs before the SDK sample-style EGL loop (no fds fixup, no sockets, no signal handlers, no threads)
    ps5_gl_selftest();
    for (;;) usleep(1000000);
#endif
    ensure_std_fds();
#ifdef GLTHREAD
    // Mesa glthread inside the SDK (tested there: multidraw, context sharing, animation): GL calls are queued on the render thread and executed by a worker,
    // so the ~0.09 ms CPU cost of every glDrawElements no longer sits on the thread that also runs Minecraft. Must be set before the first eglCreateContext.
    setenv("PS5_GLTHREAD", "1", 1);
#endif
#ifdef LOG_HOST
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    dst.sin_family = AF_INET; dst.sin_port = htons(LOG_PORT); inet_pton(AF_INET, LOG_HOST, &dst.sin_addr);
#endif
    out("[jvm] cpus: sysconf online=%ld configured=%ld; the JVM is told CPUS_N=%d\n", (long)sysconf(_SC_NPROCESSORS_ONLN), (long)sysconf(_SC_NPROCESSORS_CONF), (int)CPUS_N);   // how many logical CPUs does the title really see?
    capture_stdio();
    install_crash_handler();
    out("START jvm_host (crash reporter on) build back2l-14\n");
#ifdef VKPROBE
    vkprobe_run("pre-JVM", vkp_log, VKPROBE_SECS);
#endif
#ifdef MC_CP
    g_t0_ms = mono_ms(); usleep(300000); exitlog_replay();
#endif
#if defined(USE_GL) && defined(GL_SELFTEST) && defined(GL_SELFTEST_MAIN)
    // bisect: GL on the title's own main thread, like the SDK samples (no VM thread, no watchdog, no 64 MB pthread, no altstack)
    jdk_set_trace(trace_cb);
    out("GL_SELFTEST_MAIN: native window and 180 frames on the main thread, no JVM\n");
    ps5_gl_selftest();
    out("GL_SELFTEST_MAIN done\n");
    for (;;) usleep(1000000);
#endif
    if (ps5_calib) ps5_calib();   // libc pthread-object budget (ps5lwjgl.c): before the VM thread, while nothing else creates pthread objects
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t t;
    if (pthread_create(&t, &at, vm_main, 0)) { out("PROBE vm_thread FAIL\n"); }
    else { pthread_t w; pthread_create(&w, 0, watchdog, 0);
#ifdef MC_CP
        pthread_t qm; int qrc = pthread_create(&qm, 0, quit_monitor, 0); out("[jvm] quit monitor thread: pthread_create rc=%d\n", qrc);
#endif
        pthread_join(t, 0); }
    for (;;) usleep(1000000);   // returning from main kills the title
    return 0;
}
