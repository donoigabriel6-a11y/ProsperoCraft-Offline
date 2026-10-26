#!/usr/bin/env python3
"""Apply the PS5 port to a jdk8u checkout (jdk8u504-ga). Safe to run twice; fails loudly when an anchor is missing.
usage: python3 ps5_jdk_patch8.py ~/jdk8u
JDK 8 differs from 17/21/25: HotSpot lives in hotspot/src/{os,os_cpu,share}/.../vm, the class library in jdk/src/{share,solaris,bsd,macosx}, no modules/jlink (rt.jar)."""
import os, re, sys

root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/jdk8u")
hs = os.path.join(root, "hotspot/src")
changed = []

def rd(p): return open(p, encoding="utf-8", errors="surrogateescape").read()
def wr(p, s): open(p, "w", encoding="utf-8", errors="surrogateescape").write(s)

def edit(base, rel, fn, label):
    p = os.path.join(base, rel)
    s = rd(p); t = fn(s)
    if t != s:
        wr(p, t); changed.append(label)

def must_replace(s, old, new, label, marker=None, count=1):
    """replace old by new once; no-op when marker (or new) is already there; abort when the anchor is missing"""
    if (marker or new) in s: return s
    if old not in s: raise SystemExit("ps5_jdk_patch8: anchor missing for '%s'" % label)
    return s.replace(old, new, count)

def hs_edit(rel, fn, label): edit(hs, rel, fn, label)
def jdk_edit(rel, fn, label): edit(os.path.join(root, "jdk/src"), rel, fn, label)

def write_file(p, content, label):
    if not os.path.exists(p) or rd(p) != content:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        wr(p, content); changed.append(label)

# --- configure: JDK 8 refuses a 64-bit client VM; C1-only is exactly what the console build wants (hotspot/make/Makefile supports it: make compiler1)
def cfg(s):
    old = 'as_fn_error $? "You cannot build a client JVM for a 64-bit machine." "$LINENO" 5'
    return must_replace(s, old, ': # PS5: 64-bit client (C1-only) VM allowed', "configure client check", marker=": # PS5: 64-bit client")
edit(root, "common/autoconf/generated-configure.sh", cfg, "generated-configure.sh:64-bit client")

# --- hotspot/make/Makefile: generic_build1 (compiler1 = client VM) only runs for ARCH_DATA_MODEL=32; the 64-bit C1 sources are the same ones (x86 c1_* files) and build fine
def mk1(s):
    old = "else\n  ifeq ($(ARCH_DATA_MODEL), 32)\n\t$(CD) $(OUTPUTDIR); \\\n\t    $(MAKE) -f $(ABS_OS_MAKEFILE) \\\n\t\t      $(MAKE_ARGS) $(VM_TARGET)\n  else\n\t@$(ECHO) \"No compiler1"
    new = "else\n  ifneq ($(ARCH_DATA_MODEL), 0)   # PS5: 64-bit C1 allowed\n\t$(CD) $(OUTPUTDIR); \\\n\t    $(MAKE) -f $(ABS_OS_MAKEFILE) \\\n\t\t      $(MAKE_ARGS) $(VM_TARGET)\n  else\n\t@$(ECHO) \"No compiler1"
    return must_replace(s, old, new, "hotspot Makefile generic_build1", marker="PS5: 64-bit C1 allowed")
edit(root, "hotspot/make/Makefile", mk1, "hotspot/make/Makefile:64-bit C1")

# ============================================================================================================================================
# HotSpot 8 (hotspot/src/{os/bsd/vm, os_cpu/bsd_x86/vm, share/vm}). Support code (libc stand-ins, memory, dlopen) is shared with the 17 patch:
# the C++ sources are taken from ps5_jdk_patch.py (one copy of the truth), only the hooks into HotSpot differ.
# ============================================================================================================================================
HERE = os.path.dirname(os.path.abspath(__file__))
SRC17 = rd(os.path.join(HERE, "ps5_jdk_patch.py"))
def grab(name):
    m = re.search(r"^%s = r(\'\'\'|\"\"\")(.*?)\1\n" % name, SRC17, re.S | re.M)
    if not m: raise SystemExit("ps5_jdk_patch8: cannot extract %s from ps5_jdk_patch.py" % name)
    return m.group(2)
SUPPORT, MEM, DL, IMPORTS, IMPORTS2 = [grab(n) for n in ("SUPPORT", "MEM", "DL", "IMPORTS", "IMPORTS2")]

# the 17 patch adds these includes while appending IMPORTS/IMPORTS2
SUPPORT8 = SUPPORT.replace("#include <dirent.h>\n", "#include <dirent.h>\n#include <netdb.h>\n#include <sys/mount.h>\n#include <sys/socket.h>\n#include <sys/time.h>\n", 1)
SUPPORT8 = SUPPORT8.rstrip("\n") + "\n" + IMPORTS
SUPPORT8 = SUPPORT8.replace("#include <sys/mount.h>\n", "#include <locale.h>\n#include <sys/mount.h>\n#include <sys/resource.h>\n#include <sys/sysctl.h>\n", 1)
SUPPORT8 = SUPPORT8.rstrip("\n") + "\n" + IMPORTS2
# SysV shared memory (os::reserve_memory_special, large pages) does not exist on the console: UseLargePages is off, these only keep the link closed
SUPPORT8 += """
#include <sys/ipc.h>
#include <sys/shm.h>
extern "C" {
int shmget(key_t, size_t, int) { errno = ENOSYS; return -1; }
void *shmat(int, const void *, int) { errno = ENOSYS; return (void *)-1; }
int shmctl(int, int, struct shmid_ds *) { errno = ENOSYS; return -1; }
int shmdt(const void *) { errno = ENOSYS; return -1; }
}
"""

