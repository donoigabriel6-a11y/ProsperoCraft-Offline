import java.io.*;
import java.nio.charset.StandardCharsets;

// First-boot attempt for vanilla 1.20.1: routes System.out/err to the UDP log, then calls net.minecraft.client.main.Main.main.
// Only used when the title was built with the "mc" option (Hello.java picks McBoot over GLTest when /app0/assets/mc/client.jar exists).
public class McBoot {
    private static final java.util.concurrent.atomic.AtomicBoolean LEFT = new java.util.concurrent.atomic.AtomicBoolean();
    /** The game is over (quit, crash, window closed): bring the launcher back. The VM's own exit paths are refused by the console, so this runs the console calls itself, one native step per call,
     *  and logs from Java before and after each (a native log line can get lost; these cannot). Called from the shutdown hook, from the window-close watcher and after a crash. */
    /** Called by the ps5-crashwatch thread after the host flagged a native SIGSEGV/SIGBUS: prints every Java thread's stack so the log says which Java/JNI call the fault happened in. A thread stuck in native code (the faulting Render thread) is safepoint-safe, so this normally works; jvm_host gives up on it after 6 s. */
    public static void crashDump() {
        try {
            Hello.log("CRASH java: thread stacks (native fault or frozen game)");
            for (java.util.Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                Thread t = e.getKey(); StackTraceElement[] st = e.getValue();
                int lim = "Render thread".equals(t.getName()) || "main".equals(t.getName()) || "ps5-relaunch".equals(t.getName()) || t == relaunchThread ? 70 : 6;
                Hello.log("CRASH java: thread '" + t.getName() + "' " + t.getState() + ", " + st.length + " frames");
                for (int i = 0; i < st.length && i < lim; i++) { Hello.log("CRASH java:   at " + st[i]); if ((i & 7) == 7) try { Thread.sleep(3); } catch (InterruptedException x) { } }
                try { Thread.sleep(3); } catch (InterruptedException x) { }
            }
            try {   // files a stuck write may be filling (Quilt's transform cache): size now, so two dumps tell a hang from a slow copy
                java.util.ArrayDeque<File> q = new java.util.ArrayDeque<>(); q.add(new File(DIR, ".cache")); int n = 0;
                while (!q.isEmpty() && n < 20) { File d = q.poll(); File[] fs = d.listFiles(); if (fs == null) continue; for (File f : fs) { if (f.isDirectory()) q.add(f); else { Hello.log("CRASH files: " + f.getPath() + " " + f.length() + " bytes, modified " + (System.currentTimeMillis() - f.lastModified()) / 1000 + " s ago"); n++; } } }
                Hello.log("CRASH files: disk usable " + (new File(DIR).getUsableSpace() >> 20) + " MB, free " + (new File(DIR).getFreeSpace() >> 20) + " MB");
            } catch (Throwable x) { }
            Hello.log("CRASH java: end");
        } catch (Throwable t) { Hello.log("CRASH java: dump failed " + t); }
    }
    private static void startCrashWatch() {   // a plain Java thread (no JNI attach): the host sets the flag after a native crash, this thread prints the stacks and acknowledges
        Thread w = new Thread(() -> {
            try { while (true) { Thread.sleep(200); int f = Hello.crashFlag(); if (f == 2) { Hello.log("STALL java: the game has presented no frame for seconds"); crashDump(); Hello.crashAck(); } else if (f != 0) { crashDump(); Hello.crashAck(); return; } } }
            catch (Throwable t) { Hello.log("CRASH java: watcher ended: " + t); }
        }, "ps5-crashwatch");
        w.setDaemon(true); w.start();
        Hello.log("crash watcher started (prints the Java stacks after a native crash)");
    }
    private static void elog(String m) { Hello.mark(m); Hello.log("exit: " + m); try { Hello.exitStep(-1, m); } catch (Throwable t) { } }   // the second copy goes to exitlog.txt, which the next start prints again
    /** In-process return to the launcher (the console cannot restart or end this title): called when the game is over (shutdown hook, quit watcher, window closed, MC returned or crashed).
     *  Once per game: a fresh thread (64 MB stack, like the VM thread the launcher always ran on) shows the launcher again and starts the chosen game; the old game's thread stays parked in its System.exit. */
    private static final java.util.concurrent.atomic.AtomicBoolean ENDED = new java.util.concurrent.atomic.AtomicBoolean();
    private static volatile int round = 1;
    private static volatile Thread relaunchThread;   // the game renames its thread ("Render thread", "Client thread"): compare the object
    private static volatile int loops;   // relaunch threads started so far (each keeps a 64 MB stack: one is normally enough, the console runs out of threads quickly)
    public static void leave(String why) { leave(why, true); }
    /** spawn=false: the caller is the relaunch thread itself, which is about to unwind out of the game and starts the next round in its own loop (a new thread per round ran the console out of threads after a few rounds) */
    static void leave(String why, boolean spawn) {
        if (!ENDED.compareAndSet(false, true)) return;
        boolean off = false;
        try { off = Launcher.load(new File(DIR, "launcher.cfg")).getProperty("ui", "native").trim().equals("off"); } catch (Throwable t) { }
        if (off) { ENDED.set(false); leaveProcess(why); return; }   // no launcher to come back to (ui=off): the old way, end the title
        boolean closeApp = false;   // launcher.cfg onexit=app: when the game is over the app closes instead of showing the launcher (a crash still comes back to the launcher: its report is shown there)
        try { closeApp = Launcher.load(new File(DIR, "launcher.cfg")).getProperty("onexit", "launcher").trim().equals("app") && !crashedThisRun(); } catch (Throwable t) { }
        if (closeApp) { elog("onexit=app: the game is over (" + why + "), the app closes"); ENDED.set(false); leaveProcess("app exit (" + why + ")"); return; }
        elog("game over (" + why + "): back to the launcher, round " + (round + 1));
        showLastCrashReport();
        round++;
        if (!spawn) return;
        if ("1".equals(System.getProperty("ps5.noui"))) { elog("Vulkan title: the game is not restarted inside this process (LWJGL's Vulkan can be created only once per class loader): the app restarts into the launcher now"); ENDED.set(false); leaveProcess("vulkan game over"); return; }
        if (loops >= 3) { elog("too many relaunch threads (" + loops + "): ending the title"); ENDED.set(false); leaveProcess(why); return; }
        loops++;
        Runnable body = () -> {
            relaunchThread = Thread.currentThread();
            try { Thread.sleep(300); } catch (InterruptedException e) { }
            for (;;) {
                for (int w = 0; !gameDone && w < 100; w++) { try { Thread.sleep(50); } catch (InterruptedException e) { } }   // the old game's thread is still unwinding (max 5 s)
                if (!gameDone) elog("the old game's thread did not unwind in 5 s: going on anyway");
                String r = "?";
                try { r = playOnce(); } catch (Throwable x) { elog("round " + round + " failed: " + x); }
                elog("round " + round + " ended: " + r);
                showLastCrashReport();
                if (!started) ENDED.set(false);   // it never got as far as the game: the launcher comes back from this loop
                if (ENDED.compareAndSet(false, true)) { elog("game over (" + r + "): back to the launcher, round " + (round + 1)); round++; }   // (System.exit already did this and ENDED is set)
                try { Thread.sleep(300); } catch (InterruptedException e) { }
            }
        };
        // The game runs on this thread, so it wants a big stack (mod loaders recurse deeply). Thread stacks come from the console's flexible memory, which a 1.21 session leaves with only ~40 MB free: the 64 MB stack was refused
        // (pthread_create EAGAIN -> OutOfMemoryError "unable to create native thread", log 65902977) and the title ended instead of coming back to the launcher. Try smaller stacks before giving up.
        Thread t = null;
        for (long mb : new long[] { 64, 32, 16, 8 }) {
            try { Thread c = new Thread(null, body, "ps5-relaunch", mb << 20); c.start(); t = c; if (mb != 64) elog("relaunch thread started with a " + mb + " MB stack"); break; }
            catch (OutOfMemoryError e) { elog("relaunch thread: a " + mb + " MB stack was refused (" + e.getMessage() + ")"); }
        }
        if (t == null) { elog("relaunch thread: no stack size worked: ending the title"); ENDED.set(false); leaveProcess(why); }
    }

    static void leaveProcess(String why) {
        if (!LEFT.compareAndSet(false, true)) return;
        try {
            elog("leave(" + why + ") on thread " + Thread.currentThread().getName());
            int mask = Hello.exitStep(0, why);
            elog("restart calls available mask=0x" + Integer.toHexString(mask));
            for (int s = (mask & 0x1000) != 0 ? 3 : 1; s <= 5; s++) {
                elog("step " + s + " ...");
                int rc = Hello.exitStep(s, null);
                elog("step " + s + " returned 0x" + Integer.toHexString(rc));
            }
        } catch (Throwable t) { elog("failed: " + t); }
    }

    /** Quit watcher: the exit path of the title never ran (no shutdown hook, no window-close) although the game was quitting, so the VM sat there. If the game's quit log line is seen and nothing else happens,
     *  dump what every thread is doing (after 2 s and 4.5 s: where does it hang?) and finally bring the launcher back from this healthy thread. */
    static volatile boolean stopDumped;
    static void startExitWatch() {
        Thread t = new Thread(() -> {
            int dumped = 0;
            while (true) {
                try { Thread.sleep(500); } catch (InterruptedException e) { return; }
                { long sa = Hello.stopAt;   // leaving a world: the integrated server saves, then the game quits. A save that never ends froze the "Saving world" screen for good: show where it sits, then bring the launcher back
                  if (sa != 0 && !LEFT.get() && !ENDED.get()) { long sage = System.currentTimeMillis() - sa;
                    if (!stopDumped && sage > 8000) { stopDumped = true; dumpThreads("world save has taken " + sage / 1000 + " s"); }
                    if (sage > 35000) { elog("the world save did not finish in " + sage / 1000 + " s: bringing the launcher back (the world keeps what its autosave and the last pause-save wrote)"); stopDumped = false; Hello.stopAt = 0; leaveProcess("save hang"); continue; }
                  } else if (sa == 0) stopDumped = false; }
                long at = Hello.soundDownAt; if (at == 0 || LEFT.get() || ENDED.get()) { dumped = 0; continue; }
                long age = System.currentTimeMillis() - at; Hello.mark("watch: quit line " + age + " ms ago, process alive");
                if ((dumped == 0 && age > 1500) || (dumped == 1 && age > 3000)) { dumped++; dumpThreads("quit watch +" + age / 1000 + " s"); }
                if (age > 3500) { elog("the game said it is quitting " + age / 1000 + " s ago and nothing ended it: bringing the launcher back"); leave("quit watcher"); continue; }
            }
        }, "ps5-exit-watch");
        t.setDaemon(true); t.start();
    }
    static void dumpThreads(String why) {
        try {
            Hello.log("exit: ---- " + why + ": thread dump ----");
            for (java.util.Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                Thread th = e.getKey(); StackTraceElement[] st = e.getValue(); String n = th.getName();
                boolean key = n.equals("main") || n.contains("Client") || n.contains("Render") || n.startsWith("Thread-") || n.startsWith("ps5") || n.contains("Server") || n.contains("IO") || n.contains("Worker");
                StringBuilder sb = new StringBuilder("exit: [" + n + (th.isDaemon() ? " d" : "") + " " + th.getState() + "]");
                int lim = key ? 22 : 1;
                for (int i = 0; i < st.length && i < lim; i++) sb.append(i == 0 ? " " : " <- ").append(st[i].getClassName().replaceAll(".*\\.", "")).append('.').append(st[i].getMethodName()).append(':').append(st[i].getLineNumber());
                Hello.mark(sb.toString()); Hello.log(sb.toString()); try { Thread.sleep(25); } catch (InterruptedException x) { }
            }
            Hello.log("exit: ---- end of thread dump ----");
        } catch (Throwable t) { Hello.log("exit: thread dump failed: " + t); }
    }

    /** System.exit of the game must not start the VM's shutdown (it cannot end here, and a VM in shutdown refuses new shutdown hooks: the next round of Minecraft would fail "Shutdown in progress"):
     *  a SecurityManager turns it into "game over" (the launcher comes back) plus an exception that unwinds the caller. Needs -Djava.security.manager=allow on JDK 17/21 (jvm_host.cpp passes it). */
    static final class ExitGuard extends SecurityException { final int status; ExitGuard(int status) { super("System.exit(" + status + ") intercepted"); this.status = status; } }
    static volatile Thread procThread;   // a Forge installer processor is running on this thread (McForge): its System.exit is an exception for the caller, not "back to the launcher"
    static void installExitGuard() {
        try {
            System.setSecurityManager(new SecurityManager() {
                @Override public void checkExit(int status) { if (Thread.currentThread() == procThread) throw new ExitGuard(status); if (firstExit < 0) firstExit = status; elog("System.exit(" + status + ") intercepted on " + Thread.currentThread().getName()); leave("System.exit(" + status + ")", Thread.currentThread() != relaunchThread); throw new ExitGuard(status); }
                @Override public void checkPermission(java.security.Permission p) { }
                @Override public void checkPermission(java.security.Permission p, Object ctx) { }
                @Override public void checkRead(String f) { } @Override public void checkRead(String f, Object c) { } @Override public void checkRead(java.io.FileDescriptor f) { }
                @Override public void checkWrite(String f) { } @Override public void checkWrite(java.io.FileDescriptor f) { } @Override public void checkDelete(String f) { }
                @Override public void checkConnect(String h, int p) { } @Override public void checkConnect(String h, int p, Object c) { } @Override public void checkListen(int p) { } @Override public void checkAccept(String h, int p) { }
                @Override public void checkPropertyAccess(String k) { } @Override public void checkPropertiesAccess() { } @Override public void checkAccess(Thread t) { } @Override public void checkAccess(ThreadGroup g) { }
                @Override public void checkExec(String c) { } @Override public void checkLink(String l) { } @Override public void checkCreateClassLoader() { } @Override public void checkPackageAccess(String p) { } @Override public void checkPackageDefinition(String p) { }
                @Override public void checkSetFactory() { } @Override public void checkSecurityAccess(String t) { } @Override public void checkMulticast(java.net.InetAddress a) { }
            });
            Hello.log("exit guard: SecurityManager installed, System.exit brings the launcher back");
        } catch (Throwable t) {
            // JDK 24+ (our 25 build): no SecurityManager. The title's own JDK 25 has java.lang.Runtime.ps5ExitHook (ps5_jdk_patch25.py), called first thing in Shutdown.exit: same effect as checkExit.
            try {
                java.util.function.IntConsumer hook = status -> { if (Thread.currentThread() == procThread) throw new ExitGuard(status); elog("System.exit(" + status + ") intercepted on " + Thread.currentThread().getName()); leave("System.exit(" + status + ")", Thread.currentThread() != relaunchThread); throw new ExitGuard(status); };
                Runtime.class.getField("ps5ExitHook").set(null, hook);
                Hello.log("exit guard: no SecurityManager (" + t.getClass().getSimpleName() + "), Runtime.ps5ExitHook installed: System.exit brings the launcher back");
            } catch (Throwable x) { Hello.log("exit guard: no SecurityManager (" + t + ") and no Runtime.ps5ExitHook (" + x.getClass().getSimpleName() + "): System.exit goes through the shutdown hook, which works for one round only (rebuild the JDK 25 with the current ps5_jdk_patch25.py)"); }
        }
    }


    /** The newest HotSpot crash log (hs_err_pid*.log, written next to the title) is summarised into the UDP log once: header, native frames and Java frames, so a native crash can be read without fetching the file. */
    static void showLastHsErr() throws IOException {
        File best = null;
        for (String d : new String[] { "/app0", DIR }) {
            File[] fs = new File(d).listFiles((x, n) -> n.startsWith("hs_err_pid") && n.endsWith(".log"));
            if (fs != null) for (File f : fs) if (best == null || f.lastModified() > best.lastModified()) best = f;
        }
        if (best == null) { Hello.log("hs_err: no HotSpot crash log in /app0"); return; }
        File seen = new File(DIR, "hs_err_seen.txt"); String key = best.getName() + "@" + best.lastModified(), old = "";
        try { if (seen.isFile()) old = new String(java.nio.file.Files.readAllBytes(seen.toPath()), StandardCharsets.UTF_8).trim(); } catch (Throwable t) { }
        if (old.equals(key)) { Hello.log("hs_err: newest crash log " + best.getName() + " was already shown"); return; }
        try { java.nio.file.Files.write(seen.toPath(), key.getBytes(StandardCharsets.UTF_8)); } catch (Throwable t) { }
        java.util.List<String> ls = java.nio.file.Files.readAllLines(best.toPath(), StandardCharsets.ISO_8859_1);
        Hello.log("hs_err: " + best.getPath() + " (" + best.length() + " bytes, " + ls.size() + " lines) ----");
        int shown = 0, sec = 0;   // sec: lines left to print of the current section
        for (int i = 0; i < ls.size() && shown < 110; i++) {
            String l = ls.get(i);
            if (i < 14 && !l.isEmpty() && !l.startsWith("#  ") ) { Hello.log("hs_err: " + l); shown++; continue; }
            if (l.startsWith("# ") && i < 40 && (l.contains("SIG") || l.contains("Problematic") || l.contains("pc=") || l.contains("Internal Error") || l.contains("libjvm") || l.startsWith("# [") || l.startsWith("# V") || l.startsWith("# J") || l.startsWith("# C"))) { Hello.log("hs_err: " + l); shown++; continue; }
            if (l.startsWith("Current thread") || l.startsWith("siginfo") || l.startsWith("Native frames") || l.startsWith("Java frames") || l.startsWith("Stack:") || l.startsWith("Registers")) { sec = l.startsWith("Registers") ? 6 : 28; Hello.log("hs_err: " + l); shown++; continue; }
            if (sec > 0) { if (l.trim().isEmpty()) { sec = 0; continue; } Hello.log("hs_err: " + (l.length() > 230 ? l.substring(0, 230) : l)); shown++; sec--; }
        }
        Hello.log("hs_err: ---- end");
    }

    static String DIR = "/download0/mc";   // game directory; run() replaces it with the first location whose file operations all work (see pickGameDir)

    static class LineOut extends OutputStream {
        private final ByteArrayOutputStream buf = new ByteArrayOutputStream();
        @Override public synchronized void write(int b) {
            if (b == '\n') { String ln = new String(buf.toByteArray(), StandardCharsets.UTF_8); Hello.log(ln); buf.reset(); }
            else if (b != '\r' && buf.size() < 900) buf.write(b);
        }
    }

