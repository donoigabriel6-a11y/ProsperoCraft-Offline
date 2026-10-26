import java.io.File;
import java.net.URL;
/** Host test of the LWJGL 2 port (tests/lw2_test.sh): loads the port's jar the way McBoot does (child-first loader, Sys initialised first) and runs org.lwjgl.Lw2Drive. */
public class Lw2Test {
    static class ChildFirst extends java.net.URLClassLoader {   // same as McBoot.ChildFirst
        ChildFirst(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!name.startsWith("org.lwjgl.")) return super.loadClass(name, resolve);
            synchronized (getClassLoadingLock(name)) {
                Class<?> c = findLoadedClass(name);
                if (c == null) { try { c = findClass(name); } catch (ClassNotFoundException e) { return super.loadClass(name, resolve); } }
                if (resolve) resolveClass(c);
                return c;
            }
        }
    }
    public static void main(String[] a) throws Throwable {
        ChildFirst cl = new ChildFirst(new URL[] { new File(a[0]).toURI().toURL(), new File(a[1]).toURI().toURL() }, Lw2Test.class.getClassLoader());
        Thread.currentThread().setContextClassLoader(cl);
        Class.forName("org.lwjgl.Sys", true, cl);
        Object r;
        try { r = Class.forName("org.lwjgl.Lw2Drive", true, cl).getMethod("run").invoke(null); }
        catch (java.lang.reflect.InvocationTargetException e) { e.getCause().printStackTrace(System.out); System.out.println("   FAIL: exception " + e.getCause() + "\nfails=1"); return; }
        System.out.println("fails=" + r);
    }
}
