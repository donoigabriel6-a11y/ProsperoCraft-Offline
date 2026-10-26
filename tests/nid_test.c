// Checks in_nid() (ps5lwjgl.c, extracted at test time) against known NIDs of the console's libkernel exports.
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "nid_extract.inc"
int main(void) {
    static const struct { const char *n, *nid; } k[] = { {"sceKernelLoadStartModule", "wzvqT4UqKX8"}, {"sceKernelDlsym", "LwG8g3niqwA"}, {"sceKernelAllocateDirectMemory", "rTXw65xmLIA"} };
    int fails = 0; char o[16];
    for (int i = 0; i < 3; i++) { in_nid(k[i].n, o); if (strcmp(o, k[i].nid)) { printf("FAIL %s: got %s want %s\n", k[i].n, o, k[i].nid); fails++; } }
    printf("fails=%d\n", fails); return fails != 0;
}
