import java.io.*;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.*;

// Microsoft account login for Minecraft, device-code flow (no browser or keyboard on the console: the code is shown, the player types it on a phone/PC, or scans a QR of the same link).
// Chain: Microsoft device code -> MSA token -> Xbox Live -> XSTS -> Minecraft services -> profile. Accounts live in <root>/accounts.properties (keys <id>.name/.uuid/.refresh/.mcToken/.mcExpires, plus "active").
// A client id is required (launcher.cfg msaClientId=, or -Dps5.msa.clientid): register a free Azure app as a PUBLIC client with device-code flow allowed. Mojang also has to approve third-party app ids for the
// Minecraft services endpoint; an unapproved id is refused at the last steps (log: "login_with_xbox ... HTTP 403").
public class McAuth {
    static final class Session {
        String name, uuid, token, type;
        boolean demo = false;
    }
    static String base(String key, String dflt) { String b = System.getProperty("ps5.auth.base"); return b != null ? b + "/" + key : dflt; }
    static String ms() { return base("ms", "https://login.microsoftonline.com"); }
    static String live() { return base("live", "https://login.live.com"); }
    /** a legacy (live.com) application id: 16 hex digits; those use the login.live.com endpoints and an "t=" Xbox ticket instead of the Azure v2 ones */
    static boolean legacy(String cid) { return cid.matches("[0-9a-fA-F]{16}"); }
    static final String LEGACY_SCOPE = "service::user.auth.xboxlive.com::MBI_SSL";
    static String xbl() { return base("xbl", "https://user.auth.xboxlive.com"); }
    static String xsts() { return base("xsts", "https://xsts.auth.xboxlive.com"); }
    static String mc() { return base("mc", "https://api.minecraftservices.com"); }
    static void log(String s) { Hello.log("net: auth " + s); }
    static String enc(String s) { try { return URLEncoder.encode(s, "UTF-8"); } catch (UnsupportedEncodingException e) { throw new RuntimeException(e); } }
    static String jq(String s) { return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""; }
    static final String FORM = "application/x-www-form-urlencoded", JSON = "application/json";

    static String clientId(String root) {
        String c = System.getProperty("ps5.msa.clientid"); if (c != null && !c.isEmpty()) return c;
        c = Launcher.load(new File(root, "launcher.cfg")).getProperty("msaClientId", "").trim();
        if (!c.isEmpty()) return c;
        try { File f = new File("/app0/assets/mc/msa_client_id"); if (f.isFile()) return new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim(); } catch (Exception e) { }   // staged by make_jvm_title.sh from MSA_CLIENT_ID
        return "";
    }
    static Map<String, Object> json(McDownload.Resp r, String what) throws IOException {
        if (r.code / 100 != 2) throw new IOException(what + " -> HTTP " + r.code + ": " + (r.body.length() > 300 ? r.body.substring(0, 300) : r.body));
        return McDownload.obj(McDownload.Json.parse(r.body));
    }
    static File accountsFile(String root) { return new File(root, "accounts.properties"); }

    /** blocking: shows the code (log + <root>/launcher/login.txt), waits for the player, stores the account and makes it the active one; returns the account id */
    public static String login(String root) throws Exception {
        String cid = clientId(root);
        if (cid.isEmpty()) throw new IOException("no Microsoft client id: put msaClientId=<your Azure app id> into " + root + "/launcher.cfg (Azure portal > App registrations > new, 'Accounts in any organizational directory and personal Microsoft accounts', Authentication > allow public client flows)");
        final boolean lg = legacy(cid);
        Map<String, Object> d = lg
            ? json(McDownload.http("POST", live() + "/oauth20_connect.srf", "client_id=" + enc(cid) + "&scope=" + enc(LEGACY_SCOPE) + "&response_type=device_code", FORM), "device code")
            : json(McDownload.http("POST", ms() + "/consumers/oauth2/v2.0/devicecode", "client_id=" + enc(cid) + "&scope=" + enc("XboxLive.signin offline_access"), FORM), "device code");
        String code = McDownload.str(d.get("user_code")), uri = McDownload.str(d.get("verification_uri")), dev = McDownload.str(d.get("device_code"));
        long interval = Math.max(1, McDownload.num(d.get("interval"))), expires = McDownload.num(d.get("expires_in"));
        log("open " + uri + " on a phone or PC and enter the code " + code + " (valid " + expires + " s)");
        File lf = new File(root, "launcher/login.txt"); lf.getParentFile().mkdirs();
        java.nio.file.Files.write(lf.toPath(), (code + "\n" + uri + "\n" + "https://www.microsoft.com/link?otc=" + code + "\n").getBytes(StandardCharsets.UTF_8));
        long end = System.currentTimeMillis() + expires * 1000L;
        Map<String, Object> tok = null;
        File cf = new File(root, "launcher/login.cancel"); cf.delete();
        while (System.currentTimeMillis() < end) {
            for (long w = 0; w < interval * 1000L; w += 200) {   // the launcher's "Cancel sign-in" writes login.cancel
                if (cf.exists()) { cf.delete(); lf.delete(); throw new IOException("login cancelled"); }
                Thread.sleep(200);
            }
            McDownload.Resp r = lg
                ? McDownload.http("POST", live() + "/oauth20_token.srf", "grant_type=device_code&client_id=" + enc(cid) + "&device_code=" + enc(dev), FORM)
                : McDownload.http("POST", ms() + "/consumers/oauth2/v2.0/token", "grant_type=" + enc("urn:ietf:params:oauth:grant-type:device_code") + "&client_id=" + enc(cid) + "&device_code=" + enc(dev), FORM);
            Map<String, Object> j = McDownload.obj(McDownload.Json.parse(r.body));
            if (r.code == 200) { tok = j; break; }
            String err = McDownload.str(j.get("error"));
            if ("authorization_pending".equals(err)) continue;
            if ("slow_down".equals(err)) { interval += 5; continue; }
            throw new IOException("login failed: " + err + " " + McDownload.str(j.get("error_description")));
        }
        lf.delete();
        if (tok == null) throw new IOException("login expired (the code ran out)");
        Properties p = Launcher.load(accountsFile(root));
        String id = chain(p, McDownload.str(tok.get("access_token")), McDownload.str(tok.get("refresh_token")), lg);
        if ("demo".equals(id)) {
            // Demo mode: don't save as an account, just return "demo"
            log("signed in (demo mode)");
            return "demo";
        }
        p.setProperty("active", id);
        Launcher.save(accountsFile(root), p);
        skin(root, p, id);
        log("signed in as " + p.getProperty(id + ".name") + " (" + p.getProperty(id + ".uuid") + ")");
        return id;
    }

