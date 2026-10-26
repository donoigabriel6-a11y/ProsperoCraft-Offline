package sun.awt;

import java.awt.*;
import java.awt.image.*;

/** generated: every abstract method of java.awt.Graphics / Graphics2D with a do-nothing body; sun.awt.PS5Graphics2D overrides what it supports. */
abstract class PS5GraphicsStubs extends Graphics2D {
    public void addRenderingHints(java.util.Map p0) { }
    public void clearRect(int p0, int p1, int p2, int p3) { }
    public void clip(java.awt.Shape p0) { }
    public void clipRect(int p0, int p1, int p2, int p3) { }
    public void copyArea(int p0, int p1, int p2, int p3, int p4, int p5) { }
    public java.awt.Graphics create() { return null; }
    public void dispose() { }
    public void draw(java.awt.Shape p0) { }
    public void drawArc(int p0, int p1, int p2, int p3, int p4, int p5) { }
    public void drawGlyphVector(java.awt.font.GlyphVector p0, float p1, float p2) { }
    public boolean drawImage(java.awt.Image p0, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, java.awt.Color p9, java.awt.image.ImageObserver p10) { return false; }
    public boolean drawImage(java.awt.Image p0, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, java.awt.image.ImageObserver p9) { return false; }
    public boolean drawImage(java.awt.Image p0, int p1, int p2, int p3, int p4, java.awt.Color p5, java.awt.image.ImageObserver p6) { return false; }
    public boolean drawImage(java.awt.Image p0, int p1, int p2, int p3, int p4, java.awt.image.ImageObserver p5) { return false; }
    public boolean drawImage(java.awt.Image p0, int p1, int p2, java.awt.Color p3, java.awt.image.ImageObserver p4) { return false; }
    public boolean drawImage(java.awt.Image p0, int p1, int p2, java.awt.image.ImageObserver p3) { return false; }
    public boolean drawImage(java.awt.Image p0, java.awt.geom.AffineTransform p1, java.awt.image.ImageObserver p2) { return false; }
    public void drawImage(java.awt.image.BufferedImage p0, java.awt.image.BufferedImageOp p1, int p2, int p3) { }
    public void drawLine(int p0, int p1, int p2, int p3) { }
    public void drawOval(int p0, int p1, int p2, int p3) { }
    public void drawPolygon(int[] p0, int[] p1, int p2) { }
    public void drawPolyline(int[] p0, int[] p1, int p2) { }
    public void drawRenderableImage(java.awt.image.renderable.RenderableImage p0, java.awt.geom.AffineTransform p1) { }
    public void drawRenderedImage(java.awt.image.RenderedImage p0, java.awt.geom.AffineTransform p1) { }
    public void drawRoundRect(int p0, int p1, int p2, int p3, int p4, int p5) { }
    public void drawString(java.lang.String p0, float p1, float p2) { }
    public void drawString(java.lang.String p0, int p1, int p2) { }
    public void drawString(java.text.AttributedCharacterIterator p0, float p1, float p2) { }
    public void drawString(java.text.AttributedCharacterIterator p0, int p1, int p2) { }
    public void fill(java.awt.Shape p0) { }
    public void fillArc(int p0, int p1, int p2, int p3, int p4, int p5) { }
    public void fillOval(int p0, int p1, int p2, int p3) { }
    public void fillPolygon(int[] p0, int[] p1, int p2) { }
    public void fillRect(int p0, int p1, int p2, int p3) { }
    public void fillRoundRect(int p0, int p1, int p2, int p3, int p4, int p5) { }
    public java.awt.Color getBackground() { return null; }
    public java.awt.Shape getClip() { return null; }
    public java.awt.Rectangle getClipBounds() { return null; }
    public java.awt.Color getColor() { return null; }
    public java.awt.Composite getComposite() { return null; }
    public java.awt.GraphicsConfiguration getDeviceConfiguration() { return null; }
    public java.awt.Font getFont() { return null; }
    public java.awt.FontMetrics getFontMetrics(java.awt.Font p0) { return null; }
    public java.awt.font.FontRenderContext getFontRenderContext() { return null; }
    public java.awt.Paint getPaint() { return null; }
    public java.lang.Object getRenderingHint(java.awt.RenderingHints.Key p0) { return null; }
    public java.awt.RenderingHints getRenderingHints() { return null; }
    public java.awt.Stroke getStroke() { return null; }
    public java.awt.geom.AffineTransform getTransform() { return null; }
    public boolean hit(java.awt.Rectangle p0, java.awt.Shape p1, boolean p2) { return false; }
    public void rotate(double p0) { }
    public void rotate(double p0, double p1, double p2) { }
    public void scale(double p0, double p1) { }
    public void setBackground(java.awt.Color p0) { }
    public void setClip(int p0, int p1, int p2, int p3) { }
    public void setClip(java.awt.Shape p0) { }
    public void setColor(java.awt.Color p0) { }
    public void setComposite(java.awt.Composite p0) { }
    public void setFont(java.awt.Font p0) { }
    public void setPaint(java.awt.Paint p0) { }
    public void setPaintMode() { }
    public void setRenderingHint(java.awt.RenderingHints.Key p0, java.lang.Object p1) { }
    public void setRenderingHints(java.util.Map p0) { }
    public void setStroke(java.awt.Stroke p0) { }
    public void setTransform(java.awt.geom.AffineTransform p0) { }
    public void setXORMode(java.awt.Color p0) { }
    public void shear(double p0, double p1) { }
    public void transform(java.awt.geom.AffineTransform p0) { }
    public void translate(double p0, double p1) { }
    public void translate(int p0, int p1) { }
}
