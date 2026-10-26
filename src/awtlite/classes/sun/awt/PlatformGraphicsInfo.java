package sun.awt;

import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.GraphicsDevice;
import java.awt.GraphicsEnvironment;
import java.awt.HeadlessException;
import java.awt.Toolkit;
import java.awt.image.BufferedImage;
import java.util.Locale;

/** PS5 stub: there is no AWT (no natives, no display). Everything reports "headless"; images are drawn in software by PS5Graphics2D. */
public class PlatformGraphicsInfo {
    public static GraphicsEnvironment createGE() {
        return new GraphicsEnvironment() {
            public GraphicsDevice[] getScreenDevices() throws HeadlessException { throw new HeadlessException(); }
            public GraphicsDevice getDefaultScreenDevice() throws HeadlessException { throw new HeadlessException(); }
            public Graphics2D createGraphics(BufferedImage img) { return new PS5Graphics2D(img); }
            public Font[] getAllFonts() { return new Font[0]; }
            public String[] getAvailableFontFamilyNames() { return new String[0]; }
            public String[] getAvailableFontFamilyNames(Locale l) { return new String[0]; }
        };
    }
    public static Toolkit createToolkit() { return new PS5Toolkit(); }
    public static boolean getDefaultHeadlessProperty() { return true; }
    public static String getDefaultHeadlessMessage() { return "This platform has no AWT support."; }
}
