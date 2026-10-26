package org.lwjgl;

import java.nio.*;
import org.lwjgl.input.*;
import org.lwjgl.opengl.*;
import org.lwjgl.openal.*;

/** What Minecraft 1.12.2 does with LWJGL 2, in miniature: Display, GL context, fixed-function drawing, Keyboard / Mouse events, OpenAL. Returns the number of failed checks. */
public final class Lw2Drive {
    static int fails, checks;
    static void ok(boolean c, String what) { checks++; if (!c) { fails++; System.out.println("   FAIL: " + what); } else if (System.getenv("LW2_VERBOSE") != null) System.out.println("   ok: " + what); }
    static ByteBuffer px = BufferUtils.createByteBuffer(4);
    static int[] pix(int x, int y) { px.clear(); GL11.glReadPixels(x, y, 1, 1, GL11.GL_RGBA, GL11.GL_UNSIGNED_BYTE, px); return new int[] { px.get(0) & 255, px.get(1) & 255, px.get(2) & 255 }; }
    static boolean near(int[] p, int r, int g, int b) { return Math.abs(p[0] - r) <= 6 && Math.abs(p[1] - g) <= 6 && Math.abs(p[2] - b) <= 6; }
    static String s(int[] p) { return p[0] + "," + p[1] + "," + p[2]; }

