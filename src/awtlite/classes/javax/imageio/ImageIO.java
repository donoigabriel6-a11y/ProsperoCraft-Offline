package javax.imageio;

import java.awt.image.BufferedImage;
import java.awt.image.RenderedImage;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URL;
import java.util.Arrays;
import java.util.Iterator;
import javax.imageio.stream.FileImageInputStream;
import javax.imageio.stream.ImageInputStream;
import javax.imageio.stream.MemoryCacheImageInputStream;
import java.util.zip.CRC32;
import java.util.zip.DataFormatException;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * PS5 AWT-lite ImageIO: PNG only (that is all Minecraft reads and writes: textures, skins, the window icon, screenshots, world icons). The real ImageIO needs AWT natives, ICC colour management and
 * sun.awt.AppContext. read() understands every PNG colour type and bit depth (1/2/4/8/16), palettes with tRNS, gray/RGB colour keys and Adam7 interlacing and returns TYPE_INT_ARGB (alpha present) or TYPE_INT_RGB.
 * write() produces 8-bit RGBA PNGs. read returns null for data that is not a PNG (like the real one for an unknown format); write returns false for formats other than "png".
 */
public final class ImageIO {
    private ImageIO() { }
    private static boolean cache;

    public static void scanForPlugins() { }
    public static void setUseCache(boolean b) { cache = b; }
    public static boolean getUseCache() { return cache; }
    public static void setCacheDirectory(File dir) { }
    public static File getCacheDirectory() { return null; }
    public static String[] getReaderFormatNames() { return new String[] { "png", "PNG" }; }
    public static String[] getWriterFormatNames() { return new String[] { "png", "PNG" }; }
    public static String[] getReaderFileSuffixes() { return new String[] { "png" }; }
    public static String[] getWriterFileSuffixes() { return new String[] { "png" }; }

    public static BufferedImage read(File f) throws IOException {
        if (f == null) throw new IllegalArgumentException("input == null!");
        if (!f.canRead()) throw new IIOException("Can't read input file!");
        try (InputStream in = new FileInputStream(f)) { return read(in); }
    }
    public static BufferedImage read(URL u) throws IOException {
        if (u == null) throw new IllegalArgumentException("input == null!");
        try (InputStream in = u.openStream()) { return read(in); }
    }
    public static BufferedImage read(InputStream in) throws IOException {
        if (in == null) throw new IllegalArgumentException("input == null!");
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(Math.max(in.available(), 8192));   // not InputStream.readAllBytes: this source also builds against Java 8
        byte[] chunk = new byte[16384]; int rn;
        while ((rn = in.read(chunk)) > 0) bo.write(chunk, 0, rn);
        byte[] all = bo.toByteArray();
        return decodePng(all);
    }

