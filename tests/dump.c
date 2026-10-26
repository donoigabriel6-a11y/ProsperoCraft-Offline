#include "../ps5lwjgl.c"
#include <unistd.h>
void ps5_trace(const char *m) {}
int scePadReadState(int h, PadState *s) { return -1; } int scePadInit(void) { return 0; } int scePadOpen(int a, int b, int c, const void *d) { return 1; }
int sceUserServiceInitialize(void *p) { return 0; } int sceUserServiceGetInitialUser(int *u) { *u = 1; return 0; }
size_t ps5_opengl_heap_live_bytes(void) { return 0; } void ps5_opengl_heap_stats_print(unsigned x) {}
void *eglGetProcAddress(const char *n) { return 0; }
int main(void) { g_w = 1920; g_h = 1080; g_osk_open = 1; g_osk_row = 2; g_osk_k = 3; g_osk_shift = 1; g_cx = 700; g_cy = 500; g_osk_top = 0;
  g_ov_n = 0; osk_build(); cursor_build(); FILE *f = fopen("/tmp/ov.bin", "wb"); fwrite(&g_ov_n, 4, 1, f); fwrite(g_ov, sizeof(OvV), g_ov_n, f); fclose(f); return 0; }