osbsd = "os/bsd/vm"
write_file(os.path.join(hs, osbsd, "ps5_support.cpp"), SUPPORT8, "ps5_support.cpp")
MEM8 = MEM.replace('extern "C" char *ps5_reserve(char *hint, size_t bytes, bool exec) {', 'extern "C" int ps5_reserve_exec = 0;   // set by ReservedSpace::initialize (JDK 8 does not pass `exec` to reserve_memory)\nextern "C" char *ps5_reserve(char *hint, size_t bytes, bool exec) {', 1)
if MEM8 == MEM: raise SystemExit("ps5_jdk_patch8: ps5_reserve anchor missing in MEM")
# JDK 8's Metaspace grows in 2 MB virtual space nodes: below DIRECT_MIN they are plain mmap(), i.e. the console's small "flexible" memory budget (45 MB free late in a session in the kmem probe);
# when it ran out, reserve returned NULL and loading Minecraft 1.16.5 crashed. 2 MB and up go to direct memory like JDK 17's 4 MB metaspace chunks do.
assert "const size_t DIRECT_MIN = 4u << 20;" in MEM8, "ps5_jdk_patch8: DIRECT_MIN anchor missing in MEM"
MEM8 = MEM8.replace("const size_t DIRECT_MIN = 4u << 20;", "const size_t DIRECT_MIN = 2u << 20;", 1)
# HotSpot 8 compiles with -std=gnu++98: no brace initialisation of aggregates
MEM8 = re.sub(r"Resv\{([^}]*)\}", r"mkresv(\1)", MEM8)
MEM8 = re.sub(r"Run\{([^}]*)\}", r"mkrun(\1)", MEM8)
_anchor = "Resv rv[512]; int nrv;"
if _anchor not in MEM8: raise SystemExit("ps5_jdk_patch8: Resv anchor missing in MEM")
MEM8 = MEM8.replace(_anchor, "Resv mkresv(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, int e) { Resv r; r.base = a; r.len = b; r.obase = c; r.olen = d; r.direct = e; return r; }\n"
                    "Run mkrun(uintptr_t a, uintptr_t b, int64_t c) { Run r; r.base = a; r.len = b; r.phys = c; return r; }\n" + _anchor, 1)
write_file(os.path.join(hs, osbsd, "ps5_mem.cpp"), MEM8, "ps5_mem.cpp")
write_file(os.path.join(hs, osbsd, "ps5_dl.cpp"), DL, "ps5_dl.cpp")
write_file(os.path.join(hs, osbsd, "alloca.h"), "#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca(n) __builtin_alloca(n)\n#endif\n", "alloca.h")

def body_end(t, start):
    i = t.index("{", start); depth = 0
    while True:
        c = t[i]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0: return i
        i += 1

# --- os_bsd.cpp: compile fixes
def os_bsd_fixes(s):
    # sem_t is a struct on the console: no integer initialiser
    s = must_replace(s, "Semaphore::Semaphore() : _semaphore(0) {\n  SEM_INIT(_semaphore, 0);", "Semaphore::Semaphore() {   // PS5: sem_t is a struct\n  SEM_INIT(_semaphore, 0);", "Semaphore ctor")
    # the SDK has no MAP_NORESERVE
    s = must_replace(s, "# include <sys/sysctl.h>\n", "# include <sys/sysctl.h>\n// PS5 compat\n#ifndef MAP_NORESERVE\n#define MAP_NORESERVE 0\n#endif\n#ifndef HW_CPU_FREQ\n#define HW_CPU_FREQ 0\n#endif\n", "MAP_NORESERVE", marker="// PS5 compat")
    return s
hs_edit(osbsd + "/os_bsd.cpp", os_bsd_fixes, "os_bsd.cpp:compile fixes")

# --- os_perf_bsd.cpp: macOS-only network statistics
def perf8(s):
    key = "int NetworkPerformanceInterface::NetworkPerformance::network_utilization(NetworkInterface** network_interfaces) const {"
    i = s.find(key)
    if i < 0: raise SystemExit("ps5_jdk_patch8: network_utilization anchor missing")
    if "// PS5 stub" in s[i:i+200]: return s
    j = s.index("{", i); k = body_end(s, i)
    return s[:j+1] + "\n  // PS5 stub\n  *network_interfaces = NULL;\n  return OS_ERR;\n}" + s[k+1:]
hs_edit(osbsd + "/os_perf_bsd.cpp", perf8, "os_perf_bsd.cpp")

# --- memory: big reservations = VA reserve + direct-memory commit; executable ones = plain mmap. JDK 8 does not pass `exec` to reserve_memory: ReservedSpace::initialize tells us.
def mem_hooks(s):
    if "ps5_reserve" in s: return s
    s = must_replace(s, "// PS5 compat\n", "// PS5 compat\nextern \"C\" char* ps5_reserve(char*, size_t, bool);\nextern \"C\" bool ps5_commit(char*, size_t, bool);\nextern \"C\" bool ps5_release(char*, size_t);\nextern \"C\" unsigned long long ps5_physical_memory(void);\nextern \"C\" void* ps5_dll_open(const char*);\nextern \"C\" const char* ps5_jvm_path(void);\nextern \"C\" int ps5_reserve_exec;\n", "mem decls")
    s = must_replace(s, "char* os::pd_reserve_memory(size_t bytes, char* requested_addr,\n                         size_t alignment_hint) {\n", "char* os::pd_reserve_memory(size_t bytes, char* requested_addr,\n                         size_t alignment_hint) {\n  return ps5_reserve(requested_addr, bytes, ps5_reserve_exec != 0);\n", "pd_reserve_memory")
    s = must_replace(s, "bool os::pd_release_memory(char* addr, size_t size) {\n  return anon_munmap(addr, size);\n}", "bool os::pd_release_memory(char* addr, size_t size) {\n  return ps5_release(addr, size);\n}", "pd_release_memory")
    s = must_replace(s, "bool os::pd_commit_memory(char* addr, size_t size, bool exec) {\n", "bool os::pd_commit_memory(char* addr, size_t size, bool exec) {\n  return ps5_commit(addr, size, exec);\n", "pd_commit_memory")
    s = must_replace(s, "bool os::pd_uncommit_memory(char* addr, size_t size) {\n", "bool os::pd_uncommit_memory(char* addr, size_t size) {\n  return true;  // PS5: committed direct memory stays mapped\n", "pd_uncommit_memory")
    s = must_replace(s, "char* os::pd_attempt_reserve_memory_at(size_t bytes, char* requested_addr) {\n", "char* os::pd_attempt_reserve_memory_at(size_t bytes, char* requested_addr) {\n  return ps5_reserve(requested_addr, bytes, ps5_reserve_exec != 0);\n", "pd_attempt_reserve_memory_at")
    return s
