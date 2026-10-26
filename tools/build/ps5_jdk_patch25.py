#!/usr/bin/env python3
"""Apply the PS5 compile fixes to a jdk21u checkout (port of ps5_jdk_patch.py, which targets jdk17u). Safe to run twice.
usage: python3 ps5_jdk_patch21.py ~/jdk21u"""
import os, re, sys

root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/jdk21u")
hs = os.path.join(root, "src/hotspot")
changed = []

def edit(rel, fn, label):
    p = os.path.join(hs, rel) if not rel.startswith("/") else rel
    s = open(p).read()
    t = fn(s)
    if t != s:
        open(p, "w").write(t)
        changed.append(label)

def sub(old, new):
    def f(s):
        return s.replace(old, new) if old in s else s
    return f

# --- make files / cpu header
def jvmmapfile(s):
    return s.replace("ifeq ($(call isTargetOs, linux), true)",
                     "ifeq ($(call Or, $(call isTargetOs, linux) $(call isTargetOs, bsd)), true)", 1)
p = os.path.join(root, "make/hotspot/lib/JvmMapfile.gmk")
if os.path.exists(p):
    s = open(p).read(); t = jvmmapfile(s)
    if t != s: open(p, "w").write(t); changed.append("JvmMapfile.gmk")
# jdk25: JvmMapfile.gmk is gone (symbols are exported by visibility attributes; only linux uses a version script), edit dropped

# jdk21: bytes_bsd_x86.hpp no longer exists (cpu/x86/bytes_x86.hpp uses utilities/byteswap.hpp), the __bswap -> __builtin_bswap edit is not needed

# jdk25: HotSpot includes libc++ headers (<cstddef>, <type_traits>, ...). The precompiled header is built with the C compiler driver ($1_CC, "-x c++-header"), and the SDK's prospero-clang puts
# target/include before libc++'s directory, which libc++ rejects ("didn't find libc++'s <stddef.h>"). prospero-clang++ orders them correctly, so build the PCH with the C++ driver.
p = os.path.join(root, "make/common/native/CompileFile.gmk")
s = open(p).read()
t = s.replace("$1_PCH_COMMAND := $$($1_CC) $$($1_CFLAGS)", "$1_PCH_COMMAND := $$($1_CXX) $$($1_CFLAGS)", 1)
if t != s: open(p, "w").write(t); changed.append("CompileFile.gmk:PCH with the C++ driver")

# --- missing header
alloca = os.path.join(hs, "os/bsd/alloca.h")
if not os.path.exists(alloca):
    open(alloca, "w").write("#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca(n) __builtin_alloca(n)\n#endif\n")
    changed.append("alloca.h")

NORES = "#ifndef MAP_NORESERVE\n#define MAP_NORESERVE 0\n#endif\n"
def add_after_precompiled(block, marker):
    def f(s):
        if marker in s: return s
        i = s.index('#include "')          # before the first project include
        return s[:i] + block + s[i:]
    return f

# --- os_posix.cpp
edit("os/posix/os_posix.cpp", add_after_precompiled("// PS5\n" + NORES, "// PS5\n"), "os_posix.cpp:MAP_NORESERVE")
# jdk21: the 'bsd and aix don't have this' block in os_posix.cpp no longer exists, edit dropped (a compile error there would show up in the first build)

# --- os_bsd.cpp
SYS = ("# include <sys/sysctl.h>\n"
       "// PS5 compat\n"
       "#ifndef HW_CPU_FREQ\n#define HW_CPU_FREQ 0\n#endif\n"
       "#ifndef __APPLE__\nstruct xsw_usage { uint64_t xsu_total, xsu_avail, xsu_used; };\n#endif\n")
def os_bsd(s):
    if "// PS5 compat" not in s:
        s = s.replace("# include <sys/sysctl.h>\n", SYS, 1)
    s = add_after_precompiled("// PS5\n" + NORES, "// PS5\n")(s)
    s = s.replace("const char* const error_report = ::dlerror();\n  if (error_report == NULL) {\n    error_report = \"dlerror returned no error description\";",
                  "const char* error_report = ::dlerror();\n  if (error_report == NULL) {\n    error_report = \"dlerror returned no error description\";")
    s = s.replace("arch_t lib_arch={elf_head.e_machine,0,elf_head.e_ident[EI_CLASS], elf_head.e_ident[EI_DATA], NULL};",
                  "arch_t lib_arch={elf_head.e_machine,0,(char)elf_head.e_ident[EI_CLASS], (char)elf_head.e_ident[EI_DATA], NULL};")
    s = s.replace("int os::get_loaded_modules_info(os::LoadedModulesCallbackFunc callback, void *param) {\n#ifdef RTLD_DI_LINKMAP",
                  "int os::get_loaded_modules_info(os::LoadedModulesCallbackFunc callback, void *param) {\n#if defined(RTLD_DI_LINKMAP) && defined(__APPLE__)")
    return s
edit("os/bsd/os_bsd.cpp", os_bsd, "os_bsd.cpp")

# --- os_bsd_x86.cpp (sysctlbyname)
edit("os_cpu/bsd_x86/os_bsd_x86.cpp",
     lambda s: s if ("#include <sys/sysctl.h>\njuint" in s or "static inline int ps5_sysctl" in s) else s.replace(
         "juint os::cpu_microcode_revision() {", "#include <sys/sysctl.h>\njuint os::cpu_microcode_revision() {", 1),
     "os_bsd_x86.cpp")

# --- os_perf_bsd.cpp: stub the macOS-only network statistics
def perf(s):
    key = "int NetworkPerformanceInterface::NetworkPerformance::network_utilization(NetworkInterface** network_interfaces) const {"
    i = s.find(key)
    if i < 0 or "// PS5 stub" in s[i:i+200]: return s
    j = s.index("{", i); depth = 0; k = j
    while True:
        if s[k] == "{": depth += 1
        elif s[k] == "}":
            depth -= 1
            if depth == 0: break
        k += 1
    return s[:j+1] + "\n  // PS5 stub\n  *network_interfaces = NULL;\n  return OS_ERR;\n}" + s[k+1:]
edit("os/bsd/os_perf_bsd.cpp", perf, "os_perf_bsd.cpp")


