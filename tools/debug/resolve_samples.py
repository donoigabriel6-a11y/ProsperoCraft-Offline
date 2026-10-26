#!/usr/bin/env python3
"""resolve_samples.py <log.txt> [elf]: names the threads of the 'WHOLE JVM STOPPED' all-thread sample (SAMPLE #i lines + 'thr #i fn=' table) with function names.
Uses the unstripped link output of the title you deployed (default ~/ps5probe/build/llvm-pie.elf). Prints, per thread: start routine, then the first resolved frames."""
import os, re, shutil, subprocess, sys
log = open(sys.argv[1], errors="replace").read().replace("\r", "")
cands = [sys.argv[2]] if len(sys.argv) > 2 else ["~/ps5probe/.vkrelink/llvm-pie.elf", "~/ps5probe/build/llvm-pie.elf"]   # the vk build links the deployed ELF in .vkrelink
elf = next((os.path.expanduser(c) for c in cands if os.path.isfile(os.path.expanduser(c))), os.path.expanduser(cands[0]))
print("using", elf)
nm = subprocess.run(["nm", elf], capture_output=True, text=True).stdout
m = re.search(r"^([0-9a-fA-F]+) [Tt] main$", nm, re.M)
if not m: sys.exit("no 'main' symbol in " + elf)
link_main = int(m.group(1), 16)
tool = next((t for t in ("llvm-symbolizer-18", "llvm-symbolizer", "llvm-symbolizer-17", "llvm-symbolizer-16") if shutil.which(t)), None)
_cache = {}
def sym(a):
    if a in _cache: return _cache[a]
    _cache[a] = _sym(a); return _cache[a]
def _sym(a):
    if tool: r = subprocess.run([tool, "-f", "-C", "--obj=" + elf, hex(a)], capture_output=True, text=True).stdout.split("\n")
    else: r = subprocess.run(["addr2line", "-f", "-C", "-e", elf, hex(a)], capture_output=True, text=True).stdout.split("\n")
    return r[0] if r and r[0] and r[0] != "??" else "?"
tab = {}
for i, fn, arg in re.findall(r"thr #(\d+) fn=(\S+) arg=(\S+)", log): tab[int(i)] = (fn, arg)
n = 0
for mm in re.finditer(r"SAMPLE #(\d+) thr=(\S+) main=(\w+) rip=(\w+) rax=(\w+) rdi=(\w+) rsi=(\w+) rdx=(\w+) stk:([ 0-9a-f]*)", log):
    i = int(mm.group(1)); run_main = int(mm.group(3), 16); d = run_main - link_main
    fn = tab.get(i, ("?", "?"))[0]
    fnname = "-"
    rip = int(mm.group(4), 16)
    print("#%d start=%s  rip=%s (%s) syscall-ish rax=%s rdi=%s rsi=%s" % (i, fnname, hex(rip), sym(rip - d) if 0x400000 <= rip < 0x10000000 else ("jit/stub" if rip >= 0x100000000 else "libkernel/other"), mm.group(5), mm.group(6), mm.group(7)))
    frames = [sym(int(x, 16) - d) for x in mm.group(9).split()]
    out = []
    for f in frames:
        if f != "?" and (not out or out[-1] != f): out.append(f)
    print("     " + " < ".join(out[:12])); n += 1
if not n: sys.exit("no SAMPLE lines in the log (the stall must reach 'WHOLE JVM STOPPED')")
