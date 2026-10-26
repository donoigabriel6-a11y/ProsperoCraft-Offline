// g4_test.c - fixed-function OpenGL through gl4es + ps5gl4es.c on a real OpenGL 4.5 CORE context (Mesa llvmpipe, headless EGL), the same situation as on the console.
// Renders small legacy-GL scenes and checks pixels. Build/run: see tests/run_all.sh (needs libEGL + Mesa; skipped when there is no usable software GL).
//   gcc -w -std=gnu11 -O1 -I.. g4_test.c ~/ps5jvm/libgl4es_ps5.a -lEGL -lm -lpthread -o g4_test
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps5_trace(const char *s) { if (getenv("G4_VERBOSE")) printf("   [trace] %s\n", s); }
extern void ps5g4_configure(void *(*)(const char *), int);
extern void *ps5g4_lookup(const char *);
extern void ps5g4_pre_swap(void);
extern void ps5g4_report(void);
extern void ps5g4_resync(void);

static int fails, checks;
#define F(n) static __typeof__(&n) p_##n;
#define LD(n) do { p_##n = (__typeof__(&n))ps5g4_lookup(#n); if (!p_##n) { printf("   MISSING function %s\n", #n); fails++; } } while (0)
#define OK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

F(glMatrixMode) F(glLoadIdentity) F(glOrtho) F(glBegin) F(glEnd) F(glColor3f) F(glColor4f) F(glVertex2f) F(glVertex3f) F(glClearColor) F(glClear) F(glEnable) F(glDisable)
F(glAlphaFunc) F(glReadPixels) F(glGetError) F(glViewport) F(glEnableClientState) F(glDisableClientState) F(glVertexPointer) F(glColorPointer) F(glTexCoordPointer) F(glNormalPointer)
F(glDrawArrays) F(glDrawElements) F(glGenTextures) F(glBindTexture) F(glTexImage2D) F(glTexParameteri) F(glTexEnvi) F(glTexCoord2f) F(glPushMatrix) F(glPopMatrix) F(glTranslatef) F(glScalef) F(glRotatef)
F(glLightfv) F(glMaterialfv) F(glNormal3f) F(glShadeModel) F(glFogi) F(glFogf) F(glFogfv) F(glGenLists) F(glNewList) F(glEndList) F(glCallList) F(glGenBuffers) F(glBindBuffer) F(glBufferData)
F(glGetString) F(glBlendFunc) F(glDepthFunc) F(glClearDepth) F(glActiveTexture) F(glClientActiveTexture) F(glFinish) F(glTexSubImage2D) F(glPixelStorei) F(glDeleteTextures) F(glPointSize) F(glLightModelfv)
F(glColorMaterial) F(glMultMatrixf) F(glLoadMatrixf) F(glFlush)
F(glGenFramebuffers) F(glBindFramebuffer) F(glFramebufferTexture2D) F(glGenRenderbuffers) F(glBindRenderbuffer) F(glRenderbufferStorage) F(glFramebufferRenderbuffer) F(glCheckFramebufferStatus)
F(glGenerateMipmap) F(glScissor) F(glColorMask) F(glCullFace) F(glFrontFace) F(glPolygonOffset) F(glStencilFunc) F(glStencilOp) F(glClearStencil) F(glMultiTexCoord2f) F(glLineWidth) F(glTexGeni) F(glTexGenfv)
F(glCreateShader) F(glShaderSource) F(glCompileShader) F(glCreateProgram) F(glAttachShader) F(glLinkProgram) F(glUseProgram) F(glGetUniformLocation) F(glUniformMatrix4fv) F(glUniform4f) F(glUniform1i) F(glBindAttribLocation)
F(glEnableVertexAttribArray) F(glDisableVertexAttribArray) F(glVertexAttribPointer) F(glGetShaderiv) F(glGetProgramiv) F(glGetShaderInfoLog) F(glGetProgramInfoLog) F(glUniform2f)
F(glTexEnvf) F(glDepthMask) F(glGetIntegerv) F(glIsEnabled) F(glGetFloatv) F(glCopyTexSubImage2D) F(glMatrixMode)

static unsigned char px[4];
static void readpx(int x, int y) { p_glFinish(); p_glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); }
static int near_(int a, int b) { return abs(a - b) <= 6; }
static int is_rgb(int r, int g, int b) { return near_(px[0], r) && near_(px[1], g) && near_(px[2], b); }
#define EXPECT_PX(x, y, r, g, b, what) do { readpx(x, y); OK(is_rgb(r, g, b), "%s: pixel (%d,%d) = %d,%d,%d, expected %d,%d,%d", what, x, y, px[0], px[1], px[2], r, g, b); } while (0)
#define NOERR(what) do { unsigned e = p_glGetError(); OK(e == 0, "%s: glGetError = 0x%x", what, e); } while (0)
static void clear(float r, float g, float b) { p_glClearColor(r, g, b, 1); p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); }
static void ortho64(void) { p_glViewport(0, 0, 64, 64); p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity(); p_glOrtho(0, 64, 0, 64, -1, 1); p_glMatrixMode(GL_MODELVIEW); p_glLoadIdentity(); }