    /** MSA access token -> Minecraft token + profile, written into p under the profile's uuid; returns that id.
     *  Returns "demo" if the account doesn't own Minecraft (demo mode), throws on other errors. */
    static String chain(Properties p, String msaAccess, String refresh, boolean legacy) throws Exception {
        Map<String, Object> x = json(McDownload.http("POST", xbl() + "/user/authenticate",
            "{\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":" + jq((legacy ? "t=" : "d=") + msaAccess) + "},\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}", JSON), "Xbox Live");
        String xblToken = McDownload.str(x.get("Token"));
        Map<String, Object> s = json(McDownload.http("POST", xsts() + "/xsts/authorize",
            "{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[" + jq(xblToken) + "]},\"RelyingParty\":\"rp://api.minecraftservices.com/\",\"TokenType\":\"JWT\"}", JSON), "XSTS");
        String xstsToken = McDownload.str(s.get("Token"));
        String uhs = McDownload.str(McDownload.obj(McDownload.arr(McDownload.obj(s.get("DisplayClaims")).get("xui")).get(0)).get("uhs"));
        Map<String, Object> m = json(McDownload.http("POST", mc() + "/authentication/login_with_xbox", "{\"identityToken\":" + jq("XBL3.0 x=" + uhs + ";" + xstsToken) + "}", JSON), "login_with_xbox");
        String mcToken = McDownload.str(m.get("access_token")); long ttl = McDownload.num(m.get("expires_in"));
        McDownload.Resp pr = McDownload.http("GET", mc() + "/minecraft/profile", null, null, "Authorization", "Bearer " + mcToken);
        if (pr.code == 404) {
            // Account doesn't own Minecraft: return "demo" to indicate demo mode
            log("account does not own Minecraft: enabling demo mode");
            return "demo";
        }
        Map<String, Object> prof = json(pr, "profile");
        String id = McDownload.str(prof.get("id"));
        p.setProperty(id + ".name", McDownload.str(prof.get("name"))); p.setProperty(id + ".uuid", id);
        p.setProperty(id + ".mcToken", mcToken); p.setProperty(id + ".mcExpires", String.valueOf(System.currentTimeMillis() / 1000 + ttl));
        if (refresh != null) p.setProperty(id + ".refresh", refresh);
        try { for (Object o : McDownload.arr(prof.get("skins"))) { Map<String, Object> sk = McDownload.obj(o); if ("ACTIVE".equals(McDownload.str(sk.get("state")))) { p.setProperty(id + ".skin", McDownload.str(sk.get("url"))); break; } } } catch (Exception e) { }
        return id;
    }

    /** the player's skin (the launcher draws the face as the profile picture): <root>/launcher/skin-<id>.png, downloaded when missing or when the skin changed; never fails the sign-in */
    static void skin(String root, Properties p, String id) {
        try {
            String url = p.getProperty(id + ".skin", "");
            if (url.isEmpty()) {   // an account signed in before skins were recorded: ask the profile again with the saved Minecraft token
                String tok = p.getProperty(id + ".mcToken", ""); if (tok.isEmpty()) return;
                McDownload.Resp pr = McDownload.http("GET", mc() + "/minecraft/profile", null, null, "Authorization", "Bearer " + tok);
                if (pr.code != 200) return;
                for (Object o : McDownload.arr(McDownload.obj(McDownload.Json.parse(pr.body)).get("skins"))) { Map<String, Object> sk = McDownload.obj(o); if ("ACTIVE".equals(McDownload.str(sk.get("state")))) { url = McDownload.str(sk.get("url")); break; } }
                if (url.isEmpty()) return;
                p.setProperty(id + ".skin", url); Launcher.save(accountsFile(root), p);
            }
            File dest = new File(root, "launcher/skin-" + id + ".png"), mark = new File(root, "launcher/skin-" + id + ".url");
            if (dest.isFile() && mark.isFile() && new String(java.nio.file.Files.readAllBytes(mark.toPath()), StandardCharsets.UTF_8).trim().equals(url)) return;
            McDownload.fetch(new McDownload.Task(url, dest, null, 0, "skin"));
            java.nio.file.Files.write(mark.toPath(), url.getBytes(StandardCharsets.UTF_8));
        } catch (Throwable t) { log("skin: " + t); }
    }

