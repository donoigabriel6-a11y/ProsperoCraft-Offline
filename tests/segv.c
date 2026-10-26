#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
static void h(int s, siginfo_t *si, void *u) { ucontext_t *uc = (ucontext_t *)u; char b[200]; int n = snprintf(b, sizeof b, "SEGV addr=%p rip=%llx\n", si->si_addr, (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]); write(2, b, n); _exit(9); }
__attribute__((constructor)) static void init(void) { struct sigaction sa = {0}; sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, 0); }
