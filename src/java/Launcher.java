import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

// The launcher's model: instances with their own settings, and the command queue that the UI (later) and the text file <gameDir>/download.txt (now) both drive.
//   <root>/launcher.cfg                         instance=<id>  account=<id>  msaClientId=<azure app id>  ui=native|off (off skips the launcher at start-up)  jdk=8|17|21|25 (global Java choice from the launcher Settings; absent = automatic)  java=<major the last chosen version needs, written by McBoot>
//   <root>/instances/<id>/instance.cfg          name version loader gameDir xmx xms cpus jitThreads gc width height username account customJar options.<key> prop.<key>
//   (the id "default" without a file keeps the old layout: game folder = <root>, version = versions/selected.txt)
// jvm_host.cpp reads launcher.cfg + instance.cfg BEFORE the JVM exists (xmx, xms, cpus, jitThreads, gc); everything else is applied here, at launch.
public class Launcher {
    static void log(String s) { Hello.log("launcher: " + s); }

    // ---- key=value files (java.util.Properties reads them; they are written back plainly so the native side can parse them too)
    static Properties load(File f) {
        Properties p = new Properties();
        if (f.isFile()) try (InputStream in = new FileInputStream(f)) { p.load(in); } catch (IOException e) { log("cannot read " + f + ": " + e); }
        return p;
    }
    static void save(File f, Properties p) throws IOException {
        f.getParentFile().mkdirs();
        StringBuilder sb = new StringBuilder();
        for (String k : new TreeSet<String>(p.stringPropertyNames())) sb.append(k).append('=').append(p.getProperty(k).replace("\n", " ").replace("\r", " ")).append('\n');
        File tmp = new File(f.getPath() + ".tmp");
        Files.write(tmp.toPath(), sb.toString().getBytes(StandardCharsets.UTF_8));
        if (!tmp.renameTo(f)) Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }
    static String clean(String id) { String s = id == null ? "" : id.replaceAll("[^A-Za-z0-9._-]", "_"); return s.isEmpty() || s.startsWith(".") ? "default" : s; }

    static class Instance {
        final String id; final Properties p;
        Instance(String id, Properties p) { this.id = id; this.p = p; }
        String get(String k, String d) { String v = p.getProperty(k); return v == null || v.trim().isEmpty() ? d : v.trim(); }
        int geti(String k, int d) { try { return Integer.parseInt(get(k, "")); } catch (NumberFormatException e) { return d; } }
        String version() { return get("version", ""); }
        String loader() { return get("loader", ""); }
        String customJar() { return get("customJar", ""); }
    }
    static File instanceFile(String root, String id) { return new File(root, "instances/" + clean(id) + "/instance.cfg"); }
    static Instance instance(String root, String id) { id = clean(id); return new Instance(id, load(instanceFile(root, id))); }
    static void saveInstance(String root, Instance i) throws IOException { save(instanceFile(root, i.id), i.p); }
    static String currentId(String root) {
        String s = System.getProperty("ps5.instance");
        return clean(s != null ? s : load(new File(root, "launcher.cfg")).getProperty("instance", "default"));
    }
    static Instance current(String root) { return instance(root, currentId(root)); }
    static List<String> instanceIds(String root) {
        List<String> out = new ArrayList<>(); out.add("default");
        File[] fs = new File(root, "instances").listFiles(File::isDirectory);
        if (fs != null) { Arrays.sort(fs); for (File f : fs) if (!f.getName().equals("default")) out.add(f.getName()); }
        return out;
    }

    /** the instance's game folder (saves, options.txt, mods...). A custom folder (USB: /mnt/usb0/...) must accept writes, else the internal one is used. */
    static String gameDirOf(Instance i, String root) {
        String g = i.get("gameDir", "");
        if (g.isEmpty()) return i.id.equals("default") ? root : root + "/instances/" + i.id + "/game";
        File d = new File(g);
        try { d.mkdirs(); File t = new File(d, ".write_test"); Files.write(t.toPath(), "ok".getBytes()); t.delete(); return g; }
        catch (Throwable t) { String fb = root + "/instances/" + i.id + "/game"; log("game folder " + g + " is not writable (" + t + "), using " + fb); new File(fb).mkdirs(); return fb; }
    }

