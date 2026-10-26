package org.lwjgl.opengl;

/** Stand-in for java.awt.Canvas in Display.setParent(): the console's Java has no AWT, and nothing on the console embeds the window in a parent. */
public final class PS5Parent {
    private PS5Parent() {}
    public int getWidth() { return 0; }
    public int getHeight() { return 0; }
    public int getX() { return 0; }
    public int getY() { return 0; }
    public boolean isDisplayable() { return false; }
}
