import org.lwjgl.glfw.GLFW;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL12C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.system.MemoryUtil;

import java.nio.ByteBuffer;

/**
 * Channel-order test for the "MC loading screen is blue instead of red" report. Three rows, each must show RED, GREEN, BLUE from left to right:
 *   top    : glClear through a scissor box          (pure framebuffer/scanout format)
 *   middle : quads with GL_UNSIGNED_BYTE vertex colors (what MC's position_color shader uses)
 *   bottom : a 3x1 GL_RGBA texture drawn NEAREST      (what MC's textures use)
 */
public class ColorTest {
    static int prog(String vs, String fs) {
        int p = GL20C.glCreateProgram();
        for (int i = 0; i < 2; i++) {
            int s = GL20C.glCreateShader(i == 0 ? GL20C.GL_VERTEX_SHADER : GL20C.GL_FRAGMENT_SHADER);
            GL20C.glShaderSource(s, i == 0 ? vs : fs);
            GL20C.glCompileShader(s);
            if (GL20C.glGetShaderi(s, GL20C.GL_COMPILE_STATUS) == 0) Hello.log("ColorTest: shader FAIL " + GL20C.glGetShaderInfoLog(s));
            GL20C.glAttachShader(p, s);
        }
        GL20C.glLinkProgram(p);
        if (GL20C.glGetProgrami(p, GL20C.GL_LINK_STATUS) == 0) Hello.log("ColorTest: link FAIL " + GL20C.glGetProgramInfoLog(p));
        return p;
    }