# --- stubs for the few libc/VM symbols the PS5 SDK lacks (compiled automatically: every .cpp in os/bsd is built)
SUPPORT = r'''// PS5 support: libc/VM symbols the SDK stubs lack, and the Linux-only decoder.
#include "jvm.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <langinfo.h>
#include <stdio.h>
extern "C" void ps5_trace(const char *);
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>
#include "utilities/decoder_elf.hpp"

bool ElfDecoder::demangle(const char* symbol, char* buf, int buflen) { return false; }

namespace {
struct EmuCtl { size_t size; size_t align; uintptr_t index; void *value; };  // LLVM emulated-TLS control block
struct EmuArr { uintptr_t n; void *slot[1]; };
pthread_key_t emu_key;
pthread_once_t emu_once = PTHREAD_ONCE_INIT;
pthread_mutex_t emu_mu = PTHREAD_MUTEX_INITIALIZER;
uintptr_t emu_count;
void emu_init() { pthread_key_create(&emu_key, 0); }

bool leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
struct tm *utc_tm(const time_t *tp, struct tm *r) {
    long long t = *tp, days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    r->tm_hour = (int)(rem / 3600); r->tm_min = (int)(rem % 3600 / 60); r->tm_sec = (int)(rem % 60);
    r->tm_wday = (int)((days + 4) % 7); if (r->tm_wday < 0) r->tm_wday += 7;
    long y = 1970;
    while (days < 0) { y--; days += leap(y) ? 366 : 365; }
    while (days >= (leap(y) ? 366 : 365)) { days -= leap(y) ? 366 : 365; y++; }
    r->tm_year = (int)(y - 1900); r->tm_yday = (int)days;
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = 0;
    while (m < 12) { int d = md[m] + (m == 1 && leap(y) ? 1 : 0); if (days < d) break; days -= d; m++; }
    r->tm_mon = m; r->tm_mday = (int)days + 1; r->tm_isdst = 0;
    r->tm_gmtoff = 0; r->tm_zone = (char *)"UTC";
    return r;
}
}  // namespace

extern "C" {
void *__emutls_get_address(void *vctl) {
    EmuCtl *c = (EmuCtl *)vctl;
    pthread_once(&emu_once, emu_init);
    if (!c->index) {
        pthread_mutex_lock(&emu_mu);
        if (!c->index) c->index = ++emu_count;
        pthread_mutex_unlock(&emu_mu);
    }
    uintptr_t idx = c->index;
    EmuArr *a = (EmuArr *)pthread_getspecific(emu_key);
    if (!a || a->n < idx) {
        uintptr_t n = idx + 16;
        EmuArr *na = (EmuArr *)calloc(1, sizeof(uintptr_t) + n * sizeof(void *));
        if (!na) { char m[96]; snprintf(m, sizeof m, "emutls: calloc(%lu) failed idx=%lu", (unsigned long)(sizeof(uintptr_t) + n * sizeof(void *)), (unsigned long)idx); ps5_trace(m); abort(); }
        if (a) { memcpy(na->slot, a->slot, a->n * sizeof(void *)); free(a); }
        na->n = n; pthread_setspecific(emu_key, na); a = na;
    }
    void **slot = &a->slot[idx - 1];
    if (!*slot) {
        size_t al = c->align < sizeof(void *) ? sizeof(void *) : c->align;
        void *p = 0;
        if (posix_memalign(&p, al, c->size ? c->size : 1)) { char m[96]; snprintf(m, sizeof m, "emutls: posix_memalign(align=%lu size=%lu) failed", (unsigned long)al, (unsigned long)c->size); ps5_trace(m); abort(); }
        if (c->value) memcpy(p, c->value, c->size); else memset(p, 0, c->size);
        *slot = p;
    }
    return *slot;
}

int dladdr(const void *, Dl_info *) { return 0; }
int __xuname(int len, void *buf) {
    memset(buf, 0, (size_t)len * 5);
    char *b = (char *)buf;
    strcpy(b, "FreeBSD"); strcpy(b + len, "ps5"); strcpy(b + 2 * len, "11.0-RELEASE"); strcpy(b + 3 * len, "PS5"); strcpy(b + 4 * len, "amd64");
    return 0;
}
int dirfd(DIR *d) { return d ? *(int *)d : -1; }
int getloadavg(double *l, int n) { for (int i = 0; i < n; i++) l[i] = 0; return -1; }
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **res) { *res = 0; return ENOENT; }
struct tm *gmtime_r(const time_t *t, struct tm *r) { return utc_tm(t, r); }
struct tm *localtime_r(const time_t *t, struct tm *r) { return utc_tm(t, r); }
int posix_fallocate(int, off_t, off_t) { return ENOSYS; }
int syscall(int, ...) { errno = ENOSYS; return -1; }
clock_t times(struct tms *b) {
    if (b) memset(b, 0, sizeof *b);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (clock_t)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
}
// Solaris Studio profiler hook that forte.cpp references weakly; the title converter needs every import resolved.
void collector_func_load(char *, void *, void *, void *, int, int, void *) {}
int getpeereid(int, unsigned int *, unsigned int *) { return -1; }
ssize_t getline(char **lp, size_t *np, FILE *f) {
    size_t n = 0; int c;
    if (!*lp || !*np) { *np = 128; *lp = (char *)malloc(*np); if (!*lp) return -1; }
    while ((c = getc(f)) != EOF) {
        if (n + 2 > *np) { size_t nn = *np * 2; char *t = (char *)realloc(*lp, nn); if (!t) return -1; *lp = t; *np = nn; }
        (*lp)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*lp)[n] = 0;
    return (ssize_t)n;
}
int mknod(const char *, mode_t, dev_t) { errno = ENOSYS; return -1; }
char *nl_langinfo(nl_item) { return (char *)"UTF-8"; }
int waitid(idtype_t, id_t, siginfo_t *, int) { errno = ENOSYS; return -1; }
struct passwd *getpwuid(uid_t) { return 0; }
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **res) { *res = 0; return ENOENT; }
int getgrgid_r(gid_t, struct group *, char *, size_t, struct group **res) { *res = 0; return ENOENT; }
int getgrnam_r(const char *, struct group *, char *, size_t, struct group **res) { *res = 0; return ENOENT; }
void *getutxent(void) { return 0; }
void setutxent(void) {}
}
'''
sp = os.path.join(hs, "os/bsd/ps5_support.cpp")
if not os.path.exists(sp) or open(sp).read() != SUPPORT:
    open(sp, "w").write(SUPPORT)
    changed.append("ps5_support.cpp")

# --- PS5 memory shim: big reservations = VA reserve + direct-memory commit; small/exec = plain mmap
MEM = r'''// ps5_mem.cpp: HotSpot memory on the PS5.
// mmap(PROT_NONE) draws on a ~370 MB pool, so large reservations use sceKernelReserveVirtualRange
// (address space only) and commit with direct memory mapped MAP_FIXED. Executable and tiny
// reservations use plain mmap (direct memory refuses PROT_EXEC).
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdio.h>

extern "C" void ps5_trace(const char *);
static void mtrace(const char *what, unsigned long long a, unsigned long long b, int r) {
    char m[160]; snprintf(m, sizeof m, "mem %s a=%llx n=%llx -> %d", what, a, b, r); ps5_trace(m);
}

extern "C" {
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelReserveVirtualRange(void **, size_t, int, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
}

namespace {
const size_t GRAN = 2u << 20;         // commit granule (alignment the probe proved works)
const size_t DIRECT_MIN = 4u << 20;   // below this, use plain mmap
struct Resv { uintptr_t base, len, obase, olen; int direct; };
struct Run  { uintptr_t base, len; int64_t phys; };
Resv rv[512]; int nrv;
Run  cm[4096]; int ncm;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

uintptr_t up(uintptr_t v, size_t a) { return (v + a - 1) & ~(uintptr_t)(a - 1); }
uintptr_t dn(uintptr_t v, size_t a) { return v & ~(uintptr_t)(a - 1); }
Resv *find(uintptr_t a, size_t n) {
    for (int i = 0; i < nrv; i++) if (a >= rv[i].base && a + n <= rv[i].base + rv[i].len) return &rv[i];
    return 0;
}
bool map_gap(uintptr_t p, size_t n) {
    int64_t phys = 0, total = sceKernelGetDirectMemorySize();
    if (sceKernelAllocateDirectMemory(0, total, n, GRAN, 0, &phys)) return false;
    void *a = (void *)p;
    if (sceKernelMapDirectMemory(&a, n, PROT_READ | PROT_WRITE, MAP_FIXED, phys, GRAN) || a != (void *)p) {
        sceKernelReleaseDirectMemory(phys, n); return false;
    }
    if (ncm >= 4096) return false;
    cm[ncm++] = Run{ p, n, phys };
    return true;
}
}  // namespace

extern "C" char *ps5_reserve(char *hint, size_t bytes, bool exec) {
    pthread_mutex_lock(&mu);
    char *res = 0;
    if (nrv >= 512) goto out;
    if (exec || bytes < DIRECT_MIN) {
        int prot = PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0);
        void *m = mmap(hint, bytes, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) goto out;
        if (hint && m != hint) { munmap(m, bytes); goto out; }
        rv[nrv++] = Resv{ (uintptr_t)m, bytes, (uintptr_t)m, bytes, 0 };
        res = (char *)m;
    } else {
        size_t len = up(bytes, GRAN), olen = len + GRAN;
        void *a = hint;
        if (sceKernelReserveVirtualRange(&a, olen, 0, 0) || !a) goto out;
        uintptr_t base = up((uintptr_t)a, GRAN);
        if (hint && base != (uintptr_t)hint) { munmap(a, olen); goto out; }
        rv[nrv++] = Resv{ base, len, (uintptr_t)a, olen, 1 };
        res = (char *)base;
    }
out:
    pthread_mutex_unlock(&mu);
    mtrace(exec ? "reserve(exec)" : "reserve", (unsigned long long)(uintptr_t)(res ? res : hint), (unsigned long long)bytes, res != 0);
    return res;
}

extern "C" bool ps5_commit(char *addr, size_t size, bool exec) {
    pthread_mutex_lock(&mu);
    Resv *r = find((uintptr_t)addr, size);
    bool ok = true;
    if (!r) {
        ok = mprotect(addr, size, PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0)) == 0;
    } else if (!r->direct) {
        ok = mprotect(addr, size, PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0)) == 0;
    } else {
        uintptr_t s = dn((uintptr_t)addr, GRAN), e = up((uintptr_t)addr + size, GRAN);
        if (s < r->base) s = r->base;
        if (e > r->base + r->len) e = r->base + r->len;
        uintptr_t p = s;
        while (p < e && ok) {
            uintptr_t end = e; bool covered = false;
            for (int i = 0; i < ncm; i++) {
                if (cm[i].base <= p && p < cm[i].base + cm[i].len) { p = cm[i].base + cm[i].len; covered = true; break; }
                if (cm[i].base > p && cm[i].base < end) end = cm[i].base;
            }
            if (covered) continue;
            ok = map_gap(p, end - p);
            p = end;
        }
    }
    pthread_mutex_unlock(&mu);
    if (!ok || size >= (1u << 20)) mtrace("commit", (unsigned long long)(uintptr_t)addr, (unsigned long long)size, ok);
    return ok;
}

extern "C" unsigned long long ps5_physical_memory(void) {
    int64_t t = sceKernelGetDirectMemorySize();
    return t > 0 ? (unsigned long long)t : (4ull << 30);
}

extern "C" bool ps5_release(char *addr, size_t size) {
    pthread_mutex_lock(&mu);
    bool ok = true;
    for (int i = 0; i < nrv; i++) {
        if (rv[i].base != (uintptr_t)addr) continue;           // partial releases are ignored (address space leak only)
        if (size + GRAN <= rv[i].len) break;                    // trimming the front of a bigger reservation (reserve_memory_aligned): keep it
        if (rv[i].direct) {
            for (int j = 0; j < ncm;) {
                if (cm[j].base >= rv[i].base && cm[j].base < rv[i].base + rv[i].len) {
                    munmap((void *)cm[j].base, cm[j].len);
                    sceKernelReleaseDirectMemory(cm[j].phys, cm[j].len);
                    cm[j] = cm[--ncm];
                } else j++;
            }
            munmap((void *)rv[i].obase, rv[i].olen);
        } else {
            ok = munmap((void *)rv[i].base, rv[i].len) == 0;
        }
        rv[i] = rv[--nrv];
        break;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}
'''
mp = os.path.join(hs, "os/bsd/ps5_mem.cpp")
if not os.path.exists(mp) or open(mp).read() != MEM:
    open(mp, "w").write(MEM)
    changed.append("ps5_mem.cpp")

