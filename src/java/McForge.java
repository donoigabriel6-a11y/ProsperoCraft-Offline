// SPDX-License-Identifier: GPL-3.0-or-later
import java.io.*;
import java.lang.reflect.InvocationTargetException;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Forge / NeoForge (Minecraft 1.17+, "modern") installer, the in-process equivalent of `java -jar forge-installer.jar --installClient`:
 *  installer jar -> version JSON + maven/ libraries + the client-side processors (they build the patched client jar) -> versions/<id>/<id>.json + <id>.installed.
 *  Launching is McBoot's job (the version JSON's JVM arguments go through .kit/jvmargs.txt, because --module-path / --add-opens can only be given when the VM is created). */
final class McForge {
    static void log(String s) { Hello.log("forge: " + s); }
    static String forgeMaven() { return System.getProperty("ps5.forge.maven", "https://maven.minecraftforge.net"); }
    static String forgePromos() { return System.getProperty("ps5.forge.promos", "https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json"); }
    static String neoMaven() { return System.getProperty("ps5.neoforge.maven", "https://maven.neoforged.net/releases"); }

    /** newest recommended (else latest) loader version for a Minecraft version */
    static String latest(String kind, String mc) throws IOException {
        if (kind.equals("neoforge")) {
            String[] p = mc.split("\\.");
            if (p.length < 2 || !p[0].equals("1")) throw new IOException("NeoForge: unexpected Minecraft version " + mc);
            if (mc.equals("1.20.1")) throw new IOException("NeoForge for 1.20.1 is a different artifact (net.neoforged:forge); use `forge 1.20.1` or NeoForge 1.20.2+");
            String prefix = p[1] + "." + (p.length > 2 ? p[2] : "0") + ".";
            String xml = new String(McDownload.get(neoMaven() + "/net/neoforged/neoforge/maven-metadata.xml"), StandardCharsets.UTF_8);
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("<version>([^<]+)</version>").matcher(xml);
            String best = null, bestAny = null;
            while (m.find()) { String v = m.group(1); if (!v.startsWith(prefix)) continue; bestAny = v; if (!v.contains("-")) best = v; }   // the list is oldest first
            if (best == null) best = bestAny;
            if (best == null) throw new IOException("NeoForge has no build for Minecraft " + mc);
            return best;
        }
        Map<String, Object> pr = McDownload.obj(McDownload.Json.parse(new String(McDownload.get(forgePromos()), StandardCharsets.UTF_8)));
        Map<String, Object> promos = pr != null ? McDownload.obj(pr.get("promos")) : null;
        String v = promos != null ? McDownload.str(promos.get(mc + "-recommended")) : null;
        if (v == null && promos != null) v = McDownload.str(promos.get(mc + "-latest"));
        if (v == null) throw new IOException("Forge has no build for Minecraft " + mc);
        return v;
    }

