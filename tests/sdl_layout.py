#!/usr/bin/env python3
# Checks the SDL_Event / struct offsets used by ps5sdl.inc against the LWJGL 3.4.3 generated Layout code (needs the LWJGL checkout: arg 1, default ~/lwjgl3-3.4.3).
# Skips quietly when the sources are not there. Prints "fails=N".
import os, re, sys
root = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/lwjgl3-3.4.3")
d = os.path.join(root, "modules/lwjgl/sdl/src/generated/java/org/lwjgl/sdl")
if not os.path.isdir(d): print("skipped (no LWJGL 3.4.3 checkout at %s)" % root); print("fails=0"); sys.exit(0)
def offsets(cls):
    src = open(os.path.join(d, cls + ".java")).read()
    m = re.search(r"Layout layout = (?:__struct|__union)\((.*?)\);\s*\n\s*SIZEOF", src, re.S) or re.search(r"Layout layout = __struct\((.*?)\);", src, re.S)
    off = 0; res = []; size_max = 1
    for t in re.finditer(r"__(member|array)\(([^)]*)\)|__padding\(([^)]*)\)", m.group(1)):
        if t.group(1) == "member": sz = 8 if "POINTER" in t.group(2) else int(t.group(2)); al = sz; n = 1
        elif t.group(1) == "array": a = [x.strip() for x in t.group(2).split(",")]; sz = int(a[0]); al = sz; n = int(a[1])
        else: continue
        off = (off + al - 1) // al * al; res.append(off); off += sz * n; size_max = max(size_max, al)
    return res, (off + size_max - 1) // size_max * size_max
# member index -> offset the C code relies on
want = {
 "SDL_KeyboardEvent": {3: 16, 4: 20, 5: 24, 6: 28, 7: 32, 9: 36, 10: 37},
 "SDL_TextInputEvent": {3: 16, 4: 24},
 "SDL_MouseMotionEvent": {3: 16, 4: 20, 5: 24, 6: 28, 7: 32, 8: 36, 9: 40},
 "SDL_MouseButtonEvent": {3: 16, 4: 20, 5: 24, 6: 25, 7: 26, 9: 28, 10: 32},
 "SDL_MouseWheelEvent": {3: 16, 4: 20, 5: 24, 6: 28, 7: 32, 8: 36, 9: 40, 10: 44, 11: 48},
 "SDL_WindowEvent": {3: 16, 4: 20, 5: 24},
 "SDL_DisplayMode": {0: 0, 1: 4, 2: 8, 3: 12, 4: 16, 5: 20, 6: 24, 7: 28, 8: 32},
 "SDL_Surface": {0: 0, 1: 4, 2: 8, 3: 12, 4: 16, 5: 24, 6: 32},
 "SDL_PixelFormatDetails": {0: 0, 1: 4, 2: 5, 4: 8, 8: 24, 9: 25, 10: 26},
 "SDL_MessageBoxData": {0: 0, 1: 8, 2: 16, 3: 24, 4: 32, 5: 40},
 "SDL_MessageBoxButtonData": {0: 0, 1: 4, 2: 8},
 "SDL_Rect": {0: 0, 1: 4, 2: 8, 3: 12},
}
sizes = {"SDL_DisplayMode": 40, "SDL_Surface": 48, "SDL_PixelFormatDetails": 32}
fails = 0
for cls, w in want.items():
    offs, size = offsets(cls)
    for i, o in w.items():
        if i >= len(offs) or offs[i] != o: print("FAIL %s member %d: LWJGL says %s, shim uses %d" % (cls, i, offs[i] if i < len(offs) else "?", o)); fails += 1
    if cls in sizes and size != sizes[cls]: print("FAIL %s size %d != %d" % (cls, size, sizes[cls])); fails += 1
print("checked %d structs" % len(want)); print("fails=%d" % fails); sys.exit(1 if fails else 0)
