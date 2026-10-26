package javax.imageio;

import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import javax.imageio.stream.ImageInputStream;

/**
 * PS5 AWT-lite ImageReader: the one reader ImageIO.getImageReaders* hands out (PNG). Code that picks a reader itself (reader.setInput(stream); reader.read(0), getWidth/getHeight)
 * gets the same decoder as ImageIO.read. The real javax.imageio.ImageReader is an SPI plug-in framework that needs ImageTypeSpecifier / IIOMetadata / ColorModel, none of which run here.
 */
public class ImageReader {
    protected Object input;
    private BufferedImage cached;
    public ImageReader() { }
    public void setInput(Object in) { setInput(in, false, false); }
    public void setInput(Object in, boolean seekForwardOnly) { setInput(in, seekForwardOnly, false); }
    public void setInput(Object in, boolean seekForwardOnly, boolean ignoreMetadata) { input = in; cached = null; }
    public Object getInput() { return input; }
    public String getFormatName() throws IOException { return "png"; }
    public int getNumImages(boolean allowSearch) throws IOException { return 1; }
    public int getWidth(int imageIndex) throws IOException { return img().getWidth(); }
    public int getHeight(int imageIndex) throws IOException { return img().getHeight(); }
    public BufferedImage read(int imageIndex) throws IOException { return img(); }
    public BufferedImage read(int imageIndex, ImageReadParam param) throws IOException { return img(); }
    public boolean canReadRaster() { return false; }
    public void abort() { }
    public void reset() { input = null; cached = null; }
    public void dispose() { input = null; cached = null; }
    private BufferedImage img() throws IOException {
        if (cached != null) return cached;
        if (input == null) throw new IllegalStateException("Input not set!");
        BufferedImage b;
        if (input instanceof ImageInputStream) b = ImageIO.read((ImageInputStream) input);
        else if (input instanceof InputStream) b = ImageIO.read((InputStream) input);
        else if (input instanceof File) b = ImageIO.read((File) input);
        else throw new IllegalArgumentException("Unsupported input " + input.getClass().getName());
        if (b == null) throw new IIOException("Unsupported Image Type");
        cached = b;
        return b;
    }
}