    /** One options.txt for every Minecraft version (Video Settings, FOV, render distance, keys: what you set stays set). The per-version copies (options.<version>.txt, the .options_for tag) were a mess: the launcher
     *  read a different file than the game, and the folder filled with copies. They are no longer written or switched; existing ones are left alone (delete them by hand when not needed). */
    static void optionsSwap(String gameDir, String key) {
        try { File tag = new File(gameDir, ".options_for"); if (tag.isFile() && tag.delete()) log("options.txt: one shared file for all versions now (the old per-version copies options.<version>.txt are left untouched)"); }
        catch (Throwable t) { log("options.txt tag cleanup failed: " + t); }
    }
    /** the options file the game of this instance reads: always <gameDir>/options.txt */
    static File optionsFileOf(String root, Instance i) { return new File(gameDirOf(i, root), "options.txt"); }
    static String optionValue(File f, String key) {
        try { if (f.isFile()) for (String l : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) if (l.startsWith(key + ":")) return l.substring(key.length() + 1).trim(); } catch (Throwable t) { }
        return "";
    }
    static void setOption(File f, String key, String val) throws IOException {
        List<String> lines = f.isFile() ? Files.readAllLines(f.toPath(), StandardCharsets.UTF_8) : new ArrayList<String>(); boolean done = false;
        for (int n = 0; n < lines.size(); n++) if (lines.get(n).startsWith(key + ":")) { lines.set(n, key + ":" + val); done = true; }
        if (!done) lines.add(key + ":" + val);
        f.getParentFile().mkdirs(); Files.write(f.toPath(), lines, StandardCharsets.UTF_8);
    }
    /** options.<key>=<value> of the instance are merged into <gameDir>/options.txt (Minecraft's own key:value file), other lines stay */
    static void applyOptions(Instance i, String gameDir) {
        Map<String, String> want = new LinkedHashMap<>();
        for (String k : i.p.stringPropertyNames()) if (k.startsWith("options.") && !i.p.getProperty(k).trim().isEmpty()) want.put(k.substring(8), i.p.getProperty(k).trim());
        // Minecraft (1.8.9 .. 26.x) takes overrideWidth/overrideHeight from options.txt over --width/--height: a leftover 720x408 there made the game start in a 720x408 window (the Mojang splash drawn small in the corner of
        // the 1920x1080 picture, the shim only reports the real size after the window exists). 0 = "use --width/--height" (1920x1080).
        want.putIfAbsent("overrideWidth", "0"); want.putIfAbsent("overrideHeight", "0");
        // a game that was killed (console has no clean exit) leaves startedCleanly:false -> MC then resets fullscreen and forces the OpenGL backend, which fails in the native Vulkan process
        want.putIfAbsent("startedCleanly", "true");
        try {
            File f = new File(gameDir, "options.txt"); List<String> lines = f.isFile() ? Files.readAllLines(f.toPath(), StandardCharsets.UTF_8) : new ArrayList<String>();
            for (String l : lines) if (l.startsWith("overrideWidth:") || l.startsWith("overrideHeight:") || l.startsWith("fullscreen:")) log("options.txt was: " + l);
            // Minecraft 26.2+ has two renderers and picks by "preferredGraphicsBackend" (default / opengl / vulkan; "default" is OpenGL in 26.2, Vulkan from 26.4): log 28851446, 26.2 on a Vulkan run took the OpenGL backend ("There is no OpenGL context current").
            // The run decides: a Vulkan run asks for vulkan, an OpenGL run (SDK or Zink) for opengl. Older games ignore the key.
            if (!want.containsKey("preferredGraphicsBackend")) {
                boolean has = false; for (String l : lines) if (l.startsWith("preferredGraphicsBackend:")) has = true;
                String run = System.getProperty("ps5.renderer"), rv = "vulkan".equals(run) ? "vulkan" : "opengl";
                if (has || "vulkan".equals(run)) want.put("preferredGraphicsBackend", "\"" + rv + "\"");
            }
            // Memory guard (log 700ea73e: view distance 32 on a 3 GB heap -> heap at 2.9 GB, full collections of 2 s each, "WHOLE JVM STOPPED" for 49 s): the chunk cache of a big view distance does not fit the heap this console gives the
            // game with Zink. Above the limit for this heap the option is lowered for the start (logged); touch .kit/no_distance_cap to turn this off.
            try {
                if (!new File(McBoot.DIR, ".kit/no_distance_cap").exists()) {
                    long mb = Runtime.getRuntime().maxMemory() >> 20; int cap = mb <= 3200 ? 16 : mb <= 4500 ? 20 : mb <= 6200 ? 24 : 32;
                    for (int n = 0; n < lines.size(); n++) { String l = lines.get(n); if (l.startsWith("renderDistance:")) { int v = Integer.parseInt(l.substring(15).trim()); if (v > cap && !want.containsKey("renderDistance")) { lines.set(n, "renderDistance:" + cap); log("options: renderDistance " + v + " lowered to " + cap + " for a " + mb + " MB heap (.kit/no_distance_cap keeps it)"); } } }
                }
            } catch (Throwable t) { }
            Set<String> done = new HashSet<>();
            for (int n = 0; n < lines.size(); n++) { int c = lines.get(n).indexOf(':'); if (c > 0) { String k = lines.get(n).substring(0, c); if (want.containsKey(k)) { lines.set(n, k + ":" + want.get(k)); done.add(k); } } }
            for (Map.Entry<String, String> e : want.entrySet()) if (!done.contains(e.getKey())) lines.add(e.getKey() + ":" + e.getValue());
            new File(gameDir).mkdirs(); Files.write(f.toPath(), lines, StandardCharsets.UTF_8);
            log("options.txt: " + want);
        } catch (Throwable t) { log("options.txt not written: " + t); }
    }
    /** prop.<name>=<value> become system properties (Fabric/mod flags, -Dfoo=bar style); real JVM flags (heap, cores, GC) are handled by the native host */
    static void applyProps(Instance i) { for (String k : i.p.stringPropertyNames()) if (k.startsWith("prop.")) System.setProperty(k.substring(5), i.p.getProperty(k).trim()); }

