import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/**
 * Bridge between the game's Java side and the native launcher (the PS5 UI kit, C++).
 *
 * The native UI runs INSIDE Kit.run() (registered by jvm_host.cpp) on the calling thread and returns when the player presses Play.
 * It never calls back into Java. Everything that needs the network is a job: the UI writes one line to <root>/.kit/job.cmd, the
 * job thread below picks it up, runs it with the existing engines (McDownload, McMods, Launcher) and reports through
 * <root>/.kit/job.status; list results go to .kit/versions.tsv and .kit/search.tsv. The UI polls those files.
 *
 *   job.cmd     "<id>\t<command>"       command: list-versions | search <type> <query...> | any Launcher.command line
 *                                        (<version> select, fabric <mc>, quilt <mc>, mod|resourcepack|shader <slug>, instance ...,
 *                                         remove version <id> | remove mods|mods-disabled|resourcepacks|shaderpacks <url-encoded name>, toggle mods <url-encoded name>,
 *                                         instance delete <id>, info mods|resourcepacks|saves, world rename <url-encoded folder> <url-encoded name>)
 *   runtime.txt java= vm=               written once when the job thread starts (the Java this title runs on)
 *   job.status  id= state=running|done|error title= detail= progress=<0..1 or -1>
 */
public final class Kit {
    static native int run(String root);
    /** buttons held at start-up (polls the pad up to ms milliseconds; returns early once Square, 0x8000, is down) */
    static native int held(int ms);

    static void log(String s) { Hello.log("kit: " + s); }
    static File dir(String root) { File d = new File(root, ".kit"); d.mkdirs(); return d; }

    /** Shows the launcher; returns when the player pressed Play (launcher.cfg "instance" is then set). Only ui=off skips it (an absent or any other value shows it).
     *  If it cannot run at all (no native launcher in this build, or it failed to start 6 times in a row) the game starts with the selected instance, and the log says why. */
    public static void show(String root) {
        String mode = Launcher.load(new File(root, "launcher.cfg")).getProperty("ui", "native").trim();
        if (mode.equals("off")) {
            int held = 0;
            try { held = held(800); } catch (Throwable t) { log("button check at start-up not available: " + t); }
            if ((held & 0x8000) == 0) { log("ui=off: the launcher is skipped (hold Square while the app starts to show it once)"); return; }
            log("Square was held at start-up: the launcher shows this once (launcher.cfg keeps ui=off)");
        }
        File marker = new File(dir(root), "running");
        // A stale marker (the title was closed or crashed while the launcher was open) used to skip the launcher "this once": that is the random straight-into-Minecraft start. It is only logged now.
        if (marker.exists()) { log("the last native launcher did not end cleanly (title closed / crashed while it was open): showing it again"); marker.delete(); }
        for (int attempt = 1; attempt <= 6; attempt++) {
            try {
                startJobs(root);
                soundIndex(root);
                Files.write(marker.toPath(), "1\n".getBytes(StandardCharsets.UTF_8));
                bootReset(root);
                int r = run(root);
                marker.delete();
                if (r > 0) booting = true;   // the native side now shows its loading screen; bootLine() feeds it until the game has its window
                log("native launcher returned " + r + (attempt > 1 ? " (attempt " + attempt + ")" : ""));
                if (r > 0) return;   // the player pressed Play
                log("native launcher did not start (attempt " + attempt + " of 6)");
            } catch (UnsatisfiedLinkError e) {
                marker.delete(); log("this build has no native launcher (" + e.getMessage() + "): rebuild with the 'launcher' option; the selected instance starts"); return;
            } catch (Throwable t) {
                marker.delete(); log("native launcher failed (attempt " + attempt + " of 6): " + t);
            }
            try { Thread.sleep(1500); } catch (InterruptedException e) { return; }
        }
        log("native launcher could not be started 6 times: the selected instance starts (set ui=off in launcher.cfg to skip it on purpose)");
    }