hs_edit(osbsd + "/os_bsd.cpp", mem_hooks, "os_bsd.cpp:memory hooks")

def virtualspace(s):
    if "ps5_reserve_exec" in s: return s
    old = "                               bool executable) {\n"
    i = s.find("void ReservedSpace::initialize(size_t size, size_t alignment, bool large,")
    if i < 0: raise SystemExit("ps5_jdk_patch8: ReservedSpace::initialize anchor missing")
    j = s.index("{", i)
    scope = ('\n  // PS5: os::pd_reserve_memory cannot see `executable`; executable reservations must be plain mmap (direct memory refuses PROT_EXEC)\n'
             '  struct Ps5ExecScope { Ps5ExecScope(bool e) { ps5_reserve_exec = e ? 1 : 0; } ~Ps5ExecScope() { ps5_reserve_exec = 0; } } ps5_exec_scope(executable);\n')
    s = s[:j+1] + scope + s[j+1:]
    k = s.index('#include "')
    return s[:k] + 'extern "C" int ps5_reserve_exec;\n' + s[k:]
hs_edit("share/vm/runtime/virtualspace.cpp", virtualspace, "virtualspace.cpp:exec scope")

# --- dlopen / dlsym stand-ins, jvm_path
def dl_hooks(s):
    if "// PS5 dll_load non-Apple" in s: return s
    # two definitions of dll_load (Apple / other); patch both
    if "ps5_dll_open(filename)" not in s: s = s.replace("void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {\n",
                  "void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {\n"
                  "  { void* ps5h = ps5_dll_open(filename); if (ps5h) return ps5h;\n"
                  "    if (ebuf && ebuflen > 0) ::snprintf(ebuf, ebuflen, \"ps5: cannot load %s\", filename ? filename : \"(null)\");\n"
                  "    return NULL; }\n")
    s = must_replace(s, "void * os::dll_load(const char *filename, char *ebuf, int ebuflen)\n{\n",
                     "void * os::dll_load(const char *filename, char *ebuf, int ebuflen)\n{\n"
                     "  { void* ps5h = ps5_dll_open(filename); if (ps5h) return ps5h;\n"
                     "    if (ebuf && ebuflen > 0) ::snprintf(ebuf, ebuflen, \"ps5: cannot load %s\", filename ? filename : \"(null)\");\n"
                     "    return NULL; }  // PS5 dll_load non-Apple\n", "dll_load (non-Apple)", marker="// PS5 dll_load non-Apple")
    s = must_replace(s, "void os::jvm_path(char *buf, jint buflen) {\n", "void os::jvm_path(char *buf, jint buflen) {\n  ::snprintf(buf, buflen, \"%s\", ps5_jvm_path());\n  return;\n", "jvm_path")
    s = must_replace(s, "void* os::get_default_process_handle() {\n", 'extern "C" void* ps5_dll_handle(void);\nextern "C" void* ps5_dlsym(const char*);\nvoid* os::get_default_process_handle() {\n  return ps5_dll_handle();\n', "default process handle")
    s = must_replace(s, "void* os::dll_lookup(void* handle, const char* name) {\n", "void* os::dll_lookup(void* handle, const char* name) {\n  return ps5_dlsym(name);\n", "dll_lookup")
    return s
hs_edit(osbsd + "/os_bsd.cpp", dl_hooks, "os_bsd.cpp:dl hooks")

# --- the PS5 kernel kills any process that asks for a sysctl name outside its approved list (SIGSYS)
SYSCTL_STUBS = ('// PS5: sysctl is not approved for titles (SIGSYS). Everything fails softly instead.\n'
                'static inline int ps5_sysctl(const int*, unsigned, void*, size_t*, const void*, size_t) { return -1; }\n'
                'static inline int ps5_sysctlbyname(const char*, void*, size_t*, const void*, size_t) { return -1; }\n')
def no_sysctl(s):
    if "static inline int ps5_sysctl" in s: return s
    s = re.sub(r"(?<![\w:])sysctlbyname\(", "ps5_sysctlbyname(", s)
    s = re.sub(r"(?<![\w:])sysctl\(", "ps5_sysctl(", s)
    if "// PS5 compat\n" in s:
        j = s.index("// PS5 compat\n") + len("// PS5 compat\n")
        return s[:j] + SYSCTL_STUBS + s[j:]
    k = s.index("# include <sys/sysctl.h>\n") + len("# include <sys/sysctl.h>\n")
    return s[:k] + SYSCTL_STUBS + s[k:]
def sysinfo(s):
    a = s.find("void os::Bsd::initialize_system_info() {")
    if a < 0: raise SystemExit("ps5_jdk_patch8: initialize_system_info anchor missing")
    if "// PS5 sysinfo" in s[a:a+200]: return s
    b = body_end(s, a)
    new = ('void os::Bsd::initialize_system_info() {\n  // PS5 sysinfo: no sysctl; sysconf is approved, memory comes from the direct-memory size\n'
           '  long n = sysconf(_SC_NPROCESSORS_ONLN);\n  set_processor_count(n >= 1 ? (int)n : 1);\n'
           '  _physical_memory = ps5_physical_memory();\n}')
    return s[:a] + new + s[b+1:]
