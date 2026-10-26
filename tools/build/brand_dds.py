#!/usr/bin/env python3
"""brand_dds.py <in.png|jpg|dds> <out.dds>: any picture -> the 3840x2160 BC7_UNORM DX10 DDS (single mip level) that the PS5 app builder accepts. Wider / taller pictures are cropped to 16:9 (centre), smaller ones are scaled up.
Needs: pip install --break-system-packages etcpak pillow"""
import struct, sys
from PIL import Image
import etcpak
W, H = 3840, 2160
src, dst = sys.argv[1:3]
im = Image.open(src).convert("RGBA")
w, h = im.size
if w * H != h * W:   # crop to 16:9 around the centre
    if w * H > h * W: nw = h * W // H; im = im.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    else: nh = w * H // W; im = im.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
if im.size != (W, H): im = im.resize((W, H), Image.LANCZOS)
data = etcpak.compress_bc7(im.tobytes(), W, H)
hd = bytearray(148); hd[:4] = b"DDS "
struct.pack_into("<IIIII", hd, 4, 124, 0x1007 | 0x80000 | 0x20000, H, W, len(data))   # CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE | MIPMAPCOUNT
struct.pack_into("<I", hd, 28, 1); struct.pack_into("<I", hd, 76, 32); struct.pack_into("<I", hd, 80, 4); hd[84:88] = b"DX10"; struct.pack_into("<I", hd, 108, 0x1000)
struct.pack_into("<IIIII", hd, 128, 98, 3, 0, 1, 0)
open(dst, "wb").write(bytes(hd) + data)
print("%s -> %s (%dx%d BC7, %d bytes)" % (src, dst, W, H, len(hd) + len(data)))