    /** A Play that continues after the app was restarted (McBoot.takeAutoplay) has no launcher screen, but it still gets the loading screen: the native side sees .kit/bootonly, skips the launcher and goes straight to it. */
    public static void resume(String root) {
        String mode = Launcher.load(new File(root, "launcher.cfg")).getProperty("ui", "native").trim();
        if (mode.equals("off")) return;
        File flag = new File(dir(root), "bootonly");
        try {
            startJobs(root);
            soundIndex(root);
            bootReset(root);
            Files.write(flag.toPath(), "1\n".getBytes(StandardCharsets.UTF_8));
            int r = run(root);
            if (r > 0) booting = true;
            log("loading screen for the resumed Play: native returned " + r);
        } catch (UnsatisfiedLinkError e) {
            log("this build has no native launcher (" + e.getMessage() + "): no loading screen");
        } catch (Throwable t) {
            log("loading screen for the resumed Play failed: " + t);
        } finally { flag.delete(); }
    }
    /** Replaces the loading screen's text (the app is about to end, see McBoot.restartNotice); false when no loading screen is up. The game's log lines no longer change it afterwards. */
    static synchronized boolean bootNotice(String title, String stage, String detail) {
        if (!booting || bootFile == null) return false;
        if (detail != null && detail.length() > 170) detail = detail.substring(0, 170);
        bTitle = title; bStage = stage; bDetail = detail == null ? "" : detail;
        bootWrite();
        booting = false;
        return true;
    }

    // ---------------------------------------------------------------------------------------------- loading screen (Play -> the game's window)
    // The native launcher keeps its window as a loading screen after Play and reads <root>/.kit/boot.txt (title / stage / newest log line, one per line, ~7x a second at most).
    // Every log line passes Hello.log -> bootLine(): the stage follows well-known lines of the game and its loader, the detail is the line itself. Unknown lines only change the detail,
    // so a new Minecraft version or loader still shows something true.
    static volatile boolean booting;
    static File bootFile;
    static String bTitle = "", bStage = "Starting the game", bDetail = "";
    static long bLast, bStart;

    static synchronized void bootReset(String root) {
        booting = false; bootFile = new File(dir(root), "boot.txt"); bootFile.delete();
        Hello.logHook = new java.util.function.Consumer<String>() { public void accept(String line) { if (booting) bootLine(line); } };
        bTitle = ""; bStage = "Starting the game"; bDetail = ""; bLast = 0; bStart = System.currentTimeMillis();
    }
    static final String[][] BOOT_STAGES = {   // log text -> stage shown (first match wins)
        { "Copying minecraft to a temporary jar", "Preparing the game files for these mods (first start only, this can take a few minutes)" },
        { "Loading Minecraft", "Starting the mod loader" },
        { "SpongePowered MIXIN", "Starting the mods" },
        { "Searching for graphics cards", "Starting the mods" },
        { "Setting user", "Starting the game" },
        { "Backend library", "Opening the window" },
        { "OpenGL Renderer", "Starting the renderer" },
        { "Reloading ResourceManager", "Loading resources" },
        { "McBoot: launching", "Loading the game" },
        { "launcher: instance", "Loading the game" },
        { "mods: ", "Checking the mods" },
        { "deps: ", "Checking the mods" },
    };
    static synchronized void bootLine(String s) {
        if (!booting || s == null) return;
        String t = s.trim();
        if (t.startsWith("[java]")) t = t.substring(6).trim();
        t = t.replaceFirst("^\\[\\d\\d:\\d\\d:\\d\\d\\] \\[[^\\]]*\\]: ", "");
        if (t.isEmpty() || t.startsWith("at ") || t.startsWith("\tat ") || t.startsWith("kit:") || t.startsWith("CRASH")) return;
        int i = t.indexOf("; running ");
        if (t.startsWith("version: installed") && i > 0 && bTitle.isEmpty()) {   // the first pick is the game being started; later picks (the Forge priming parse) must not rename it
            String id = t.substring(i + 10).trim();
            if (id.startsWith("fabric-loader-") && id.lastIndexOf('-') > 13) id = "Fabric " + id.substring(id.lastIndexOf('-') + 1);
            else if (id.startsWith("quilt-loader-") && id.lastIndexOf('-') > 12) id = "Quilt " + id.substring(id.lastIndexOf('-') + 1);
            bTitle = "Starting " + id; }
        for (String[] st : BOOT_STAGES) if (t.contains(st[0])) { bStage = st[1]; break; }
        if (t.length() > 170) t = t.substring(0, 170);
        bDetail = t;
        // the window is created right after these: the loading screen ends there, nothing more to write
        if (t.contains("Backend library") || t.contains("OpenGL Vendor") || t.contains("OpenGL Renderer")) { bootWrite(); booting = false; return; }
        long now = System.currentTimeMillis();
        if (now - bLast < 140) return;
        bootWrite();
    }
    static void bootWrite() {
        try {
            bLast = System.currentTimeMillis();
            File tmp = new File(bootFile.getPath() + ".tmp");
            Files.write(tmp.toPath(), (bTitle + "\n" + bStage + "\n" + bDetail + "\n").getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(bootFile)) { bootFile.delete(); tmp.renameTo(bootFile); }
        } catch (Throwable t) { }
    }

