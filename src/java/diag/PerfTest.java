import org.lwjgl.glfw.GLFW;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL12C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.system.MemoryUtil;

import java.nio.ByteBuffer;

/**
 * Per-call cost of the GL patterns Minecraft uses (47 draws + 55 glBufferData per frame, ~4 ms per draw, ~1 ms per glBufferData). Every test runs 6 frames of 40 ops
 * (first frame discarded) and logs the average time per call, measured around the GL call only (clear and swap excluded).
 */
public class PerfTest {
    interface Op { void run(int i); }

    static int prog, vao, vbo, ibo, fbo, fboTex;
    static final int[] rot = new int[64];
    static ByteBuffer quad;
    static long win;
    static double tA, tB;   // accumulated ns for the two timed sections of an op

    static int program() {
        int p = GL20C.glCreateProgram();
        String vs = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec4 c; out vec4 vc; void main(){ gl_Position=vec4(p,0.0,1.0); vc=c; }";
        String fs = "#version 330 core\nin vec4 vc; out vec4 o; void main(){ o=vc; }";
        for (int i = 0; i < 2; i++) {
            int s = GL20C.glCreateShader(i == 0 ? GL20C.GL_VERTEX_SHADER : GL20C.GL_FRAGMENT_SHADER);
            GL20C.glShaderSource(s, i == 0 ? vs : fs);
            GL20C.glCompileShader(s);
            GL20C.glAttachShader(p, s);
        }
        GL20C.glLinkProgram(p);
        return p;
    }

    /** four vertices (x, y, rgba bytes) for quad number i: a 24x24 px square somewhere on the 1920x1080 target */
    static void fillQuad(int i) {
        float x0 = -1f + (i % 40) * 0.04f, y0 = -0.9f + ((i / 40) % 10) * 0.1f, w = 0.025f, h = 0.04f;
        float[][] v = { { x0, y0 }, { x0 + w, y0 }, { x0 + w, y0 + h }, { x0, y0 + h } };
        quad.clear();
        for (float[] c : v) { quad.putFloat(c[0]).putFloat(c[1]); quad.put((byte) 255).put((byte) (i * 5)).put((byte) 64).put((byte) 255); }
        quad.flip();
    }

    static void attribs() {
        GL20C.glVertexAttribPointer(0, 2, GL11C.GL_FLOAT, false, 12, 0L);
        GL20C.glEnableVertexAttribArray(0);
        GL20C.glVertexAttribPointer(1, 4, GL11C.GL_UNSIGNED_BYTE, true, 12, 8L);
        GL20C.glEnableVertexAttribArray(1);
    }

    static void draw() { GL11C.glDrawElements(GL11C.GL_TRIANGLES, 6, GL11C.GL_UNSIGNED_SHORT, 0L); }

