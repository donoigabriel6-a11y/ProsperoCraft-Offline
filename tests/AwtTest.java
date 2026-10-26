import java.awt.*;
import java.awt.image.*;
import java.io.*;
import java.util.*;
import java.util.zip.CRC32;
import javax.imageio.ImageIO;
import javax.script.*;

/** Runs on the real JDK (reference) and on the minimal image with AWT-lite (tests/awt_test.sh); both print the same lines for the same input. args: fixtures dir, out dir */
public class AwtTest {
    static String hash(int[] a) { CRC32 c = new CRC32(); for (int v : a) { c.update(v >>> 24); c.update(v >>> 16); c.update(v >>> 8); c.update(v); } return Long.toHexString(c.getValue()); }
    static String hash(BufferedImage im) { return hash(im.getRGB(0, 0, im.getWidth(), im.getHeight(), null, 0, im.getWidth())); }

    /** ImageBufferDownload.parseUserSkin of Minecraft 1.12.2 (decompiled shape), on a synthetic skin */
    static BufferedImage skin(BufferedImage image) {
        int imageWidth = 64, imageHeight = 64;
        BufferedImage bufferedimage = new BufferedImage(64, 64, 2);
        Graphics graphics = bufferedimage.getGraphics();
        graphics.drawImage(image, 0, 0, null);
        if (image.getHeight() == 32) {
            graphics.setColor(new Color(0, 0, 0, 0));
            graphics.fillRect(0, 32, 64, 32);
            graphics.drawImage(bufferedimage, 24, 48, 20, 52, 4, 16, 8, 20, null);
            graphics.drawImage(bufferedimage, 28, 48, 24, 52, 8, 16, 12, 20, null);
            graphics.drawImage(bufferedimage, 20, 52, 16, 64, 8, 20, 12, 32, null);
            graphics.drawImage(bufferedimage, 24, 52, 20, 64, 4, 20, 8, 32, null);
            graphics.drawImage(bufferedimage, 28, 52, 24, 64, 0, 20, 4, 32, null);
            graphics.drawImage(bufferedimage, 32, 52, 28, 64, 12, 20, 16, 32, null);
            graphics.drawImage(bufferedimage, 40, 48, 36, 52, 44, 16, 48, 20, null);
            graphics.drawImage(bufferedimage, 44, 48, 40, 52, 48, 16, 52, 20, null);
            graphics.drawImage(bufferedimage, 36, 52, 32, 64, 48, 20, 52, 32, null);
            graphics.drawImage(bufferedimage, 40, 52, 36, 64, 44, 20, 48, 32, null);
            graphics.drawImage(bufferedimage, 44, 52, 40, 64, 40, 20, 44, 32, null);
            graphics.drawImage(bufferedimage, 48, 52, 44, 64, 52, 20, 56, 32, null);
        }
        graphics.dispose();
        int[] aint = ((DataBufferInt) bufferedimage.getRaster().getDataBuffer()).getData();
        setAreaOpaque(aint, 0, 0, 32, 16, 64);
        setAreaTransparent(aint, 32, 0, 64, 32, 64);
        setAreaOpaque(aint, 0, 16, 64, 32, 64);
        setAreaTransparent(aint, 0, 32, 16, 48, 64);
        return bufferedimage;
    }
    static void setAreaTransparent(int[] a, int x, int y, int x2, int y2, int w) { for (int i = x; i < x2; ++i) for (int j = y; j < y2; ++j) { int k = a[i + j * w]; if ((k >> 24 & 255) < 128) return; } for (int i = x; i < x2; ++i) for (int j = y; j < y2; ++j) a[i + j * w] &= 16777215; }
    static void setAreaOpaque(int[] a, int x, int y, int x2, int y2, int w) { for (int i = x; i < x2; ++i) for (int j = y; j < y2; ++j) a[i + j * w] |= -16777216; }

