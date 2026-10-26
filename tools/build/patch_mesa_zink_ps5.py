#!/usr/bin/env python3
"""patch_mesa_zink_ps5.py [MESA_SRC]: lets Mesa's Zink present on the console's display (VK_KHR_display) through a surfaceless-EGL window surface.
Default tree: ~/PS5_Vulkan/.deps/work/radv-src (what build-zink-probe.sh compiles). Idempotent (marker PS5_ZINK_PATCH); every anchor must match or nothing is written.
  - include/kopper_interface.h : PS5_STRUCTURE_TYPE_DISPLAY_SURFACE (the surface-create-info type the surfaceless platform hands to Zink)
  - zink_kopper.h/.c           : KOPPER_PS5 display target; the surface comes from ps5_zink_create_display_surface() (the shim: SDL_Vulkan_CreateSurface's VK_KHR_display code)
  - zink_instance.py           : VK_KHR_display instance extension
  - egl_dri2.c                 : window configs next to the pbuffer ones when the display is kopper (Zink)
  - platform_surfaceless.c     : eglCreateWindowSurface (native_window = int[2] {width, height}), eglSwapBuffers, eglSwapInterval through kopper
"""
import os, re, sys
src = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/PS5_Vulkan/.deps/work/radv-src")
M = "PS5_ZINK_PATCH"
edits = {}   # path -> text
def load(rel):
    p = os.path.join(src, rel)
    if p not in edits:
        if not os.path.exists(p): sys.exit("missing file: " + p)
        edits[p] = open(p).read()
    return p
def need(cond, msg):
    if not cond: sys.exit("PATCH FAILED (nothing written): " + msg)
def sub1(rel, old, new, what):
    p = load(rel); t = edits[p]
    need(t.count(old) == 1, "%s: anchor found %d times in %s:\n%s" % (what, t.count(old), rel, old[:300]))
    edits[p] = t.replace(old, new)
def insert_before_default(rel, func_anchor, new, what):   # first "   default:" after the anchor
    p = load(rel); t = edits[p]
    i = t.find(func_anchor); need(i >= 0, what + ": function anchor missing in " + rel + ": " + func_anchor)
    j = t.find("   default:\n", i); need(j >= 0, what + ": no default: after the anchor")
    edits[p] = t[:j] + new + t[j:]

def diag():
    # diagnostics go straight into the title's trace (ps5_trace in the executable); second version marker PS5_ZINK_DIAG2
    rel = "src/egl/drivers/dri2/egl_dri2.c"
    t = load(rel)
    t = edits[t]
    if "PS5_ZINK_DIAG2" in t: return
    t = re.sub(r'   _eglLog\(_EGL_WARNING, "PS5_ZINK_DIAG: [^\n]*\n', "", t)   # the first version (Mesa's own log did not reach the trace)
    edits[os.path.join(src, rel)] = t
    sub1(rel, "   unsigned int format_count[ARRAY_SIZE(dri2_pbuffer_visuals)] = {0};\n",
         "   unsigned int format_count[ARRAY_SIZE(dri2_pbuffer_visuals)] = {0};\n"
         "   { extern void ps5_trace(const char *); char b[200]; snprintf(b, sizeof b, \"PS5_ZINK_DIAG2 add_pbuffer_configs: kopper=%d swrast=%d driver=%s\", (int)dri2_dpy->kopper, (int)dri2_dpy->swrast, dri2_dpy->driver_name ? dri2_dpy->driver_name : \"?\"); ps5_trace(b); }\n", "diag head")
    sub1(rel, "                                  attr_list);\n      if (dri2_conf)\n         format_count[idx]++;",
         "                                  attr_list);\n"
         "      if (i < 40) { extern void ps5_trace(const char *); char b[200]; snprintf(b, sizeof b, \"PS5_ZINK_DIAG2 driver config %u: double=%d idx=%d -> egl config %p surface=0x%x\", i, (int)gl_config->doubleBufferMode, idx, (void *)dri2_conf, dri2_conf ? (unsigned)dri2_conf->base.SurfaceType : 0u); ps5_trace(b); }\n"
         "      if (dri2_conf)\n         format_count[idx]++;", "diag per config")