    /** The launcher plays Minecraft's own UI sounds, decoded from the game assets: this tells it which asset index to read (the largest = newest one in assets/indexes). Empty: nothing downloaded yet, tones are used. */
    static void soundIndex(String root) {
        try {
            File[] fs = new File(root, "assets/indexes").listFiles();
            File best = null;
            if (fs != null) for (File f : fs) if (f.getName().endsWith(".json") && (best == null || f.length() > best.length())) best = f;
            put(new File(dir(root), "sounds.txt"), best == null ? "" : best.getPath() + "\n");
        } catch (Throwable t) { log("cannot write sounds.txt: " + t); }
    }

    // ---------------------------------------------------------------------------------------------- job thread
    static Thread jobs;
    static volatile String result = "";
    static volatile String error = null;

    static synchronized void startJobs(final String root) {
        if (jobs != null) return;
        final File d = dir(root);
        new File(d, "job.cmd").delete();
        try { put(new File(d, "runtime.txt"), "java=" + System.getProperty("java.version") + "\nvm=" + System.getProperty("java.vm.name") + "\n"); } catch (IOException e) { log("cannot write runtime.txt: " + e); }
        status(d, "0", "idle", "", "", -1);
        jobs = new Thread(new Runnable() { public void run() { loop(root, d); } }, "kit-jobs");
        jobs.setDaemon(true);
        jobs.start();
    }

    static void loop(String root, File d) {
        File cmd = new File(d, "job.cmd");
        for (;;) {
            try { Thread.sleep(150); } catch (InterruptedException e) { return; }
            if (!cmd.isFile()) continue;
            String line;
            try { line = new String(Files.readAllBytes(cmd.toPath()), StandardCharsets.UTF_8).trim(); } catch (IOException e) { continue; }
            cmd.delete();
            if (line.isEmpty()) continue;
            int tab = line.indexOf('\t');
            String id = tab > 0 ? line.substring(0, tab) : "0", c = tab > 0 ? line.substring(tab + 1).trim() : line;
            runJob(root, d, id, c);
        }
    }

    static void runJob(final String root, File d, String id, final String c) {
        final String title = describe(c);
        log("job " + id + ": " + c);
        result = ""; error = null;
        status(d, id, "running", title, "", -1);
        Thread w = new Thread(new Runnable() { public void run() {
            try { result = execute(root, c); } catch (Throwable t) { error = message(t); log("job failed: " + t); t.printStackTrace(); }
        } }, "kit-job");
        w.setDaemon(true);
        w.start();
        while (w.isAlive()) {
            try { w.join(250); } catch (InterruptedException e) { break; }
            String label = McDownload.pLabel; int tot = McDownload.pTotal;
            String detail = label.isEmpty() ? "" : label + "  " + McDownload.pDone + " / " + tot + " files";
            long bt = McDownload.pBytesTotal; double pr = -1;
            if (!label.isEmpty() && bt > 0) pr = Math.min(1.0, Math.max(0.0, (McDownload.BYTES.get() - McDownload.pBytesBase) / (double) bt));
            else if (!label.isEmpty() && tot > 0) pr = Math.min(1.0, McDownload.pDone / (double) tot);
            if (w.isAlive()) status(d, id, "running", title, detail, pr);
        }
        if (error != null) status(d, id, "error", title, error, -1);
        else status(d, id, "done", title, result, 1);
    }

    static String message(Throwable t) {
        String m = t.getMessage();
        if (m == null || m.isEmpty()) m = t.toString();
        return m.replace('\n', ' ').replace('\r', ' ');
    }

