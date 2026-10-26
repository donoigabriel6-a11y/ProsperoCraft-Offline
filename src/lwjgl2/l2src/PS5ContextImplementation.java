package org.lwjgl.opengl;

import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import org.lwjgl.LWJGLException;

/** LWJGL 2 context calls on the console: the EGL context is created by the GLFW shim (ps5lwjgl.c) together with the window; this just makes it current and swaps. */
final class PS5ContextImplementation implements ContextImplementation {
    private static boolean current;
    private static native void nMakeCurrent();
    private static native void nSwap();
    private static native void nSetSwapInterval(int interval);

    public ByteBuffer create(PeerInfo peer_info, IntBuffer attribs, ByteBuffer shared_context_handle) throws LWJGLException {
        if (shared_context_handle != null) throw new LWJGLException("Shared OpenGL contexts are not supported on this platform");
        return ByteBuffer.allocateDirect(16);
    }
    public void swapBuffers() throws LWJGLException { nSwap(); }
    public void releaseDrawable(ByteBuffer context_handle) throws LWJGLException {}
    public void releaseCurrentContext() throws LWJGLException { current = false; }
    public void update(ByteBuffer context_handle) {}
    public void makeCurrent(PeerInfo peer_info, ByteBuffer handle) throws LWJGLException { nMakeCurrent(); current = true; }
    public boolean isCurrent(ByteBuffer handle) throws LWJGLException { return current; }
    public void setSwapInterval(int value) { nSetSwapInterval(value); }
    public void destroy(PeerInfo peer_info, ByteBuffer handle) throws LWJGLException { current = false; }
}