def extra():
    # PS5_ZINK_DRIVER: the surfaceless software fallback picked "swrast" (kopper off, so no window configs / no native present); on the console the software path IS Zink
    rel = "src/egl/drivers/dri2/platform_surfaceless.c"
    t = open(os.path.join(src, rel)).read()
    if "PS5_ZINK_DRIVER" in t: return
    sub1(rel, 'dri2_dpy->driver_name = strdup(disp->Options.Zink ? "zink" : "swrast");',
         'dri2_dpy->driver_name = strdup("zink"); /* PS5_ZINK_DRIVER */', "driver name")

def extra2():
    # PS5_ZINK_WINDOW: eglapi.c refuses window surfaces on the surfaceless platform (EGL_MESA_platform_surfaceless); on the console the surfaceless display presents through kopper
    rel = "src/egl/main/eglapi.c"
    t = open(os.path.join(src, rel)).read()
    if "PS5_ZINK_WINDOW" in t: return
    sub1(rel, "   if (disp && (disp->Platform == _EGL_PLATFORM_SURFACELESS ||\n                disp->Platform == _EGL_PLATFORM_DEVICE)) {\n      /* From the EGL_MESA_platform_surfaceless spec (v1):\n       *\n       *    eglCreatePlatformWindowSurface fails",
         "   if (0 /* PS5_ZINK_WINDOW */ && disp && (disp->Platform == _EGL_PLATFORM_SURFACELESS ||\n                disp->Platform == _EGL_PLATFORM_DEVICE)) {\n      /* From the EGL_MESA_platform_surfaceless spec (v1):\n       *\n       *    eglCreatePlatformWindowSurface fails", "eglapi window check")

def extra3():
    # PS5_ZINK_CACHE: Mesa's disk cache is off when getuid() != geteuid() (the jailbroken process is not a "normal user"); the console's cache directory is ours anyway
    rel = "src/util/disk_cache_os.c"
    t = open(os.path.join(src, rel)).read()
    if "PS5_ZINK_CACHE" not in t:
        sub1(rel, "   if (!__normal_user())\n      return false;\n\n   /* At user request, disable shader cache entirely.",
             "   /* PS5_ZINK_CACHE: no __normal_user() check */\n\n   /* At user request, disable shader cache entirely.", "disk_cache_enabled")
    rel = "src/gallium/drivers/zink/zink_screen.c"
    t = open(os.path.join(src, rel)).read()
    if "PS5_ZINK_CACHE" not in t:
        sub1(rel, '   screen->disk_cache = disk_cache_create("zink", cache_id, 0);\n',
             '   screen->disk_cache = disk_cache_create("zink", cache_id, 0);\n   { extern void ps5_trace(const char *); ps5_trace(screen->disk_cache ? "zink: Mesa disk cache created /* PS5_ZINK_CACHE */" : "zink: Mesa disk cache NOT created (disk_cache_create failed)"); }\n', "zink disk cache trace")

main_done = M in open(os.path.join(src, "include/kopper_interface.h")).read()
if main_done:
    diag(); extra(); extra2(); extra3()
    if edits:
        for p, t in edits.items(): open(p, "w").write(t)
        print("diagnostics added in %s" % src)
    else: print("already patched:", src)
    sys.exit(0)
# --- kopper_interface.h
sub1("include/kopper_interface.h", "#define __DRI_KOPPER_LOADER_VERSION 0\n",
     "#define __DRI_KOPPER_LOADER_VERSION 0\n/* %s: surface-create-info type of the console display (not a Khronos structure type) */\n#define PS5_STRUCTURE_TYPE_DISPLAY_SURFACE ((VkStructureType)0x50530001)\n" % M, "kopper_interface.h")