    public static BufferedImage read(ImageInputStream st) throws IOException {   // (Vitrail's noise textures, mods that wrap a byte array in a MemoryCacheImageInputStream)
        if (st == null) throw new IllegalArgumentException("stream == null!");
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream(8192);
        byte[] chunk = new byte[16384]; int rn;
        while ((rn = st.read(chunk, 0, chunk.length)) > 0) bo.write(chunk, 0, rn);
        return decodePng(bo.toByteArray());
    }
    public static ImageInputStream createImageInputStream(Object in) throws IOException {
        if (in == null) throw new IllegalArgumentException("input == null!");
        if (in instanceof ImageInputStream) return (ImageInputStream) in;
        if (in instanceof InputStream) return new MemoryCacheImageInputStream((InputStream) in);
        if (in instanceof File) return new FileImageInputStream((File) in);
        return null;
    }
    private static boolean isPngName(String n) { return n != null && "png".equalsIgnoreCase(n); }
    private static Iterator<ImageReader> oneReader() { return Arrays.asList(new ImageReader()).iterator(); }
    private static Iterator<ImageReader> noReader() { return Arrays.<ImageReader>asList().iterator(); }
    /** The PNG reader for a stream that starts with the PNG signature (the stream is left where it was), none otherwise. */
    public static Iterator<ImageReader> getImageReaders(Object input) {
        if (input == null) throw new IllegalArgumentException("input == null!");
        try {
            if (input instanceof ImageInputStream) {
                ImageInputStream st = (ImageInputStream) input; st.mark();
                try { byte[] sig = new byte[4]; st.readFully(sig); return (sig[0] & 0xff) == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G' ? oneReader() : noReader(); }
                finally { st.reset(); }
            }
        } catch (IOException e) { return noReader(); }
        return oneReader();
    }
    public static Iterator<ImageReader> getImageReadersByFormatName(String f) { if (f == null) throw new IllegalArgumentException("formatName == null!"); return isPngName(f) ? oneReader() : noReader(); }
    public static Iterator<ImageReader> getImageReadersBySuffix(String s) { if (s == null) throw new IllegalArgumentException("fileSuffix == null!"); return isPngName(s) ? oneReader() : noReader(); }
    public static Iterator<ImageReader> getImageReadersByMIMEType(String m) { if (m == null) throw new IllegalArgumentException("MIMEType == null!"); return "image/png".equalsIgnoreCase(m) ? oneReader() : noReader(); }

    public static boolean write(RenderedImage im, String format, File f) throws IOException {
        if (f == null) throw new IllegalArgumentException("output == null!");
        if (!"png".equalsIgnoreCase(format)) return false;
        byte[] b = encodePng(im);
        try (OutputStream out = new FileOutputStream(f)) { out.write(b); }
        return true;
    }
    public static boolean write(RenderedImage im, String format, OutputStream out) throws IOException {
        if (out == null) throw new IllegalArgumentException("output == null!");
        if (!"png".equalsIgnoreCase(format)) return false;
        out.write(encodePng(im)); out.flush();
        return true;
    }

    // ---------------------------------------------------------------------------------------- PNG writer
    private static void chunk(ByteArrayOutputStream o, String type, byte[] data, int len) {
        byte[] t = type.getBytes(java.nio.charset.StandardCharsets.ISO_8859_1);
        int2(o, len); o.write(t, 0, 4); o.write(data, 0, len);
        CRC32 c = new CRC32(); c.update(t, 0, 4); c.update(data, 0, len); int2(o, (int) c.getValue());
    }
    private static void int2(ByteArrayOutputStream o, int v) { o.write(v >>> 24); o.write(v >>> 16); o.write(v >>> 8); o.write(v); }
    private static byte[] encodePng(RenderedImage ri) throws IOException {
        if (!(ri instanceof BufferedImage)) throw new IIOException("Unsupported image class " + (ri == null ? "null" : ri.getClass().getName()));
        BufferedImage im = (BufferedImage) ri; int w = im.getWidth(), h = im.getHeight();
        byte[] raw = new byte[h * (1 + 4 * w)]; int p = 0; int[] row = new int[w];
        boolean alpha = im.getTransparency() != java.awt.Transparency.OPAQUE;
        for (int y = 0; y < h; y++) {
            im.getRGB(0, y, w, 1, row, 0, w); raw[p++] = 0;
            for (int x = 0; x < w; x++) { int v = row[x]; raw[p++] = (byte) (v >> 16); raw[p++] = (byte) (v >> 8); raw[p++] = (byte) v; raw[p++] = (byte) (alpha ? v >>> 24 : 255); }
        }
        Deflater d = new Deflater(Deflater.DEFAULT_COMPRESSION); d.setInput(raw); d.finish();
        ByteArrayOutputStream z = new ByteArrayOutputStream(); byte[] buf = new byte[65536];
        while (!d.finished()) { int n = d.deflate(buf); z.write(buf, 0, n); }
        d.end();
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(new byte[] { (byte) 0x89, 'P', 'N', 'G', 13, 10, 26, 10 }, 0, 8);
        ByteArrayOutputStream hd = new ByteArrayOutputStream(); int2(hd, w); int2(hd, h); hd.write(8); hd.write(6); hd.write(0); hd.write(0); hd.write(0);
        byte[] hb = hd.toByteArray(); chunk(o, "IHDR", hb, hb.length);
        byte[] zb = z.toByteArray(); chunk(o, "IDAT", zb, zb.length);
        chunk(o, "IEND", new byte[0], 0);
        return o.toByteArray();
    }

    // ---------------------------------------------------------------------------------------- PNG reader
    private static int be32(byte[] b, int o) { return ((b[o] & 255) << 24) | ((b[o + 1] & 255) << 16) | ((b[o + 2] & 255) << 8) | (b[o + 3] & 255); }

    private static BufferedImage decodePng(byte[] b) throws IOException {
        if (b.length < 8 || (b[0] & 255) != 0x89 || b[1] != 'P' || b[2] != 'N' || b[3] != 'G') return null;
        int pos = 8, w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
        byte[] plte = null, trns = null; ByteArrayOutputStream idat = new ByteArrayOutputStream(); boolean seenHdr = false, end = false;
        while (pos + 8 <= b.length && !end) {
            int len = be32(b, pos); String type = new String(b, pos + 4, 4, java.nio.charset.StandardCharsets.ISO_8859_1); int d = pos + 8;
            if (len < 0 || d + len > b.length) throw new IIOException("Truncated PNG chunk " + type);
            switch (type) {
                case "IHDR": w = be32(b, d); h = be32(b, d + 4); depth = b[d + 8] & 255; ctype = b[d + 9] & 255; interlace = b[d + 12] & 255; seenHdr = true; break;
                case "PLTE": plte = java.util.Arrays.copyOfRange(b, d, d + len); break;
                case "tRNS": trns = java.util.Arrays.copyOfRange(b, d, d + len); break;
                case "IDAT": idat.write(b, d, len); break;
                case "IEND": end = true; break;
                default: break;
            }
            pos = d + len + 4;
        }
        if (!seenHdr || w <= 0 || h <= 0) throw new IIOException("Not a valid PNG: no IHDR");
        int channels; switch (ctype) { case 0: case 3: channels = 1; break; case 2: channels = 3; break; case 4: channels = 2; break; case 6: channels = 4; break; default: throw new IIOException("Bad PNG colour type " + ctype); }
        if (ctype == 3 && plte == null) throw new IIOException("PNG palette image without PLTE");
        int bpp = channels * depth;   // bits per pixel
        boolean hasAlpha = ctype == 4 || ctype == 6 || trns != null;
        int[] pix = new int[w * h];
        Inflater inf = new Inflater(); inf.setInput(idat.toByteArray());
        int[] pw = interlace == 1 ? new int[] { 1, 1, 1, 1, 1, 1, 1 } : null;
        int[][] adam = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };   // x0, y0, dx, dy
        int passes = interlace == 1 ? 7 : 1;
        int bytesPerPixelFilter = Math.max(1, bpp / 8);
        try {
            for (int ps = 0; ps < passes; ps++) {
                int x0 = interlace == 1 ? adam[ps][0] : 0, y0 = interlace == 1 ? adam[ps][1] : 0, dx = interlace == 1 ? adam[ps][2] : 1, dy = interlace == 1 ? adam[ps][3] : 1;
                int pwid = (w - x0 + dx - 1) / dx, phei = (h - y0 + dy - 1) / dy;
                if (pwid <= 0 || phei <= 0) continue;
                int stride = (pwid * bpp + 7) / 8;
                byte[] prev = new byte[stride], cur = new byte[stride]; byte[] ft = new byte[1];
                for (int y = 0; y < phei; y++) {
                    readFully(inf, ft, 1); readFully(inf, cur, stride);
                    int f = ft[0] & 255;
                    for (int i = 0; i < stride; i++) {
                        int a = i >= bytesPerPixelFilter ? cur[i - bytesPerPixelFilter] & 255 : 0, up = prev[i] & 255, c = i >= bytesPerPixelFilter ? prev[i - bytesPerPixelFilter] & 255 : 0, x = cur[i] & 255, r;
                        switch (f) {
                            case 0: r = x; break;
                            case 1: r = x + a; break;
                            case 2: r = x + up; break;
                            case 3: r = x + ((a + up) >> 1); break;
                            case 4: { int pp = a + up - c, pa = Math.abs(pp - a), pb = Math.abs(pp - up), pc = Math.abs(pp - c); r = x + (pa <= pb && pa <= pc ? a : pb <= pc ? up : c); break; }
                            default: throw new IIOException("Bad PNG filter " + f);
                        }
                        cur[i] = (byte) r;
                    }
                    for (int x = 0; x < pwid; x++) pix[(y0 + y * dy) * w + (x0 + x * dx)] = pixel(cur, x, ctype, depth, plte, trns);
                    byte[] t = prev; prev = cur; cur = t;
                }
            }
        } catch (DataFormatException e) { throw new IIOException("PNG data error: " + e.getMessage()); }
        finally { inf.end(); }
        BufferedImage im = new BufferedImage(w, h, hasAlpha ? BufferedImage.TYPE_INT_ARGB : BufferedImage.TYPE_INT_RGB);
        im.setRGB(0, 0, w, h, pix, 0, w);
        return im;
    }
    private static void readFully(Inflater inf, byte[] out, int n) throws DataFormatException, IOException {
        int got = 0;
        while (got < n) {
            int r = inf.inflate(out, got, n - got);
            if (r == 0) { if (inf.finished() || inf.needsInput() || inf.needsDictionary()) throw new EOFException("PNG image data ends early"); }
            got += r;
        }
    }
    private static int sample(byte[] row, int index, int depth) {   // index-th sample of the row, depth 1/2/4/8/16 (16 -> full 16 bit value)
        switch (depth) {
            case 8: return row[index] & 255;
            case 16: return ((row[2 * index] & 255) << 8) | (row[2 * index + 1] & 255);
            default: { int perByte = 8 / depth, v = row[index / perByte] & 255, shift = 8 - depth * (index % perByte + 1); return (v >> shift) & ((1 << depth) - 1); }
        }
    }
    private static int scale8(int v, int depth) { return depth == 8 ? v : depth == 16 ? v >> 8 : v * 255 / ((1 << depth) - 1); }
    private static int pixel(byte[] row, int x, int ctype, int depth, byte[] plte, byte[] trns) {
        switch (ctype) {
            case 0: { int v = sample(row, x, depth), g = scale8(v, depth); int a = 255; if (trns != null && trns.length >= 2 && v == (((trns[0] & 255) << 8) | (trns[1] & 255))) a = 0; return (a << 24) | (g << 16) | (g << 8) | g; }
            case 2: {
                int r = sample(row, 3 * x, depth), g = sample(row, 3 * x + 1, depth), bl = sample(row, 3 * x + 2, depth); int a = 255;
                if (trns != null && trns.length >= 6 && r == (((trns[0] & 255) << 8) | (trns[1] & 255)) && g == (((trns[2] & 255) << 8) | (trns[3] & 255)) && bl == (((trns[4] & 255) << 8) | (trns[5] & 255))) a = 0;
                return (a << 24) | (scale8(r, depth) << 16) | (scale8(g, depth) << 8) | scale8(bl, depth);
            }
            case 3: { int i = sample(row, x, depth); if (i * 3 + 2 >= plte.length) return 0xFF000000; int a = trns != null && i < trns.length ? trns[i] & 255 : 255; return (a << 24) | ((plte[3 * i] & 255) << 16) | ((plte[3 * i + 1] & 255) << 8) | (plte[3 * i + 2] & 255); }
            case 4: { int g = scale8(sample(row, 2 * x, depth), depth), a = scale8(sample(row, 2 * x + 1, depth), depth); return (a << 24) | (g << 16) | (g << 8) | g; }
            default: { int r = scale8(sample(row, 4 * x, depth), depth), g = scale8(sample(row, 4 * x + 1, depth), depth), bl = scale8(sample(row, 4 * x + 2, depth), depth), a = scale8(sample(row, 4 * x + 3, depth), depth); return (a << 24) | (r << 16) | (g << 8) | bl; }
        }
    }
}
