package java.awt.image;

import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.Image;
import java.awt.Point;
import java.awt.Rectangle;
import java.awt.Transparency;
import java.util.Vector;

/**
 * PS5 AWT-lite: a BufferedImage that keeps its pixels as ARGB ints. The real one builds a ColorModel, whose sRGB colour space needs the ICC / LittleCMS natives of java.desktop (not in the image).
 * What Minecraft (1.12.2 and older) uses: constructors, getWidth / getHeight / getType, getRGB / setRGB (single and area), getSubimage (shares the pixels), getRaster().getDataBuffer() as DataBufferInt
 * (ImageBufferDownload), getGraphics / createGraphics (sun.awt.PS5Graphics2D), and use as a RenderedImage (ImageIO.write). Types without alpha store 0xFF in the alpha byte.
 */
public class BufferedImage extends Image implements WritableRenderedImage, Transparency {
    public static final int TYPE_CUSTOM = 0, TYPE_INT_RGB = 1, TYPE_INT_ARGB = 2, TYPE_INT_ARGB_PRE = 3, TYPE_INT_BGR = 4, TYPE_3BYTE_BGR = 5, TYPE_4BYTE_ABGR = 6, TYPE_4BYTE_ABGR_PRE = 7,
        TYPE_USHORT_565_RGB = 8, TYPE_USHORT_555_RGB = 9, TYPE_BYTE_GRAY = 10, TYPE_USHORT_GRAY = 11, TYPE_BYTE_BINARY = 12, TYPE_BYTE_INDEXED = 13;

    private final int width, height, type, offset, stride;
    private final int[] data;
    private final boolean hasAlpha;
    private WritableRaster raster;

    public BufferedImage(int width, int height, int imageType) {
        if (width <= 0 || height <= 0) throw new IllegalArgumentException("Width (" + width + ") and height (" + height + ") cannot be <= 0");
        if (imageType < 1 || imageType > 13) throw new IllegalArgumentException("Unknown image type " + imageType);
        this.width = width; this.height = height; this.type = imageType; this.offset = 0; this.stride = width;
        this.hasAlpha = imageType == TYPE_INT_ARGB || imageType == TYPE_INT_ARGB_PRE || imageType == TYPE_4BYTE_ABGR || imageType == TYPE_4BYTE_ABGR_PRE;
        this.data = new int[Math.multiplyExact(width, height)];
        if (!hasAlpha) java.util.Arrays.fill(data, 0xFF000000);
    }
    private BufferedImage(BufferedImage base, int x, int y, int w, int h) {
        this.width = w; this.height = h; this.type = base.type; this.hasAlpha = base.hasAlpha; this.data = base.data; this.stride = base.stride; this.offset = base.offset + y * base.stride + x;
    }

    public int getType() { return type; }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getWidth(ImageObserver o) { return width; }
    public int getHeight(ImageObserver o) { return height; }
    public int getMinX() { return 0; }
    public int getMinY() { return 0; }
    public ImageProducer getSource() { throw new UnsupportedOperationException("BufferedImage.getSource"); }
    public Object getProperty(String name, ImageObserver o) { return getProperty(name); }
    public Object getProperty(String name) { if (name == null) throw new NullPointerException("null property name is not allowed"); return Image.UndefinedProperty; }
    public String[] getPropertyNames() { return null; }
    public Vector<RenderedImage> getSources() { return null; }
    public ColorModel getColorModel() { return null; }
    public SampleModel getSampleModel() { return getRaster().getSampleModel(); }
    public int getTransparency() { return hasAlpha ? Transparency.TRANSLUCENT : Transparency.OPAQUE; }
    public boolean isAlphaPremultiplied() { return type == TYPE_INT_ARGB_PRE || type == TYPE_4BYTE_ABGR_PRE; }
    public WritableRaster getAlphaRaster() { return null; }
    public void flush() { }

