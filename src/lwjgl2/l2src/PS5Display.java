package org.lwjgl.opengl;

import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import org.lwjgl.LWJGLException;
import org.lwjgl.input.Keyboard;

/**
 * LWJGL 2 window + input on the console. Replaces LinuxDisplay (X11): the window, the EGL context and the pad -> keyboard/mouse mapping already exist in the
 * GLFW shim (ps5lwjgl.c); the natives in ps5lwjgl2.c call that shim, collect its key / char / mouse callbacks in a ring, and this class turns them into
 * LWJGL 2's keyboard and mouse events (the same byte layouts LinuxKeyboard / LinuxMouse produce).
 */
final class PS5Display implements DisplayImplementation {
    private static native boolean nInit();
    private static native void nScreen(int[] wh);
    private static native boolean nCreateWindow(int w, int h);
    private static native void nDestroyWindow();
    private static native void nPoll();
    private static native int nDrain(int[] ev);
    private static native void nGrab(boolean grab);
    private static native void nSetCursor(int x, int y);
    private static native void nCursor(int[] xy);
    private static native boolean nCloseRequested();
    private static native void nSize(int[] wh);
    private static native void nTitle(String title);

    private static final int EV_KEY = 1, EV_CHAR = 2, EV_BUTTON = 3, EV_MOVE = 4, EV_SCROLL = 5, EV_FOCUS = 6;
    private static final int WHEEL_SCALE = 120, QUEUE = 4096;

    private DisplayMode desktop;
    private final int[] wh = new int[2], xy = new int[2], raw = new int[4 * 256];
    private int width, height, createdW, createdH;
    private boolean created, active = true, resizedOnce, keyboardCreated, mouseCreated, grabbed;

    // keyboard: state + queued events (int key, byte state, int char, long nanos, byte repeat = 18 bytes)
    private final byte[] keyDown = new byte[Keyboard.KEYBOARD_SIZE];
    private final ByteBuffer keyEvents = ByteBuffer.allocate(Keyboard.EVENT_SIZE * QUEUE);
    private int pendingKeyAt = -1;   // position of the last key press event that has no character yet (GLFW sends the char callback after the key callback)
    // mouse: buttons + queued events (byte button, byte state, int x|dx, int y|dy, int dwheel, long nanos = 22 bytes)
    private final byte[] buttons = new byte[8];
    private final ByteBuffer mouseEvents = ByteBuffer.allocate(22 * QUEUE);
    private int lastX, lastY, accumDX, accumDY, accumDZ;   // lastX/Y in LWJGL coordinates (origin bottom-left)
    private int rawX, rawY;                                  // last cursor position as GLFW reports it (origin top-left)