int main(void) {
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min; if (!d || !eglInitialize(d, &maj, &min)) { printf("SKIP: no EGL display (fails=0)\n"); return 0; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE }; EGLConfig c; EGLint n = 0;
    eglChooseConfig(d, ca, &c, 1, &n);
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext x = n ? eglCreateContext(d, c, EGL_NO_CONTEXT, xa) : EGL_NO_CONTEXT; EGLint pa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface s = x ? eglCreatePbufferSurface(d, c, pa) : EGL_NO_SURFACE;
    if (!x || !s || !eglMakeCurrent(d, s, s, x)) { printf("SKIP: no core GL context (fails=0)\n"); return 0; }
    printf("   context: %s | %s\n", (const char *)eglGetProcAddress("glGetString") ? (const char *)((const unsigned char *(*)(unsigned))eglGetProcAddress("glGetString"))(GL_VERSION) : "?", "core");
    ps5g4_configure((void *(*)(const char *))eglGetProcAddress, 21);

    LD(glMatrixMode); LD(glLoadIdentity); LD(glOrtho); LD(glBegin); LD(glEnd); LD(glColor3f); LD(glColor4f); LD(glVertex2f); LD(glVertex3f); LD(glClearColor); LD(glClear); LD(glEnable); LD(glDisable);
    LD(glAlphaFunc); LD(glReadPixels); LD(glGetError); LD(glViewport); LD(glEnableClientState); LD(glDisableClientState); LD(glVertexPointer); LD(glColorPointer); LD(glTexCoordPointer); LD(glNormalPointer);
    LD(glDrawArrays); LD(glDrawElements); LD(glGenTextures); LD(glBindTexture); LD(glTexImage2D); LD(glTexParameteri); LD(glTexEnvi); LD(glTexCoord2f); LD(glPushMatrix); LD(glPopMatrix); LD(glTranslatef); LD(glScalef); LD(glRotatef);
    LD(glLightfv); LD(glMaterialfv); LD(glNormal3f); LD(glShadeModel); LD(glFogi); LD(glFogf); LD(glFogfv); LD(glGenLists); LD(glNewList); LD(glEndList); LD(glCallList); LD(glGenBuffers); LD(glBindBuffer); LD(glBufferData);
    LD(glGetString); LD(glBlendFunc); LD(glDepthFunc); LD(glClearDepth); LD(glActiveTexture); LD(glClientActiveTexture); LD(glFinish); LD(glTexSubImage2D); LD(glPixelStorei); LD(glDeleteTextures); LD(glPointSize);
    LD(glLightModelfv); LD(glColorMaterial); LD(glMultMatrixf); LD(glLoadMatrixf); LD(glFlush);
    LD(glGenFramebuffers); LD(glBindFramebuffer); LD(glFramebufferTexture2D); LD(glGenRenderbuffers); LD(glBindRenderbuffer); LD(glRenderbufferStorage); LD(glFramebufferRenderbuffer); LD(glCheckFramebufferStatus);
    LD(glGenerateMipmap); LD(glScissor); LD(glColorMask); LD(glCullFace); LD(glFrontFace); LD(glPolygonOffset); LD(glStencilFunc); LD(glStencilOp); LD(glClearStencil); LD(glMultiTexCoord2f); LD(glLineWidth); LD(glTexGeni); LD(glTexGenfv);
    LD(glCreateShader); LD(glShaderSource); LD(glCompileShader); LD(glCreateProgram); LD(glAttachShader); LD(glLinkProgram); LD(glUseProgram); LD(glGetUniformLocation); LD(glUniformMatrix4fv); LD(glUniform4f); LD(glUniform1i); LD(glBindAttribLocation);
    LD(glEnableVertexAttribArray); LD(glDisableVertexAttribArray); LD(glVertexAttribPointer); LD(glGetShaderiv); LD(glGetProgramiv); LD(glGetShaderInfoLog); LD(glGetProgramInfoLog); LD(glUniform2f);
    LD(glTexEnvf); LD(glDepthMask); LD(glGetIntegerv); LD(glIsEnabled); LD(glGetFloatv); LD(glCopyTexSubImage2D);
    if (fails) { printf("fails=%d\n", fails); return 1; }
    printf("   gl4es says: %s | %s\n", (const char *)p_glGetString(GL_VERSION), (const char *)p_glGetString(GL_RENDERER));

    // 1. the call that killed the JVM on the console, then a state-less clear
    p_glEnable(GL_ALPHA_TEST); p_glAlphaFunc(GL_GREATER, 0.1f); NOERR("glAlphaFunc");
    clear(0.2f, 0.4f, 0.6f); EXPECT_PX(10, 10, 51, 102, 153, "clear");

    // 2. immediate mode triangle with vertex colours
    ortho64(); clear(0, 0, 0);
    p_glBegin(GL_TRIANGLES); p_glColor3f(1, 0, 0); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(0, 64); p_glEnd();
    EXPECT_PX(8, 8, 255, 0, 0, "glBegin triangle (red)"); EXPECT_PX(56, 56, 0, 0, 0, "outside the triangle"); NOERR("glBegin");

    // 3. client-side vertex arrays, GL_QUADS, ModelView translate + scale
    ortho64(); clear(0, 0, 0);
    { float v[] = { 0, 0, 8, 0, 8, 8, 0, 8 }; unsigned char col[] = { 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255 };
      p_glEnableClientState(GL_VERTEX_ARRAY); p_glEnableClientState(GL_COLOR_ARRAY); p_glVertexPointer(2, GL_FLOAT, 0, v); p_glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
      p_glPushMatrix(); p_glTranslatef(20, 20, 0); p_glScalef(2, 2, 1); p_glDrawArrays(GL_QUADS, 0, 4); p_glPopMatrix();
      p_glDisableClientState(GL_COLOR_ARRAY); p_glDisableClientState(GL_VERTEX_ARRAY); }
    EXPECT_PX(26, 26, 0, 255, 0, "client arrays GL_QUADS (green)"); EXPECT_PX(10, 10, 0, 0, 0, "outside the quad"); NOERR("client arrays");

    // 4. interleaved client array (position + colour, stride 24) with client indices (GL_UNSIGNED_SHORT)
    ortho64(); clear(0, 0, 0);
    { float v[] = { 0, 0, 0, 0, 1, 1,   32, 0, 0, 0, 1, 1,   32, 32, 0, 0, 1, 1,   0, 32, 0, 0, 1, 1 }; unsigned short idx[] = { 0, 1, 2, 0, 2, 3 };
      p_glEnableClientState(GL_VERTEX_ARRAY); p_glEnableClientState(GL_COLOR_ARRAY);
      p_glVertexPointer(2, GL_FLOAT, 24, v); p_glColorPointer(4, GL_FLOAT, 24, v + 2);
      p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      p_glDisableClientState(GL_COLOR_ARRAY); p_glDisableClientState(GL_VERTEX_ARRAY); }
    EXPECT_PX(10, 10, 0, 0, 255, "interleaved array + client indices (blue)"); NOERR("interleaved");

    // 5. vertex buffer object + client-side indices, and VBO + element buffer
    ortho64(); clear(0, 0, 0);
    { float v[] = { 0, 0, 64, 0, 64, 64, 0, 64 }; unsigned idx[] = { 0, 1, 2, 0, 2, 3 }; unsigned bufs[2];
      p_glGenBuffers(2, bufs); p_glBindBuffer(GL_ARRAY_BUFFER, bufs[0]); p_glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
      p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs[1]); p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
      p_glColor3f(1, 1, 0); p_glEnableClientState(GL_VERTEX_ARRAY); p_glVertexPointer(2, GL_FLOAT, 0, 0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0); p_glDisableClientState(GL_VERTEX_ARRAY);
      p_glBindBuffer(GL_ARRAY_BUFFER, 0); p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); }
    EXPECT_PX(32, 32, 255, 255, 0, "VBO + element buffer (yellow)"); NOERR("vbo");

    // 6. textures: RGBA with GL_CLAMP, GL_LUMINANCE_ALPHA, GL_ALPHA (swizzled R8 / RG8), alpha test, modulate
    ortho64(); clear(0, 0, 0); p_glEnable(GL_TEXTURE_2D); p_glColor4f(1, 1, 1, 1);
    { unsigned tex[3]; p_glGenTextures(3, tex); unsigned char rgba[4 * 4], la[4 * 2], al[4]; for (int i = 0; i < 4; i++) { rgba[i * 4] = 255; rgba[i * 4 + 1] = 128; rgba[i * 4 + 2] = 0; rgba[i * 4 + 3] = 255; la[i * 2] = 200; la[i * 2 + 1] = 255; al[i] = 255; }
      p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      p_glBindTexture(GL_TEXTURE_2D, tex[0]); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba); NOERR("RGBA texture");
      p_glBegin(GL_QUADS); p_glTexCoord2f(0, 0); p_glVertex2f(0, 0); p_glTexCoord2f(1, 0); p_glVertex2f(20, 0); p_glTexCoord2f(1, 1); p_glVertex2f(20, 20); p_glTexCoord2f(0, 1); p_glVertex2f(0, 20); p_glEnd();
      EXPECT_PX(10, 10, 255, 128, 0, "RGBA texture, GL_CLAMP");
      p_glBindTexture(GL_TEXTURE_2D, tex[1]); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 2, 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la); NOERR("LUMINANCE_ALPHA texture");
      p_glBegin(GL_QUADS); p_glTexCoord2f(0, 0); p_glVertex2f(30, 0); p_glTexCoord2f(1, 0); p_glVertex2f(50, 0); p_glTexCoord2f(1, 1); p_glVertex2f(50, 20); p_glTexCoord2f(0, 1); p_glVertex2f(30, 20); p_glEnd();
      EXPECT_PX(40, 10, 200, 200, 200, "GL_LUMINANCE_ALPHA texture (grey)");
      p_glBindTexture(GL_TEXTURE_2D, tex[2]); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 2, 2, 0, GL_ALPHA, GL_UNSIGNED_BYTE, al); NOERR("ALPHA texture");
      p_glColor4f(0, 1, 1, 1);
      p_glBegin(GL_QUADS); p_glTexCoord2f(0, 0); p_glVertex2f(0, 30); p_glTexCoord2f(1, 0); p_glVertex2f(20, 30); p_glTexCoord2f(1, 1); p_glVertex2f(20, 50); p_glTexCoord2f(0, 1); p_glVertex2f(0, 50); p_glEnd();
      EXPECT_PX(10, 40, 0, 255, 255, "GL_ALPHA texture modulated with colour (cyan)");
      p_glBindTexture(GL_TEXTURE_2D, tex[0]);   // a texture bound again after the swizzled ones must be unaffected
      p_glColor4f(1, 1, 1, 1);
      p_glBegin(GL_QUADS); p_glTexCoord2f(0, 0); p_glVertex2f(30, 30); p_glTexCoord2f(1, 0); p_glVertex2f(50, 30); p_glTexCoord2f(1, 1); p_glVertex2f(50, 50); p_glTexCoord2f(0, 1); p_glVertex2f(30, 50); p_glEnd();
      EXPECT_PX(40, 40, 255, 128, 0, "RGBA texture after the swizzled ones");
      p_glDeleteTextures(3, tex); }
    p_glDisable(GL_TEXTURE_2D); NOERR("textures");

    // 7. alpha test discards (alpha 0 pixels), blending
    ortho64(); clear(0, 0, 0); p_glAlphaFunc(GL_GREATER, 0.5f);
    p_glBegin(GL_QUADS); p_glColor4f(1, 0, 0, 0.2f); p_glVertex2f(0, 0); p_glVertex2f(32, 0); p_glVertex2f(32, 32); p_glVertex2f(0, 32); p_glEnd();
    EXPECT_PX(16, 16, 0, 0, 0, "alpha test rejects alpha 0.2");
    p_glBegin(GL_QUADS); p_glColor4f(0, 1, 0, 0.9f); p_glVertex2f(32, 32); p_glVertex2f(64, 32); p_glVertex2f(64, 64); p_glVertex2f(32, 64); p_glEnd();
    EXPECT_PX(48, 48, 0, 255, 0, "alpha test accepts alpha 0.9");
    p_glDisable(GL_ALPHA_TEST);
    p_glEnable(GL_BLEND); p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glBegin(GL_QUADS); p_glColor4f(1, 1, 1, 0.5f); p_glVertex2f(0, 0); p_glVertex2f(32, 0); p_glVertex2f(32, 32); p_glVertex2f(0, 32); p_glEnd();
    readpx(16, 16); OK(near_(px[0], 128) && near_(px[1], 128) && near_(px[2], 128), "blending: %d,%d,%d expected ~128 grey", px[0], px[1], px[2]);
    p_glDisable(GL_BLEND); NOERR("alpha test / blend");

    // 8. lighting
    ortho64(); clear(0, 0, 0);
    { float pos[] = { 0, 0, 1, 0 }, dif[] = { 1, 1, 1, 1 }, mat[] = { 0, 0.5f, 1, 1 }, amb[] = { 0, 0, 0, 1 };
      p_glEnable(GL_LIGHTING); p_glEnable(GL_LIGHT0); p_glLightfv(GL_LIGHT0, GL_POSITION, pos); p_glLightfv(GL_LIGHT0, GL_DIFFUSE, dif); p_glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
      p_glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat);
      p_glBegin(GL_TRIANGLES); p_glNormal3f(0, 0, 1); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(0, 64); p_glEnd();
      p_glDisable(GL_LIGHTING); }
    EXPECT_PX(8, 8, 0, 128, 255, "lighting (diffuse * material)"); NOERR("lighting");

    // 9. fog
    ortho64(); clear(0, 0, 0);
    { float fc[] = { 1, 0, 1, 1 }; p_glEnable(GL_FOG); p_glFogi(GL_FOG_MODE, GL_LINEAR); p_glFogf(GL_FOG_START, 0); p_glFogf(GL_FOG_END, 1); p_glFogfv(GL_FOG_COLOR, fc);
      p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity(); p_glOrtho(0, 64, 0, 64, 0, 1); p_glMatrixMode(GL_MODELVIEW);
      p_glColor3f(0, 1, 0);
      p_glBegin(GL_TRIANGLES); p_glVertex3f(0, 0, -1); p_glVertex3f(64, 0, -1); p_glVertex3f(0, 64, -1); p_glEnd();   // at the far plane: fully fogged
      p_glDisable(GL_FOG); }
    EXPECT_PX(8, 8, 255, 0, 255, "linear fog at the far distance (magenta)"); NOERR("fog");

    // 10. display list
    ortho64(); clear(0, 0, 0);
    { unsigned l = p_glGenLists(1); p_glNewList(l, GL_COMPILE); p_glColor3f(0, 1, 1); p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glVertex2f(0, 64); p_glEnd(); p_glEndList();
      p_glCallList(l); }
    EXPECT_PX(32, 32, 0, 255, 255, "display list (cyan)"); NOERR("display list");

    // 11. depth test between two overlapping triangles
    ortho64(); clear(0, 0, 0); p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity(); p_glOrtho(0, 64, 0, 64, 0, 10); p_glMatrixMode(GL_MODELVIEW);
    p_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_LESS);
    p_glColor3f(1, 0, 0); p_glBegin(GL_QUADS); p_glVertex3f(0, 0, -2); p_glVertex3f(64, 0, -2); p_glVertex3f(64, 64, -2); p_glVertex3f(0, 64, -2); p_glEnd();
    p_glColor3f(0, 0, 1); p_glBegin(GL_QUADS); p_glVertex3f(0, 0, -5); p_glVertex3f(64, 0, -5); p_glVertex3f(64, 64, -5); p_glVertex3f(0, 64, -5); p_glEnd();
    p_glDisable(GL_DEPTH_TEST);
    EXPECT_PX(32, 32, 255, 0, 0, "depth test (the nearer red quad stays)"); NOERR("depth");

    // 12. a lot of draws: ring buffer wraps (client arrays of 1 MB, 40 times), no errors
    ortho64(); clear(0, 0, 0);
    { int nv = 6 * 14000; float *v = malloc((size_t)nv * 2 * sizeof(float)); for (int i = 0; i < nv / 6; i++) { float *q = v + i * 12; float x0 = (float)(i % 64), y0 = (float)((i / 64) % 64); float t[12] = { x0, y0, x0 + 1, y0, x0 + 1, y0 + 1, x0, y0, x0 + 1, y0 + 1, x0, y0 + 1 }; memcpy(q, t, sizeof t); }
      p_glColor3f(1, 1, 1); p_glEnableClientState(GL_VERTEX_ARRAY); p_glVertexPointer(2, GL_FLOAT, 0, v);
      for (int k = 0; k < 40; k++) p_glDrawArrays(GL_TRIANGLES, 0, nv);
      p_glDisableClientState(GL_VERTEX_ARRAY); free(v); }
    EXPECT_PX(5, 5, 255, 255, 255, "many large client-array draws"); NOERR("ring wraps");


    // 13. framebuffer object (Minecraft renders the world into one): RGBA texture + depth renderbuffer, draw into it, then draw its texture to the screen
    { unsigned fbo, tex, rb; p_glGenFramebuffers(1, &fbo); p_glGenTextures(1, &tex); p_glGenRenderbuffers(1, &rb);
      p_glBindTexture(GL_TEXTURE_2D, tex); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
      p_glBindRenderbuffer(GL_RENDERBUFFER, rb); p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 32, 32);
      p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0); p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
      unsigned st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER); OK(st == GL_FRAMEBUFFER_COMPLETE, "FBO status 0x%x", st);
      p_glViewport(0, 0, 32, 32); p_glMatrixMode(GL_PROJECTION); p_glLoadIdentity(); p_glOrtho(0, 32, 0, 32, -1, 1); p_glMatrixMode(GL_MODELVIEW); p_glLoadIdentity();
      p_glClearColor(0, 0, 1, 1); p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      p_glBegin(GL_QUADS); p_glColor3f(1, 0, 0); p_glVertex2f(0, 0); p_glVertex2f(16, 0); p_glVertex2f(16, 32); p_glVertex2f(0, 32); p_glEnd();
      p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
      ortho64(); clear(0, 0, 0); p_glEnable(GL_TEXTURE_2D); p_glBindTexture(GL_TEXTURE_2D, tex); p_glColor3f(1, 1, 1);
      p_glBegin(GL_QUADS); p_glTexCoord2f(0, 0); p_glVertex2f(0, 0); p_glTexCoord2f(1, 0); p_glVertex2f(64, 0); p_glTexCoord2f(1, 1); p_glVertex2f(64, 64); p_glTexCoord2f(0, 1); p_glVertex2f(0, 64); p_glEnd();
      p_glDisable(GL_TEXTURE_2D);
      EXPECT_PX(10, 32, 255, 0, 0, "FBO colour texture shown on screen (left half red)"); EXPECT_PX(54, 32, 0, 0, 255, "FBO colour texture (right half blue)"); NOERR("FBO"); }

    // 14. texture uploads the way Minecraft does them: BGRA / packed ABGR, mipmaps, sub-image, GL_TEXTURE_MAX_LEVEL
    ortho64(); clear(0, 0, 0); p_glEnable(GL_TEXTURE_2D);
    { unsigned tex; p_glGenTextures(1, &tex); p_glBindTexture(GL_TEXTURE_2D, tex); unsigned px4[16]; for (int i = 0; i < 16; i++) px4[i] = 0xFF0000FFu;   // packed ABGR: A=ff B=00 G=00 R=ff -> red
      p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, px4); NOERR("packed RGBA upload");
      p_glGenerateMipmap(GL_TEXTURE_2D); NOERR("glGenerateMipmap");
      unsigned bg[4] = { 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u };   // BGRA bytes 00 FF 00 FF -> green when read as BGRA ... as a uint it is A=ff R=00 G=ff B=00
      p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bg); NOERR("BGRA sub-image");
      p_glColor3f(1, 1, 1);
      p_glBegin(GL_QUADS); p_glTexCoord2f(0, 0); p_glVertex2f(0, 0); p_glTexCoord2f(1, 0); p_glVertex2f(64, 0); p_glTexCoord2f(1, 1); p_glVertex2f(64, 64); p_glTexCoord2f(0, 1); p_glVertex2f(0, 64); p_glEnd();
      EXPECT_PX(48, 48, 255, 0, 0, "packed ABGR texture (red part)"); readpx(16, 16); OK(px[0] < 40 && px[1] > 200 && px[2] < 40, "BGRA sub-image (green part): %d,%d,%d", px[0], px[1], px[2]);
      p_glDeleteTextures(1, &tex); }
    p_glDisable(GL_TEXTURE_2D);

    // 15. multitexture with the lightmap on unit 1 (modulate), glMultiTexCoord
    ortho64(); clear(0, 0, 0);
    { unsigned t[2]; p_glGenTextures(2, t); unsigned char white[4] = { 255, 255, 255, 255 }, grey[4] = { 128, 128, 128, 255 };
      p_glActiveTexture(GL_TEXTURE0); p_glEnable(GL_TEXTURE_2D); p_glBindTexture(GL_TEXTURE_2D, t[0]); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
      p_glActiveTexture(GL_TEXTURE1); p_glEnable(GL_TEXTURE_2D); p_glBindTexture(GL_TEXTURE_2D, t[1]); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey);
      p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      p_glColor3f(1, 1, 0);
      p_glBegin(GL_QUADS);
      for (int k = 0; k < 4; k++) { float xs[4] = { 0, 64, 64, 0 }, ys[4] = { 0, 0, 64, 64 }; p_glMultiTexCoord2f(GL_TEXTURE0, 0.5f, 0.5f); p_glMultiTexCoord2f(GL_TEXTURE1, 0.5f, 0.5f); p_glVertex2f(xs[k], ys[k]); }
      p_glEnd();
      p_glDisable(GL_TEXTURE_2D); p_glActiveTexture(GL_TEXTURE0); p_glDisable(GL_TEXTURE_2D);
      readpx(32, 32); OK(near_(px[0], 128) && near_(px[1], 128) && px[2] < 10, "multitexture modulate (yellow * white * grey): %d,%d,%d", px[0], px[1], px[2]); NOERR("multitexture"); }

    // 16. scissor, colour mask, culling, polygon offset, stencil, quad strip, lines, points
    ortho64(); clear(0, 0, 0);
    p_glEnable(GL_SCISSOR_TEST); p_glScissor(0, 0, 32, 64); clear(0, 1, 0); p_glDisable(GL_SCISSOR_TEST);
    EXPECT_PX(8, 8, 0, 255, 0, "scissored clear (inside)"); EXPECT_PX(56, 8, 0, 0, 0, "scissored clear (outside)");
    clear(0, 0, 0); p_glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE); p_glColor3f(1, 1, 1); p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glVertex2f(0, 64); p_glEnd(); p_glColorMask(1, 1, 1, 1);
    EXPECT_PX(32, 32, 0, 255, 0, "colour mask (green only)");
    clear(0, 0, 0); p_glEnable(GL_CULL_FACE); p_glCullFace(GL_BACK); p_glFrontFace(GL_CCW); p_glColor3f(1, 0, 0);
    p_glBegin(GL_TRIANGLES); p_glVertex2f(0, 0); p_glVertex2f(32, 0); p_glVertex2f(0, 32); p_glEnd();            // counter-clockwise: kept
    p_glColor3f(0, 0, 1); p_glBegin(GL_TRIANGLES); p_glVertex2f(32, 32); p_glVertex2f(32, 64); p_glVertex2f(64, 32); p_glEnd();   // clockwise: culled
    p_glDisable(GL_CULL_FACE); EXPECT_PX(8, 8, 255, 0, 0, "culling keeps the front face"); EXPECT_PX(40, 40, 0, 0, 0, "culling removes the back face");
    clear(0, 0, 0); p_glEnable(GL_STENCIL_TEST); p_glClearStencil(0); p_glClear(GL_STENCIL_BUFFER_BIT); p_glStencilFunc(GL_ALWAYS, 1, 0xFF); p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); p_glColorMask(0, 0, 0, 0);
    p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(32, 0); p_glVertex2f(32, 64); p_glVertex2f(0, 64); p_glEnd(); p_glColorMask(1, 1, 1, 1);
    p_glStencilFunc(GL_EQUAL, 1, 0xFF); p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); p_glColor3f(1, 1, 0); p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glVertex2f(0, 64); p_glEnd(); p_glDisable(GL_STENCIL_TEST);
    EXPECT_PX(8, 32, 255, 255, 0, "stencil (inside the mask)"); EXPECT_PX(56, 32, 0, 0, 0, "stencil (outside the mask)");
    clear(0, 0, 0); p_glColor3f(0, 1, 1); p_glBegin(GL_QUAD_STRIP); p_glVertex2f(0, 0); p_glVertex2f(0, 64); p_glVertex2f(32, 0); p_glVertex2f(32, 64); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glEnd();
    EXPECT_PX(48, 32, 0, 255, 255, "GL_QUAD_STRIP"); clear(0, 0, 0);
    p_glColor3f(1, 1, 1); p_glLineWidth(1); p_glBegin(GL_LINES); p_glVertex2f(0, 32.5f); p_glVertex2f(64, 32.5f); p_glEnd(); readpx(32, 32); OK(px[0] > 128, "GL_LINES: %d,%d,%d", px[0], px[1], px[2]);
    clear(0, 0, 0); p_glPointSize(8); p_glBegin(GL_POINTS); p_glVertex2f(32, 32); p_glEnd(); EXPECT_PX(32, 32, 255, 255, 255, "GL_POINTS with glPointSize");
    NOERR("misc state");

    // 17. texture matrix / glTexGen (the end portal and enchantment glint use them) and glPolygonOffset: the calls must be accepted
    ortho64(); clear(0, 0, 0);
    { float pl[] = { 1, 0, 0, 0 }; p_glEnable(GL_TEXTURE_2D); p_glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR); p_glTexGenfv(GL_S, GL_OBJECT_PLANE, pl); p_glEnable(GL_TEXTURE_GEN_S);
      p_glMatrixMode(GL_TEXTURE); p_glLoadIdentity(); p_glScalef(0.01f, 0.01f, 1); p_glMatrixMode(GL_MODELVIEW); p_glEnable(GL_POLYGON_OFFSET_FILL); p_glPolygonOffset(-1, -1);
      p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glVertex2f(0, 64); p_glEnd();
      p_glDisable(GL_POLYGON_OFFSET_FILL); p_glDisable(GL_TEXTURE_GEN_S); p_glMatrixMode(GL_TEXTURE); p_glLoadIdentity(); p_glMatrixMode(GL_MODELVIEW); p_glDisable(GL_TEXTURE_2D); }
    NOERR("texgen / texture matrix / polygon offset");

    // 18. state queries the game makes
    { int v[4] = { 0 }; p_glGetIntegerv(GL_VIEWPORT, v); OK(v[2] == 64 && v[3] == 64, "GL_VIEWPORT = %d,%d,%d,%d", v[0], v[1], v[2], v[3]); p_glGetIntegerv(GL_MAX_TEXTURE_SIZE, v); OK(v[0] >= 2048, "GL_MAX_TEXTURE_SIZE = %d", v[0]);
      p_glGetIntegerv(GL_MAX_TEXTURE_UNITS, v); OK(v[0] >= 2, "GL_MAX_TEXTURE_UNITS = %d", v[0]); }

    // 19. the game's own GLSL (1.16 post-processing shaders are "#version 110": attribute / varying / gl_FragColor / texture2D) and a "#version 120" pair using fixed-function built-ins
    { static const char *vs110 = "#version 110\nattribute vec4 Position;\nuniform mat4 ProjMat;\nuniform vec2 OutSize;\nvarying vec2 texCoord;\nvoid main() {\n    vec4 outPos = ProjMat * vec4(Position.xy, 0.0, 1.0);\n    gl_Position = vec4(outPos.xy, 0.2, 1.0);\n    texCoord = Position.xy / OutSize;\n}\n";
      static const char *fs110 = "#version 110\nuniform sampler2D DiffuseSampler;\nuniform vec4 ColorModulate;\nvarying vec2 texCoord;\nvoid main() {\n    gl_FragColor = texture2D(DiffuseSampler, texCoord) * ColorModulate;\n}\n";
      unsigned vs = p_glCreateShader(GL_VERTEX_SHADER), fs = p_glCreateShader(GL_FRAGMENT_SHADER), pr = p_glCreateProgram(); int st = 0;
      p_glShaderSource(vs, 1, &vs110, 0); p_glCompileShader(vs); p_glGetShaderiv(vs, GL_COMPILE_STATUS, &st); OK(st == 1, "#version 110 vertex shader compiles");
      p_glShaderSource(fs, 1, &fs110, 0); p_glCompileShader(fs); p_glGetShaderiv(fs, GL_COMPILE_STATUS, &st); OK(st == 1, "#version 110 fragment shader compiles");
      p_glAttachShader(pr, vs); p_glAttachShader(pr, fs); p_glBindAttribLocation(pr, 0, "Position"); p_glLinkProgram(pr); p_glGetProgramiv(pr, GL_LINK_STATUS, &st); OK(st == 1, "program links");
      unsigned tex; p_glGenTextures(1, &tex); unsigned char c[4] = { 10, 200, 30, 255 }; p_glBindTexture(GL_TEXTURE_2D, tex); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, c);
      ortho64(); clear(0, 0, 0); p_glUseProgram(pr);
      float proj[16] = { 2.0f / 64, 0, 0, 0,  0, 2.0f / 64, 0, 0,  0, 0, -1, 0,  -1, -1, 0, 1 };
      p_glUniformMatrix4fv(p_glGetUniformLocation(pr, "ProjMat"), 1, 0, proj); p_glUniform2f(p_glGetUniformLocation(pr, "OutSize"), 64, 64); p_glUniform4f(p_glGetUniformLocation(pr, "ColorModulate"), 1, 1, 1, 1); p_glUniform1i(p_glGetUniformLocation(pr, "DiffuseSampler"), 0);
      float quad[] = { 0, 0, 64, 0, 64, 64, 0, 0, 64, 64, 0, 64 };
      p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad); p_glDrawArrays(GL_TRIANGLES, 0, 6); p_glDisableVertexAttribArray(0); p_glUseProgram(0);
      EXPECT_PX(32, 32, 10, 200, 30, "user #version 110 program (texture * modulate)"); NOERR("user shaders"); }
    { static const char *vs120 = "#version 120\nvarying vec4 col;\nvoid main() {\n    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n    col = gl_Color;\n    gl_TexCoord[0] = gl_MultiTexCoord0;\n}\n";
      static const char *fs120 = "#version 120\nvarying vec4 col;\nvoid main() {\n    gl_FragColor = col * vec4(1.0, 0.5, 1.0, 1.0);\n}\n";
      unsigned vs = p_glCreateShader(GL_VERTEX_SHADER), fs = p_glCreateShader(GL_FRAGMENT_SHADER), pr = p_glCreateProgram(); int st = 0;
      p_glShaderSource(vs, 1, &vs120, 0); p_glCompileShader(vs); p_glGetShaderiv(vs, GL_COMPILE_STATUS, &st); OK(st == 1, "#version 120 vertex shader with built-ins compiles");
      p_glShaderSource(fs, 1, &fs120, 0); p_glCompileShader(fs); p_glGetShaderiv(fs, GL_COMPILE_STATUS, &st); OK(st == 1, "#version 120 fragment shader compiles");
      p_glAttachShader(pr, vs); p_glAttachShader(pr, fs); p_glLinkProgram(pr); p_glGetProgramiv(pr, GL_LINK_STATUS, &st); OK(st == 1, "#version 120 program links");
      ortho64(); clear(0, 0, 0); p_glUseProgram(pr); p_glColor3f(1, 1, 1);
      p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glVertex2f(0, 64); p_glEnd(); p_glUseProgram(0);
      EXPECT_PX(32, 32, 255, 128, 255, "user #version 120 program with fixed-function built-ins"); NOERR("user shaders 120"); }

    // 19a. glShadeModel (1.12.2 GuiScreen.drawGradientRect: shadeModel(7425) / shadeModel(7424) around every gradient, every frame): the console's real GL exports glShadeModel but it is invalid in a core context -> glGetError 0x502 per call, so gl4es must not forward it
    { p_glShadeModel(0x1D00); p_glShadeModel(0x1D01); p_glShadeModel(0x1D00); p_glShadeModel(0x1D01); NOERR("glShadeModel FLAT / SMOOTH"); }

    // 19b. built-in math the game's shaders call (1.12.2 post shaders: clamp / min / max / pow / mod with float arguments): gl4es' int/float overload "hacks" hide the built-ins on the console's compiler and every such shader failed ("no matching function for call to clamp(float, float, float)", GL error 1282 each frame)
    { static const char *vsb = "#version 120\nvoid main() {\n    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n}\n";
      static const char *fsb = "#version 120\nvoid main() {\n    float a = clamp(4.0, 0.0, 1.0);\n    float b = max(0.0, min(1.0, -3.0));\n    float c = mod(7.0, 2.0);\n    float d = pow(1.0, 3.0);\n    vec2 e = clamp(vec2(2.0, -1.0), 0.0, 1.0);\n    gl_FragColor = vec4(a, b + e.y, c * d, 1.0);\n}\n";
      unsigned vs = p_glCreateShader(GL_VERTEX_SHADER), fs = p_glCreateShader(GL_FRAGMENT_SHADER), pr = p_glCreateProgram(); int st = 0;
      p_glShaderSource(vs, 1, &vsb, 0); p_glCompileShader(vs); p_glShaderSource(fs, 1, &fsb, 0); p_glCompileShader(fs); p_glGetShaderiv(fs, GL_COMPILE_STATUS, &st); OK(st == 1, "fragment shader with clamp / min / max / pow / mod on floats compiles");
      p_glAttachShader(pr, vs); p_glAttachShader(pr, fs); p_glLinkProgram(pr); p_glGetProgramiv(pr, GL_LINK_STATUS, &st); OK(st == 1, "math program links");
      ortho64(); clear(0, 0, 0); p_glUseProgram(pr);
      p_glBegin(GL_QUADS); p_glVertex2f(0, 0); p_glVertex2f(64, 0); p_glVertex2f(64, 64); p_glVertex2f(0, 64); p_glEnd(); p_glUseProgram(0);
      EXPECT_PX(32, 32, 255, 0, 255, "built-in math program output"); NOERR("math shader"); }

    // 20. buffer bindings stay right when client-array draws (vertex ring, index ring) and VBO / element-buffer draws alternate (the ring bindings are shadowed and left bound between draws, identical index lists are reused)
    { unsigned short idx[] = { 0, 1, 2, 0, 2, 3 }; unsigned idx32[] = { 0, 1, 2, 0, 2, 3 }; unsigned vb, eb; ortho64(); clear(0, 0, 0);
      p_glGenBuffers(1, &vb); p_glGenBuffers(1, &eb); p_glEnableClientState(GL_VERTEX_ARRAY);
      for (int rep = 0; rep < 3; rep++) {
        float a0[] = { 0, 0, 16, 0, 16, 16, 0, 16 }, b0[] = { 16, 0, 32, 0, 32, 16, 16, 16 }, c0[] = { 32, 0, 48, 0, 48, 16, 32, 16 }, d0[] = { 48, 0, 64, 0, 64, 16, 48, 16 };
        float e0[] = { 0, 16, 16, 16, 16, 32, 0, 32 }, f0[] = { 16, 16, 32, 16, 32, 32, 16, 32 };
        p_glColor3f(1, 0, 0); p_glVertexPointer(2, GL_FLOAT, 0, a0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);   // client array + client indices (twice: the second reuses the index upload)
        p_glBindBuffer(GL_ARRAY_BUFFER, vb); p_glBufferData(GL_ARRAY_BUFFER, sizeof b0, b0, GL_STATIC_DRAW); p_glColor3f(0, 1, 0); p_glVertexPointer(2, GL_FLOAT, 0, 0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);   // VBO + client indices
        p_glBindBuffer(GL_ARRAY_BUFFER, 0); p_glColor3f(0, 0, 1); p_glVertexPointer(2, GL_FLOAT, 0, c0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);   // back to a client array
        p_glBindBuffer(GL_ARRAY_BUFFER, vb); p_glBufferData(GL_ARRAY_BUFFER, sizeof d0, d0, GL_STATIC_DRAW); p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb); p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx32, idx32, GL_STATIC_DRAW);
        p_glColor3f(1, 1, 0); p_glVertexPointer(2, GL_FLOAT, 0, 0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);   // VBO + element buffer
        p_glBufferData(GL_ARRAY_BUFFER, sizeof e0, e0, GL_STATIC_DRAW); p_glColor3f(0, 1, 1); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);   // re-upload the bound VBO (must not land in a ring) and draw it again
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); p_glBindBuffer(GL_ARRAY_BUFFER, 0); p_glColor3f(1, 0, 1); p_glVertexPointer(2, GL_FLOAT, 0, f0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      }
      p_glDisableClientState(GL_VERTEX_ARRAY);
      EXPECT_PX(8, 8, 255, 0, 0, "mixed bindings: client array + client indices"); EXPECT_PX(24, 8, 0, 255, 0, "mixed bindings: VBO + client indices"); EXPECT_PX(40, 8, 0, 0, 255, "mixed bindings: client array after a VBO draw");
      EXPECT_PX(56, 8, 255, 255, 0, "mixed bindings: VBO + element buffer"); EXPECT_PX(8, 24, 0, 255, 255, "mixed bindings: re-uploaded VBO + element buffer"); EXPECT_PX(24, 24, 255, 0, 255, "mixed bindings: client array after an element buffer"); EXPECT_PX(56, 40, 0, 0, 0, "mixed bindings: nothing else drawn");
      // positions in a VBO + colours in a client array (both kinds in ONE draw), then the VBO again without binding it: the array binding must be back on the VBO
      { unsigned char cc[] = { 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255 }; float g0[] = { 32, 16, 48, 16, 48, 32, 32, 32 };
        p_glEnableClientState(GL_VERTEX_ARRAY); p_glEnableClientState(GL_COLOR_ARRAY); p_glBindBuffer(GL_ARRAY_BUFFER, 0); p_glColorPointer(4, GL_UNSIGNED_BYTE, 0, cc);   // the colours: a client array (pointers are only client pointers while no buffer is bound)
        p_glBindBuffer(GL_ARRAY_BUFFER, vb); p_glBufferData(GL_ARRAY_BUFFER, sizeof g0, g0, GL_STATIC_DRAW); p_glVertexPointer(2, GL_FLOAT, 0, 0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx); p_glDisableClientState(GL_COLOR_ARRAY);
        EXPECT_PX(40, 24, 255, 128, 0, "mixed bindings: VBO positions + client colours");
        p_glColor3f(1, 1, 1); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
        EXPECT_PX(40, 24, 255, 255, 255, "mixed bindings: the VBO is still the array buffer afterwards");
        { float g1[] = { 48, 16, 64, 16, 64, 32, 48, 32 }; p_glBufferData(GL_ARRAY_BUFFER, sizeof g1, g1, GL_STATIC_DRAW); p_glColor3f(1, 0, 0); p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);   // no bind in between: the data must go to the VBO, not to a ring
          EXPECT_PX(56, 24, 255, 0, 0, "mixed bindings: glBufferData after a mixed draw reaches the VBO"); }
        p_glDisableClientState(GL_VERTEX_ARRAY); p_glBindBuffer(GL_ARRAY_BUFFER, 0); }
      NOERR("mixed bindings"); }

    // 13. the native launcher uses the same context between two games: its own VAO and array buffer replace gl4es' bindings. Without ps5g4_resync() the next client-array draw is sent with the ring OFFSET
    //     while another buffer (or none) is bound: on Mesa that is a segfault inside glDrawElements (checked by hand), on the console the SDK crash "memcpy from a small address" after the second 1.12.2 start.
    { void (*gen_vao)(int, unsigned *) = (void (*)(int, unsigned *))eglGetProcAddress("glGenVertexArrays"), (*bind_vao)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glBindVertexArray");
      void (*gen_buf)(int, unsigned *) = (void (*)(int, unsigned *))eglGetProcAddress("glGenBuffers"), (*bind_buf)(unsigned, unsigned) = (void (*)(unsigned, unsigned))eglGetProcAddress("glBindBuffer");
      void (*buf_data)(unsigned, long, const void *, unsigned) = (void (*)(unsigned, long, const void *, unsigned))eglGetProcAddress("glBufferData");
      void (*geti)(unsigned, int *) = (void (*)(unsigned, int *))eglGetProcAddress("glGetIntegerv");
      float v[] = { 0, 0, 40, 0, 40, 40, 0, 40 }; unsigned char junk[64] = { 0 }; unsigned kv = 0, kb = 0; int vao0 = 0, arr0 = 0, vao1 = 0, arr1 = 0, vao2 = 0, arr2 = 0;
      ortho64(); p_glColor3f(0, 1, 0); p_glEnableClientState(GL_VERTEX_ARRAY); p_glVertexPointer(2, GL_FLOAT, 0, v);
      clear(0, 0, 0); p_glDrawArrays(GL_QUADS, 0, 4); EXPECT_PX(10, 10, 0, 255, 0, "before the launcher: client-array quad");   // leaves the ring bound
      geti(0x85B5, &vao0); geti(0x8894, &arr0);   // VERTEX_ARRAY_BINDING, ARRAY_BUFFER_BINDING
      gen_vao(1, &kv); gen_buf(1, &kb); bind_vao(kv); bind_buf(GL_ARRAY_BUFFER, kb); buf_data(GL_ARRAY_BUFFER, sizeof junk, junk, GL_STATIC_DRAW);   // "the launcher"
      geti(0x85B5, &vao1); geti(0x8894, &arr1); OK(vao1 != vao0 && arr1 != arr0 && arr0 != 0, "the launcher moved the bindings (vao %d -> %d, array %d -> %d)", vao0, vao1, arr0, arr1);
      // the launcher also leaves ITS shader program bound; gl4es caches "my fixed-function program is current" and would not bind its own again (every glUniformMatrix then hit the launcher's program: "non-matrix uniform")
      { unsigned (*rcs)(unsigned) = (unsigned (*)(unsigned))eglGetProcAddress("glCreateShader"), (*rcp)(void) = (unsigned (*)(void))eglGetProcAddress("glCreateProgram");
        void (*rss)(unsigned, int, const char *const *, const int *) = (void (*)(unsigned, int, const char *const *, const int *))eglGetProcAddress("glShaderSource"), (*rcc)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glCompileShader");
        void (*rat)(unsigned, unsigned) = (void (*)(unsigned, unsigned))eglGetProcAddress("glAttachShader"), (*rlk)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glLinkProgram"), (*rup)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glUseProgram");
        const char *vsrc = "#version 330 core\nlayout(location=0) in vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }", *fsrc = "#version 330 core\nout vec4 c; void main(){ c = vec4(1.0, 0.0, 1.0, 1.0); }";
        unsigned vs = rcs(GL_VERTEX_SHADER), fs = rcs(GL_FRAGMENT_SHADER), pr = rcp(); rss(vs, 1, &vsrc, 0); rcc(vs); rss(fs, 1, &fsrc, 0); rcc(fs); rat(pr, vs); rat(pr, fs); rlk(pr); rup(pr); }
      ps5g4_resync(); geti(0x85B5, &vao2); geti(0x8894, &arr2); OK(vao2 == vao0 && arr2 == arr0, "ps5g4_resync restores the bindings (vao %d, array %d; expected %d, %d)", vao2, arr2, vao0, arr0);
      clear(0, 0, 0); p_glDrawArrays(GL_QUADS, 0, 4); EXPECT_PX(10, 10, 0, 255, 0, "after the launcher + ps5g4_resync: client-array quad"); NOERR("after resync");
      p_glDisableClientState(GL_VERTEX_ARRAY); }

    ps5g4_pre_swap(); ps5g4_report();
    { unsigned (*rge)(void) = (unsigned (*)(void))eglGetProcAddress("glGetError"); unsigned e = rge(); OK(e == 0, "the console-side (real) glGetError at the end = 0x%x", e); }
    printf("   %d checks\nfails=%d\n", checks, fails);
    return fails ? 1 : 0;
}