def hook_mem(s):
    if "ps5_reserve" in s: return s
    s = s.replace("// PS5 compat\n", "// PS5 compat\nextern \"C\" char* ps5_reserve(char*, size_t, bool);\nextern \"C\" bool ps5_commit(char*, size_t, bool);\nextern \"C\" bool ps5_release(char*, size_t);\n", 1)
    s = s.replace("char* os::pd_reserve_memory(size_t bytes, bool exec) {\n", "char* os::pd_reserve_memory(size_t bytes, bool exec) {\n  return ps5_reserve(NULL, bytes, exec);\n", 1)
    s = s.replace("  return anon_munmap(addr, size);\n}\n\nstatic bool bsd_mprotect", "  return ps5_release(addr, size);\n}\n\nstatic bool bsd_mprotect", 1)
    s = s.replace("bool os::pd_commit_memory(char* addr, size_t size, bool exec) {\n", "bool os::pd_commit_memory(char* addr, size_t size, bool exec) {\n  return ps5_commit(addr, size, exec);\n", 1)
    s = s.replace("bool os::pd_uncommit_memory(char* addr, size_t size, bool exec) {\n", "bool os::pd_uncommit_memory(char* addr, size_t size, bool exec) {\n  return true;  // PS5: committed direct memory stays mapped\n", 1)
    s = s.replace("char* os::pd_attempt_reserve_memory_at(char* requested_addr, size_t bytes, bool exec) {\n", "char* os::pd_attempt_reserve_memory_at(char* requested_addr, size_t bytes, bool exec) {\n  return ps5_reserve(requested_addr, bytes, exec);\n", 1)
    return s
edit("os/bsd/os_bsd.cpp", hook_mem, "os_bsd.cpp:memory hooks")

# --- jdk21 HotSpot: non-Apple BSD code has bit-rotted upstream (only macOS is built from it)
# --- jdk21 introduces compiler thread-local storage (17 had none: USE_LIBRARY_BASED_TLS_ONLY covered everything): symbolTable.cpp (_lookup_shared_first), WorkerThread::_worker_id (parallel GC) and the
# upcall linker's C++ thread_local UpcallContext (whose destructor is what pulled in __cxa_thread_atexit_impl). Any of them gives the title's executable a PT_TLS segment, which the console's libc is not
# known to cope with (the first 21 run crashed inside the allocator on the first native-call preparation). Replace all three with ordinary variables / per-object fields.
def _sym_tls(t):
    return t.replace("static THREAD_LOCAL bool _lookup_shared_first = false;", "static bool _lookup_shared_first = false;   // PS5: was THREAD_LOCAL; only a CDS lookup-order hint (CDS is off)", 1)
edit("share/classfile/symbolTable.cpp", _sym_tls, "symbolTable.cpp:no TLS")
def _wt_hpp(t):
    if "_my_worker_id" in t: return t
    t = t.replace("  static THREAD_LOCAL uint _worker_id;\n\n  WorkerTaskDispatcher* const _dispatcher;\n\n  static void set_worker_id(uint worker_id) { _worker_id = worker_id; }\n",
                  "  WorkerTaskDispatcher* const _dispatcher;\n  uint _my_worker_id;   // PS5: was a THREAD_LOCAL static (compiler TLS); now a field of the worker's own Thread object\n\n  static void set_worker_id(uint worker_id) { Thread* t = Thread::current_or_null(); if (t != nullptr && t->is_Worker_thread()) ((WorkerThread*)t)->_my_worker_id = worker_id; }\n", 1)
    t = t.replace("  static uint worker_id() { return _worker_id; }\n", "  static uint worker_id() { Thread* t = Thread::current_or_null(); return (t != nullptr && t->is_Worker_thread()) ? ((WorkerThread*)t)->_my_worker_id : UINT_MAX; }\n", 1)
    return t
edit("share/gc/shared/workerThread.hpp", _wt_hpp, "workerThread.hpp:no TLS")
def _wt_cpp(t):
    t = t.replace("THREAD_LOCAL uint WorkerThread::_worker_id = UINT_MAX;\n\n", "", 1)
    t = t.replace("    _dispatcher(dispatcher) {\n  set_name(\"%s#%u\", name_prefix, name_suffix);", "    _dispatcher(dispatcher), _my_worker_id(UINT_MAX) {\n  set_name(\"%s#%u\", name_prefix, name_suffix);", 1)
    return t
edit("share/gc/shared/workerThread.cpp", _wt_cpp, "workerThread.cpp:no TLS")
edit("share/prims/upcallLinker.cpp", lambda t: t.replace("APPROVED_CPP_THREAD_LOCAL UpcallContext threadContext;", "UpcallContext threadContext;   // PS5: was thread_local (foreign-linker upcalls are not used here)", 1), "upcallLinker.cpp:no TLS")

edit("os/bsd/osThread_bsd.cpp", lambda t: t.replace("#else\n  _thread_id        = nullptr;\n#endif", "#else\n  _thread_id        = 0;   // PS5: thread_id_t is pid_t here\n#endif", 1), "osThread_bsd.cpp:thread_id")
edit("os/bsd/os_bsd.cpp", lambda t: t.replace("const char* const error_report = ::dlerror();", "const char* error_report = ::dlerror();", 1), "os_bsd.cpp:error_report")
edit("os/bsd/os_bsd.cpp", lambda t: t.replace("arch_t lib_arch={elf_head.e_machine,0,elf_head.e_ident[EI_CLASS], elf_head.e_ident[EI_DATA], nullptr};", "arch_t lib_arch={elf_head.e_machine,0,(char)elf_head.e_ident[EI_CLASS], (char)elf_head.e_ident[EI_DATA], nullptr};", 1), "os_bsd.cpp:narrowing")

# jdk25: the non-Apple BSD os::dll_load still names a variable (error_report) that no longer exists in that scope, and OSThread's non-Apple branch initialises a pid_t with nullptr (only macOS is built from this code upstream)
edit("os/bsd/os_bsd.cpp", lambda t: t.replace('failed, %s", filename, error_report);', 'failed, %s", filename, ebuf);'), "os_bsd.cpp:dll_load error_report")
edit("os/bsd/osThread_bsd.cpp", lambda t: t.replace("#else\n        nullptr\n#endif", "#else\n        0   // PS5: thread_id_t is pid_t here\n#endif", 1), "osThread_bsd.cpp:thread_id")

# --- jdk21: jdk.internal.util.OperatingSystem is an enum (LINUX, MACOS, WINDOWS, AIX) built from the target OS string ("bsd" here): OperatingSystem.current() would throw "No enum constant ...BSD"
# in its static initializer (ZipFile, PortConfig, ProcessImpl, LauncherHelper). This BSD port is derived from the macOS code, so it presents itself as macos.
def _os_canon(t):
    old = "ifeq ($(OPENJDK_TARGET_OS), macosx)\n  OPENJDK_TARGET_OS_CANONICAL = macos"
    if old not in t: return t
    return t.replace(old, "ifneq ($(filter macosx bsd,$(OPENJDK_TARGET_OS)),)   # PS5: bsd presents itself as macos\n  OPENJDK_TARGET_OS_CANONICAL = macos", 1)
edit(os.path.join(root, "make/modules/java.base/gensrc/GensrcMisc.gmk"), _os_canon, "GensrcMisc.gmk:bsd=macos")