    // Sampling profiler for the "Render thread": 10 samples/s, one report every 10 s. Says whether frame time goes to the swap/vblank wait, to LWJGL/GL native
    // calls (driver CPU cost), to the JDK, or to Minecraft's own Java code (and which methods). Logged straight to the UDP log.
    // Panorama check: the GL log says the six panorama face textures are 1x1 on the GPU. Decode the same PNGs from client.jar with the same STB library MC uses and
    // log size, channels and the average colour, to tell "the file/decoder is wrong" from "the upload is wrong". One line per file ("pngprobe: ...").
    static void pngProbe() {
        try {
            for (String n : new String[] { "panorama_0", "panorama_3", "panorama_overlay" }) {
                String path = "/assets/minecraft/textures/gui/title/background/" + n + ".png";
                java.io.InputStream in = McBoot.class.getResourceAsStream(path);
                if (in == null) { Hello.log("pngprobe: " + path + " NOT FOUND in the classpath"); continue; }
                byte[] raw = Hello.readAll(in); in.close();
                java.nio.ByteBuffer buf = org.lwjgl.system.MemoryUtil.memAlloc(raw.length); buf.put(raw).flip();
                try (org.lwjgl.system.MemoryStack st = org.lwjgl.system.MemoryStack.stackPush()) {
                    java.nio.IntBuffer w = st.mallocInt(1), h = st.mallocInt(1), c = st.mallocInt(1);
                    java.nio.ByteBuffer px = org.lwjgl.stb.STBImage.stbi_load_from_memory(buf, w, h, c, 4);
                    if (px == null) { Hello.log("pngprobe: " + n + " decode FAILED: " + org.lwjgl.stb.STBImage.stbi_failure_reason()); }
                    else {
                        long r = 0, g = 0, b = 0, a = 0; int cnt = w.get(0) * h.get(0);
                        for (int i = 0; i < cnt; i++) { r += px.get(i * 4) & 255; g += px.get(i * 4 + 1) & 255; b += px.get(i * 4 + 2) & 255; a += px.get(i * 4 + 3) & 255; }
                        Hello.log("pngprobe: " + n + " " + raw.length + " bytes -> " + w.get(0) + "x" + h.get(0) + " channels-in-file=" + c.get(0) + " avg RGBA=" + (r / cnt) + "," + (g / cnt) + "," + (b / cnt) + "," + (a / cnt)
                                + " first pixel=" + (px.get(0) & 255) + "," + (px.get(1) & 255) + "," + (px.get(2) & 255) + "," + (px.get(3) & 255));
                        org.lwjgl.stb.STBImage.stbi_image_free(px);
                    }
                }
                org.lwjgl.system.MemoryUtil.memFree(buf);
            }
        } catch (Throwable t) { Hello.log("pngprobe: failed " + t); }
    }

    static void startProfiler() {
        Thread p = new Thread(() -> {
            java.util.HashMap<String, Integer> top = new java.util.HashMap<>(), mc = new java.util.HashMap<>();
            int swap = 0, lw = 0, jdk = 0, other = 0, n = 0;
            long next = System.nanoTime() + 10_000_000_000L;
            Thread rt = null; String lastSig = ""; int same = 0;
            for (;;) {
                try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                // The thread that runs the game: the relaunch thread in round 2+ (a game renames it: "Render thread" / "Client thread"; sampling a thread found by name picked a stale, sleeping "Render thread" in every
                // later round and the profile said 100% Thread.sleepNanos0), the thread named "Render thread" / "Client thread" in the first round.
                Thread cand = relaunchThread != null && started && !gameDone && relaunchThread.isAlive() ? relaunchThread : null;
                if (cand == null && (rt == null || !rt.isAlive() || !(rt.getName().equals("Render thread") || rt.getName().equals("Client thread") || rt.getName().equals("Minecraft main thread")))) {
                    for (Thread t : Thread.getAllStackTraces().keySet()) if ("Render thread".equals(t.getName()) || "Client thread".equals(t.getName()) || ("Minecraft main thread".equals(t.getName()) && t.isAlive())) { cand = t; break; }
                } else if (cand == null) cand = rt;
                if (cand == null) { rt = null; continue; }
                rt = cand;
                StackTraceElement[] st = rt.getStackTrace();
                if (st.length == 0) continue;
                { String sig = st[0] + "|" + (st.length > 1 ? st[1] : "");
                  if (sig.equals(lastSig)) { if (++same == 30 || same % 300 == 0) { StringBuilder sb = new StringBuilder("prof STALL " + same / 10 + "s in " + rt.getName() + ":"); for (int i = 0; i < Math.min(25, st.length); i++) sb.append("\n    at ").append(st[i]); Hello.log(sb.toString()); } } else { lastSig = sig; same = 0; } }
                n++;
                String c0 = st[0].getClassName(), m0 = st[0].getMethodName();
                top.merge(c0 + "." + m0, 1, Integer::sum);
                if (c0.equals("org.lwjgl.glfw.GLFW") && m0.contains("SwapBuffers")) swap++;
                else if (c0.startsWith("org.lwjgl.")) lw++;
                else if (c0.startsWith("java.") || c0.startsWith("jdk.") || c0.startsWith("sun.")) jdk++;
                else other++;
                for (StackTraceElement e : st) {
                    String c = e.getClassName();
                    if (!(c.startsWith("java.") || c.startsWith("jdk.") || c.startsWith("sun.") || c.startsWith("org.lwjgl.") || c.startsWith("com.mojang.blaze3d.platform.GlStateManager"))) {
                        mc.merge(c + "." + e.getMethodName(), 1, Integer::sum);
                        break;
                    }
                }
                if (System.nanoTime() >= next && n > 0) {
                    Hello.log("prof: n=" + n + " swap=" + swap * 100 / n + "% lwjgl/GL-native=" + lw * 100 / n + "% jdk=" + jdk * 100 / n + "% mc/other=" + other * 100 / n + "%");
                    Hello.log("prof top frames: " + topN(top, 6));
                    Hello.log("prof first MC frame: " + topN(mc, 6));
                    top.clear(); mc.clear(); swap = lw = jdk = other = n = 0;
                    next = System.nanoTime() + 10_000_000_000L;
                }
            }
        }, "prof");
        p.setDaemon(true);
        p.start();
    }

