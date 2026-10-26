#!/usr/bin/env python3
# brand_check.py <file.dds>: exit 0 when it is the 3840x2160 BC7_UNORM DX10 single-mip DDS that the PS5 app builder accepts
import struct, sys
b = open(sys.argv[1], "rb").read(148)
ok = len(b) >= 148 and b[:4] == b"DDS " and b[84:88] == b"DX10"
if ok:
    h, w, mips = struct.unpack_from("<I", b, 12)[0], struct.unpack_from("<I", b, 16)[0], struct.unpack_from("<I", b, 28)[0]
    fmt = struct.unpack_from("<I", b, 128)[0]
    print("   %s: %dx%d, %d mip level(s), DXGI format %d" % (sys.argv[1].split("/")[-1], w, h, mips, fmt))
    ok = w == 3840 and h == 2160 and mips <= 1 and fmt == 98
sys.exit(0 if ok else 1)