# --- java.base: JDK 17 only ships BSD classes for macOS. Build a "bsd" overlay from them (poll selector, no kqueue).
def overlay():
    jb = os.path.join(root, "src/java.base")
    mac, bsd = os.path.join(jb, "macosx"), os.path.join(jb, "bsd")
    def cp(rel, subs=()):
        sp, dp = os.path.join(mac, rel), os.path.join(bsd, rel)
        if os.path.exists(dp): return
        os.makedirs(os.path.dirname(dp), exist_ok=True)
        t = open(sp).read()
        for a, b in subs: t = t.replace(a, b)
        open(dp, "w").write(t); changed.append("bsd/" + rel)
    def put(rel, text):
        dp = os.path.join(bsd, rel)
        if os.path.exists(dp): return
        os.makedirs(os.path.dirname(dp), exist_ok=True)
        open(dp, "w").write(text); changed.append("bsd/" + rel)
    for n in ["BsdFileSystem", "BsdFileSystemProvider", "BsdFileStore", "BsdNativeDispatcher", "BsdUserDefinedFileAttributeView", "BsdFileAttributeViews"]:
        cp("classes/sun/nio/fs/%s.java" % n)
    cp("classes/sun/nio/fs/DefaultFileSystemProvider.java", [("MacOSXFileSystemProvider", "BsdFileSystemProvider")])
    cp("classes/sun/nio/ch/DefaultSelectorProvider.java", [("KQueueSelectorProvider", "PollSelectorProvider")])
    cp("native/libnio/fs/BsdNativeDispatcher.c")
    # jdk21: sun.nio.ch.FileDispatcherImpl is per platform (unix/ only has UnixFileDispatcherImpl). macOS's version needs F_FULLFSYNC and Apple's sendfile: keep the Java side, replace the natives.
    cp("classes/sun/nio/ch/FileDispatcherImpl.java")
    put("native/libnio/ch/FileDispatcherImpl.c", "#include <sys/types.h>\n#include <unistd.h>\n#include <errno.h>\n\n#include \"jni.h\"\n#include \"nio.h\"\n#include \"nio_util.h\"\n#include \"sun_nio_ch_FileDispatcherImpl.h\"\n\n"
        "JNIEXPORT jint JNICALL\nJava_sun_nio_ch_FileDispatcherImpl_force0(JNIEnv *env, jobject this, jobject fdo, jboolean md)\n{\n    jint fd = fdval(env, fdo);\n    int result = fsync(fd);\n"
        "    if (result == -1) {\n        if (errno == EINTR) return IOS_INTERRUPTED;\n        JNU_ThrowIOExceptionWithLastError(env, \"Force failed\");\n        return IOS_THROWN;\n    }\n    return result;\n}\n\n"
        "JNIEXPORT jlong JNICALL\nJava_sun_nio_ch_FileDispatcherImpl_transferTo0(JNIEnv *env, jobject this, jobject srcFDO, jlong position, jlong count, jobject dstFDO, jboolean append)\n{\n    return IOS_UNSUPPORTED_CASE;   // the Java side then copies with read/write\n}\n")
    # jdk25: PollerProvider's factory methods take a boolean subPoller.
    # jdk21: virtual threads park on a Poller; there is no kqueue here, so the BSD provider refuses (platform threads, which Minecraft uses, never ask for it)
    put("classes/sun/nio/ch/DefaultPollerProvider.java", "package sun.nio.ch;\n\nimport java.io.IOException;\n\nclass DefaultPollerProvider extends PollerProvider {\n    DefaultPollerProvider() { }\n\n"
        "    @Override\n    Poller readPoller(boolean subPoller) throws IOException { throw new UnsupportedOperationException(\"virtual-thread socket polling is not supported on this platform\"); }\n\n"
        "    @Override\n    Poller writePoller(boolean subPoller) throws IOException { throw new UnsupportedOperationException(\"virtual-thread socket polling is not supported on this platform\"); }\n}\n")
    pp = os.path.join(bsd, "native/libjava/ProcessHandleImpl_bsd.c")
    if not os.path.exists(pp):
        os.makedirs(os.path.dirname(pp), exist_ok=True)
        open(pp, "w").write("#include <jni.h>\n#include <sys/types.h>\n#include \"ProcessHandleImpl_unix.h\"\n\n"
            "void os_initNative(JNIEnv *env, jclass clazz) {}\n"
            "jint os_getChildren(JNIEnv *env, jlong jpid, jlongArray jarray, jlongArray jparentArray, jlongArray jstimesArray) { return 0; }\n"
            "pid_t os_getParentPidAndTimings(JNIEnv *env, pid_t jpid, jlong *totalTime, jlong *startTime) { return -1; }\n"
            "void os_getCmdlineAndUserInfo(JNIEnv *env, jobject jinfo, pid_t pid) {}\n")
        changed.append("bsd/ProcessHandleImpl_bsd.c")
    ap = os.path.join(bsd, "classes/sun/nio/ch/DefaultAsynchronousChannelProvider.java")
    if not os.path.exists(ap):
        os.makedirs(os.path.dirname(ap), exist_ok=True)
        open(ap, "w").write("package sun.nio.ch;\n\nimport java.nio.channels.spi.AsynchronousChannelProvider;\n\n"
            "public class DefaultAsynchronousChannelProvider {\n    private DefaultAsynchronousChannelProvider() { }\n\n"
            "    public static AsynchronousChannelProvider create() {\n"
            "        throw new UnsupportedOperationException(\"asynchronous channels are not supported on this platform\");\n    }\n}\n")
        changed.append("bsd/DefaultAsynchronousChannelProvider")
overlay()

# --- jdk21: macOS-only APIs (clonefile, fsetattrlist, copyfile, sys/attr.h) sit behind _ALLBSD_SOURCE in the shared unix sources, as if every BSD were macOS. Make them __APPLE__-only and give the BSD overlay stand-ins.
def bsd_edit(rel, fn, label):
    p = os.path.join(root, "src/java.base/bsd", rel)
    t0 = open(p).read(); t1 = fn(t0)
    if t1 != t0: open(p, "w").write(t1); changed.append(label)

def uconst(t):
    if "// PS5 uc" in t: return t
    t = t.replace("#ifdef _ALLBSD_SOURCE\n#include <sys/attr.h>\n#include <sys/clonefile.h>\n#endif", "// PS5 uc\n#ifdef __APPLE__\n#include <sys/attr.h>\n#include <sys/clonefile.h>\n#endif", 1)
    t = t.replace("#ifdef _ALLBSD_SOURCE\n    // flags used with clonefile", "#ifdef __APPLE__\n    // flags used with clonefile", 1)
    t = t.replace("#else\n    // not supported (dummy values will not be used at runtime).\n    static final int PREFIX_ATTR_CMN_CRTIME = 00;",
                  "#else\n    // not supported (dummy values will not be used at runtime).\n    static final int PREFIX_CLONE_NOFOLLOW = 00;\n    static final int PREFIX_CLONE_NOOWNERCOPY = 00;\n    static final int PREFIX_ATTR_CMN_CRTIME = 00;", 1)
    return t
def _jbn(rel, fn, label):
    p = os.path.join(root, "src/java.base", rel)
    t0 = open(p).read(); t1 = fn(t0)
    if t1 != t0: open(p, "w").write(t1); changed.append(label)
_jbn("unix/classes/sun/nio/fs/UnixConstants.java.template", uconst, "UnixConstants.java.template")

# --- jdk21: with the OS presented as macos (see GensrcMisc above) ProcessImpl no longer refuses to start on "FreeBSD" the way 17 did, so Runtime.exec (oshi runs sysctl/uname this way) reaches the native
# forkAndExec -> startChild -> posix_spawn. posix_spawn is an unresolved import on this console: the call jumps through an empty slot to garbage (SIGSEGV with rip = a bogus address). Report "not supported" instead.
def _noexec(t):
    if "PS5: no posix_spawn" in t: return t
    old = "                                       jboolean redirectErrorStream)\n{\n"
    if old not in t: return t
    return t.replace(old, old + "    // PS5: no posix_spawn / fork+exec here (unresolved import); fail like a missing executable instead of crashing\n    throwIOException(env, ENOSYS, \"process creation is not supported on this platform\");\n    return -1;\n", 1)
_jbn("unix/native/libjava/ProcessImpl_md.c", _noexec, "ProcessImpl_md.c:no posix_spawn")

def bsdfs(t):
    if "// PS5 clone" in t: return t
    t = t.replace("            BsdNativeDispatcher.clonefile(src, dst, flags);", "            if (true) throw new UnixException(ENOTSUP);   // PS5 clone: no clonefile(2) here, copyFile then falls back to the superclass\n            BsdNativeDispatcher.clonefile(src, dst, flags);", 1)
    t = t.replace("        return directCopy0(dst, src, addressToPollForCancel);", "        return IOStatus.UNSUPPORTED;   // PS5: fcopyfile is macOS-only; UnixFileSystem copies with read/write", 1)
    return t
bsd_edit("classes/sun/nio/fs/BsdFileSystem.java", bsdfs, "bsd BsdFileSystem.java (no clonefile/fcopyfile)")

def bsdnd(t):
    if "// PS5 nodisp" in t: return t
    t = t.replace("#include <sys/attr.h>\n#include <unistd.h>", "// PS5 nodisp\n#ifdef __APPLE__\n#include <sys/attr.h>\n#endif\n#include <unistd.h>", 1)
    t = t.replace("#include <sys/attr.h>\n#include <sys/clonefile.h>\n", "#ifdef __APPLE__\n#include <sys/attr.h>\n#include <sys/clonefile.h>\n#endif\n", 1)
    k = "JNIEXPORT jint JNICALL\nJava_sun_nio_fs_BsdNativeDispatcher_clonefile0"
    i = t.index(k)
    stubs = ("#else   // PS5: clonefile(2) / setattrlist(2) do not exist here; the Java side expects ENOTSUP and falls back\n"
        "JNIEXPORT jint JNICALL\nJava_sun_nio_fs_BsdNativeDispatcher_clonefile0(JNIEnv* env, jclass this, jlong srcAddress, jlong dstAddress, jint flags)\n{ throwUnixException(env, ENOTSUP); return -1; }\n\n"
        "JNIEXPORT void JNICALL\nJava_sun_nio_fs_BsdNativeDispatcher_setattrlist0(JNIEnv* env, jclass this, jlong pathAddress, int commonattr, jlong modTime, jlong accTime, jlong createTime, jlong options)\n{ throwUnixException(env, ENOTSUP); }\n\n"
        "JNIEXPORT void JNICALL\nJava_sun_nio_fs_BsdNativeDispatcher_fsetattrlist0(JNIEnv* env, jclass this, jint fd, int commonattr, jlong modTime, jlong accTime, jlong createTime, jlong options)\n{ throwUnixException(env, ENOTSUP); }\n#endif\n")
    return t[:i] + "#ifdef __APPLE__\n" + t[i:].rstrip("\n") + "\n" + stubs
bsd_edit("native/libnio/fs/BsdNativeDispatcher.c", bsdnd, "bsd BsdNativeDispatcher.c (no clonefile/setattrlist)")

# --- java.base natives: BSD gaps that macOS hides behind MACOSX
def jb_native(rel, fn, label):
    p = os.path.join(root, "src/java.base", rel)
    t0 = open(p).read(); t1 = fn(t0)
    if t1 != t0: open(p, "w").write(t1); changed.append(label)