    /** runs 6 frames x 40 ops; op returns nothing, it adds its own timings to tA/tB */
    static void test(String name, String labelA, String labelB, Op op) {
        double sumA = 0, sumB = 0; int n = 0;
        for (int f = 0; f < 6; f++) {
            GL11C.glClearColor(0.1f, 0.1f, 0.1f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            GL30C.glBindVertexArray(vao);
            GL20C.glUseProgram(prog);
            tA = 0; tB = 0;
            for (int i = 0; i < 40; i++) op.run(i);
            if (f > 0) { sumA += tA; sumB += tB; n += 40; }
            GLFW.glfwSwapBuffers(win);
            GLFW.glfwPollEvents();
        }
        Hello.log(String.format("PerfTest %-34s %s=%.3f ms%s  (%d ops)", name, labelA, sumA / n / 1e6,
                labelB == null ? "" : String.format("  %s=%.3f ms", labelB, sumB / n / 1e6), n));
    }

    public static void run(long w) {
        win = w;
        Hello.log("PerfTest: per-call cost of Minecraft's GL patterns (avg ms per call; lower is better; a 60 fps frame is 16.7 ms)");
        prog = program();
        quad = MemoryUtil.memAlloc(48);
        ByteBuffer idx = MemoryUtil.memAlloc(12);
        idx.putShort((short) 0).putShort((short) 1).putShort((short) 2).putShort((short) 0).putShort((short) 2).putShort((short) 3).flip();
        vao = GL30C.glGenVertexArrays();
        GL30C.glBindVertexArray(vao);
        ibo = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ELEMENT_ARRAY_BUFFER, ibo);
        GL15C.glBufferData(GL15C.GL_ELEMENT_ARRAY_BUFFER, idx, GL15C.GL_STATIC_DRAW);
        vbo = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);
        fillQuad(0);
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_STATIC_DRAW);
        attribs();
        for (int i = 0; i < rot.length; i++) {
            rot[i] = GL15C.glGenBuffers();
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, rot[i]);
            fillQuad(i);
            GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_DYNAMIC_DRAW);
        }
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);

        GLFW.glfwSwapInterval(1);
        // A: static buffers, no state changes between draws
        test("A static draws", "draw", null, i -> { long t = System.nanoTime(); draw(); tA += System.nanoTime() - t; });
        // B: same with blending on (Minecraft's GUI and panorama use blend)
        GL11C.glEnable(GL11C.GL_BLEND);
        GL11C.glBlendFunc(GL11C.GL_SRC_ALPHA, GL11C.GL_ONE_MINUS_SRC_ALPHA);
        test("B static draws + blend", "draw", null, i -> { long t = System.nanoTime(); draw(); tA += System.nanoTime() - t; });
        GL11C.glDisable(GL11C.GL_BLEND);
        // C: Minecraft's pattern: glBufferData into the SAME vertex buffer, then draw
        test("C glBufferData(same VBO)+draw", "bufferData", "draw", i -> {
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo); fillQuad(i);
            long t = System.nanoTime(); GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_DYNAMIC_DRAW); tA += System.nanoTime() - t;
            attribs();
            t = System.nanoTime(); draw(); tB += System.nanoTime() - t; });
        // D: glBufferSubData (no reallocation) into the same buffer, then draw
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, 48L, GL15C.GL_DYNAMIC_DRAW);
        test("D glBufferSubData(same VBO)+draw", "subData", "draw", i -> {
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo); fillQuad(i);
            long t = System.nanoTime(); GL15C.glBufferSubData(GL15C.GL_ARRAY_BUFFER, 0L, quad); tA += System.nanoTime() - t;
            attribs();
            t = System.nanoTime(); draw(); tB += System.nanoTime() - t; });
        // E: rotate over 64 buffers, one glBufferData each (a buffer is not rewritten until 64 draws later)
        test("E glBufferData(64 rotating VBOs)+draw", "bufferData", "draw", i -> {
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, rot[i % 64]); fillQuad(i);
            long t = System.nanoTime(); GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_DYNAMIC_DRAW); tA += System.nanoTime() - t;
            attribs();
            t = System.nanoTime(); draw(); tB += System.nanoTime() - t; });
        // F: static draws into an offscreen FBO (Minecraft renders its whole frame into one)
        fboTex = GL11C.glGenTextures();
        GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, fboTex);
        GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MIN_FILTER, GL11C.GL_NEAREST);
        GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MAG_FILTER, GL11C.GL_NEAREST);
        GL11C.glTexImage2D(GL11C.GL_TEXTURE_2D, 0, GL11C.GL_RGBA8, 1920, 1080, 0, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, (ByteBuffer) null);
        fbo = GL30C.glGenFramebuffers();
        GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo);
        GL30C.glFramebufferTexture2D(GL30C.GL_FRAMEBUFFER, GL30C.GL_COLOR_ATTACHMENT0, GL11C.GL_TEXTURE_2D, fboTex, 0);
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);
        fillQuad(3);
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_STATIC_DRAW);
        attribs();
        test("F static draws into an FBO", "draw", null, i -> { long t = System.nanoTime(); draw(); tA += System.nanoTime() - t; });
        // G: glBufferData alone, no draw at all
        test("G glBufferData only (no draw)", "bufferData", null, i -> { GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo); fillQuad(i);
            long t = System.nanoTime(); GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_DYNAMIC_DRAW); tA += System.nanoTime() - t; });
        GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, 0);
        Hello.log("PerfTest: done, glError=" + GL11C.glGetError());
        GL20C.glUseProgram(0);
        GL30C.glBindVertexArray(0);
    }
}