hs_edit(osbsd + "/os_bsd.cpp", sysinfo, "os_bsd.cpp:sysinfo")
hs_edit(osbsd + "/os_bsd.cpp", no_sysctl, "os_bsd.cpp:no sysctl")
def no_sysctl_x86(s):
    if "static inline int ps5_sysctl" in s: return s
    if "sysctl" not in s: return s
    s = re.sub(r"(?<![\w:])sysctlbyname\(", "ps5_sysctlbyname(", s)
    s = re.sub(r"(?<![\w:])sysctl\(", "ps5_sysctl(", s)
    k = s.index("# include <sys/sysctl.h>\n") + len("# include <sys/sysctl.h>\n")
    return s[:k] + SYSCTL_STUBS + s[k:]
hs_edit("os_cpu/bsd_x86/vm/os_bsd_x86.cpp", no_sysctl_x86, "os_bsd_x86.cpp:no sysctl")

# --- the PS5 kernel's saved context starts 48 bytes later than <ucontext.h> says
def uc(s):
    old = "                        int abort_if_unrecognized) {\n  ucontext_t* uc = (ucontext_t*) ucVoid;\n"
    new = ("                        int abort_if_unrecognized) {\n  // PS5: the kernel's saved CPU context sits 48 bytes after where the SDK's ucontext_t puts uc_mcontext\n"
           "  ucontext_t* uc = (ucontext_t*) ((char*) ucVoid + 48);\n")
    return must_replace(s, old, new, "ucontext shift", marker="PS5: the kernel's saved CPU context")
hs_edit("os_cpu/bsd_x86/vm/os_bsd_x86.cpp", uc, "os_bsd_x86.cpp:ucontext shift")

# --- run HotSpot's error-signal handlers on the alternate stack
def onstack(s):
    if "PS5: always use the alternate stack" in s: return s
    old = "  if (!set_installed) {\n    sigAct.sa_flags = SA_SIGINFO|SA_RESTART;\n  } else {\n    sigAct.sa_sigaction = signalHandler;\n    sigAct.sa_flags = SA_SIGINFO|SA_RESTART;\n  }"
    new = "  if (!set_installed) {\n    sigAct.sa_flags = SA_SIGINFO|SA_RESTART;\n  } else {\n    sigAct.sa_sigaction = signalHandler;\n    sigAct.sa_flags = SA_SIGINFO|SA_RESTART|SA_ONSTACK;   // PS5: always use the alternate stack\n  }"
    return must_replace(s, old, new, "SA_ONSTACK")
hs_edit(osbsd + "/os_bsd.cpp", onstack, "os_bsd.cpp:SA_ONSTACK")

# --- hotspot/make/bsd/makefiles/vm.make: no linker mapfile (the BSD one is a Mach-O export list), no serviceability agent, and libjvm.so may carry duplicates of SDK libc symbols
# (only the object files go into the title; libjvm.so merely has to link so that `make hotspot` runs its export step)
def vmmake(s):
    if "# PS5: no mapfile" in s: return s
    s = must_replace(s, "ifeq ($(LINK_INTO),AOUT)\n  LIBJVM.o                 =\n", "# PS5: no mapfile\nLDNOMAP = true\nifeq ($(LINK_INTO),AOUT)\n  LIBJVM.o                 =\n", "vm.make LDNOMAP", marker="# PS5: no mapfile")
    s = must_replace(s, "  LFLAGS_VM                += $(SONAMEFLAG:SONAME=$(LIBJVM))\n", "  LFLAGS_VM                += $(SONAMEFLAG:SONAME=$(LIBJVM))\n  LFLAGS_VM                += -Wl,--allow-multiple-definition   # PS5\n", "vm.make multiple definitions", marker="-Wl,--allow-multiple-definition   # PS5")
    s = must_replace(s, "build: $(LIBJVM) $(LAUNCHER) $(LIBJSIG) $(LIBJVM_DB) $(BUILDLIBSAPROC)\n", "build: $(LIBJVM) $(LAUNCHER) $(LIBJSIG) $(LIBJVM_DB)   # PS5: no libsaproc\n", "vm.make build", marker="# PS5: no libsaproc")
    s = must_replace(s, "install: install_jvm install_jsig install_saproc\n", "install: install_jvm install_jsig   # PS5: no libsaproc\n", "vm.make install", marker="install_jsig   # PS5")
    return s
edit(root, "hotspot/make/bsd/makefiles/vm.make", vmmake, "vm.make:no mapfile/saproc")

# ============================================================================================================================================
# Class library (jdk/src/{share,solaris,bsd}). JDK 8 has real BSD support in the build system; only PS5/SDK gaps remain.
# ============================================================================================================================================
# java.security-<os> is picked by OS name: there is no -bsd file
sec = os.path.join(root, "jdk/src/share/lib/security")
if not os.path.exists(os.path.join(sec, "java.security-bsd")):
    wr(os.path.join(sec, "java.security-bsd"), rd(os.path.join(sec, "java.security-linux"))); changed.append("java.security-bsd")

# TimeZone_md.c: no platform time-zone lookup on the console (the host sets TZ/UTC), and BSD has no `timezone` variable
def tzmd(s):
    if "// PS5 tz" in s: return s
    key = "/*ARGSUSED1*/\nchar *\nfindJavaTZ_md"
    if key not in s: raise SystemExit("ps5_jdk_patch8: findJavaTZ_md anchor missing")
    s = s.replace(key, "// PS5 tz\n#if defined(_ALLBSD_SOURCE) && !defined(MACOSX) && !defined(__linux__) && !defined(__solaris__)\nstatic char *getPlatformTimeZoneID(void) { return NULL; }\n#endif\n\n" + key, 1)
    old = "#else\n    offset = timezone;\n#endif\n"
    new = ("#elif defined(_ALLBSD_SOURCE)\n    struct tm localtm;\n    time_t currenttime = time(NULL);\n    if (localtime_r(&currenttime, &localtm) == NULL) {\n        return strdup(\"GMT\");\n    }\n"
           "    offset = -localtm.tm_gmtoff;   /* PS5: BSD has no `timezone` variable */\n#else\n    offset = timezone;\n#endif\n")
    if old not in s: raise SystemExit("ps5_jdk_patch8: getGMTOffsetID anchor missing")
    return s.replace(old, new, 1)