def tz(t):
    if "// PS5 tz" in t: return t
    k = "/*ARGSUSED1*/\nchar *\nfindJavaTZ_md"
    return t.replace(k, "// PS5 tz\n#if defined(_ALLBSD_SOURCE) && !defined(MACOSX) && !defined(__linux__)\nstatic char *getPlatformTimeZoneID(void) { return NULL; }\n#endif\n\n" + k, 1)
jb_native("unix/native/libjava/TimeZone_md.c", tz, "TimeZone_md.c")

# jdk21: UnixFileDispatcherImpl.c already maps statvfs64 for _ALLBSD_SOURCE (the 17 edit to FileDispatcherImpl.c is not needed)

# jdk21: unix/native/libnio/fs/UnixCopyFile.c no longer exists (the macOS copyfile edit is not needed)

def xattr(t):
    t = t.replace("#if defined(__linux__) || defined(_ALLBSD_SOURCE)\n#include <sys/xattr.h>", "#if defined(__linux__) || defined(MACOSX)\n#include <sys/xattr.h>")
    t = t.replace("#elif _ALLBSD_SOURCE\n    res = f", "#elif defined(MACOSX)\n    res = f")
    t = t.replace("#elif defined(_ALLBSD_SOURCE)\n    res = f", "#elif defined(MACOSX)\n    res = f")   # jdk21 spelling of the same guard (macOS xattr calls take two extra arguments)
    return t
jb_native("unix/native/libnio/fs/UnixNativeDispatcher.c", xattr, "UnixNativeDispatcher.c")

# --- no dlopen on the console: built-in "libraries" + a generated symbol table (see gen_symtab.sh)
DL = r'''// ps5_dl.cpp: stand-in for dlopen/dlsym. The JDK native libraries are linked into the title;
// symbols come from ps5_symtab[], generated at build time (sorted by name).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
struct Ps5Sym { const char *name; void *addr; };
extern const Ps5Sym ps5_symtab[] __attribute__((weak));
extern const int ps5_symtab_n __attribute__((weak));

void (*ps5_trace_hook)(const char *) = 0;
void ps5_trace(const char *m) { if (ps5_trace_hook) ps5_trace_hook(m); }
void ps5_trace_bytes(const char *s, size_t n) {
    if (!ps5_trace_hook || !s) return;
    char b[200];
    while (n) {
        size_t c = n < sizeof b - 1 ? n : sizeof b - 1;
        memcpy(b, s, c); b[c] = 0;
        s += c; n -= c;
        while (c && (b[c - 1] == 10 || b[c - 1] == 13)) b[--c] = 0;
        if (c) ps5_trace_hook(b);
    }
}

static const char *java_home = "/app0/assets/jdk";
void ps5_set_java_home(const char *p) { java_home = p; }

const char *ps5_jvm_path(void) {
    static char b[512];
    snprintf(b, sizeof b, "%s/lib/amd64/server/libjvm.so", java_home);   // BSD expects <home>/lib/<arch>/server/libjvm.so
    return b;
}
void *ps5_dll_handle(void) { return (void *)&java_home; }

void *ps5_dll_open(const char *fn) {
    if (!fn) return ps5_dll_handle();
    const char *b = strrchr(fn, '/'); b = b ? b + 1 : fn;
    static const char *const known[] = { "libjava.so", "libzip.so", "libnet.so", "libnio.so", "libjimage.so", "libverify.so", "libjsig.so", 0 };
    for (int i = 0; known[i]; i++) if (!strcmp(b, known[i])) return ps5_dll_handle();
    return 0;
}

static int cmp(const void *k, const void *e) { return strcmp((const char *)k, ((const Ps5Sym *)e)->name); }
void *ps5_dlsym(const char *name) {
    if (!&ps5_symtab[0] || !&ps5_symtab_n || !name) return 0;
    const Ps5Sym *r = (const Ps5Sym *)bsearch(name, ps5_symtab, (size_t)ps5_symtab_n, sizeof(Ps5Sym), cmp);
    return r ? r->addr : 0;
}
}
'''
dp = os.path.join(hs, "os/bsd/ps5_dl.cpp")
if not os.path.exists(dp) or open(dp).read() != DL:
    open(dp, "w").write(DL)
    changed.append("ps5_dl.cpp")

def hook_dl(s):
    if "ps5_dll_open" in s: return s
    s = s.replace('extern "C" bool ps5_release(char*, size_t);\n',
                  'extern "C" bool ps5_release(char*, size_t);\nextern "C" void* ps5_dll_open(const char*);\nextern "C" const char* ps5_jvm_path(void);\n', 1)
    s = s.replace("void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {\n",
                  "void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {\n"
                  "  { void* ps5h = ps5_dll_open(filename); if (ps5h) return ps5h;\n"
                  "    if (ebuf && ebuflen > 0) ::snprintf(ebuf, ebuflen, \"ps5: cannot load %s\", filename ? filename : \"(null)\");\n"
                  "    return NULL; }\n")
    s = s.replace("void os::jvm_path(char *buf, jint buflen) {\n",
                  "void os::jvm_path(char *buf, jint buflen) {\n  ::snprintf(buf, buflen, \"%s\", ps5_jvm_path());\n  return;\n", 1)
    return s
edit("os/bsd/os_bsd.cpp", hook_dl, "os_bsd.cpp:dl hooks")

def hook_posix(s):
    if "ps5_dll_handle" in s: return s
    s = s.replace("void* os::get_default_process_handle() {\n",
                  'extern "C" void* ps5_dll_handle(void);\nextern "C" void* ps5_dlsym(const char*);\nvoid* os::get_default_process_handle() {\n  return ps5_dll_handle();\n', 1)
    s = s.replace("void* os::dll_lookup(void* handle, const char* name) {\n",
                  "void* os::dll_lookup(void* handle, const char* name) {\n  return ps5_dlsym(name);\n", 1)
    return s
edit("os/posix/os_posix.cpp", hook_posix, "os_posix.cpp:dl hooks")

def _body_end(t, start):
    i = t.index("{", start); depth = 0
    while True:
        c = t[i]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0: return i
        i += 1

# --- the PS5 kernel kills any process that asks for a sysctl name outside its approved list (SIGSYS)
SYSCTL_STUBS = ('// PS5: sysctl is not approved for titles (SIGSYS). Everything fails softly instead.\n'
                'static inline int ps5_sysctl(const int*, unsigned, void*, size_t*, const void*, size_t) { return -1; }\n'
                'static inline int ps5_sysctlbyname(const char*, void*, size_t*, const void*, size_t) { return -1; }\n')

def _no_sysctl(s):
    import re
    if "ps5_sysctl(" in s and "static inline int ps5_sysctl" in s: return s
    s = re.sub(r"(?<![\w:])sysctlbyname\(", "ps5_sysctlbyname(", s)
    s = re.sub(r"(?<![\w:])sysctl\(", "ps5_sysctl(", s)
    i = s.index("// PS5 compat\n") if "// PS5 compat\n" in s else None
    if i is not None:
        j = i + len("// PS5 compat\n")
        return s[:j] + SYSCTL_STUBS + s[j:]
    k = s.index("#include <sys/sysctl.h>\njuint") if "#include <sys/sysctl.h>\njuint" in s else None
    if k is not None:
        j = k + len("#include <sys/sysctl.h>\n")
        return s[:j] + SYSCTL_STUBS + s[j:]
    raise SystemExit("no anchor for the sysctl stubs")

def _sysinfo(s):
    a = s.find("void os::Bsd::initialize_system_info() {")
    if a < 0 or "// PS5 sysinfo" in s[a:a+200]: return s
    b = _body_end(s, a)
    new = ('void os::Bsd::initialize_system_info() {\n  // PS5 sysinfo: no sysctl; sysconf is approved, memory comes from the direct-memory size\n'
           '  long n = sysconf(_SC_NPROCESSORS_ONLN);\n  set_processor_count(n >= 1 ? (int)n : 1);\n'
           '  _physical_memory = ps5_physical_memory();\n}')
    return s[:a] + new + s[b+1:]

def _decl_phys(s):
    if "ps5_physical_memory" in s.split("void os::Bsd::initialize_system_info")[0]: return s
    return s.replace('extern "C" char* ps5_reserve(char*, size_t, bool);\n', 'extern "C" char* ps5_reserve(char*, size_t, bool);\nextern "C" unsigned long long ps5_physical_memory(void);\n', 1)

edit("os/bsd/os_bsd.cpp", _decl_phys, "os_bsd.cpp:decl phys")
edit("os/bsd/os_bsd.cpp", _sysinfo, "os_bsd.cpp:sysinfo")
edit("os/bsd/os_bsd.cpp", _no_sysctl, "os_bsd.cpp:no sysctl")
edit("os_cpu/bsd_x86/os_bsd_x86.cpp", _no_sysctl, "os_bsd_x86.cpp:no sysctl")

# --- startup trace beacons: lets the host see how far JNI_CreateJavaVM gets before it dies
import re as _re
def _body_end(t, start):
    i = t.index("{", start); depth = 0
    while True:
        c = t[i]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0: return i
        i += 1