    public static int run() throws Throwable {
        ok(Sys.getVersion().startsWith("2.9"), "Sys.getVersion " + Sys.getVersion());
        ok(Sys.is64Bit(), "Sys.is64Bit");
        ok(Sys.getTime() >= 0 && Sys.getTimerResolution() > 0, "Sys timer");
        DisplayMode[] modes = Display.getAvailableDisplayModes();
        ok(modes != null && modes.length > 0, "available display modes: " + (modes == null ? -1 : modes.length));
        Display.setDisplayMode(new DisplayMode(640, 360));
        Display.setTitle("lw2 test");
        Display.create(new PixelFormat(8, 24, 8));
        ok(Display.isCreated() && Display.isActive(), "Display.create");
        ok(Display.getWidth() == 640 && Display.getHeight() == 360, "Display size " + Display.getWidth() + "x" + Display.getHeight());
        ContextCapabilities cap = GLContext.getCapabilities();
        ok(cap.OpenGL11 && cap.OpenGL12 && cap.OpenGL13 && cap.OpenGL14 && cap.OpenGL15, "GL 1.1 - 1.5 capabilities");
        ok(cap.OpenGL20 && cap.OpenGL21, "GL 2.0 / 2.1 capabilities");
        ok(cap.GL_EXT_framebuffer_object || cap.OpenGL30, "framebuffers (EXT or core)");
        System.out.println("   GL_VERSION=" + GL11.glGetString(GL11.GL_VERSION) + "  OpenGL30=" + cap.OpenGL30 + " ARB_VBO=" + cap.GL_ARB_vertex_buffer_object + " ARB_multitexture=" + cap.GL_ARB_multitexture + " EXT_fbo=" + cap.GL_EXT_framebuffer_object);

        // fixed-function drawing
        GL11.glViewport(0, 0, 64, 64);
        GL11.glMatrixMode(GL11.GL_PROJECTION); GL11.glLoadIdentity(); GL11.glOrtho(0, 64, 0, 64, -1, 1);
        GL11.glMatrixMode(GL11.GL_MODELVIEW); GL11.glLoadIdentity();
        GL11.glClearColor(0, 0, 1, 1); GL11.glClear(GL11.GL_COLOR_BUFFER_BIT | GL11.GL_DEPTH_BUFFER_BIT);
        GL11.glColor3f(1, 0, 0);
        GL11.glBegin(GL11.GL_QUADS); GL11.glVertex2f(0, 0); GL11.glVertex2f(32, 0); GL11.glVertex2f(32, 64); GL11.glVertex2f(0, 64); GL11.glEnd();
        int[] p = pix(10, 32); ok(near(p, 255, 0, 0), "immediate-mode quad is red at (10,32): " + s(p));
        p = pix(50, 32); ok(near(p, 0, 0, 255), "clear colour blue at (50,32): " + s(p));
        ok(GL11.glGetError() == 0, "glGetError after immediate mode");
        // client-side arrays through a direct FloatBuffer (this is the MemoryUtil / buffer address path)
        FloatBuffer v = BufferUtils.createFloatBuffer(8); v.put(new float[] { 33, 0, 64, 0, 64, 64, 33, 64 }).flip();
        GL11.glColor3f(0, 1, 0);
        GL11.glEnableClientState(GL11.GL_VERTEX_ARRAY); GL11.glVertexPointer(2, 0, v); GL11.glDrawArrays(GL11.GL_QUADS, 0, 4); GL11.glDisableClientState(GL11.GL_VERTEX_ARRAY);
        p = pix(50, 32); ok(near(p, 0, 255, 0), "vertex-array quad is green at (50,32): " + s(p));
        // a vertex buffer object
        IntBuffer ids = BufferUtils.createIntBuffer(1); ARBVertexBufferObject.glGenBuffersARB(ids); int vbo = ids.get(0);
        ok(vbo != 0, "glGenBuffersARB");
        FloatBuffer q = BufferUtils.createFloatBuffer(8); q.put(new float[] { 0, 0, 32, 0, 32, 20, 0, 20 }).flip();
        ARBVertexBufferObject.glBindBufferARB(ARBVertexBufferObject.GL_ARRAY_BUFFER_ARB, vbo);
        ARBVertexBufferObject.glBufferDataARB(ARBVertexBufferObject.GL_ARRAY_BUFFER_ARB, q, ARBVertexBufferObject.GL_STATIC_DRAW_ARB);
        GL11.glColor3f(1, 1, 0); GL11.glEnableClientState(GL11.GL_VERTEX_ARRAY); GL11.glVertexPointer(2, GL11.GL_FLOAT, 0, 0L); GL11.glDrawArrays(GL11.GL_QUADS, 0, 4); GL11.glDisableClientState(GL11.GL_VERTEX_ARRAY);
        ARBVertexBufferObject.glBindBufferARB(ARBVertexBufferObject.GL_ARRAY_BUFFER_ARB, 0);
        p = pix(10, 10); ok(near(p, 255, 255, 0), "VBO quad is yellow at (10,10): " + s(p));
        // a texture
        int tex = GL11.glGenTextures(); GL11.glBindTexture(GL11.GL_TEXTURE_2D, tex);
        ByteBuffer t = BufferUtils.createByteBuffer(16); t.put(new byte[] { (byte) 255, (byte) 0, (byte) 255, (byte) 255, (byte) 255, 0, (byte) 255, (byte) 255, (byte) 255, 0, (byte) 255, (byte) 255, (byte) 255, 0, (byte) 255, (byte) 255 }).flip();
        GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL11.GL_TEXTURE_MIN_FILTER, GL11.GL_NEAREST); GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL11.GL_TEXTURE_MAG_FILTER, GL11.GL_NEAREST);
        GL11.glTexImage2D(GL11.GL_TEXTURE_2D, 0, GL11.GL_RGBA, 2, 2, 0, GL11.GL_RGBA, GL11.GL_UNSIGNED_BYTE, t);
        GL11.glEnable(GL11.GL_TEXTURE_2D); GL11.glColor3f(1, 1, 1);
        GL11.glBegin(GL11.GL_QUADS); GL11.glTexCoord2f(0, 0); GL11.glVertex2f(0, 40); GL11.glTexCoord2f(1, 0); GL11.glVertex2f(30, 40); GL11.glTexCoord2f(1, 1); GL11.glVertex2f(30, 60); GL11.glTexCoord2f(0, 1); GL11.glVertex2f(0, 60); GL11.glEnd();
        GL11.glDisable(GL11.GL_TEXTURE_2D);
        p = pix(5, 42); ok(near(p, 255, 0, 255), "textured quad texel (0,0) magenta at (5,42): " + s(p));
        ok(GL11.glGetError() == 0, "glGetError after arrays / VBO / texture");
        int swaps0 = Lw2Inject.swaps();
        Display.update();
        ok(Lw2Inject.swaps() > swaps0, "Display.update swaps buffers");
        ok(!Display.isCloseRequested(), "no close request");

        // keyboard
        Keyboard.create(); Mouse.create();
        ok(Keyboard.isCreated() && Mouse.isCreated(), "Keyboard / Mouse created");
        Lw2Inject.inject(1, 87, 1, 0); Lw2Inject.inject(2, 'w', 0, 0); Lw2Inject.inject(1, 87, 0, 0);
        Lw2Inject.inject(1, 256, 1, 0); Lw2Inject.inject(1, 256, 0, 0);
        Lw2Inject.inject(1, 340, 1, 0); Lw2Inject.inject(1, 292, 1, 0); Lw2Inject.inject(1, 292, 0, 0); Lw2Inject.inject(1, 340, 0, 0);
        Lw2Inject.inject(1, 49, 1, 0); Lw2Inject.inject(1, 49, 0, 0);
        Lw2Inject.inject(1, 32, 1, 0); Lw2Inject.inject(1, 265, 1, 0); Lw2Inject.inject(1, 257, 1, 0);
        Display.update();
        StringBuilder sb = new StringBuilder(); int n = 0;
        int[] want = { Keyboard.KEY_W, Keyboard.KEY_W, Keyboard.KEY_ESCAPE, Keyboard.KEY_ESCAPE, Keyboard.KEY_LSHIFT, Keyboard.KEY_F3, Keyboard.KEY_F3, Keyboard.KEY_LSHIFT, Keyboard.KEY_1, Keyboard.KEY_1, Keyboard.KEY_SPACE, Keyboard.KEY_UP, Keyboard.KEY_RETURN };
        boolean[] st = { true, false, true, false, true, true, false, false, true, false, true, true, true };
        boolean seq = true; char ch = 0;
        while (Keyboard.next()) {
            if (n < want.length) { if (Keyboard.getEventKey() != want[n] || Keyboard.getEventKeyState() != st[n]) seq = false; }
            if (n == 0) ch = Keyboard.getEventCharacter();
            sb.append(Keyboard.getKeyName(Keyboard.getEventKey())).append(Keyboard.getEventKeyState() ? "+ " : "- "); n++;
        }
        ok(n == want.length && seq, "keyboard event sequence (" + n + " events: " + sb + ")");
        ok(ch == 'w', "character 'w' attached to the key press: " + (int) ch);
        ok(Keyboard.isKeyDown(Keyboard.KEY_SPACE) && Keyboard.isKeyDown(Keyboard.KEY_UP) && !Keyboard.isKeyDown(Keyboard.KEY_W), "key state: space / up held, W released");
        // OSK-style text with no key press
        Lw2Inject.inject(2, 'ä', 0, 0); Display.update();
        boolean got = false; while (Keyboard.next()) if (Keyboard.getEventCharacter() == 'ä') got = true;
        ok(got, "stand-alone character event (on-screen keyboard)");

        // mouse
        Lw2Inject.inject(4, 100, 60, 0); Lw2Inject.inject(3, 0, 1, 0); Lw2Inject.inject(3, 0, 0, 0); Lw2Inject.inject(3, 1, 1, 0); Lw2Inject.inject(5, 0, 1000, 0); Lw2Inject.inject(5, 0, -1000, 0);
        Display.update();
        ok(Mouse.getX() == 100 && Mouse.getY() == 360 - 1 - 60, "mouse position (100,60) -> LWJGL (" + Mouse.getX() + "," + Mouse.getY() + ") with the origin bottom-left");
        int moves = 0, buttons = 0, wheel = 0; boolean b0 = false, b1 = false;
        while (Mouse.next()) { int bt = Mouse.getEventButton();
            if (bt == 0 && Mouse.getEventButtonState()) b0 = true; if (bt == 1 && Mouse.getEventButtonState()) b1 = true; if (bt >= 0) buttons++; wheel += Mouse.getEventDWheel(); moves++; }
        ok(b0 && b1 && buttons == 3, "mouse buttons 0 down, 0 up, 1 down: " + buttons + " button events");
        ok(wheel == 0, "scroll +1 / -1 -> DWheel +120 / -120 (sum " + wheel + ")");
        ok(Mouse.isButtonDown(1) && !Mouse.isButtonDown(0), "mouse button state");
        Lw2Inject.inject(5, 0, 1000, 0); Display.update(); int w2 = 0; while (Mouse.next()) w2 += Mouse.getEventDWheel();
        ok(w2 == 120, "one scroll step is 120: " + w2);
        Mouse.setGrabbed(true);
        ok(Lw2Inject.cursorMode() == 0x34003, "grab -> GLFW cursor disabled: 0x" + Integer.toHexString(Lw2Inject.cursorMode()));
        ok(Mouse.isGrabbed(), "Mouse.isGrabbed");
        Mouse.getDX(); Mouse.getDY();
        Lw2Inject.inject(4, 110, 70, 0); Lw2Inject.inject(4, 125, 65, 0); Display.update();
        int dx = 0, dy = 0; while (Mouse.next()) { dx += Mouse.getEventDX(); dy += Mouse.getEventDY(); }
        System.out.println("   grabbed mouse: event deltas dx=" + dx + " dy=" + dy);
        ok(dx > 0 && dy != 0, "grabbed mouse produces deltas (dx=" + dx + ", dy=" + dy + ")");
        Mouse.setGrabbed(false);
        ok(Lw2Inject.cursorMode() == 0x34001, "release -> GLFW cursor normal");

        // OpenAL (the software OpenAL of ps5al.inc through the extal hooks)
        AL.create();
        ok(AL.isCreated(), "AL.create");
        IntBuffer src = BufferUtils.createIntBuffer(1); AL10.alGenSources(src);
        IntBuffer buf = BufferUtils.createIntBuffer(1); AL10.alGenBuffers(buf);
        ok(src.get(0) != 0 && buf.get(0) != 0, "alGenSources / alGenBuffers");
        ByteBuffer pcm = BufferUtils.createByteBuffer(4800 * 2); for (int i = 0; i < 4800; i++) pcm.putShort((short) (Math.sin(i * 0.1) * 8000)); pcm.flip();
        AL10.alBufferData(buf.get(0), AL10.AL_FORMAT_MONO16, pcm, 48000);
        AL10.alSourcei(src.get(0), AL10.AL_BUFFER, buf.get(0)); AL10.alSourcef(src.get(0), AL10.AL_GAIN, 0.5f);
        AL10.alSourcePlay(src.get(0));
        ok(AL10.alGetSourcei(src.get(0), AL10.AL_SOURCE_STATE) == AL10.AL_PLAYING, "source playing");
        ok(AL10.alGetError() == AL10.AL_NO_ERROR, "no AL error");
        Thread.sleep(300);
        AL10.alSourceStop(src.get(0)); AL10.alDeleteSources(src); AL10.alDeleteBuffers(buf);
        AL.destroy();
        ok(!AL.isCreated(), "AL.destroy");

        Display.destroy();
        ok(!Display.isCreated(), "Display.destroy");
        System.out.println("   " + checks + " checks");
        { // Display.sync(30) is raised to 60 (menu fps cap); other rates untouched
            Display.sync(1000); long t0 = System.nanoTime(); for (int i = 0; i < 30; i++) Display.sync(30); double ms30 = (System.nanoTime() - t0) / 1e6 / 30;
            t0 = System.nanoTime(); for (int i = 0; i < 30; i++) Display.sync(20); double ms20 = (System.nanoTime() - t0) / 1e6 / 30;
            for (int i = 0; i < 250; i++) Display.sync(500);   // prints the "[ps5] frame:" breakdown line
            ok(ms30 > 13 && ms30 < 20, "Display.sync(30) paces ~60 fps (" + ms30 + " ms)");
            ok(ms20 > 45 && ms20 < 55, "Display.sync(20) unchanged (" + ms20 + " ms)");
        }
        return fails;
    }
}
