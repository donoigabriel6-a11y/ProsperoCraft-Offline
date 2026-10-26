import java.io.*;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

// Mod loaders and content for an instance: Fabric / Quilt profiles (version JSONs that inherit from the vanilla one, handled by McDownload.resolve/install) and Modrinth projects (mods, resource packs,
// shader packs) downloaded into the instance's game folder. APIs: meta.fabricmc.net, meta.quiltmc.org, api.modrinth.com (overridable for tests).
public class McMods {
    static String fabricBase() { return System.getProperty("ps5.fabric.meta", "https://meta.fabricmc.net/v2"); }
    static String quiltBase() { return System.getProperty("ps5.quilt.meta", "https://meta.quiltmc.org/v3"); }
    static String modrinth() { return System.getProperty("ps5.modrinth.base", "https://api.modrinth.com"); }
    static void log(String s) { Hello.log("net: mods " + s); }
    static String enc(String s) { try { return URLEncoder.encode(s, "UTF-8"); } catch (UnsupportedEncodingException e) { throw new RuntimeException(e); } }
    static Object getJson(String url) throws IOException {
        McDownload.Resp r = McDownload.http("GET", url, null, null);
        if (r.code != 200) throw new IOException("GET " + url + " -> HTTP " + r.code);
        return McDownload.Json.parse(r.body);
    }

    /** installs the newest stable (or the given) Fabric/Quilt loader for a Minecraft version; returns the profile id to run (e.g. fabric-loader-0.16.5-1.21.1) */
    public static String installLoader(String gameDir, String kind, String mc, String loaderVersion) throws Exception {
        String base = kind.equals("quilt") ? quiltBase() : fabricBase();
        String lv = loaderVersion;
        if (lv == null || lv.isEmpty()) {
            List<Object> all = McDownload.arr(getJson(base + "/versions/loader/" + enc(mc)));
            if (all == null || all.isEmpty()) throw new IOException(kind + " has no loader for Minecraft " + mc);
            for (Object o : all) { Map<String, Object> lo = McDownload.obj(McDownload.obj(o).get("loader")); if (lo != null && !Boolean.FALSE.equals(lo.get("stable") == null ? Boolean.TRUE : lo.get("stable"))) { lv = McDownload.str(lo.get("version")); break; } }
            if (lv == null) lv = McDownload.str(McDownload.obj(McDownload.obj(all.get(0)).get("loader")).get("version"));
        }
        McDownload.Resp r = McDownload.http("GET", base + "/versions/loader/" + enc(mc) + "/" + enc(lv) + "/profile/json", null, null);
        if (r.code != 200) throw new IOException(kind + " profile for " + mc + " / " + lv + " -> HTTP " + r.code);
        Map<String, Object> prof = McDownload.obj(McDownload.Json.parse(r.body));
        String id = McDownload.str(prof.get("id"));
        if (id == null || id.isEmpty() || id.contains("/") || id.contains("..")) throw new IOException("bad profile id " + id);
        File f = McDownload.jsonFile(gameDir, id); f.getParentFile().mkdirs();
        Files.write(f.toPath(), r.body.getBytes(StandardCharsets.UTF_8));
        log(kind + " loader " + lv + " for " + mc + " -> profile " + id);
        McDownload.install(gameDir, id);   // vanilla parent + the loader's libraries
        return id;
    }


