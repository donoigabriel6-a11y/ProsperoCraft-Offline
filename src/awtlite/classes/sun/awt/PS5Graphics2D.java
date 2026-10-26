package sun.awt;

import java.awt.AlphaComposite;
import java.awt.Color;
import java.awt.Composite;
import java.awt.Image;
import java.awt.Paint;
import java.awt.Rectangle;
import java.awt.Shape;
import java.awt.image.BufferedImage;
import java.awt.image.ImageObserver;

/**
 * PS5 AWT-lite: the Graphics2D of a BufferedImage, in software: what Minecraft 1.12.2 and older do with it (skin conversion in ImageBufferDownload, layered horse / armour textures in LayeredTexture):
 * setColor / fillRect / clearRect and drawImage (1:1, scaled, flipped) with the SrcOver (default) or Src composite, nearest neighbour, optional clip rectangle. Everything else is a no-op (PS5GraphicsStubs).
 */
public final class PS5Graphics2D extends PS5GraphicsStubs {
    private final BufferedImage img;
    private Color color = Color.WHITE, background = new Color(0, 0, 0, 0);
    private Paint paint = Color.WHITE;
    private Composite composite = AlphaComposite.SrcOver;
    private int tx, ty;
    private Rectangle clip;   // device space

    public PS5Graphics2D(BufferedImage img) { this.img = img; }

    public java.awt.Graphics create() {
        PS5Graphics2D g = new PS5Graphics2D(img);
        g.color = color; g.background = background; g.paint = paint; g.composite = composite; g.tx = tx; g.ty = ty; g.clip = clip == null ? null : new Rectangle(clip);
        return g;
    }
    public void dispose() { }

    public void setColor(Color c) { if (c != null) { color = c; paint = c; } }
    public Color getColor() { return color; }
    public void setPaint(Paint p) { if (p instanceof Color) setColor((Color) p); else if (p != null) paint = p; }
    public Paint getPaint() { return paint; }
    public void setBackground(Color c) { background = c; }
    public Color getBackground() { return background; }
    public void setComposite(Composite c) { if (c == null) throw new IllegalArgumentException("null Composite"); composite = c; }
    public Composite getComposite() { return composite; }
    public void translate(int x, int y) { tx += x; ty += y; }
    public void translate(double x, double y) { tx += (int) Math.round(x); ty += (int) Math.round(y); }
    public void setClip(int x, int y, int w, int h) { clip = new Rectangle(x + tx, y + ty, w, h); }
    public void clipRect(int x, int y, int w, int h) { Rectangle r = new Rectangle(x + tx, y + ty, w, h); clip = clip == null ? r : clip.intersection(r); }
    public void setClip(Shape s) { if (s == null) clip = null; else { Rectangle b = s.getBounds(); clip = new Rectangle(b.x + tx, b.y + ty, b.width, b.height); } }
    public Shape getClip() { return clip == null ? null : new Rectangle(clip.x - tx, clip.y - ty, clip.width, clip.height); }
    public Rectangle getClipBounds() { return clip == null ? null : new Rectangle(clip.x - tx, clip.y - ty, clip.width, clip.height); }

    private int rule() { return composite instanceof AlphaComposite ? ((AlphaComposite) composite).getRule() : AlphaComposite.SRC_OVER; }
    private float extra() { return composite instanceof AlphaComposite ? ((AlphaComposite) composite).getAlpha() : 1f; }

    /** Writes one pixel (device space) with the current composite. src is non-premultiplied ARGB. */
    private void put(int x, int y, int src) {
        if (x < 0 || y < 0 || x >= img.getWidth() || y >= img.getHeight()) return;
        if (clip != null && !clip.contains(x, y)) return;
        int r = rule(); float ea = extra();
        int sa = src >>> 24;
        if (ea < 1f) sa = Math.round(sa * ea);
        if (r == AlphaComposite.SRC) { img.setRGB(x, y, (sa << 24) | (src & 0xFFFFFF)); return; }
        if (r == AlphaComposite.CLEAR) { img.setRGB(x, y, 0); return; }
        if (r != AlphaComposite.SRC_OVER) throw new UnsupportedOperationException("AlphaComposite rule " + r);
        if (sa == 0) return;
        if (sa == 255) { img.setRGB(x, y, 0xFF000000 | (src & 0xFFFFFF)); return; }
        int dst = img.getRGB(x, y), da = dst >>> 24;
        // non-premultiplied source-over: ra = sa + da (1 - sa); rc = (sc sa + dc da (1 - sa)) / ra
        int ra255 = sa * 255 + da * (255 - sa);   // ra scaled by 255*255
        if (ra255 == 0) return;
        int out = Math.round(ra255 / 255f) << 24;
        for (int sh = 16; sh >= 0; sh -= 8) {
            int sc = (src >> sh) & 255, dc = (dst >> sh) & 255;
            int num = sc * sa * 255 + dc * da * (255 - sa);
            out |= Math.min(255, Math.round(num / (float) ra255)) << sh;
        }
        img.setRGB(x, y, out);
    }

