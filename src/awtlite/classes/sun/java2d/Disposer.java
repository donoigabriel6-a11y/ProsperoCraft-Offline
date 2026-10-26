package sun.java2d;

import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;

/**
 * PS5 AWT-lite Disposer: nothing to free natively (no AWT natives here), so records are dropped. The real one reads its settings through sun.security.action.GetPropertyAction, a class JDK 24+ no longer has:
 * javax.imageio.stream.MemoryCacheImageInputStream (Vitrail's noise texture decode, any mod that reads an image from a stream) died with NoClassDefFoundError in Disposer.<clinit>.
 */
public class Disposer {
    public static final ReferenceQueue<Object> queue = new ReferenceQueue<Object>();
    private Disposer() { }
    public static void addRecord(Object target, DisposerRecord rec) { }
    public static void addObjectRecord(Object obj, DisposerRecord rec) { }
    public static void addRecord(Object target, long disposeMethod, long pData) { }
    public static void addReference(Reference<Object> ref, DisposerRecord rec) { }
    public static ReferenceQueue<Object> getQueue() { return queue; }
    public static void pollRemove() { }
}