# --- zink_kopper.h
sub1("src/gallium/drivers/zink/zink_kopper.h", "   KOPPER_WIN32\n", "   KOPPER_WIN32,\n   KOPPER_PS5 /* %s */\n" % M, "zink_kopper.h enum")
# --- zink_kopper.c
K = "src/gallium/drivers/zink/zink_kopper.c"
sub1(K, "static void\ninit_dt_type(struct kopper_displaytarget *cdt)",
     "/* %s: implemented by the title's shim (SDL_Vulkan_CreateSurface's VK_KHR_display code) */\nextern VkResult ps5_zink_create_display_surface(VkInstance, VkSurfaceKHR *);\n\nstatic void\ninit_dt_type(struct kopper_displaytarget *cdt)" % M, "kopper.c decl")
insert_before_default(K, "init_dt_type(struct kopper_displaytarget *cdt)\n{",
     "   case PS5_STRUCTURE_TYPE_DISPLAY_SURFACE:\n      cdt->type = KOPPER_PS5;\n      break;\n", "init_dt_type")
insert_before_default(K, "kopper_CreateSurface(struct zink_screen *screen",
     "   case PS5_STRUCTURE_TYPE_DISPLAY_SURFACE:\n      error = ps5_zink_create_display_surface(screen->instance, &surface);\n      break;\n", "kopper_CreateSurface")
sub1(K, "   VkBool32 supported;\n   error = VKSCR(GetPhysicalDeviceSurfaceSupportKHR)(screen->pdev, screen->gfx_queue, surface, &supported);\n   if (!zink_screen_handle_vkresult(screen, error) || !supported)\n      goto fail;\n",
     "   VkBool32 supported = VK_TRUE;\n   if (cdt->type != KOPPER_PS5) { /* %s: the display surface is for queue family 0 (graphics) by construction */\n      error = VKSCR(GetPhysicalDeviceSurfaceSupportKHR)(screen->pdev, screen->gfx_queue, surface, &supported);\n      if (!zink_screen_handle_vkresult(screen, error) || !supported)\n         goto fail;\n   }\n" % M, "surface support check")
insert_before_default(K, "find_dt_entry(struct zink_screen *screen",
     "   case KOPPER_PS5:\n      he = _mesa_hash_table_search(&screen->dts, cdt);\n      break;\n", "find_dt_entry")
insert_before_default(K, "simple_mtx_lock(&screen->dt_lock);\n   switch (cdt->type) {",
     "   case KOPPER_PS5:\n      _mesa_hash_table_insert(&screen->dts, cdt, cdt);\n      break;\n", "dts insert")
p = load(K); t = edits[p]
i = t.find("if (unlikely(!screen->dts.table)) {"); need(i >= 0, "dts init block not found")
m = re.compile(r"switch \((?:cdt->type|k\.type)\) \{\n").search(t, i, i + 400); need(m, "dts init has no switch on the kopper type near it:\n" + t[i:i+500])
j = m.end()
edits[p] = t[:j] + "         case KOPPER_PS5:\n            _mesa_hash_table_init(&screen->dts, screen, _mesa_hash_pointer, _mesa_key_pointer_equal);\n            break;\n" + t[j:]
# --- zink_instance.py
sub1("src/gallium/drivers/zink/zink_instance.py", '    Extension("VK_KHR_surface"),\n', '    Extension("VK_KHR_surface"),\n    Extension("VK_KHR_display"),\n', "instance extension")
# --- egl_dri2.c: window configs when kopper
sub1("src/egl/drivers/dri2/egl_dri2.c", "      dri2_conf = dri2_add_config(disp, dri2_dpy->driver_configs[i],\n                                  EGL_PBUFFER_BIT, attr_list);",
     "      dri2_conf = dri2_add_config(disp, dri2_dpy->driver_configs[i],\n                                  dri2_dpy->kopper ? (EGL_PBUFFER_BIT | EGL_WINDOW_BIT) : EGL_PBUFFER_BIT, /* %s */\n                                  attr_list);" % M, "pbuffer configs")
