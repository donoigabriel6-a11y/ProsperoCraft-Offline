#!/usr/bin/env python3
"""Launcher backgrounds: upscale, sharpen and crop every picture in <src> to 1920x1080 JPEG into <out>.

usage: prepare_backgrounds.py <src dir> <out dir>

Old, small or odd-shaped pictures are fine: each one is cropped to 16:9 (a little above the centre, where faces
and horizons usually are), enlarged in 2x steps with Lanczos (a light sharpen after every step so the edges do not
go soft), and given a final unsharp mask that grows with the enlargement. Large pictures are reduced with Lanczos
and lightly sharpened. Files that cannot be read are skipped with a message.
"""
import math
import os
import sys

W, H = 1920, 1080
EXTS = (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff", ".gif")


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.lower().endswith((".jpg", ".jpeg", ".png")):
            os.remove(os.path.join(out, f))
    names = sorted(f for f in os.listdir(src) if f.lower().endswith(EXTS) and not f.startswith("."))
    if not names:
        print("backgrounds: none found in %s (the launcher keeps its animated backdrop)" % src)
        return 0
    try:
        from PIL import Image, ImageFilter, ImageOps
    except ImportError:
        import subprocess
        subprocess.call([sys.executable, "-m", "pip", "install", "--user", "--break-system-packages", "-q", "pillow"])
        try:
            from PIL import Image, ImageFilter, ImageOps
        except ImportError:
            Image = None
    if Image is None:
        print("backgrounds: Pillow is missing, copying the pictures as they are (no upscaling / sharpening)")
        import shutil
        for i, n in enumerate(names):
            if n.lower().endswith((".jpg", ".jpeg", ".png")):
                shutil.copy(os.path.join(src, n), os.path.join(out, "%02d_%s" % (i, n)))
        return 0

    done = 0
    for i, n in enumerate(names):
        try:
            im = Image.open(os.path.join(src, n))
            im = ImageOps.exif_transpose(im)
            if im.mode in ("RGBA", "LA", "P"):
                im = im.convert("RGBA")
                bg = Image.new("RGB", im.size, (0, 0, 0))
                bg.paste(im, mask=im.split()[3])
                im = bg
            else:
                im = im.convert("RGB")
            w, h = im.size
            cw = min(w, h * W / H)
            ch = min(h, w * H / W)
            x0 = (w - cw) / 2.0
            y0 = (h - ch) * 0.4
            im = im.crop((int(round(x0)), int(round(y0)), int(round(x0 + cw)), int(round(y0 + ch))))
            cw = im.size[0]
            f = W / float(cw)
            if f > 1.001:
                # enlarge in steps of at most 2x; each step is sharpened a little so the next one has edges to work with
                cur = im
                while cur.size[0] * 2 < W:
                    cur = cur.resize((cur.size[0] * 2, cur.size[1] * 2), Image.LANCZOS)
                    cur = cur.filter(ImageFilter.UnsharpMask(radius=1.4, percent=55, threshold=2))
                im = cur.resize((W, H), Image.LANCZOS)
                steps = math.log2(f)
                im = im.filter(ImageFilter.UnsharpMask(radius=min(2.4, 1.0 + 0.35 * steps), percent=int(min(210, 70 + 45 * steps)), threshold=2))
            else:
                if im.size != (W, H):
                    im = im.resize((W, H), Image.LANCZOS)
                im = im.filter(ImageFilter.UnsharpMask(radius=0.9, percent=55, threshold=2))
            base = os.path.splitext(n)[0]
            safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in base)[:40]
            im.save(os.path.join(out, "%02d_%s.jpg" % (i, safe)), "JPEG", quality=92, subsampling=0, optimize=True)
            print("backgrounds: %s  %dx%d -> %dx%d (x%.2f)" % (n, w, h, W, H, f))
            done += 1
        except Exception as e:
            print("backgrounds: skipped %s (%s)" % (n, e))
    print("backgrounds: %d of %d ready" % (done, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