_KW = ("return", "break", "continue", "goto", "throw", "case", "default", "delete", "else", "typedef", "using", "do", "while", "for", "if", "switch")
def _instrument(sig, label):
    def f(t):
        k = t.find(sig)
        if k < 0 or "// PS5 TRACE " + label in t: return t
        end = _body_end(t, k)
        head, body, tail = t[:k], t[k:end], t[end:]
        lines = body.split("\n"); out = []; prev = ""
        first_brace_done = False
        for n, ln in enumerate(lines):
            out.append(ln)
            if not first_brace_done:
                if ln.rstrip().endswith("{"):
                    first_brace_done = True
                    out.append('  ps5_trace("%s: enter");  // PS5 TRACE %s' % (label, label))
                continue
            m = _re.match(r"^  ([A-Za-z_][^#{}]*);\s*$", ln)
            if (m and not prev.rstrip().endswith("\\") and not ln.strip().startswith(_KW)
                    and ln.count("(") == ln.count(")") and not ln.strip().endswith(":")):
                txt = _re.sub(r'["\\]', "", ln.strip())[:70]
                out.append('  ps5_trace("%s %s");' % (label, txt))
            prev = ln
        decl = 'extern "C" void ps5_trace(const char*);\n'
        return head + decl + "\n".join(out) + tail
    return f

edit("share/runtime/threads.cpp", _instrument("jint Threads::create_vm(JavaVMInitArgs* args, bool* canTryAgain) {", "create_vm"), "threads.cpp:trace")
edit("share/runtime/init.cpp", _instrument("jint init_globals() {", "init_globals"), "init.cpp:trace")
edit("os/bsd/os_bsd.cpp", _instrument("void os::init(void) {", "os_init"), "os_bsd.cpp:trace os::init")
edit("os/bsd/os_bsd.cpp", _instrument("jint os::init_2(void) {", "os_init_2"), "os_bsd.cpp:trace os::init_2")
edit("share/prims/jni.cpp", _instrument("static jint JNI_CreateJavaVM_inner(JavaVM **vm, void **penv, void *args) {", "jni_inner"), "jni.cpp:trace")
edit("share/runtime/arguments.cpp", _instrument("void Arguments::init_system_properties() {", "args_sysprops"), "arguments.cpp:trace sysprops")
edit("share/runtime/arguments.cpp", _instrument("jint Arguments::parse(const JavaVMInitArgs* initial_cmd_args) {", "args_parse"), "arguments.cpp:trace parse")
edit("share/runtime/arguments.cpp", _instrument("jint Arguments::apply_ergo() {", "apply_ergo"), "arguments.cpp:trace ergo")
edit("share/runtime/arguments.cpp", _instrument("void Arguments::set_heap_size() {", "set_heap_size"), "arguments.cpp:trace heap")
edit("os/bsd/os_bsd.cpp", _instrument("void os::init_system_properties_values() {", "os_sysprops"), "os_bsd.cpp:trace sysprops")

# --- report HotSpot's own fatal/exit reasons through the trace hook (stdout is not reachable on the console)
def _after_sig(sig, code, imports=""):
    def f(t):
        if "// PS5 ERR " + sig[:24] in t: return t
        k = t.find(sig)
        if k < 0: return t
        j = t.index("{", k) + 1
        return t[:j] + "  // PS5 ERR " + sig[:24] + "\n" + code + t[j:]
    return f

_DECL = 'extern "C" void ps5_trace(const char*);\n'
def _decl_once(s):
    return s if 'extern "C" void ps5_trace(const char*);' in s else _DECL + s

# java.cpp: exits during initialization + vm_abort
def _java(s):
    s = _after_sig("void vm_abort(bool dump_core) {", '  ps5_trace("vm_abort");\n')(s)
    s = _after_sig("void vm_exit_during_initialization(const char* error, const char* message) {",
                   '  ps5_trace("vm_exit_during_initialization:"); ps5_trace(error ? error : "(null)"); ps5_trace(message ? message : "(null)");\n')(s)
    s = _after_sig("void vm_exit_during_initialization(Symbol* ex, const char* message) {",
                   '  ps5_trace("vm_exit_during_initialization(sym):"); ps5_trace(ex ? ex->as_C_string() : "(null)"); ps5_trace(message ? message : "(null)");\n')(s)
    s = _after_sig("void vm_shutdown_during_initialization(const char* error, const char* message) {",
                   '  ps5_trace("vm_shutdown_during_initialization:"); ps5_trace(error ? error : "(null)"); ps5_trace(message ? message : "(null)");\n')(s)
    return _decl_once(s) if "ps5_trace(" in s else s
edit("share/runtime/java.cpp", _java, "java.cpp:err trace")

# debug.cpp: guarantee()/fatal() failures
def _debug(s):
    s = _after_sig("void report_vm_error(const char* file, int line, const char* error_msg)",
                   '  ps5_trace("report_vm_error:"); ps5_trace(file ? file : "?"); { char b[48]; snprintf(b, sizeof b, "line %d", line); ps5_trace(b); } ps5_trace(error_msg ? error_msg : "(null)");\n')(s)
    s = _after_sig("void report_vm_error(const char* file, int line, const char* error_msg, const char* detail_fmt, ...)",
                   '  ps5_trace("report_vm_error:"); ps5_trace(file ? file : "?"); { char b[48]; snprintf(b, sizeof b, "line %d", line); ps5_trace(b); } ps5_trace(error_msg ? error_msg : "(null)"); ps5_trace(detail_fmt ? detail_fmt : "(null)");\n')(s)
    s = _after_sig("void report_fatal(VMErrorType error_type, const char* file, int line, const char* detail_fmt, ...) {",
                   '  ps5_trace("report_fatal:"); ps5_trace(file ? file : "?"); { char b[48]; snprintf(b, sizeof b, "line %d", line); ps5_trace(b); } ps5_trace(detail_fmt ? detail_fmt : "(null)");\n')(s)
    return _decl_once(s) if "ps5_trace(" in s else s
edit("share/utilities/debug.cpp", _debug, "debug.cpp:err trace")

# os.cpp: what exactly does the boot-path check see?
def _bootpath(s):
    old = "  bool has_jimage = (os::stat(jimage, &st) == 0);\n"
    new = (old + '  { char b[220]; snprintf(b, sizeof b, "set_boot_path home=%s", home); ps5_trace(b);\n'
           '    snprintf(b, sizeof b, "set_boot_path jimage=%s stat=%d errno=%d", jimage, (int)has_jimage, errno); ps5_trace(b); }\n')
    if old not in s or "set_boot_path home=" in s: return s
    s = s.replace(old, new, 1)
    return _decl_once(s)
edit("share/runtime/os.cpp", _bootpath, "os.cpp:boot path trace")

# os::abort / exit
def _abort(s):
    return _decl_once(_after_sig("void os::abort(bool dump_core, void* siginfo, const void* context) {", '  ps5_trace(dump_core ? "os::abort(dump_core=true)" : "os::abort(dump_core=false)");\n')(s)) if "void os::abort(bool dump_core, void* siginfo, const void* context) {" in s else s
edit("os/posix/os_posix.cpp", _abort, "os_posix.cpp:abort trace")

# also trace the 4-space statements inside blocks of os::init_system_properties_values
def _instrument_deep(sig, label):
    def f(t):
        k = t.find(sig)
        if k < 0 or "// PS5 DEEP " + label in t: return t
        end = _body_end(t, k)
        head, body, tail = t[:k], t[k:end], t[end:]
        out = []; first = False
        for ln in body.split("\n"):
            out.append(ln)
            if not first:
                if ln.rstrip().endswith("{"):
                    first = True; out.append("  // PS5 DEEP " + label)
                continue
            m = _re.match(r"^    ([A-Za-z_*][^#{}]*);\s*$", ln)
            if m and not ln.strip().startswith(_KW) and ln.count("(") == ln.count(")"):
                txt = _re.sub(r'["\\]', "", ln.strip())[:60]
                out.append('    ps5_trace("%s> %s");' % (label, txt))
        return head + "\n".join(out) + tail
    return f
edit("os/bsd/os_bsd.cpp", _instrument_deep("void os::init_system_properties_values() {", "sysprops"), "os_bsd.cpp:deep trace")

# --- mirror HotSpot's console output (tty, hs_err reports, -Xlog) into the trace hook: fd 1/2 cannot be redirected on the console
def _tty(s):
    if "ps5_trace_bytes" in s: return s
    s = s.replace("void defaultStream::write(const char* s, size_t len) {\n", "void defaultStream::write(const char* s, size_t len) {\n  ps5_trace_bytes(s, len);\n", 1)
    s = s.replace("void fdStream::write(const char* s, size_t len) {\n", "void fdStream::write(const char* s, size_t len) {\n  ps5_trace_bytes(s, len);\n", 1)
    i = s.index('#include "')
    return s[:i] + '#include <stddef.h>\nextern "C" void ps5_trace_bytes(const char*, size_t);\n' + s[i:]
edit("share/utilities/ostream.cpp", _tty, "ostream.cpp:tty trace")

# --- the JDK libraries are linked into the title: dll_locate_lib must not require a file to exist
def _locate(s):
    old = "    struct stat statbuf;\n    return os::stat(buffer, &statbuf) == 0;\n  }\n  return false;\n}"
    new = "    // PS5: libjimage/libzip/... are linked into the title, so there is no file to find\n    return true;\n  }\n  return false;\n}"
    if "PS5: libjimage/libzip" in s or old not in s: return s
    return s.replace(old, new, 1)
edit("share/runtime/os.cpp", _locate, "os.cpp:dll_locate_lib")