    /** the "skin" command: make sure the face source of an account exists (the launcher asks once per account) */
    public static void skinFor(String root, String id) {
        if (id == null || id.isEmpty() || id.equals("offline") || id.equals("demo")) return;
        Properties p = Launcher.load(accountsFile(root));
        skin(root, p, id);
    }

    /** forget an account: its tokens, name and skin; the active account falls back to offline */
    public static void signOut(String root, String id) throws IOException {
        if (id == null || id.isEmpty() || id.equals("offline") || id.equals("demo")) return;
        Properties p = Launcher.load(accountsFile(root));
        for (String k : new ArrayList<>(p.stringPropertyNames())) if (k.startsWith(id + ".")) p.remove(k);
        if (id.equals(p.getProperty("active", ""))) p.remove("active");
        Launcher.save(accountsFile(root), p);
        new File(root, "launcher/skin-" + id + ".png").delete(); new File(root, "launcher/skin-" + id + ".url").delete();
        Properties g = Launcher.load(new File(root, "launcher.cfg"));
        if (id.equals(g.getProperty("account", "").trim())) { g.setProperty("account", "offline"); Launcher.save(new File(root, "launcher.cfg"), g); }
        log("signed out " + id);
    }

    /** a launch session for an account id (refreshing the token when it is about to expire); throws if that is impossible */
    public static Session sessionFor(String root, String id) throws Exception {
        Properties p = Launcher.load(accountsFile(root));
        if (id == null || id.isEmpty()) id = p.getProperty("active", "");
        if (id.isEmpty() || p.getProperty(id + ".name") == null) {
            // Check if it's demo mode
            Properties g = Launcher.load(new File(root, "launcher.cfg"));
            if ("demo".equals(g.getProperty("account", "").trim())) {
                return demo("Demo Player");
            }
            throw new IOException("no such account: " + id);
        }
        // Check if this is a demo account
        if ("demo".equals(id)) {
            return demo(p.getProperty("demo.name", "Demo Player"));
        }
        long exp = Long.parseLong(p.getProperty(id + ".mcExpires", "0"));
        if (exp < System.currentTimeMillis() / 1000 + 120) {
            String cid = clientId(root), rt = p.getProperty(id + ".refresh", "");
            if (rt.isEmpty() || cid.isEmpty()) throw new IOException("the Minecraft token expired and there is no refresh token: run the login command again");
            final boolean lg = legacy(cid);
            Map<String, Object> t = lg
                ? json(McDownload.http("POST", live() + "/oauth20_token.srf", "grant_type=refresh_token&client_id=" + enc(cid) + "&refresh_token=" + enc(rt) + "&scope=" + enc(LEGACY_SCOPE), FORM), "token refresh")
                : json(McDownload.http("POST", ms() + "/consumers/oauth2/v2.0/token", "grant_type=refresh_token&client_id=" + enc(cid) + "&refresh_token=" + enc(rt) + "&scope=" + enc("XboxLive.signin offline_access"), FORM), "token refresh");
            String nid = chain(p, McDownload.str(t.get("access_token")), McDownload.str(t.get("refresh_token")), lg);
            if ("demo".equals(nid)) {
                // Token refresh revealed demo mode
                Properties g = Launcher.load(new File(root, "launcher.cfg"));
                g.setProperty("account", "demo");
                Launcher.save(new File(root, "launcher.cfg"), g);
                return demo(p.getProperty(id + ".name"));
            }
            id = nid; Launcher.save(accountsFile(root), p);
        }
        skin(root, p, id);
        Session s = new Session(); s.name = p.getProperty(id + ".name"); s.uuid = p.getProperty(id + ".uuid"); s.token = p.getProperty(id + ".mcToken"); s.type = "msa";
        return s;
    }
    public static Session offline(String name) {
        Session s = new Session(); s.name = name; s.uuid = UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes(StandardCharsets.UTF_8)).toString().replace("-", ""); s.token = "0"; s.type = "legacy"; return s;
    }
    public static Session demo(String name) {
        Session s = new Session(); s.name = name; s.uuid = UUID.nameUUIDFromBytes(("DemoPlayer:" + name).getBytes(StandardCharsets.UTF_8)).toString().replace("-", ""); s.token = "0"; s.type = "demo"; s.demo = true; return s;
    }
}
