package org.lwjgl;

/** Replaces LWJGL's Linux Sys implementation: the original loads AWT (libawt/libjawt) in its static initializer, and the console's Java has no AWT. */
final class LinuxSysImplementation extends J2SESysImplementation {
    private static final int JNI_VERSION = 19;
    public int getRequiredJNIVersion() { return JNI_VERSION; }
    public boolean openURL(final String url) { return false; }
    public boolean has64Bit() { return true; }
}