jdk_edit("solaris/native/java/util/TimeZone_md.c", tzmd, "TimeZone_md.c")

# NetworkInterface.c includes <ifaddrs.h> only on macOS
def netif(s):
    if "#define ETHER_ADDR_LEN 6" not in s and "/* PS5 */" in s:   # earlier revision of this patch: add the missing define
        s = s.replace("#include <ifaddrs.h>   /* PS5 */\n", "#include <ifaddrs.h>   /* PS5 */\n#ifndef ETHER_ADDR_LEN\n#define ETHER_ADDR_LEN 6\n#endif\n", 1)
    old = "#include <netinet/in_var.h>\n#include <ifaddrs.h>\n#endif\n#endif"
    new = "#include <netinet/in_var.h>\n#include <ifaddrs.h>\n#else\n#include <net/if_dl.h>\n#include <ifaddrs.h>   /* PS5 */\n#ifndef ETHER_ADDR_LEN\n#define ETHER_ADDR_LEN 6\n#endif\n#endif\n#endif"
    return must_replace(s, old, new, "NetworkInterface.c ifaddrs", marker="/* PS5 */")
jdk_edit("solaris/native/java/net/NetworkInterface.c", netif, "NetworkInterface.c")

# --- BSD nio: JDK 8 only knows the Mac flavour of BSD. Same sources, no kqueue (the poll() selector is the default for unknown OS names), plain BsdFileSystem
def gmk_edit(rel, fn, label): edit(os.path.join(root, "jdk/make"), rel, fn, label)

def nio_gmk(s):
    old = "$(eval $(call SetupNativeCompilation,BUILD_LIBNIO, \\\n"
    new = ("ifeq ($(OPENJDK_TARGET_OS), bsd)   # PS5\n  BUILD_LIBNIO_FILES += \\\n      InheritedChannel.c \\\n      NativeThread.c \\\n      PollArrayWrapper.c \\\n"
           "      UnixAsynchronousServerSocketChannelImpl.c \\\n      UnixAsynchronousSocketChannelImpl.c \\\n      BsdNativeDispatcher.c \\\n      UnixCopyFile.c \\\n      UnixNativeDispatcher.c\nendif\n\n") + old
    return must_replace(s, old, new, "NioLibraries.gmk bsd", marker="ifeq ($(OPENJDK_TARGET_OS), bsd)   # PS5")
gmk_edit("lib/NioLibraries.gmk", nio_gmk, "NioLibraries.gmk:bsd")

def classes_gmk(s):
    if "# PS5: BSD nio" not in s:
        old = "ifneq ($(OPENJDK_TARGET_OS),aix)\n    EXFILES+=sun/nio/ch/AixAsynchronousChannelProvider.java"
        new = ("# PS5: BSD nio: keep the plain BSD file system classes (no kqueue, no Mac classes)\nifeq ($(OPENJDK_TARGET_OS), bsd)\n"
               "  EXFILES := $(filter-out sun/nio/fs/BsdFileStore.java sun/nio/fs/BsdFileSystem.java sun/nio/fs/BsdFileSystemProvider.java sun/nio/fs/BsdNativeDispatcher.java, $(EXFILES))\nendif\n\n") + old
        s = must_replace(s, old, new, "CompileJavaClasses.gmk bsd fs", marker="# PS5: BSD nio")
    s = must_replace(s, "ifeq (, $(filter $(OPENJDK_TARGET_OS), solaris macosx aix))\n  #\n  # only solaris, macosx and aix\n",
                     "ifeq (, $(filter $(OPENJDK_TARGET_OS), solaris macosx aix bsd))   # PS5: bsd too (BsdFileSystem.newWatchService polls)\n  #\n  # only solaris, macosx and aix\n",
                     "CompileJavaClasses.gmk PollingWatchService", marker="aix bsd))   # PS5")
    return s
gmk_edit("CompileJavaClasses.gmk", classes_gmk, "CompileJavaClasses.gmk:bsd nio classes")

def dfsp(s):
    old = '        if (osname.equals("AIX"))\n'
    new = '        if (osname.endsWith("BSD"))   // PS5\n            return createProvider("sun.nio.fs.BsdFileSystemProvider");\n' + old
    return must_replace(s, old, new, "DefaultFileSystemProvider", marker="// PS5\n            return createProvider(\"sun.nio.fs.BsdFileSystemProvider\")")
jdk_edit("solaris/classes/sun/nio/fs/DefaultFileSystemProvider.java", dfsp, "DefaultFileSystemProvider.java:BSD")

