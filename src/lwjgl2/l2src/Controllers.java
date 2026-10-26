package org.lwjgl.input;

import org.lwjgl.LWJGLException;

/** Stand-in for LWJGL 2's Controllers (it needs JInput): the console's pad is delivered as keyboard / mouse events by PS5Display, so there are no separate controllers. */
public class Controllers {
    private Controllers() { }
    public static void create() throws LWJGLException { throw new LWJGLException("No controllers on this platform"); }
    public static boolean isCreated() { return false; }
    public static void destroy() { }
    public static int getControllerCount() { return 0; }
    public static Controller getController(int index) { return null; }
    public static void poll() { }
    public static void clearEvents() { }
    public static boolean next() { return false; }
    public static boolean isEventButton() { return false; }
    public static boolean isEventAxis() { return false; }
    public static boolean isEventXAxis() { return false; }
    public static boolean isEventYAxis() { return false; }
    public static boolean isEventPovX() { return false; }
    public static boolean isEventPovY() { return false; }
    public static int getEventControlIndex() { return 0; }
    public static Controller getEventSource() { return null; }
    public static boolean getEventButtonState() { return false; }
    public static float getEventXAxisValue() { return 0f; }
    public static float getEventYAxisValue() { return 0f; }
    public static long getEventNanoseconds() { return 0L; }
}
