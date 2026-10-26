#!/usr/bin/env python3
"""resolve_crash.py <log.txt> [elf]: turn the CRASH lines of a listener log into function names.
Uses the unstripped link output of the title you deployed (default ~/ps5probe/build/llvm-pie.elf)."""
import os, re, subprocess, sys

log = open(sys.argv[1], errors="replace").read().replace("\r", "")
cands = [sys.argv[2]] if len(sys.argv) > 2 else ["~/ps5probe/.vkrelink/llvm-pie.elf", "~/ps5probe/build/llvm-pie.elf"]   # the vk build links the deployed ELF in .vkrelink (the other one is only the boilerplate link that fails on purpose)
elf = next((os.path.expanduser(c) for c in cands if os.path.isfile(os.path.expanduser(c))), os.path.expanduser(cands[0]))
print("using", elf)
hx = lambda x: int(x, 16)
blocks = log.split("CRASH sig=")[1:]
partial = False
if not blocks or "main=" not in blocks[-1] or log.count("CRASH end") > len(blocks):   # the UDP listener lost the header lines of a dump: use the last stk[] group (load base from the first main= line)
    tail = log.rsplit("CRASH end", 2)
    tail = tail[-2] if len(tail) > 2 else log
    stk = re.findall(r"CRASH stk\[\d+\]=\w+", tail)
    if not stk: sys.exit("no 'CRASH sig=' block and no CRASH stk[] lines in the log")
    blocks = ["0 code=0 addr=0\nCRASH main=0 rip=0 rsp=0 rflags=0\n" + "\n".join(stk[-24:])]
    partial = True
nm = subprocess.run(["nm", elf], capture_output=True, text=True).stdout
mm = re.search(r"^([0-9a-fA-F]+) [Tt] main$", nm, re.M)
if not mm: sys.exit("could not find 'main' in " + elf)
link_main = hx(mm.group(1))

_segs = []
for _l in subprocess.run(["readelf", "-lW", elf], capture_output=True, text=True).stdout.splitlines():
    _f = _l.split()
    if _f and _f[0] == "LOAD": _segs.append((int(_f[2], 16), int(_f[1], 16), int(_f[4], 16)))
_data = open(elf, "rb").read()
def rd(va, n):
    for v, o, sz in _segs:
        if v <= va < v + sz: return _data[o + va - v : o + va - v + n]
    return None

def sym(addr, delta):
    r = subprocess.run(["llvm-symbolizer-18", "-f", "-C", "--obj=" + elf, hex(addr - delta)], capture_output=True, text=True).stdout.strip().split("\n")
    return r[0] if r and r[0] != "??" else "?"

for n, b in enumerate(blocks):
    if n == 0 and b.startswith("30 "): continue          # skip the host's harmless SIGUSR1 self-test
    m = re.search(r"main=(\w+) rip=(\w+) rsp=(\w+)", b)
    if not m: continue
    delta = (hx(re.search(r"main=(\w+)", log).group(1)) - link_main) if partial else hx(m.group(1)) - link_main   # partial: the base from the first main= line of the log (the SIGUSR1 self-test)
    head = re.match(r"(\d+) code=(\d+) addr=(\w+)", b)
    print("== crash: sig=%s code=%s addr=%s" % head.groups())
    rip = hx(m.group(2))
    if partial: print("  (dump header lost in the log: stk[] only; load base taken from the self-test main= line)")
    print("  rip  %-12x %s" % (rip, sym(rip, delta) if 0x400000 <= rip < 0x10000000 else "(not in the executable)"))
    st0 = re.search(r"stk\[\d+\]=(\w+)", b)
    if st0 and not partial:                                              # decode the call that jumped to the bad address, from the ELF file itself
        ret = hx(st0.group(1))
        code = rd(ret - delta - 16, 16)
        slot, how = None, "unknown"
        if code and code[-6:-4] == b"\xff\x15":                    # call [rip+disp32]
            slot = ret + int.from_bytes(code[-4:], "little", signed=True)
            how = "call [rip+disp]  (import or global function pointer)"
        elif code and code[-5:-4] == b"\xe8":                       # call rel32, possibly into a PLT stub that jumps through the GOT
            target = ret + int.from_bytes(code[-4:], "little", signed=True)
            dis = subprocess.run(["objdump", "-d", "-M", "intel", "--start-address=%#x" % (target - delta), "--stop-address=%#x" % (target - delta + 16), elf],
                                 capture_output=True, text=True).stdout
            mm2 = re.search(r"\[rip\+0x[0-9a-f]+\]\s+# (0x)?([0-9a-f]+)", dis)
            how = "call %x -> %s" % (target, sym(target, delta))
            if mm2: slot = hx(mm2.group(2)) + delta
        elif code:
            how = "indirect call via register/memory; bytes before return: " + code.hex()
        print("  caller instruction: " + how)
        if slot is not None:
            rel = subprocess.run(["readelf", "-rW", elf], capture_output=True, text=True).stdout
            hit = [l for l in rel.splitlines() if re.match(r"\s*0*%x\s" % (slot - delta), l)]
            name = hit[0].split()[4] if hit and len(hit[0].split()) > 4 else "?"
            print("  GOT slot %x -> %s" % (slot, name if hit else "(no relocation: a plain data pointer, not an import)"))
    seen = set()
    for k, a in re.findall(r"stk\[(\d+)\]=(\w+)", b):
        a = hx(a)
        if a in seen: continue
        seen.add(a)
        print("  stk%-4s %-10x %s" % (k, a, sym(a, delta)))
