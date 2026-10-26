import org.lwjgl.glfw.GLFW;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.system.MemoryStack;

import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.ByteBuffer;
import java.nio.FloatBuffer;

/** First graphics test: LWJGL -> our GLFW shim -> EGL/OpenGL 4.6 Core. Draws a spinning colored triangle for ~8 seconds. */
public class GLTest {
    static int compile(int type, String src) {
        int s = GL20C.glCreateShader(type);
        GL20C.glShaderSource(s, src);
        GL20C.glCompileShader(s);
        if (GL20C.glGetShaderi(s, GL20C.GL_COMPILE_STATUS) == 0) Hello.log("FAIL shader: " + GL20C.glGetShaderInfoLog(s));
        return s;
    }

    static void routeOutput() {
        PrintStream ps = new PrintStream(new OutputStream() {
            final StringBuilder sb = new StringBuilder();
            @Override public void write(int b) {
                if (b == '\n') { Hello.log("[lwjgl] " + sb); sb.setLength(0); } else if (b != '\r') sb.append((char) b);
            }
        }, true);
        System.setErr(ps);
        System.setOut(ps);
    }

    public static void run() {
        routeOutput();
        System.setProperty("org.lwjgl.util.Debug", "true");
        System.setProperty("org.lwjgl.util.DebugLoader", "true");
        Hello.log("GLTest: start");
        if (!GLFW.glfwInit()) { Hello.log("FAIL glfwInit"); return; }
        long win = GLFW.glfwCreateWindow(1920, 1080, "gltest", 0L, 0L);
        if (win == 0L) { Hello.log("FAIL glfwCreateWindow"); return; }
        GLFW.glfwMakeContextCurrent(win);
        Hello.log("GLTest: context current, calling GL.createCapabilities() (GL loads its library itself)");
        GL.createCapabilities();
        Hello.log("GLTest: createCapabilities() returned");
        if (new java.io.File("/app0/assets/hello/PerfTest.class").exists()) PerfTest.run(win);   // per-call GL cost of Minecraft's patterns (about 10 s), then the triangle
        // (ColorTest.run(win) is still available for the channel-order test)
        Hello.log("GL_VERSION=" + GL11C.glGetString(GL11C.GL_VERSION) + " RENDERER=" + GL11C.glGetString(GL11C.GL_RENDERER));

        int prog = GL20C.glCreateProgram();
        GL20C.glAttachShader(prog, compile(GL20C.GL_VERTEX_SHADER,
            "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec3 c; out vec3 vc; uniform float t;\n"
          + "void main(){ float s=sin(t), k=cos(t); gl_Position=vec4(p.x*k-p.y*s, p.x*s+p.y*k, 0.0, 1.0); vc=c; }"));
        GL20C.glAttachShader(prog, compile(GL20C.GL_FRAGMENT_SHADER,
            "#version 330 core\nin vec3 vc; out vec4 o; void main(){ o=vec4(vc,1.0); }"));
        GL20C.glLinkProgram(prog);
        if (GL20C.glGetProgrami(prog, GL20C.GL_LINK_STATUS) == 0) { Hello.log("FAIL link: " + GL20C.glGetProgramInfoLog(prog)); return; }

        int vao = GL30C.glGenVertexArrays();
        GL30C.glBindVertexArray(vao);
        int vbo = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);
        try (MemoryStack st = MemoryStack.stackPush()) {
            FloatBuffer v = st.floats(0f, 0.7f, 1f, 0f, 0f,   -0.7f, -0.6f, 0f, 1f, 0f,   0.7f, -0.6f, 0f, 0f, 1f);
            GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, v, GL15C.GL_STATIC_DRAW);
        }
        GL20C.glVertexAttribPointer(0, 2, GL11C.GL_FLOAT, false, 20, 0L);
        GL20C.glEnableVertexAttribArray(0);
        GL20C.glVertexAttribPointer(1, 3, GL11C.GL_FLOAT, false, 20, 8L);
        GL20C.glEnableVertexAttribArray(1);
        int tLoc = GL20C.glGetUniformLocation(prog, "t");

        GLFW.glfwSwapInterval(1);
        long t0 = System.nanoTime();
        int frames = 0;
        ByteBuffer px = org.lwjgl.system.MemoryUtil.memAlloc(4);
        while ((System.nanoTime() - t0) < 8_000_000_000L && !GLFW.glfwWindowShouldClose(win)) {
            float t = (System.nanoTime() - t0) / 1e9f;
            GL11C.glClearColor(0.05f, 0.08f, 0.2f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            GL20C.glUseProgram(prog);
            GL20C.glUniform1f(tLoc, t);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 3);
            if (frames == 30) {
                GL11C.glReadPixels(960, 540, 1, 1, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, px);
                Hello.log("center pixel rgba=" + (px.get(0) & 255) + "," + (px.get(1) & 255) + "," + (px.get(2) & 255) + "," + (px.get(3) & 255));
            }
            GLFW.glfwSwapBuffers(win);
            GLFW.glfwPollEvents();
            frames++;
        }
        double secs = (System.nanoTime() - t0) / 1e9;
        Hello.log("GLTest: " + frames + " frames in " + String.format("%.2f", secs) + " s, glError=" + GL11C.glGetError());
    }
}