# --- -Xlog output and jio_fprintf all funnel through jio_vfprintf: mirror them into the trace hook
def _jio(s):
    old = "int jio_vfprintf(FILE* f, const char *fmt, va_list args) {\n"
    if old not in s or "PS5 jio mirror" in s: return s
    new = (old + "  { // PS5 jio mirror\n    char b[600]; va_list c; va_copy(c, args);\n"
           "    int n = vsnprintf(b, sizeof b, fmt, c); va_end(c);\n"
           "    if (n > 0) ps5_trace_bytes(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1)); }\n")
    s = s.replace(old, new, 1)
    i = s.index('#include "')
    return s[:i] + '#include <stddef.h>\nextern "C" void ps5_trace_bytes(const char*, size_t);\n' + s[i:]
edit("share/prims/jvm.cpp", _jio, "jvm.cpp:jio mirror")

# --- realpath() is not usable in the title sandbox: our paths (/app0/...) are already canonical
def _canon(s):
    old = "    /* First try realpath() on the entire path */\n    if (realpath(orig, out)) {"
    new = ("    /* PS5: realpath() fails in the title sandbox; /app0 paths are already canonical */\n"
           "    strncpy(out, orig, (size_t)len - 1); out[len - 1] = '\\0';\n    collapse(out);\n    return 0;\n"
           "    if (realpath(orig, out)) {")
    if "PS5: realpath()" in s or old not in s: return s
    return s.replace(old, new, 1)
jb_native("unix/native/libjava/canonicalize_md.c", _canon, "canonicalize_md.c")

# --- more beacons around the first code-cache use (VM_Version stub), stub generation and heap creation
def _both(sig, label):
    f1 = _instrument(sig, label)
    f2 = _instrument_deep(sig, label + "_d")
    return lambda t: f2(f1(t))
edit("cpu/x86/vm_version_x86.cpp", _both("void VM_Version::initialize() {", "vmver_init"), "vm_version_x86.cpp:trace init")
edit("cpu/x86/vm_version_x86.cpp", _instrument("void VM_Version::get_processor_features() {", "vmver_features"), "vm_version_x86.cpp:trace features")
edit("share/code/codeBlob.cpp", _both("BufferBlob* BufferBlob::create(const char* name, uint buffer_size) {", "bufblob"), "codeBlob.cpp:trace")
# jdk25: StubRoutines::initialize_initial_stubs() is macro-generated now; no beacon there
edit("share/memory/universe.cpp", _instrument("jint universe_init() {", "universe_init"), "universe.cpp:trace init")
edit("share/memory/universe.cpp", _instrument("jint Universe::initialize_heap() {", "init_heap"), "universe.cpp:trace heap")
edit("share/runtime/vm_version.cpp", _instrument("void VM_Version_init() {", "vmver_top"), "vm_version.cpp:trace")

# --- the PS5 kernel's saved context starts 48 bytes later than <ucontext.h> says (rip is word 28 instead of word 22)
def _uc(s):
    old = "  ucontext_t* const uc = (ucontext_t*) ucVoid;\n  Thread* const t = Thread::current_or_null_safe();\n"
    new = ("  // PS5: the kernel's saved CPU context sits 48 bytes after where the SDK's ucontext_t puts uc_mcontext\n"
           "  ucontext_t* const uc = (ucontext_t*) ((char*) ucVoid + 48);\n  Thread* const t = Thread::current_or_null_safe();\n")
    if "PS5: the kernel's saved CPU context" in s or old not in s: return s
    return s.replace(old, new, 1)
edit("os/posix/signals_posix.cpp", _uc, "signals_posix.cpp:ucontext shift")

# --- imports that only exist in browser/system-only libraries (libScePosixForWebKit, libkernel_sys): they are NULL in a game process.
IMPORTS = r"""
// Replacements for libc functions the SDK only exports from libraries a game does not load (isatty crashed GetJavaProperties).
extern "C" {
int isatty(int) { errno = ENOTTY; return 0; }
mode_t umask(mode_t) { return 022; }
ssize_t readlink(const char *__restrict, char *__restrict, size_t) { errno = EINVAL; return -1; }
long pathconf(const char *, int) { return 255; }
int chown(const char *, uid_t, gid_t) { errno = EPERM; return -1; }
int fchown(int, uid_t, gid_t) { errno = EPERM; return -1; }
int lchown(const char *, uid_t, gid_t) { errno = EPERM; return -1; }
int fchdir(int) { errno = ENOSYS; return -1; }
int link(const char *, const char *) { errno = ENOSYS; return -1; }
int symlink(const char *__restrict, const char *__restrict) { errno = ENOSYS; return -1; }
int lutimes(const char *, const struct timeval *) { errno = ENOSYS; return -1; }
int mkstemp(char *) { errno = ENOSYS; return -1; }
pid_t fork(void) { errno = ENOSYS; return -1; }
pid_t vfork(void) { errno = ENOSYS; return -1; }
int statfs(const char *, struct statfs *) { errno = ENOSYS; return -1; }
int getfsstat(struct statfs *, long, int) { errno = ENOSYS; return -1; }
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **res) { if (res) *res = 0; return EAI_FAIL; }
int getnameinfo(const struct sockaddr *, socklen_t, char *, size_t, char *, size_t, int) { return EAI_FAIL; }
void freeaddrinfo(struct addrinfo *) {}
const char *gai_strerror(int) { return "name resolution is not available yet"; }
}
"""
def _imports(s):
    if "Replacements for libc functions the SDK only exports" in s: return s
    s = s.replace("#include <dirent.h>\n", "#include <dirent.h>\n#include <netdb.h>\n#include <sys/mount.h>\n#include <sys/socket.h>\n#include <sys/time.h>\n", 1)
    # append the definitions at the end of the support file
    return s.rstrip("\n") + "\n" + IMPORTS
edit("os/bsd/ps5_support.cpp", _imports, "ps5_support.cpp:missing imports")

# --- libSceLibcInternal functions whose internal state is not initialised in a title (getcwd jumped to garbage): replace them too
IMPORTS2 = r"""
// More replacements: getcwd crashed inside the console's libc, and sysctl names outside the approved list kill the process.
extern "C" {
char *getcwd(char *buf, size_t size) {
    static const char cwd[] = "/app0";
    if (!buf) { buf = (char *)malloc(size > sizeof cwd ? size : sizeof cwd); if (!buf) { errno = ENOMEM; return 0; } }
    else if (size < sizeof cwd) { errno = ERANGE; return 0; }
    memcpy(buf, cwd, sizeof cwd);
    return buf;
}
char *realpath(const char *__restrict path, char *__restrict out) {
    if (!path) { errno = EINVAL; return 0; }
    size_t n = strlen(path);
    if (!out) { out = (char *)malloc(n + 1); if (!out) { errno = ENOMEM; return 0; } }
    else if (n >= 1024) { errno = ENAMETOOLONG; return 0; }
    memcpy(out, path, n + 1);
    return out;
}
int gethostname(char *name, size_t len) { if (len < 4) { errno = ENAMETOOLONG; return -1; } memcpy(name, "ps5", 4); return 0; }
char *setlocale(int, const char *) { return (char *)"C"; }
void tzset(void) {}
int getrlimit(int, struct rlimit *r) { if (!r) { errno = EFAULT; return -1; } r->rlim_cur = 65536; r->rlim_max = 65536; return 0; }
int sysctlbyname(const char *, void *, size_t *, const void *, size_t) { errno = ENOENT; return -1; }
}
"""
def _imports2(s):
    if "More replacements: getcwd crashed" in s: return s
    s = s.replace("#include <sys/mount.h>\n", "#include <locale.h>\n#include <sys/mount.h>\n#include <sys/resource.h>\n#include <sys/sysctl.h>\n", 1)
    return s.rstrip("\n") + "\n" + IMPORTS2
edit("os/bsd/ps5_support.cpp", _imports2, "ps5_support.cpp:more imports")

# --- jdk25: UnixNativeDispatcher.c on BSD names openat/fstatat/fchmodat directly (21 looked openat up with dlsym, which is a stub here). The SDK only exports these three from libkernel_sys,
# which a game process does not load: the import slot stays empty and the first call jumps to garbage. SUPPORTS_OPENAT stays off (its dlsym("unlinkat") is NULL), so only Files.setPosixFilePermissions
# without following links reaches fchmodat; still, give all three a safe, path-based implementation (a real directory fd answers ENOSYS).
IMPORTS3 = r"""
// jdk25: openat/fstatat/fchmodat exist in the SDK only as libkernel_sys stubs (not loaded in a game). Path-based versions; a real directory descriptor is not supported.
extern "C" {
int openat(int fd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    if (fd == AT_FDCWD || (path && path[0] == '/')) return open(path, flags, mode);
    errno = ENOSYS; return -1;
}
int fstatat(int fd, const char *path, struct stat *st, int flag) {
    if (fd == AT_FDCWD || (path && path[0] == '/')) return (flag & AT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
    errno = ENOSYS; return -1;
}
int fchmodat(int fd, const char *path, mode_t mode, int flag) {
    (void)flag;
    if (fd == AT_FDCWD || (path && path[0] == '/')) return chmod(path, mode);
    errno = ENOSYS; return -1;
}
// futimens/utimensat: the SDK has them only in its static libc.a, which the title does not link; futimes/utimes (libkernel) are what 21 used
static void ts2tv(const struct timespec *t, struct timeval *tv) { for (int i = 0; i < 2; i++) { tv[i].tv_sec = t[i].tv_sec; tv[i].tv_usec = t[i].tv_nsec / 1000; } }
int futimens(int fd, const struct timespec *t) {
    if (!t) return futimes(fd, 0);
    struct timeval tv[2]; ts2tv(t, tv); return futimes(fd, tv);
}
int utimensat(int fd, const char *path, const struct timespec *t, int flag) {
    (void)flag;
    if (fd != AT_FDCWD && !(path && path[0] == '/')) { errno = ENOSYS; return -1; }
    if (!t) return utimes(path, 0);
    struct timeval tv[2]; ts2tv(t, tv); return utimes(path, tv);
}
}
"""
def _imports3(s):
    if "futimens/utimensat: the SDK has them" in s: return s
    if "jdk25: openat/fstatat/fchmodat" in s:
        i = s.index("\n// jdk25: openat/fstatat/fchmodat"); s = s[:i].rstrip("\n") + "\n"
    s = s.replace("#include <errno.h>\n", "#include <errno.h>\n#include <stdarg.h>\n", 1)
    return s.rstrip("\n") + "\n" + IMPORTS3