    static BufferedImage pattern(int w, int h, boolean alpha, long seed) {
        BufferedImage im = new BufferedImage(w, h, alpha ? BufferedImage.TYPE_INT_ARGB : BufferedImage.TYPE_INT_RGB); Random r = new Random(seed);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int a = alpha ? (r.nextInt(4) == 0 ? 0 : 255) : 255; im.setRGB(x, y, (a << 24) | (r.nextInt(256) << 16) | (r.nextInt(256) << 8) | r.nextInt(256)); }
        return im;
    }

    public static void main(String[] args) throws Exception {
        File fx = new File(args[0]), out = new File(args[1]); out.mkdirs();
        // 1. PNG fixtures written by tests/awt_fixtures.py (expected ARGB values are in <name>.expect)
        File[] fs = fx.listFiles((d, n) -> n.endsWith(".png")); Arrays.sort(fs);
        for (File f : fs) {
            BufferedImage im = ImageIO.read(f);
            System.out.println("png " + f.getName() + " " + (im == null ? "null" : im.getWidth() + "x" + im.getHeight() + " " + hash(im)));
        }
        // 2. skin conversion (64x32 -> 64x64) and a 64x64 skin
        BufferedImage s32 = pattern(64, 32, true, 7), s64 = pattern(64, 64, true, 9);
        System.out.println("skin32 " + hash(skin(s32)));
        System.out.println("skin64 " + hash(skin(s64)));
        // 3. blending: src-over of semi-transparent pixels (exact match not required: reported as the max channel difference by awt_test.sh)
        BufferedImage dst = pattern(16, 16, true, 3), src = new BufferedImage(16, 16, 2);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) src.setRGB(x, y, ((x * 16) << 24) | (y * 16 << 16) | (x * 8 << 8) | 0x40);
        Graphics2D g = dst.createGraphics(); g.drawImage(src, 0, 0, null); g.dispose();
        StringBuilder sb = new StringBuilder("blend"); for (int i = 0; i < 256; i++) sb.append(' ').append(Integer.toHexString(dst.getRGB(i % 16, i / 16)));
        System.out.println(sb);
        // 4. scaled / flipped draws
        BufferedImage a = pattern(8, 8, true, 11), b = new BufferedImage(16, 16, 2);
        Graphics2D g2 = b.createGraphics(); g2.drawImage(a, 0, 0, 16, 16, null); g2.drawImage(a, 16, 0, 8, 8, 0, 0, 8, 8, null); g2.drawImage(a, 0, 8, 8, 16, 8, 0, 0, 8, null); g2.dispose();
        System.out.println("scale " + hash(b));
        // 5. sub-images share pixels; raster edits show up
        BufferedImage m = pattern(8, 8, false, 5); BufferedImage sub = m.getSubimage(2, 2, 4, 4); sub.setRGB(0, 0, 0x123456);
        System.out.println("sub " + Integer.toHexString(m.getRGB(2, 2)) + " " + hash(sub) + " type " + m.getType());
        int[] px = ((DataBufferInt) m.getRaster().getDataBuffer()).getData(); px[0] = 0xFF010203;
        System.out.println("raster " + Integer.toHexString(m.getRGB(0, 0)));
        // 6. write PNG (read back by the reference JDK in awt_test.sh)
        BufferedImage w = pattern(33, 17, true, 21); ImageIO.write(w, "png", new File(out, "written.png"));
        ByteArrayOutputStream bo = new ByteArrayOutputStream(); ImageIO.write(pattern(5, 5, false, 2), "png", bo);
        System.out.println("written " + hash(w) + " " + (bo.size() > 40) + " " + ImageIO.write(w, "bmp-not-supported-here", new ByteArrayOutputStream()));
        // 7. Color + javax.script
        System.out.println("color " + Integer.toHexString(new Color(10, 20, 30, 40).getRGB()) + " " + Integer.toHexString(Color.HSBtoRGB(0.3f, 0.5f, 0.8f)));
        ScriptEngineManager sem = new ScriptEngineManager();
        System.out.println("script " + (sem.getEngineByName("nashorn-not-here") == null));
        // 8. java.datatransfer + Toolkit (Minecraft 1.12.2: GuiScreen.getClipboardString -> Toolkit.getDefaultToolkit().getSystemClipboard() inside try/catch HeadlessException; bib.aq loads Transferable)
        String clip;
        try { java.awt.Toolkit.getDefaultToolkit().getSystemClipboard(); clip = "no exception"; } catch (java.awt.HeadlessException e) { clip = "HeadlessException"; }
        java.awt.datatransfer.StringSelection ss = new java.awt.datatransfer.StringSelection("hello");
        String txt;
        try { txt = (String) ss.getTransferData(java.awt.datatransfer.DataFlavor.stringFlavor); } catch (Exception e) { txt = "ERR " + e; }
        java.awt.datatransfer.Clipboard cb = new java.awt.datatransfer.Clipboard("t"); cb.setContents(ss, null);
        System.out.println("clip " + clip + " " + txt + " " + ss.isDataFlavorSupported(java.awt.datatransfer.DataFlavor.stringFlavor) + " " + cb.getContents(null).isDataFlavorSupported(java.awt.datatransfer.DataFlavor.stringFlavor)
            + " " + java.awt.GraphicsEnvironment.isHeadless() + " " + new java.awt.datatransfer.DataFlavor("text/plain; charset=utf-8").getMimeType());
        System.out.println("done");
    }
}