    static String artPath(String d) {   // group:artifact:version[:classifier][@ext]
        String ext = "jar"; int at = d.indexOf('@'); if (at >= 0) { ext = d.substring(at + 1); d = d.substring(0, at); }
        String[] p = d.split(":");
        return p[0].replace('.', '/') + "/" + p[1] + "/" + p[2] + "/" + p[1] + "-" + p[2] + (p.length > 3 ? "-" + p[3] : "") + "." + ext;
    }
    static byte[] read(ZipFile z, String name) throws IOException {
        ZipEntry e = z.getEntry(name); if (e == null) throw new IOException("the installer has no " + name);
        try (InputStream in = z.getInputStream(e)) { ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] x = new byte[16384]; int n; while ((n = in.read(x)) > 0) b.write(x, 0, n); return b.toByteArray(); }
    }
    static String sha1(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-1");
        try (InputStream in = new FileInputStream(f)) { byte[] x = new byte[65536]; int n; while ((n = in.read(x)) > 0) md.update(x, 0, n); }
        return McDownload.hex(md.digest());
    }

    /** libraries a download can fetch: an artifact with an empty url is produced by a processor (or sits in the installer's maven/ folder), not downloadable */
    static List<Object> downloadable(List<Object> libs) {
        List<Object> out = new ArrayList<Object>();
        if (libs == null) return out;
        for (Object o : libs) {
            Map<String, Object> lib = McDownload.obj(o); if (lib == null) continue;
            Map<String, Object> dl = McDownload.obj(lib.get("downloads")), art = dl != null ? McDownload.obj(dl.get("artifact")) : null;
            if (art != null) { String u = McDownload.str(art.get("url")); if (u == null || u.isEmpty()) continue; }
            out.add(o);
        }
        return out;
    }

    /** installs (or completes) a Forge / NeoForge client; returns the profile id to run */
    public static String install(String root, String kind, String mc, String fv) throws Exception {
        boolean neo = kind.equals("neoforge");
        if (McMods.runJava() < 17) throw new IOException(kind + " for Minecraft 1.17+ needs Java 17 or newer; this run has Java " + McMods.runJava() + " (set the Java setting to 17 or newer and restart)");
        if (fv == null || fv.isEmpty()) fv = latest(kind, mc);
        String coord = neo ? "net/neoforged/neoforge/" + fv + "/neoforge-" + fv : "net/minecraftforge/forge/" + mc + "-" + fv + "/forge-" + mc + "-" + fv;
        String url = (neo ? neoMaven() : forgeMaven()) + "/" + coord + "-installer.jar";
        File inst = new File(root, ".kit/forge/" + new File(coord).getName() + "-installer.jar");
        if (!inst.isFile() || inst.length() == 0) { log("downloading " + url); McDownload.fetch(new McDownload.Task(url, inst, null, 0, "installer")); }
        String id;
        try (ZipFile z = new ZipFile(inst)) {
            Map<String, Object> prof = McDownload.obj(McDownload.Json.parse(new String(read(z, "install_profile.json"), StandardCharsets.UTF_8)));
            String jn = McDownload.str(prof.get("json")); if (jn == null) throw new IOException("install_profile.json has no \"json\": this installer is too old (Forge before 1.13 is not handled here)");
            if (jn.startsWith("/")) jn = jn.substring(1);
            byte[] vb = read(z, jn);
            Map<String, Object> ver = McDownload.obj(McDownload.Json.parse(new String(vb, StandardCharsets.UTF_8)));
            id = McDownload.str(ver.get("id"));
            if (id == null || id.isEmpty() || id.contains("/") || id.contains("..")) throw new IOException("bad profile id " + id);
            String parent = McDownload.str(ver.get("inheritsFrom")); if (parent == null) parent = McDownload.str(prof.get("minecraft")); if (parent == null) parent = mc;
            log(kind + " " + fv + " -> profile " + id + " on Minecraft " + parent);
            McDownload.pLabel = "Minecraft " + parent; McDownload.pTotal = 0; McDownload.pDone = 0; McDownload.pBytesTotal = 0;
            McDownload.install(root, parent);
            File vf = McDownload.jsonFile(root, id); vf.getParentFile().mkdirs(); Files.write(vf.toPath(), vb);
            // the installer's own maven/ folder: the universal jar and friends, laid out like libraries/
            int ex = 0;
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement(); String n = e.getName();
                if (e.isDirectory() || !n.startsWith("maven/") || n.contains("..")) continue;
                File d = new File(root, "libraries/" + n.substring(6));
                if (d.isFile() && d.length() == e.getSize()) continue;
                d.getParentFile().mkdirs(); Files.write(d.toPath(), read(z, n)); ex++;
            }
            log(ex + " files taken from the installer's maven/ folder");
            List<Object> libs = new ArrayList<Object>();
            List<Object> pl = McDownload.arr(prof.get("libraries")), vl = McDownload.arr(ver.get("libraries"));
            if (pl != null) libs.addAll(pl); if (vl != null) libs.addAll(vl);
            List<McDownload.Task> tasks = McDownload.libraryTasks(root, downloadable(libs), new int[3]);
            if (McDownload.runAll(id + " libraries", tasks) > 0) throw new IOException(id + ": some libraries failed to download (run again to resume)");
            runProcessors(root, z, inst, prof, parent);
        }
        Files.write(new File(root, "versions/" + id + "/" + id + ".installed").toPath(), "ok\n".getBytes(StandardCharsets.UTF_8));
        log("installed " + id);
        return id;
    }

    static void runProcessors(String root, ZipFile z, File installer, Map<String, Object> prof, String mc) throws Exception {
        List<Object> procs = McDownload.arr(prof.get("processors"));
        if (procs == null || procs.isEmpty()) return;
        final Map<String, String> vars = new HashMap<String, String>();
        vars.put("SIDE", "client"); vars.put("MINECRAFT_JAR", new File(root, "versions/" + mc + "/" + mc + ".jar").getPath()); vars.put("MINECRAFT_VERSION", mc);
        vars.put("ROOT", new File(root).getPath()); vars.put("INSTALLER", installer.getPath()); vars.put("LIBRARY_DIR", new File(root, "libraries").getPath());
        File dataDir = new File(root, ".kit/forge/data");
        Map<String, Object> dm = McDownload.obj(prof.get("data"));
        if (dm != null) for (Map.Entry<String, Object> en : dm.entrySet()) {
            Map<String, Object> e = McDownload.obj(en.getValue()); String c = e != null ? McDownload.str(e.get("client")) : null;
            if (c != null) vars.put(en.getKey(), val(root, z, dataDir, c));
        }
        fsProbe(root);
        int todo = 0; for (Object o : procs) { Map<String, Object> p = McDownload.obj(o); if (p != null && forClient(p)) todo++; }
        int i = 0;
        for (Object o : procs) {
            Map<String, Object> p = McDownload.obj(o); if (p == null || !forClient(p)) continue;
            i++;
            McDownload.pLabel = "Forge: building the game (step " + i + " of " + todo + ")"; McDownload.pTotal = todo; McDownload.pDone = i - 1; McDownload.pBytesTotal = 0;
            List<String> args = new ArrayList<String>();
            List<Object> ra = McDownload.arr(p.get("args")); if (ra != null) for (Object a : ra) args.add(val(root, z, dataDir, subst(vars, McDownload.str(a))));
            Map<String, Object> outs = McDownload.obj(p.get("outputs"));
            if (outs != null && !outs.isEmpty()) {
                boolean ok = true;
                for (Map.Entry<String, Object> e : outs.entrySet()) {
                    File f = new File(val(root, z, dataDir, subst(vars, e.getKey()))); String want = val(root, z, dataDir, subst(vars, McDownload.str(e.getValue())));
                    if (!f.isFile() || !sha1(f).equalsIgnoreCase(want)) { ok = false; break; }
                }
                if (ok) { log("step " + i + "/" + todo + ": outputs already present"); continue; }
            }
            String jarName = McDownload.str(p.get("jar"));
            File jar = new File(root, "libraries/" + artPath(jarName));
            List<File> cp = new ArrayList<File>(); cp.add(jar);
            List<Object> pc = McDownload.arr(p.get("classpath")); if (pc != null) for (Object c : pc) cp.add(new File(root, "libraries/" + artPath(McDownload.str(c))));
            log("step " + i + "/" + todo + ": " + jarName);
            runMain(jar, cp, args, "Forge: step " + i + " of " + todo);
        }
        McDownload.pDone = todo;
    }
    /** how expensive are small unbuffered writes on this file system? (the tools write zip output through plain FileOutputStreams) */
    static void fsProbe(String root) {
        File f = new File(root, ".kit/forge/fsprobe.bin"); f.getParentFile().mkdirs();
        try {
            byte[] b = new byte[512]; long t0 = System.nanoTime();
            try (FileOutputStream o = new FileOutputStream(f)) { for (int i = 0; i < 2000; i++) o.write(b); }
            long small = (System.nanoTime() - t0) / 1000;
            t0 = System.nanoTime();
            try (BufferedOutputStream o = new BufferedOutputStream(new FileOutputStream(f), 65536)) { for (int i = 0; i < 2000; i++) o.write(b); }
            long big = (System.nanoTime() - t0) / 1000;
            t0 = System.nanoTime();
            java.util.zip.Deflater d = new java.util.zip.Deflater(); byte[] in = new byte[65536], out = new byte[65536]; java.util.Random r = new java.util.Random(1); for (int i = 0; i < in.length; i++) in[i] = (byte) (r.nextInt(16) * 3);
            for (int k = 0; k < 20; k++) { d.reset(); d.setInput(in); d.finish(); while (!d.finished()) d.deflate(out); } d.end();
            long z = (System.nanoTime() - t0) / 1000;
            log("fs probe: 2000 x 512 B unbuffered " + small / 1000 + " ms (" + small / 2000 + " us each), same buffered " + big / 1000 + " ms; deflate 20 x 64 KB " + z / 1000 + " ms");
        } catch (Throwable t) { log("fs probe failed: " + t); } finally { f.delete(); }
    }
    static boolean forClient(Map<String, Object> p) {
        List<Object> s = McDownload.arr(p.get("sides")); if (s == null) return true;
        for (Object o : s) if ("client".equals(o)) return true;
        return false;
    }
    static String subst(Map<String, String> vars, String s) {
        if (s == null) return "";
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\{([A-Za-z_0-9]+)\\}").matcher(s);
        StringBuffer b = new StringBuffer();
        while (m.find()) { String v = vars.get(m.group(1)); m.appendReplacement(b, java.util.regex.Matcher.quoteReplacement(v != null ? v : m.group(0))); }
        m.appendTail(b); return b.toString();
    }
    /** [group:artifact:version] -> the library file; 'text' -> text; /path/in/installer -> extracted file; anything else as it is */
    static String val(String root, ZipFile z, File dataDir, String s) throws IOException {
        if (s == null) return "";
        if (s.length() > 1 && s.startsWith("[") && s.endsWith("]")) return new File(root, "libraries/" + artPath(s.substring(1, s.length() - 1))).getPath();
        if (s.length() > 1 && s.startsWith("'") && s.endsWith("'")) return s.substring(1, s.length() - 1);
        if (s.startsWith("/") && z != null && z.getEntry(s.substring(1)) != null) {
            File d = new File(dataDir, s.substring(1)); d.getParentFile().mkdirs(); Files.write(d.toPath(), read(z, s.substring(1))); return d.getPath();
        }
        return s;
    }

    // ---- small writes: the tools write their jars through `new ZipOutputStream(new FileOutputStream(f))`, i.e. a write() system call for every 2-4 bytes of a zip header. On this file system one write() costs
    // 0.7 ms and rises to seconds as the file grows (log 7c18c978: "fs probe: 2000 x 512 B unbuffered 1484 ms, buffered 59 ms"; the processor thread sat in FileOutputStream.write for 25 min, ~1 class per second).
    // The processors' class loader rewrites the class-name constant java/io/FileOutputStream in every class it loads to McForge$BFOS, a FileOutputStream that collects its writes in a 64 KB buffer.
    public static class BFOS extends FileOutputStream {
        private final byte[] buf = new byte[65536]; private int n;
        public BFOS(File f) throws FileNotFoundException { super(f); }
        public BFOS(File f, boolean append) throws FileNotFoundException { super(f, append); }
        public BFOS(String f) throws FileNotFoundException { super(f); }
        public BFOS(String f, boolean append) throws FileNotFoundException { super(f, append); }
        public BFOS(FileDescriptor fd) { super(fd); }
        private void drain() throws IOException { if (n > 0) { int k = n; n = 0; super.write(buf, 0, k); } }
        @Override public void write(int b) throws IOException { if (n == buf.length) drain(); buf[n++] = (byte) b; }
        @Override public void write(byte[] b) throws IOException { write(b, 0, b.length); }
        @Override public void write(byte[] b, int off, int len) throws IOException {
            if (len >= buf.length) { drain(); super.write(b, off, len); return; }
            if (len > buf.length - n) drain();
            System.arraycopy(b, off, buf, n, len); n += len;
        }
        @Override public void flush() throws IOException { drain(); super.flush(); }
        @Override public java.nio.channels.FileChannel getChannel() { try { drain(); } catch (IOException e) { } return super.getChannel(); }
        @Override public void close() throws IOException { try { drain(); } finally { super.close(); } }
    }
    static final class ProcLoader extends URLClassLoader {
        ProcLoader(URL[] u, ClassLoader p) { super(u, p); }
        @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (name.equals("McForge$BFOS")) return Class.forName(name, false, McForge.class.getClassLoader());
            return super.loadClass(name, resolve);
        }
        @Override protected Class<?> findClass(String name) throws ClassNotFoundException {
            String res = name.replace('.', '/') + ".class";
            try (InputStream in = getResourceAsStream(res)) {
                if (in == null) throw new ClassNotFoundException(name);
                ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] x = new byte[16384]; int k; while ((k = in.read(x)) > 0) bo.write(x, 0, k);
                byte[] b = rewrite(bo.toByteArray());
                return defineClass(name, b, 0, b.length);
            } catch (IOException e) { throw new ClassNotFoundException(name, e); }
        }
    }
    /** the class file with the Utf8 constant "java/io/FileOutputStream" (a class name) replaced by "McForge$BFOS"; the rest is copied as it is */
    static byte[] rewrite(byte[] b) throws IOException {
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(b));
        ByteArrayOutputStream bo = new ByteArrayOutputStream(b.length + 64); DataOutputStream o = new DataOutputStream(bo);
        o.writeInt(in.readInt()); o.writeShort(in.readUnsignedShort()); o.writeShort(in.readUnsignedShort());
        int n = in.readUnsignedShort(); o.writeShort(n);
        boolean hit = false;
        for (int i = 1; i < n; i++) {
            int tag = in.readUnsignedByte(); o.writeByte(tag);
            int skip;
            switch (tag) {
                case 1: { String u = in.readUTF(); if (u.equals("java/io/FileOutputStream")) { u = "McForge$BFOS"; hit = true; } o.writeUTF(u); continue; }
                case 3: case 4: case 9: case 10: case 11: case 12: case 17: case 18: skip = 4; break;
                case 5: case 6: skip = 8; i++; break;
                case 7: case 8: case 16: case 19: case 20: skip = 2; break;
                case 15: skip = 3; break;
                default: throw new IOException("bad constant pool tag " + tag);
            }
            byte[] t = new byte[skip]; in.readFully(t); o.write(t);
        }
        if (!hit) return b;
        byte[] rest = new byte[in.available()]; in.readFully(rest); o.write(rest);
        return bo.toByteArray();
    }

    /** runs a processor's Main-Class in this VM, on this thread; its System.exit becomes an exception (McBoot.procThread) instead of "back to the launcher" */
    static void runMain(File jar, List<File> cp, List<String> args, final String label) throws Exception {
        String main;
        try (ZipFile jz = new ZipFile(jar)) {
            java.util.jar.Manifest mf = new java.util.jar.Manifest(new ByteArrayInputStream(read(jz, "META-INF/MANIFEST.MF")));
            main = mf.getMainAttributes().getValue("Main-Class");
        }
        if (main == null) throw new IOException(jar.getName() + " has no Main-Class");
        URL[] urls = new URL[cp.size()]; for (int i = 0; i < urls.length; i++) urls[i] = cp.get(i).toURI().toURL();
        URLClassLoader cl = new ProcLoader(urls, ClassLoader.getSystemClassLoader().getParent());
        ClassLoader old = Thread.currentThread().getContextClassLoader();
        Thread.currentThread().setContextClassLoader(cl);
        McBoot.procThread = Thread.currentThread();
        // The tools print one line per class (thousands): every line went through the UDP log and the render thread stalled behind it (log 47371f13: 600 ms frames, ~5 classes/s). Lines starting with two blanks
        // are only counted (one in 1000 is logged, with the rate); every other line (task headers, errors) is logged as before. The progress label shows the elapsed time, so a long step visibly runs.
        final PrintStream so = System.out, se = System.err;
        final long t0 = System.currentTimeMillis();
        final long[] cnt = new long[1];
        PrintStream quiet = new PrintStream(new OutputStream() {
            final StringBuilder sb = new StringBuilder();
            public void write(int b) {
                if (b == '\n') { line(sb.toString()); sb.setLength(0); } else if (b != '\r' && sb.length() < 400) sb.append((char) b);
            }
            boolean in;   // Hello.log must not feed itself if it ever prints to System.out
            void line(String l) {
                if (in) return; in = true;
                try { line2(l); } finally { in = false; }
            }
            void line2(String l) {
                cnt[0]++;
                boolean bulk = l.startsWith("  ") && l.indexOf("rror") < 0 && l.indexOf("xception") < 0;
                if (!bulk) log(l);
                else if (cnt[0] % 1000 == 0) log(cnt[0] + " lines so far, " + (cnt[0] * 1000L / Math.max(1, System.currentTimeMillis() - t0)) + " per second, last: " + l.trim());
            }
        }, true);
        final boolean[] run = { true };
        Thread tick = new Thread(new Runnable() { public void run() {
            while (run[0]) {
                long sec = (System.currentTimeMillis() - t0) / 1000;
                McDownload.pLabel = label + " (" + sec / 60 + ":" + (sec % 60 < 10 ? "0" : "") + sec % 60 + ")";
                try { Thread.sleep(1000); } catch (InterruptedException e) { return; }
            }
        } }, "forge-progress");
        tick.setDaemon(true); tick.start();
        // Probe (log c0086df7: jarsplitter ran at ~1 class per second with the Java side idle: gc/JIT counters flat, the sampled threads "asleep in the kernel", libc free pieces 51 -> 27): where is the
        // processor thread? Its state and top frames every 10 s, and how many lines came out in between.
        final Thread worker = Thread.currentThread();
        Thread mon = new Thread(new Runnable() { public void run() {
            long last = 0;
            while (run[0]) {
                try { Thread.sleep(10000); } catch (InterruptedException e) { return; }
                if (!run[0]) return;
                StringBuilder b = new StringBuilder("where: " + worker.getState() + ", +" + (cnt[0] - last) + " lines");
                last = cnt[0];
                StackTraceElement[] st = worker.getStackTrace();
                for (int i = 0; i < st.length && i < 9; i++) b.append(i == 0 ? " | " : " < ").append(st[i].getClassName().replaceAll("^.*\\.", "")).append('.').append(st[i].getMethodName()).append(':').append(st[i].getLineNumber());
                log(b.toString());
            }
        } }, "forge-where");
        mon.setDaemon(true); mon.start();
        System.setOut(quiet); System.setErr(quiet);
        final int prio = Thread.currentThread().getPriority(); try { Thread.currentThread().setPriority(Thread.MIN_PRIORITY); } catch (Throwable t) { }   // the launcher's own threads stay ahead
        try {
            Class.forName(main, true, cl).getMethod("main", String[].class).invoke(null, (Object) args.toArray(new String[0]));
        } catch (InvocationTargetException e) {
            Throwable t = e.getCause();
            if (t instanceof McBoot.ExitGuard) { int st = ((McBoot.ExitGuard) t).status; if (st != 0) throw new IOException(jar.getName() + " ended with exit code " + st + " (see the lines above)"); }
            else if (t instanceof Exception) throw (Exception) t; else throw new IOException(jar.getName() + " failed: " + t, t);
        } finally {
            System.setOut(so); System.setErr(se); try { Thread.currentThread().setPriority(prio); } catch (Throwable t) { } run[0] = false; tick.interrupt(); mon.interrupt();
            log(label + " finished in " + (System.currentTimeMillis() - t0) / 1000 + " s, " + cnt[0] + " output lines");
            McBoot.procThread = null; Thread.currentThread().setContextClassLoader(old);
            try { cl.close(); } catch (Throwable t) { }
        }
    }
}
