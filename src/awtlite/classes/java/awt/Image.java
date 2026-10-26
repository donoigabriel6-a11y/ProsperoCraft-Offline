package java.awt;

import java.awt.image.ImageObserver;
import java.awt.image.ImageProducer;

/** PS5 AWT-lite: java.awt.Image without the Toolkit / native initialisation of the real one (java.desktop's natives are not in the image). */
public abstract class Image {
    public static final Object UndefinedProperty = new Object();
    public static final int SCALE_DEFAULT = 1, SCALE_FAST = 2, SCALE_SMOOTH = 4, SCALE_REPLICATE = 8, SCALE_AREA_AVERAGING = 16;
    public abstract int getWidth(ImageObserver observer);
    public abstract int getHeight(ImageObserver observer);
    public abstract ImageProducer getSource();
    public abstract Graphics getGraphics();
    public abstract Object getProperty(String name, ImageObserver observer);
    public Image getScaledInstance(int width, int height, int hints) { throw new UnsupportedOperationException("Image.getScaledInstance"); }
    public void flush() { }
}
