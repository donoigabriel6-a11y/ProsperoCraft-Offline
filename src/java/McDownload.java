import java.io.*;
import java.net.HttpURLConnection;
import java.net.Proxy;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

// Version downloader for the launcher. <gameDir>/download.txt lists what to install, one per line ("1.20.1", "latest", "latest-snapshot"; add " select" to make it the version that starts).
// Done lines are removed, failed ones stay. Layout (what McBoot.pickVersion reads): versions/<id>/<id>.json + <id>.jar, libraries/<maven path>, assets/indexes/<index>.json, assets/objects/<xx>/<sha1>.
// Blocking sockets with generous timeouts only (non-blocking mode is emulated by ps5lwjgl.c; DNS by its getaddrinfo replacement). The <id>.jar is written LAST, so a half-installed version is never picked.
// LWJGL libraries are skipped: the title carries its own LWJGL jars + static natives (a version needing another LWJGL generation may not run). Log lines: "net: dl ...".
public class McDownload {
    static final String MANIFEST = System.getProperty("ps5.dl.manifest", "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    static final String ASSETS = System.getProperty("ps5.dl.assets", "https://resources.download.minecraft.net/");
    static final String LIBS = System.getProperty("ps5.dl.libs", "https://libraries.minecraft.net/");
    static final int THREADS = Integer.getInteger("ps5.dl.threads", 8);
    static final boolean WITH_LWJGL = "1".equals(System.getProperty("ps5.dl.lwjgl"));

    static void log(String s) { Hello.log("net: dl " + s); }

    // ---- minimal JSON reader (objects -> LinkedHashMap, arrays -> ArrayList, numbers -> Double); the JDK has none and the title must not depend on a library jar
    static final class Json {
        final String s; int i;
        Json(String s) { this.s = s; }
        static Object parse(String s) { Json j = new Json(s); Object o = j.val(); j.ws(); if (j.i != s.length()) throw new IllegalArgumentException("trailing JSON at " + j.i); return o; }
        void ws() { while (i < s.length() && s.charAt(i) <= ' ') i++; }
        Object val() {
            ws(); if (i >= s.length()) throw new IllegalArgumentException("unexpected end of JSON");
            char c = s.charAt(i);
            if (c == '{') { i++; Map<String, Object> m = new LinkedHashMap<>(); ws(); if (s.charAt(i) == '}') { i++; return m; }
                for (;;) { ws(); String k = str(); ws(); expect(':'); m.put(k, val()); ws(); char d = s.charAt(i++); if (d == '}') return m; if (d != ',') throw new IllegalArgumentException("bad object at " + i); } }
            if (c == '[') { i++; List<Object> l = new ArrayList<>(); ws(); if (s.charAt(i) == ']') { i++; return l; }
                for (;;) { l.add(val()); ws(); char d = s.charAt(i++); if (d == ']') return l; if (d != ',') throw new IllegalArgumentException("bad array at " + i); } }
            if (c == '"') return str();
            if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
            if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
            if (s.startsWith("null", i)) { i += 4; return null; }
            int b = i; while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
            if (b == i) throw new IllegalArgumentException("bad JSON at " + i);
            return Double.valueOf(s.substring(b, i));
        }
        void expect(char c) { if (s.charAt(i) != c) throw new IllegalArgumentException("expected " + c + " at " + i); i++; }
        String str() {
            expect('"'); StringBuilder sb = new StringBuilder();
            for (;;) { char c = s.charAt(i++); if (c == '"') return sb.toString();
                if (c != '\\') { sb.append(c); continue; }
                char e = s.charAt(i++);
                switch (e) { case 'n': sb.append('\n'); break; case 't': sb.append('\t'); break; case 'r': sb.append('\r'); break; case 'b': sb.append('\b'); break; case 'f': sb.append('\f'); break;
                    case 'u': sb.append((char) Integer.parseInt(s.substring(i, i + 4), 16)); i += 4; break; default: sb.append(e); } }
        }
    }
    @SuppressWarnings("unchecked") static Map<String, Object> obj(Object o) { return o instanceof Map ? (Map<String, Object>) o : null; }
    @SuppressWarnings("unchecked") static List<Object> arr(Object o) { return o instanceof List ? (List<Object>) o : null; }
    static String str(Object o) { return o instanceof String ? (String) o : null; }
    static long num(Object o) { return o instanceof Double ? ((Double) o).longValue() : 0; }

    // ---- Mojang's rules: no "rules" = allowed; otherwise the last matching rule decides (default deny). We present as 64-bit Linux; rules keyed on launcher "features" never match.
    static boolean allowed(Object rules) {
        List<Object> l = arr(rules); if (l == null) return true;
        boolean ok = false;
        for (Object r : l) {
            Map<String, Object> m = obj(r); if (m == null || m.containsKey("features")) continue;
            Map<String, Object> os = obj(m.get("os")); boolean match = true;
            if (os != null) { String n = str(os.get("name")), arch = str(os.get("arch")); if (n != null && !n.equals("linux")) match = false; if (arch != null && !arch.equals("x86_64") && !arch.equals("amd64")) match = false; }
            if (match) ok = "allow".equals(str(m.get("action")));
        }
        return ok;
    }

    // ---- HTTP
    static final AtomicLong BYTES = new AtomicLong();
    // progress of the running batch (runAll), read by the native launcher through Kit
    static volatile String pLabel = ""; static volatile int pDone, pTotal; static volatile long pBytesTotal, pBytesBase;
    static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection(Proxy.NO_PROXY);
        c.setConnectTimeout(20000); c.setReadTimeout(30000); c.setRequestProperty("User-Agent", "ps5-mc-launcher");
        return c;
    }
    static byte[] get(String url) throws IOException {
        IOException last = null;
        for (int t = 0; t < 3; t++) {
            try { HttpURLConnection c = open(url); int code = c.getResponseCode(); if (code != 200) throw new IOException("HTTP " + code + " for " + url);
                  try (InputStream in = c.getInputStream()) { ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] b = new byte[16384]; int n; while ((n = in.read(b)) > 0) { bo.write(b, 0, n); BYTES.addAndGet(n); } return bo.toByteArray(); } }
            catch (IOException e) { last = e; sleep(500L << t); }
        }
        throw last;
    }
    static void sleep(long ms) { try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
    static String hex(byte[] d) { StringBuilder sb = new StringBuilder(); for (byte b : d) sb.append(String.format("%02x", b & 255)); return sb.toString(); }

    static final class Task {
        final String url, sha1, what; final File dest; final long size;
        Task(String url, File dest, String sha1, long size, String what) { this.url = url; this.dest = dest; this.sha1 = sha1; this.size = size; this.what = what; }
        boolean present() { return dest.isFile() && (size <= 0 ? dest.length() > 0 : dest.length() == size); }
    }
    // one file: download to <dest>.part, verify length and SHA-1, then rename; 3 tries
    static void fetch(Task t) throws IOException {
        IOException last = null;
        for (int tries = 0; tries < 3; tries++) {
            File tmp = new File(t.dest.getPath() + ".part");
            try {
                t.dest.getParentFile().mkdirs();
                HttpURLConnection c = open(t.url); int code = c.getResponseCode(); if (code != 200) throw new IOException("HTTP " + code);
                MessageDigest md = t.sha1 != null ? MessageDigest.getInstance("SHA-1") : null; long total = 0;
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(tmp)) {
                    byte[] b = new byte[65536]; int n; while ((n = in.read(b)) > 0) { out.write(b, 0, n); if (md != null) md.update(b, 0, n); total += n; BYTES.addAndGet(n); } }
                if (t.size > 0 && total != t.size) throw new IOException("size " + total + " != " + t.size);
                if (md != null && !hex(md.digest()).equalsIgnoreCase(t.sha1)) throw new IOException("SHA-1 mismatch");
                try { Files.move(tmp.toPath(), t.dest.toPath(), StandardCopyOption.REPLACE_EXISTING); } catch (IOException e) { if (!tmp.renameTo(t.dest)) throw e; }
                return;
            } catch (Exception e) { last = e instanceof IOException ? (IOException) e : new IOException(e); tmp.delete(); sleep(500L << tries); }
        }
        throw new IOException(t.what + ": " + last);
    }

    // run a batch of downloads on THREADS threads, logging progress every 3 s; returns the number of failures
    static int runAll(String label, List<Task> tasks) throws InterruptedException {
        List<Task> todo = new ArrayList<>(); long totalBytes = 0, have = 0;
        for (Task t : tasks) { if (t.present()) have++; else { todo.add(t); totalBytes += Math.max(t.size, 0); } }
        log(label + ": " + tasks.size() + " files, " + have + " already present, " + todo.size() + " to download (" + (totalBytes >> 20) + " MB)");
        if (todo.isEmpty()) return 0;
        pLabel = label; pTotal = todo.size(); pDone = 0; pBytesTotal = totalBytes; pBytesBase = BYTES.get();
        ExecutorService ex = Executors.newFixedThreadPool(Math.max(1, Math.min(THREADS, todo.size())));
        AtomicInteger done = new AtomicInteger(), failed = new AtomicInteger();
        for (final Task t : todo) ex.execute(new Runnable() { public void run() { try { fetch(t); } catch (IOException e) { failed.incrementAndGet(); log("FAILED " + e.getMessage()); } finally { pDone = done.incrementAndGet(); } } });
        ex.shutdown();
        long t0 = System.nanoTime(), b0 = BYTES.get();
        while (!ex.awaitTermination(3, TimeUnit.SECONDS)) {
            double s = Math.max((System.nanoTime() - t0) / 1e9, 0.001);
            log(label + ": " + done.get() + "/" + todo.size() + " files, " + ((BYTES.get() - b0) >> 20) + " MB, " + String.format("%.1f", (BYTES.get() - b0) / 1048576.0 / s) + " MB/s, failed " + failed.get());
        }
        pLabel = "";
        log(label + ": finished, " + (todo.size() - failed.get()) + " ok, " + failed.get() + " failed (" + ((BYTES.get() - b0) >> 20) + " MB)");
        return failed.get();
    }

    static String mavenPath(String name) {   // group:artifact:version[:classifier] -> group/path/artifact/version/artifact-version[-classifier].jar
        String[] p = name.split(":"); if (p.length < 3) return null;
        return p[0].replace('.', '/') + "/" + p[1] + "/" + p[2] + "/" + p[1] + "-" + p[2] + (p.length > 3 ? "-" + p[3] : "") + ".jar";
    }

    static Map<String, Object> manifest(File cache) throws IOException {
        try { byte[] b = get(MANIFEST); Map<String, Object> m = obj(Json.parse(new String(b, StandardCharsets.UTF_8))); try { cache.getParentFile().mkdirs(); Files.write(cache.toPath(), b); } catch (IOException ce) { log("cannot cache the manifest: " + ce); } return m; }
        catch (Exception e) {
            log("manifest download failed (" + e + ")");
            if (cache.isFile()) { log("using the cached manifest " + cache); return obj(Json.parse(new String(Files.readAllBytes(cache.toPath()), StandardCharsets.UTF_8))); }
            throw new IOException("no manifest: " + e);
        }
    }

    // ---- generic HTTP for the launcher's JSON APIs (auth, Fabric meta, Modrinth). Returns the status and the body for 4xx/5xx too (the device-code poll answers HTTP 400 while it waits).
    static final class Resp { int code; String body = ""; }
    static Resp http(String method, String url, String body, String contentType, String... headers) throws IOException {
        HttpURLConnection c = open(url);
        c.setRequestMethod(method); c.setRequestProperty("Accept", "application/json");
        for (int i = 0; i + 1 < headers.length; i += 2) c.setRequestProperty(headers[i], headers[i + 1]);
        if (body != null) { byte[] b = body.getBytes(StandardCharsets.UTF_8); c.setDoOutput(true); c.setRequestProperty("Content-Type", contentType); c.setFixedLengthStreamingMode(b.length); try (OutputStream o = c.getOutputStream()) { o.write(b); } }
        Resp r = new Resp(); r.code = c.getResponseCode();
        InputStream in = r.code >= 400 ? c.getErrorStream() : c.getInputStream();
        if (in != null) try (InputStream i2 = in) { ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] bb = new byte[8192]; int n; while ((n = i2.read(bb)) > 0) bo.write(bb, 0, n); r.body = bo.toString("UTF-8"); }
        return r;
    }
    static Map<String, Object> readJson(File f) throws IOException { return obj(Json.parse(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8))); }
    static File jsonFile(String gameDir, String id) { return new File(gameDir, "versions/" + id + "/" + id + ".json"); }
    static String libKey(Object o) { Map<String, Object> m = obj(o); String n = m != null ? str(m.get("name")) : null; if (n == null) return String.valueOf(o); String[] p = n.split(":"); return p.length >= 3 ? p[0] + ":" + p[1] + (p.length > 3 ? ":" + p[3] : "") : n; }

    /** the version JSON with "inheritsFrom" followed: libraries = the child's, then the parent's that the child does not replace (same group:artifact[:classifier]); mainClass/arguments from the child; assetIndex, downloads, javaVersion from the parent.
     *  "_jarId" names the version whose <id>.jar is the game jar (Fabric/Quilt profiles have none of their own). */
    static Map<String, Object> resolve(String gameDir, String id) throws IOException {
        Map<String, Object> cur = readJson(jsonFile(gameDir, id));
        Map<String, Object> out = new LinkedHashMap<>(cur);
        out.put("_jarId", str(cur.get("jar")) != null ? str(cur.get("jar")) : id);
        String parentId = str(cur.get("inheritsFrom"));
        if (parentId == null) return out;
        Map<String, Object> par = resolve(gameDir, parentId);
        List<Object> libs = new ArrayList<>(); Set<String> keys = new HashSet<>();
        List<Object> own = arr(cur.get("libraries")); if (own != null) for (Object o : own) { libs.add(o); keys.add(libKey(o)); }
        List<Object> inh = arr(par.get("libraries")); if (inh != null) for (Object o : inh) if (!keys.contains(libKey(o))) libs.add(o);
        out.put("libraries", libs);
        for (String k : new String[] { "assetIndex", "assets", "downloads", "javaVersion", "type", "mainClass" }) if (!out.containsKey(k) && par.containsKey(k)) out.put(k, par.get(k));
        if (str(cur.get("jar")) == null) out.put("_jarId", par.get("_jarId"));
        return out;
    }

    /** the download tasks for a "libraries" array (rules applied, LWJGL and native bundles skipped); counts = {lwjgl, native, rule} */
    static List<Task> libraryTasks(String gameDir, List<Object> ll, int[] counts) {
        List<Task> libs = new ArrayList<>();
        if (ll == null) return libs;
        for (Object o : ll) {
            Map<String, Object> lib = obj(o); if (lib == null) continue;
            String name = str(lib.get("name")); if (name == null) continue;
            if (!allowed(lib.get("rules"))) { counts[2]++; continue; }
            if (!WITH_LWJGL && name.startsWith("org.lwjgl")) { counts[0]++; continue; }
            Map<String, Object> dl = obj(lib.get("downloads")), art = dl != null ? obj(dl.get("artifact")) : null;
            String path = art != null ? str(art.get("path")) : null, url = art != null ? str(art.get("url")) : null, sha = art != null ? str(art.get("sha1")) : null; long size = art != null ? num(art.get("size")) : 0;
            if (art == null) {
                if (lib.containsKey("natives") || (dl != null && dl.containsKey("classifiers"))) { counts[1]++; continue; }   // old-style native bundles: LWJGL/natives are static in the title
                path = mavenPath(name); if (path == null) continue; String base = str(lib.get("url")); url = (base != null ? (base.endsWith("/") ? base : base + "/") : LIBS) + path;
                if (str(lib.get("sha1")) != null) sha = str(lib.get("sha1"));   // Fabric/Quilt profiles carry sha1/size beside name+url
                size = num(lib.get("size"));
            }
            if (path == null || url == null || path.contains("-natives-")) { counts[1]++; continue; }
            libs.add(new Task(url, new File(gameDir, "libraries/" + path), sha, size, name));
        }
        return libs;
    }

    /** installs one version; returns a short summary, throws if anything failed (the version jar is then not written) */
    public static String install(String gameDir, String want) throws Exception {
        long t0 = System.nanoTime(); long b0 = BYTES.get();
        File vroot = new File(gameDir, "versions"); vroot.mkdirs();
        File localJson = jsonFile(gameDir, want);
        if (localJson.isFile()) {   // a profile that is already on disk and builds on another version (Fabric, Quilt, a custom one)
            Map<String, Object> lj = readJson(localJson);
            if (str(lj.get("inheritsFrom")) != null) return installDerived(gameDir, want, lj, t0, b0);
        }
        Map<String, Object> man = manifest(new File(vroot, "version_manifest_v2.json"));
        String id = want;
        Map<String, Object> latest = obj(man.get("latest"));
        if (want.equals("latest") && latest != null) id = str(latest.get("release")); else if (want.equals("latest-snapshot") && latest != null) id = str(latest.get("snapshot"));
        Map<String, Object> entry = null;
        for (Object o : arr(man.get("versions"))) { Map<String, Object> m = obj(o); if (m != null && id.equals(str(m.get("id")))) { entry = m; break; } }
        if (entry == null) throw new IOException("version " + id + " is not in the manifest");
        log(want + (want.equals(id) ? "" : " = " + id) + ": " + str(entry.get("type")) + ", released " + str(entry.get("releaseTime")));
        File vdir = new File(vroot, id), vjson = new File(vdir, id + ".json"), vjar = new File(vdir, id + ".jar");
        vdir.mkdirs();
        String jsonSha = str(entry.get("sha1"));
        boolean haveJson = false;
        if (vjson.isFile()) try { MessageDigest md = MessageDigest.getInstance("SHA-1"); haveJson = jsonSha == null || hex(md.digest(Files.readAllBytes(vjson.toPath()))).equalsIgnoreCase(jsonSha); } catch (Exception ignore) { }
        if (!haveJson) fetch(new Task(str(entry.get("url")), vjson, jsonSha, 0, id + ".json"));
        Map<String, Object> v = readJson(vjson);
        Map<String, Object> jv = obj(v.get("javaVersion"));
        if (jv != null) log(id + " wants Java " + (long) num(jv.get("majorVersion")) + " (this title: " + System.getProperty("java.specification.version") + ")");
        int[] cnt = new int[3];
        List<Task> libs = libraryTasks(gameDir, arr(v.get("libraries")), cnt);
        log(id + ": " + libs.size() + " libraries (" + cnt[0] + " LWJGL skipped, " + cnt[1] + " native bundles skipped, " + cnt[2] + " not for this OS)");
        int bad = runAll(id + " libraries", libs);
        // assets
        List<Task> assets = new ArrayList<>(); Map<String, Object> ai = obj(v.get("assetIndex"));
        if (ai != null) {
            String aid = str(ai.get("id")); File ifile = new File(gameDir, "assets/indexes/" + aid + ".json");
            Task it = new Task(str(ai.get("url")), ifile, str(ai.get("sha1")), num(ai.get("size")), "asset index " + aid);
            if (!it.present()) fetch(it);
            Map<String, Object> ix = obj(Json.parse(new String(Files.readAllBytes(ifile.toPath()), StandardCharsets.UTF_8)));
            Map<String, Object> objs = ix != null ? obj(ix.get("objects")) : null;
            if (ix != null && (Boolean.TRUE.equals(ix.get("virtual")) || Boolean.TRUE.equals(ix.get("map_to_resources")))) log(id + ": asset index " + aid + " is a virtual/legacy one (pre-1.7.3 layout): copied to resources/ at launch");
            Set<String> seen = new HashSet<>();
            if (objs != null) for (Object o : objs.values()) {
                Map<String, Object> ob = obj(o); String h = ob != null ? str(ob.get("hash")) : null; if (h == null || !seen.add(h)) continue;
                assets.add(new Task(ASSETS + h.substring(0, 2) + "/" + h, new File(gameDir, "assets/objects/" + h.substring(0, 2) + "/" + h), h, num(ob.get("size")), "asset " + h));
            }
        }
        bad += runAll(id + " assets", assets);
        if (bad > 0) throw new IOException(id + ": " + bad + " files failed, not finishing (the client jar is written last; run again to resume)");
        // client jar last: its presence marks a complete install
        Map<String, Object> dls = obj(v.get("downloads")), cl = dls != null ? obj(dls.get("client")) : null;
        if (cl == null) throw new IOException(id + " has no client download");
        Task jar = new Task(str(cl.get("url")), vjar, str(cl.get("sha1")), num(cl.get("size")), id + ".jar");
        if (!jar.present()) { log(id + ": client jar " + (num(cl.get("size")) >> 20) + " MB"); fetch(jar); }
        return id + " installed in " + ((System.nanoTime() - t0) / 1000000000L) + " s, " + ((BYTES.get() - b0) >> 20) + " MB downloaded";
    }

    /** a profile with "inheritsFrom": install the parent, then the profile's own libraries; <id>.installed (not a jar) marks it complete */
    static String installDerived(String gameDir, String id, Map<String, Object> lj, long t0, long b0) throws Exception {
        String parent = str(lj.get("inheritsFrom"));
        String pr = install(gameDir, parent);
        int[] cnt = new int[3];
        List<Task> libs = libraryTasks(gameDir, arr(lj.get("libraries")), cnt);
        log(id + " (builds on " + parent + "): " + libs.size() + " own libraries");
        int bad = runAll(id + " libraries", libs);
        if (bad > 0) throw new IOException(id + ": " + bad + " libraries failed (run again to resume)");
        Files.write(new File(gameDir, "versions/" + id + "/" + id + ".installed").toPath(), "ok\n".getBytes(StandardCharsets.UTF_8));
        return id + " installed in " + ((System.nanoTime() - t0) / 1000000000L) + " s, " + ((BYTES.get() - b0) >> 20) + " MB downloaded";
    }

    /** reads <gameDir>/download.txt; installs each listed version; keeps the lines that failed */
    public static void processQueue(String gameDir) {
        File q = new File(gameDir, "download.txt");
        if (!q.isFile()) return;
        List<String> keep = new ArrayList<>(); int ok = 0, fail = 0;
        try {
            for (String raw : Files.readAllLines(q.toPath(), StandardCharsets.UTF_8)) {
                String l = raw.trim();
                if (l.isEmpty() || l.startsWith("#")) { keep.add(raw); continue; }
                String[] w = l.split("\\s+");
                try {
                    String r = install(gameDir, w[0]); ok++; log("done: " + r);
                    if (w.length > 1 && w[1].equals("select")) { String id = r.substring(0, r.indexOf(' ')); Files.write(new File(gameDir, "versions/selected.txt").toPath(), (id + "\n").getBytes(StandardCharsets.UTF_8)); log("selected " + id); }
                } catch (Throwable t) { fail++; keep.add(raw); log("FAILED " + w[0] + ": " + t); }
            }
            boolean any = false; for (String k : keep) if (!k.trim().isEmpty() && !k.trim().startsWith("#")) any = true;
            if (any || !keep.isEmpty()) Files.write(q.toPath(), keep, StandardCharsets.UTF_8); else q.delete();
            log("queue finished: " + ok + " installed, " + fail + " failed");
        } catch (Throwable t) { log("queue " + q + " unusable: " + t); }
    }
}