    private static final int[] KEYMAP = new int[349];
    private static void map(int glfw, int lwjgl) { KEYMAP[glfw] = lwjgl; }
    static {
        map(32, Keyboard.KEY_SPACE); map(39, Keyboard.KEY_APOSTROPHE); map(44, Keyboard.KEY_COMMA); map(45, Keyboard.KEY_MINUS); map(46, Keyboard.KEY_PERIOD); map(47, Keyboard.KEY_SLASH);
        final int[] digits = { Keyboard.KEY_0, Keyboard.KEY_1, Keyboard.KEY_2, Keyboard.KEY_3, Keyboard.KEY_4, Keyboard.KEY_5, Keyboard.KEY_6, Keyboard.KEY_7, Keyboard.KEY_8, Keyboard.KEY_9 };
        for (int i = 0; i < 10; i++) map(48 + i, digits[i]);
        map(59, Keyboard.KEY_SEMICOLON); map(61, Keyboard.KEY_EQUALS);
        final int[] letters = { Keyboard.KEY_A, Keyboard.KEY_B, Keyboard.KEY_C, Keyboard.KEY_D, Keyboard.KEY_E, Keyboard.KEY_F, Keyboard.KEY_G, Keyboard.KEY_H, Keyboard.KEY_I, Keyboard.KEY_J, Keyboard.KEY_K, Keyboard.KEY_L, Keyboard.KEY_M,
            Keyboard.KEY_N, Keyboard.KEY_O, Keyboard.KEY_P, Keyboard.KEY_Q, Keyboard.KEY_R, Keyboard.KEY_S, Keyboard.KEY_T, Keyboard.KEY_U, Keyboard.KEY_V, Keyboard.KEY_W, Keyboard.KEY_X, Keyboard.KEY_Y, Keyboard.KEY_Z };
        for (int i = 0; i < 26; i++) map(65 + i, letters[i]);
        map(91, Keyboard.KEY_LBRACKET); map(92, Keyboard.KEY_BACKSLASH); map(93, Keyboard.KEY_RBRACKET); map(96, Keyboard.KEY_GRAVE);
        map(256, Keyboard.KEY_ESCAPE); map(257, Keyboard.KEY_RETURN); map(258, Keyboard.KEY_TAB); map(259, Keyboard.KEY_BACK); map(260, Keyboard.KEY_INSERT); map(261, Keyboard.KEY_DELETE);
        map(262, Keyboard.KEY_RIGHT); map(263, Keyboard.KEY_LEFT); map(264, Keyboard.KEY_DOWN); map(265, Keyboard.KEY_UP); map(266, Keyboard.KEY_PRIOR); map(267, Keyboard.KEY_NEXT);
        map(268, Keyboard.KEY_HOME); map(269, Keyboard.KEY_END); map(280, Keyboard.KEY_CAPITAL); map(281, Keyboard.KEY_SCROLL); map(282, Keyboard.KEY_NUMLOCK); map(283, Keyboard.KEY_SYSRQ); map(284, Keyboard.KEY_PAUSE);
        final int[] f = { Keyboard.KEY_F1, Keyboard.KEY_F2, Keyboard.KEY_F3, Keyboard.KEY_F4, Keyboard.KEY_F5, Keyboard.KEY_F6, Keyboard.KEY_F7, Keyboard.KEY_F8, Keyboard.KEY_F9, Keyboard.KEY_F10, Keyboard.KEY_F11, Keyboard.KEY_F12,
            Keyboard.KEY_F13, Keyboard.KEY_F14, Keyboard.KEY_F15 };
        for (int i = 0; i < f.length; i++) map(290 + i, f[i]);
        final int[] kp = { Keyboard.KEY_NUMPAD0, Keyboard.KEY_NUMPAD1, Keyboard.KEY_NUMPAD2, Keyboard.KEY_NUMPAD3, Keyboard.KEY_NUMPAD4, Keyboard.KEY_NUMPAD5, Keyboard.KEY_NUMPAD6, Keyboard.KEY_NUMPAD7, Keyboard.KEY_NUMPAD8, Keyboard.KEY_NUMPAD9 };
        for (int i = 0; i < 10; i++) map(320 + i, kp[i]);
        map(330, Keyboard.KEY_DECIMAL); map(331, Keyboard.KEY_DIVIDE); map(332, Keyboard.KEY_MULTIPLY); map(333, Keyboard.KEY_SUBTRACT); map(334, Keyboard.KEY_ADD); map(335, Keyboard.KEY_NUMPADENTER); map(336, Keyboard.KEY_NUMPADEQUALS);
        map(340, Keyboard.KEY_LSHIFT); map(341, Keyboard.KEY_LCONTROL); map(342, Keyboard.KEY_LMENU); map(343, Keyboard.KEY_LMETA);
        map(344, Keyboard.KEY_RSHIFT); map(345, Keyboard.KEY_RCONTROL); map(346, Keyboard.KEY_RMENU); map(347, Keyboard.KEY_RMETA);
    }