# --- native library set: java zip net nio verify fdlibm (+ management, sunec from Ps5Libraries.gmk). The Platform/Security/Serviceability/Awt2d/Sound makefiles are skipped
# (libattach, libjaas ... have no BSD sources and nothing here loads them; AWT comes from awtlite).
PS5LIBS = r"""# Ps5Libraries.gmk (PS5 port): the extra native libraries the title links statically
BUILD_LIBMANAGEMENT_SRC := $(JDK_TOPDIR)/src/share/native/sun/management \
    $(JDK_TOPDIR)/src/$(OPENJDK_TARGET_OS_API_DIR)/native/sun/management

$(eval $(call SetupNativeCompilation,BUILD_LIBMANAGEMENT, \
    LIBRARY := management, \
    OUTPUT_DIR := $(INSTALL_LIBRARIES_HERE), \
    SRC := $(BUILD_LIBMANAGEMENT_SRC), \
    EXCLUDE_FILES := SolarisOperatingSystem.c LinuxOperatingSystem.c MacosxOperatingSystem.c, \
    LANG := C, \
    OPTIMIZATION := HIGH, \
    CFLAGS := $(CFLAGS_JDKLIB) -I$(JDK_TOPDIR)/src/share/native/sun/management, \
    LDFLAGS := $(LDFLAGS_JDKLIB) $(call SET_SHARED_LIBRARY_ORIGIN), \
    LDFLAGS_SUFFIX := $(LDFLAGS_JDKLIB_SUFFIX), \
    OBJECT_DIR := $(JDK_OUTPUTDIR)/objs/libmanagement, \
    DEBUG_SYMBOLS := false))

$(BUILD_LIBMANAGEMENT): $(BUILD_LIBJAVA)

BUILD_LIBRARIES += $(BUILD_LIBMANAGEMENT)

BUILD_LIBSUNEC_FLAGS := -I$(JDK_TOPDIR)/src/share/native/sun/security/ec \
    -I$(JDK_TOPDIR)/src/share/native/sun/security/ec/impl

$(eval $(call SetupNativeCompilation,BUILD_LIBSUNEC, \
    LIBRARY := sunec, \
    OUTPUT_DIR := $(INSTALL_LIBRARIES_HERE), \
    SRC := $(JDK_TOPDIR)/src/share/native/sun/security/ec \
        $(JDK_TOPDIR)/src/share/native/sun/security/ec/impl, \
    LANG := C++, \
    OPTIMIZATION := LOW, \
    CFLAGS := $(CFLAGS_JDKLIB) $(BUILD_LIBSUNEC_FLAGS) -DMP_API_COMPATIBLE -DNSS_ECC_MORE_THAN_SUITE_B, \
    CXXFLAGS := $(CXXFLAGS_JDKLIB) $(BUILD_LIBSUNEC_FLAGS), \
    LDFLAGS := $(LDFLAGS_JDKLIB) $(LDFLAGS_CXX_JDK), \
    OBJECT_DIR := $(JDK_OUTPUTDIR)/objs/libsunec, \
    DEBUG_SYMBOLS := false))

BUILD_LIBRARIES += $(BUILD_LIBSUNEC)
"""
write_file(os.path.join(root, "jdk/make/lib/Ps5Libraries.gmk"), PS5LIBS, "Ps5Libraries.gmk")

def native_gmk(s):
    for lib in ("PlatformLibraries", "SecurityLibraries", "ServiceabilityLibraries", "Awt2dLibraries", "SoundLibraries"):
        s = re.sub(r"^(?:#TMP )?include lib/%s\.gmk$" % lib, "# PS5: not built: include lib/%s.gmk" % lib, s, flags=re.M)
    if "include lib/Ps5Libraries.gmk" not in s:
        k = s.index("include lib/NioLibraries.gmk\n") + len("include lib/NioLibraries.gmk\n")
        s = s[:k] + "\ninclude lib/Ps5Libraries.gmk\n" + s[k:]
    return s
gmk_edit("CompileNativeLibraries.gmk", native_gmk, "CompileNativeLibraries.gmk:lib set")

# libmanagement OperatingSystemImpl.c: the BSD branch is the Mac one (HW_MEMSIZE via sysctl, which the console kills); take the title's own figure
def osimpl(s):
    old = ("#ifdef _ALLBSD_SOURCE\n    jlong result = 0;\n    int mib[2];\n    size_t rlen;\n\n    mib[0] = CTL_HW;\n    mib[1] = HW_MEMSIZE;\n    rlen = sizeof(result);\n"
           "    if (sysctl(mib, 2, &result, &rlen, NULL, 0) != 0) {\n        throw_internal_error(env, \"sysctl failed\");\n        return -1;\n    }\n    return result;\n#elif defined(_AIX)")
    new = ("#ifdef _ALLBSD_SOURCE\n    extern unsigned long long ps5_physical_memory(void);   /* PS5 */\n    return (jlong) ps5_physical_memory();\n#elif defined(_AIX)")
    return must_replace(s, old, new, "OperatingSystemImpl.c", marker="/* PS5 */\n    return (jlong) ps5_physical_memory")
jdk_edit("solaris/native/sun/management/OperatingSystemImpl.c", osimpl, "OperatingSystemImpl.c")

# Child processes: UNIXProcess.<clinit> throws an Error ("FreeBSD is not a supported OS platform") and the console cannot fork anyway. netty 4.0 (Minecraft 1.8.x) runs Runtime.exec("whoami") from a static
# initialiser and only catches Exceptions: an IOException lets the network start (the Error killed the integrated server's connection setup, i.e. entering a world).
def procimpl(s):
    old = "                         boolean redirectErrorStream)\n        throws IOException\n    {\n"
    new = old + '        if (true) throw new IOException("ps5: the console cannot start child processes");   // PS5\n'
    return must_replace(s, old, new, "ProcessImpl.java", marker="// PS5\n")
jdk_edit("solaris/classes/java/lang/ProcessImpl.java", procimpl, "ProcessImpl.java")

# libnet: bsd_close.c (the interruptible NET_* socket wrappers) is only compiled for macosx
def net_gmk(s):
    old = "ifneq ($(OPENJDK_TARGET_OS), macosx)\n  LIBNET_EXCLUDE_FILES += bsd_close.c\nendif"
    new = "ifeq (, $(filter $(OPENJDK_TARGET_OS), macosx bsd))   # PS5\n  LIBNET_EXCLUDE_FILES += bsd_close.c\nendif"
    return must_replace(s, old, new, "NetworkingLibraries.gmk", marker="macosx bsd))   # PS5")
gmk_edit("lib/NetworkingLibraries.gmk", net_gmk, "NetworkingLibraries.gmk:bsd_close")

# CreateJars.gmk wants make/data/classlist/classlist.<os> (a class-loading order hint); the Linux one is as good for any ELF-ish target
cl = os.path.join(root, "jdk/make/data/classlist")
if not os.path.exists(os.path.join(cl, "classlist.bsd")):
    wr(os.path.join(cl, "classlist.bsd"), rd(os.path.join(cl, "classlist.linux"))); changed.append("classlist.bsd")