edit("os/bsd/ps5_support.cpp", _imports3, "ps5_support.cpp:openat/fstatat/fchmodat")

# --- run HotSpot's error-signal handlers on the alternate stack (a native stack overflow otherwise kills the process with no report)
def _onstack(s):
    old = "  sigAct->sa_flags = SA_SIGINFO|SA_RESTART;\n#if defined(__APPLE__)"
    new = "  sigAct->sa_flags = SA_SIGINFO|SA_RESTART|SA_ONSTACK;   // PS5: always use the alternate stack\n#if defined(__APPLE__)"
    if "PS5: always use the alternate stack" in s or old not in s: return s
    return s.replace(old, new, 1)
edit("os/posix/signals_posix.cpp", _onstack, "signals_posix.cpp:SA_ONSTACK")


# --- jdk.management (needed for com.sun.management.HotSpotDiagnosticMXBean: Iris reads the UseFMA flag in a static initialiser and the class was missing from the image).
# The unix OperatingSystemImpl.c needs Linux /proc, macOS mach/sysctl or AIX perfstat details that this target does not have, so it is replaced by a file whose natives report
# "not available": the OS statistics bean is not used by anything here, while Flag.c / HotSpotDiagnostic.c / management_ext.c (the JVM flag queries) build as they are.
OSIMPL = r"""/* PS5 stand-in: statistics of com.sun.management.internal.OperatingSystemImpl are not available on this platform (see ps5_jdk_patch21.py). */
#include <jni.h>
#include "com_sun_management_internal_OperatingSystemImpl.h"

JNIEXPORT void JNICALL Java_com_sun_management_internal_OperatingSystemImpl_initialize0(JNIEnv *env, jclass cls) { }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getCommittedVirtualMemorySize0(JNIEnv *env, jobject mbean) { return -1; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getFreeMemorySize0(JNIEnv *env, jobject mbean) { return -1; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getFreeSwapSpaceSize0(JNIEnv *env, jobject mbean) { return 0; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getMaxFileDescriptorCount0(JNIEnv *env, jobject mbean) { return -1; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getOpenFileDescriptorCount0(JNIEnv *env, jobject mbean) { return -1; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getProcessCpuTime0(JNIEnv *env, jobject mbean) { return -1; }
JNIEXPORT jdouble JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getProcessCpuLoad0(JNIEnv *env, jobject mbean) { return -1.0; }
JNIEXPORT jdouble JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getCpuLoad0(JNIEnv *env, jobject mbean) { return -1.0; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getTotalMemorySize0(JNIEnv *env, jobject mbean) { return -1; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getTotalSwapSpaceSize0(JNIEnv *env, jobject mbean) { return 0; }
JNIEXPORT jdouble JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getSingleCpuLoad0(JNIEnv *env, jobject mbean, jint cpuNum) { return -1.0; }
JNIEXPORT jint JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getHostConfiguredCpuCount0(JNIEnv *env, jobject mbean) { return 4; }
JNIEXPORT jint JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getHostOnlineCpuCount0(JNIEnv *env, jobject mbean) { return 4; }
JNIEXPORT jlong JNICALL Java_com_sun_management_internal_OperatingSystemImpl_getHostTotalCpuTicks0(JNIEnv *env, jobject mbean) { return -1; }
"""
_osp = os.path.join(root, "src/jdk.management/unix/native/libmanagement_ext/OperatingSystemImpl.c")
if os.path.isfile(_osp) and "PS5 stand-in" not in open(_osp).read():
    open(_osp, "w").write(OSIMPL); changed.append("jdk.management OperatingSystemImpl.c stand-in")

# --- JDK 24+ removed the SecurityManager, which is how McBoot turned System.exit into "game over, back to the launcher" (17/21). Without it System.exit starts the VM shutdown, which this title cannot end
# or restart: the render thread parks in Runtime.exit and the GL context is never released (frozen app). Same effect through a hook: Runtime.ps5ExitHook (public static, set from McBoot by reflection)
# is called first thing in Shutdown.exit (System.exit -> Runtime.exit -> Shutdown.exit; also SIGTERM/SIGINT); a hook that throws unwinds the caller, exactly like the old SecurityManager.checkExit.
def _jlang(name, fn, label):
    p = os.path.join(root, "src/java.base/share/classes/java/lang", name)
    t0 = open(p).read(); t1 = fn(t0)
    if t1 == t0 and "ps5ExitHook" not in t0:
        for i, ln in enumerate(t0.split("\n"), 1):
            if re.search(r"\bclass\s+(Runtime|Shutdown)\b|\bvoid\s+exit\s*\(", ln): print("  %s:%d: %s" % (name, i, ln.rstrip()))
        raise SystemExit("PS5 exit hook: no anchor in " + name + " (the JDK source changed): fix ps5_jdk_patch25.py before building (lines above are what the source has)")
    if t1 != t0: open(p, "w").write(t1); changed.append(label)
def _rt(t):
    if "ps5ExitHook" in t: return t
    return re.sub(r"(^[ \t]*(?:public\s+)?(?:final\s+)?class\s+Runtime\b[^{;]*\{[ \t]*\n)", r"\1    /** PS5: called by Shutdown.exit before anything else (null = normal exit); may throw to stop the exit. Set by the title's launcher (McBoot) by reflection. */\n"
                  r"    public static volatile java.util.function.IntConsumer ps5ExitHook;\n", t, count=1, flags=re.M)
def _sd(t):
    if "ps5ExitHook" in t: return t
    return re.sub(r"(^[ \t]*static\s+void\s+exit\s*\(\s*int\s+status\s*\)\s*\{[ \t]*\n)", r"\1        { java.util.function.IntConsumer ps5h = Runtime.ps5ExitHook; if (ps5h != null) ps5h.accept(status); }   // PS5 exit hook (see ps5_jdk_patch25.py)\n", t, count=1, flags=re.M)
_jlang("Runtime.java", _rt, "Runtime.ps5ExitHook")
_jlang("Shutdown.java", _sd, "Shutdown.exit hook")


# --- hidden classes (lambda proxies, LambdaForms) each got their own ClassLoaderData with its own Mutex (a pthread mutex + condvar): Minecraft makes ~9,000 of them in the first minute and the console's libc
# has a fixed pool of pthread objects (~9,800). When it is full pthread_mutex_init / pthread_attr_init fail with ENOMEM and the VM dies. All of them now share ONE metaspace allocation lock (it is a leaf lock that is only
# held for a single metaspace allocation, so sharing it only serialises those allocations). Regular class loaders keep their own lock.
def _cld_share(s):
    if "PS5: hidden classes share one metaspace lock" in s: return s
    ctor = "ClassLoaderData::ClassLoaderData(Handle h_class_loader, bool has_class_mirror_holder) :"
    if ctor not in s: return s
    m = re.search(r"  _metaspace_lock\(new Mutex\((.*?)\)\),\n", s, re.S)
    if not m: return s
    args = m.group(1)
    helper = ("// PS5: hidden classes share one metaspace lock (see ps5_jdk_patch): the console's libc has a fixed pool of pthread objects.\n"
              "static Mutex* ps5_hidden_metaspace_lock() {\n"
              "  static Mutex* volatile lock = nullptr;\n"
              "  Mutex* m = Atomic::load_acquire(&lock);\n"
              "  if (m == nullptr) {\n"
              "    Mutex* n = new Mutex(" + args + ");\n"
              "    m = Atomic::cmpxchg(&lock, (Mutex*)nullptr, n);\n"
              "    if (m == nullptr) { m = n; } else { delete n; }\n"
              "  }\n"
              "  return m;\n"
              "}\n\n")
    s = s.replace(ctor, helper + ctor, 1)
    s = s.replace(m.group(0), "  _metaspace_lock(has_class_mirror_holder ? ps5_hidden_metaspace_lock() : new Mutex(" + args + ")),\n", 1)
    s = s.replace("  delete _metaspace_lock;\n", "  if (!_has_class_mirror_holder) { delete _metaspace_lock; }   // PS5: hidden classes share one\n", 1)
    return s
edit("share/classfile/classLoaderData.cpp", _cld_share, "classLoaderData.cpp:hidden classes share one metaspace lock")

print("patched:", ", ".join(changed) if changed else "nothing (already applied)")
