package org.lwjgl;
/** Test-only: calls the fake GLFW shim's callbacks (tests/lw2_shim.c). Lives in org.lwjgl so the child-first loader defines it (the shim's .so is loaded by that loader). */
public final class Lw2Inject {
    public static native void inject(int kind, int a, int b, int c);   // 1 key(a=glfw key, b=action) 2 char(a=cp) 3 button(a,b=action) 4 move(a=x,b=y) 5 scroll(b=dy*1000) 6 focus(a)
    public static native int cursorMode();
    public static native int swaps();
    public static native String glString(int name);
}