# ============================================================================================================================================
# Trace beacons (same idea as the 17 patch): how far JNI_CreateJavaVM gets, HotSpot's own fatal/exit reasons, tty/-Xlog output -> ps5_trace -> UDP log
# ============================================================================================================================================
_KW = ("return", "break", "continue", "goto", "throw", "case", "default", "delete", "else", "typedef", "using", "do", "while", "for", "if", "switch")
def instrument(sig, label):
    def f(t):
        k = t.find(sig)
        if k < 0: raise SystemExit("ps5_jdk_patch8: trace anchor missing: " + sig)
        if "// PS5 TRACE " + label + "\n" in t: return t
        end = body_end(t, k)
        head, body, tail = t[:k], t[k:end], t[end:]
        lines = body.split("\n"); out = []; prev = ""; first_brace_done = False
        for ln in lines:
            out.append(ln)
            if not first_brace_done:
                if ln.rstrip().endswith("{"):
                    first_brace_done = True
                    out.append('  ps5_trace("%s: enter");  // PS5 TRACE %s' % (label, label))
                continue
            m = re.match(r"^  ([A-Za-z_][^#{}]*);\s*$", ln)
            if (m and not prev.rstrip().endswith("\\") and not ln.strip().startswith(_KW)
                    and ln.count("(") == ln.count(")") and not ln.strip().endswith(":")):
                txt = re.sub(r'["\\%]', "", ln.strip())[:70]
                out.append('  ps5_trace("%s %s");' % (label, txt))
            prev = ln
        t2 = head + "\n".join(out) + tail
        return decl_once(t2)
    return f

TRACE_DECL = 'extern "C" void ps5_trace(const char*);\n'
def decl_once(s):
    if TRACE_DECL in s: return s
    i = s.index('#include "')   # after the file's first include: precompiled.hpp must stay first
    j = s.index("\n", i) + 1
    return s[:j] + TRACE_DECL + s[j:]

def after_sig(sig, code, tag):
    def f(t):
        if "// PS5 ERR " + tag + "\n" in t: return t
        k = t.find(sig)
        if k < 0: raise SystemExit("ps5_jdk_patch8: trace anchor missing: " + sig)
        j = t.index("{", k) + 1
        return decl_once(t[:j] + "  // PS5 ERR " + tag + "\n" + code + t[j:])
    return f

def tr(rel, sig, label): hs_edit(rel, instrument(sig, label), rel + ":trace " + label)
tr("share/vm/runtime/thread.cpp", "jint Threads::create_vm(JavaVMInitArgs* args, bool* canTryAgain) {", "create_vm")
tr("share/vm/runtime/init.cpp", "jint init_globals() {", "init_globals")
tr(osbsd + "/os_bsd.cpp", "void os::init(void) {", "os_init")
tr(osbsd + "/os_bsd.cpp", "jint os::init_2(void)", "os_init_2")
tr("share/vm/prims/jni.cpp", "static jint JNI_CreateJavaVM_inner(JavaVM **vm, void **penv, void *args) {", "jni_inner")
tr("share/vm/runtime/arguments.cpp", "void Arguments::init_system_properties() {", "args_sysprops")
tr("share/vm/runtime/arguments.cpp", "jint Arguments::parse(const JavaVMInitArgs* args) {", "args_parse")
tr("share/vm/runtime/arguments.cpp", "jint Arguments::apply_ergo() {", "apply_ergo")
tr("share/vm/runtime/arguments.cpp", "void Arguments::set_heap_size() {", "set_heap_size")
tr(osbsd + "/os_bsd.cpp", "void os::init_system_properties_values() {", "os_sysprops")
tr("cpu/x86/vm/vm_version_x86.cpp", "void VM_Version::get_processor_features() {", "vmver_features")
tr("share/vm/runtime/stubRoutines.cpp", "void StubRoutines::initialize1() {", "stubs1")
tr("share/vm/memory/universe.cpp", "jint universe_init() {", "universe_init")
tr("share/vm/memory/universe.cpp", "jint Universe::initialize_heap() {", "init_heap")
tr("share/vm/runtime/vm_version.cpp", "void VM_Version_init() {", "vmver_top")

# --- extra beacons: init_globals lines that carry a trailing comment, universe_post_init steps, and who enters ObjectSynchronizer::inflate (obj, thread, return address)
def init_extra(t):
    if "// PS5 TRACE init_extra" in t: return t
    for stmt in ("interpreter_init();  // before any methods loaded", "universe2_init();  // dependent on codeCache_init and stubRoutines_init1",
                 "javaClasses_init();   // must happen after vtable initialization", "stubRoutines_init2(); // note: StubRoutines need 2-phase init"):
        t = must_replace(t, "  " + stmt + "\n", "  " + stmt + "\n  ps5_trace(\"init_globals %s\");  // PS5 TRACE init_extra\n" % stmt.split("(")[0], "init_extra " + stmt[:12], marker="never-present-marker")
    t = must_replace(t, "  if (!universe_post_init()) {\n", '  ps5_trace("init_globals universe_post_init: begin");\n  if (!universe_post_init()) {\n', "post_init begin", marker='universe_post_init: begin')
    return decl_once(t)
hs_edit("share/vm/runtime/init.cpp", init_extra, "init.cpp:extra beacons")

def post_init_extra(t):
    if "// PS5 TRACE post_init" in t: return t
    t = must_replace(t, "    Interpreter::initialize();      // needed for interpreter entry points\n", "    ps5_trace(\"post_init Interpreter::initialize\");  // PS5 TRACE post_init\n    Interpreter::initialize();      // needed for interpreter entry points\n    ps5_trace(\"post_init Interpreter::initialize done\");\n", "pi1", marker="post_init Interpreter::initialize done")
    t = must_replace(t, "      Universe::reinitialize_itables(CHECK_false);\n", "      Universe::reinitialize_itables(CHECK_false);\n      ps5_trace(\"post_init vtables/itables done\");\n", "pi2", marker="post_init vtables/itables done")
    for nm in ("OutOfMemoryError", "NullPointerException", "ArithmeticException"):
        t = must_replace(t, "k = SystemDictionary::resolve_or_fail(vmSymbols::java_lang_%s(), true, CHECK_false);\n" % nm, "ps5_trace(\"post_init resolve %s\");\n    k = SystemDictionary::resolve_or_fail(vmSymbols::java_lang_%s(), true, CHECK_false);\n    ps5_trace(\"post_init resolved %s\");\n" % (nm, nm, nm), "pi " + nm, marker="post_init resolved " + nm)
    return decl_once(t)