    public static void run(long win) {
        Hello.log("ColorTest: top = clear bars, middle = ubyte vertex colors, bottom = RGBA texture; each row should read RED GREEN BLUE (left to right)");
        int pc = prog("#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec4 c; out vec4 vc; void main(){ gl_Position=vec4(p,0.0,1.0); vc=c; }",
                      "#version 330 core\nin vec4 vc; out vec4 o; void main(){ o=vc; }");
        int pt = prog("#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 uv; out vec2 vuv; void main(){ gl_Position=vec4(p,0.0,1.0); vuv=uv; }",
                      "#version 330 core\nuniform sampler2D tex; in vec2 vuv; out vec4 o; void main(){ o=texture(tex, vuv); }");
        int texLoc = GL20C.glGetUniformLocation(pt, "tex");

        // middle row: 3 quads, interleaved float x,y + 4 normalized unsigned bytes (12 bytes per vertex)
        float[][] x = { { -1f, -0.333f }, { -0.333f, 0.333f }, { 0.333f, 1f } };
        int[][] col = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 } };
        float y0 = -0.30f, y1 = 0.30f;
        ByteBuffer vb = MemoryUtil.memAlloc(3 * 6 * 12);
        for (int q = 0; q < 3; q++) {
            float[][] corner = { { x[q][0], y0 }, { x[q][1], y0 }, { x[q][1], y1 }, { x[q][0], y0 }, { x[q][1], y1 }, { x[q][0], y1 } };
            for (float[] c : corner) {
                vb.putFloat(c[0]).putFloat(c[1]);
                vb.put((byte) col[q][0]).put((byte) col[q][1]).put((byte) col[q][2]).put((byte) 255);
            }
        }
        vb.flip();
        int vaoC = GL30C.glGenVertexArrays();
        GL30C.glBindVertexArray(vaoC);
        int vboC = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vboC);
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, vb, GL15C.GL_STATIC_DRAW);
        GL20C.glVertexAttribPointer(0, 2, GL11C.GL_FLOAT, false, 12, 0L);
        GL20C.glEnableVertexAttribArray(0);
        GL20C.glVertexAttribPointer(1, 4, GL11C.GL_UNSIGNED_BYTE, true, 12, 8L);
        GL20C.glEnableVertexAttribArray(1);

        // bottom row: one textured quad over a 3x1 RGBA texture (R, G, B texels), NEAREST so the three texels stay three bars
        ByteBuffer tx = MemoryUtil.memAlloc(12);
        tx.put((byte) 255).put((byte) 0).put((byte) 0).put((byte) 255)
          .put((byte) 0).put((byte) 255).put((byte) 0).put((byte) 255)
          .put((byte) 0).put((byte) 0).put((byte) 255).put((byte) 255).flip();
        int tex = GL11C.glGenTextures();
        GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, tex);
        GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MIN_FILTER, GL11C.GL_NEAREST);
        GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MAG_FILTER, GL11C.GL_NEAREST);
        GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_S, GL12C.GL_CLAMP_TO_EDGE);
        GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_T, GL12C.GL_CLAMP_TO_EDGE);
        GL11C.glTexImage2D(GL11C.GL_TEXTURE_2D, 0, GL11C.GL_RGBA8, 3, 1, 0, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, tx);
        float ty0 = -0.95f, ty1 = -0.40f;
        ByteBuffer qb = MemoryUtil.memAlloc(6 * 16);
        float[][] tq = { { -1f, ty0, 0f, 0f }, { 1f, ty0, 1f, 0f }, { 1f, ty1, 1f, 1f }, { -1f, ty0, 0f, 0f }, { 1f, ty1, 1f, 1f }, { -1f, ty1, 0f, 1f } };
        for (float[] v : tq) qb.putFloat(v[0]).putFloat(v[1]).putFloat(v[2]).putFloat(v[3]);
        qb.flip();
        int vaoT = GL30C.glGenVertexArrays();
        GL30C.glBindVertexArray(vaoT);
        int vboT = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vboT);
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, qb, GL15C.GL_STATIC_DRAW);
        GL20C.glVertexAttribPointer(0, 2, GL11C.GL_FLOAT, false, 16, 0L);
        GL20C.glEnableVertexAttribArray(0);
        GL20C.glVertexAttribPointer(1, 2, GL11C.GL_FLOAT, false, 16, 8L);
        GL20C.glEnableVertexAttribArray(1);

        GLFW.glfwSwapInterval(1);
        long t0 = System.nanoTime();
        int frames = 0;
        while ((System.nanoTime() - t0) < 10_000_000_000L && !GLFW.glfwWindowShouldClose(win)) {
            GL11C.glClearColor(0.25f, 0.25f, 0.25f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            // top row (y = 1080*0.58 .. 1080*0.98, from the bottom): three scissored clears
            GL11C.glEnable(GL11C.GL_SCISSOR_TEST);
            float[][] clr = { { 1f, 0f, 0f }, { 0f, 1f, 0f }, { 0f, 0f, 1f } };
            for (int i = 0; i < 3; i++) {
                GL11C.glScissor(i * 640, 640, 640, 400);
                GL11C.glClearColor(clr[i][0], clr[i][1], clr[i][2], 1f);
                GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            }
            GL11C.glDisable(GL11C.GL_SCISSOR_TEST);
            GL20C.glUseProgram(pc);
            GL30C.glBindVertexArray(vaoC);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 18);
            GL20C.glUseProgram(pt);
            GL20C.glUniform1i(texLoc, 0);
            GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, tex);
            GL30C.glBindVertexArray(vaoT);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 6);
            GLFW.glfwSwapBuffers(win);
            GLFW.glfwPollEvents();
            frames++;
        }
        Hello.log("ColorTest: " + frames + " frames, glError=" + GL11C.glGetError());

        // ---- phase 2 (10 s): MC renders into an FBO colour texture and then draws that texture to the screen. Same R G B bars, three ways:
        //   top: direct clear (reference) | middle: bars cleared INTO an FBO texture, shown via the texture | bottom: ubyte-colour quads drawn INTO an FBO texture
        Hello.log("ColorTest phase 2: top = direct reference, middle = FBO texture filled by clears, bottom = FBO texture filled by vertex-colour quads; each RED GREEN BLUE");
        final int FW = 960, FH = 128;
        int[] fboTex = new int[2], fbo = new int[2];
        for (int k = 0; k < 2; k++) {
            fboTex[k] = GL11C.glGenTextures();
            GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, fboTex[k]);
            GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MIN_FILTER, GL11C.GL_NEAREST);
            GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MAG_FILTER, GL11C.GL_NEAREST);
            GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_S, GL12C.GL_CLAMP_TO_EDGE);
            GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_T, GL12C.GL_CLAMP_TO_EDGE);
            GL11C.glTexImage2D(GL11C.GL_TEXTURE_2D, 0, GL11C.GL_RGBA8, FW, FH, 0, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, (ByteBuffer) null);
            fbo[k] = GL30C.glGenFramebuffers();
            GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo[k]);
            GL30C.glFramebufferTexture2D(GL30C.GL_FRAMEBUFFER, GL30C.GL_COLOR_ATTACHMENT0, GL11C.GL_TEXTURE_2D, fboTex[k], 0);
            Hello.log("ColorTest: fbo " + k + " status=0x" + Integer.toHexString(GL30C.glCheckFramebufferStatus(GL30C.GL_FRAMEBUFFER)) + " (0x8cd5 = complete)");
        }
        GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, 0);
        // textured quad for the middle row (the bottom row reuses vaoT from phase 1)
        float my0 = -0.30f, my1 = 0.30f;
        ByteBuffer mb = MemoryUtil.memAlloc(6 * 16);
        float[][] mq = { { -1f, my0, 0f, 0f }, { 1f, my0, 1f, 0f }, { 1f, my1, 1f, 1f }, { -1f, my0, 0f, 0f }, { 1f, my1, 1f, 1f }, { -1f, my1, 0f, 1f } };
        for (float[] v : mq) mb.putFloat(v[0]).putFloat(v[1]).putFloat(v[2]).putFloat(v[3]);
        mb.flip();
        int vaoM = GL30C.glGenVertexArrays();
        GL30C.glBindVertexArray(vaoM);
        int vboM = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vboM);
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, mb, GL15C.GL_STATIC_DRAW);
        GL20C.glVertexAttribPointer(0, 2, GL11C.GL_FLOAT, false, 16, 0L);
        GL20C.glEnableVertexAttribArray(0);
        GL20C.glVertexAttribPointer(1, 2, GL11C.GL_FLOAT, false, 16, 8L);
        GL20C.glEnableVertexAttribArray(1);
        t0 = System.nanoTime(); frames = 0;
        while ((System.nanoTime() - t0) < 10_000_000_000L && !GLFW.glfwWindowShouldClose(win)) {
            float[][] clr = { { 1f, 0f, 0f }, { 0f, 1f, 0f }, { 0f, 0f, 1f } };
            // FBO 0: three scissored clears
            GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo[0]);
            GL11C.glViewport(0, 0, FW, FH);
            GL11C.glEnable(GL11C.GL_SCISSOR_TEST);
            for (int i = 0; i < 3; i++) {
                GL11C.glScissor(i * (FW / 3), 0, FW / 3, FH);
                GL11C.glClearColor(clr[i][0], clr[i][1], clr[i][2], 1f);
                GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            }
            GL11C.glDisable(GL11C.GL_SCISSOR_TEST);
            // FBO 1: vertex-colour quads
            GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo[1]);
            GL11C.glViewport(0, 0, FW, FH);
            GL11C.glClearColor(0.25f, 0.25f, 0.25f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            GL20C.glUseProgram(pc);
            GL30C.glBindVertexArray(vaoC);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 18);
            // back to the screen
            GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, 0);
            GL11C.glViewport(0, 0, 1920, 1080);
            GL11C.glClearColor(0.25f, 0.25f, 0.25f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            GL11C.glEnable(GL11C.GL_SCISSOR_TEST);
            for (int i = 0; i < 3; i++) {
                GL11C.glScissor(i * 640, 640, 640, 400);
                GL11C.glClearColor(clr[i][0], clr[i][1], clr[i][2], 1f);
                GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            }
            GL11C.glDisable(GL11C.GL_SCISSOR_TEST);
            GL20C.glUseProgram(pt);
            GL20C.glUniform1i(texLoc, 0);
            GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, fboTex[0]);
            GL30C.glBindVertexArray(vaoM);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 6);
            GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, fboTex[1]);
            GL30C.glBindVertexArray(vaoT);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 6);
            GLFW.glfwSwapBuffers(win);
            GLFW.glfwPollEvents();
            frames++;
        }
        Hello.log("ColorTest phase 2: " + frames + " frames, glError=" + GL11C.glGetError());
        MemoryUtil.memFree(mb);
        GL20C.glUseProgram(0);
        GL30C.glBindVertexArray(0);
        MemoryUtil.memFree(vb); MemoryUtil.memFree(tx); MemoryUtil.memFree(qb);
    }
}
