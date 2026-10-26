package org.lwjgl.opengl;

import java.nio.ByteBuffer;
import org.lwjgl.LWJGLException;

/** The console has one window and one context: the peer info carries nothing, only the lock bookkeeping of PeerInfo. */
final class PS5PeerInfo extends PeerInfo {
    PS5PeerInfo() { super(ByteBuffer.allocateDirect(16)); }
    protected void doLockAndInitHandle() throws LWJGLException {}
    protected void doUnlock() throws LWJGLException {}
}
