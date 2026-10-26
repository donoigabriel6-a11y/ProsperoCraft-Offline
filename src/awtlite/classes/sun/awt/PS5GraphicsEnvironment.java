package sun.awt;

import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.GraphicsDevice;
import java.awt.GraphicsEnvironment;
import java.awt.HeadlessException;
import java.awt.image.BufferedImage;
import java.util.Locale;

/** Java 8 only (-Djava.awt.graphicsenv=sun.awt.PS5GraphicsEnvironment, jvm_host.cpp): Java 8 has no PlatformGraphicsInfo, it creates the environment named by that property. Same body as PlatformGraphicsInfo.createGE. */
public class PS5GraphicsEnvironment extends GraphicsEnvironment {
    public GraphicsDevice[] getScreenDevices() throws HeadlessException { throw new HeadlessException(); }
    public GraphicsDevice getDefaultScreenDevice() throws HeadlessException { throw new HeadlessException(); }
    public Graphics2D createGraphics(BufferedImage img) { return new PS5Graphics2D(img); }
    public Font[] getAllFonts() { return new Font[0]; }
    public String[] getAvailableFontFamilyNames() { return new String[0]; }
    public String[] getAvailableFontFamilyNames(Locale l) { return new String[0]; }
}