# --- platform_surfaceless.c
S = "src/egl/drivers/dri2/platform_surfaceless.c"
sub1(S, """static const struct dri2_egl_display_vtbl dri2_surfaceless_display_vtbl = {
   .create_pbuffer_surface = dri2_surfaceless_create_pbuffer_surface,
   .destroy_surface = surfaceless_destroy_surface,
   .create_image = dri2_create_image_khr,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};
""", """/* %s: window surfaces for Zink (kopper) on the console display. native_window points to int[2] { width, height }. */
static _EGLSurface *
dri2_surfaceless_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                                       void *native_window,
                                       const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const struct dri_config *config;

   if (!dri2_dpy->kopper) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "surfaceless window surfaces need Zink");
      return NULL;
   }
   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateWindowSurface");
      return NULL;
   }
   if (!dri2_init_surface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list,
                          false, native_window))
      goto cleanup_surface;
   if (native_window) {
      const int *wh = native_window;
      dri2_surf->base.Width = wh[0];
      dri2_surf->base.Height = wh[1];
   }
   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH, "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }
   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;
   dri2_surf->base.SwapInterval = 1;
   kopperSetSwapInterval(dri2_surf->dri_drawable, 1);
   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static EGLBoolean
dri2_surfaceless_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (!dri2_surf->dri_drawable)
      return _eglError(EGL_BAD_SURFACE, "eglSwapBuffers");
   kopperSwapBuffers(dri2_surf->dri_drawable, __DRI2_FLUSH_CONTEXT | __DRI2_FLUSH_INVALIDATE_ANCILLARY);
   return EGL_TRUE;
}

static EGLBoolean
dri2_surfaceless_swap_interval(_EGLDisplay *disp, _EGLSurface *surf, EGLint interval)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   if (dri2_surf->dri_drawable)
      kopperSetSwapInterval(dri2_surf->dri_drawable, interval);
   return EGL_TRUE;
}

static void
surfaceless_kopper_set_surface_create_info(void *_draw, struct kopper_loader_info *ci)
{
   struct dri2_egl_surface *dri2_surf = _draw;

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return; /* pbuffers: no surface */
   ci->bos.sType = PS5_STRUCTURE_TYPE_DISPLAY_SURFACE;
   ci->bos.pNext = NULL;
   ci->has_alpha = 0;
   ci->present_opaque = true;
}

static void
surfaceless_kopper_get_drawable_info(struct dri_drawable *draw, int *w, int *h,
                                     void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static const __DRIkopperLoaderExtension surfaceless_kopper_loader_extension = {
   .base = {__DRI_KOPPER_LOADER, 1},
   .SetSurfaceCreateInfo = surfaceless_kopper_set_surface_create_info,
   .GetDrawableInfo = surfaceless_kopper_get_drawable_info,
};

static const struct dri2_egl_display_vtbl dri2_surfaceless_display_vtbl = {
   .create_window_surface = dri2_surfaceless_create_window_surface,
   .create_pbuffer_surface = dri2_surfaceless_create_pbuffer_surface,
   .destroy_surface = surfaceless_destroy_surface,
   .swap_buffers = dri2_surfaceless_swap_buffers,
   .swap_interval = dri2_surfaceless_swap_interval,
   .create_image = dri2_create_image_khr,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};
""" % M, "surfaceless vtbl")
sub1(S, "   &kopper_pbuffer_loader_extension.base, &image_lookup_extension.base,\n   &image_lookup_extension.base, NULL,",
     "   &surfaceless_kopper_loader_extension.base, &image_lookup_extension.base,\n   &image_lookup_extension.base, NULL,", "kopper loader list")

diag(); extra(); extra2(); extra3()
for p, t in edits.items(): open(p, "w").write(t)
if edits: print("patched %d files in %s" % (len(edits), src)); [print("  " + os.path.relpath(p, src)) for p in sorted(edits)]