hs_edit("share/vm/memory/universe.cpp", post_init_extra, "universe.cpp:post_init beacons")

def inflate_untrace(t):   # the per-call inflate beacon of the previous patch revision is gone again (hot path)
    i = t.find("  { char b[120]; ::snprintf(b, sizeof b, \"inflate self=")
    if i < 0: return t
    j = t.index("ps5_trace(b); }\n", t.index("inflate mark=", i)) + len("ps5_trace(b); }\n")
    return t[:i] + t[j:]
hs_edit("share/vm/runtime/synchronizer.cpp", inflate_untrace, "synchronizer.cpp:inflate beacon removed")

# --- clang miscompiles markOopDesc: its value() is `(uintptr_t) this` and clang assumes `this` is 8-byte aligned (alignof(oopDesc)), so every tag-bit test
# (has_monitor, is_neutral, has_locker, biased/hash bits ...) is folded to a constant. Seen on the console as a SEGV in ObjectSynchronizer::inflate (mark 1 treated as a stack lock).
# An empty asm hides the value from the optimiser.
def markop_value(t):
    old = "  uintptr_t value() const { return (uintptr_t) this; }"
    new = "  uintptr_t value() const { uintptr_t v = (uintptr_t) this; __asm__ (\"\" : \"+r\" (v)); return v; }   // PS5: clang assumes `this` is 8-byte aligned and folds every tag-bit test to a constant"
    return must_replace(t, old, new, "markOop value()", marker="folds every tag-bit test")
hs_edit("share/vm/oops/markOop.hpp", markop_value, "markOop.hpp:value() laundering")

def java_cpp(s):
    s = after_sig("void vm_abort(bool dump_core) {", '  ps5_trace("vm_abort");\n', "vm_abort")(s)
    s = after_sig("void vm_exit_during_initialization(Symbol* ex, const char* message) {",
                  '  ps5_trace("vm_exit_during_initialization(sym):"); ps5_trace(ex ? ex->as_C_string() : "(null)"); ps5_trace(message ? message : "(null)");\n', "vmexit_sym")(s)
    s = after_sig("void vm_exit_during_initialization(const char* error, const char* message) {",
                  '  ps5_trace("vm_exit_during_initialization:"); ps5_trace(error ? error : "(null)"); ps5_trace(message ? message : "(null)");\n', "vmexit_str")(s)
    s = after_sig("void vm_shutdown_during_initialization(const char* error, const char* message) {",
                  '  ps5_trace("vm_shutdown_during_initialization:"); ps5_trace(error ? error : "(null)"); ps5_trace(message ? message : "(null)");\n', "vmshut")(s)
    return s
hs_edit("share/vm/runtime/java.cpp", java_cpp, "java.cpp:trace exits")

def debug_cpp(s):
    rep = '  ps5_trace("%s:"); ps5_trace(file ? file : "?"); { char b[48]; snprintf(b, sizeof b, "line %%d", line); ps5_trace(b); } ps5_trace(%s);\n'
    s = after_sig("void report_vm_error(const char* file, int line, const char* error_msg,", rep % ("report_vm_error", 'error_msg ? error_msg : "(null)"'), "report_vm_error")(s)
    s = after_sig("void report_fatal(const char* file, int line, const char* message)", rep % ("report_fatal", 'message ? message : "(null)"'), "report_fatal")(s)
    return s
hs_edit("share/vm/utilities/debug.cpp", debug_cpp, "debug.cpp:trace fatal")

hs_edit(osbsd + "/os_bsd.cpp", after_sig("void os::abort(bool dump_core) {", '  ps5_trace(dump_core ? "os::abort(dump_core=true)" : "os::abort(dump_core=false)");\n', "os_abort"), "os_bsd.cpp:trace abort")

# tty / hs_err / -Xlog output and jio_fprintf are mirrored into the trace hook (stdout cannot be redirected on the console)
BYTES_DECL = '#include <stddef.h>\nextern "C" void ps5_trace_bytes(const char*, size_t);\n'
def bytes_decl(s):
    if "ps5_trace_bytes(const char*, size_t)" in s: return s
    i = s.index('#include "'); j = s.index("\n", i) + 1
    return s[:j] + BYTES_DECL + s[j:]
def tty(s):
    if "ps5_trace_bytes(s, len)" in s: return s
    for sig in ("void defaultStream::write(const char* s, size_t len) {\n", "void fdStream::write(const char* s, size_t len) {\n"):
        s = must_replace(s, sig, sig + "  ps5_trace_bytes(s, len);\n", "ostream " + sig[:20], marker="never")
    return bytes_decl(s)
hs_edit("share/vm/utilities/ostream.cpp", tty, "ostream.cpp:tty trace")

def jio(s):
    old = "int jio_vfprintf(FILE* f, const char *fmt, va_list args) {\n"
    if "PS5 jio mirror" in s: return s
    new = (old + "  { // PS5 jio mirror\n    char b[600]; va_list c; va_copy(c, args);\n"
           "    int n = vsnprintf(b, sizeof b, fmt, c); va_end(c);\n"
           "    if (n > 0) ps5_trace_bytes(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1)); }\n")
    return bytes_decl(must_replace(s, old, new, "jio_vfprintf", marker="PS5 jio mirror"))
hs_edit("share/vm/prims/jvm.cpp", jio, "jvm.cpp:jio mirror")

print("patched:", ", ".join(changed) if changed else "nothing (already applied)")
