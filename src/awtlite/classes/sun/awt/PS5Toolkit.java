package sun.awt;

import java.awt.*;
import java.awt.image.*;

/** generated: every abstract method of java.awt.Toolkit with a do-nothing body. A Toolkit object is only needed so that Toolkit.getDefaultToolkit() works; it is wrapped in the HeadlessToolkit (clipboard etc. throw HeadlessException, which Minecraft catches). */
public class PS5Toolkit extends Toolkit {
    public void beep() { }
    public int checkImage(java.awt.Image p0, int p1, int p2, java.awt.image.ImageObserver p3) { return 0; }
    public java.awt.Image createImage(byte[] p0, int p1, int p2) { return null; }
    public java.awt.Image createImage(java.awt.image.ImageProducer p0) { return null; }
    public java.awt.Image createImage(java.lang.String p0) { return null; }
    public java.awt.Image createImage(java.net.URL p0) { return null; }
    public java.awt.image.ColorModel getColorModel() { return null; }
    public java.lang.String[] getFontList() { return null; }
    public java.awt.FontMetrics getFontMetrics(java.awt.Font p0) { return null; }
    public java.awt.Image getImage(java.lang.String p0) { return null; }
    public java.awt.Image getImage(java.net.URL p0) { return null; }
    public java.awt.PrintJob getPrintJob(java.awt.Frame p0, java.lang.String p1, java.util.Properties p2) { return null; }
    public int getScreenResolution() { return 96; }
    public java.awt.Dimension getScreenSize() { return new Dimension(1920, 1080); }
    public java.awt.datatransfer.Clipboard getSystemClipboard() { return null; }
    protected java.awt.EventQueue getSystemEventQueueImpl() { return null; }
    public boolean isModalExclusionTypeSupported(java.awt.Dialog.ModalExclusionType p0) { return false; }
    public boolean isModalityTypeSupported(java.awt.Dialog.ModalityType p0) { return false; }
    public java.util.Map mapInputMethodHighlight(java.awt.im.InputMethodHighlight p0) { return null; }
    public boolean prepareImage(java.awt.Image p0, int p1, int p2, java.awt.image.ImageObserver p3) { return false; }
    public void sync() { }
}