    // ---- mods folder vs the Minecraft version about to run
    /** Mods are built for one Minecraft version: a 1.21.1 Sodium in the folder stops Fabric 1.20.1 at startup ("incompatible mods"). Before a Fabric/Quilt launch every jar in <gameDir>/mods whose fabric.mod.json / quilt.mod.json
     *  depends on a Minecraft range that excludes <mc> moves to <gameDir>/mods-disabled, and jars there that fit <mc> move back. Jars that say nothing about Minecraft, or that cannot be read, are left alone. */
    /** What a mod jar says about itself (fabric.mod.json, quilt.mod.json, META-INF/mods.toml, neoforge.mods.toml, mcmod.info). loader "" = could not tell. */
    static final class ModInfo {
        String loader = "", id = "", name = "", version = "", mcLabel = "";
        Object mcNeed;   // Fabric/Quilt "minecraft" dependency: a range string or a list of them
        String tomlRange = "";
        int javaMajor = 0;   // newest Java the jar's classes (the first few) were compiled for: 17, 21 ... 0 = unknown
        String ns = "";   // namespace the mod's class tweaker / access widener is written in: "official" = unobfuscated Minecraft (26.x), "intermediary"/"named" = obfuscated (1.x)
        final List<String> provides = new ArrayList<String>(), deps = new ArrayList<String>();
        final Map<String, Object> depRange = new HashMap<String, Object>();   // Fabric "depends": mod id -> version range (a string or a list of them)
    }
    static ModInfo readMod(File jar) {
        ModInfo m = new ModInfo();
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(jar)) {
            java.util.zip.ZipEntry e;
            int seen = 0;   // class file major version of the first classes: Veinminer 2.0.7 is Java 21 and kills a Java 17 start with UnsupportedClassVersionError
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements() && seen < 12; ) {
                java.util.zip.ZipEntry ce = en.nextElement(); String cn = ce.getName();
                if (!cn.endsWith(".class") || cn.startsWith("META-INF/")) continue;
                seen++;
                try (java.io.InputStream in = z.getInputStream(ce)) { byte[] h = new byte[8]; int got = 0, k; while (got < 8 && (k = in.read(h, got, 8 - got)) > 0) got += k; if (got == 8) m.javaMajor = Math.max(m.javaMajor, ((h[6] & 255) << 8 | (h[7] & 255)) - 44); }
            }
            if ((e = z.getEntry("fabric.mod.json")) != null) {
                Map<String, Object> j = McDownload.obj(McDownload.Json.parse(readAll(z.getInputStream(e))));
                if (j != null) {
                    m.loader = "Fabric"; m.id = s(j.get("id")); m.name = s(j.get("name")); m.version = s(j.get("version"));
                    List<Object> pr = McDownload.arr(j.get("provides")); if (pr != null) for (Object o : pr) if (o instanceof String) m.provides.add((String) o);
                    for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements(); ) {   // jars nested in this one (Fabric API's modules, libraries a mod carries) are loaded by Fabric: what they provide counts
                        java.util.zip.ZipEntry ne = en.nextElement(); String nn = ne.getName();
                        if (nn.startsWith("META-INF/jars/") && nn.endsWith(".jar")) try (InputStream ni = z.getInputStream(ne)) { nestedIds(ni, m.provides, 1); } catch (Throwable t) { }
                    }
                    Map<String, Object> d = McDownload.obj(j.get("depends"));
                    if (d != null) for (Map.Entry<String, Object> en : d.entrySet()) { if (en.getKey().equals("minecraft")) m.mcNeed = en.getValue(); else { m.deps.add(en.getKey()); m.depRange.put(en.getKey(), en.getValue()); } }
                    for (String key : new String[] { "classTweaker", "accessWidener" }) {   // the first line says which names the file uses: "classTweaker v1 official" / "accessWidener v2 named"
                        Object aw = j.get(key); if (!(aw instanceof String)) continue;
                        java.util.zip.ZipEntry ae = z.getEntry((String) aw); if (ae == null) continue;
                        String first = readAll(z.getInputStream(ae)); int nl = first.indexOf('\n'); if (nl > 0) first = first.substring(0, nl);
                        String[] p = first.trim().split("\\s+"); if (p.length >= 3) { m.ns = p[2]; break; }
                    }
                }
            } else if ((e = z.getEntry("quilt.mod.json")) != null) {
                Map<String, Object> j = McDownload.obj(McDownload.Json.parse(readAll(z.getInputStream(e)))), q = j != null ? McDownload.obj(j.get("quilt_loader")) : null;
                if (q != null) {
                    m.loader = "Quilt"; m.id = s(q.get("id")); m.version = s(q.get("version"));
                    Map<String, Object> md = McDownload.obj(q.get("metadata")); if (md != null) m.name = s(md.get("name"));
                    List<Object> pr = McDownload.arr(q.get("provides")); if (pr != null) for (Object o : pr) { if (o instanceof String) m.provides.add((String) o); else { Map<String, Object> pm = McDownload.obj(o); if (pm != null) m.provides.add(s(pm.get("id"))); } }
                    List<Object> d = McDownload.arr(q.get("depends"));
                    if (d != null) for (Object o : d) {
                        String id; Object vers = null;
                        if (o instanceof String) id = (String) o; else { Map<String, Object> dm = McDownload.obj(o); if (dm == null) continue; id = s(dm.get("id")); vers = dm.get("versions"); if (Boolean.TRUE.equals(dm.get("optional"))) continue; }
                        if (id.equals("minecraft")) m.mcNeed = vers; else m.deps.add(id);
                    }
                }
            } else if ((e = z.getEntry("META-INF/neoforge.mods.toml")) != null) { m.loader = "NeoForge"; toml(m, readAll(z.getInputStream(e))); }
            else if ((e = z.getEntry("META-INF/mods.toml")) != null) { m.loader = "Forge"; toml(m, readAll(z.getInputStream(e))); }
            else if (z.getEntry("mcmod.info") != null) m.loader = "Forge";
            if (m.loader.equals("Forge") || m.loader.equals("NeoForge")) {   // jars nested in a Forge mod (META-INF/jarjar) are mods / libraries the loader sees too
                for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                    java.util.zip.ZipEntry ne = en.nextElement(); String nn = ne.getName();
                    if (!(nn.startsWith("META-INF/jarjar/") && nn.endsWith(".jar"))) continue;
                    try (java.util.zip.ZipInputStream zi = new java.util.zip.ZipInputStream(z.getInputStream(ne))) {
                        for (java.util.zip.ZipEntry ie; (ie = zi.getNextEntry()) != null; ) if (ie.getName().equals("META-INF/mods.toml") || ie.getName().equals("META-INF/neoforge.mods.toml")) { tomlSections(readAll(zi), m.provides, null); break; }
                    } catch (Throwable t) { }
                }
            }
        } catch (Throwable t) { }
        if (m.mcNeed != null) m.mcLabel = rangeLabel(m.mcNeed); else if (!m.tomlRange.isEmpty()) m.mcLabel = m.tomlRange;
        return m;
    }
    static String s(Object o) { return o instanceof String ? (String) o : ""; }
    /** just enough TOML: the first modId/displayName/version of the file and the minecraft dependency's versionRange */
    /** every [[mods]] modId (-> out) and every required dependency of mods.toml / neoforge.mods.toml (-> deps): a "[[dependencies.x]]" block with modId = "y" and mandatory = true (Forge) or type = "required" (NeoForge) */
    /** Maven range of mods.toml ("[1.20.1,1.21)", "[1.20,)", "[1.20.1]", "1.20.1") as the Fabric style range the rest of this class reads (">=1.20.1 <1.21") */
    static String mavenToFabric(String r) {
        r = r.trim(); if (r.isEmpty()) return "";
        if (r.charAt(0) != '[' && r.charAt(0) != '(') return ">=" + r;
        boolean loIn = r.charAt(0) == '[', hiIn = r.charAt(r.length() - 1) == ']'; String in = r.substring(1, r.length() - 1); int c = in.indexOf(',');
        if (c < 0) return in.trim();
        String lo = in.substring(0, c).trim(), hi = in.substring(c + 1).trim(); StringBuilder o = new StringBuilder();
        if (!lo.isEmpty()) o.append(loIn ? ">=" : ">").append(lo);
        if (!hi.isEmpty()) o.append(o.length() > 0 ? " " : "").append(hiIn ? "<=" : "<").append(hi);
        return o.toString();
    }
    static String tomlMcRange(String t) {   // the "minecraft" dependency's versionRange, whichever order its keys are in
        java.util.regex.Matcher h = java.util.regex.Pattern.compile("(?m)^\\s*\\[\\[\\s*dependencies[^\\]]*\\]\\]").matcher(t); List<Integer> at = new ArrayList<Integer>(); while (h.find()) at.add(h.end());
        for (int i = 0; i < at.size(); i++) {
            int end = t.indexOf("[[", at.get(i)); if (end < 0) end = t.length(); String body = t.substring(at.get(i), end);
            if (!java.util.regex.Pattern.compile("(?m)^\\s*modId\\s*=\\s*\"minecraft\"").matcher(body).find()) continue;
            java.util.regex.Matcher x = java.util.regex.Pattern.compile("(?m)^\\s*versionRange\\s*=\\s*[\"']([^\"']+)[\"']").matcher(body); if (x.find()) return x.group(1);
        }
        return "";
    }
    static void tomlSections(String t, List<String> ids, List<String> deps) {
        java.util.regex.Matcher h = java.util.regex.Pattern.compile("(?m)^\\s*\\[\\[\\s*([^\\]\\s]+)\\s*\\]\\]").matcher(t);
        List<int[]> at = new ArrayList<int[]>(); List<String> nm = new ArrayList<String>();
        while (h.find()) { at.add(new int[] { h.end() }); nm.add(h.group(1)); }
        for (int i = 0; i < at.size(); i++) {
            int end = i + 1 < at.size() ? t.indexOf("[[", at.get(i)[0]) : t.length(); if (end < 0) end = t.length();
            String body = t.substring(at.get(i)[0], end); java.util.regex.Matcher x = java.util.regex.Pattern.compile("(?m)^\\s*modId\\s*=\\s*\"([^\"]+)\"").matcher(body);
            if (!x.find()) continue; String id = x.group(1);
            if (nm.get(i).equals("mods")) ids.add(id);
            else if (nm.get(i).startsWith("dependencies") && deps != null) {
                boolean req = java.util.regex.Pattern.compile("(?m)^\\s*mandatory\\s*=\\s*true").matcher(body).find() || java.util.regex.Pattern.compile("(?m)^\\s*type\\s*=\\s*\"required\"").matcher(body).find();
                if (req && !deps.contains(id)) deps.add(id);
            }
        }
    }
    static void toml(ModInfo m, String t) {
        java.util.regex.Matcher x;
        if ((x = java.util.regex.Pattern.compile("(?m)^\\s*modId\\s*=\\s*\"([^\"]+)\"").matcher(t)).find()) m.id = x.group(1);
        if ((x = java.util.regex.Pattern.compile("(?m)^\\s*displayName\\s*=\\s*\"([^\"]+)\"").matcher(t)).find()) m.name = x.group(1);
        if ((x = java.util.regex.Pattern.compile("(?m)^\\s*version\\s*=\\s*\"([^\"]+)\"").matcher(t)).find()) m.version = x.group(1);
        { String mr = tomlMcRange(t); if (!mr.isEmpty()) { String fr = mavenToFabric(mr); if (!fr.isEmpty()) m.mcNeed = fr; } }
        { List<String> ids = new ArrayList<String>(); tomlSections(t, ids, m.deps); for (String i : ids) if (!i.equals(m.id)) m.provides.add(i); }
                if ((x = java.util.regex.Pattern.compile("modId\\s*=\\s*\"minecraft\"[^\\[]*?versionRange\\s*=\\s*\"([^\"]+)\"", java.util.regex.Pattern.DOTALL).matcher(t)).find()) m.tomlRange = maven(x.group(1));
    }
    /** "[1.20,1.21)" -> "1.20 - <1.21" */
    static String maven(String r) {
        r = r.trim(); if (r.length() < 3 || (r.charAt(0) != '[' && r.charAt(0) != '(')) return r;
        String in = r.substring(1, r.length() - 1); int c = in.indexOf(',');
        if (c < 0) return in;
        String lo = in.substring(0, c).trim(), hi = in.substring(c + 1).trim();
        if (hi.isEmpty()) return lo + "+";
        return (lo.isEmpty() ? "<" : lo + " - " + (r.endsWith(")") ? "<" : "")) + hi;
    }
    /** a Fabric/Quilt "minecraft" range as a short label: ">=1.20" -> "1.20+", ">=1.20 <1.21" -> "1.20 - <1.21", "~1.21" -> "1.21.x", "26.4-snapshot-4+" style kept */
    static String rangeLabel(Object need) {
        List<Object> alts = need instanceof List ? McDownload.arr(need) : Collections.singletonList(need);
        StringBuilder out = new StringBuilder();
        for (Object o : alts) {
            if (!(o instanceof String)) continue;
            for (String alt : ((String) o).split("\\|\\|")) {
                String lo = null, hi = null, other = "";
                for (String t : alt.trim().split("\\s+")) {
                    if (t.isEmpty()) continue;
                    if (t.startsWith(">=")) lo = t.substring(2); else if (t.startsWith(">")) lo = ">" + t.substring(1);
                    else if (t.startsWith("<=")) hi = t.substring(2); else if (t.startsWith("<")) hi = "<" + t.substring(1);
                    else if (t.startsWith("~") || t.startsWith("^")) { String v = t.substring(1); String[] p = v.split("\\."); other = t.startsWith("~") && p.length >= 2 ? p[0] + "." + p[1] + ".x" : v + "+"; }
                    else if (t.equals("*") || t.equalsIgnoreCase("x")) other = "any";
                    else other = t.startsWith("=") ? t.substring(1) : t;
                }
                String lab = lo != null && hi != null ? lo + " - " + hi : lo != null ? lo + "+" : hi != null ? (hi.startsWith("<") ? hi : "to " + hi) : other;
                if (!lab.isEmpty()) out.append(out.length() > 0 ? " / " : "").append(lab);
            }
        }
        String r = out.toString(); return r.length() > 28 ? r.substring(0, 27) + "\u2026" : r;
    }

    /** Mods that draw with OpenGL calls themselves: with Minecraft's Vulkan backend they call GL with no context and LWJGL aborts the whole app (seen: Iris -> glGetInteger). They are set aside for a Vulkan run and come back for an OpenGL one. */
    static final Set<String> GL_ONLY = new HashSet<String>(Arrays.asList("sodium", "iris", "embeddium", "oculus", "rubidium", "sodium-extra", "sodiumextra", "reeses-sodium-options", "reeses_sodium_options", "sodiumdynamiclights", "immediatelyfast", "distanthorizons", "nvidium", "indium", "canvas", "optifine", "optifabric"));
    /** Mods that replace the renderer with their own Vulkan one (VulkanMod): they need the Vulkan run (GLFW surface on the display, RADV) and fail in every OpenGL run. */
    static final Set<String> VK_ONLY = new HashSet<String>(Arrays.asList("vulkanmod"));
    /** Mods that take over mouse / controller input from the platform layer: Ixeris moves event polling to another thread, Controlify needs SDL3 natives (log 20453ee4: "Could not load SDL natives") and still hooks the mouse, scroll and look code. On the console the pad
     *  becomes mouse and keyboard in the shim, so with them the UI would not scroll, the camera would not turn and sliders could not be dragged. They are set aside and come back with the other settings. */
    static final Set<String> INPUT_HOSTILE = new HashSet<String>(Arrays.asList("ixeris", "controlify"));
    static int runJava() { String v = System.getProperty("java.specification.version", "17"); try { return v.startsWith("1.") ? Integer.parseInt(v.substring(2)) : Integer.parseInt(v); } catch (NumberFormatException e) { return 17; } }
    static final Set<String> BUILTIN_DEPS = new HashSet<String>(Arrays.asList("minecraft", "java", "javafml", "lowcodefml", "mclanguage", "neoforge", "forge", "fabricloader", "fabric-loader", "quilt_loader", "quilt_loader_dependency", "forge", "neoforge"));
    /** Why each mod jar of <gameDir>/mods and mods-disabled cannot run on Minecraft <mc> with <loader> (fabric|quilt), or null when it can: built for another loader, another Minecraft version, or a mod that needs one that cannot run. */
    /** The Fabric Loader version Quilt's compatibility layer (quilted_fabric_loader) reports to mods; mods asking for more stop Quilt with "requires version [0.16.10, inf) of fabricloader, which is missing" (log b5a02aa4). System property ps5.quilt.fabricloader overrides. */
    static final String QUILT_FABRICLOADER = System.getProperty("ps5.quilt.fabricloader", "0.14.22");
    static boolean quiltCannotProvide(Object range) {
        int[] have = mcParts(QUILT_FABRICLOADER); if (have == null) return false;
        List<Object> alts = range instanceof List ? McDownload.arr(range) : Collections.singletonList(range);
        for (Object a : alts) { if (!(a instanceof String)) return false; Boolean b = rangeOk((String) a, have); if (b == null || b) return false; }
        return !alts.isEmpty();
    }
    /** Minecraft version named in a jar's file name or version when its metadata gives no range: the first "+1.20.1" / "-1.21" / "mc1.20.1" token (not "-1.9" of a mod's own version) */
    static String fileMc(String name, String version) {
        java.util.regex.Pattern p = java.util.regex.Pattern.compile("(?:\\+|mc|-)(1\\.(?:1[4-9]|2[0-9])(?:\\.\\d+)?)(?![0-9])");
        for (String src : new String[] { name, version }) {
            if (src == null) continue;
            java.util.regex.Matcher m = p.matcher(src);
            while (m.find()) {
                // "fabric-language-kotlin-1.14.2+kotlin.2.4.21.jar": 1.14.2 is the mod's OWN version (log 92330f8f: it was set aside as "built for Minecraft 1.14.2", Zoomify then failed to set up without Kotlin and the 26.3 pack crashed)
                if (version != null && !version.isEmpty() && version.startsWith(m.group(1)) && src == name && m.start() > 0 && name.contains("-" + version)) continue;
                return m.group(1);
            }
        }
        return "";
    }
    /** a Minecraft range with no upper bound (">=1.20.1"): mods declare these for jars that only work on the version in their name (Accessories +1.20.1, Cloth Config, owo +1.20 crashed 1.21.1 with Mixin errors) */
    static boolean openEndedMc(Object need) {
        List<Object> alts = need instanceof List ? McDownload.arr(need) : Collections.singletonList(need);
        if (alts.isEmpty()) return false;
        for (Object a : alts) { String t = String.valueOf(a); if (!t.contains(">") || t.contains("<") || t.contains("||")) return false; }
        return true;
    }
    static boolean fileMcMismatch(String name, String version, int[] run) {
        String v = fileMc(name, version); if (v.isEmpty() || run == null || run.length < 2) return false;
        int[] j = mcParts(v); if (j == null || j.length < 2) return false;
        return j[0] != run[0] || j[1] != run[1];
    }
    static Map<File, String> fitPlan(String gameDir, String mc, String loader) {
        final int runJava = runJava();
        int[] run = mcParts(mc);
        boolean vulkanRun = "vulkan".equals(System.getProperty("ps5.renderer"));
        List<File> jars = new ArrayList<File>();
        for (String dir : new String[] { "mods", "mods-disabled" }) { File[] a = new File(gameDir, dir).listFiles((d, n) -> n.endsWith(".jar") || n.endsWith(".jar.disabled")); if (a != null) { Arrays.sort(a); jars.addAll(Arrays.asList(a)); } }
        boolean vitrail = false; for (File f : jars) if (f.getParentFile() != null && f.getParentFile().getName().equals("mods") && f.getName().toLowerCase().contains("vitrail")) vitrail = true;   // Vitrail Shaders (the shader engine of the game's own Vulkan renderer) lists Sodium as a dependency and lets Iris stay: on a Vulkan run they are kept for it, not set aside as OpenGL-only
        Map<File, ModInfo> info = new LinkedHashMap<File, ModInfo>(); Map<File, String> why = new LinkedHashMap<File, String>();
        for (File f : jars) {
            ModInfo m = readMod(f); info.put(f, m); String r = null;
            if (run != null && !m.ns.isEmpty() && (run[0] >= 26) != m.ns.equals("official")) r = run[0] >= 26 ? "built for Minecraft before 26 (" + m.ns + " names)" : "built for Minecraft 26+ (unobfuscated names)";
            else if (m.javaMajor > runJava) r = "needs Java " + m.javaMajor + " (this run is Java " + runJava + ")";
            else if (vulkanRun && GL_ONLY.contains(m.id) && !(vitrail && m.id.equals("sodium"))) r = "needs OpenGL (this run is Vulkan)";   // Vitrail keeps only Sodium (its dependency); Iris calls glGetInteger in its renderer init and the Vulkan backend has no GL context: "[LWJGL] No context is current" ended the whole app
            else if (INPUT_HOSTILE.contains(m.id)) r = "replaces the input layer the console pad drives (mouse / look / scroll)";
            else if (!vulkanRun && VK_ONLY.contains(m.id)) r = "needs Vulkan (this run is OpenGL)";
            else if ((loader.equals("forge") || loader.equals("neoforge")) && (m.loader.equals("Fabric") || m.loader.equals("Quilt") || (loader.equals("forge") && m.loader.equals("NeoForge")))) r = "built for " + m.loader;   // Forge stops the whole game: "Mod File yet_another_config_lib-fabric.jar needs language provider javafml" (log 458247f5)
            else if (loader.equals("fabric") && (m.loader.equals("Forge") || m.loader.equals("NeoForge") || m.loader.equals("Quilt"))) r = "built for " + m.loader;
            else if (loader.equals("quilt") && (m.loader.equals("Forge") || m.loader.equals("NeoForge"))) r = "built for " + m.loader;
            else if (loader.equals("quilt") && m.depRange.get("fabricloader") != null && quiltCannotProvide(m.depRange.get("fabricloader"))) r = "needs a newer Fabric Loader than Quilt provides (" + QUILT_FABRICLOADER + ")";
            else if (run != null && m.tomlRange.isEmpty() && (m.mcNeed == null || openEndedMc(m.mcNeed)) && fileMcMismatch(f.getName(), m.version, run)) r = "built for Minecraft " + fileMc(f.getName(), m.version);
            else if (run != null && m.mcNeed != null) {
                List<Object> alts = m.mcNeed instanceof List ? McDownload.arr(m.mcNeed) : Collections.singletonList(m.mcNeed);
                boolean ok = alts.isEmpty();
                for (Object o : alts) { if (!(o instanceof String)) { ok = true; break; } Boolean b = rangeOk((String) o, run); if (b == null || b) { ok = true; break; } }
                if (!ok) r = "needs Minecraft " + m.mcLabel;
            }
            log("fit: check " + f.getParentFile().getName() + "/" + f.getName() + " id=" + m.id + " " + m.version + " mc=" + (m.mcNeed == null ? (m.tomlRange.isEmpty() ? "?" : m.tomlRange) : m.mcLabel) + " running " + mc + " -> " + (r == null ? "fits" : r));
            if (r != null) why.put(f, r);
        }
        for (boolean changed = true; changed; ) {   // a mod that needs a mod which cannot run cannot run either
            changed = false;
            Set<String> live = new HashSet<String>(), dead = new HashSet<String>();
            Map<String, String> ver = new HashMap<String, String>(); for (File f : jars) { ModInfo m = info.get(f); if (!why.containsKey(f) && !f.getName().endsWith(".disabled") && !m.id.isEmpty() && !m.version.isEmpty()) ver.put(m.id, m.version); }
            for (File f : jars) { if (f.getName().endsWith(".disabled")) continue; ModInfo m = info.get(f); Set<String> ids = new HashSet<String>(m.provides); if (!m.id.isEmpty()) ids.add(m.id); (why.containsKey(f) ? dead : live).addAll(ids); }
            for (File f : jars) {
                if (why.containsKey(f)) continue;
                for (String d : info.get(f).deps) if (!BUILTIN_DEPS.contains(d) && dead.contains(d) && !live.contains(d)) { why.put(f, "needs " + d); changed = true; break; }
                if (why.containsKey(f)) continue;
                for (Map.Entry<String, Object> dr : info.get(f).depRange.entrySet()) {   // the mod is there but in a version the dependent refuses (Accessories beta.49 wants Cloth Config 11.1.118, the pack has 11.1.106): Fabric would stop the whole game
                    String have = ver.get(dr.getKey()); int[] hv = have == null ? null : mcParts(have.split("\\+")[0]); if (hv == null || BUILTIN_DEPS.contains(dr.getKey())) continue;
                    Object o = dr.getValue(); List<Object> alts = o instanceof List ? McDownload.arr(o) : Collections.singletonList(o); boolean ok = alts.isEmpty();
                    for (Object a : alts) { if (!(a instanceof String)) { ok = true; break; } Boolean b = rangeOk((String) a, hv); if (b == null || b) { ok = true; break; } }
                    if (!ok) { why.put(f, "needs " + dr.getKey() + " " + dr.getValue() + " but " + have + " is installed"); changed = true; break; }
                }
            }
        }
        return why;
    }
    /** Before a Fabric/Quilt launch: jars that cannot run on <mc> / <loader> move to <gameDir>/mods-disabled, and jars there that can run move back. Jars that say nothing, or cannot be read, are left alone. */
    /** Mods that bundle LWJGL as jar-in-jar (VulkanMod ships lwjgl-vulkan / vma / shaderc 3.4.1) clash with the title's own LWJGL 3.4 jars, which live in the shared child-first loader next to their natives:
     *  Knot loads the mod's copy of org.lwjgl.vulkan.VkInstance itself while GLFWVulkan (shared loader) wants its own -> "LinkageError: loader constraint violation ... VkInstance" (log 0f61b927, 26.1 + VulkanMod).
     *  The nested LWJGL jars are taken out of the mod (and out of its fabric.mod.json "jars" list); the original stays in <game>/mods-original. Only for Minecraft 26+ (LWJGL 3.4), idempotent. */
    static void stripBundledLwjgl(String gameDir) {
        for (String dir : new String[] { "mods", "mods-disabled" }) {
            File[] a = new File(gameDir, dir).listFiles((d, n) -> n.endsWith(".jar"));
            if (a == null) continue;
            for (File f : a) { try { stripBundledLwjgl1(f, gameDir); } catch (Throwable t) { log("lwjgl: could not check " + f.getName() + ": " + t); } }
        }
    }
    private static void stripBundledLwjgl1(File jar, String gameDir) throws IOException {
        List<String> drop = new ArrayList<String>();
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(jar)) {
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> e = z.entries(); e.hasMoreElements(); ) {
                String n = e.nextElement().getName(), l = n.toLowerCase();
                if (l.startsWith("meta-inf/jars/") && l.endsWith(".jar") && l.contains("lwjgl")) drop.add(n);
            }
        }
        if (drop.isEmpty()) return;
        File bk = new File(gameDir, "mods-original"); bk.mkdirs();
        File bf = new File(bk, jar.getName()); if (!bf.exists()) Files.copy(jar.toPath(), bf.toPath());
        File tmp = new File(jar.getPath() + ".tmp");
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(jar); java.util.zip.ZipOutputStream o = new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(tmp))) {
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> e = z.entries(); e.hasMoreElements(); ) {
                java.util.zip.ZipEntry en = e.nextElement(); String n = en.getName();
                if (drop.contains(n)) continue;
                byte[] data; try (InputStream in = z.getInputStream(en)) { ByteArrayOutputStream bo = new ByteArrayOutputStream(); byte[] x = new byte[16384]; int k; while ((k = in.read(x)) > 0) bo.write(x, 0, k); data = bo.toByteArray(); }
                if (n.equals("fabric.mod.json")) {
                    String js = new String(data, java.nio.charset.StandardCharsets.UTF_8);
                    for (String d : drop) js = js.replaceAll("\\{\\s*\"file\"\\s*:\\s*\"" + java.util.regex.Pattern.quote(d) + "\"\\s*\\}", "");
                    js = js.replaceAll(",(\\s*,)+", ",").replaceAll("\\[\\s*,", "[").replaceAll(",\\s*\\]", "]");
                    McDownload.Json.parse(js);   // still valid JSON, or the whole rewrite is dropped
                    data = js.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                }
                java.util.zip.ZipEntry ne = new java.util.zip.ZipEntry(n); if (en.getTime() > 0) ne.setTime(en.getTime());
                o.putNextEntry(ne); o.write(data); o.closeEntry();
            }
        } catch (Throwable t) { tmp.delete(); throw t instanceof IOException ? (IOException) t : new IOException(t); }
        Files.delete(jar.toPath()); Files.move(tmp.toPath(), jar.toPath());
        log("lwjgl: " + jar.getName() + " bundled " + drop.size() + " LWJGL jar(s) (" + drop + "): taken out, the title's LWJGL 3.4 is used (original kept in mods-original/)");
    }
    public static void fitMods(String gameDir, String mc, String loader) {
        if (loader == null || loader.isEmpty()) loader = "fabric";
        { int[] rp = mcParts(mc); if (rp != null && rp[0] >= 26) stripBundledLwjgl(gameDir); }
        if (mcParts(mc) == null) log("fit: cannot compare against Minecraft '" + mc + "', only the loader and dependencies are checked");
        Map<File, String> why = fitPlan(gameDir, mc, loader);
        File mods = new File(gameDir, "mods"), off = new File(gameDir, "mods-disabled");
        int out = 0, in = 0, kept = 0;
        File[] b = off.listFiles((d, n) -> n.endsWith(".jar"));   // what was already set aside before this run: only those can come back
        for (Map.Entry<File, String> e : why.entrySet()) {   // the plan lists every jar only through its reason: jars without one are looked up below
            File f = e.getKey();
            if (f.getParentFile().getName().equals("mods") && f.getName().endsWith(".jar")) { if (move(f, new File(off, f.getName()))) { out++; log("fit: set aside " + f.getName() + " (" + e.getValue() + ", running " + mc + ")"); } }
        }
        Set<String> blocked = crashBlocked(gameDir);   // jars a crash report named: they stay aside until the user puts them back
        if (b != null) { Arrays.sort(b); for (File f : b) { if (why.containsKey(f) || blocked.contains(f.getName())) continue; if (move(f, new File(mods, f.getName()))) { in++; log("fit: restored " + f.getName() + " (fits " + mc + ")"); } } }
        File[] k = mods.listFiles((d, n) -> n.endsWith(".jar")); kept = k == null ? 0 : k.length;
        log("fit: Minecraft " + mc + " / " + loader + ": " + kept + " mods run, " + out + " set aside, " + in + " restored");
    }
    public static void fitMods(String gameDir, String mc) { fitMods(gameDir, mc, "fabric"); }
    static File blockFile(String gameDir) { return new File(gameDir, "mods-disabled/.crash_blocked"); }
    static Set<String> crashBlocked(String gameDir) {
        Set<String> s = new HashSet<String>();
        try { File f = blockFile(gameDir); if (f.isFile()) for (String l : java.nio.file.Files.readAllLines(f.toPath(), java.nio.charset.StandardCharsets.UTF_8)) if (!l.trim().isEmpty()) s.add(l.trim()); } catch (Throwable t) { }
        return s;
    }
    /** A crash report names the mod whose mixin could not be applied (Controlify's accumulateScrolling against another mod's class, ...): that jar moves to mods-disabled and stays there, so the next Play starts without it
     *  instead of crashing again. Returns the jar names set aside. Libraries (Fabric API, MixinExtras, the loader) are never blamed. */
    public static List<String> blameCrash(String gameDir, String report) {
        List<String> out = new ArrayList<String>();
        try {
            Set<String> cfgs = new LinkedHashSet<String>(), ids = new LinkedHashSet<String>(), prov = new LinkedHashSet<String>();
            java.util.regex.Pattern pc = java.util.regex.Pattern.compile("([A-Za-z0-9_.\\-]+\\.mixins?\\.json)"), pm = java.util.regex.Pattern.compile("from mod ([A-Za-z0-9_\\-]+)"), pp = java.util.regex.Pattern.compile("provided by '([^']+)'");
            for (String l : report.split("\n")) {
                boolean mixin = l.contains("InjectionError") || l.contains("Critical injection") || l.contains("MixinApplyError") || l.contains("Mixin apply") || l.contains("MixinTransformerError") || l.contains("InvalidInjectionException")
                    || l.contains("Failed to validate sugar") || l.contains("Mixin prepare") || l.contains("InvalidMixinException") || l.contains("Callback method") || l.contains("Mixin config") || l.contains("mixin config");
                if (mixin) { java.util.regex.Matcher m = pc.matcher(l); while (m.find()) cfgs.add(m.group(1)); m = pm.matcher(l); while (m.find()) ids.add(m.group(1)); }
                java.util.regex.Matcher m2 = pp.matcher(l); while (m2.find()) prov.add(m2.group(1));
            }
            File[] jars = new File(gameDir, "mods").listFiles((d, n) -> n.endsWith(".jar")); if (jars == null) return out;
            Set<String> hit = new LinkedHashSet<String>(); File off = new File(gameDir, "mods-disabled");
            for (int pass = 0; pass < 2 && hit.isEmpty(); pass++)
                for (File f : jars) {
                    ModInfo mi = readMod(f); String id = mi.id; if (id.isEmpty() || id.startsWith("fabric") || id.equals("mixinextras") || id.equals("minecraft") || id.startsWith("quilt")) continue;
                    boolean b = false;
                    if (pass == 0) { b = ids.contains(id); if (!b) try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(f)) { for (String c : cfgs) if (z.getEntry(c) != null) { b = true; break; } } catch (Throwable t) { } }
                    else b = prov.contains(id);   // no mixin named: the mod whose entrypoint failed
                    if (b) hit.add(f.getName());
                }
            if (hit.isEmpty()) return out;
            off.mkdirs();
            Set<String> blocked = crashBlocked(gameDir);
            for (String n : hit) { if (move(new File(gameDir, "mods/" + n), new File(off, n))) { out.add(n); blocked.add(n); log("crash: set aside " + n + " (the crash report names it); it stays in mods-disabled"); } }
            java.nio.file.Files.write(blockFile(gameDir).toPath(), String.join("\n", blocked).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (Throwable t) { log("crash: blame failed: " + t); }
        return out;
    }
    static boolean move(File from, File to) {
        try { to.getParentFile().mkdirs(); if (to.exists()) { if (to.length() == from.length()) { Files.delete(from.toPath()); log("fit: " + from.getName() + " is already in place, duplicate removed"); return true; } Files.move(from.toPath(), to.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING); log("fit: " + from.getName() + " replaced the different copy in " + to.getParentFile().getName()); return true; } Files.move(from.toPath(), to.toPath()); return true; }
        catch (Throwable t) { log("fit: cannot move " + from.getName() + ": " + t); return false; }
    }
    static String readAll(InputStream in) throws IOException { try (ByteArrayOutputStream b = new ByteArrayOutputStream()) { byte[] x = new byte[8192]; int n; while ((n = in.read(x)) > 0) b.write(x, 0, n); return b.toString("UTF-8"); } }
    /** "1.20.1" -> {1,20,1}; snapshots and other ids -> null */
    static int[] mcParts(String v) {
        if (v == null || !v.matches("\\d+(\\.\\d+)*(-.*)?")) return null;
        String[] p = v.split("-")[0].split("\\."); int[] r = new int[p.length]; for (int i = 0; i < p.length; i++) r[i] = Integer.parseInt(p[i]); return r;
    }
    static int cmp(int[] a, int[] b) { for (int i = 0; i < Math.max(a.length, b.length); i++) { int x = i < a.length ? a[i] : 0, y = i < b.length ? b[i] : 0; if (x != y) return x < y ? -1 : 1; } return 0; }
    /** Fabric/Quilt version range: predicates joined by spaces are all required, "||" separates alternatives; operators >= <= > < = ~ ^, "1.20.x" wildcards, "*". null = cannot tell (treated as a fit) */
    static Boolean rangeOk(String range, int[] run) {
        boolean unknown = false;
        for (String alt : range.split("\\|\\|")) {
            boolean all = true, any = false;
            for (String t : alt.trim().split("\\s+")) { if (t.isEmpty()) continue; any = true; Boolean b = predicate(t, run); if (b == null) { unknown = true; all = false; break; } if (!b) { all = false; break; } }
            if (any && all) return true;
            if (!any) return true;
        }
        return unknown ? null : Boolean.FALSE;
    }
    static Boolean predicate(String t, int[] run) {
        if (t.equals("*") || t.equals("x") || t.equals("X")) return true;
        String op = ""; int i = 0; while (i < t.length() && "<>=~^".indexOf(t.charAt(i)) >= 0) i++; op = t.substring(0, i); String v = t.substring(i);
        boolean wild = false; List<Integer> parts = new ArrayList<>();
        for (String s : v.split("-")[0].split("\\.")) { if (s.equals("x") || s.equals("X") || s.equals("*")) { wild = true; break; } if (!s.matches("\\d+")) return null; parts.add(Integer.parseInt(s)); }
        if (parts.isEmpty()) return null;
        int[] p = new int[parts.size()]; for (int k = 0; k < p.length; k++) p[k] = parts.get(k);
        if (wild || (op.isEmpty() && p.length < 3)) { for (int k = 0; k < p.length; k++) if ((k < run.length ? run[k] : 0) != p[k]) return false; return true; }   // 1.20.x, bare 1.20: any release of that line
        switch (op) {
            case "": case "=": return cmp(run, p) == 0;
            case ">=": return cmp(run, p) >= 0;
            case ">": return cmp(run, p) > 0;
            case "<=": return cmp(run, p) <= 0;
            case "<": return cmp(run, p) < 0;
            case "~": { int[] hi = p.length >= 2 ? new int[] { p[0], p[1] + 1 } : new int[] { p[0] + 1 }; return cmp(run, p) >= 0 && cmp(run, hi) < 0; }
            case "^": { int[] hi = new int[] { p[0] + 1 }; return cmp(run, p) >= 0 && cmp(run, hi) < 0; }
            default: return null;
        }
    }

    /** folder (relative to the game folder) a project type installs into */
    static String typeDir(String projectType, String gameDir) throws IOException {
        switch (projectType) {
            case "resourcepack": return "resourcepacks";
            case "shader": return "shaderpacks";
            case "datapack": return "datapacks";   // the instance's library; the Library tab switches a pack on per world (copies it into saves/<world>/datapacks)
            default: return "mods";
        }
    }

    /** search Modrinth; returns "slug  title  (downloads)" lines */
    public static List<String> search(String query, String projectType, String mc, String loader, int limit) throws IOException {
        StringBuilder facets = new StringBuilder("[[\"project_type:" + projectType + "\"]");
        if (mc != null && !mc.isEmpty()) facets.append(",[\"versions:").append(mc).append("\"]");
        if (loader != null && !loader.isEmpty() && projectType.equals("mod")) facets.append(",[\"categories:").append(loader).append("\"]");
        facets.append("]");
        Map<String, Object> r = McDownload.obj(getJson(modrinth() + "/v2/search?query=" + enc(query) + "&limit=" + limit + "&facets=" + enc(facets.toString())));
        List<String> out = new ArrayList<>();
        List<Object> hits = McDownload.arr(r.get("hits")); if (hits != null) for (Object h : hits) { Map<String, Object> m = McDownload.obj(h); out.add(McDownload.str(m.get("slug")) + "  " + McDownload.str(m.get("title")) + "  (" + McDownload.num(m.get("downloads")) + " downloads)"); }
        return out;
    }

    /** search rows for the native launcher: slug, title, author, downloads, one-line description (tabs and newlines removed) */
    public static List<String[]> searchRows(String query, String projectType, String mc, String loader, int limit) throws IOException {
        StringBuilder facets = new StringBuilder("[[\"project_type:" + projectType + "\"]");
        if (mc != null && !mc.isEmpty()) facets.append(",[\"versions:").append(mc).append("\"]");
        if (loader != null && !loader.isEmpty() && projectType.equals("mod")) facets.append(",[\"categories:").append(loader).append("\"]");
        facets.append("]");
        Map<String, Object> r = McDownload.obj(getJson(modrinth() + "/v2/search?query=" + enc(query) + "&limit=" + limit + "&index=downloads&facets=" + enc(facets.toString())));
        List<String[]> out = new ArrayList<>();
        List<Object> hits = McDownload.arr(r.get("hits"));
        if (hits != null) for (Object h : hits) {
            Map<String, Object> m = McDownload.obj(h); if (m == null) continue;
            StringBuilder lo = new StringBuilder(); List<Object> cats = McDownload.arr(m.get("categories"));
            if (cats != null && projectType.equals("mod")) for (String k : new String[] { "fabric", "quilt", "forge", "neoforge" }) if (cats.contains(k)) lo.append(lo.length() > 0 ? "," : "").append(k);
            List<Object> vs = McDownload.arr(m.get("versions")); String span = "";
            if (vs != null && !vs.isEmpty()) { String f0 = String.valueOf(vs.get(0)), f1 = String.valueOf(vs.get(vs.size() - 1)); span = f0.equals(f1) ? f0 : f0 + " - " + f1; }
            out.add(new String[] { clean(McDownload.str(m.get("slug"))), clean(McDownload.str(m.get("title"))), clean(McDownload.str(m.get("author"))), String.valueOf(McDownload.num(m.get("downloads"))), clean(McDownload.str(m.get("description"))), lo.toString(), clean(span) });
        }
        return out;
    }
    /** ids (and "provides") of the fabric.mod.json files inside a nested jar, and of the jars nested in that one */
    static void nestedIds(InputStream in, List<String> out, int depth) {
        try {
            java.util.zip.ZipInputStream zi = new java.util.zip.ZipInputStream(in);
            for (java.util.zip.ZipEntry e; (e = zi.getNextEntry()) != null; ) {
                String n = e.getName();
                if (n.equals("fabric.mod.json")) {
                    Map<String, Object> j = McDownload.obj(McDownload.Json.parse(readAll(zi)));
                    if (j != null) { String id = s(j.get("id")); if (!id.isEmpty()) out.add(id); List<Object> pr = McDownload.arr(j.get("provides")); if (pr != null) for (Object o : pr) if (o instanceof String) out.add((String) o); }
                } else if (depth < 3 && n.startsWith("META-INF/jars/") && n.endsWith(".jar")) nestedIds(zi, out, depth + 1);
            }
        } catch (Throwable t) { }
    }
    static String norm(String x) { return x == null ? "" : x.toLowerCase(java.util.Locale.ROOT).replaceAll("[^a-z0-9]", ""); }
    /** mod ids that are not the Modrinth slug */
    static final Map<String, String> DEP_SLUG = new HashMap<String, String>();
    static { DEP_SLUG.put("owo", "owo-lib"); DEP_SLUG.put("fabric", "fabric-api"); DEP_SLUG.put("fabric-api", "fabric-api"); DEP_SLUG.put("fabricloader", ""); DEP_SLUG.put("yet_another_config_lib_v3", "yacl"); DEP_SLUG.put("cloth_config", "cloth-config"); DEP_SLUG.put("kotlinforforge", "kotlin-for-forge"); DEP_SLUG.put("playeranimator", "player-animator"); DEP_SLUG.put("geckolib", "geckolib"); }
    /** the Modrinth project that ships mod id <id>: the slug of the same name, else the search hit whose slug or title is that name (or the shortest slug containing it); null when there is none */
    static String projectFor(String id, String mc, String loader) {
        String slug = DEP_SLUG.get(id); if (slug != null && !slug.isEmpty()) return slug;
        try { Map<String, Object> p = McDownload.obj(getJson(modrinth() + "/v2/project/" + enc(id))); if (p != null && "mod".equals(McDownload.str(p.get("project_type")))) return McDownload.str(p.get("slug")); } catch (Throwable t) { }
        try {
            StringBuilder f = new StringBuilder("[[\"project_type:mod\"]"); if (mc != null && !mc.isEmpty()) f.append(",[\"versions:").append(mc).append("\"]"); if (loader != null && !loader.isEmpty()) f.append(",[\"categories:").append(loader).append("\"]"); f.append("]");
            Map<String, Object> r = McDownload.obj(getJson(modrinth() + "/v2/search?limit=10&query=" + enc(id) + "&facets=" + enc(f.toString())));
            List<Object> hits = r == null ? null : McDownload.arr(r.get("hits")); if (hits == null) return null;
            String want = norm(id), best = null;
            for (Object h : hits) { Map<String, Object> m = McDownload.obj(h); if (m == null) continue; String sl = McDownload.str(m.get("slug")); if (sl == null) continue;
                if (norm(sl).equals(want) || norm(McDownload.str(m.get("title"))).equals(want)) return sl;
                if (want.length() >= 3 && norm(sl).contains(want) && (best == null || sl.length() < best.length())) best = sl; }
            return best;
        } catch (Throwable t) { return null; }
    }
    /** Installs what the jars in <gameDir>/mods say they need (their own fabric.mod.json "depends", which Modrinth's dependency list often lacks: Accessories needs Cloth Config) and nothing in the folder provides,
     *  nested jars counted. Up to 4 rounds, because a new mod brings its own needs. Never throws; returns the files installed. */
    /** Logs what the Fabric / Quilt loader will see: every jar with its id, version and the dependencies it declares, and which of those nothing in the folder provides (the loaders' own error screens cannot be shown here). */
    public static void listMods(String gameDir) {
        try {
            File[] jars = new File(gameDir, "mods").listFiles((d, n) -> n.endsWith(".jar")); if (jars == null) return;
            java.util.Arrays.sort(jars);
            Set<String> have = new HashSet<String>(); List<ModInfo> infos = new ArrayList<ModInfo>();
            for (File f : jars) { ModInfo m = readMod(f); infos.add(m); if (!m.id.isEmpty()) have.add(m.id); have.addAll(m.provides); }
            for (int i = 0; i < jars.length; i++) {
                ModInfo m = infos.get(i); StringBuilder miss = new StringBuilder();
                for (String d : m.deps) if (!d.isEmpty() && !BUILTIN_DEPS.contains(d) && !have.contains(d)) miss.append(miss.length() > 0 ? "," : "").append(d);
                log("mods: " + jars[i].getName() + " = " + m.id + " " + m.version + " (" + m.loader + ", mc " + (m.mcNeed == null ? "?" : m.mcNeed) + ", java " + m.javaMajor + ") needs " + m.depRange + (miss.length() > 0 ? "  MISSING: " + miss : ""));
            }
        } catch (Throwable t) { log("mods: list failed: " + t); }
    }
    public static List<String> fixDeps(String gameDir, String mc, String loader) {
        List<String> got = new ArrayList<String>(); Set<String> tried = new HashSet<String>();
        try {
            for (int round = 0; round < 4; round++) {
                File[] jars = new File(gameDir, "mods").listFiles((d, n) -> n.endsWith(".jar")); if (jars == null) break;
                Set<String> have = new HashSet<String>(); List<ModInfo> infos = new ArrayList<ModInfo>();
                for (File f : jars) { ModInfo m = readMod(f); infos.add(m); if (!m.id.isEmpty()) have.add(m.id); have.addAll(m.provides); }
                Set<String> missing = new LinkedHashSet<String>();
                for (ModInfo m : infos) for (String d : m.deps) if (!d.isEmpty() && !BUILTIN_DEPS.contains(d) && !have.contains(d) && !tried.contains(d)) missing.add(d);
                if (!tried.contains("indium") && !have.contains("indium") && have.contains("sodium") && have.contains("fabric-renderer-api-v1")) {   // Sodium before 0.6 replaces Fabric's renderer: mods that use the renderer API (Aether: RendererAccess.getRenderer() == null, log 3981c9d6) need Indium next to it
                    int[] sv = null; for (ModInfo m : infos) if (m.id.equals("sodium")) sv = mcParts(m.version.split("\\+")[0]);
                    if (sv != null && cmp(sv, new int[] { 0, 6 }) < 0) missing.add("indium");
                }
                if (loader.equals("quilt") && jars.length > 0 && !have.contains("mixinextras") && !tried.contains("mixinextras")) {   // log 37f27117: Sodium/Iris use MixinExtras ("Error loading class ... wrapoperation/Operation", the game dies in the first mixin); Fabric Loader 0.15+ brings it, Quilt Loader does not
                    tried.add("mixinextras");
                    try { McDownload.Task t = new McDownload.Task("https://repo1.maven.org/maven2/io/github/llamalad7/mixinextras-fabric/0.4.1/mixinextras-fabric-0.4.1.jar", new File(gameDir, "mods/mixinextras-fabric-0.4.1.jar"), null, 0, "mixinextras"); if (!t.present()) McDownload.fetch(t); relaxLoaderDep(t.dest); got.add("mods/mixinextras-fabric-0.4.1.jar"); log("deps: mixinextras 0.4.1 (Quilt does not bundle it; from Maven Central, Modrinth has no such project) -> mods/mixinextras-fabric-0.4.1.jar"); } catch (Throwable t) { log("deps: mixinextras: " + t.getMessage()); }
                }
                if (missing.isEmpty()) break;
                for (String id : missing) {
                    tried.add(id);
                    try {
                        String slug = projectFor(id, mc, loader);
                        if (slug == null) { log("deps: " + id + " is needed by an installed mod but is not on Modrinth for " + mc); continue; }
                        List<String> f = new ArrayList<String>(); installOne(slug, "mod", mc, loader, gameDir, f, new HashSet<String>(), 0); got.addAll(f); log("deps: " + id + " (needed by an installed mod) -> " + f);
                    } catch (Throwable t) { log("deps: " + id + ": " + t.getMessage()); }
                }
            }
        } catch (Throwable t) { log("deps: " + t); }
        return got;
    }
    /** log 5825a98e: MixinExtras 0.4.1 asks for fabricloader >= 0.14.25 and Quilt Loader 0.20 emulates 0.14.22: "MixinExtras requires version [0.14.25, inf) of fabricloader, which is missing". It only needs Mixin, so the
     *  loader requirement in its fabric.mod.json becomes "*" (the jar is rewritten once; nothing to do when it already says that). */
    static void relaxLoaderDep(File jar) {
        try {
            java.util.zip.ZipFile z = new java.util.zip.ZipFile(jar); java.util.zip.ZipEntry e = z.getEntry("fabric.mod.json"); if (e == null) { z.close(); return; }
            byte[] b = new byte[(int) e.getSize()]; java.io.InputStream in = z.getInputStream(e); int o = 0; while (o < b.length) { int r = in.read(b, o, b.length - o); if (r < 0) break; o += r; } in.close();
            String js = new String(b, "UTF-8"), nj = js.replaceAll("\"fabricloader\"\\s*:\\s*\"[^\"]*\"", "\"fabricloader\": \"*\""); if (nj.equals(js)) { z.close(); return; }
            File tmp = new File(jar.getPath() + ".tmp"); java.util.zip.ZipOutputStream out = new java.util.zip.ZipOutputStream(new BufferedOutputStream(new FileOutputStream(tmp)));
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                java.util.zip.ZipEntry x = en.nextElement(); out.putNextEntry(new java.util.zip.ZipEntry(x.getName())); 
                if (x.getName().equals("fabric.mod.json")) out.write(nj.getBytes("UTF-8")); else if (!x.isDirectory()) { java.io.InputStream i2 = z.getInputStream(x); byte[] buf = new byte[65536]; int n; while ((n = i2.read(buf)) > 0) out.write(buf, 0, n); i2.close(); }
                out.closeEntry(); }
            out.close(); z.close(); if (!tmp.renameTo(jar)) { jar.delete(); tmp.renameTo(jar); }
            log("deps: " + jar.getName() + ": fabricloader requirement relaxed (Quilt provides 0.14.22)");
        } catch (Throwable t) { log("deps: relax " + jar.getName() + ": " + t); }
    }
    static String clean(String s) { return s == null ? "" : s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim(); }

    /** downloads the newest compatible version of a Modrinth project (and its required dependencies) into <gameDir>/mods|resourcepacks|shaderpacks; returns the file names */
    public static List<String> install(String slugOrId, String projectType, String mc, String loader, String gameDir) throws Exception {
        List<String> files = new ArrayList<>();
        installOne(slugOrId, projectType, mc, loader, gameDir, files, new HashSet<String>(), 0);
        if (projectType.equals("mod") && loader != null && !loader.isEmpty()) files.addAll(fixDeps(gameDir, mc, loader));
        return files;
    }
    /** Installs a Modrinth modpack (.mrpack) as its own instance: the pack's Fabric/Quilt loader, every client file from modrinth.index.json, then overrides/ and client-overrides/. Returns the instance id.
     *  Works for any pack and Minecraft version; Forge/NeoForge packs are refused. */
    public static String installPack(String root, String slug, String mc) throws Exception {
        Map<String, Object> proj = McDownload.obj(getJson(modrinth() + "/v2/project/" + enc(slug)));
        if (!"modpack".equals(McDownload.str(proj.get("project_type")))) throw new IOException(slug + " is not a modpack");
        String q = mc != null && !mc.isEmpty() ? "?game_versions=" + enc("[\"" + mc + "\"]") : "";
        List<Object> vers = McDownload.arr(getJson(modrinth() + "/v2/project/" + enc(slug) + "/version" + q));
        if (vers == null || vers.isEmpty()) throw new IOException(slug + ": no version for Minecraft " + mc);
        List<Object> fl = McDownload.arr(McDownload.obj(vers.get(0)).get("files")); Map<String, Object> file = null;
        for (Object o : fl) { Map<String, Object> m = McDownload.obj(o); if (Boolean.TRUE.equals(m.get("primary"))) { file = m; break; } }
        if (file == null) file = McDownload.obj(fl.get(0));
        String fname = McDownload.str(file.get("filename"));
        if (fname == null || fname.contains("/") || fname.contains("\\") || fname.startsWith(".")) throw new IOException("bad file name " + fname);
        Map<String, Object> hashes = McDownload.obj(file.get("hashes"));
        File pack = new File(root, "downloads/" + fname);
        McDownload.fetch(new McDownload.Task(McDownload.str(file.get("url")), pack, hashes != null ? McDownload.str(hashes.get("sha1")) : null, McDownload.num(file.get("size")), slug));
        String id = Launcher.clean(slug);
        Launcher.Instance inst = Launcher.instance(root, id);
        String gameDir = Launcher.gameDirOf(inst, root);
        File base = new File(gameDir).getCanonicalFile();
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(pack)) {
            java.util.zip.ZipEntry ie = z.getEntry("modrinth.index.json");
            if (ie == null) throw new IOException(fname + " has no modrinth.index.json");
            Map<String, Object> idx = McDownload.obj(McDownload.Json.parse(readAll(z.getInputStream(ie))));
            Map<String, Object> deps = McDownload.obj(idx.get("dependencies"));
            String mcv = deps == null ? null : McDownload.str(deps.get("minecraft"));
            if (mcv == null) throw new IOException("the pack names no Minecraft version");
            String kind = "", lv = null;
            for (String k : new String[] { "fabric", "quilt" }) if (deps.get(k + "-loader") != null) { kind = k; lv = McDownload.str(deps.get(k + "-loader")); }
            if (kind.isEmpty() && (deps.get("forge") != null || deps.get("neoforge") != null)) throw new IOException("this pack needs " + (deps.get("neoforge") != null ? "NeoForge" : "Forge") + "; only Fabric and Quilt packs run here");
            String prof = mcv;
            if (!kind.isEmpty()) { prof = installLoader(root, kind, mcv, null);   /* newest loader, not the pack's: loaders stay compatible and mods added later may need a newer one (Veinminer wants 0.15) */ }
            List<McDownload.Task> tasks = new ArrayList<>();
            List<Object> files = McDownload.arr(idx.get("files"));
            if (files != null) for (Object o : files) {
                Map<String, Object> m = McDownload.obj(o); if (m == null) continue;
                Map<String, Object> env = McDownload.obj(m.get("env"));
                if (env != null && "unsupported".equals(McDownload.str(env.get("client")))) continue;
                List<Object> dl = McDownload.arr(m.get("downloads")); Map<String, Object> hh = McDownload.obj(m.get("hashes"));
                if (dl == null || dl.isEmpty()) continue;
                tasks.add(new McDownload.Task(String.valueOf(dl.get(0)), packPath(base, McDownload.str(m.get("path"))), hh != null ? McDownload.str(hh.get("sha1")) : null, McDownload.num(m.get("fileSize")), McDownload.str(m.get("path"))));
            }
            java.util.Set<String> keep = new java.util.HashSet<String>(); for (McDownload.Task t : tasks) keep.add(t.dest.getPath());
            File[] old = new File(base, "mods").listFiles(); int moved = 0;   // a pack is the whole mod set: jars left from earlier tries or added by hand (other Sodium/Iris versions) stop the game, so they step aside (mods-removed/)
            if (old != null) for (File f : old) if (f.isFile() && f.getName().endsWith(".jar") && !keep.contains(f.getPath())) { File rd = new File(base, "mods-removed"); rd.mkdirs(); Files.move(f.toPath(), new File(rd, f.getName()).toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING); moved++; }
            if (moved > 0) log("modpack " + slug + ": " + moved + " mods that are not part of the pack moved to mods-removed/");
            int bad = McDownload.runAll("modpack " + slug, tasks);
            if (bad > 0) throw new IOException(bad + " pack files failed to download");
            for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                java.util.zip.ZipEntry e = en.nextElement(); String n = e.getName(); String rel = n.startsWith("overrides/") ? n.substring(10) : n.startsWith("client-overrides/") ? n.substring(17) : null;
                if (rel == null || rel.isEmpty() || e.isDirectory()) continue;
                File d = packPath(base, rel); d.getParentFile().mkdirs();
                try (InputStream in = z.getInputStream(e)) { Files.copy(in, d.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING); }
            }
            inst.p.setProperty("name", McDownload.str(proj.get("title")) != null ? McDownload.str(proj.get("title")) : slug);
            inst.p.setProperty("version", prof); inst.p.setProperty("loader", kind);
            inst.p.setProperty("options.renderDistance", "8");   // this GL layer is GPU-bound on chunk drawing: distance 12 ran 12-19 fps with Sodium, 90-200 at 2; the instance settings change it
            Launcher.saveInstance(root, inst);
            log("modpack " + slug + " -> instance " + id + " (" + prof + ", " + tasks.size() + " files)");
        }
        pack.delete();
        return id;
    }
    static File packPath(File base, String rel) throws IOException {
        if (rel == null || rel.isEmpty()) throw new IOException("empty path in the pack");
        File f = new File(base, rel).getCanonicalFile();
        if (!f.getPath().startsWith(base.getPath() + File.separator)) throw new IOException("path outside the game folder: " + rel);
        return f;
    }
    static void installOne(String slug, String wantType, String mc, String loader, String gameDir, List<String> files, Set<String> seen, int depth) throws Exception {
        if (!seen.add(slug) || depth > 3) return;
        Map<String, Object> proj = McDownload.obj(getJson(modrinth() + "/v2/project/" + enc(slug)));
        String type = McDownload.str(proj.get("project_type")); if (type == null) type = wantType;
        if (depth == 0 && wantType.equals("datapack")) type = "datapack";   // a mod project can ship a datapack build too (Veinminer): take that build, not the mod
        if (!type.equals(wantType) && depth == 0) log(slug + " is a " + type + " project, not a " + wantType + " (installing it as such)");
        StringBuilder q = new StringBuilder("?");
        if (mc != null && !mc.isEmpty()) q.append("game_versions=").append(enc("[\"" + mc + "\"]")).append("&");
        if (loader != null && !loader.isEmpty() && type.equals("mod")) q.append("loaders=").append(enc("[\"" + loader + "\"]"));
        else if (type.equals("datapack")) q.append("loaders=").append(enc("[\"datapack\"]"));
        List<Object> vers = McDownload.arr(getJson(modrinth() + "/v2/project/" + enc(slug) + "/version" + q));
        if (vers == null || vers.isEmpty()) throw new IOException(slug + ": no version for Minecraft " + mc + (loader != null && !loader.isEmpty() ? " / " + loader : ""));
        Map<String, Object> ver = McDownload.obj(vers.get(0));
        if (slug.equals("mixinextras")) for (String pre : new String[] { "0.4.", "0.3." }) { boolean hit = false; for (Object o : vers) { Map<String, Object> m = McDownload.obj(o); if (McDownload.str(m.get("version_number")).startsWith(pre)) { ver = m; hit = true; break; } } if (hit) break; }   // 0.3/0.4 run on the Mixin 0.8.5 that Quilt Loader 0.20 ships; newer ones want a newer Mixin
        List<Object> fl = McDownload.arr(ver.get("files")); Map<String, Object> file = null;
        for (Object o : fl) { Map<String, Object> m = McDownload.obj(o); if (Boolean.TRUE.equals(m.get("primary"))) { file = m; break; } }
        if (file == null) file = McDownload.obj(fl.get(0));
        String fname = McDownload.str(file.get("filename"));
        if (fname == null || fname.contains("/") || fname.contains("\\") || fname.startsWith(".")) throw new IOException("bad file name " + fname);
        fname = fname.replaceAll("[^A-Za-z0-9._+()\\[\\] -]", "_");   // the JDK on the console maps file names as US-ASCII: a pack called "Glowing Ores §6" was an InvalidPathException
        Map<String, Object> hashes = McDownload.obj(file.get("hashes"));
        String tdir = typeDir(type, gameDir);
        File dest = new File(gameDir, tdir + "/" + fname);
        McDownload.Task t = new McDownload.Task(McDownload.str(file.get("url")), dest, hashes != null ? McDownload.str(hashes.get("sha1")) : null, McDownload.num(file.get("size")), slug);
        if (!t.present()) McDownload.fetch(t);
        log(slug + " " + McDownload.str(ver.get("version_number")) + " -> " + tdir + "/" + fname);
        files.add(tdir + "/" + fname);
        List<Object> deps = McDownload.arr(ver.get("dependencies"));
        if (deps != null) for (Object o : deps) { Map<String, Object> d = McDownload.obj(o); if ("required".equals(McDownload.str(d.get("dependency_type"))) && McDownload.str(d.get("project_id")) != null) installOne(McDownload.str(d.get("project_id")), "mod", mc, loader, gameDir, files, seen, depth + 1); }
    }
}