    // ---- DisplayImplementation ----
    public DisplayMode init() throws LWJGLException {
        if (!nInit()) throw new LWJGLException("The console display could not be initialised");
        nScreen(wh);
        desktop = new DisplayMode(wh[0] > 0 ? wh[0] : 1920, wh[1] > 0 ? wh[1] : 1080, 32, 60);
        return desktop;
    }
    public DisplayMode[] getAvailableDisplayModes() throws LWJGLException { return new DisplayMode[] { desktop }; }
    public void createWindow(DrawableLWJGL drawable, DisplayMode mode, PS5Parent parent, int x, int y) throws LWJGLException {
        grabbed = false; accumDX = accumDY = accumDZ = 0;   // a new game round: the shim starts with the pointer free (the previous round may have ended with the camera grabbed)
        if (!nCreateWindow(mode.getWidth(), mode.getHeight())) throw new LWJGLException("The console window could not be created");
        nSize(wh); width = wh[0]; height = wh[1]; createdW = mode.getWidth(); createdH = mode.getHeight();
        resizedOnce = width != createdW || height != createdH;   // the console picture is fixed: report one resize so the game takes the real size
        created = true; windowGen++;
        nCursor(xy); rawX = xy[0]; rawY = xy[1]; lastX = rawX; lastY = height - 1 - rawY;
    }
    public void destroyWindow() {
        created = false; nDestroyWindow();
        // The game closes its window only when it quits (a fullscreen toggle recreates it within a moment): then the host brings the launcher back. A fresh thread, because the exit paths of the VM itself
        // are refused by the console and the thread that closes the window is about to run them.
        final int gen = ++windowGen;
        Thread t = new Thread(new Runnable() { public void run() {
            try { Thread.sleep(1500); } catch (InterruptedException e) { }
            if (gen == windowGen && !created) quitHost("window closed");
        } }, "ps5-closing");
        t.setDaemon(true); t.start();
    }
    private static volatile int windowGen;
    static void quitHost(String why) {
        try {
            Class<?> h = Class.forName("McBoot", true, ClassLoader.getSystemClassLoader());
            java.lang.reflect.Method m = h.getDeclaredMethod("leave", String.class); m.setAccessible(true); m.invoke(null, why);
        } catch (Throwable t) { System.out.println("[ps5] quit to launcher unavailable: " + t); }
    }
    public void switchDisplayMode(DisplayMode mode) throws LWJGLException {}
    public void resetDisplayMode() {}
    public int getGammaRampLength() { return 0; }
    public void setGammaRamp(FloatBuffer gammaRamp) throws LWJGLException { throw new LWJGLException("Gamma ramps are not supported on this platform"); }
    public String getAdapter() { return "PlayStation 5"; }
    public String getVersion() { return "PS5"; }
    public void setTitle(String title) { nTitle(title); }
    public boolean isCloseRequested() { return nCloseRequested(); }
    public boolean isVisible() { return created; }
    public boolean isActive() { return created && active; }
    public boolean isDirty() { return false; }
    public PeerInfo createPeerInfo(PixelFormat pixel_format, ContextAttribs attribs) throws LWJGLException { return new PS5PeerInfo(); }
    public void reshape(int x, int y, int w, int h) {}
    public int getPbufferCapabilities() { return 0; }
    public boolean isBufferLost(PeerInfo handle) { return false; }
    public PeerInfo createPbuffer(int w, int h, PixelFormat pf, ContextAttribs attribs, IntBuffer pixelFormatCaps, IntBuffer pBufferAttribs) throws LWJGLException { throw new LWJGLException("Pbuffers are not supported on this platform"); }
    public void setPbufferAttrib(PeerInfo handle, int attrib, int value) { throw new UnsupportedOperationException(); }
    public void bindTexImageToPbuffer(PeerInfo handle, int buffer) { throw new UnsupportedOperationException(); }
    public void releaseTexImageFromPbuffer(PeerInfo handle, int buffer) { throw new UnsupportedOperationException(); }
    public int setIcon(ByteBuffer[] icons) { return 0; }
    public void setResizable(boolean resizable) {}
    public boolean wasResized() { boolean r = resizedOnce; resizedOnce = false; return r; }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getX() { return 0; }
    public int getY() { return 0; }
    public float getPixelScaleFactor() { return 1f; }

