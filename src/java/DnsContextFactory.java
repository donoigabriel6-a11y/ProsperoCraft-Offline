// Minecraft looks up "_minecraft._tcp.<host>" SRV records through JNDI (com.sun.jndi.dns.DnsContextFactory). The title's Java images carry no jdk.naming.dns module, so the lookup failed with
// "Failed to initialize SRV redirect resolved, some servers might not work" and every server that is published through an SRV record (PebbleHost and most hosts with a custom port) did not answer.
// This class takes that name: a minimal DirContext that answers getAttributes(name, {"SRV"}) with a DNS query over TCP (blocking sockets work on the console; the system resolver is not needed).
package com.sun.jndi.dns;

import java.io.*;
import java.lang.reflect.Proxy;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.Hashtable;
import java.util.Random;
import javax.naming.Context;
import javax.naming.NamingException;
import javax.naming.OperationNotSupportedException;
import javax.naming.directory.Attributes;
import javax.naming.directory.BasicAttributes;
import javax.naming.directory.DirContext;
import javax.naming.spi.InitialContextFactory;

public final class DnsContextFactory implements InitialContextFactory {
    static final String[] RESOLVERS = System.getProperty("ps5.dns", "1.1.1.1,8.8.8.8").split(",");

    public Context getInitialContext(Hashtable<?, ?> env) throws NamingException {
        return (Context) Proxy.newProxyInstance(DnsContextFactory.class.getClassLoader(), new Class<?>[] { DirContext.class }, (proxy, m, a) -> {
            String n = m.getName();
            if (n.equals("getAttributes") && a != null && a.length == 2 && a[1] instanceof String[]) return lookup(String.valueOf(a[0]), (String[]) a[1]);
            if (n.equals("close")) return null;
            if (n.equals("hashCode")) return System.identityHashCode(proxy);
            if (n.equals("equals")) return proxy == a[0];
            if (n.equals("toString")) return "DnsContext(ps5)";
            throw new OperationNotSupportedException(n);
        });
    }

    static Attributes lookup(String name, String[] ids) throws NamingException {
        BasicAttributes out = new BasicAttributes(true);
        boolean srv = false; for (String i : ids) if (i.equalsIgnoreCase("SRV")) srv = true;
        if (!srv) return out;
        String best = null; int bestPri = Integer.MAX_VALUE;
        for (String r : RESOLVERS) {
            try {
                for (String rec : srvQuery(r.trim(), name)) { int pri = Integer.parseInt(rec.substring(0, rec.indexOf(' '))); if (pri < bestPri) { bestPri = pri; best = rec; } }
                break;   // an answer (even an empty one) from the first resolver that responds
            } catch (IOException e) { /* try the next resolver */ }
        }
        if (best != null) out.put("srv", best);
        return out;
    }

    /** "priority weight port target." for every SRV record of <name> */
    static java.util.List<String> srvQuery(String server, String name) throws IOException {
        ByteArrayOutputStream q = new ByteArrayOutputStream(); DataOutputStream d = new DataOutputStream(q);
        int id = new Random().nextInt(0x10000);
        d.writeShort(id); d.writeShort(0x0100); d.writeShort(1); d.writeShort(0); d.writeShort(0); d.writeShort(0);
        for (String label : name.split("\\.")) { if (label.isEmpty()) continue; byte[] b = label.getBytes("US-ASCII"); d.writeByte(b.length); d.write(b); }
        d.writeByte(0); d.writeShort(33); d.writeShort(1);
        byte[] resp;
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(server, 53), 3000); s.setSoTimeout(3000);
            DataOutputStream o = new DataOutputStream(s.getOutputStream()); o.writeShort(q.size()); o.write(q.toByteArray()); o.flush();
            DataInputStream in = new DataInputStream(s.getInputStream()); int len = in.readUnsignedShort(); resp = new byte[len]; in.readFully(resp);
        }
        java.util.List<String> out = new java.util.ArrayList<String>();
        if (resp.length < 12 || ((resp[0] & 255) << 8 | (resp[1] & 255)) != id || (resp[3] & 15) != 0) return out;   // other answer / NXDOMAIN / error: no records
        int qd = (resp[4] & 255) << 8 | (resp[5] & 255), an = (resp[6] & 255) << 8 | (resp[7] & 255), p = 12;
        for (int i = 0; i < qd; i++) { p = skipName(resp, p) + 4; }
        for (int i = 0; i < an && p < resp.length; i++) {
            p = skipName(resp, p);
            int type = (resp[p] & 255) << 8 | (resp[p + 1] & 255), rdlen = (resp[p + 8] & 255) << 8 | (resp[p + 9] & 255); p += 10;
            if (type == 33 && rdlen >= 7) {
                int pri = (resp[p] & 255) << 8 | (resp[p + 1] & 255), wt = (resp[p + 2] & 255) << 8 | (resp[p + 3] & 255), port = (resp[p + 4] & 255) << 8 | (resp[p + 5] & 255);
                out.add(pri + " " + wt + " " + port + " " + readName(resp, p + 6) + ".");
            }
            p += rdlen;
        }
        return out;
    }
    static int skipName(byte[] b, int p) { while (p < b.length) { int l = b[p] & 255; if (l == 0) return p + 1; if ((l & 0xC0) == 0xC0) return p + 2; p += l + 1; } return p; }
    static String readName(byte[] b, int p) {
        StringBuilder sb = new StringBuilder(); int hops = 0;
        while (p < b.length && hops < 20) {
            int l = b[p] & 255;
            if (l == 0) break;
            if ((l & 0xC0) == 0xC0) { p = (l & 0x3F) << 8 | (b[p + 1] & 255); hops++; continue; }
            if (sb.length() > 0) sb.append('.');
            sb.append(new String(b, p + 1, l, java.nio.charset.StandardCharsets.US_ASCII)); p += l + 1;
        }
        return sb.toString();
    }
}