    public void fillRect(int x, int y, int w, int h) {
        int c = color.getRGB();
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put(x + tx + i, y + ty + j, c);
    }
    public void clearRect(int x, int y, int w, int h) {
        Composite old = composite; composite = AlphaComposite.Src; int c = background.getRGB();
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put(x + tx + i, y + ty + j, c);
        composite = old;
    }
    public void drawLine(int x1, int y1, int x2, int y2) {
        int c = color.getRGB(), dx = Math.abs(x2 - x1), dy = Math.abs(y2 - y1), sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx - dy;
        for (;;) { put(x1 + tx, y1 + ty, c); if (x1 == x2 && y1 == y2) break; int e2 = 2 * err; if (e2 > -dy) { err -= dy; x1 += sx; } if (e2 < dx) { err += dx; y1 += sy; } }
    }
    public void drawRect(int x, int y, int w, int h) { if (w < 0 || h < 0) return; drawLine(x, y, x + w, y); drawLine(x + w, y, x + w, y + h); drawLine(x + w, y + h, x, y + h); drawLine(x, y + h, x, y); }

    public boolean drawImage(Image im, int x, int y, ImageObserver o) { return im != null && drawScaled(im, x, y, x + im.getWidth(null), y + im.getHeight(null), 0, 0, im.getWidth(null), im.getHeight(null), null); }
    public boolean drawImage(Image im, int x, int y, Color bg, ImageObserver o) { return im != null && drawScaled(im, x, y, x + im.getWidth(null), y + im.getHeight(null), 0, 0, im.getWidth(null), im.getHeight(null), bg); }
    public boolean drawImage(Image im, int x, int y, int w, int h, ImageObserver o) { return im != null && drawScaled(im, x, y, x + w, y + h, 0, 0, im.getWidth(null), im.getHeight(null), null); }
    public boolean drawImage(Image im, int x, int y, int w, int h, Color bg, ImageObserver o) { return im != null && drawScaled(im, x, y, x + w, y + h, 0, 0, im.getWidth(null), im.getHeight(null), bg); }
    public boolean drawImage(Image im, int dx1, int dy1, int dx2, int dy2, int sx1, int sy1, int sx2, int sy2, ImageObserver o) { return drawScaled(im, dx1, dy1, dx2, dy2, sx1, sy1, sx2, sy2, null); }
    public boolean drawImage(Image im, int dx1, int dy1, int dx2, int dy2, int sx1, int sy1, int sx2, int sy2, Color bg, ImageObserver o) { return drawScaled(im, dx1, dy1, dx2, dy2, sx1, sy1, sx2, sy2, bg); }

    private boolean drawScaled(Image im, int dx1, int dy1, int dx2, int dy2, int sx1, int sy1, int sx2, int sy2, Color bg) {
        if (!(im instanceof BufferedImage)) return false;
        BufferedImage s = (BufferedImage) im;
        int dw = dx2 - dx1, dh = dy2 - dy1;
        if (dw == 0 || dh == 0 || sx1 == sx2 || sy1 == sy2) return true;
        // corner (sx1, sy1) maps to (dx1, dy1) and (sx2, sy2) to (dx2, dy2): a destination corner that comes before the first one means a mirrored copy. Nearest neighbour at pixel centres.
        int xa = Math.min(dx1, dx2), xb = Math.max(dx1, dx2), ya = Math.min(dy1, dy2), yb = Math.max(dy1, dy2);
        for (int Y = ya; Y < yb; Y++) {
            int sy = (int) Math.floor(sy1 + (Y + 0.5 - dy1) / dh * (sy2 - sy1));
            if (sy < 0 || sy >= s.getHeight()) continue;
            for (int X = xa; X < xb; X++) {
                int sx = (int) Math.floor(sx1 + (X + 0.5 - dx1) / dw * (sx2 - sx1));
                if (sx < 0 || sx >= s.getWidth()) continue;
                int px = s.getRGB(sx, sy);
                if (bg != null) put(X + tx, Y + ty, bg.getRGB());
                put(X + tx, Y + ty, px);
            }
        }
        return true;
    }
}