    /** one pass over the shim's events: called from Display.update() (processMessages) on the render thread */
    public void update() {
        if (!created) return;
        nPoll();
        int n;
        while ((n = nDrain(raw)) > 0) {
            for (int i = 0; i < n; i++) handle(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2]);
            if (n < raw.length / 4) break;
        }
        pendingKeyAt = -1;
    }
    private void handle(int type, int a, int b) {
        long nanos = System.nanoTime();
        switch (type) {
            case EV_KEY: {
                int key = a >= 0 && a < KEYMAP.length ? KEYMAP[a] : 0;
                if (key == 0) return;
                boolean down = b != 0;
                if (b != 2) keyDown[key] = (byte) (down ? 1 : 0);
                if (keyEvents.remaining() >= Keyboard.EVENT_SIZE) {
                    pendingKeyAt = down ? keyEvents.position() : -1;
                    keyEvents.putInt(key).put((byte) (down ? 1 : 0)).putInt(0).putLong(nanos).put((byte) (b == 2 ? 1 : 0));
                }
                break;
            }
            case EV_CHAR: {
                if (pendingKeyAt >= 0 && keyEvents.getInt(pendingKeyAt + 5) == 0) { keyEvents.putInt(pendingKeyAt + 5, a); pendingKeyAt = -1; }
                else if (keyEvents.remaining() >= Keyboard.EVENT_SIZE) keyEvents.putInt(0).put((byte) 1).putInt(a).putLong(nanos).put((byte) 0);   // a character without a key (on-screen keyboard)
                break;
            }
            case EV_BUTTON: {
                if (a < 0 || a >= buttons.length) return;
                byte s = (byte) (b != 0 ? 1 : 0);
                buttons[a] = s;
                putMouse((byte) a, s, 0, nanos);
                break;
            }
            case EV_MOVE: {
                int dx = a - rawX, dy = b - rawY; rawX = a; rawY = b;
                if (grabbed) {
                    if (dx == 0 && dy == 0) return;
                    accumDX += dx; accumDY -= dy;   // LWJGL: y grows upwards
                    putMouseXY((byte) -1, (byte) 0, dx, -dy, 0, nanos);
                } else {
                    int ly = height - 1 - b;
                    accumDX += a - lastX; accumDY += ly - lastY; lastX = a; lastY = ly;
                    putMouseXY((byte) -1, (byte) 0, a, ly, 0, nanos);
                }
                break;
            }
            case EV_SCROLL: {
                int dz = b * WHEEL_SCALE / 1000;   // the native side sends the scroll amount in thousandths
                if (dz == 0) return;
                accumDZ += dz;
                putMouse((byte) -1, (byte) 0, dz, nanos);
                break;
            }
            case EV_FOCUS: active = a != 0; break;
            default: break;
        }
    }
    private void putMouse(byte button, byte state, int dz, long nanos) {
        if (grabbed) putMouseXY(button, state, 0, 0, dz, nanos); else putMouseXY(button, state, lastX, lastY, dz, nanos);
    }
    private void putMouseXY(byte button, byte state, int c1, int c2, int dz, long nanos) {
        if (mouseEvents.remaining() >= 22) mouseEvents.put(button).put(state).putInt(c1).putInt(c2).putInt(dz).putLong(nanos);
    }

    // ---- InputImplementation ----
    public boolean hasWheel() { return true; }
    public int getButtonCount() { return 3; }
    public void createMouse() throws LWJGLException { mouseCreated = true; mouseEvents.clear(); }
    public void destroyMouse() { mouseCreated = false; }
    public void pollMouse(IntBuffer coord_buffer, ByteBuffer buttons_buffer) {
        if (grabbed) { coord_buffer.put(0, accumDX); coord_buffer.put(1, accumDY); } else { coord_buffer.put(0, lastX); coord_buffer.put(1, lastY); }
        coord_buffer.put(2, accumDZ);
        accumDX = accumDY = accumDZ = 0;
        int p = buttons_buffer.position();
        buttons_buffer.put(buttons, 0, Math.min(buttons.length, buttons_buffer.remaining()));
        buttons_buffer.position(p);
    }
    public void readMouse(ByteBuffer buffer) {
        mouseEvents.flip();
        int n = Math.min(mouseEvents.remaining(), buffer.remaining()) / 22 * 22;
        for (int i = 0; i < n; i++) buffer.put(mouseEvents.get());
        mouseEvents.compact();
    }
    public void grabMouse(boolean grab) {
        if (grab == grabbed) return;
        grabbed = grab; nGrab(grab);
        nCursor(xy); rawX = xy[0]; rawY = xy[1]; lastX = rawX; lastY = height - 1 - rawY;
        accumDX = accumDY = 0; mouseEvents.clear();
    }
    public int getNativeCursorCapabilities() { return 0; }
    public void setCursorPosition(int x, int y) {
        nSetCursor(x, height - 1 - y);
        rawX = x; rawY = height - 1 - y; lastX = x; lastY = y;
    }
    public void setNativeCursor(Object handle) throws LWJGLException {}
    public int getMinCursorSize() { return 0; }
    public int getMaxCursorSize() { return 0; }
    public void createKeyboard() throws LWJGLException { keyboardCreated = true; keyEvents.clear(); java.util.Arrays.fill(keyDown, (byte) 0); }
    public void destroyKeyboard() { keyboardCreated = false; }
    public void pollKeyboard(ByteBuffer keyDownBuffer) {
        int p = keyDownBuffer.position();
        keyDownBuffer.put(keyDown, 0, Math.min(keyDown.length, keyDownBuffer.remaining()));
        keyDownBuffer.position(p);
    }
    public void readKeyboard(ByteBuffer buffer) {
        keyEvents.flip();
        int n = Math.min(keyEvents.remaining(), buffer.remaining()) / Keyboard.EVENT_SIZE * Keyboard.EVENT_SIZE;
        for (int i = 0; i < n; i++) buffer.put(keyEvents.get());
        keyEvents.compact();
        pendingKeyAt = -1;
    }
    public Object createCursor(int width, int height, int xHotspot, int yHotspot, int numImages, IntBuffer images, IntBuffer delays) throws LWJGLException { throw new LWJGLException("Native cursors are not supported on this platform"); }
    public void destroyCursor(Object cursor_handle) {}
    public boolean isInsideWindow() { return true; }
}
