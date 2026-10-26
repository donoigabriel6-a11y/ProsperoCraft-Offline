#pragma once
#include <stdint.h>
typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig, *EGLNativeWindowType; typedef int EGLint; typedef unsigned EGLBoolean;
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_DISPLAY ((void*)0)
#define EGL_NO_SURFACE ((void*)0)
#define EGL_NO_CONTEXT ((void*)0)
#define EGL_SURFACE_TYPE 1
#define EGL_WINDOW_BIT 2
#define EGL_RENDERABLE_TYPE 3
#define EGL_OPENGL_BIT 4
#define EGL_RED_SIZE 5
#define EGL_GREEN_SIZE 6
#define EGL_BLUE_SIZE 7
#define EGL_ALPHA_SIZE 8
#define EGL_DEPTH_SIZE 9
#define EGL_NONE 0x3038
#define EGL_OPENGL_API 1
#define EGL_WIDTH 2
#define EGL_HEIGHT 3
EGLDisplay eglGetDisplay(void*); EGLBoolean eglInitialize(EGLDisplay,EGLint*,EGLint*); EGLint eglGetError(void); EGLBoolean eglBindAPI(unsigned); EGLBoolean eglChooseConfig(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*);
EGLSurface eglCreateWindowSurface(EGLDisplay,EGLConfig,void*,const EGLint*); EGLContext eglCreateContext(EGLDisplay,EGLConfig,EGLContext,const EGLint*); EGLBoolean eglQuerySurface(EGLDisplay,EGLSurface,EGLint,EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay,EGLSurface,EGLSurface,EGLContext); EGLContext eglGetCurrentContext(void); EGLBoolean eglSwapBuffers(EGLDisplay,EGLSurface); EGLBoolean eglSwapInterval(EGLDisplay,EGLint); void *eglGetProcAddress(const char*);