    static String topN(java.util.HashMap<String, Integer> m, int k) {
        java.util.ArrayList<java.util.Map.Entry<String, Integer>> l = new java.util.ArrayList<>(m.entrySet());
        l.sort((a, b) -> b.getValue() - a.getValue());
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < Math.min(k, l.size()); i++) sb.append(l.get(i).getKey()).append(" x").append(l.get(i).getValue()).append("; ");
        return sb.toString();
    }

    interface Step { void run() throws Exception; }

    static boolean step(StringBuilder sb, String name, Step s) {
        try { s.run(); sb.append(' ').append(name).append("=ok"); return true; }
        catch (Throwable t) {
            String m = String.valueOf(t.getMessage()); if (m.length() > 48) m = m.substring(m.length() - 48);
            sb.append(' ').append(name).append("=FAIL(").append(t.getClass().getSimpleName()).append(": ").append(m).append(')');
            return false;
        }
    }

    /** Runs every file operation Minecraft needs for a world (create/delete folders, atomic replace, session.lock lock, listing) under base. */
    static boolean fsProbe(String base) {
        final java.nio.file.Path root = java.nio.file.Paths.get(base);
        final java.nio.file.Path d = root.resolve("_fsprobe_" + (System.nanoTime() & 0xffff));
        final java.nio.file.Path a = d.resolve("a.txt"), bb = d.resolve("b.txt"), c = d.resolve("c.txt"), lock = d.resolve("session.lock");
        StringBuilder sb = new StringBuilder("fsprobe " + base + ":");
        boolean ok = true;
        ok &= step(sb, "mkdirs", () -> java.nio.file.Files.createDirectories(root));
        ok &= step(sb, "mkdir", () -> java.nio.file.Files.createDirectory(d));
        ok &= step(sb, "lstat", () -> java.nio.file.Files.readAttributes(d, java.nio.file.attribute.BasicFileAttributes.class, java.nio.file.LinkOption.NOFOLLOW_LINKS));
        ok &= step(sb, "write", () -> java.nio.file.Files.write(a, new byte[] { 1, 2, 3, 4 }));
        ok &= step(sb, "read", () -> { if (java.nio.file.Files.readAllBytes(a).length != 4) throw new java.io.IOException("short read"); });
        ok &= step(sb, "rename", () -> java.nio.file.Files.move(a, bb));
        ok &= step(sb, "replace", () -> { java.nio.file.Files.write(c, new byte[] { 9 });
            java.nio.file.Files.move(bb, c, java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE); });
        ok &= step(sb, "lock", () -> { try (java.nio.channels.FileChannel ch = java.nio.channels.FileChannel.open(lock, java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.WRITE)) {
            java.nio.channels.FileLock l = ch.tryLock(); if (l == null) throw new java.io.IOException("lock refused"); l.release(); } });
        step(sb, "list(soft)", () -> { try (java.util.stream.Stream<java.nio.file.Path> s = java.nio.file.Files.list(d)) { if (s.count() < 2) throw new java.io.IOException("listing short"); } });
        ok &= step(sb, "delfile", () -> { java.nio.file.Files.delete(c); java.nio.file.Files.delete(lock); });
        ok &= step(sb, "rmdir", () -> java.nio.file.Files.delete(d));
        Hello.log(sb.toString() + (ok ? "" : " => not usable"));
        return ok;
    }

    /** Persistent, fully working location for saves: the first candidate where every file operation Minecraft needs succeeds. */
    static String pickGameDir() {
        // Results so far: /data -> AccessDeniedException (this registered title is sandboxed), /download0 -> create/write/lock/list work but rename, delete and rmdir are refused.
        // Another hint: earlier launches left logs/ and hs_err_pid*.log inside /app0 (listed at startup), so /app0 accepted writes and kept them across runs.
        for (String p : new String[] { "/", "/app0", "/data", "/data/homebrew", "/temp0", "/tmp", "/savedata0", "/mnt", "/user" }) {
            File f = new File(p); String[] names = f.list();
            Hello.log("listing " + p + ": exists=" + f.exists() + (names == null ? " (not listable)" : " " + java.util.Arrays.toString(names).replaceAll("(.{0,380}).*", "$1")));
        }
        String[] cand = { "/app0/mc", "/temp0/mc", "/tmp/mc", "/mnt/usb0/ps5-mc", "/download0/mc" };
        String chosen = null;
        for (String c : cand) { boolean ok = fsProbe(c); if (ok && chosen == null) chosen = c; }
        if (chosen == null) { chosen = "/download0/mc"; Hello.log("fsprobe: no location passed every check, falling back to " + chosen); }
        else Hello.log("fsprobe: game directory = " + chosen);
        return chosen;
    }

    // Why does javax.crypto.Cipher (and so SSL) fail to initialize? Print the whole cause chain and check the files JceSecurity reads.
    static void diagCrypto() {
        String jh = System.getProperty("java.home");
        for (String f : new String[] { "/conf/security/java.security", "/conf/security/policy/unlimited/default_US_export.policy",
                                       "/conf/security/policy/unlimited/default_local.policy", "/conf/security/policy/limited/default_local.policy" })
            Hello.log("crypto: " + jh + f + " exists=" + new File(jh + f).exists());
        try { javax.crypto.Cipher.getInstance("AES/CBC/NoPadding"); Hello.log("crypto: Cipher.getInstance ok"); }
        catch (Throwable t) {
            Hello.log("crypto: Cipher.getInstance FAILED");
            for (Throwable c = t; c != null; c = c.getCause()) Hello.log("crypto:   cause: " + c);
            t.printStackTrace();
        }
        try { Hello.log("crypto: SSLContext default = " + javax.net.ssl.SSLContext.getDefault().getProtocol()); }
        catch (Throwable t) { Hello.log("crypto: SSLContext FAILED " + t); for (Throwable c = t.getCause(); c != null; c = c.getCause()) Hello.log("crypto:   cause: " + c); }
    }


    // ---------------- network probe (NETPROBE=1) ----------------
    // First probe version was misleading: the JDK's getaddrinfo is a stub that always fails (ps5_jdk_patch.py), so every DNS/HTTP/HTTPS step "failed" before touching the network, and a Java socket
    // connect WITH a timeout makes NIO switch the socket to non-blocking (NioSocketImpl.connect, millis > 0), which this console refuses (fcntl EPERM) -> "Permission denied" without any connect().
    // So: no names, no timeouts. Literal IPs only, blocking calls only, own daemon thread (a hang cannot stall the boot; "start" lines are logged before each step). The native half of the probe
    // (jvm_host.cpp, "net: c:" lines) tests the same things with plain POSIX calls and a hand-made UDP DNS lookup.
    static String chain(Throwable t) { StringBuilder sb = new StringBuilder(String.valueOf(t)); for (Throwable c = t.getCause(); c != null && sb.length() < 600; c = c.getCause()) sb.append(" <- ").append(c); return sb.toString(); }
    static String firstLine(byte[] b, int n) { String s = new String(b, 0, Math.max(n, 0), StandardCharsets.ISO_8859_1); int i = 0; while (i < s.length() && s.charAt(i) != '\r' && s.charAt(i) != '\n') i++; return s.substring(0, Math.min(i, 100)); }
    static void netProbe() {
        Thread t = new Thread(new Runnable() { public void run() { try { netProbeRun(); } catch (Throwable e) { Hello.log("net: java probe crashed " + chain(e)); } } }, "netprobe");
        t.setDaemon(true);
        t.start();
    }
    static void netProbeRun() throws Exception {
        String ip = System.getProperty("ps5.netprobe.ip", "1.1.1.1");
        int p80 = Integer.getInteger("ps5.netprobe.port80", 80), p443 = Integer.getInteger("ps5.netprobe.port443", 443);
        java.net.InetAddress addr = java.net.InetAddress.getByAddress(new byte[] { (byte) Integer.parseInt(ip.split("\\.")[0]), (byte) Integer.parseInt(ip.split("\\.")[1]), (byte) Integer.parseInt(ip.split("\\.")[2]), (byte) Integer.parseInt(ip.split("\\.")[3]) });
        long t0;
        Hello.log("net: java probe start, target " + ip + " (literal address, blocking calls, no timeouts)");
        File ca = new File(System.getProperty("java.home", "/app0/assets/jdk"), "lib/security/cacerts");
        Hello.log("net: java cacerts " + ca + " exists=" + ca.isFile() + " size=" + ca.length());
        // 1. plain blocking Socket.connect (timeout 0 = no NIO non-blocking switch) + raw HTTP
        Hello.log("net: java 1 socket connect " + ip + ":" + p80 + " ...");
        try { t0 = System.nanoTime();
              try (java.net.Socket so = new java.net.Socket()) {
                  so.connect(new java.net.InetSocketAddress(addr, p80));
                  Hello.log("net: java 1 connect ok, local " + so.getLocalSocketAddress() + " (" + ms(t0) + ")");
                  OutputStream o = so.getOutputStream(); o.write(("GET / HTTP/1.0\r\nHost: " + ip + "\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.ISO_8859_1)); o.flush();
                  byte[] b = new byte[300]; int n = so.getInputStream().read(b);
                  Hello.log("net: java 1 raw HTTP reply " + n + " bytes: " + firstLine(b, n) + " (" + ms(t0) + ")"); } }
        catch (Throwable e) { Hello.log("net: java 1 FAILED " + chain(e)); }
        // 2. HttpURLConnection to a literal IP, no timeouts set (the downloader's likely transport once a name -> IP lookup exists)
        Hello.log("net: java 2 HttpURLConnection http://" + ip + ":" + p80 + "/ ...");
        try { t0 = System.nanoTime();
              java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("http://" + ip + ":" + p80 + "/").openConnection(java.net.Proxy.NO_PROXY);
              c.setInstanceFollowRedirects(false);
              int code = c.getResponseCode();
              Hello.log("net: java 2 HTTP status " + code + " location=" + c.getHeaderField("Location") + " (" + ms(t0) + ")"); }
        catch (Throwable e) { Hello.log("net: java 2 FAILED " + chain(e)); }
        // 3. TLS handshake over a blocking socket (SNI name given explicitly, no resolver): shows whether JSSE + the trust store work at all
        Hello.log("net: java 3 TLS handshake " + ip + ":" + p443 + " ...");
        try { t0 = System.nanoTime();
              java.net.Socket raw = new java.net.Socket();
              raw.connect(new java.net.InetSocketAddress(addr, p443));
              javax.net.ssl.SSLSocket ss = (javax.net.ssl.SSLSocket) javax.net.ssl.SSLContext.getDefault().getSocketFactory().createSocket(raw, "one.one.one.one", p443, true);
              ss.startHandshake();
              javax.net.ssl.SSLSession se = ss.getSession();
              java.security.cert.Certificate[] pc = se.getPeerCertificates();
              Hello.log("net: java 3 TLS ok " + se.getProtocol() + " " + se.getCipherSuite() + " peer=" + (pc.length > 0 && pc[0] instanceof java.security.cert.X509Certificate ? ((java.security.cert.X509Certificate) pc[0]).getSubjectX500Principal().getName() : "?") + " (" + ms(t0) + ")");
              ss.close(); }
        catch (Throwable e) { Hello.log("net: java 3 FAILED " + chain(e)); }
        // 4. the real thing minus DNS: HttpsURLConnection to https://<literal ip>/ (hostname check against the certificate's IP SAN)
        Hello.log("net: java 4 HttpsURLConnection https://" + ip + ":" + p443 + "/ ...");
        try { t0 = System.nanoTime();
              java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("https://" + ip + ":" + p443 + "/").openConnection(java.net.Proxy.NO_PROXY);
              c.setInstanceFollowRedirects(false);
              int code = c.getResponseCode();
              Hello.log("net: java 4 HTTPS status " + code + " (" + ms(t0) + ")"); }
        catch (Throwable e) { Hello.log("net: java 4 FAILED " + chain(e)); }
        // 5./6. names (ps5lwjgl.c getaddrinfo replacement) and the real download: Mojang's version manifest over HTTPS, no timeouts
        Hello.log("net: java 5 DNS piston-meta.mojang.com ...");
        try { t0 = System.nanoTime();
              java.net.InetAddress[] r = java.net.InetAddress.getAllByName("piston-meta.mojang.com");
              Hello.log("net: java 5 DNS ok " + r.length + " address(es), first " + r[0].getHostAddress() + " (" + ms(t0) + ")"); }
        catch (Throwable e) { Hello.log("net: java 5 FAILED " + chain(e)); }
        Hello.log("net: java 6 HttpsURLConnection https://piston-meta.mojang.com/mc/game/version_manifest_v2.json ...");
        try { t0 = System.nanoTime();
              java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json").openConnection(java.net.Proxy.NO_PROXY);
              int code = c.getResponseCode();
              InputStream in = c.getInputStream(); ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] bb = new byte[8192]; int n; while ((n = in.read(bb)) > 0 && bo.size() < 2000000) bo.write(bb, 0, n); in.close();
              String body = bo.toString("UTF-8");
              Hello.log("net: java 6 HTTPS status " + code + ", " + bo.size() + " bytes, contains 1.20.1: " + body.contains("\"1.20.1\"") + ", begins " + body.substring(0, Math.min(60, body.length())).replaceAll("\\s+", " ") + " (" + ms(t0) + ")"); }
        catch (Throwable e) { Hello.log("net: java 6 FAILED " + chain(e)); }
        nioSelfTest();
        Hello.log("net: java probe done");
    }
    // 7. what Netty does: non-blocking ServerSocketChannel + SocketChannel + Selector on loopback, then a plain Socket with connect/read timeouts (what Minecraft's HTTP clients do).
    // fcntl(O_NONBLOCK) is refused on sockets here; ps5lwjgl.c emulates it (log lines "net: nb:"). Every step has its own try/catch and logs the top stack frames of a failure,
    // so the first real failure names the native call (the previous version stopped at one unexplained "Permission denied").
    static String top(Throwable e) { StackTraceElement[] st = e.getStackTrace(); StringBuilder sb = new StringBuilder(chain(e)); for (int i = 0; i < Math.min(5, st.length); i++) sb.append(" | ").append(st[i]); return sb.toString(); }
    static void nioSelfTest() {
        Hello.log("net: java 7 NIO loopback (non-blocking server socket + selector) ...");
        java.nio.channels.ServerSocketChannel ssc = null; java.nio.channels.Selector sel = null; java.nio.channels.SocketChannel c = null, srv = null; int port = 0;
        java.net.InetAddress lo = null; long t0 = System.nanoTime();
        try {
            lo = java.net.InetAddress.getByAddress(new byte[] { 127, 0, 0, 1 });
            ssc = java.nio.channels.ServerSocketChannel.open(); ssc.configureBlocking(false); ssc.bind(new java.net.InetSocketAddress(lo, 0));
            port = ((java.net.InetSocketAddress) ssc.getLocalAddress()).getPort();
            sel = java.nio.channels.Selector.open();
            Hello.log("net: java 7a server channel non-blocking + selector opened, port " + port + " (" + ms(t0) + ")");
        } catch (Throwable e) { Hello.log("net: java 7a FAILED " + top(e)); return; }
        try { ssc.register(sel, java.nio.channels.SelectionKey.OP_ACCEPT); Hello.log("net: java 7a2 register OP_ACCEPT ok"); } catch (Throwable e) { Hello.log("net: java 7a2 register FAILED " + top(e)); }
        try { c = java.nio.channels.SocketChannel.open(); c.configureBlocking(false); boolean done = c.connect(new java.net.InetSocketAddress(lo, port)); Hello.log("net: java 7b client connect returned " + done + " (" + ms(t0) + ")");
              for (int i = 0; !done && i < 20; i++) { done = c.finishConnect(); if (!done) Thread.sleep(50); }
              if (!done) Hello.log("net: java 7b finishConnect never completed"); }
        catch (Throwable e) { Hello.log("net: java 7b FAILED " + top(e)); return; }
        try { for (int i = 0; i < 20 && srv == null; i++) { srv = ssc.accept(); if (srv == null) Thread.sleep(50); } Hello.log("net: java 7c direct accept -> " + (srv != null ? "got a connection" : "null after 1 s") + " (" + ms(t0) + ")"); }
        catch (Throwable e) { Hello.log("net: java 7c direct accept FAILED " + top(e)); }
        if (srv != null) try { srv.configureBlocking(false); Hello.log("net: java 7c2 accepted channel non-blocking"); } catch (Throwable e) { Hello.log("net: java 7c2 FAILED " + top(e)); }
        java.nio.ByteBuffer bb = java.nio.ByteBuffer.allocate(16);
        if (srv != null) try { Hello.log("net: java 7d read with no data -> " + srv.read(bb) + " (0 = non-blocking behaves)"); } catch (Throwable e) { Hello.log("net: java 7d FAILED " + top(e)); }
        try { Hello.log("net: java 7g selectNow -> " + sel.selectNow()); sel.selectedKeys().clear(); } catch (Throwable e) { Hello.log("net: java 7g selectNow FAILED " + top(e)); }
        try { Hello.log("net: java 7g select(300) -> " + sel.select(300)); sel.selectedKeys().clear(); } catch (Throwable e) { Hello.log("net: java 7g select(300) FAILED " + top(e)); }
        if (srv != null && c != null) try {
            c.write(java.nio.ByteBuffer.wrap("ping".getBytes(StandardCharsets.ISO_8859_1)));
            int got = 0; for (int i = 0; i < 20 && got < 4; i++) { int n = srv.read(bb); if (n > 0) got += n; else Thread.sleep(50); }
            Hello.log("net: java 7e direct read got " + got + " bytes: " + new String(bb.array(), 0, bb.position(), StandardCharsets.ISO_8859_1));
        } catch (Throwable e) { Hello.log("net: java 7e FAILED " + top(e)); }
        if (srv != null && c != null) try {
            srv.register(sel, java.nio.channels.SelectionKey.OP_READ);
            c.write(java.nio.ByteBuffer.wrap("pong".getBytes(StandardCharsets.ISO_8859_1)));
            int n = sel.select(1500); Hello.log("net: java 7h selector with a readable channel -> " + n + " ready"); sel.selectedKeys().clear();
        } catch (Throwable e) { Hello.log("net: java 7h FAILED " + top(e)); }
        try {   // plain blocking-API Socket with a connect timeout and a read timeout
            java.net.Socket s = new java.net.Socket(); s.connect(new java.net.InetSocketAddress(lo, port), 2000); s.setSoTimeout(500);
            long t1 = System.nanoTime();
            try { int r = s.getInputStream().read(); Hello.log("net: java 7f read returned " + r + " (expected a timeout)"); }
            catch (java.net.SocketTimeoutException e) { Hello.log("net: java 7f Socket connect(timeout)+read(timeout) ok: timed out after " + ms(t1)); }
            s.close();
        } catch (Throwable e) { Hello.log("net: java 7f FAILED " + top(e)); }
        try { if (srv != null) srv.close(); if (c != null) c.close(); ssc.close(); sel.close(); } catch (Throwable e) { }
        hostSelfTest();
    }
    // 8. what "Open to LAN" does in 1.20.1 (IntegratedServer.publishServer, any IOException -> "Unable to host local game"): HttpUtil.getAvailablePort() = new ServerSocket(0); Netty binds the wildcard address on that
    // port with backlog 128 and registers with interest 0 then OP_ACCEPT; then new LanServerPinger() = new DatagramSocket() and a send to 224.0.2.60:4445 (LAN discovery); the multiplayer screen listens with a MulticastSocket(4445).
    static void hostSelfTest() {
        Hello.log("net: java 8 LAN-host sequence ...");
        int port = 0; java.net.InetAddress lo = null;
        try { lo = java.net.InetAddress.getByAddress(new byte[] { 127, 0, 0, 1 }); try (java.net.ServerSocket ss = new java.net.ServerSocket(0)) { port = ss.getLocalPort(); } Hello.log("net: java 8a ServerSocket(0) -> free port " + port); }
        catch (Throwable e) { Hello.log("net: java 8a FAILED " + top(e)); return; }
        for (int pp : new int[] { port, 25565, 8080, 40000 }) {   // Java's own view: wildcard / loopback binds on specific ports
            try (java.net.ServerSocket t = new java.net.ServerSocket(pp)) { Hello.log("net: java 8a2 ServerSocket(" + pp + ") wildcard ok -> " + t.getLocalSocketAddress()); }
            catch (Throwable e) { Hello.log("net: java 8a2 ServerSocket(" + pp + ") wildcard FAILED " + chain(e)); }
            try (java.net.ServerSocket t = new java.net.ServerSocket(pp, 50, lo)) { Hello.log("net: java 8a2 ServerSocket(" + pp + ") loopback ok"); }
            catch (Throwable e) { Hello.log("net: java 8a2 ServerSocket(" + pp + ") loopback FAILED " + chain(e)); }
        }
        java.nio.channels.Selector sel = null; java.nio.channels.ServerSocketChannel s2 = null; java.nio.channels.SocketChannel acc = null; java.net.Socket cl = null;
        try {
            sel = java.nio.channels.Selector.open();
            s2 = java.nio.channels.ServerSocketChannel.open(); s2.configureBlocking(false);
            s2.bind(new java.net.InetSocketAddress(port), 128);
            java.nio.channels.SelectionKey k = s2.register(sel, 0, null); k.interestOps(java.nio.channels.SelectionKey.OP_ACCEPT);
            Hello.log("net: java 8b wildcard bind (port " + port + ", backlog 128) + register + OP_ACCEPT ok, local address " + s2.getLocalAddress());
        } catch (Throwable e) { Hello.log("net: java 8b FAILED " + top(e)); }
        if (s2 != null && s2.isOpen()) try {
            cl = new java.net.Socket(); cl.connect(new java.net.InetSocketAddress(lo, port), 2000);
            int n = sel.select(1500); Hello.log("net: java 8c client connected, selector reports " + n + " ready");
            sel.selectedKeys().clear();
            acc = s2.accept(); Hello.log("net: java 8c accept -> " + (acc != null ? "ok, " + acc.getRemoteAddress() : "null"));
            if (acc != null) { acc.configureBlocking(false); acc.register(sel, java.nio.channels.SelectionKey.OP_READ); cl.getOutputStream().write(new byte[] { 1, 2, 3 }); cl.getOutputStream().flush();
                               n = sel.select(1500); Hello.log("net: java 8d accepted channel readable via selector: " + n + " ready"); }
        } catch (Throwable e) { Hello.log("net: java 8c FAILED " + top(e)); }
        try { if (acc != null) acc.close(); if (cl != null) cl.close(); if (s2 != null) s2.close(); if (sel != null) sel.close(); } catch (Throwable e) { }
        java.net.DatagramSocket ds = null;
        try { ds = new java.net.DatagramSocket(); Hello.log("net: java 8e new DatagramSocket() ok, local port " + ds.getLocalPort()); }
        catch (Throwable e) { Hello.log("net: java 8e new DatagramSocket() FAILED " + top(e)); }
        if (ds != null) {
            try { byte[] m = "[MOTD]ps5[/MOTD][AD]1[/AD]".getBytes(StandardCharsets.ISO_8859_1); ds.send(new java.net.DatagramPacket(m, m.length, java.net.InetAddress.getByAddress(new byte[] { (byte) 224, 0, 2, 60 }), 4445)); Hello.log("net: java 8f send to 224.0.2.60:4445 ok"); }
            catch (Throwable e) { Hello.log("net: java 8f send to 224.0.2.60:4445 FAILED " + top(e)); }
            try { java.net.DatagramSocket rx = new java.net.DatagramSocket(0, lo); rx.setSoTimeout(500); byte[] m = { 9, 8, 7 };
                  ds.send(new java.net.DatagramPacket(m, 3, lo, rx.getLocalPort())); java.net.DatagramPacket in = new java.net.DatagramPacket(new byte[16], 16); rx.receive(in); Hello.log("net: java 8g UDP loopback with SO_TIMEOUT: got " + in.getLength() + " bytes");
                  try { rx.receive(in); Hello.log("net: java 8g second receive returned (expected a timeout)"); } catch (java.net.SocketTimeoutException e) { Hello.log("net: java 8g receive timed out as expected"); } rx.close(); }
            catch (Throwable e) { Hello.log("net: java 8g UDP loopback FAILED " + top(e)); }
            ds.close();
        }
        try { java.net.MulticastSocket ms = new java.net.MulticastSocket(4445); Hello.log("net: java 8h MulticastSocket(4445) ok");
              try { ms.setSoTimeout(300); ms.joinGroup(java.net.InetAddress.getByAddress(new byte[] { (byte) 224, 0, 2, 60 })); Hello.log("net: java 8h joinGroup 224.0.2.60 ok"); } catch (Throwable e) { Hello.log("net: java 8h joinGroup FAILED " + top(e)); }
              ms.close(); }
        catch (Throwable e) { Hello.log("net: java 8h MulticastSocket(4445) FAILED " + top(e)); }
    }
    static String ms(long t0) { return ((System.nanoTime() - t0) / 1000000) + " ms"; }


    // ---------------- version loader ----------------
    // The point of this is that the classpath and the version stop being baked into the build. A version lives in <gameDir>/versions/<id>/ with <id>.jar and <id>.json (the vanilla launcher
    // layout), and its libraries in <gameDir>/libraries. The jars are loaded at run time, the main class and the asset index come from the JSON, and the version to run is chosen by the
    // ps5.mcversion property, else <gameDir>/versions/selected.txt, else the newest directory. If no version is installed this returns null and McBoot launches the built-in 1.20.1 as before.
    /** Class loader that takes org.lwjgl.* from its own jars before asking the parent: the app class path carries LWJGL 3.3, 26.x needs the 3.4 classes.
     *  strict (the 3.4 loader): org.lwjgl.* is NEVER taken from the parent. Letting one package through mixes two LWJGL generations in one game, and the VM kills it with
     *  "loader constraint violation: loader 'app' wants to load class org.lwjgl.PointerBuffer" as soon as a 3.3 class and a 3.4 class meet in one signature (26.1, log 3981c9d6:
     *  assets/lib34 had no lwjgl-glfw, so org.lwjgl.glfw.GLFW came from the app's 3.3 jar). Refusing is loud and names the jar to add; falling back silently corrupts the game. */
    static class ChildFirst extends java.net.URLClassLoader {
        final boolean strict; final String tag;
        final java.util.Set<String> missed = java.util.Collections.synchronizedSet(new java.util.LinkedHashSet<String>());
        ChildFirst(java.net.URL[] urls, ClassLoader parent) { this(urls, parent, false, "LWJGL"); }
        ChildFirst(java.net.URL[] urls, ClassLoader parent, boolean strict, String tag) { super(urls, parent); this.strict = strict; this.tag = tag; }
        String missed() { synchronized (missed) { return missed.isEmpty() ? "none" : missed.toString(); } }
        @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!name.startsWith("org.lwjgl.")) return super.loadClass(name, resolve);
            synchronized (getClassLoadingLock(name)) {
                Class<?> c = findLoadedClass(name);
                if (c == null) try { c = findClass(name); } catch (ClassNotFoundException e) {
                    int dot = name.lastIndexOf('.'); String pkg = dot > 0 ? name.substring(0, dot) : name;
                    if (missed.add(pkg)) Hello.log("lwjgl: the " + tag + " loader has no " + pkg + ".* "
                        + (strict ? "(jar missing from /app0/assets/lib34): refused, the app class path's 3.3 copy must not be mixed in" : "(taken from the app class path)"));
                    if (strict) throw e;
                    return super.loadClass(name, resolve);
                }
                if (resolve) resolveClass(c);
                return c;
            }
        }
    }
    /** Fabric / Quilt / vanilla in a VM that was started with Forge's module path (see sharedVm): the module path's ASM (org.objectweb.asm, securejarhandler's) is visible through the app loader and would shadow the ASM
     *  in the game's own libraries (Fabric Loader / Mixin want their version). These packages come from the game's jars first; everything else is delegated as usual. */
    static class PrefixFirst extends java.net.URLClassLoader {
        final String[] first;
        PrefixFirst(java.net.URL[] urls, ClassLoader parent, String... first) { super(urls, parent); this.first = first; }
        boolean mineRes(String n) { for (String p : first) if (n.startsWith(p.replace('.', '/'))) return true; return false; }
        /** Fabric Loader 0.19 stops with "duplicate ASM classes found on classpath" when getResources("org/objectweb/asm/ClassReader.class") returns the game's ASM and the VM's (Forge's module path): resources under the child-first prefixes come from the game's jars only (log 458247f5). */
        @Override public java.net.URL getResource(String n) { if (mineRes(n)) { java.net.URL u = findResource(n); if (u != null) return u; } return super.getResource(n); }
        @Override public java.util.Enumeration<java.net.URL> getResources(String n) throws java.io.IOException { if (mineRes(n)) { java.util.Enumeration<java.net.URL> e = findResources(n); if (e.hasMoreElements()) return e; } return super.getResources(n); }
        @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            boolean mine = false; for (String p : first) if (name.startsWith(p)) { mine = true; break; }
            if (!mine) return super.loadClass(name, resolve);
            synchronized (getClassLoadingLock(name)) {
                Class<?> c = findLoadedClass(name);
                if (c == null) try { c = findClass(name); } catch (ClassNotFoundException e) { return super.loadClass(name, resolve); }
                if (resolve) resolveClass(c);
                return c;
            }
        }
    }
    /** "lwjgl-glfw-3.4.3.jar" -> "lwjgl-glfw": the module name, so the 3.4 set and the app class path's 3.3 set can be compared. */
    static String lwModule(String jar) {
        String s = jar.endsWith(".jar") ? jar.substring(0, jar.length() - 4) : jar;
        return s.replaceAll("-\\d+(\\.\\d+)*([+-]\\S*)?$", "");
    }
    static class Version {
        String id, mainClass = "net.minecraft.client.main.Main", assetIndex = "", type = "release";
        java.util.List<java.net.URL> cp = new java.util.ArrayList<>();
        boolean derived; String jarPath = "";   // derived = a Fabric/Quilt style profile (inheritsFrom)
        int javaMajor = 8;   // javaVersion.majorVersion of the version JSON (8 = Minecraft up to 1.16.5 and older versions that have no such field, 16 = 1.17, 17 = 1.18 - 1.20.4, 21 = 1.20.5+, 25 = 26.x)
        boolean lwjgl2;      // lists LWJGL 2 (org.lwjgl.lwjgl:lwjgl): Minecraft 1.12.2 and older
        int nLwjgl;        // lwjgl34: how many of the first cp entries are the title's LWJGL 3.4 jars (they live in one shared loader, see playOnce)
        String legacyCp = "";   // modular: the libraries as BootstrapLauncher's -DlegacyClassPath (Forge's profile does not carry it: the official launcher passes -cp, and the loader falls back to java.class.path, which here is the title's own)
        boolean modular;   // Forge / NeoForge 1.17+: the version JSON has JVM arguments with a module path (BootstrapLauncher); they are VM-creation options, see needsOtherJvmArgs
        java.util.List<String> jvmArgs = new java.util.ArrayList<>(), gameArgs = new java.util.ArrayList<>();   // jvmArgs already joined (--module-path=...) for jvm_host.cpp; gameArgs = the profile's fixed game arguments (--launchTarget ...)
        boolean lwjgl34;   // this version lists LWJGL 3.4.x (Minecraft 26.x: SDL3 windowing, FreeType): the game gets the title's 3.4 jars (assets/lib34) in a child-first loader
    }
    static String jsonStr(String json, String key) {   // minimal reader: "key" : "value" at any depth; enough for the few fields we need, and avoids needing a JSON library before the classpath exists
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        return m.find() ? m.group(1) : null;
    }
    /** Minecraft before 1.6 reads sounds / music from <gameDir>/resources/<name> (1.6 - 1.7.2: assets/virtual/legacy/<name>) and its own download from the dead s3 host fails: copy them out of the hashed store once. */
    static void legacyAssets(String aid, String root, String gdir) throws Exception {
        java.io.File ix = new java.io.File(root, "assets/indexes/" + aid + ".json"); if (!ix.isFile()) return;
        java.util.Map<String, Object> top = McDownload.readJson(ix); if (top == null) return;
        boolean res = Boolean.TRUE.equals(top.get("map_to_resources")), virt = Boolean.TRUE.equals(top.get("virtual")); if (!res && !virt) return;
        java.util.Map<String, Object> objs = McDownload.obj(top.get("objects")); if (objs == null) return;
        java.io.File dst = res ? new java.io.File(gdir, "resources") : new java.io.File(root, "assets/virtual/legacy");
        int n = 0, miss = 0;
        for (java.util.Map.Entry<String, Object> e : objs.entrySet()) {
            java.util.Map<String, Object> ob = McDownload.obj(e.getValue()); String h = ob != null ? McDownload.str(ob.get("hash")) : null; if (h == null) continue;
            java.io.File src = new java.io.File(root, "assets/objects/" + h.substring(0, 2) + "/" + h), d = new java.io.File(dst, e.getKey());
            if (!src.isFile()) { miss++; continue; }
            if (d.isFile() && d.length() == src.length()) continue;
            d.getParentFile().mkdirs(); java.nio.file.Files.copy(src.toPath(), d.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING); n++;
        }
        Hello.log("assets: legacy index " + aid + " -> " + dst + ": " + n + " copied, " + miss + " not downloaded");
    }

    static String jsonAssetIndex(String json) {        // "assetIndex": { ... "id": "5" ... }
        int i = json.indexOf("\"assetIndex\"");
        if (i < 0) return null;
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"id\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        return m.find(i) ? m.group(1) : null;
    }
    static void collectJars(File dir, java.util.List<java.net.URL> out, int depth) {
        File[] fs = dir.listFiles();
        if (fs == null || depth > 8) return;
        java.util.Arrays.sort(fs);
        for (File f : fs) {
            if (f.isDirectory()) collectJars(f, out, depth + 1);
            else if (f.getName().endsWith(".jar")) try { out.add(f.toURI().toURL()); } catch (Throwable t) { Hello.log("version: skipping " + f + ": " + t); }
        }
    }
    static Version pickVersion(String gameDir) { return pickVersion(gameDir, "", ""); }
    static Version pickVersion(String gameDir, String instVersion, String customJar) {
        File vdir = new File(gameDir + "/versions");
        File[] vs = vdir.listFiles(f -> f.isDirectory() && (new File(f, f.getName() + ".jar").isFile() || new File(f, f.getName() + ".installed").isFile()));   // only complete installs (the downloader writes the jar, or for Fabric/Quilt profiles the .installed marker, last)
        if (vs == null || vs.length == 0) { Hello.log("version: no " + vdir + " directory, using the version built into this title"); return null; }
        java.util.Arrays.sort(vs);
        String want = System.getProperty("ps5.mcversion");
        if (want == null && instVersion != null && !instVersion.isEmpty()) want = instVersion;
        if (want == null) { File sel = new File(vdir, "selected.txt");
            if (sel.isFile()) try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(sel))) { String l = r.readLine(); if (l != null && !l.trim().isEmpty()) want = l.trim(); } catch (Throwable t) { Hello.log("version: cannot read " + sel + ": " + t); } }
        File chosen = null;
        if (want != null) { for (File f : vs) if (f.getName().equals(want)) chosen = f;
            if (chosen == null) Hello.log("version: requested " + want + " is not installed"); }
        if (chosen == null) chosen = vs[vs.length - 1];
        StringBuilder all = new StringBuilder();
        for (File f : vs) all.append(' ').append(f.getName());
        Hello.log("version: installed" + all + "; running " + chosen.getName());
        Version v = new Version(); v.id = chosen.getName();
        java.util.Map<String, Object> top;
        try { top = McDownload.resolve(gameDir, v.id); }   // follows "inheritsFrom" (Fabric, Quilt): merged libraries, the parent's asset index and game jar
        catch (Throwable t) { Hello.log("version: cannot read the JSON of " + v.id + ": " + t); return null; }
        String jarId = McDownload.str(top.get("_jarId")); if (jarId == null) jarId = v.id;
        File jar = new File(gameDir + "/versions/" + jarId + "/" + jarId + ".jar");
        if (customJar != null && !customJar.isEmpty()) { if (new File(customJar).isFile()) { Hello.log("version: custom game jar " + customJar); jar = new File(customJar); } else Hello.log("version: custom jar " + customJar + " not found, using " + jar); }
        if (!jar.isFile()) { Hello.log("version: " + jar + " is missing: falling back to the built-in version"); return null; }
        String mc = McDownload.str(top.get("mainClass")); if (mc != null) v.mainClass = mc;
        java.util.Map<String, Object> ai = McDownload.obj(top.get("assetIndex")); String aid = ai != null ? McDownload.str(ai.get("id")) : null; if (aid == null) aid = McDownload.str(top.get("assets")); if (aid != null) v.assetIndex = aid;
        String ty = McDownload.str(top.get("type")); if (ty != null) v.type = ty;
        { java.util.Map<String, Object> jv = McDownload.obj(top.get("javaVersion")); Object mj = jv != null ? jv.get("majorVersion") : null; if (mj instanceof Number) v.javaMajor = ((Number) mj).intValue(); }
        Hello.log("version: " + v.id + ": mainClass=" + v.mainClass + " assetIndex=" + v.assetIndex + " type=" + v.type + (top.containsKey("inheritsFrom") ? " (inherits " + top.get("inheritsFrom") + ")" : ""));
        java.util.List<Object> libList = McDownload.arr(top.get("libraries"));
        try { v.cp.add(jar.toURI().toURL()); } catch (Throwable t) { Hello.log("version: bad jar url " + t); return null; }
        v.derived = top.containsKey("inheritsFrom"); v.jarPath = jar.getPath();
        if (libList != null) {   // exactly the libraries this version lists: the folder is shared by all versions, scanning it put the 1.20.1 AND the 1.21.1 jars of the same library on the class path (NoSuchMethodError in DFU's Codec)
            int missing = 0; java.util.Set<String> seen = new java.util.HashSet<>();
            for (Object o : libList) {
                java.util.Map<String, Object> lib = McDownload.obj(o); if (lib == null) continue;
                String name = McDownload.str(lib.get("name")); if (name == null || !McDownload.allowed(lib.get("rules"))) continue;
                if (!McDownload.WITH_LWJGL && name.startsWith("org.lwjgl")) continue;   // the title carries its own LWJGL
                java.util.Map<String, Object> dl = McDownload.obj(lib.get("downloads")), art = dl != null ? McDownload.obj(dl.get("artifact")) : null;
                String path = art != null ? McDownload.str(art.get("path")) : null;
                if (path == null && art == null && !lib.containsKey("natives") && (dl == null || !dl.containsKey("classifiers"))) path = McDownload.mavenPath(name);
                if (path == null || path.contains("-natives-") || !seen.add(path)) continue;
                File f = new File(gameDir + "/libraries/" + path);
                if (f.isFile()) { try { v.cp.add(f.toURI().toURL()); } catch (Throwable t) { } }
                else { missing++; Hello.log("version: library missing: " + path); }
            }
            Hello.log("version: " + (v.cp.size() - 1) + " libraries from the version JSON" + (missing > 0 ? ", " + missing + " MISSING (run the download again)" : ""));
        } else {
            collectJars(new File(gameDir + "/libraries"), v.cp, 0);
            collectJars(new File(chosen, "libraries"), v.cp, 0);
        }
        if (v.assetIndex.isEmpty()) {   // fall back to whatever index file is present
            File[] idx = new File(gameDir + "/assets/indexes").listFiles();
            if (idx != null) for (File f : idx) if (f.getName().endsWith(".json")) v.assetIndex = f.getName().substring(0, f.getName().length() - 5);
        }
        try { modularArgs(v, top, gameDir); } catch (Throwable t) { Hello.log("forge: cannot read the profile arguments: " + t); }
        String lw = lwjglVersion(libList);
        if (libList != null) for (Object o : libList) { java.util.Map<String, Object> lib = McDownload.obj(o); String n = lib != null ? McDownload.str(lib.get("name")) : null; if (n != null && n.startsWith("org.lwjgl.lwjgl:") && McDownload.allowed(lib.get("rules"))) v.lwjgl2 = true; }
        if (lw != null && lw.startsWith("3.4")) {   // Minecraft 26.x: the 3.4 jars first (a child-first loader keeps them apart from the 3.3 classes of the app class path)
            java.io.File[] js = new java.io.File("/app0/assets/lib34").listFiles((d, n) -> n.endsWith(".jar"));
            if (js == null || js.length == 0) Hello.log("lwjgl: this version needs LWJGL " + lw + " but the title has no 3.4 jars (/app0/assets/lib34): rebuild it with the launcher option");
            else {
                java.util.Arrays.sort(js); v.lwjgl34 = true; int at = 0;
                for (java.io.File f : js) { try { v.cp.add(at++, f.toURI().toURL()); } catch (Throwable t) { } }
                v.nLwjgl = at;
                java.util.TreeSet<String> have = new java.util.TreeSet<>(), gap = new java.util.TreeSet<>();
                for (java.io.File f : js) have.add(lwModule(f.getName()));
                for (String e : System.getProperty("java.class.path", "").split(java.io.File.pathSeparator)) {   // every LWJGL module the app class path has must exist in 3.4 too (see ChildFirst)
                    String b = new java.io.File(e).getName();
                    if (b.startsWith("lwjgl") && b.endsWith(".jar") && !have.contains(lwModule(b))) gap.add(lwModule(b));
                }
                Hello.log("lwjgl: " + js.length + " LWJGL 3.4 jars first on the class path (child-first loader)"
                    + (gap.isEmpty() ? ", every app-class-path module covered" : "; MISSING from /app0/assets/lib34 (rebuild the title): " + gap));
            }
        }
        if (v.lwjgl2) {   // LWJGL 2 (Minecraft 1.12.2 and older): the title's own port of it (assets/lib2, built by build_lwjgl2.sh) first, loaded child-first so it wins over the LWJGL 3 classes of the app class path
            java.io.File j2 = new java.io.File("/app0/assets/lib2/lwjgl2_ps5.jar");
            if (!j2.isFile()) { v.lwjgl2 = false; Hello.log("lwjgl: " + v.id + " needs LWJGL 2 but this title has no /app0/assets/lib2/lwjgl2_ps5.jar: rebuild it (build_lwjgl2.sh, LWJGL2=0 leaves it out)"); }
            else { try { v.cp.add(0, j2.toURI().toURL()); Hello.log("lwjgl: LWJGL 2 port first on the class path (child-first loader)"); } catch (Throwable t) { } }
        }
        Hello.log("version: classpath has " + v.cp.size() + " jars, Java " + v.javaMajor + (v.lwjgl2 ? ", LWJGL 2" : ""));
        return v;
    }
    /** Which OpenGL the game gets: Minecraft 1.17 and newer draw with shaders (the console's core profile as it is); 1.16.5 and older draw with the fixed-function pipeline,
     *  which gl4es (ps5gl4es.c) provides on top of the core profile. Decided from the version JSON's javaVersion (8 or missing = old); instance.cfg "prop.ps5.glcompat=0" / "=21" overrides it. */
    static void chooseGl(Version v) {
        int level = 0;
        if (v != null && !v.lwjgl34 && v.javaMajor < 16) level = 21;
        String ov = System.getProperty("ps5.glcompat");
        if (ov != null) try { level = Integer.parseInt(ov.trim()); } catch (Throwable t) { Hello.log("gl: ignoring prop.ps5.glcompat=" + ov); }
        try { Hello.glCompat(level); Hello.log("gl: " + (level > 0 ? "fixed-function OpenGL through gl4es (reports GL " + level / 10 + "." + level % 10 + ")" : "core profile as it is") + (v != null ? " [" + v.id + ", Java " + v.javaMajor + "]" : "")); }
        catch (Throwable t) { Hello.log("gl: this title has no fixed-function support (rebuild with the new Hello/jvm_host): " + t); }
    }
    /** The title has one LWJGL (3.3.3 in launcher builds), but mods and Minecraft compare org.lwjgl.Version.getVersion() with the LWJGL their Minecraft version lists (1.20.1: 3.3.1, 1.21.x: 3.3.3):
     *  "Installed version / Required version / You must change the LWJGL version in your launcher". The title's Version class reports -Dps5.lwjgl.version when it is set; the natives of a newer 3.3.x are a superset of an older one's. */
    static String lwjglVersion(java.util.List<Object> libs) {
        System.clearProperty("ps5.lwjgl.version");
        if (libs == null) { Hello.log("lwjgl: this version has no library list"); return null; }
        String want = null;
        for (Object o : libs) { java.util.Map<String, Object> lib = McDownload.obj(o); String n = lib != null ? McDownload.str(lib.get("name")) : null;
            if (n != null && n.startsWith("org.lwjgl:lwjgl:") && McDownload.allowed(lib.get("rules"))) { want = n.substring(16); break; } }
        if (want == null) { Hello.log("lwjgl: this version lists no org.lwjgl:lwjgl library (" + libs.size() + " libraries)"); return null; }
        if (want.startsWith("3.4")) {   // the title's 3.4 jars are 3.4.3: only a different 3.4.x has to be reported
            if (want.equals("3.4.3")) System.clearProperty("ps5.lwjgl.version"); else { System.setProperty("ps5.lwjgl.version", want); Hello.log("lwjgl: the title has 3.4.3, this version lists " + want + ": reporting " + want + " to the game"); }
            return want;
        }
        String have = null; try { have = org.lwjgl.Version.getVersion(); } catch (Throwable t) { Hello.log("lwjgl: cannot read the title's version: " + t); }
        if (have != null && !have.startsWith(want)) { System.setProperty("ps5.lwjgl.version", want); Hello.log("lwjgl: the title has " + have + ", this version lists " + want + ": reporting " + want + " to the game"); }
        else Hello.log("lwjgl: " + have + " matches what this version lists (" + want + ")");
        return want;
    }
    /** Fabric's Knot builds its world from the java.class.path property: the game and its libraries become its own code sources, and anything it must NOT load itself has to be named in fabric.systemLibraries
     *  (otherwise "can't load class ... it hasn't been exposed to the game"). The title's own LWJGL jars (static natives, loaded once by the system loader) are such libraries: keep them on the path and declare them. */
    /** Minecraft finds SRV records through Class.forName("com.sun.jndi.dns.DnsContextFactory"); Knot refuses classes whose code source is not a declared system library (log ae2d81e1: "hasn't been exposed to the game").
     *  The class lives in the title's hello directory (system loader), so that directory is the one entry to declare. */
    static String dnsSource() {
        try {
            java.security.CodeSource cs = Class.forName("com.sun.jndi.dns.DnsContextFactory").getProtectionDomain().getCodeSource();
            String p = cs == null ? null : new File(cs.getLocation().toURI()).getPath();
            Hello.log("dns: SRV resolver is declared from " + p);
            return p;
        } catch (Throwable t) { Hello.log("dns: resolver class not found " + t); return null; }
    }
    private static int jnaLaunch;
    private static String origClassPath;   // the title's own class path, read once: fabricProps replaces java.class.path, so a second round (Fabric after Fabric) would otherwise count the first round's game jars as "system libraries" and Knot could not find the game
    /** Quilt installs a URLStreamHandlerFactory (once per VM, Java refuses a second one): a game that ended or failed earlier in this VM leaves it behind and the next Quilt start dies with "factory already defined" / "Could not initialize class QuiltZipFileSystem". Clear it first (jvm_host opens java.net). */
    /** is a module of that name in the boot layer? (reflection: McBoot is compiled for Java 8, which has no ModuleLayer) */
    static boolean hasModule(String name) {
        try { Object boot = Class.forName("java.lang.ModuleLayer").getMethod("boot").invoke(null); Object opt = boot.getClass().getMethod("findModule", String.class).invoke(boot, name); return (Boolean) opt.getClass().getMethod("isPresent").invoke(opt); }
        catch (Throwable t) { return false; }
    }
    static void resetUrlFactory() {
        try { java.lang.reflect.Field f = java.net.URL.class.getDeclaredField("factory"); f.setAccessible(true); if (f.get(null) != null) { f.set(null, null); Hello.log("launcher: cleared the URL stream handler factory of the previous game"); } }
        catch (Throwable t) { Hello.log("launcher: URL factory not cleared (" + t + ")"); }
    }
    /** Forge's securejarhandler turns every jar into a "union:" URL, and the JDK finds that protocol through a URLStreamHandlerProvider service. After a Quilt / Fabric game in the same VM (its URL factory, handler cache) the second Forge start
     *  died with "MalformedURLException: unknown protocol: union" (log c9bf40da). Before a Forge start: drop a leftover factory, try the protocol, and if the JDK still does not know it, find the provider ourselves and install it as the URL factory. */
    static void ensureUnionUrl() {
        try {
            resetUrlFactory();
            try { new java.net.URL("union:/probe"); Hello.log("forge: the union: URL protocol is known"); return; } catch (java.net.MalformedURLException e) { Hello.log("forge: union: URLs are unknown to the JDK (" + e.getMessage() + "), looking for the provider"); }
            Class<?> pc = Class.forName("java.net.spi.URLStreamHandlerProvider");
            Object found = null;
            for (ClassLoader l : new ClassLoader[] { ClassLoader.getSystemClassLoader(), McBoot.class.getClassLoader(), Thread.currentThread().getContextClassLoader() }) {
                if (l == null || found != null) continue;
                try { for (Object pr : java.util.ServiceLoader.load((Class) pc, l)) { Object h = pc.getMethod("createURLStreamHandler", String.class).invoke(pr, "union"); if (h != null) { found = pr; break; } } } catch (Throwable t) { Hello.log("forge: provider lookup via " + l + ": " + t); }
            }
            if (found == null) {   // not offered as a service: the securejarhandler jar names its handler class; look for it by name
                String mp = System.getProperty("jdk.module.path", "") + java.io.File.pathSeparator + System.getProperty("java.class.path", "");
                outer:
                for (String e : mp.split(java.io.File.pathSeparator)) {
                    if (!e.contains("securejarhandler")) continue;
                    try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(e)) {
                        for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements();) {
                            String n = en.nextElement().getName();
                            if (!n.endsWith(".class") || !n.contains("URLStreamHandler")) continue;
                            String cn = n.substring(0, n.length() - 6).replace('/', '.');
                            try { Object o = Class.forName(cn, true, ClassLoader.getSystemClassLoader()).getDeclaredConstructor().newInstance(); Hello.log("forge: handler class " + cn);
                                if (pc.isInstance(o)) { found = o; break outer; } else if (o instanceof java.net.URLStreamHandler) { final java.net.URLStreamHandler hh = (java.net.URLStreamHandler) o; java.net.URL.setURLStreamHandlerFactory(pr -> "union".equals(pr) ? hh : null); found = hh; break outer; } } catch (Throwable t) { Hello.log("forge: " + cn + ": " + t); }
                        }
                    } catch (Throwable t) { Hello.log("forge: scanning " + e + ": " + t); }
                }
            }
            if (found == null) { Hello.log("forge: no provider for union: found"); return; }
            if (pc.isInstance(found)) { final Object prov = found; final java.lang.reflect.Method m = pc.getMethod("createURLStreamHandler", String.class); java.net.URL.setURLStreamHandlerFactory(pr -> { try { return "union".equals(pr) ? (java.net.URLStreamHandler) m.invoke(prov, pr) : null; } catch (Throwable t) { return null; } }); }
            try { new java.net.URL("union:/probe"); Hello.log("forge: union: URLs work now (provider installed as the URL factory)"); } catch (Throwable t) { Hello.log("forge: union: still unknown: " + t); }
        } catch (Throwable t) { Hello.log("forge: union: URL setup failed: " + t); }
    }
    /** Probe for the Quilt freeze: one 24 MB FileChannel.write of a heap array (what Quilt does when it copies the Minecraft jar), then the same in 64 KB pieces, timed. If the single write has not returned after 10 s the thread is left behind and false comes back,
     *  so the launcher can refuse to start Quilt (the game thread would freeze the app) instead of hanging. */
    static boolean largeWriteWorks() {
        final boolean[] done = new boolean[2]; final long[] ms = new long[2];
        Thread t = new Thread(() -> {
            File f = new File(DIR, ".cache/_wprobe.bin"); f.getParentFile().mkdirs();
            try {
                byte[] big = new byte[24 << 20];
                long t0 = System.currentTimeMillis();
                try (java.nio.channels.FileChannel c = java.nio.channels.FileChannel.open(f.toPath(), java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.WRITE, java.nio.file.StandardOpenOption.TRUNCATE_EXISTING)) { c.write(java.nio.ByteBuffer.wrap(big)); }
                ms[0] = System.currentTimeMillis() - t0; done[0] = true;
                t0 = System.currentTimeMillis();
                try (java.io.OutputStream o = java.nio.file.Files.newOutputStream(f.toPath())) { for (int i = 0; i < big.length; i += 65536) o.write(big, i, 65536); }
                ms[1] = System.currentTimeMillis() - t0; done[1] = true;
            } catch (Throwable x) { Hello.log("probe: large write failed: " + x); }
            f.delete();
        }, "ps5-wprobe");
        t.setDaemon(true); t.start();
        try { t.join(10000); } catch (InterruptedException e) { }
        Hello.log("probe: 24 MB in one FileChannel.write: " + (done[0] ? ms[0] + " ms" : "DID NOT RETURN in 10 s") + "; in 64 KB pieces: " + (done[1] ? ms[1] + " ms" : "not finished"));
        return done[0];
    }
    static String curGdir;
    /** VulkanMod turns GL calls into Vulkan by Mixin-patching org.lwjgl.opengl.GL11/GL14/GL15/GL30 (its compatibility.gl.* mixins). A class the shared loader owns cannot be patched, so the real GL11 ran with no GL context ("No context is current" at glTexSubImage2D, log 341d0ea7).
     *  On a Vulkan run with an enabled VulkanMod the LWJGL OpenGL jar is therefore left to Knot (still on the class path; its natives bind to Knot's loader alone: the shared loader never loads GL on a Vulkan run). */
    static boolean knotOwnsGl() {
        if (!"vulkan".equals(System.getProperty("ps5.renderer")) || curGdir == null) return false;
        String[] l = new File(curGdir, "mods").list(); if (l != null) for (String n : l) if (n.toLowerCase().contains("vulkanmod") && n.endsWith(".jar")) return true;
        return false;
    }
    static void fabricProps(Version v) {
        if (v.id.startsWith("quilt-loader-") && !largeWriteWorks()) throw new IllegalStateException("Quilt cannot start: one big file write never returns on this console (see the probe line in the log)");
        if (v.id.startsWith("quilt-loader-")) System.setProperty("loader.transform_cache.log_changed_keys", "true");   // "Not reusing previous transform cache since it has 1 different keys": the log then names the key (the 40 s first start comes back whenever it changes)
        if (v.id.startsWith("quilt-loader-")) {   // log e7d42205: Quilt's "Copying minecraft to a temporary jar file" is a stream of 1-byte writes, no stack in the log: every 20 s print the stack of whatever thread is inside Quilt/zip code, and the cache file size
            Thread w = new Thread(() -> { for (int k = 0; k < 9; k++) { try { Thread.sleep(20000); } catch (InterruptedException x) { return; }
                try { for (java.util.Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) { StackTraceElement[] st = e.getValue(); boolean q = false; for (StackTraceElement f : st) if (f.getClassName().contains("quiltmc") || f.getClassName().contains("zip")) { q = true; break; }
                    if (!q) continue; Hello.log("quilt watch " + (k + 1) + ": thread '" + e.getKey().getName() + "' " + e.getKey().getState()); for (int i = 0; i < st.length && i < 40; i++) { Hello.log("quilt watch:   at " + st[i]); if ((i & 7) == 7) try { Thread.sleep(3); } catch (InterruptedException x) { } } }
                    File f = new File(DIR, ".cache/quilt_loader/transform-cache-client/transformed-mod-minecraft-v1.20.1.jar"); Hello.log("quilt watch " + (k + 1) + ": cache jar " + f.length() + " bytes");
                } catch (Throwable x) { } } }, "ps5-quiltwatch"); w.setDaemon(true); w.start();
        }
        resetUrlFactory();
        System.setProperty("loader.disable_forked_guis", "true");   // Quilt shows its error screen in a forked JVM; this console cannot start processes (the real error then stays readable instead of "Cannot run program java")
        System.setProperty("fabric.debug.disableForkedGui", "true");
        if (origClassPath == null) origClassPath = System.getProperty("java.class.path", "");
        String orig = origClassPath;
        StringBuilder sys = new StringBuilder();
        String glJar = null;
        final boolean glK = v.lwjgl34 && knotOwnsGl();
        for (String e : orig.split(":")) if (e.endsWith(".jar") && new File(e).isFile() && !(glK && new File(e).getName().startsWith("lwjgl-opengl"))) sys.append(sys.length() > 0 ? ":" : "").append(e);   // jars only: the hello directory (Hello, McBoot) stays out of the game
        String dj = dnsSource(); if (dj != null) sys.append(sys.length() > 0 ? ":" : "").append(dj);   // SRV lookups: declared as system library so Knot lets the game reach it
        StringBuilder cpp = new StringBuilder();
        int from = 0;
        if (v.lwjgl34 && v.nLwjgl > 0 && v.nLwjgl < v.cp.size()) {   // Fabric on Minecraft 26.x: the title's LWJGL 3.4 jars (cp[0..nLwjgl)) live in the shared child-first loader that already holds their natives. Knot must not load its own copy
            // (the builtin native library "lwjgl" can be bound by one loader only: "Failed to locate library: liblwjgl.so"), so they are system libraries like the title's own jars: Knot asks its parent (the game loader on the shared one) for them.
            boolean glKnot = glK; if (glKnot) Hello.log("lwjgl: VulkanMod is on: the OpenGL classes are loaded by Fabric itself so its mixins can patch GL11/GL14/GL15/GL30");
            for (int i = 0; i < v.nLwjgl; i++) { String pth = v.cp.get(i).getPath(); if (glKnot && new File(pth).getName().startsWith("lwjgl-opengl")) { glJar = pth; continue; } sys.append(sys.length() > 0 ? ":" : "").append(pth); }
            from = v.nLwjgl;
        }
        for (int i = from; i < v.cp.size(); i++) cpp.append(cpp.length() > 0 ? ":" : "").append(v.cp.get(i).getPath());
        if (glJar != null) cpp.append(cpp.length() > 0 ? ":" : "").append(glJar);   // on Knot's path, but not a system library: Knot loads (and Mixin patches) it
        if (sys.length() > 0) cpp.append(":").append(sys);
        System.setProperty("java.class.path", cpp.toString());
        System.setProperty("fabric.systemLibraries", sys.toString());
        System.setProperty("fabric.gameJarPath", v.jarPath);
        Hello.log("launcher: Fabric class path: " + v.cp.size() + " version jars + system libraries " + (sys.length() == 0 ? "(none)" : sys.toString().replace("/app0/assets/", "")));
    }
    static String[] launchArgs(String id, String assetIndex, String type, String dir) {
        return new String[] {
            "--version", id, "--gameDir", dir, "--assetsDir", dir + "/assets", "--assetIndex", assetIndex,
            "--username", "Player", "--uuid", "00000000-0000-0000-0000-000000000000", "--accessToken", "0",
            "--userType", "legacy", "--versionType", type, "--width", String.valueOf(Launcher.displaySize("/app0/mc")[0]), "--height", String.valueOf(Launcher.displaySize("/app0/mc")[1]) };
    }

    public static void run() {
        if ("1".equals(System.getProperty("ps5.safemode"))) {   // jvm_host: the last VM start died before Java ran
            for (String d : new String[] { ".kit/patched", ".kit/stubs" }) { File[] fs = new File(DIR, d).listFiles(); if (fs != null) for (File f : fs) f.delete(); new File(DIR, d).delete(); }
            new File(DIR, ".kit/restarts.txt").delete();
            Hello.log("boot guard: the last VM start died before Java ran: patched modules and restart state cleared");
        }
        PrintStream ps = new PrintStream(new LineOut(), true);
        System.setOut(ps);
        System.setErr(ps);
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> { Hello.log("UNCAUGHT in " + t.getName() + ":"); e.printStackTrace(); });
        Hello.log("McBoot build: " + BUILD + " (java side of back2l-14)");
        DIR = pickGameDir();
        try {   // the exit marks of the previous run (written synchronously, so they survive a process that died a moment after): print them again, keep a copy
            new File(DIR, ".kit").mkdirs();
            File mf = new File(DIR, ".kit/exitmark.txt");
            if (mf.exists()) {
                Hello.log("prev: ---- exit marks of the previous run ----");
                try (BufferedReader r = new BufferedReader(new FileReader(mf))) { String ln; while ((ln = r.readLine()) != null) { Hello.log("prev: " + ln); try { Thread.sleep(20); } catch (InterruptedException e) { } } }
                Hello.log("prev: ---- end ----");
                File last = new File(DIR, ".kit/exitmark.last"); last.delete(); mf.renameTo(last);
            } else Hello.log("prev: no exit marks (the previous run never reached the quit line, or this is the first start of this build)");
            Hello.markPath = DIR + "/.kit/exitmark.txt"; Hello.mark("boot: mark file is writable");
        } catch (Throwable t) { Hello.log("prev: marks unavailable: " + t); }
        try { Hello.rmprobe(); } catch (Throwable t) { Hello.log("rmprobe native unavailable: " + t); }   // the same operations from C, but from inside the running JVM
        File assets = new File(DIR + "/assets");
        boolean made = assets.mkdirs() || assets.isDirectory();
        boolean wrote = false;   // File.canWrite() said false here, so test with a real write
        try { File f = new File(DIR + "/write_test.txt"); try (FileWriter fw = new FileWriter(f)) { fw.write("ok"); } wrote = f.length() == 2; f.delete(); }
        catch (Throwable t) { Hello.log("McBoot: write test failed: " + t); }
        Hello.log("McBoot: gameDir=" + DIR + " dir=" + made + " canWrite()=" + assets.canWrite() + " realWrite=" + wrote);
        diagCrypto();
        // GC/heap monitor (replaces a -Xlog:gc flag, which made JNI_CreateJavaVM fail with JNI_EINVAL): one log line every 5 s
        Thread gw = new Thread(() -> {
            for (;;) {
                try { Thread.sleep(5000); } catch (InterruptedException e) { return; }
                StringBuilder sb = new StringBuilder("gc:");
                try {
                    for (java.lang.management.GarbageCollectorMXBean g : java.lang.management.ManagementFactory.getGarbageCollectorMXBeans())
                        sb.append(' ').append(g.getName()).append('=').append(g.getCollectionCount()).append('/').append(g.getCollectionTime()).append("ms");
                } catch (Throwable t) { sb.append(" stats unavailable ").append(t); }
                Runtime r = Runtime.getRuntime();
                sb.append(" heap=").append((r.totalMemory() - r.freeMemory()) >> 20).append('/').append(r.totalMemory() >> 20).append("MB");
                try { java.lang.management.CompilationMXBean cm = java.lang.management.ManagementFactory.getCompilationMXBean(); sb.append(" jit=").append(System.getProperty("java.vm.name")).append('/').append(cm.getName()).append(' ').append(cm.getTotalCompilationTime()).append("ms"); } catch (Throwable t) { }   // C2 build: "Server VM/HotSpot 64-Bit Tiered Compilers"; C1 build: "Client VM/HotSpot 64-Bit Tiered Compilers"
                Hello.log(sb.toString());
            }
        }, "gc-watch");
        gw.setDaemon(true);
        gw.start();
        // pngProbe();   // off: it loaded LWJGL 3.3 (core natives!) in the app class loader, and a class loader of the 3.4 generation could then not load the same builtin native library ("already loaded in another classloader")
        if (new File(DIR, ".kit/prof").exists()) startProfiler();   // off by default: every sample is a JVM-wide ThreadDump safepoint (10 per second)
        startExitWatch();
        startCrashWatch();
        installExitGuard();
        try { showLastHsErr(); } catch (Throwable t) { Hello.log("hs_err: summary failed: " + t); }
        if ("1".equals(System.getProperty("ps5.netprobe"))) netProbe();
        // Offline bypass (UNVERIFIED property names, harmless if wrong): point authlib at a dead plain-http endpoint so it never needs SSL at startup.
        applyAuthHosts();
        String why = playOnce();   // round 1 runs here, on the VM thread, exactly as before
        if (!started) ENDED.set(false);
        leave(why);
        for (;;) { try { Thread.sleep(1000000); } catch (InterruptedException e) { } }   // never return: Hello.main returning would end the title
    }

    /** Offline bypass vs. real Mojang services, decided for EVERY round (right before the game starts): signing in inside the launcher and pressing Play must work without restarting the app.
     *  With a Microsoft account selected the game keeps the real hosts (online play, skins, chat signing); otherwise authlib is pointed at a dead plain-http endpoint so it never needs SSL at start-up. */
    static void applyAuthHosts() {
        String selAcc = Launcher.load(new File(DIR, "launcher.cfg")).getProperty("account", "").trim();
        String[] keys = { "minecraft.api.env", "minecraft.api.auth.host", "minecraft.api.account.host", "minecraft.api.session.host", "minecraft.api.services.host" };
        if (selAcc.isEmpty() || selAcc.equals("offline") || selAcc.equals("demo")) {
            System.setProperty(keys[0], "custom");
            for (int i = 1; i < keys.length; i++) System.setProperty(keys[i], "http://127.0.0.1:9");
            Hello.log("auth: " + (selAcc.equals("demo") ? "demo" : "offline") + ", Mojang services are switched off for this start");
        } else {
            for (String k : keys) System.clearProperty(k);
            Hello.log("auth: account " + selAcc + " selected, the game keeps the real Mojang service hosts");
        }
    }
    static ChildFirst LWJGL34, LWJGL2L; static int game34;   // the one loader that holds the LWJGL 3.4 classes + natives for every 26.x round
    static final java.util.HashMap<String, java.net.URLClassLoader> LOADERS = new java.util.HashMap<>();
    static boolean hooked;
    static volatile boolean started;   // the game's main() was reached in this round
    static volatile boolean gameDone = true;   // false while a game's main() runs; set when its thread has unwound and let go of the GL context (the launcher of the next round needs the context)
    static void memLog(String when) {
        try {
            Runtime r = Runtime.getRuntime(); String d = "";
            try { for (java.lang.management.BufferPoolMXBean b : java.lang.management.ManagementFactory.getPlatformMXBeans(java.lang.management.BufferPoolMXBean.class)) d += " " + b.getName() + "=" + b.getCount() + "/" + b.getMemoryUsed() / 1048576 + "MB"; } catch (Throwable t) { d = " (no buffer pools)"; }
            try { for (java.lang.management.MemoryPoolMXBean p : java.lang.management.ManagementFactory.getMemoryPoolMXBeans()) { String pn = p.getName(); if (pn.contains("CodeHeap") || pn.contains("CodeCache") || pn.contains("Metaspace")) d += " " + pn.replace("'", "") + "=" + p.getUsage().getUsed() / 1048576 + "MB"; }
                d += " threads=" + java.lang.management.ManagementFactory.getThreadMXBean().getThreadCount() + " classes=" + java.lang.management.ManagementFactory.getClassLoadingMXBean().getLoadedClassCount(); } catch (Throwable t) { }
            try { java.lang.management.CompilationMXBean cm = java.lang.management.ManagementFactory.getCompilationMXBean(); d += " vm=" + System.getProperty("java.vm.name") + " jit=" + cm.getName() + " compMs=" + cm.getTotalCompilationTime(); } catch (Throwable t) { }
            Hello.log("mem: " + when + ": heap " + (r.totalMemory() - r.freeMemory()) / 1048576 + "/" + r.totalMemory() / 1048576 + " MB (max " + r.maxMemory() / 1048576 + ") buffers" + d);
        } catch (Throwable t) { }
    }
    /** Launcher (Kit) + start of the chosen version; returns why it ended ("main returned", "crash java", ...). The game usually never returns: it ends in System.exit, which the shutdown hook below catches. */
    /** Automatic Java: the title carries every Java it was built with (8 / 17 / 21 / 25) but a process can start only ONE HotSpot, so the Java is chosen before the VM exists:
     *  jvm_host.cpp reads launcher.cfg java=<major the last chosen version needs> and boots the smallest linked Java that is at least that (a newer Java runs every older version).
     *  Here: record what this version needs; if this run's Java is too old, say so and end the app, the next start boots the right Java straight into the launcher. */
    /** LWJGL 3.3 (1.14 - 1.21.x, the title's own class path) and LWJGL 3.4 (26.x, its own shared loader) both bind the SAME built-in native libraries ("lwjgl", "lwjgl_glfw", ...), and the VM lets only one class loader have a
     *  library: after a 26.x game every 3.3 version failed with "Failed to locate library: liblwjgl.so" / "Could not initialize class org.lwjgl.glfw.GLFW" (and the other way round), and every failed attempt left classes, threads and
     *  GL objects behind. The first LWJGL 3 game of a process fixes its generation; the other generation ends the app with a message (the next start is a fresh process, the launcher comes up and Play works). LWJGL 2 is independent. */
    static String lwjglGen;
    static String lwjglGenLock(Version v) {
        if (v.lwjgl2) return null;
        String gen = v.lwjgl34 ? "3.4" : "3.3";
        if (lwjglGen == null || lwjglGen.equals(gen)) { lwjglGen = gen; return null; }
        Hello.log("lwjgl: " + v.id + " uses LWJGL " + gen + " but this run already loaded LWJGL " + lwjglGen + " (one class loader per native library): ending the app, press Play again after the restart");
        try { Thread.sleep(1500); } catch (InterruptedException e) { }
        try { Hello.quit("LWJGL " + lwjglGen + " -> " + gen + " needs a restart: open the app again and press Play"); } catch (Throwable t) { Hello.log("lwjgl: quit unavailable: " + t); }
        return "needs restart (LWJGL " + gen + ")";
    }
    /** The renderer is fixed per process (merged title, launcher.cfg renderer=gl|zink|vulkan, Settings > Renderer). The launcher UI only runs on OpenGL (SDK or Zink), so Vulkan is a one-game start: Play with Vulkan selected
     *  writes <root>/.kit/vk_once and ends the app; the next start (jvm_host.cpp) plays on Vulkan and the launcher is back after the game. A gl <-> zink change also needs a restart: the launcher is shown again, press Play. */
    /** Does this game have a Vulkan renderer at all? Minecraft 26.2+ ships its own backend (a VulkanBackend class in the client jar); before that only VulkanMod (a Fabric mod, in mods/ or set aside in mods-disabled/) gives a game Vulkan. */
    static boolean vulkanCapable(Version v, String gdir) {
        if (v == null) return true;
        if (v.lwjgl2) return false;
        try {
            for (java.net.URL u : v.cp) {
                String path = u.getPath(); if (path == null || !path.endsWith(".jar") || !path.contains("/versions/")) continue;
                try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(path)) {
                    for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements(); ) { String n = en.nextElement().getName(); if (n.endsWith("/VulkanBackend.class")) return true; }
                } catch (Throwable t) { }
            }
        } catch (Throwable t) { }
        if (v.derived && gdir != null) for (String d : new String[] { "mods", "mods-disabled" }) { String[] l = new File(gdir, d).list(); if (l != null) for (String n : l) if (n.toLowerCase().contains("vulkanmod") && (n.endsWith(".jar") || n.endsWith(".jar.disabled"))) return true; }
        return false;
    }
    /** Zink launcher -> Minecraft on Vulkan without ending the app: the launcher (Zink on RADV) has the display; the host's "vkswitch" tears Mesa's EGL down (zink_eglTerminate releases its swapchain), checks that a Vulkan swapchain can be made,
     *  and flips the renderer flags to Vulkan. Then this very run is a Vulkan run: options, mods and the class path are made for it from here on. Any failure falls back to the old way (end the app, the next start plays on Vulkan).
     *  touch .kit/no_vkswitch to turn it off (the restart path). */
    static void trySwitchToVulkan(Version v, String gdir, Launcher.Instance inst) {
        if (!"zink".equals(System.getProperty("ps5.renderer")) || new File(DIR, ".kit/no_vkswitch").exists()) return;
        String want = ""; try { want = Launcher.load(new File(DIR, "launcher.cfg")).getProperty("renderer", "").trim(); } catch (Throwable t) { }
        if (!want.equals("vulkan") || !vulkanCapable(v, gdir)) return;
        Hello.log("renderer: Vulkan selected, this run has zink: handing the display over to Vulkan inside this process (no restart)");
        try { System.clearProperty("ps5.vkswitch"); Hello.mark("vkswitch: begin"); Hello.quit(new File(DIR, ".kit/no_vkdefer").exists() ? "vkswitch" : "vkdefer"); Hello.mark("vkswitch: native hand-over returned"); } catch (Throwable t) { Hello.log("renderer: hand-over unavailable: " + t); return; }
        if (!"1".equals(System.getProperty("ps5.vkswitch"))) { Hello.log("renderer: the hand-over did not work: the app ends and the next start plays on Vulkan"); return; }
        System.setProperty("ps5.renderer", "vulkan"); System.setProperty("ps5.noui", "1"); System.setProperty("ps5.vulkan", "1");
        Launcher.applyOptions(inst, gdir);
        Hello.mark("vkswitch: options applied, game launch follows"); Hello.log("renderer: this run is Vulkan now (the launcher is back the next time the app starts)");
    }
    /** The console refuses chdir (errno 1), so mods that write "config/x" or "local/x" relative to the game folder failed (log 20453ee4: crash_assistant NoSuchFileException). Java resolves relative paths against a directory it cached at start: set that cache. */
    static boolean softChdir(String dir) {
        boolean ok = false; try { System.setProperty("user.dir", dir); } catch (Throwable t) { }
        try { Object fs = java.nio.file.FileSystems.getDefault(); Class<?> c = Class.forName("sun.nio.fs.UnixFileSystem"); java.lang.reflect.Field f = c.getDeclaredField("defaultDirectory"); f.setAccessible(true); f.set(fs, dir.getBytes(java.nio.charset.StandardCharsets.UTF_8)); ok = true; } catch (Throwable t) { Hello.log("cwd: nio default directory not set: " + t); }
        try { Class<?> d = Class.forName("java.io.DefaultFileSystem"); java.lang.reflect.Method gm = d.getDeclaredMethod("getFileSystem"); gm.setAccessible(true); Object fs = gm.invoke(null); java.lang.reflect.Field f = fs.getClass().getDeclaredField("userDir"); f.setAccessible(true); f.set(fs, dir); ok = true; } catch (Throwable t) { Hello.log("cwd: io user dir not set: " + t); }
        return ok;
    }
    static String needsOtherRenderer(Version v, String gdir) {
        String run = System.getProperty("ps5.renderer");   // set by jvm_host.cpp in the merged title only
        if (run == null) return null;
        String want = "";
        try { want = Launcher.load(new File(DIR, "launcher.cfg")).getProperty("renderer", "").trim(); } catch (Throwable t) { }
        boolean vkOk = vulkanCapable(v, gdir);
        if ("vulkan".equals(run) && !vkOk) {   // a Vulkan one-shot start of a game without a Vulkan renderer (26.1 and older, no VulkanMod): there is no OpenGL context, the game would die with "There is no OpenGL context current"
            Hello.log("renderer: " + (v == null ? "this version" : v.id) + " has no Vulkan renderer (Minecraft 26.2+ or VulkanMod needed): ending the app, it plays on the OpenGL renderer from the next start");
            try { Thread.sleep(1000); } catch (InterruptedException e) { }
            try { Hello.quit("This version has no Vulkan renderer: open the app again, it plays on OpenGL"); } catch (Throwable t) { Hello.log("renderer: quit unavailable: " + t); }
            return "no Vulkan renderer";
        }
        if (want.isEmpty() || want.equals(run)) return null;
        if (want.equals("vulkan") && !vkOk) { Hello.log("renderer: Vulkan selected, but " + (v == null ? "this version" : v.id) + " has no Vulkan renderer (Minecraft 26.2+ or VulkanMod needed): playing it on " + run); return null; }
        if (want.equals("zink") && "vulkan".equals(run)) return null;   // a one-shot Vulkan start of a game picked with another setting: the setting applies when the launcher is back
        if ("vulkan".equals(run)) return null;
        if (want.equals("vulkan")) {
            try { File k = new File(DIR, ".kit"); k.mkdirs(); try (java.io.FileOutputStream o = new java.io.FileOutputStream(new File(k, "vk_once"))) { o.write('1'); } } catch (Throwable t) { Hello.log("renderer: could not write .kit/vk_once: " + t); return null; }
            Hello.log("renderer: Vulkan selected, this run has " + run + ": ending the app, the next start plays this game on Vulkan");
            try { Thread.sleep(1000); } catch (InterruptedException e) { }
            try { Hello.quit("Open the app again: this game starts on Vulkan"); } catch (Throwable t) { Hello.log("renderer: quit unavailable: " + t); }
            return "needs Vulkan";
        }
        Hello.log("renderer: " + want + " selected, this run has " + run + ": ending the app, the next start uses " + want);
        markAutoplay("renderer " + want);
        restartNotice("Restarting the app for the " + want + " renderer", "Reopen ProsperoCraft from the home screen", autoplayOn() ? "The game starts by itself when the app is open again." : "Open the app again and press Play.");
        try { Hello.quit("Switched to " + want + ": press Play again"); } catch (Throwable t) { Hello.log("renderer: quit unavailable: " + t); }
        return "needs " + want;
    }
    /** Forge / NeoForge 1.17+: "arguments.jvm" holds --module-path, --add-modules, --add-opens ... (and -D flags BootstrapLauncher reads), "arguments.game" the fixed game arguments. The class path then
     *  is only what BootstrapLauncher gets as -DlegacyClassPath (the libraries, NOT the vanilla jar: Forge builds Minecraft from its own patched client jar in libraries/). */
    static void modularArgs(Version v, java.util.Map<String, Object> top, String gameDir) {
        java.util.Map<String, Object> ar = McDownload.obj(top.get("arguments")); if (ar == null) return;
        java.util.List<Object> jl = McDownload.arr(ar.get("jvm")); if (jl == null) return;
        boolean mp = false; java.util.List<String> raw = new java.util.ArrayList<>();
        for (Object o : jl) {
            if (o instanceof String) raw.add((String) o);
            else { java.util.Map<String, Object> m = McDownload.obj(o); if (m == null || !McDownload.allowed(m.get("rules"))) continue; Object val = m.get("value"); if (val instanceof String) raw.add((String) val); else { java.util.List<Object> vl = McDownload.arr(val); if (vl != null) for (Object x : vl) if (x instanceof String) raw.add((String) x); } }
        }
        for (String r : raw) if (r.equals("-p") || r.equals("--module-path")) mp = true;
        if (!mp) return;
        StringBuilder cp = new StringBuilder();
        for (int i = 1; i < v.cp.size(); i++) { if (cp.length() > 0) cp.append(':'); File pf = new File(DIR, ".kit/patched/" + new File(v.cp.get(i).getPath()).getName()); cp.append(pf.isFile() ? pf.getPath() : new File(v.cp.get(i).getPath()).getPath()); }   // a jar with a patched module-info (patchRequires) replaces the original
        java.util.Map<String, String> sub = new java.util.HashMap<>();
        sub.put("library_directory", gameDir + "/libraries"); sub.put("classpath_separator", ":"); sub.put("version_name", v.id); sub.put("classpath", cp.toString());
        sub.put("launcher_name", "ProsperoCraft"); sub.put("launcher_version", "1"); sub.put("natives_directory", gameDir + "/natives");
        java.util.List<String> out = new java.util.ArrayList<>();
        for (int i = 0; i < raw.size(); i++) {
            String a = sub(raw.get(i), sub);
            if (a.equals("-cp") || a.equals("-classpath") || a.equals("--class-path")) { i++; continue; }   // the title's own class path stays; the game's goes through -DlegacyClassPath
            if (a.equals("-p") || a.equals("--module-path") || a.equals("--add-modules") || a.equals("--add-opens") || a.equals("--add-exports") || a.equals("--add-reads") || a.equals("--patch-module")) {
                if (i + 1 < raw.size()) { String k = a.equals("-p") ? "--module-path" : a; out.add(k + "=" + sub(raw.get(++i), sub)); continue; }
            }
            out.add(a);
        }
        // The bootstrap layer's class loader has no parent: anything not in the layer must come from the boot layer's loader. The title's LWJGL jars (natives are bound to the app loader, one loader only) therefore go onto the
        // module path of the VM (boot layer, defined by the app loader): the game's modules read them from there. The same jars stay on the class path; the module wins for its packages.
        if (origClassPath == null) origClassPath = System.getProperty("java.class.path", "");
        StringBuilder lw = new StringBuilder();
        for (String e : origClassPath.split(":")) if (e.endsWith(".jar") && e.contains("lwjgl") && new File(e).isFile()) lw.append(':').append(e);
        // JDK modules the runtime image lacks (assets/extra/*.jar, built by make_jvm_title.sh: jdk.dynalink for Forge's Nashorn) join the module path when no module of that name is in the boot layer
        try {
            File ex = new File(new File(System.getProperty("java.home")).getParentFile(), "extra"); File[] xs = ex.listFiles((d, n) -> n.endsWith(".jar"));
            File[] old = new File(DIR, ".kit/stubs").listFiles(); if (old != null) { for (File f : old) f.delete(); new File(DIR, ".kit/stubs").delete(); Hello.log("forge: removed the old stub modules (a module named java.* on the module path made the VM abort at start)"); }
            java.util.List<File> all = new java.util.ArrayList<>(); if (xs != null) all.addAll(java.util.Arrays.asList(xs)); java.util.Collections.sort(all);
            for (File x : all) { String mn = x.getName().replaceAll("\\.jar$", ""); lw.append(':').append(x.getPath()); Hello.log("forge: module " + mn + " is not in this Java: added " + x.getPath() + " to the module path"); }
        } catch (Throwable t) { Hello.log("forge: extra modules: " + t); }
        for (int i = 0; i < out.size(); i++) if (out.get(i).startsWith("--module-path=")) {
            StringBuilder nv = new StringBuilder("--module-path=");   // jars whose module-info was patched (patchRequires) replace the originals
            for (String e : out.get(i).substring(14).split(":")) { if (e.isEmpty()) continue; File pf = new File(DIR, ".kit/patched/" + new File(e).getName()); if (nv.length() > 14) nv.append(':'); nv.append(pf.isFile() ? pf.getPath() : e); if (pf.isFile()) Hello.log("forge: module path uses the patched " + pf.getName()); }
            out.set(i, nv.append(lw).toString());
        }
        v.legacyCp = cp.toString();
        v.modular = true; v.jvmArgs = out;
        java.util.List<Object> gl = McDownload.arr(ar.get("game"));
        if (gl != null) for (int i = 0; i < gl.size(); i++) {   // plain strings only; a flag whose value is a ${placeholder} (the launcher fills those itself) is dropped with it
            if (!(gl.get(i) instanceof String)) continue; String g = (String) gl.get(i);
            if (g.startsWith("--") && i + 1 < gl.size() && gl.get(i + 1) instanceof String && ((String) gl.get(i + 1)).contains("${")) { i++; continue; }
            if (!g.contains("${")) v.gameArgs.add(g);
        }
        v.cp.remove(0);   // the vanilla jar is not on the game's class path (see above); cp[0] was only needed to build the string
        Hello.log("forge: " + v.id + " is a module-path profile: " + out.size() + " JVM options, game arguments " + v.gameArgs);
    }
    static String sub(String s, java.util.Map<String, String> m) { for (java.util.Map.Entry<String, String> e : m.entrySet()) s = s.replace("${" + e.getKey() + "}", e.getValue()); return s; }
    private static final java.util.List<java.nio.file.spi.FileSystemProvider> regFs = new java.util.ArrayList<>();
    /** Jars that bring a file system (Quilt's QuiltZipFileSystemProvider) list it in META-INF/services/java.nio.file.spi.FileSystemProvider; the JDK reads those from the system class path once, at its first use, and the
     *  game's jars are on a loader of ours: Quilt stopped with "Unable to load QuiltZipFileSystemProvider via services!". Add the providers of the game's libraries to the JDK's list (needs --add-opens java.base/java.nio.file.spi, jvm_host.cpp). */
    static void registerFsProviders(java.util.List<java.net.URL> jars, ClassLoader cl) {
        try {
            java.util.List<String> names = new java.util.ArrayList<>();
            for (java.net.URL u : jars) {
                if (!u.getPath().endsWith(".jar")) continue;
                try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(u.getPath())) {
                    java.util.zip.ZipEntry e = z.getEntry("META-INF/services/java.nio.file.spi.FileSystemProvider"); if (e == null) continue;
                    try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(z.getInputStream(e), StandardCharsets.UTF_8))) {
                        for (String l; (l = r.readLine()) != null; ) { int h = l.indexOf('#'); if (h >= 0) l = l.substring(0, h); l = l.trim(); if (!l.isEmpty() && !names.contains(l)) names.add(l); }
                    }
                } catch (Throwable t) { }
            }
            if (names.isEmpty()) return;
            java.nio.file.spi.FileSystemProvider.installedProviders();
            java.lang.reflect.Field f = java.nio.file.spi.FileSystemProvider.class.getDeclaredField("installedProviders"); f.setAccessible(true);
            @SuppressWarnings("unchecked") java.util.List<java.nio.file.spi.FileSystemProvider> cur = (java.util.List<java.nio.file.spi.FileSystemProvider>) f.get(null);
            java.util.List<java.nio.file.spi.FileSystemProvider> add = new java.util.ArrayList<>(cur);
            add.removeAll(regFs); regFs.clear();   // the previous round's providers belong to a loader that is gone
            for (String n : names) {
                boolean have = false; for (java.nio.file.spi.FileSystemProvider p : add) if (p.getClass().getName().equals(n)) have = true;
                if (have) continue;
                try { java.nio.file.spi.FileSystemProvider p = (java.nio.file.spi.FileSystemProvider) Class.forName(n, true, cl).getDeclaredConstructor().newInstance(); add.add(p); regFs.add(p); Hello.log("fs providers: registered " + n + " (scheme " + p.getScheme() + ")"); }
                catch (Throwable t) { Hello.log("fs providers: " + n + " not usable: " + t); }
            }
            f.set(null, java.util.Collections.unmodifiableList(add));
        } catch (Throwable t) { Hello.log("fs providers: not registered (" + t + ")"); }
    }
    static String jvmArgsId(Version v) { return v.modular ? Integer.toHexString(String.join("\n", v.jvmArgs).hashCode()) : ""; }
    /** The module-path options exist only when the VM is created: write them to <root>/.kit/jvmargs.txt (jvm_host.cpp passes them at the next start) and end the app, like needsOtherJava. The file goes away again for a plain version. */
    /** A Play that ends the app to get other VM options (Forge module path, other Java) is continued by the next start: no launcher screen, no second Play. The note is only honoured for 30 minutes and once. */
    /** A module the game layer requires but this runtime image does not have (java.instrument for Mixin, ...): the module that requires it gets a patched copy of its jar in <root>/.kit/patched where that requirement is "requires static"
     *  (resolved when the module exists, ignored when not: the classes are only needed by code paths the game does not take here). modularArgs puts the patched jar on the module path of the next VM. A stub module named java.* does not
     *  work: the VM aborts at start with it. Returns true when a jar was patched. */
    static boolean patchRequires(String missing, String requiredBy) {
        try {
            java.util.List<File> cands = new java.util.ArrayList<>();
            String mp = System.getProperty("jdk.module.path");
            if (mp != null) for (String e : mp.split(":")) if (e.endsWith(".jar") && new File(e).isFile()) cands.add(new File(e));
            String lcp = System.getProperty("legacyClassPath");   // Forge: BootstrapLauncher builds its own layer from these (Mixin is one of them, not on the VM's module path)
            if (lcp != null) for (String e : lcp.split(":")) if (e.endsWith(".jar") && new File(e).isFile()) cands.add(new File(e));
            if (cands.isEmpty()) { java.util.ArrayDeque<File> q = new java.util.ArrayDeque<>(); q.add(new File(DIR, "libraries")); int seen = 0; while (!q.isEmpty() && seen < 5000) { File[] fs = q.poll().listFiles(); if (fs == null) continue; for (File f : fs) { seen++; if (f.isDirectory()) q.add(f); else if (f.getName().endsWith(".jar")) cands.add(f); } } }
            for (File jar : cands) {
                byte[] mi; try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(jar)) { java.util.zip.ZipEntry e = z.getEntry("module-info.class"); if (e == null) continue; mi = Hello.readAll(z.getInputStream(e)); } catch (Throwable t) { continue; }
                String[] name = new String[1]; byte[] patched = staticRequires(mi, missing, name);
                if (patched == null || !requiredBy.equals(name[0])) continue;
                File d = new File(DIR, ".kit/patched"); d.mkdirs(); File target = new File(d, jar.getName()), out = new File(d, jar.getName() + ".new");   // (the jar may already be the patched copy from an earlier round: patch into a new file, then replace)
                try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(jar); java.util.zip.ZipOutputStream zo = new java.util.zip.ZipOutputStream(new BufferedOutputStream(new FileOutputStream(out)))) {
                    for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                        java.util.zip.ZipEntry x = en.nextElement(); String n = x.getName();
                        if (n.startsWith("META-INF/") && (n.endsWith(".SF") || n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC"))) continue;   // the signature would no longer match
                        zo.putNextEntry(new java.util.zip.ZipEntry(n));
                        if (n.equals("module-info.class")) zo.write(patched); else if (!x.isDirectory()) zo.write(Hello.readAll(z.getInputStream(x)));
                        zo.closeEntry();
                    }
                }
                target.delete(); if (!out.renameTo(target)) throw new IOException("cannot move " + out + " to " + target);
                Hello.log("forge: " + jar.getName() + " (module " + requiredBy + ") now requires " + missing + " only if present -> " + target.getPath());
                return true;
            }
            Hello.log("forge: no module " + requiredBy + " found on the module path to patch");
        } catch (Throwable t) { Hello.log("forge: patching the requirement on " + missing + ": " + t); }
        return false;
    }
    /** module-info.class with "requires <missing>" turned into "requires static <missing>" (ACC_STATIC_PHASE); null when it has no such requirement. name[0] gets the module's own name. */
    static byte[] staticRequires(byte[] b, String missing, String[] name) throws IOException {
        java.io.DataInputStream in = new java.io.DataInputStream(new java.io.ByteArrayInputStream(b));
        in.readInt(); in.readUnsignedShort(); in.readUnsignedShort();
        int n = in.readUnsignedShort(); String[] utf = new String[n]; int[] ref = new int[n]; int pos = 10;
        for (int i = 1; i < n; i++) {
            int tag = in.readUnsignedByte(); pos++;
            switch (tag) {
                case 1: utf[i] = in.readUTF(); pos += 2 + utf[i].getBytes(StandardCharsets.UTF_8).length; break;   // (modified UTF-8 differs only for NUL / supplementary characters: names do not contain them)
                case 3: case 4: case 9: case 10: case 11: case 12: case 17: case 18: in.skipBytes(4); pos += 4; break;
                case 5: case 6: in.skipBytes(8); pos += 8; i++; break;
                case 7: case 8: case 16: case 19: case 20: ref[i] = in.readUnsignedShort(); pos += 2; break;
                case 15: in.skipBytes(3); pos += 3; break;
                default: return null;
            }
        }
        in.skipBytes(6); pos += 6;   // access, this, super
        int ic = in.readUnsignedShort(); pos += 2; in.skipBytes(ic * 2); pos += ic * 2;
        int fc = in.readUnsignedShort(); pos += 2; int mc = 0; if (fc != 0) return null; mc = in.readUnsignedShort(); pos += 2; if (mc != 0) return null;
        int ac = in.readUnsignedShort(); pos += 2;
        for (int a = 0; a < ac; a++) {
            int ni = in.readUnsignedShort(); int len = in.readInt(); pos += 6;
            if ("Module".equals(utf[ni])) {
                int mn = in.readUnsignedShort(); name[0] = utf[ref[mn]]; in.skipBytes(4); int rc = in.readUnsignedShort(); int p = pos + 8; byte[] o = null;
                for (int r = 0; r < rc; r++) { int ri = in.readUnsignedShort(); int fl = in.readUnsignedShort(); in.readUnsignedShort();
                    if (missing.equals(utf[ref[ri]])) { if (o == null) o = b.clone(); fl |= 0x0040; fl &= ~0x0020; o[p + r * 6 + 2] = (byte) (fl >> 8); o[p + r * 6 + 3] = (byte) fl; } }
                return o;
            }
            in.skipBytes(len); pos += len;
        }
        return null;
    }
    /** A Play that restarts the app must not do it forever (a restart that does not change what the next start needs, or one that keeps failing): more than 2 restarts for the same reason within 10 minutes -> true, and the caller stays in the launcher. */
    static boolean restartLoop(String why) {
        try {
            File f = new File(DIR, ".kit/restarts.txt"); long now = System.currentTimeMillis(); int n = 0; StringBuilder keep = new StringBuilder();
            if (f.isFile()) for (String l : java.nio.file.Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) { int t = l.indexOf('\t'); if (t < 0) continue; long at; try { at = Long.parseLong(l.substring(0, t)); } catch (NumberFormatException e) { continue; } if (now - at > 10L * 60 * 1000) continue; keep.append(l).append('\n'); if (l.substring(t + 1).equals(why)) n++; }
            if (n >= 2) { Hello.log("restart guard: '" + why + "' already restarted the app " + n + " times in the last 10 minutes: not again; staying in the launcher (see the log lines above for why the start does not hold)"); return true; }
            f.getParentFile().mkdirs(); java.nio.file.Files.write(f.toPath(), (keep.toString() + now + "\t" + why + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Throwable t) { }
        return false;
    }
    /** launcher.cfg autoplay=on (launcher Settings > "Play again after a restart"): the Play that needed an app restart continues by itself after the app is opened again. Off by default. */
    static boolean autoplayOn() { try { return Launcher.load(new File(DIR, "launcher.cfg")).getProperty("autoplay", "off").trim().equals("on"); } catch (Throwable t) { return false; } }
    static void markAutoplay(String why) {
        try {
            File f = new File(DIR, ".kit/autoplay.txt");
            if (!autoplayOn()) { f.delete(); Hello.log("restart: " + why + ": press Play again after reopening the app (Settings > Play again after a restart is off)"); return; }
            File k = new File(DIR, ".kit"); k.mkdirs(); java.nio.file.Files.write(f.toPath(), (System.currentTimeMillis() + "\n" + why + "\n" + BUILD + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Throwable t) { Hello.log("autoplay: not written: " + t); }
    }
    static boolean takeAutoplay() {
        File f = new File(DIR, ".kit/autoplay.txt");
        try {
            if (!f.isFile()) return false;
            java.util.List<String> ls = java.nio.file.Files.readAllLines(f.toPath(), StandardCharsets.UTF_8); f.delete();
            if (!autoplayOn()) return false;
            long t = Long.parseLong(ls.get(0).trim());
            if (System.currentTimeMillis() - t > 30L * 60 * 1000) return false;
            Hello.log("launcher: continuing the Play that needed a restart (" + (ls.size() > 1 ? ls.get(1) : "") + "): no launcher screen this time");
            return true;
        } catch (Throwable x) { f.delete(); return false; }
    }
    /** Forge VM: the LWJGL jars are real modules (module-info) on the VM's module path, and Minecraft's DebugMemoryUntracker calls setAccessible on org.lwjgl.system.MemoryManage$DebugAllocator.untrack
     *  (log f6aff6aa: InaccessibleObjectException "module org.lwjgl does not opens org.lwjgl.system to module minecraft", GLX.make -> crash report -> System.exit(-1)). In a normal Forge run LWJGL comes from the game's
     *  libraries as automatic modules, which are open. Open every package of the boot layer's org.lwjgl* modules to everybody (Module.implAddOpens; --add-opens java.base/java.lang is a fixed option of jvm_host). */
    static void openLwjglModules() {
        try {
            Class<?> ml = Class.forName("java.lang.ModuleLayer"), mc = Class.forName("java.lang.Module");
            Object boot = ml.getMethod("boot").invoke(null);
            java.lang.reflect.Method nm = mc.getMethod("getName"), pk = mc.getMethod("getPackages"), op = mc.getDeclaredMethod("implAddOpens", String.class);
            op.setAccessible(true);
            int mods = 0, pkgs = 0;
            for (Object m : (Iterable<?>) ml.getMethod("modules").invoke(boot)) {
                String n = (String) nm.invoke(m); if (n == null || !n.startsWith("org.lwjgl")) continue;
                mods++; for (Object p : (Iterable<?>) pk.invoke(m)) { op.invoke(m, (String) p); pkgs++; }
            }
            Hello.log("forge: " + pkgs + " packages of " + mods + " LWJGL modules opened to the game (reflection on them is allowed as with automatic modules)");
        } catch (Throwable t) { Hello.log("forge: could not open the LWJGL modules: " + t); }
    }
    /** FML's "early window" is a second render thread (pool-N-thread-1) that owns the GL context while the mods load. A Forge start that fails after it was created (log b9536ff2: Nashorn without its resource bundle) left that thread
     *  running: the launcher of the next round then died with "VertexShader linkage failure" / "renderer or fonts failed" / "cannot start the launcher thread", and every further Play failed the same way. The title has one window and its own
     *  loading screen (Kit) for the same time, so FML does not get to open one: config/fml.toml earlyWindowControl = false (Minecraft creates its window itself, as without Forge). touch .kit/fml_early to leave the setting alone. */
    static void fmlNoEarlyWindow(String gdir) {
        try {
            if (new File(DIR, ".kit/fml_early").exists()) return;
            File f = new File(gdir, "config/fml.toml"); f.getParentFile().mkdirs();
            String txt = f.isFile() ? new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8) : "";
            String n = java.util.regex.Pattern.compile("(?m)^\\s*earlyWindowControl\\s*=.*$").matcher(txt).find()
                ? txt.replaceAll("(?m)^(\\s*)earlyWindowControl\\s*=.*$", "$1earlyWindowControl = false")
                : txt + (txt.isEmpty() || txt.endsWith("\n") ? "" : "\n") + "earlyWindowControl = false\n";
            if (!n.equals(txt)) { java.nio.file.Files.write(f.toPath(), n.getBytes(StandardCharsets.UTF_8)); Hello.log("forge: config/fml.toml earlyWindowControl = false (FML's own loading window would take the title's only window and GL context)"); }
        } catch (Throwable t) { Hello.log("forge: fml.toml not updated: " + t); }
    }
    /** The app is about to end (the console has no way to start it again, and VM options only exist at VM creation): say so on the loading screen first, and keep it up long enough to read. */
    static void restartNotice(String title, String stage, String detail) {
        Hello.log("restart: " + title + " | " + stage + " | " + detail);
        boolean shown = false;
        try { shown = Kit.bootNotice(title, stage, detail); } catch (Throwable t) { }
        try { Thread.sleep(shown ? 5000 : 1500); } catch (InterruptedException e) { }
    }
    /** The VM was created with Forge's module path (jvm_host.cpp passes .kit/jvmargs.txt; its first line is the id). */
    static boolean flavoured() { return !System.getProperty("ps5.jvmargs.id", "").isEmpty(); }
    static volatile boolean sharedRun;
    /** A plain game (Fabric / Quilt / vanilla) can stay in the Forge-flavoured VM instead of restarting the app: the module path only adds modules (bootstraplauncher, securejarhandler, ASM, LWJGL as modules), and the game's loader
     *  takes ASM from its own jars (PrefixFirst). Not when .kit/vm_split exists, nor for two hours after such a game died within seconds of its start in this kind of VM (noteSharedFailure): then the old way (restart into a plain VM) is used. */
    static final String BUILD = "exitmarks-9";   // a sharedvm_off marker from another build is ignored (the cause may be fixed)
    static boolean sharedVm(Version v) {
        if (v.modular || !flavoured()) return false;
        if (v.javaMajor > 21 || v.lwjgl34) { Hello.log("vm: " + v.id + " (Java " + v.javaMajor + (v.lwjgl34 ? ", LWJGL 3.4" : "") + ") cannot share the Forge module path (its own LWJGL generation needs a fresh process anyway): plain VM"); return false; }
        if (new File(DIR, ".kit/vm_split").exists()) { Hello.log("vm: .kit/vm_split exists: " + v.id + " gets a plain VM (restart)"); return false; }
        try {
            File f = new File(DIR, ".kit/sharedvm_off");
            if (f.isFile()) {
                java.util.List<String> ls = java.nio.file.Files.readAllLines(f.toPath(), StandardCharsets.UTF_8);
                if (ls.size() > 2 && ls.get(2).trim().equals(BUILD) && System.currentTimeMillis() - Long.parseLong(ls.get(0).trim()) < 2L * 3600 * 1000) { Hello.log("vm: a plain game died at start in the Forge-flavoured VM (" + (ls.size() > 1 ? ls.get(1) : "") + "): " + v.id + " gets a plain VM (restart)"); return false; }
                f.delete();
            }
        } catch (Throwable t) { }
        Hello.log("vm: the VM has Forge's module path; " + v.id + " is played in it (no restart)");
        return true;
    }
    static void noteSharedFailure(String why) {
        try {
            if (!sharedRun || firstExit == 0 || System.currentTimeMillis() - gameStartMs > 25000) return;
            File k = new File(DIR, ".kit"); k.mkdirs();
            java.nio.file.Files.write(new File(k, "sharedvm_off").toPath(), (System.currentTimeMillis() + "\n" + why + "\n").getBytes(StandardCharsets.UTF_8));
            Hello.log("vm: the game died " + (System.currentTimeMillis() - gameStartMs) / 1000 + " s after its start in the Forge-flavoured VM: the next plain game restarts into a plain VM instead (" + why + ")");
        } catch (Throwable t) { }
    }
    /** The one restart Forge needs (its module path is a VM-creation option) happens when the VM was started without it. Playing anything else in a plain VM while a Forge / NeoForge version is installed writes .kit/jvmargs.txt right away
     *  (no restart, no autoplay): the next start of the app already has Forge's VM, the Fabric / Quilt / vanilla runs share it (sharedVm) and Forge itself starts without leaving the app. */
    static void primeForgeVm(Version cur) {
        try {
            if (flavoured() || McMods.runJava() < 17 || McMods.runJava() > 21 || cur.javaMajor > 21 || new File(DIR, ".kit/vm_split").exists() || cur == null || cur.modular) return;
            File vd = new File(DIR, "versions"); String[] names = vd.list(); if (names == null) return; java.util.Arrays.sort(names);
            for (String n : names) {
                if (!(n.contains("-forge-") || n.startsWith("neoforge-")) || !new File(vd, n + "/" + n + ".json").isFile()) continue;
                Version f = pickVersion(DIR, n, "");
                if (f == null || !f.id.equals(n) || !f.modular || f.jvmArgs.isEmpty()) continue;
                File k = new File(DIR, ".kit"); k.mkdirs();
                java.util.List<String> ls = new java.util.ArrayList<>(); ls.add("#id " + jvmArgsId(f)); ls.addAll(f.jvmArgs); java.nio.file.Files.write(new File(k, "jvmargs.txt").toPath(), ls, StandardCharsets.UTF_8);
                Hello.log("forge: " + n + " is installed: .kit/jvmargs.txt written now, the next start of the app has Forge's VM (" + cur.id + " runs in this plain VM; no restart for it, none later for Forge)");
                return;
            }
        } catch (Throwable t) { Hello.log("forge: priming the VM failed: " + t); }
    }
    static String needsOtherJvmArgs(Version v) {
        String want = jvmArgsId(v), have = System.getProperty("ps5.jvmargs.id", "");
        if (want.equals(have)) return null;
        if (want.isEmpty() && sharedVm(v)) return null;   // Fabric / Quilt / vanilla run in a Forge-flavoured VM too (sharedVm): no restart
        try {
            File k = new File(DIR, ".kit"); k.mkdirs(); File f = new File(k, "jvmargs.txt");
            if (want.isEmpty()) f.delete();
            else { java.util.List<String> ls = new java.util.ArrayList<>(); ls.add("#id " + want); ls.addAll(v.jvmArgs); java.nio.file.Files.write(f.toPath(), ls, StandardCharsets.UTF_8); }
        } catch (Throwable t) { Hello.log("forge: cannot write .kit/jvmargs.txt: " + t); return "jvm args not written"; }
        if (restartLoop("JVM options for " + v.id)) { new File(DIR, ".kit/jvmargs.txt").delete(); return "restart loop (JVM options)"; }
        Hello.log("forge: " + v.id + " needs " + (want.isEmpty() ? "no extra" : v.jvmArgs.size()) + " JVM options that this run (" + (have.isEmpty() ? "none" : have) + ") does not have: ending the app, the next start creates the VM with them");
        markAutoplay("JVM options for " + v.id);
        restartNotice(want.isEmpty() ? "Restarting the app to leave Forge mode" : "Restarting the app for Forge / NeoForge", "Reopen ProsperoCraft from the home screen", (autoplayOn() ? v.id + " starts by itself when the app is open again." : "Open the app again and press Play for " + v.id + "."));
        try { Hello.quit("Switched JVM options for " + v.id + ": press Play again"); } catch (Throwable t) { Hello.log("forge: quit unavailable: " + t); }
        return "needs JVM options";
    }
    static String needsOtherJava(Version v, Launcher.Instance inst) {
        int have = Hello.javaMajor();
        String setting = "auto";   // the global Java setting: launcher.cfg jdk=8|17|21|25 (launcher Settings), absent = automatic
        try { setting = Launcher.load(new File(DIR, "launcher.cfg")).getProperty("jdk", "auto").trim(); } catch (Throwable t) { }
        Hello.log("java: running Java " + System.getProperty("java.version") + " (" + System.getProperty("java.home") + "), linked: " + System.getProperty("ps5.jdks", "?") + ", launcher setting: " + setting);
        try {
            File cf = new File(DIR, "launcher.cfg"); java.util.Properties g = Launcher.load(cf);
            String want = Integer.toString(v.javaMajor);
            if (!want.equals(g.getProperty("java"))) { g.setProperty("java", want); Launcher.save(cf, g); }
        } catch (Throwable t) { Hello.log("java: could not record launcher.cfg java=" + v.javaMajor + ": " + t); }
        String linked = "," + System.getProperty("ps5.jdks", "");   // e.g. ",8,17,21,25,": the Javas this title carries
        int needTier = 0, newest = 0;
        for (int t : new int[] { 8, 17, 21, 25 }) if (linked.contains("," + t + ",")) { newest = t; if (needTier == 0 && t >= v.javaMajor) needTier = t; }   // smallest linked Java that is at least what the version needs (8 only when a Java 8 is linked)
        if (needTier == 0) needTier = newest != 0 ? newest : (v.javaMajor <= 17 ? 17 : v.javaMajor <= 21 ? 21 : 25);
        int forced = 0; try { forced = Integer.parseInt(setting); } catch (NumberFormatException e) { }
        if (forced != 0 && !("," + System.getProperty("ps5.jdks", "")).contains("," + forced + ",")) forced = 0;
        int tier = forced >= needTier ? forced : needTier;   // the setting cannot go below what the version needs
        if (have >= needTier) {   // this run's Java can play it: never close the app just for the setting, it applies from the next start
            if (tier != have) Hello.log("java: the setting wants Java " + tier + ", this run has Java " + have + " (enough for " + v.id + "): playing on it, the setting applies when the app is opened again");
            return null;
        }
        Hello.log("java: " + v.id + " (needs Java " + v.javaMajor + ") wants Java " + tier + ", this run has Java " + have + ": ending the app, the next start boots Java " + tier);
        if (restartLoop("Java " + tier + " for " + v.id)) return "restart loop (Java)";
        if (jvmArgsId(v).isEmpty() && (v.javaMajor > 21 || tier > 21)) { try { if (new File(DIR, ".kit/jvmargs.txt").delete()) Hello.log("java: Forge's module path (.kit/jvmargs.txt) removed: " + v.id + " starts in a plain Java " + tier + " VM"); } catch (Throwable t) { } }
        markAutoplay("Java " + tier + " for " + v.id);
        restartNotice("Restarting the app for Java " + tier, "Reopen ProsperoCraft from the home screen", v.id + " needs Java " + tier + (autoplayOn() ? " and starts by itself when the app is open again." : ". Open the app again and press Play."));
        try { Hello.quit("Switched to Java " + tier + ": press Play again"); } catch (Throwable t) { Hello.log("java: quit unavailable: " + t); }
        return "needs Java " + tier;
    }
    /** MC 26 starts a daemon "Client shutdown watchdog" when main() ends and, if the JVM is still alive 15 s later, writes a crash report ("Game crashed!", crash-reports/crash-*-client.txt). Here main() returning just means
     *  back to the launcher, so the watchdog is interrupted (it ends on InterruptedException; a version without that thread is unaffected). Scans for 20 s because the thread may start a moment after main() returns. */
    static void stopShutdownWatchdog() {
        Thread t = new Thread(() -> {
            int hit = 0;
            for (int i = 0; i < 40; i++) {
                try { for (Thread x : Thread.getAllStackTraces().keySet()) if (x.getName().startsWith("Client shutdown watchdog") && !x.isInterrupted()) { x.interrupt(); hit++; Hello.log("McBoot: interrupted '" + x.getName() + "' (the game is not being shut down by the JVM here)"); } } catch (Throwable e) { }
                try { Thread.sleep(500); } catch (InterruptedException e) { return; }
            }
        }, "ps5-watchdog-stop");
        t.setDaemon(true); t.start();
    }
    static volatile String lastGdir, lastShown; static volatile long gameStartMs; static volatile int firstExit = -1;   // status of the first System.exit of this game (0 = the player quit: Fabric then wraps the intercepted exit and calls exit(1) a moment later, which is no failure)
    /** The game said "Crash report saved to ..." but the UDP log only carries filtered lines: print the head of the newest crash report written since this round started (description, exception, first frames). */
    /** a crash report newer than this game's start exists in its game folder */
    static boolean crashedThisRun() {
        try {
            String g = lastGdir; if (g == null) return false;
            java.io.File[] fs = new java.io.File(g, "crash-reports").listFiles((d, n) -> n.endsWith(".txt"));
            if (fs != null) for (java.io.File f : fs) if (f.lastModified() >= gameStartMs - 2000) return true;
        } catch (Throwable t) { }
        return false;
    }
    static void showLastCrashReport() {
        try {
            String g = lastGdir; if (g == null) return;
            java.io.File[] fs = new java.io.File(g, "crash-reports").listFiles((d, n) -> n.endsWith(".txt"));
            if (fs == null) return;
            java.io.File best = null; for (java.io.File f : fs) if (f.lastModified() >= gameStartMs - 2000 && (best == null || f.lastModified() > best.lastModified())) best = f;
            if (best == null || best.getName().equals(lastShown)) return;
            lastShown = best.getName();
            java.util.List<String> ls = java.nio.file.Files.readAllLines(best.toPath(), java.nio.charset.StandardCharsets.UTF_8);
            try { java.util.List<String> aside = McMods.blameCrash(g, String.join("\n", ls)); if (!aside.isEmpty()) Hello.log("crash-report: mods set aside because of this crash: " + aside + " (start again; put them back from mods-disabled once updated)"); } catch (Throwable t) { Hello.log("crash-report: blame: " + t); }
            Hello.log("crash-report: " + best.getName() + " (" + ls.size() + " lines), head:");
            int n = 0, max = 45;
            for (String l : ls) { if (l.trim().isEmpty()) continue; Hello.log("crash-report: " + (l.length() > 220 ? l.substring(0, 220) : l)); if (++n >= max) break; if (n % 8 == 0) try { Thread.sleep(25); } catch (InterruptedException e) { } }
        } catch (Throwable t) { Hello.log("crash-report: unreadable: " + t); }
    }
    static String playOnce() {
        Hello.soundDownAt = 0; started = false;
        String cwd0 = null;
        try { Hello.glCompat(0); } catch (Throwable t) { }   // the launcher draws in the core profile; chooseGl() below switches the game's fixed-function mode on again
        try { Launcher.runQueue(DIR); } catch (Throwable t) { Hello.log("launcher: queue crashed: " + t); }   // installs / configures what <gameDir>/download.txt asks for, before the version is picked
        if ("1".equals(System.getProperty("ps5.noui"))) Hello.log("launcher ui: none in this title (the Vulkan title starts the instance in launcher.cfg directly)");   // -Dps5.noui=1: make_mcvk_title.sh builds
        else if (takeAutoplay()) { try { Kit.resume(DIR); } catch (Throwable t) { Hello.log("launcher ui: loading screen for the resumed Play failed: " + t); } }   // no launcher screen, but the loading screen (Kit.resume)
        else try { Kit.show(DIR); } catch (Throwable t) { Hello.log("launcher ui: crashed: " + t); }   // the native launcher (launcher.cfg ui=off skips it); it sets launcher.cfg instance=
        Launcher.Instance inst = Launcher.current(DIR);
        String gdir = Launcher.gameDirOf(inst, DIR); curGdir = gdir;
        Hello.log("launcher: instance " + inst.id + ", game folder " + gdir);
        try { Launcher.optionsSwap(gdir, Launcher.baseVersion(DIR, inst)); } catch (Throwable t) { Hello.log("options swap: " + t); }
        Launcher.applyOptions(inst, gdir); Launcher.applyProps(inst);
        if (System.getProperty("ps5.vulkan") != null) try { for (String l : java.nio.file.Files.readAllLines(new File(gdir, "options.txt").toPath(), StandardCharsets.UTF_8)) if (l.matches("(?i).*(api|graphic|backend|clean|start|vulkan|opengl|gpu|fps|sync|frame).*")) Hello.log("options.txt[vk]: " + l); } catch (Throwable t) { Hello.log("options.txt[vk]: " + t); }   // the Graphics API option's key / value are still unknown
        lastGdir = gdir;
        Version v = null;
        try { v = pickVersion(DIR, inst.version(), inst.customJar()); } catch (Throwable t) { Hello.log("version: loader failed, using the built-in version: " + t); }
        try { trySwitchToVulkan(v, gdir, inst); } catch (Throwable t) { Hello.log("renderer: hand-over failed: " + t); }
        applyAuthHosts();   // a sign-in / sign-out done in the launcher since the app started takes effect now
        McAuth.Session sess = Launcher.session(inst, DIR);
        String[] a = v != null ? Launcher.launchArgs(inst, v.id, v.assetIndex, v.type, sess, DIR, gdir)
                               : Launcher.launchArgs(inst, "1.20.1", "5", "release", sess, DIR, gdir);
        if (v != null && !v.gameArgs.isEmpty()) { String[] b = java.util.Arrays.copyOf(a, a.length + v.gameArgs.size()); for (int i = 0; i < v.gameArgs.size(); i++) b[a.length + i] = v.gameArgs.get(i); a = b; }
        if (v != null) { try { legacyAssets(v.assetIndex, DIR, gdir); } catch (Throwable t) { Hello.log("assets: legacy copy failed: " + t); } }
        if (v != null && v.modular) { try { { String fl = v.id.toLowerCase().contains("neoforge") ? "neoforge" : "forge"; McMods.fitMods(gdir, Launcher.baseVersion(DIR, inst), fl); java.util.List<String> got = McMods.fixDeps(gdir, Launcher.baseVersion(DIR, inst), fl); if (!got.isEmpty()) Hello.log("mods: installed what the Forge mods need: " + got); McMods.listMods(gdir); } } catch (Throwable t) { Hello.log("mods: fit failed: " + t); } }   // Fabric/Quilt jars stop Forge ("needs language provider javafml"): set them aside, they come back for a Fabric run
        if (v != null && v.derived && !v.modular) { try { McMods.fitMods(gdir, Launcher.baseVersion(DIR, inst), v.id.startsWith("quilt-loader-") ? "quilt" : "fabric"); McMods.fixDeps(gdir, Launcher.baseVersion(DIR, inst), v.id.startsWith("quilt-loader-") ? "quilt" : "fabric"); McMods.listMods(gdir); } catch (Throwable t) { Hello.log("mods: fit failed: " + t); } fabricProps(v); }   // set aside mods built for another Minecraft version
        if (v != null) { String need = needsOtherJava(v, inst); if (need != null) return need; }
        if (v != null) { String need = needsOtherJvmArgs(v); if (need != null) return need; }
        if (v != null) primeForgeVm(v);
        { String need = needsOtherRenderer(v, gdir); if (need != null) return need; }
        if (v != null) { String need = lwjglGenLock(v); if (need != null) return need; }   // one LWJGL 3 generation per process (see lwjglGenLock)   // this title embeds one Java; a version that needs a newer one is started from the title that has it
        chooseGl(v);
        try {
            Class<?> main;
            if (v != null) {
                // LWJGL 2 / 3.4 versions carry their own LWJGL natives, which cannot be loaded by a second loader: those reuse the loader of an earlier round. Every other version gets a FRESH loader each round (LWJGL 3 lives in the
                // title's own loader): Minecraft keeps state in statics (1.16.5: RenderSystem "Could not initialize render thread" on the second start) and the old game becomes garbage.
                boolean reuse = v.lwjgl2;   // LWJGL 3.4 (26.x) no longer reuses the whole game loader: its statics (Util's executors are shut down when the game stops, ...) made the SECOND 26.x start crash; only the LWJGL jars stay in one shared loader
                java.net.URLClassLoader cl = reuse ? LOADERS.get(v.id) : null;
                boolean fresh = cl == null;
                if (fresh) {
                    if (v.lwjgl34 && v.nLwjgl > 0 && v.nLwjgl < v.cp.size()) {   // shared loader for the LWJGL 3.4 jars (natives load once), a new loader for the game itself every round
                        if (LWJGL34 == null) LWJGL34 = new ChildFirst(v.cp.subList(0, v.nLwjgl).toArray(new java.net.URL[0]), McBoot.class.getClassLoader(), true, "LWJGL 3.4");
                        cl = new java.net.URLClassLoader(v.cp.subList(v.nLwjgl, v.cp.size()).toArray(new java.net.URL[0]), LWJGL34);
                        Hello.log("lwjgl: game loader " + (++game34) + " on the shared LWJGL 3.4 loader");
                    } else if (v.lwjgl2 && v.cp.size() > 1) {   // LWJGL 2: the port jar (cp[0]) and its native library "lwjgl2ps5" belong to ONE loader for the whole process (a second loader for another version: "Native Library lwjgl2ps5 already loaded in another classloader", 1.8.9 -> 1.12.2); every version keeps its own game loader on top
                        if (LWJGL2L == null) LWJGL2L = new ChildFirst(v.cp.subList(0, 1).toArray(new java.net.URL[0]), McBoot.class.getClassLoader());
                        cl = new java.net.URLClassLoader(v.cp.subList(1, v.cp.size()).toArray(new java.net.URL[0]), LWJGL2L);
                        Hello.log("lwjgl: game loader for " + v.id + " on the shared LWJGL 2 loader");
                    } else if (v.modular) {   // Forge: BootstrapLauncher is on the module path of this VM (app loader); an empty loader on top of it is the context loader its module layer hangs from
                        cl = new java.net.URLClassLoader(new java.net.URL[0], McBoot.class.getClassLoader());
                        fmlNoEarlyWindow(gdir);
                        openLwjglModules();
                        ensureUnionUrl();
                        System.setProperty("legacyClassPath", v.legacyCp);   // what BootstrapLauncher turns into the game's modules (modlauncher, fml, the libraries)
                        if (new File(DIR, ".kit/bsl_debug").exists()) System.setProperty("bsl.debug", "1"); else System.clearProperty("bsl.debug");   // touch .kit/bsl_debug: BootstrapLauncher lists every path and package it handles
                        Hello.log("forge: modular launch, game classes come from BootstrapLauncher's own layer, legacyClassPath = " + v.legacyCp.split(":").length + " jars");
                    } else if (!reuse && flavoured()) {   // shared VM: the game's ASM wins over the module path's
                        cl = new PrefixFirst(v.cp.toArray(new java.net.URL[0]), McBoot.class.getClassLoader(), "org.objectweb.asm.");
                    } else cl = reuse ? new ChildFirst(v.cp.toArray(new java.net.URL[0]), McBoot.class.getClassLoader()) : new java.net.URLClassLoader(v.cp.toArray(new java.net.URL[0]), McBoot.class.getClassLoader());
                    if (reuse) LOADERS.put(v.id, cl);
                }
                Thread.currentThread().setContextClassLoader(cl);
                if (!reuse) lastGameCl = cl;
                // JNA (Fabric mods: oshi, Legacy4J's libsdl4j) skips System.loadLibrary("jnidispatch") unless jna.nosys=false, and then tries to System.load a jar-extracted .so, which this title cannot do:
                // with the property off it finds the built-in stub dispatch (ps5lwjgl.c, JNI_OnLoad_jnidispatch) like it finds LWJGL.
                if (System.getProperty("jna.nosys") == null) System.setProperty("jna.nosys", "false");
                // a builtin native library can be loaded by one class loader at a time, and the previous game's loader (Knot) is still alive: every launch asks for its own name (ps5lwjgl.c has JNI_OnLoad_jnidispatch2..16,
                // all answered by the same stub functions)
                { int n = ++jnaLaunch; System.setProperty("jna.boot.library.name", n == 1 ? "jnidispatch" : "jnidispatch" + (((n - 2) % 15) + 2)); }
                try {   // probe: is java.compiler (javax.annotation.processing.Messager, which Mixin's logging needs) visible from the game's loader?
                    Class<?> m = Class.forName("javax.annotation.processing.Messager", false, cl);
                    Hello.log("probe: Messager visible from the game loader (loader " + m.getClassLoader() + ", thread " + Thread.currentThread().getName() + ")");
                } catch (Throwable t) {
                    Hello.log("probe: Messager NOT visible from the game loader: " + t + " | parent " + cl.getParent() + " | thread " + Thread.currentThread().getName());
                }
                if (v.lwjgl2 && (fresh || LWJGL2L != null)) {   // load LWJGL 2's native library (System.loadLibrary("lwjgl2ps5")) from THIS loader (idempotent: the shared loader initialises org.lwjgl.Sys once): HotSpot looks a native method up in the libraries of the loader that defined its class
                    try { Class.forName("org.lwjgl.Sys", true, cl); Hello.log("lwjgl: LWJGL 2 initialised (" + System.getProperty("ps5.lwjgl2.status", "native library loaded") + ")"); }
                    catch (Throwable t) { Hello.log("lwjgl: LWJGL 2 failed to initialise: " + t); t.printStackTrace(); }
                }
                if (!v.modular) registerFsProviders(v.cp, cl);
                if (!v.modular && flavoured()) { sharedRun = true; openLwjglModules(); Hello.log("vm: " + v.id + " runs in the VM that was started with Forge's module path (shared VM, no restart; ASM comes from the game's own jars)"); } else sharedRun = false;
                main = Class.forName(v.mainClass, true, cl);
                Hello.log("McBoot: launching " + v.id + " (" + v.mainClass + ") from " + v.cp.size() + " jars");
            } else {
                try { main = Class.forName("net.minecraft.client.main.Main"); }
                catch (ClassNotFoundException e) {   // "launcher" build: no version is baked into the title
                    Hello.log("version: nothing is installed and this title has no built-in version. Create " + DIR + "/download.txt with a version id per line (e.g. 1.20.1 select), restart the title.");
                    return "no version installed";
                }
            }
            // Quitting: the VM's own exit path cannot end this title (the console refuses the exit calls), so the shutdown hook asks the host to bring the launcher back, and parks
            // so the VM does not go on tearing itself down underneath it.
            if (!hooked) { hooked = true; Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                leave("exit");
                try { Thread.sleep(Long.MAX_VALUE); } catch (InterruptedException e) { }
            }, "ps5-exit")); }
            started = true; ENDED.set(false); Hello.soundDownAt = 0; gameDone = false; gameStartMs = System.currentTimeMillis(); firstExit = -1;
            for (String pk : new String[] { "mixin.bootstrapService", "mixin.service", "mixin.env.remapRefMap" }) System.clearProperty(pk);   // Fabric Loader sets these for good; the next Quilt / Forge game in the same VM then asks for Fabric's classes (ClassNotFoundException MixinServiceKnotBootstrap, log c05d54c8)
            memLog("game starts");
            try { cwd0 = System.getProperty("user.dir"); int rc = Hello.chdir(gdir); if (rc != 0) rc = softChdir(gdir) ? 0 : rc; Hello.log("cwd: " + gdir + (rc == 0 ? "" : " failed, errno " + rc) + " (mods write ./config/... relative to it)"); } catch (Throwable t) { Hello.log("cwd: not changed (" + t + ")"); }
            main.getMethod("main", String[].class).invoke(null, (Object) a);
            Hello.log("McBoot: Main.main returned");
            // Minecraft 1.0 - 1.5: main() only starts the thread "Minecraft main thread" and returns; the game lives on that thread until it calls System.exit (the exit guard ends the round then).
            // Treating the return as the end of the game brought the launcher back on top of the running game (it took the GL context and the pad) and a second start died with "Only one LWJGL context".
            for (int i = 0; i < 10; i++) {   // the thread is started inside main(), but give it a moment
                Thread g = null;
                for (Thread t : Thread.getAllStackTraces().keySet()) if ("Minecraft main thread".equals(t.getName()) && t.isAlive()) g = t;
                if (g == null) { if (i == 0) break; else { try { Thread.sleep(200); } catch (InterruptedException e) { } continue; } }
                Hello.log("McBoot: main() returned but '" + g.getName() + "' runs the game: waiting for it");
                while (g.isAlive() && !ENDED.get()) { try { g.join(500); } catch (InterruptedException e) { break; } }
                break;
            }
            stopShutdownWatchdog();
            return "main returned";
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable c = e.getCause();
            if (c instanceof ExitGuard) { Hello.log("McBoot: the game called System.exit"); if (((ExitGuard) c).status != 0) noteSharedFailure("System.exit(" + ((ExitGuard) c).status + ")"); return "exit"; }
            Hello.log("McBoot: MC threw " + c);
            noteSharedFailure(String.valueOf(c));
            if (c != null && c.getClass().getName().equals("java.lang.module.FindException")) {   // "Module java.instrument not found, required by org.spongepowered.mixin": a JDK module this runtime image lacks
                java.util.regex.Matcher mm = java.util.regex.Pattern.compile("Module ([A-Za-z0-9_.]+) not found, required by ([A-Za-z0-9_.]+)").matcher(String.valueOf(c.getMessage()));
                if (mm.find() && patchRequires(mm.group(1), mm.group(2))) { markAutoplay("module " + mm.group(1)); Hello.log("forge: the next Play creates the VM with the patched module path (" + mm.group(2) + " no longer insists on " + mm.group(1) + ")"); }
            }
            if (c instanceof LinkageError) {   // 26.1 (log 3981c9d6): "loader 'app' wants to load org.lwjgl.PointerBuffer" from Window.getPlatform: which loader defined what, and does the app class path see the game's classes?
                try {
                    ClassLoader gl = Thread.currentThread().getContextClassLoader(), sys = ClassLoader.getSystemClassLoader();
                    for (String n : new String[] { "com.mojang.blaze3d.platform.Window", "net.minecraft.client.main.Main", "org.lwjgl.PointerBuffer", "org.lwjgl.glfw.GLFW", "org.lwjgl.system.MemoryStack", "org.lwjgl.util.tinyfd.TinyFileDialogs" }) {
                        String via = "?", own = "?", app = "?";
                        try { via = String.valueOf(Class.forName(n, false, gl).getClassLoader()); } catch (Throwable t) { via = t.toString(); }
                        try { own = LWJGL34 == null ? "no 3.4 loader" : String.valueOf(Class.forName(n, false, LWJGL34).getClassLoader()); } catch (Throwable t) { own = t.toString(); }
                        try { app = String.valueOf(sys.getResource(n.replace('.', '/') + ".class")); } catch (Throwable t) { app = t.toString(); }
                        Hello.log("loaders: " + n + " | game loader -> " + via + " | 3.4 loader -> " + own + " | app class path: " + app);
                    }
                    Hello.log("loaders: org.lwjgl packages the 3.4 loader did not have: " + (LWJGL34 == null ? "(no 3.4 loader)" : LWJGL34.missed()));
                    Hello.log("loaders: java.class.path = " + System.getProperty("java.class.path"));
                } catch (Throwable t) { Hello.log("loaders: probe failed " + t); }
            }
            c.printStackTrace();
            return "crash java";
        } catch (Throwable t) {
            Hello.log("McBoot: launch failed " + t);
            noteSharedFailure(String.valueOf(t));
            t.printStackTrace();
            return "launch failed";
        } finally {
            if (cwd0 != null) try { Hello.chdir(cwd0); } catch (Throwable t) { }   // the launcher and the native UI use paths relative to the start-up folder
            // this thread owned the game's GL context (LWJGL 3 never lets go of it, LWJGL 2 only in Display.destroy): hand it back so the launcher of the next round can use it
            try { Hello.glCompat(-1); } catch (Throwable t) { }
            gameDone = true;
            try { Thread.currentThread().setContextClassLoader(McBoot.class.getClassLoader()); } catch (Throwable t) { }   // this thread runs the launcher (and the next game) next: it must not keep this game's loader
            if (started) { memLog("game over"); try { releaseGame(lastGameCl); } catch (Throwable t) { Hello.log("leak: release failed " + t); } leakProbe(lastGameCl); }
        }
    }
    /** Right after the game: nothing of it may keep the heap full. Log 458247f5 / a23b7c3c: the heap stayed at 2.1 of 3 GB after a Forge / 103-mod Fabric game (nothing ran a full collection, the young collections of the launcher
     *  then crawled: the launcher drew 2 frames a second and its library / content lists did not load). Threads that still point at the game's loader are re-pointed at the title's loader and interrupted, then one full collection. */
    static void releaseGame(ClassLoader gone) {
        long t0 = System.currentTimeMillis(); int n = 0;
        if (gone != null) {
            Thread me = Thread.currentThread();
            for (Thread th : Thread.getAllStackTraces().keySet()) {
                if (th == me || th == relaunchThread) continue;
                boolean hit = false; for (ClassLoader q = th.getContextClassLoader(); q != null; q = q.getParent()) if (q == gone) hit = true;
                if (!hit) continue;
                n++; try { th.setContextClassLoader(McBoot.class.getClassLoader()); } catch (Throwable x) { } th.interrupt();
            }
            try { Thread.sleep(250); } catch (InterruptedException e) { }
        }
        System.gc();
        Hello.log("leak: " + n + " threads let go of the game's loader, full collection took " + (System.currentTimeMillis() - t0) + " ms");
        memLog("game over, after full GC");
    }
    static volatile ClassLoader lastGameCl;
    /** Each 26.x round used to leave ~20k classes / 75 MB metaspace behind: the old game's loader stays alive while any thread of it runs or has it as context loader. A few seconds after the game ended, list the threads that still
     *  point at the loader (name, daemon, state, first frames), re-point their context loader at the title's loader and interrupt them (idle executor workers exit on interrupt only if their pool is shut down, but waiters/sleepers end). */
    static void leakProbe(final ClassLoader gone) {
        if (gone == null) return;
        Thread t = new Thread(() -> {
            try { Thread.sleep(4000); } catch (InterruptedException e) { }
            try {
                int n = 0; Thread me = Thread.currentThread();
                for (java.util.Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
                    Thread th = e.getKey(); if (th == me || th == relaunchThread) continue;
                    ClassLoader c = th.getContextClassLoader(); boolean hit = false;
                    for (ClassLoader q = c; q != null; q = q.getParent()) if (q == gone) hit = true;
                    if (!hit) continue;
                    n++;
                    if (n <= 12) Hello.log("leak: thread '" + th.getName() + "' " + (th.isDaemon() ? "daemon " : "") + th.getState() + " still holds the old game loader, at " + (e.getValue().length > 0 ? e.getValue()[0] : "?"));
                    try { th.setContextClassLoader(McBoot.class.getClassLoader()); } catch (Throwable x) { }
                    th.interrupt();
                }
                Hello.log("leak: " + n + " threads referenced the finished game's loader (interrupted)");
                System.gc(); memLog("game over +4s");
            } catch (Throwable x) { Hello.log("leak: probe failed " + x); }
        }, "ps5-leakprobe");
        t.setDaemon(true); t.start();
    }
}