    private void check(int x, int y) { if (x < 0 || y < 0 || x >= width || y >= height) throw new ArrayIndexOutOfBoundsException("Coordinate out of bounds!"); }
    public int getRGB(int x, int y) { check(x, y); return data[offset + y * stride + x]; }
    public void setRGB(int x, int y, int rgb) { check(x, y); data[offset + y * stride + x] = hasAlpha ? rgb : rgb | 0xFF000000; }
    public int[] getRGB(int startX, int startY, int w, int h, int[] rgbArray, int offset, int scansize) {
        if (w < 0 || h < 0 || startX < 0 || startY < 0 || startX + w > width || startY + h > height) throw new ArrayIndexOutOfBoundsException("Coordinate out of bounds!");
        if (rgbArray == null) rgbArray = new int[offset + h * scansize];
        for (int y = 0; y < h; y++) System.arraycopy(data, this.offset + (startY + y) * stride + startX, rgbArray, offset + y * scansize, w);
        return rgbArray;
    }
    public void setRGB(int startX, int startY, int w, int h, int[] rgbArray, int offset, int scansize) {
        if (w < 0 || h < 0 || startX < 0 || startY < 0 || startX + w > width || startY + h > height) throw new ArrayIndexOutOfBoundsException("Coordinate out of bounds!");
        for (int y = 0; y < h; y++) {
            int s = offset + y * scansize, d = this.offset + (startY + y) * stride + startX;
            if (hasAlpha) System.arraycopy(rgbArray, s, data, d, w); else for (int x = 0; x < w; x++) data[d + x] = rgbArray[s + x] | 0xFF000000;
        }
    }
    public BufferedImage getSubimage(int x, int y, int w, int h) {
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > width || y + h > height) throw new java.awt.image.RasterFormatException("(x + width) is outside of Raster");
        return new BufferedImage(this, x, y, w, h);
    }

    /** The pixels as a packed ARGB raster (DataBufferInt over the same int[]): Minecraft's ImageBufferDownload edits skins through ((DataBufferInt) getRaster().getDataBuffer()).getData(). Whole images only. */
    public WritableRaster getRaster() {
        if (raster == null) {
            if (offset != 0 || stride != width) throw new UnsupportedOperationException("raster of a sub-image");
            DataBufferInt db = new DataBufferInt(data, data.length);
            raster = Raster.createPackedRaster(db, width, height, width, hasAlpha ? new int[] { 0xFF0000, 0xFF00, 0xFF, 0xFF000000 } : new int[] { 0xFF0000, 0xFF00, 0xFF }, null);
        }
        return raster;
    }
    public Raster getData() { return getRaster(); }
    public Raster getData(Rectangle rect) { return getRaster().createChild(rect.x, rect.y, rect.width, rect.height, rect.x, rect.y, null); }
    public WritableRaster copyData(WritableRaster outRaster) { throw new UnsupportedOperationException("BufferedImage.copyData"); }
    public void setData(Raster r) { throw new UnsupportedOperationException("BufferedImage.setData"); }

    public Graphics getGraphics() { return createGraphics(); }
    public Graphics2D createGraphics() { return new sun.awt.PS5Graphics2D(this); }

    // RenderedImage / WritableRenderedImage: single tile
    public int getNumXTiles() { return 1; }
    public int getNumYTiles() { return 1; }
    public int getMinTileX() { return 0; }
    public int getMinTileY() { return 0; }
    public int getTileWidth() { return width; }
    public int getTileHeight() { return height; }
    public int getTileGridXOffset() { return 0; }
    public int getTileGridYOffset() { return 0; }
    public Raster getTile(int tileX, int tileY) { if (tileX != 0 || tileY != 0) throw new ArrayIndexOutOfBoundsException("BufferedImages only have one tile with index 0,0"); return getRaster(); }
    public WritableRaster getWritableTile(int tileX, int tileY) { return (WritableRaster) getTile(tileX, tileY); }
    public Point[] getWritableTileIndices() { return new Point[] { new Point(0, 0) }; }
    public boolean hasTileWriters() { return true; }
    public boolean isTileWritable(int tileX, int tileY) { return true; }
    public void addTileObserver(TileObserver to) { }
    public void removeTileObserver(TileObserver to) { }
    public void releaseWritableTile(int tileX, int tileY) { }
    public String toString() { return "BufferedImage@" + Integer.toHexString(hashCode()) + ": type = " + type + " " + width + "x" + height + " (PS5 AWT-lite)"; }
}
