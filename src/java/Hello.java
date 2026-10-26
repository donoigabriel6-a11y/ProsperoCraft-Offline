public class Hello {
    static native void log0(String s);
    static volatile long soundDownAt;   // set by log() when Minecraft logs its quit line (read by McBoot's quit watcher)
    static volatile long stopAt;   // set by log() when the integrated server starts stopping (world save on exit); cleared when it says it is done (read by McBoot's watcher: a save that never ends)
    static volatile String markPath;   // <gameDir>/.kit/exitmark.txt, set by McBoot; null until then
    /** Synced append to a file: survives a process that dies a moment later (UDP lines of that moment do not). The next start prints the file again. */
    static synchronized void mark(String m) {
        String p = markPath; if (p == null) return;
        try (java.io.FileOutputStream f = new java.io.FileOutputStream(p, true)) {
            f.write((String.format("%tT.%<tL ", System.currentTimeMillis()) + m + "\n").getBytes("UTF-8")); f.getFD().sync();
        } catch (Throwable t) { }
    }
    // Java 8 compatible helpers (the launcher side is compiled with --release 8 so that it also runs on the Java 8 VM)
    static byte[] readAll(java.io.InputStream in) throws java.io.IOException { java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); byte[] b = new byte[8192]; int n; while ((n = in.read(b)) > 0) o.write(b, 0, n); return o.toByteArray(); }
    static int javaMajor() { String s = System.getProperty("java.specification.version", "17"); if (s.startsWith("1.")) s = s.substring(2); try { return Integer.parseInt(s); } catch (NumberFormatException e) { return 17; } }
    static volatile java.util.function.Consumer<String> logHook;   // set by Kit while the loading screen is up (no compile-time reference: Hello.java is compiled on its own, before the lwjgl jars exist)
    static void log(String s) {   // every log line passes here: the game's own quit line starts the quit watcher in McBoot
        log0(s);
        java.util.function.Consumer<String> h = logHook; if (h != null) h.accept(s);
        if (s != null && s.contains("SoundSystem shutting down")) { soundDownAt = System.currentTimeMillis(); mark("quit line seen"); }
        else if (s != null && s.contains("Starting up SoundSystem")) soundDownAt = 0;
        if (s != null) { if (s.contains("Stopping singleplayer server") || s.contains("]: Stopping server")) stopAt = System.currentTimeMillis(); else if (s.contains("All dimensions are saved") || s.contains("Stopping!") || s.contains("Starting up SoundSystem")) stopAt = 0; }
    }
    static native int chdir(String path);   // the process working directory (0 or errno): mods write "./config/..." relative to it (Lithium, Sodium, ModernFix); McBoot sets it to the game folder while a game runs and back afterwards; absent in older builds
    static native void glCompat(int level);   // 0 = core profile, 21 = fixed-function OpenGL 2.1 through gl4es (Minecraft 1.16.5 and older); set by McBoot before the game loads LWJGL
    static native int exitStep(int step, String why);   // 0 = prepare (returns a mask), 1 LoadExec, 2 LaunchApp, 3 KillApp, 4 GoHome, 5 _exit: one step per call so every step is logged from Java
    static native void quit(String why);   // the game is ending (System.exit, crash, main returned): the host brings the launcher back from a healthy thread; absent in older builds
    static native int crashFlag();   // 1 once the host saw a native SIGSEGV/SIGBUS and wants the Java stacks (McBoot's ps5-crashwatch thread polls it); absent in older builds
    static native void crashAck();   // the dump is printed
    static native void rmprobe();   // host-side unlink/rmdir/rename probe, registered only in mc builds

    static void step(String name, Runnable r) {
        try { r.run(); } catch (Throwable t) { log("FAIL " + name + ": " + t); }
    }

    // Memory stress for the MC-sized heap: held heap, GC with a big old gen, direct buffers (LWJGL/NIO use the SDK malloc heap).
    static void mem(java.util.function.Consumer<String> out) {
        Runtime rt = Runtime.getRuntime();
        long max = rt.maxMemory(), T0 = System.nanoTime();
        out.accept("mem: max=" + max / 1048576 + "M total=" + rt.totalMemory() / 1048576 + "M");
        java.util.ArrayList<byte[]> hold = new java.util.ArrayList<>();
        long got = 0;
        try {
            while (got < max / 10 * 8) {
                byte[] b = new byte[32 << 20];
                java.util.Arrays.fill(b, (byte) 1);   // touch every page so it is really committed
                hold.add(b); got += b.length;
                if (got % (256L << 20) == 0) out.accept("mem: heap held " + got / 1048576 + "M at " + (System.nanoTime() - T0) / 1_000_000 + " ms");
            }
            out.accept("mem: heap hold ok " + got / 1048576 + "M, total=" + rt.totalMemory() / 1048576 + "M");
        } catch (OutOfMemoryError e) { out.accept("mem: heap OOM at " + got / 1048576 + "M"); }
        // keep ~half live, then churn small garbage: young GCs with a large old gen
        hold.subList(hold.size() / 2, hold.size()).clear();
        long c0 = System.nanoTime(); Object sink = null; int i = 0;
        for (; i < 1_000_000 && System.nanoTime() - c0 < 20_000_000_000L; i++) sink = new byte[1024];
        out.accept("mem: churn " + i / 1024 + "M garbage in " + (System.nanoTime() - c0) / 1_000_000 + " ms (live " + hold.size() * 32 + "M)" + (sink == null ? "" : ""));
        try {
            for (java.lang.management.GarbageCollectorMXBean g : java.lang.management.ManagementFactory.getGarbageCollectorMXBeans())
                out.accept("mem: gc " + g.getName() + " count=" + g.getCollectionCount() + " time=" + g.getCollectionTime() + " ms");
        } catch (Throwable t) { out.accept("mem: gc stats unavailable: " + t); }
        hold.clear();
        long g0 = System.nanoTime(); System.gc();
        out.accept("mem: full gc after release " + (System.nanoTime() - g0) / 1_000_000 + " ms, free=" + rt.freeMemory() / 1048576 + "M");
        java.util.ArrayList<java.nio.ByteBuffer> bufs = new java.util.ArrayList<>();
        long d = 0;
        try {
            while (d < (768L << 20)) {
                bufs.add(java.nio.ByteBuffer.allocateDirect(16 << 20)); d += 16 << 20;   // zero-filled, so pages are touched
                if (d % (128L << 20) == 0) out.accept("mem: direct " + d / 1048576 + "M");
            }
            out.accept("mem: direct ok " + d / 1048576 + "M");
        } catch (Throwable t) { out.accept("mem: direct failed at " + d / 1048576 + "M: " + t); }
        bufs.clear(); System.gc();
        out.accept("mem: done in " + (System.nanoTime() - T0) / 1_000_000 + " ms");
    }

    public static void main(String[] args) {
        try { new java.io.File("/app0/mc/.kit/vmboot").delete(); } catch (Throwable t) { }   // boot guard (jvm_host.cpp): Java is running, the VM start worked
        log("hello from java " + System.getProperty("java.version") + " (" + System.getProperty("java.vm.name") + ") on "
                + System.getProperty("os.name") + "/" + System.getProperty("os.arch"));
        step("props", () -> log("java.home=" + System.getProperty("java.home") + " user.dir=" + System.getProperty("user.dir")
                + " file.encoding=" + System.getProperty("file.encoding") + " jnu=" + System.getProperty("sun.jnu.encoding")
                + " cpus=" + Runtime.getRuntime().availableProcessors() + " maxMem=" + Runtime.getRuntime().maxMemory() / 1048576 + "M"));
        step("loop", () -> {
            long t0 = System.nanoTime(), sum = 0;
            for (int i = 0; i < 20_000_000; i++) sum += i ^ (i >> 3);
            log("loop sum=" + sum + " in " + (System.nanoTime() - t0) / 1_000_000 + " ms");
        });
        step("math", () -> log("math sin=" + Math.sin(1.0) + " pow=" + Math.pow(2, 0.5) + " sqrt=" + Math.sqrt(2)));
        step("thread", () -> {
            Thread th = new Thread(() -> log("thread ok: " + Thread.currentThread().getName()), "probe-thread");
            th.start();
            try { th.join(); } catch (InterruptedException e) { }
        });
        step("heap", () -> {
            int[] big = new int[16 << 20];
            big[big.length - 1] = 7;
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 100000; i++) sb.append(i);
            System.gc();
            log("heap ok chars=" + sb.length() + " big=" + big[big.length - 1] + " free=" + Runtime.getRuntime().freeMemory() / 1024 + "K");
        });
        step("file", () -> {
            try (java.io.FileInputStream in = new java.io.FileInputStream("/app0/assets/hello/Hello.class")) {
                byte[] b = readAll(in);
                log("file read " + b.length + " bytes, magic " + Integer.toHexString(((b[0] & 255) << 8) | (b[1] & 255)));
            } catch (java.io.IOException e) { log("FAIL file: " + e); }
        });
        step("nio", () -> {
            try { log("nio size=" + java.nio.file.Files.size(java.nio.file.Paths.get("/app0/assets/hello/Hello.class"))); }
            catch (java.io.IOException e) { log("FAIL nio: " + e); }
        });
        step("format", () -> log(String.format("format %05d %.3f %s", 42, Math.PI, "ok")));
        step("mem", () -> {
            // The stress test touches 80% of the heap and 768 MB of direct memory (~2.2 s at every start, and a heap that is already grown to its maximum before the game loads). The game builds skip it: touch /app0/mc/.kit/memtest to run it anyway.
            String m = "?"; try { m = new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("/app0/assets/hello/mode.txt")), "UTF-8").trim(); } catch (Throwable t) { }
            if (m.equals("mc") && !new java.io.File("/app0/mc/.kit/memtest").exists()) log("mem: stress test skipped in the game build (touch /app0/mc/.kit/memtest to run it)");
            else mem(Hello::log);
        });
        step("gl", () -> {
            // Which program to run is written into mode.txt by every build (the console keeps stale files from earlier deploys, but mode.txt is always overwritten).
            String mode = "?";
            try { mode = new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("/app0/assets/hello/mode.txt")), "UTF-8").trim(); } catch (Throwable t) { log("mode.txt unreadable: " + t); }
            boolean mc = mode.equals("mc") || (mode.equals("?") && Boolean.getBoolean("ps5.mc"));
            String cls = mc ? "McBoot" : mode.equals("vk") ? "VkTest" : "GLTest";
            log("mode: " + mode + " ps5.mc=" + Boolean.getBoolean("ps5.mc") + " -> running " + cls);
            try { Class.forName(cls).getMethod("run").invoke(null); }
            catch (ClassNotFoundException e) { log("class " + cls + " not found (build without LWJGL" + (mc ? ", or McBoot missing from this build" : "") + ")"); }
            catch (java.lang.reflect.InvocationTargetException e) { log("FAIL " + cls + ": " + e.getCause()); }
            catch (ReflectiveOperationException e) { log("FAIL " + cls + ": " + e); }
        });
        log("done");
    }
}
