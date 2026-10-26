// Drop-in replacement for org.lwjgl.Version (LWJGL 3.3.x). make_jvm_title.sh compiles it and swaps it into lib/lwjgl.jar.
// The title carries ONE LWJGL (static natives + Java classes, 3.3.3), but mods check getVersion() against the LWJGL their Minecraft version ships
// (1.20.1: 3.3.1, 1.21.x: 3.3.3): "Installed version / Required version / You must change the LWJGL version in your launcher".
// McBoot sets -Dps5.lwjgl.version=<x.y.z from the version JSON>; getVersion() then reports that number. The 3.3.3 natives are a superset of 3.3.1's, so the game itself is fine.
package org.lwjgl;

import java.io.InputStream;
import java.net.URL;
import java.util.jar.Attributes;
import java.util.jar.JarFile;
import java.util.jar.Manifest;

public final class Version {
    public static final int VERSION_MAJOR = 3, VERSION_MINOR = 3, VERSION_REVISION = 3;
    public static final BuildType BUILD_TYPE = BuildType.STABLE;

    private static final String versionPlain = String.valueOf(VERSION_MAJOR) + '.' + VERSION_MINOR + '.' + VERSION_REVISION + BUILD_TYPE.postfix;
    private static final String impl = find();
    private static final String version = versionPlain + impl;

    private Version() { }

    public static void main(String[] args) { System.out.println(getVersion()); System.err.println(versionPlain); }

    /** the real version, unless ps5.lwjgl.version names the one the running Minecraft version expects */
    public static String getVersion() {
        String w = System.getProperty("ps5.lwjgl.version");
        return w == null || w.isEmpty() ? version : w + BUILD_TYPE.postfix + impl;
    }

    public enum BuildType {
        ALPHA("a"), BETA("b"), STABLE("");
        public final String postfix;
        BuildType(String postfix) { this.postfix = postfix; }
    }

    static String createImplementation(String specVersion, String implVersion) {
        String build = "+" + (implVersion.startsWith("build ") && 6 < implVersion.length() ? implVersion.substring(6) : implVersion);
        return specVersion.contains("SNAPSHOT") || specVersion.contains("snapshot") ? "-snapshot" + build : build;
    }

    static String findImplementationFromManifest() {
        try {
            URL url = Version.class.getClassLoader().getResource("org/lwjgl/Version.class");
            if (url != null && url.toString().startsWith("jar:")) {
                URL m = Version.class.getResource("/" + JarFile.MANIFEST_NAME);
                if (m != null) try (InputStream s = m.openStream()) {
                    Attributes a = new Manifest(s).getMainAttributes();
                    if ("lwjgl".equals(a.getValue(Attributes.Name.IMPLEMENTATION_TITLE)) && "lwjgl.org".equals(a.getValue(Attributes.Name.IMPLEMENTATION_VENDOR))
                        && a.getValue(Attributes.Name.SPECIFICATION_VERSION) != null && a.getValue(Attributes.Name.IMPLEMENTATION_VERSION) != null)
                        return createImplementation(a.getValue(Attributes.Name.SPECIFICATION_VERSION), a.getValue(Attributes.Name.IMPLEMENTATION_VERSION));
                }
            }
        } catch (Exception ignored) { }
        return null;
    }

    private static String find() { String s = findImplementationFromManifest(); return s == null ? "" : s; }
}