    static McAuth.Session session(Instance i, String root) {
        // The account is chosen in the launcher (launcher.cfg account=); an instance only overrides it with a real account id, an "offline" left in instance.cfg must not beat the signed-in account.
        String acc = i.get("account", "");
        if (acc.isEmpty() || acc.equals("offline")) acc = load(new File(root, "launcher.cfg")).getProperty("account", "").trim();
        log("session: account instance.cfg='" + i.get("account", "") + "' launcher.cfg='" + load(new File(root, "launcher.cfg")).getProperty("account", "").trim() + "' -> '" + acc + "' (root " + root + ")");
        if (!acc.isEmpty() && !acc.equals("offline")) try { McAuth.Session s = McAuth.sessionFor(root, acc.equals("active") ? "" : acc); log("signed in as " + s.name); return s; }
            catch (Throwable t) { log("account " + acc + " unavailable (" + t.getMessage() + "), playing offline"); }
        String un = i.get("username", load(new File(root, "launcher.cfg")).getProperty("username", "Player").trim()).replaceAll("[^A-Za-z0-9_]", "");
        return McAuth.offline(un.length() < 3 ? "Player" : un.length() > 16 ? un.substring(0, 16) : un);
    }
    // The GLFW shim (ps5lwjgl.c) ignores the size Minecraft asks for: the window is always the console's surface (1920x1080). Minecraft keeps the REQUESTED size as its window size for the mouse mapping,
    // so any other value made the cursor/buttons land in the wrong place (a 1280x720 request: "the buttons are 720p, the picture is 1080p"). Until the shim can render at a lower resolution and scale up,
    // width/height settings that differ from the surface are ignored (and logged).
    static final int DISPLAY_W = 1920, DISPLAY_H = 1080;
    /** the size Minecraft is told: the resolution this run of the app started with (<root>/.res_active, written by jvm_host from launcher.cfg resolution=1080|1440|2160, so changing it while the app runs changes nothing until restart) when that display mode exists (the Vulkan / Zink renderers list their modes in <root>/.display_modes), else 1920x1080. Same rule as ps5vk.inc. */
    static int[] displaySize(String root) {
        int h = 1080; try { File act = new File(root, ".res_active"); h = Integer.parseInt(act.isFile() ? new String(Files.readAllBytes(act.toPath()), StandardCharsets.UTF_8).trim() : load(new File(root, "launcher.cfg")).getProperty("resolution", "1080").trim()); } catch (Throwable t) { }
        int w = h == 2160 ? 3840 : h == 1440 ? 2560 : 1920; if (w == 1920) h = 1080;
        try { File mf = new File(root, ".display_modes"); if (w != 1920 && mf.isFile() && !Arrays.asList(new String(Files.readAllBytes(mf.toPath()), StandardCharsets.UTF_8).trim().split("\\s+")).contains(String.valueOf(w))) { log("resolution " + w + "x" + h + " is not offered by this output: 1920x1080"); w = 1920; h = 1080; } } catch (Throwable t) { }
        return new int[] { w, h };
    }
    static String[] launchArgs(Instance i, String versionId, String assetIndex, String type, McAuth.Session s, String root, String gameDir) {
        int[] d = displaySize(root); int w = d[0], h = d[1];   // the launcher's Resolution setting; per-instance width/height are not used (the window is the display)
        return new String[] { "--version", versionId, "--gameDir", gameDir, "--assetsDir", root + "/assets", "--assetIndex", assetIndex,
            "--username", s.name, "--uuid", s.uuid, "--accessToken", s.token, "--userType", s.type, "--versionType", type,
            "--width", String.valueOf(w), "--height", String.valueOf(h) };
    }

    // ---- command queue: <root>/download.txt, one command per line, '#' comments. Done lines are removed, failed ones stay.
    //   <version id> [select]          install a Mojang version (latest, latest-snapshot ok)        fabric|quilt <mc version> [loader version]
    //   mod|resourcepack|shader <slug>  Modrinth download into the current instance                  login                           Microsoft device-code sign-in
    //   instance new <id> [version]    instance use <id>   instance set <id> <key> <value>          account use <id|offline|active>          config <key> <value>   (e.g. config ui menu)
    public static void runQueue(String root) {
        File q = new File(root, "download.txt");
        if (!q.isFile()) return;
        List<String> keep = new ArrayList<>(); int ok = 0, fail = 0;
        try {
            for (String raw : Files.readAllLines(q.toPath(), StandardCharsets.UTF_8)) {
                String l = raw.trim();
                if (l.isEmpty() || l.startsWith("#")) { keep.add(raw); continue; }
                String[] w = l.split("\\s+");
                try { command(root, w); ok++; } catch (Throwable t) { fail++; keep.add(raw); log("FAILED '" + l + "': " + t); }
            }
            boolean any = false; for (String k : keep) if (!k.trim().isEmpty() && !k.trim().startsWith("#")) any = true;
            if (any || !keep.isEmpty()) Files.write(q.toPath(), keep, StandardCharsets.UTF_8); else q.delete();
            log("queue finished: " + ok + " done, " + fail + " failed");
        } catch (Throwable t) { log("queue " + q + " unusable: " + t); }
    }

