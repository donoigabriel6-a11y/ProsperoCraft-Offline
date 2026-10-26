#!/usr/bin/env python3
"""resolve_prof.py <log.txt> [elf] [--last K]: turn the 'nprof' sample lines of a listener log (NPROF=1 build) into function names.
Adds up every bucket (16-byte address ranges of the interrupted instruction pointer, render thread only), resolves them against the unstripped link output of the
title you deployed (default ~/ps5probe/build/llvm-pie.elf) and prints the functions by share of samples. --last K uses only the last K 2-second windows
(e.g. --last 8 = steady state in the world). Addresses outside the executable (JIT code, system libraries) are grouped as such."""
import argparse, collections, os, re, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("log"); ap.add_argument("elf", nargs="?", default="~/ps5probe/build/llvm-pie.elf"); ap.add_argument("--last", type=int, default=0)
ap.add_argument("--top", type=int, default=40)
a = ap.parse_args()
log = open(a.log, errors="replace").read().replace("\r", "")
elf = os.path.expanduser(a.elf)
hx = lambda x: int(x, 16)

m = re.search(r"nprof: started .*?main=(?:0x)?([0-9a-fA-F]+)", log)
if not m: sys.exit("no 'nprof: started' line in the log (was the title built with NPROF=1?)")
run_main = hx(m.group(1))
nm = subprocess.run(["nm", elf], capture_output=True, text=True).stdout
mm = re.search(r"^([0-9a-fA-F]+) [Tt] main$", nm, re.M)
if not mm: sys.exit("could not find 'main' in " + elf)
delta = run_main - hx(mm.group(1))
lo, hi = 1 << 63, 0
for l in subprocess.run(["readelf", "-lW", elf], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if f and f[0] == "LOAD": lo, hi = min(lo, int(f[2], 16)), max(hi, int(f[2], 16) + int(f[5], 16))

windows = []
for l in log.split("\n"):
    if "nprof n=" in l:
        b = collections.Counter()
        mw = re.search(r"nprof n=(\d+) top:(.*)", l)
        for tok in mw.group(2).split():
            ad, c = tok.split(":"); b[hx(ad)] += int(c)
        windows.append(b)
    elif "nprof n=" not in l and "nprof" in l and windows is not None:
        pass
# a window is printed over several lines (9 buckets each): merge consecutive lines that belong to one report (same 'n=' value)
merged, last_n = [], None
for l in log.split("\n"):
    mw = re.search(r"nprof n=(\d+) top:(.*)", l)
    if not mw: last_n = None; continue
    n = int(mw.group(1)); b = collections.Counter()
    for tok in mw.group(2).split():
        ad, c = tok.split(":"); b[hx(ad)] += int(c)
    if last_n == n and merged: merged[-1].update(b)
    else: merged.append(b)
    last_n = n
if not merged: sys.exit("no nprof sample lines found")
if a.last: merged = merged[-a.last:]
tot = collections.Counter()
for w in merged: tot.update(w)
all_s = sum(tot.values())
print("windows used: %d, samples in the printed top buckets: %d, load delta %#x, executable %#x-%#x" % (len(merged), all_s, delta, lo, hi))

sym = shutil.which("llvm-symbolizer-18") or shutil.which("llvm-symbolizer")
inside = [ad for ad in tot if lo <= ad - delta < hi]
names = {}
if inside and sym:
    out = subprocess.run([sym, "-f", "-C", "--obj=" + elf], input="\n".join(hex(ad - delta) for ad in inside) + "\n", capture_output=True, text=True).stdout
    chunks = [c for c in out.strip().split("\n\n")]
    for ad, c in zip(inside, chunks): names[ad] = c.split("\n")[0]
elif inside:
    for ad in inside:
        r = subprocess.run(["addr2line", "-f", "-C", "-e", elf, hex(ad - delta)], capture_output=True, text=True).stdout.split("\n")
        names[ad] = r[0]
by = collections.Counter()
for ad, c in tot.items(): by[names.get(ad, "(outside the executable: JIT code / system library)") if ad in names or not (lo <= ad - delta < hi) else "?"] += c
print("%6s  %s" % ("share", "function"))
for name, c in by.most_common(a.top): print("%5.1f%%  %s" % (100.0 * c / all_s, name))

# second table: callers (inside the executable) of the samples that were not inside the executable, i.e. who was blocked in a system stub / libc / JIT code and where
cm, last_n = [], None
for l in log.split("\n"):
    mw = re.search(r"nprof-callers n=(\d+) top:(.*)", l)
    if not mw: last_n = None; continue
    n = int(mw.group(1)); b = collections.Counter()
    for tok in mw.group(2).split():
        ad, c = tok.split(":"); b[hx(ad)] += int(c)
    if last_n == n and cm: cm[-1].update(b)
    else: cm.append(b)
    last_n = n
if cm:
    if a.last: cm = cm[-a.last:]
    ct = collections.Counter()
    for w in cm: ct.update(w)
    ins = [ad for ad in ct if lo <= ad - delta < hi]
    nm2 = {}
    if ins and sym:
        out = subprocess.run([sym, "-f", "-C", "--obj=" + elf], input="\n".join(hex(ad - delta) for ad in ins) + "\n", capture_output=True, text=True).stdout
        for ad, c in zip(ins, out.strip().split("\n\n")): nm2[ad] = c.split("\n")[0]
    elif ins:
        for ad in ins: nm2[ad] = subprocess.run(["addr2line", "-f", "-C", "-e", elf, hex(ad - delta)], capture_output=True, text=True).stdout.split("\n")[0]
    by2 = collections.Counter()
    for ad, c in ct.items(): by2[nm2.get(ad, "?")] += c
    tot2 = sum(by2.values())
    print("\ncallers of the non-executable samples (%d samples with a caller found; share is of those):" % tot2)
    for name, c in by2.most_common(a.top): print("%5.1f%%  %s" % (100.0 * c / tot2, name))
else:
    print("\n(no nprof-callers lines: build predates the caller capture)")