    static String describe(String c) {
        String[] w = c.split("\\s+");
        switch (w[0]) {
            case "list-versions": return "Loading the version list";
            case "search": return "Searching Modrinth";
            case "fabric": case "quilt": return "Installing " + (w[0].equals("fabric") ? "Fabric" : "Quilt") + (w.length > 1 ? " for " + w[1] : "");
            case "mod": case "resourcepack": case "shader": case "datapack": return "Installing " + (w.length > 1 ? w[1] : w[0]);
            case "modpack": return "Installing modpack " + (w.length > 1 ? w[1] : "");
            case "remove": return "Deleting " + (w.length > 2 ? Launcher.dec(w[2]) : "");
            case "toggle": return "Switching " + (w.length > 2 ? Launcher.dec(w[2]) : "");
            case "info": return "Reading " + (w.length > 1 ? (w[1].equals("saves") ? "worlds" : w[1].equals("resourcepacks") ? "resource packs" : w[1]) : "the folder");
            case "world": return "Renaming the world";
            case "option": return "Saving the game setting";
            case "instance": return w.length > 1 && w[1].equals("delete") ? "Deleting instance " + (w.length > 2 ? w[2] : "") : "Updating settings";
            case "config": case "account": return "Updating settings"; default: return "Installing " + w[0];
        }
    }

    static String execute(String root, String c) throws Exception {
        String[] w = c.split("\\s+");
        File d = dir(root);
        switch (w[0]) {
            case "list-versions": {
                Map<String, Object> man = McDownload.manifest(new File(root, "versions/version_manifest_v2.json"));
                StringBuilder sb = new StringBuilder(); int n = 0;
                for (Object o : McDownload.arr(man.get("versions"))) {
                    Map<String, Object> m = McDownload.obj(o); if (m == null) continue;
                    sb.append(McDownload.str(m.get("id"))).append('\t').append(McDownload.str(m.get("type"))).append('\t').append(String.valueOf(McDownload.str(m.get("releaseTime"))).replace('T', ' ')).append('\n'); n++;
                }
                put(new File(d, "versions.tsv"), sb.toString());
                return n + " versions";
            }
            case "search": {
                if (w.length < 2) throw new IOException("search <type> <query>");
                StringBuilder q = new StringBuilder(); for (int i = 2; i < w.length; i++) q.append(i > 2 ? " " : "").append(w[i]);
                Launcher.Instance cur = Launcher.current(root);
                String mc = Launcher.baseVersion(root, cur);
                List<String[]> rows = McMods.searchRows(q.toString(), w[1], mc, cur.loader(), 40);
                StringBuilder sb = new StringBuilder();
                for (String[] r : rows) sb.append(String.join("\t", r)).append('\n');
                put(new File(d, "search.tsv"), sb.toString());
                return rows.size() + " results for Minecraft " + mc + (cur.loader().isEmpty() ? "" : " / " + cur.loader());
            }
            case "info": return Launcher.info(root, w.length > 1 ? w[1] : "mods");
            case "mod": {
                if (Launcher.current(root).loader().isEmpty()) throw new IOException("this instance has no mod loader: install Fabric or Quilt for it first (Game Versions, then Square or Triangle on a version)");
                break;
            }
            default: break;
        }
        Launcher.command(root, w);
        switch (w[0]) {
            case "remove": return "Deleted " + Launcher.dec(w[2]);
            case "toggle": return "Switched " + Launcher.dec(w[2]);
            case "world": return "World renamed";
            case "instance": if (w.length > 2 && w[1].equals("delete")) return "Instance deleted"; break;
            default: break;
        }
        return "Done: " + c;
    }

    static void put(File f, String text) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        Files.write(tmp.toPath(), text.getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    static void status(File d, String id, String state, String title, String detail, double progress) {
        try {
            put(new File(d, "job.status"), "id=" + id + "\nstate=" + state + "\ntitle=" + title.replace('\n', ' ') + "\ndetail=" + detail.replace('\n', ' ') + "\nprogress=" + String.format(Locale.ROOT, "%.3f", progress) + "\n");
        } catch (IOException e) { log("cannot write job.status: " + e); }
    }
}