    static void command(String root, String[] w) throws Exception {
        String c = w[0].toLowerCase(Locale.ROOT);
        Instance cur = current(root);
        switch (c) {
            case "fabric": case "quilt": {
                if (w.length < 2) throw new IOException(c + " needs a Minecraft version");
                String id = McMods.installLoader(root, c, w[1], w.length > 2 ? w[2] : null);
                cur.p.setProperty("version", id); cur.p.setProperty("loader", c); saveInstance(root, cur);
                log("instance " + cur.id + " now runs " + id); return; }
            case "forge": case "neoforge": {   // forge|neoforge <mc version> [loader version]: Minecraft 1.17+ (modern loaders; 1.20.1+ tested design)
                if (w.length < 2) throw new IOException(c + " needs a Minecraft version");
                String id = McForge.install(root, c, w[1], w.length > 2 ? w[2] : null);
                cur.p.setProperty("version", id); cur.p.setProperty("loader", c); saveInstance(root, cur);
                log("instance " + cur.id + " now runs " + id); return; }
            case "mod": case "resourcepack": case "shader": case "datapack": {
                if (w.length < 2) throw new IOException(c + " needs a Modrinth project slug");
                String mc = baseVersion(root, cur);
                List<String> got = McMods.install(w[1], c, mc, cur.loader(), gameDirOf(cur, root));
                log("installed " + got); return; }
            case "modpack": {   // Modrinth .mrpack as a new instance, which becomes the current one
                if (w.length < 2) throw new IOException("modpack needs a Modrinth project slug");
                String id = McMods.installPack(root, w[1], baseVersion(root, cur));
                Properties g = load(new File(root, "launcher.cfg")); g.setProperty("instance", id); save(new File(root, "launcher.cfg"), g);
                log("modpack installed as instance " + id); return; }
            case "login": { String id = McAuth.login(root); Properties g = load(new File(root, "launcher.cfg")); g.setProperty("account", id); save(new File(root, "launcher.cfg"), g); return; }
            case "config": {   // config <key> <value...>  (launcher.cfg: ui=native|off, msaClientId, ...)
                if (w.length < 3) throw new IOException("config <key> <value>");
                StringBuilder v = new StringBuilder(); for (int k = 2; k < w.length; k++) v.append(k > 2 ? " " : "").append(w[k]);
                Properties g = load(new File(root, "launcher.cfg")); g.setProperty(w[1], v.toString()); save(new File(root, "launcher.cfg"), g); log("launcher.cfg " + w[1] + "=" + v); return; }
            case "skin": { McAuth.skinFor(root, w.length > 1 ? w[1] : ""); return; }
            case "signout": { McAuth.signOut(root, w.length > 1 ? w[1] : ""); return; }
            case "account": {
                if (w.length < 3 || !w[1].equals("use")) throw new IOException("account use <id|offline|active>");
                Properties g = load(new File(root, "launcher.cfg")); g.setProperty("account", w[2]); save(new File(root, "launcher.cfg"), g); return; }
            case "instance": {
                if (w.length < 3) throw new IOException("instance new|use|set ...");
                String id = clean(w[2]);
                if (w[1].equals("new")) { Instance n = instance(root, id); if (w.length > 3) n.p.setProperty("version", w[3]); n.p.setProperty("name", id); saveInstance(root, n); new File(gameDirOf(n, root)).mkdirs(); log("instance " + id + " created"); return; }
                if (w[1].equals("use")) { Properties g = load(new File(root, "launcher.cfg")); g.setProperty("instance", id); save(new File(root, "launcher.cfg"), g); log("current instance: " + id); return; }
                if (w[1].equals("set") && w.length >= 5) { Instance n = instance(root, id); StringBuilder v = new StringBuilder(); for (int k = 4; k < w.length; k++) v.append(k > 4 ? " " : "").append(w[k]); n.p.setProperty(w[3], v.toString()); saveInstance(root, n); return; }
                if (w[1].equals("delete")) {   // the whole folder instances/<id> (settings, and the saves/mods when the game folder is the default one inside it); a custom gameDir elsewhere is not touched
                    if (id.equals("default")) throw new IOException("the default instance cannot be deleted");
                    File dir = new File(root, "instances/" + id);
                    if (!dir.isDirectory()) throw new IOException("instance " + id + " does not exist");
                    deleteTree(dir);
                    Properties g = load(new File(root, "launcher.cfg"));
                    if (id.equals(clean(g.getProperty("instance", "default")))) { g.setProperty("instance", "default"); save(new File(root, "launcher.cfg"), g); }
                    log("instance " + id + " deleted"); return; }
                throw new IOException("instance new <id> [version] | use <id> | set <id> <key> <value> | delete <id>"); }
            case "remove": {   // remove version <id> | remove mods|mods-disabled|resourcepacks|shaderpacks <url-encoded file name>
                if (w.length < 3) throw new IOException("remove version <id> | remove mods|mods-disabled|resourcepacks|shaderpacks <file name>");
                if (w[1].equals("version")) { removeVersion(root, clean(w[2])); return; }
                File f = inside(contentDir(root, cur, w[1]), dec(w[2]));
                deleteTree(f); log("removed " + w[1] + "/" + f.getName()); return; }
            case "option": {   // option renderDistance <2..32> fov <30..110> (either or both): written into the game's own options file (what Video Settings shows), no separate launcher value
                if (w.length < 3 || w.length % 2 == 0) throw new IOException("option renderDistance <2..32> fov <30..110>");
                File of = optionsFileOf(root, cur);
                for (int i = 1; i + 1 < w.length; i += 2) {
                    if (!w[i + 1].matches("\\d{1,3}")) throw new IOException("option " + w[i] + " needs a number");
                    int n = Integer.parseInt(w[i + 1]);
                    if (w[i].equals("fov")) { int deg = Math.max(30, Math.min(110, n)); setOption(of, "fov", String.valueOf((deg - 70) / 40.0)); cur.p.remove("options.fov"); log("field of view " + deg + " saved in the game's options of " + cur.id); }   // Minecraft keeps it as (degrees - 70) / 40 in every version
                    else if (w[i].equals("renderDistance")) { int v = Math.max(2, Math.min(32, n)); setOption(of, "renderDistance", String.valueOf(v)); cur.p.remove("options.renderDistance"); log("render distance " + v + " saved in the game's options of " + cur.id); }
                    else throw new IOException("unknown game setting " + w[i]);
                }
                saveInstance(root, cur); return; }
            case "world": {   // world rename <url-encoded folder> <url-encoded new name>: the name shown in Minecraft's world list (level.dat LevelName); the folder keeps its name
                if (w.length < 4 || !w[1].equals("rename")) throw new IOException("world rename <folder> <name>");
                File dir = inside(contentDir(root, cur, "saves"), dec(w[2]));
                renameWorld(dir, dec(w[3])); return; }
            case "toggle": {   // toggle datapacks <pack> <world>: copy the library pack into the world's datapacks folder, or take it out again
                if (w.length == 4 && w[1].equals("datapacks")) {
                    File src = inside(contentDir(root, cur, "datapacks"), dec(w[2])); File wd = inside(contentDir(root, cur, "saves"), dec(w[3]));
                    File dst = new File(new File(wd, "datapacks"), src.getName());
                    if (dst.exists()) { deleteTree(dst); log("datapack " + src.getName() + " removed from world " + wd.getName()); return; }
                    dst.getParentFile().mkdirs(); copyTree(src, dst); log("datapack " + src.getName() + " added to world " + wd.getName()); return; }
   // toggle mods <url-encoded jar name>: X.jar <-> X.jar.disabled (Fabric only loads *.jar)
                if (w.length < 3 || !w[1].equals("mods")) throw new IOException("toggle mods <file name>");
                File f = inside(contentDir(root, cur, "mods"), dec(w[2]));
                String n = f.getName(), to = n.endsWith(".jar.disabled") ? n.substring(0, n.length() - 9) : n.endsWith(".jar") ? n + ".disabled" : null;
                if (to == null) throw new IOException(n + " is not a mod jar");
                File t = new File(f.getParentFile(), to);
                if (t.exists()) throw new IOException(to + " already exists");
                if (!f.renameTo(t)) throw new IOException("cannot rename " + n);
                log("mod " + n + " -> " + to); return; }
            default: {
                String r = McDownload.install(root, w[0]); log("done: " + r);
                if (w.length > 1 && w[1].equals("select")) {
                    String id = r.substring(0, r.indexOf(' '));
                    Files.write(new File(root, "versions/selected.txt").toPath(), (id + "\n").getBytes(StandardCharsets.UTF_8));
                    cur.p.setProperty("version", id); if (id.startsWith("fabric-loader-")) cur.p.setProperty("loader", "fabric"); else if (id.startsWith("quilt-loader-")) cur.p.setProperty("loader", "quilt"); else if (id.contains("neoforge")) cur.p.setProperty("loader", "neoforge"); else if (id.contains("-forge-")) cur.p.setProperty("loader", "forge"); else cur.p.remove("loader"); if (!cur.id.equals("default") || instanceFile(root, "default").isFile()) saveInstance(root, cur);
                    log("selected " + id); } }
        }
    }
    // ---- removing things (Kit.java jobs: remove ..., toggle ..., instance delete ...)
    static String dec(String s) { try { return java.net.URLDecoder.decode(s, "UTF-8"); } catch (Exception e) { return s; } }
    /** recursive delete (files, folders; a world is thousands of small files, so this runs as a job, never on the UI thread) */
    static void deleteTree(File f) throws IOException {
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null) for (File k : kids) deleteTree(k);
        if (!f.delete() && f.exists()) throw new IOException("cannot delete " + f.getPath());
    }
    static File contentDir(String root, Instance i, String key) throws IOException {
        switch (key) {
            case "mods": case "mods-disabled": case "resourcepacks": case "shaderpacks": case "saves": case "datapacks": return new File(gameDirOf(i, root), key);
            default: throw new IOException("unknown folder " + key);
        }
    }
    /** a plain file name inside dir (no path parts), which must exist */
    static File inside(File dir, String name) throws IOException {
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.equals(".") || name.equals("..")) throw new IOException("bad file name '" + name + "'");
        File f = new File(dir, name);
        if (!f.exists()) throw new IOException(name + " is not there any more");
        return f;
    }
    /** instances that run version id (the default instance without its own version uses versions/selected.txt) and loader profiles that build on it */
    static List<String> versionUsers(String root, String id) {
        List<String> out = new ArrayList<>();
        for (String iid : instanceIds(root)) {
            Instance i = instance(root, iid);
            String v = i.version();
            if (v.isEmpty() && iid.equals("default")) try { v = new String(Files.readAllBytes(new File(root, "versions/selected.txt").toPath()), StandardCharsets.UTF_8).trim(); } catch (IOException e) { }
            if (v.equals(id)) out.add(i.get("name", iid.equals("default") ? "Default" : iid));
        }
        File[] vs = new File(root, "versions").listFiles(File::isDirectory);
        if (vs != null) { Arrays.sort(vs); for (File f : vs) {
            if (f.getName().equals(id)) continue;
            try { if (id.equals(McDownload.str(McDownload.readJson(McDownload.jsonFile(root, f.getName())).get("inheritsFrom")))) out.add(f.getName()); } catch (Throwable t) { } } }
        return out;
    }
    /** deletes versions/<id> (jar, json, marker) together with the Fabric/Quilt profiles built on it; libraries and assets are shared between versions and stay. Instances that ran it are left without a version (the launcher asks for one at Play). */
    static void removeVersion(String root, String id) throws IOException {
        File dir = new File(root, "versions/" + id);
        if (id.isEmpty() || !dir.isDirectory()) throw new IOException("version " + id + " is not installed");
        List<String> gone = new ArrayList<>(); gone.add(id);
        File[] vs = new File(root, "versions").listFiles(File::isDirectory);
        if (vs != null) { Arrays.sort(vs); for (File f : vs) {
            if (f.getName().equals(id)) continue;
            try { if (id.equals(McDownload.str(McDownload.readJson(McDownload.jsonFile(root, f.getName())).get("inheritsFrom")))) gone.add(f.getName()); } catch (Throwable t) { } } }
        for (String iid : instanceIds(root)) {
            Instance i = instance(root, iid);
            if (!gone.contains(i.version())) continue;
            i.p.remove("version"); i.p.remove("loader"); saveInstance(root, i);
            log("instance " + iid + " has no version now");
        }
        File sel = new File(root, "versions/selected.txt");
        try { if (sel.isFile() && gone.contains(new String(Files.readAllBytes(sel.toPath()), StandardCharsets.UTF_8).trim())) Files.write(sel.toPath(), new byte[0]); } catch (IOException e) { log("cannot clear selected.txt: " + e); }
        for (String g : gone) { File d = new File(root, "versions/" + g); if (d.isDirectory()) deleteTree(d); log("version " + g + " removed"); }
    }

    // ---- what the launcher's Library shows about worlds, mods and resource packs: `info <mods|resourcepacks|shaderpacks|saves>` writes <root>/.kit/info-<kind>.tsv
    /** minimal NBT (gzip): enough to read level.dat and to write it back with one string changed */
    static final class Tag { int t; Object v; int lt; Tag(int t, Object v) { this.t = t; this.v = v; } }
    @SuppressWarnings("unchecked") static Map<String, Tag> nbtMap(Tag t) { return t != null && t.t == 10 ? (Map<String, Tag>) t.v : null; }
    static Tag nbtRead(DataInputStream in, int type, int depth) throws IOException {
        if (depth > 40) throw new IOException("NBT too deep");
        switch (type) {
            case 1: return new Tag(1, in.readByte()); case 2: return new Tag(2, in.readShort()); case 3: return new Tag(3, in.readInt());
            case 4: return new Tag(4, in.readLong()); case 5: return new Tag(5, in.readFloat()); case 6: return new Tag(6, in.readDouble());
            case 7: { byte[] b = new byte[in.readInt()]; in.readFully(b); return new Tag(7, b); }
            case 8: return new Tag(8, in.readUTF());
            case 9: { int lt = in.readByte(), n = in.readInt(); List<Tag> l = new ArrayList<>(); for (int i = 0; i < n; i++) l.add(nbtRead(in, lt, depth + 1)); Tag t = new Tag(9, l); t.lt = lt; return t; }
            case 10: { Map<String, Tag> m = new LinkedHashMap<>(); for (;;) { int ty = in.readByte(); if (ty == 0) break; String k = in.readUTF(); m.put(k, nbtRead(in, ty, depth + 1)); } return new Tag(10, m); }
            case 11: { int[] a = new int[in.readInt()]; for (int i = 0; i < a.length; i++) a[i] = in.readInt(); return new Tag(11, a); }
            case 12: { long[] a = new long[in.readInt()]; for (int i = 0; i < a.length; i++) a[i] = in.readLong(); return new Tag(12, a); }
            default: throw new IOException("NBT tag " + type);
        }
    }
    @SuppressWarnings("unchecked") static void nbtWrite(DataOutputStream out, Tag t) throws IOException {
        switch (t.t) {
            case 1: out.writeByte((Byte) t.v); break; case 2: out.writeShort((Short) t.v); break; case 3: out.writeInt((Integer) t.v); break;
            case 4: out.writeLong((Long) t.v); break; case 5: out.writeFloat((Float) t.v); break; case 6: out.writeDouble((Double) t.v); break;
            case 7: { byte[] b = (byte[]) t.v; out.writeInt(b.length); out.write(b); break; }
            case 8: out.writeUTF((String) t.v); break;
            case 9: { List<Tag> l = (List<Tag>) t.v; out.writeByte(l.isEmpty() ? 0 : t.lt); out.writeInt(l.size()); for (Tag e : l) nbtWrite(out, e); break; }
            case 10: for (Map.Entry<String, Tag> e : ((Map<String, Tag>) t.v).entrySet()) { out.writeByte(e.getValue().t); out.writeUTF(e.getKey()); nbtWrite(out, e.getValue()); } out.writeByte(0); break;
            case 11: { int[] a = (int[]) t.v; out.writeInt(a.length); for (int x : a) out.writeInt(x); break; }
            case 12: { long[] a = (long[]) t.v; out.writeInt(a.length); for (long x : a) out.writeLong(x); break; }
            default: throw new IOException("NBT tag " + t.t);
        }
    }
    static Tag readLevel(File dir) throws IOException {
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new java.util.zip.GZIPInputStream(new FileInputStream(new File(dir, "level.dat")))))) {
            int ty = in.readByte(); if (ty != 10) throw new IOException("level.dat is not an NBT compound"); in.readUTF(); return nbtRead(in, 10, 0);
        }
    }
    static void renameWorld(File dir, String name) throws IOException {
        name = name.trim(); if (name.isEmpty() || name.length() > 64) throw new IOException("the name must be 1 to 64 characters");
        Tag root = readLevel(dir); Map<String, Tag> data = nbtMap(root) == null ? null : nbtMap(nbtMap(root).get("Data"));
        if (data == null) throw new IOException("level.dat has no Data");
        data.put("LevelName", new Tag(8, name));
        File lv = new File(dir, "level.dat"), tmp = new File(dir, "level.dat.tmp"), bak = new File(dir, "level.dat.ps5bak");
        try (DataOutputStream out = new DataOutputStream(new java.util.zip.GZIPOutputStream(new FileOutputStream(tmp)))) { out.writeByte(10); out.writeUTF(""); nbtWrite(out, root); }
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new java.util.zip.GZIPInputStream(new FileInputStream(tmp))))) {
            in.readByte(); in.readUTF(); Map<String, Tag> d2 = nbtMap(nbtMap(nbtRead(in, 10, 0)).get("Data"));
            if (d2 == null || !name.equals(d2.get("LevelName").v)) throw new IOException("the rewritten level.dat did not verify");
        }
        Files.copy(lv.toPath(), bak.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        Files.move(tmp.toPath(), lv.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        log("world " + dir.getName() + " renamed to " + name + " (the old level.dat is level.dat.ps5bak)");
    }
    static void copyTree(File a, File b) throws IOException {
        if (a.isDirectory()) { b.mkdirs(); File[] k = a.listFiles(); if (k != null) for (File c : k) copyTree(c, new File(b, c.getName())); }
        else Files.copy(a.toPath(), b.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }
    static long treeSize(File f, int[] budget) { if (!f.isDirectory()) return f.length(); long n = 0; File[] k = f.listFiles(); if (k != null) for (File c : k) { if (--budget[0] < 0) break; n += treeSize(c, budget); } return n; }
    static String clean1(String s) { return s == null ? "" : s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim(); }

    /** one tab-separated row per entry, written to <root>/.kit/info-<kind>.tsv; returns a one-line summary */
    static String info(String root, String kind) throws Exception {
        Instance cur = current(root); File gd = new File(gameDirOf(cur, root)); File out = new File(new File(root, ".kit"), "info-" + kind + ".tsv"); out.getParentFile().mkdirs();
        StringBuilder sb = new StringBuilder(); int n = 0;
        if (kind.equals("mods")) {
            for (String dirName : new String[] { "mods", "mods-disabled" }) {
                File[] fs = new File(gd, dirName).listFiles((d, nm) -> nm.endsWith(".jar") || nm.endsWith(".jar.disabled")); if (fs == null) continue; Arrays.sort(fs);
                String mc = baseVersion(root, cur); Map<File, String> why = McMods.fitPlan(gd.getPath(), mc, cur.loader().isEmpty() ? "fabric" : cur.loader());
                for (File f : fs) { McMods.ModInfo m = McMods.readMod(f);
                    sb.append(clean1(f.getName())).append('\t').append(m.loader).append('\t').append(clean1(m.mcLabel)).append('\t').append(clean1(m.version)).append('\t').append(clean1(m.name)).append('\t').append(clean1(why.get(f))).append('\n'); n++; }
            }
        } else if (kind.equals("resourcepacks")) {
            File[] fs = new File(gd, "resourcepacks").listFiles(); if (fs != null) { Arrays.sort(fs); for (File f : fs) {
                int fmt = -1; String desc = "";
                try {
                    String js = null;
                    if (f.isDirectory()) { File mm = new File(f, "pack.mcmeta"); if (mm.isFile()) js = new String(Files.readAllBytes(mm.toPath()), StandardCharsets.UTF_8); }
                    else if (f.getName().endsWith(".zip")) try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(f)) { java.util.zip.ZipEntry e = z.getEntry("pack.mcmeta"); if (e != null) js = McMods.readAll(z.getInputStream(e)); }
                    if (js != null) { Map<String, Object> top = McDownload.obj(McDownload.Json.parse(js.startsWith("﻿") ? js.substring(1) : js)), p = top != null ? McDownload.obj(top.get("pack")) : null;
                        if (p != null) { Object pf = p.get("pack_format"); if (pf instanceof Double) fmt = ((Double) pf).intValue(); else { Object mf = p.get("min_format"); if (mf instanceof Double) fmt = ((Double) mf).intValue(); } } }
                } catch (Throwable t) { }
                if (!f.isDirectory() && !f.getName().endsWith(".zip")) continue;
                sb.append(clean1(f.getName())).append('\t').append('\t').append(fmt < 0 ? "" : packLabel(fmt)).append('\t').append(fmt < 0 ? "" : "format " + fmt).append('\t').append('\t').append('\n'); n++; } }
        } else if (kind.equals("saves")) {
            File icons = new File(new File(root, ".kit"), "icons"); icons.mkdirs(); File[] old = icons.listFiles(); if (old != null) for (File o : old) o.delete();
            File[] fs = new File(gd, "saves").listFiles(File::isDirectory); List<String[]> rows = new ArrayList<>();
            if (fs != null) for (File f : fs) {
                if (!new File(f, "level.dat").isFile()) continue;
                String name = f.getName(), ver = "", mode = ""; long played = 0; 
                try { Map<String, Tag> d = nbtMap(nbtMap(readLevel(f)).get("Data"));
                    if (d != null) { if (d.get("LevelName") != null) name = (String) d.get("LevelName").v; if (d.get("LastPlayed") != null) played = (Long) d.get("LastPlayed").v;
                        int gt = d.get("GameType") != null ? ((Number) d.get("GameType").v).intValue() : -1; mode = gt == 0 ? "Survival" : gt == 1 ? "Creative" : gt == 2 ? "Adventure" : gt == 3 ? "Spectator" : "";
                        if (d.get("hardcore") != null && ((Number) d.get("hardcore").v).intValue() != 0) mode = "Hardcore";
                        Map<String, Tag> v = nbtMap(d.get("Version")); ver = v != null && v.get("Name") != null ? (String) v.get("Name").v : (d.get("version") != null ? "before 1.9" : ""); }
                } catch (Throwable t) { ver = "unreadable"; }
                String icon = ""; File ip = new File(f, "icon.png");
                if (ip.isFile()) try { java.awt.image.BufferedImage im = javax.imageio.ImageIO.read(ip);
                    if (im != null && im.getWidth() <= 512 && im.getHeight() <= 512) { int w = im.getWidth(), h = im.getHeight(); java.nio.ByteBuffer bb = java.nio.ByteBuffer.allocate(8 + w * h * 4).order(java.nio.ByteOrder.LITTLE_ENDIAN); bb.putInt(w).putInt(h);
                        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int p = im.getRGB(x, y); bb.put((byte) (p >> 16)).put((byte) (p >> 8)).put((byte) p).put((byte) (p >>> 24)); }
                        File rf = new File(icons, rows.size() + ".rgba"); Files.write(rf.toPath(), bb.array()); icon = rf.getPath(); } } catch (Throwable t) { }
                rows.add(new String[] { clean1(f.getName()), clean1(name), clean1(ver), mode, String.valueOf(played), String.valueOf(treeSize(f, new int[] { 20000 })), icon });
            }
            Collections.sort(rows, (a, b) -> Long.compare(Long.parseLong(b[4]), Long.parseLong(a[4])));
            // icons were numbered before sorting; the paths stay valid, only the row order changed
            for (String[] r : rows) { sb.append(String.join("\t", r)).append('\n'); n++; }
        } else if (kind.equals("datapacks")) {   // one row per library pack and world: file = pack, loader = world folder ("" = no world yet), mc = the world's real name (level.dat LevelName), version = bytes, name = "1" when the world has it; worlds come most recently played first
            File[] lib = new File(gd, "datapacks").listFiles(); File[] ws = new File(gd, "saves").listFiles(f -> f.isDirectory() && new File(f, "level.dat").isFile());
            if (lib != null) { Arrays.sort(lib);
                final Map<File, String> wname = new HashMap<>(), wver = new HashMap<>(); final Map<File, Long> wplay = new HashMap<>();
                if (ws != null) for (File w : ws) { String nm = w.getName(), vn = ""; long pl = 0;
                    try { Map<String, Tag> d = nbtMap(nbtMap(readLevel(w)).get("Data")); if (d != null) { if (d.get("LevelName") != null) nm = (String) d.get("LevelName").v; if (d.get("LastPlayed") != null) pl = (Long) d.get("LastPlayed").v; if (d.get("Version") != null) { Map<String, Tag> vm = nbtMap(d.get("Version")); if (vm != null && vm.get("Name") != null) vn = (String) vm.get("Name").v; } } } catch (Throwable t) { }
                    wname.put(w, nm); wver.put(w, vn); wplay.put(w, pl); }
                if (ws != null) Arrays.sort(ws, (x, y) -> Long.compare(wplay.get(y), wplay.get(x)));
                for (File f : lib) { if (f.getName().startsWith(".") || !(f.isDirectory() || f.getName().endsWith(".zip"))) continue;
                    String sz = String.valueOf(treeSize(f, new int[] { 20000 }));
                    if (ws == null || ws.length == 0) { sb.append(clean1(f.getName())).append("\t\t\t").append(sz).append("\t0\t\n"); n++; continue; }
                    for (File w : ws) { sb.append(clean1(f.getName())).append('\t').append(clean1(w.getName())).append('\t').append(clean1(wname.get(w))).append('\t').append(sz).append('\t').append(new File(w, "datapacks/" + f.getName()).exists() ? "1" : "0").append('\t').append(clean1(wver.get(w))).append('\n'); n++; } } }
        } else if (kind.equals("options")) {   // the game's own settings the launcher shows: key <tab> value ("" = the game has not saved one yet)
            File of = optionsFileOf(root, cur); sb.append("renderDistance\t").append(optionValue(of, "renderDistance")).append('\n').append("fov\t").append(optionValue(of, "fov")).append('\n'); n += 2;
        } else if (kind.equals("shaderpacks")) { return "nothing to read"; }
        else throw new IOException("info mods|resourcepacks|datapacks|saves");
        Kit.put(out, sb.toString());
        return n + " entries";
    }
    /** pack_format -> the Minecraft versions it belongs to (only the ones known for sure; newer formats are shown as "format N") */
    static String packLabel(int f) {
        switch (f) {
            case 1: return "1.6 - 1.8.9"; case 2: return "1.9 - 1.10.2"; case 3: return "1.11 - 1.12.2"; case 4: return "1.13 - 1.14.4"; case 5: return "1.15 - 1.16.1"; case 6: return "1.16.2 - 1.16.5";
            case 7: return "1.17.x"; case 8: return "1.18.x"; case 9: return "1.19 - 1.19.2"; case 12: return "1.19.3"; case 13: return "1.19.4"; case 15: return "1.20 - 1.20.1"; case 18: return "1.20.2";
            case 22: return "1.20.3 - 1.20.4"; case 32: return "1.20.5 - 1.20.6"; case 34: return "1.21 - 1.21.1"; case 42: return "1.21.2 - 1.21.3"; case 46: return "1.21.4"; case 55: return "1.21.5";
            case 63: return "1.21.6"; case 64: return "1.21.7 - 1.21.8"; case 69: return "1.21.9 - 1.21.10"; default: return "";
        }
    }

    /** the Mojang version a loader profile builds on (for mod compatibility): follows inheritsFrom */
    static String baseVersion(String root, Instance i) {
        String v = i.version(); if (v.isEmpty()) try { v = new String(Files.readAllBytes(new File(root, "versions/selected.txt").toPath()), StandardCharsets.UTF_8).trim(); } catch (IOException e) { }
        for (int n = 0; n < 5 && !v.isEmpty(); n++) { try { String p = McDownload.str(McDownload.readJson(McDownload.jsonFile(root, v)).get("inheritsFrom")); if (p == null) break; v = p; } catch (Throwable t) { break; } }
        return v;
    }
}
