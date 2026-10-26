// ps5vk_stubs.c: what ps5lwjgl.c still references from the OpenGL SDK when it is built with -DPS5_VK (make_mcvk_title.sh). The GL code is compiled but never runs in the Vulkan title:
// no EGL display or context is ever made, so these only have to exist for the link. The libc / system functions are the platform's own.
#include <stddef.h>
#ifndef AIO   // make_mcvk_title.sh AIO=1 links the real OpenGL SDK
void *eglGetProcAddress(const char *n) { (void)n; return 0; }
void *eglGetCurrentContext(void) { return 0; }
unsigned eglGetError(void) { return 0x3000; }
unsigned eglMakeCurrent(void *d, void *s, void *r, void *c) { (void)d; (void)s; (void)r; (void)c; return 0; }
unsigned eglSwapBuffers(void *d, void *s) { (void)d; (void)s; return 0; }
unsigned eglSwapInterval(void *d, int i) { (void)d; (void)i; return 0; }
#endif
size_t ps5_opengl_heap_live_bytes(void) { return 0; }
void ps5_opengl_heap_stats_print(unsigned n) { (void)n; }
// the platform's libc has no pathconf (the JDK's UnixFileSystem asks for NAME_MAX / PATH_MAX): fixed answers
long pathconf(const char *p, int name) { (void)p; return 4096; }
long fpathconf(int fd, int name) { (void)fd; (void)name; return 4096; }   // same for the descriptor form (some JDK build asks for it)
#ifdef AIO
// Mesa's glapi thread-local wrapper init (the SDK's native-app/runtime_shims.c defines the same empty function)
void ps5_glapi_tls_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void ps5_glapi_tls_init(void) {}
// the GL SDK's Mesa calls isatty (the platform's libc has none): never a terminal
int isatty(int fd) { (void)fd; return 0; }
#endif
#ifdef PS5_ZINK
// Zink + EGL (zink.o) reference a few FreeBSD libc functions the platform's libc does not have. Weak: a real definition anywhere wins. Answers are "not supported", Zink does not depend on them on this path.
#include <stdint.h>
#define WK __attribute__((weak))
extern int *__error(void);
WK char *devname_r(uint32_t dev, uint32_t type, char *buf, int len) { (void)dev; (void)type; if (len > 0) buf[0] = 0; return buf; }
WK int shmget(long k, size_t sz, int fl) { (void)k; (void)sz; (void)fl; *__error() = 38; return -1; }
WK void *shmat(int id, const void *a, int fl) { (void)id; (void)a; (void)fl; *__error() = 38; return (void *)-1; }
WK int shmdt(const void *a) { (void)a; return -1; }
WK int shmctl(int id, int cmd, void *b) { (void)id; (void)cmd; (void)b; return -1; }
WK int mknod(const char *p, unsigned m, unsigned d) { (void)p; (void)m; (void)d; *__error() = 1; return -1; }
WK int utime(const char *p, const void *t) { (void)p; (void)t; return 0; }
WK int chown(const char *p, unsigned u, unsigned g) { (void)p; (void)u; (void)g; return 0; }
WK int flock(int fd, int op) { (void)fd; (void)op; return 0; }
WK int mincore(void *a, size_t l, char *v) { (void)a; (void)l; (void)v; *__error() = 38; return -1; }
#endif
