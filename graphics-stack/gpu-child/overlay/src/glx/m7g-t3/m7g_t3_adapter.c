/* M7g T3 — private Steam-compatibility GLX/drisw/Zink presentation adapter.
 *
 * Task-local; enabled only by M7G_T3_ADAPTER=1 (see src/util/m7g_t3.h).
 *
 * Purpose: let ordinary private Mesa GLX clients (Steam/CEF and similar) use
 * their REAL window drawables, configs and contexts with Zink GPU rendering,
 * while the accepted OPAQUE_FD transport + Gamescope X11 content override
 * delivers the default-framebuffer content.  No client WSI/Kopper presentation.
 *
 * T3 scope (differences from the frozen T2 proof adapter):
 *   - arbitrary real pixels: no pattern/FNV expectation table, no frame cap,
 *     no artificial pacing or final hold; diagnostics only;
 *   - multiple window drawables per process, keyed by (Display*, XID); each owns
 *     its own transport, epoch and generation counter, bound to the real XID;
 *   - per-drawable size (16..4096, <=64 MiB); resize recreates the transport
 *     bounded (drain/destroy/init);
 *   - offscreen/pixmap drawables are NOT transported (xcb query; no X errors);
 *   - retired swapchain: one bounded recreate attempt, then honest fail (rc29);
 *   - arbitrary caller GL state: the readback normalizes READ_FRAMEBUFFER to the
 *     window default framebuffer, READ_BUFFER to GL_BACK, the pixel pack buffer
 *     to 0 (CPU destination) and the pack state to tight rows, and restores all
 *     of it; a 64-byte canary after the w*h*4 buffer detects any overrun;
 *   - separate GLX read drawable (currentReadable != this window): fail-closed
 *     BEFORE capture (rc45), because FBO 0 read side would not be this window;
 *   - transport event progress during the stream: bounded service(0) before the
 *     retired check and after every commit (release/returned/retired/protocol
 *     events); service failure is an explicit FAIL, never a hidden fallback;
 *   - live identity/provenance: T3_IDENTITY (pid/exe/starttime/run_tag/WM/XID/
 *     server_id/epoch/loaded libGLX) and T3_RENDERER once per process; every
 *     T3_SWAP carries epoch and a CLOCK_MONOTONIC nanosecond timestamp.
 *
 * Retained contract: current drawable/context checks, windows-only presentation,
 * per-slot immutable registration, ready/returned semaphores, external
 * ownership barriers, bounded reuse, fail-closed teardown.  Adapter failures:
 * "T3_FAIL stage=... rc=..." then _exit(rc) (fail-closed; no silent skip).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>

#include "glxclient.h"
#include "m7g_t3_transport.h"
#include "m9w-gpu.h"
#include "glapi.h"
#include "m7g_t3_adapter.h"

/* gl.h in this tree declares only through GL 2.1 prototypes; these two core
 * entry points are exported by libGL and needed for state isolation. */
extern void glBindFramebuffer(GLenum target, GLuint framebuffer);
extern void glBindBuffer(GLenum target, GLuint buffer);

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#endif
#ifndef GL_PACK_SKIP_ROWS
#define GL_PACK_SKIP_ROWS 0x0D03
#endif
#ifndef GL_PACK_SKIP_PIXELS
#define GL_PACK_SKIP_PIXELS 0x0D04
#endif
#ifndef GL_PACK_IMAGE_HEIGHT
#define GL_PACK_IMAGE_HEIGHT 0x806C
#endif
#ifndef GL_PACK_SKIP_IMAGES
#define GL_PACK_SKIP_IMAGES 0x806D
#endif
#ifndef GL_PACK_LSB_FIRST
#define GL_PACK_LSB_FIRST 0x0D01
#endif
#ifndef GL_PACK_SWAP_BYTES
#define GL_PACK_SWAP_BYTES 0x0D00
#endif

#define M7G_T3_MAX_DRAWABLES 16u
#define M7G_T3_CANARY 64u

struct t3_drawable {
    Display         *dpy;
    XID              xid;
    XID              gpu_parent;
    m7g_t3_xport    *xp;
    m9w_gpu         *gpu;
    unsigned         w, h;
    unsigned         gen;
    unsigned         epoch;   /* transport instance number (open/resize/recover) */
    unsigned         swaps;
    unsigned char   *pixels;
    size_t           pixels_cap; /* need + canary */
    unsigned         retired_burst;
    int              open_logged;
};

static pthread_mutex_t t3_lock = PTHREAD_MUTEX_INITIALIZER;
static struct t3_drawable t3_slots[M7G_T3_MAX_DRAWABLES];
static int t3_engaged_logged;
static int t3_nonwindow_logged;
static int t3_renderer_logged;
static unsigned t3_total_swaps;
static int m9w_enabled(void) { const char *e=getenv("M9W_GPU_CHILD");return e&&!strcmp(e,"1"); }
static m9w_proc m9w_get(const char *s) { return (m9w_proc)_glapi_get_proc_address(s); }
static void t3_fail(const char *,int);
static void m9w_close_gpu(struct t3_drawable *d) {
 if(d->gpu) {int rc=m9w_gpu_destroy(d->gpu,__glXGetCurrentContext());if(rc)t3_fail("gpu-close",rc);d->gpu=NULL;}
}


static void
t3_fail(const char *stage, int rc)
{
    fflush(stdout);
    fprintf(stdout, "T3_FAIL stage=%s rc=%d (adapter)\n", stage, rc);
    fflush(stdout);
    _exit(rc);
}

static unsigned long long
t3_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static void
t3_sanitize(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7e)
            *s = '_';
    }
}

/* ------------------------------------------------------------ window query */
/* xcb-only query: consumes BadDrawable/BadWindow error cookies instead of
 * triggering the Xlib default error handler. Returns 1 for a mapped (viewable)
 * window, 0 for non-window/unmapped, -1 on query error. */
static int
t3_query_window(Display *dpy, XID xid, unsigned *w, unsigned *h)
{
    xcb_connection_t *conn = XGetXCBConnection(dpy);
    xcb_get_window_attributes_cookie_t cookie = xcb_get_window_attributes(conn, xid);
    xcb_generic_error_t *err = NULL;
    xcb_get_window_attributes_reply_t *rep =
        xcb_get_window_attributes_reply(conn, cookie, &err);
    if (err) {
        free(err);
        return 0; /* not a window (pixmap) or gone: no presentation */
    }
    if (!rep)
        return -1;
    if (rep->map_state != XCB_MAP_STATE_VIEWABLE) {
        free(rep);
        return 0;
    }
    free(rep);

    /* window attributes carry no size; geometry has it (same error-cookie style) */
    xcb_get_geometry_cookie_t gcookie = xcb_get_geometry(conn, xid);
    xcb_generic_error_t *gerr = NULL;
    xcb_get_geometry_reply_t *grep = xcb_get_geometry_reply(conn, gcookie, &gerr);
    if (gerr) {
        free(gerr);
        return 0;
    }
    if (!grep)
        return -1;
    if (grep->width < 16 || grep->height < 16) {
        free(grep);
        return 0;
    }
    *w = grep->width;
    *h = grep->height;
    free(grep);
    return 1;
}

/* -------------------------------------------------------- X identity (cheap) */
static xcb_atom_t
t3_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t c = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_generic_error_t *err = NULL;
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, &err);
    xcb_atom_t a = 0;
    if (err) {
        free(err);
        return 0;
    }
    if (r) {
        a = r->atom;
        free(r);
    }
    return a;
}

/* returns property format (8/32) or 0; copies up to cap bytes into out */
static int
t3_prop(xcb_connection_t *conn, xcb_window_t w, xcb_atom_t prop, uint8_t *out,
        size_t cap, size_t *outlen)
{
    *outlen = 0;
    if (!prop)
        return 0;
    xcb_get_property_cookie_t c = xcb_get_property(conn, 0, w, prop,
                                                   XCB_GET_PROPERTY_TYPE_ANY, 0, 64);
    xcb_generic_error_t *err = NULL;
    xcb_get_property_reply_t *r = xcb_get_property_reply(conn, c, &err);
    if (err) {
        free(err);
        return 0;
    }
    if (!r)
        return 0;
    int len = xcb_get_property_value_length(r);
    const uint8_t *v = xcb_get_property_value(r);
    if (v && len > 0) {
        size_t m = (size_t)len;
        if (m > cap)
            m = cap;
        memcpy(out, v, m);
        *outlen = m;
    }
    int fmt = r->format;
    free(r);
    return fmt;
}

static void
t3_x11_identity(Display *dpy, XID xid, long *wm_pid, char *wm_class, size_t cc,
                char *wm_name, size_t nc)
{
    xcb_connection_t *conn = XGetXCBConnection(dpy);
    xcb_atom_t a_pid = t3_atom(conn, "_NET_WM_PID");
    xcb_atom_t a_class = t3_atom(conn, "WM_CLASS");
    xcb_atom_t a_netname = t3_atom(conn, "_NET_WM_NAME");
    xcb_atom_t a_name = t3_atom(conn, "WM_NAME");
    uint8_t buf[128];
    size_t n = 0;

    *wm_pid = -1;
    wm_class[0] = '\0';
    wm_name[0] = '\0';

    if (t3_prop(conn, xid, a_pid, buf, sizeof(buf), &n) == 32 && n >= 4) {
        uint32_t v = 0;
        memcpy(&v, buf, 4);
        *wm_pid = (long)v;
    }
    if (t3_prop(conn, xid, a_class, buf, sizeof(buf), &n) == 8 && n > 0) {
        size_t m = n > cc - 1 ? cc - 1 : n;
        memcpy(wm_class, buf, m);
        wm_class[m] = '\0';
        t3_sanitize(wm_class);
    }
    if (t3_prop(conn, xid, a_netname, buf, sizeof(buf), &n) == 8 && n > 0) {
        size_t m = n > nc - 1 ? nc - 1 : n;
        memcpy(wm_name, buf, m);
        wm_name[m] = '\0';
    } else if (t3_prop(conn, xid, a_name, buf, sizeof(buf), &n) == 8 && n > 0) {
        size_t m = n > nc - 1 ? nc - 1 : n;
        memcpy(wm_name, buf, m);
        wm_name[m] = '\0';
    }
    t3_sanitize(wm_name);
}

static void
t3_read_starttime(char *out, size_t cap)
{
    char buf[1024];
    out[0] = '\0';
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (!p)
        return;
    p++;
    /* the 20th token after comm is field 22 = starttime */
    for (int i = 0; i < 20 && *p; i++) {
        while (*p == ' ')
            p++;
        char *tok = p;
        while (*p && *p != ' ')
            p++;
        if (i == 19) {
            size_t m = (size_t)(p - tok);
            if (m >= cap)
                m = cap - 1;
            memcpy(out, tok, m);
            out[m] = '\0';
            return;
        }
    }
}

static unsigned long
t3_read_server_id(Display *dpy)
{
    int scr = DefaultScreen(dpy);
    Atom a = XInternAtom(dpy, "GAMESCOPE_XWAYLAND_SERVER_ID", False);
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    unsigned long server_id = 0;
    int ok = 0;
    if (a != None &&
        XGetWindowProperty(dpy, RootWindow(dpy, scr), a, 0, 1, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) == Success &&
        data && fmt == 32 && n >= 1) {
        server_id = *(unsigned long *)data;
        ok = 1;
    }
    if (data)
        XFree(data);
    if (!ok) {
#ifdef M7G_TEST_HOOKS
        const char *fake = getenv("M7G_T3_FAKE_SERVER_ID");
        if (fake && *fake) {
            server_id = strtoul(fake, NULL, 0);
            ok = 1;
            printf("T3_TEST fake_server_id=%lu\n", server_id);
        }
#endif
    }
    if (!ok)
        t3_fail("server-id-property", 42);
    return server_id;
}

#include "geometry-defer.h"
static struct m10k_geometry_wait m10k_geometry_waits[M7G_T3_MAX_DRAWABLES];
static XID
m9w_full_parent(Display *dpy,XID win) {
 xcb_connection_t *c=XGetXCBConnection(dpy);xcb_window_t child=win;
 for(unsigned depth=0;depth<32;depth++) {
  xcb_generic_error_t *e=NULL;
  xcb_query_tree_reply_t *t=xcb_query_tree_reply(c,xcb_query_tree(c,child),&e);
  if(e||!t){free(e);free(t);t3_fail("gpu-tree",96);}
  xcb_window_t parent=t->parent,root=t->root;free(t);
  if(parent==root){
   unsigned skipped=m10k_geometry_clear(m10k_geometry_waits,M7G_T3_MAX_DRAWABLES,win);
   if(skipped)m9w_log("M10K_GEOMETRY_RESOLVED child=0x%lx skipped=%u",(unsigned long)win,skipped);
   return child;
  }
  xcb_get_geometry_reply_t *g=xcb_get_geometry_reply(c,xcb_get_geometry(c,child),&e);
  if(e||!g){free(e);free(g);t3_fail("gpu-geometry",96);}
  xcb_get_geometry_reply_t *pg=xcb_get_geometry_reply(c,xcb_get_geometry(c,parent),&e);
  if(e||!pg){free(e);free(g);free(pg);t3_fail("gpu-parent-geometry",96);}
  int same=g->x==0&&g->y==0&&g->width==pg->width&&g->height==pg->height;
  if(!same){
   unsigned skipped=0;
   int deferred=m10k_geometry_defer(m10k_geometry_waits,M7G_T3_MAX_DRAWABLES,win,t3_now_ns(),&skipped);
   if(skipped<=3 || skipped%120==0 || deferred<0)
    m9w_log("M10K_GEOMETRY_DEFER child=0x%x xy=%d,%d size=%u,%u parent=0x%x size=%u,%u skipped=%u bounded_rc=%d",child,g->x,g->y,g->width,g->height,parent,pg->width,pg->height,skipped,deferred);
   free(g);free(pg);
   if(deferred<0)t3_fail("gpu-subrect-did-not-settle",96);
   return 0; /* No ready signal, transport commit, or slot reuse for this frame. */
  }
  free(g);free(pg);child=parent;
 }
 t3_fail("gpu-tree-depth",96);return 0;
}

static struct t3_drawable *
t3_get_free_slot(void)
{
    for (unsigned i = 0; i < M7G_T3_MAX_DRAWABLES; i++) {
        if (!t3_slots[i].dpy && !t3_slots[i].xp && !t3_slots[i].pixels)
            return &t3_slots[i];
    }
    return NULL;
}

static const char *
t3_basename(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void
t3_log_identity(struct t3_drawable *d, unsigned long server_id)
{
    long wm_pid = -1;
    char wm_class[65] = "";
    char wm_name[65] = "";
    char exe[256] = "unknown";
    char starttime[32] = "?";
    char run_tag[64] = "";
    char obs_dir[128] = "";
    const char *libglx = "?";
    Dl_info di;
    ssize_t n;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
        exe[n] = '\0';
    else
        strcpy(exe, "unknown");
    t3_read_starttime(starttime, sizeof(starttime));
    {
        /* run_tag = the wrapper probe-unit run id (explicit binding supplied by
         * the thin); observer_dir = the thin's own tmp dir, a DISTINCT id bound
         * through the captured thin binding line. */
        const char *rt = getenv("M7G_ZINK_T3_WRAPPER_RUN_ID");
        const char *od = getenv("M7G_ZINK_T3_RUNDIR");
        if (rt && *rt) {
            strncpy(run_tag, rt, sizeof(run_tag) - 1);
            run_tag[sizeof(run_tag) - 1] = '\0';
        }
        if (od && *od) {
            strncpy(obs_dir, od, sizeof(obs_dir) - 1);
            obs_dir[sizeof(obs_dir) - 1] = '\0';
        }
    }
    if (dladdr((void *)&m7g_t3_adapter_swap, &di) && di.dli_fname)
        libglx = di.dli_fname;
    t3_x11_identity(d->dpy, d->xid, &wm_pid, wm_class, sizeof(wm_class),
                    wm_name, sizeof(wm_name));
    printf("T3_IDENTITY pid=%ld exe=%s starttime=%s run_tag=%s observer_dir=%s xid=0x%lx server_id=%lu epoch=%u wm_pid=%ld wm_class=\"%s\" wm_name=\"%s\" libglx=%s\n",
           (long)getpid(), exe, starttime, run_tag, obs_dir, (unsigned long)d->xid,
           server_id, d->epoch, wm_pid, wm_class, wm_name, libglx);
}

static void
t3_log_renderer(void)
{
    const GLubyte *r = glGetString(GL_RENDERER);
    const GLubyte *v = glGetString(GL_VERSION);
    const GLubyte *vd = glGetString(GL_VENDOR);
    char rb[160] = "(null)", vb[96] = "(null)", vdb[96] = "(null)";
    if (r) { strncpy(rb, (const char *)r, sizeof(rb) - 1); rb[sizeof(rb) - 1] = 0; }
    if (v) { strncpy(vb, (const char *)v, sizeof(vb) - 1); vb[sizeof(vb) - 1] = 0; }
    if (vd) { strncpy(vdb, (const char *)vd, sizeof(vdb) - 1); vdb[sizeof(vdb) - 1] = 0; }
    t3_sanitize(rb);
    t3_sanitize(vb);
    t3_sanitize(vdb);
    printf("T3_RENDERER pid=%ld renderer=\"%s\" version=\"%s\" vendor=\"%s\" t_ns=%llu\n",
           (long)getpid(), rb, vb, vdb, t3_now_ns());
}

/* Create/recreate the per-drawable transport at the current size. */
static void
t3_open_transport(struct t3_drawable *d, unsigned w, unsigned h)
{
    unsigned long server_id;
    int rc;

    if (d->xp) {
        m9w_log("M10K_LIFE event=reopen-begin xid=0x%lx epoch=%u old=%ux%u new=%ux%u",(unsigned long)d->xid,d->epoch,d->w,d->h,w,h);
        m9w_close_gpu(d);
        rc = m7g_t3_xport_destroy(d->xp);
        d->xp = NULL;
        if (rc)
            t3_fail("resize-teardown", rc);
    }
    if (!m9w_enabled() && d->pixels_cap < (size_t)w * h * 4u + M7G_T3_CANARY) {
        unsigned char *np = realloc(d->pixels, (size_t)w * h * 4u + M7G_T3_CANARY);
        if (!np)
            t3_fail("readback-alloc", 40);
        d->pixels = np;
        d->pixels_cap = (size_t)w * h * 4u + M7G_T3_CANARY;
    }
    d->epoch++;
    server_id = t3_read_server_id(d->dpy);
    rc = m7g_t3_xport_init(&d->xp, (uint32_t)server_id, (uint32_t)d->xid, w, h);
    if (rc != 0)
        t3_fail("transport-init", rc);
    d->w = w;
    d->h = h;
    if(m9w_enabled()) {rc=m9w_gpu_init(&d->gpu,d->xp,m9w_get,__glXGetCurrentContext());if(rc)t3_fail("gpu-init",rc);}
    printf("T3_DRAWABLE_OPEN xid=0x%lx w=%u h=%u server_id=%lu epoch=%u\n",
           (unsigned long)d->xid, w, h, server_id, d->epoch);
    t3_log_identity(d, server_id);
    if (!t3_renderer_logged) {
        t3_renderer_logged = 1;
        t3_log_renderer();
    }
    if (!t3_engaged_logged) {
        const char *rt = getenv("M7G_ZINK_T3_WRAPPER_RUN_ID");
        const char *od = getenv("M7G_ZINK_T3_RUNDIR");
        char tag[64] = "";
        char obs[128] = "";
        t3_engaged_logged = 1;
        if (rt && *rt) {
            strncpy(tag, rt, sizeof(tag) - 1);
            tag[sizeof(tag) - 1] = '\0';
        }
        if (od && *od) {
            strncpy(obs, od, sizeof(obs) - 1);
            obs[sizeof(obs) - 1] = '\0';
        }
        printf("T3_ADAPTER_ENGAGED backend=%s run_tag=%s observer_dir=%s pid=%ld\n",
               m7g_t3_xport_backend(), tag, obs, (long)getpid());
    }
}

/* ------------------------------------------------------ default-FB readback */
/* Reads this window's default framebuffer into d->pixels (tight w*h*4) while
 * preserving/normalizing/restoring the caller's read framebuffer, read buffer,
 * pack buffer and pack state.  Fail-closed on any adapter GL error or canary
 * overrun. */
static void
t3_readback_default_fb(struct t3_drawable *d, unsigned w, unsigned h)
{
    GLint old_fb = 0, old_read_buf = 0, old_pbo = 0;
    GLint old_align = 0, old_row = 0, old_skip_rows = 0, old_skip_px = 0;
    GLint old_img_h = 0, old_skip_img = 0, old_lsb = 0, old_swap = 0;
    size_t need = (size_t)w * h * 4u;

    /* drop any pending app-side errors: they are not ours to attribute */
    while (glGetError() != GL_NO_ERROR)
        ;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);              /* the window's default FB */
    glGetIntegerv(GL_READ_BUFFER, &old_read_buf);           /* default-FB context state */
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_skip_px);
    glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &old_img_h);
    glGetIntegerv(GL_PACK_SKIP_IMAGES, &old_skip_img);
    glGetIntegerv(GL_PACK_LSB_FIRST, &old_lsb);
    glGetIntegerv(GL_PACK_SWAP_BYTES, &old_swap);

    /* normalize: CPU destination, tight rows (w*4 is 4-aligned), GL_BACK */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);

    if (d->gen == 0) {
        GLint rf = 0, rt = 0;
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rf);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rt);
        printf("T3_READ_FORMAT xid=0x%lx format=0x%x type=0x%x old_read_fb=%d\n",
               (unsigned long)d->xid, (unsigned)rf, (unsigned)rt, (int)old_fb);
    }

    memset(d->pixels + need, 0xA5, M7G_T3_CANARY);
    glFinish();
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, d->pixels);
    if (glGetError() != GL_NO_ERROR)
        t3_fail("readback-gl-error", 43);
    for (unsigned i = 0; i < M7G_T3_CANARY; i++) {
        if (d->pixels[need + i] != 0xA5)
            t3_fail("readback-overrun-canary", 46);
    }

    /* restore exactly what the caller had */
    glPixelStorei(GL_PACK_ALIGNMENT, old_align);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_skip_px);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, old_img_h);
    glPixelStorei(GL_PACK_SKIP_IMAGES, old_skip_img);
    glPixelStorei(GL_PACK_LSB_FIRST, old_lsb);
    glPixelStorei(GL_PACK_SWAP_BYTES, old_swap);
    if (old_pbo)
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pbo);
    glReadBuffer((GLenum)old_read_buf);          /* still FB 0: legal context state */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_fb);
    if (glGetError() != GL_NO_ERROR)
        t3_fail("readback-restore-gl-error", 43);
}

/* Save the actual already-read CPU transfer bytes, no additional GL/GPU call.
 * Called under T3's existing mutex. Optional diagnostic, process owner + token
 * scoped by the private runner. Immutable O_EXCL files; bounded total size. */
static void
m9r_capture_frame(const char *kind,XID xid,unsigned w,unsigned h,const unsigned char *pixels)
{
    const char *dir=getenv("M9R_T3_FRAME_DIR"),*token=getenv("STEAM_ZINK_TOKEN");
    if(!dir)return;
    if(dir[0]!='/' || !token || strlen(token)!=32)t3_fail("frame-config",89);
    for(unsigned i=0;i<32;i++)if(!((token[i]>='0'&&token[i]<='9')||(token[i]>='a'&&token[i]<='f')||(token[i]>='A'&&token[i]<='F')))t3_fail("frame-token",89);
    static unsigned count;static size_t total;
    size_t bytes=(size_t)w*h*4;
    if(!w||!h||w>4096||h>4096||bytes>4u*1024u*1024u){printf("M9R_FRAME_SKIP size kind=%s w=%u h=%u\n",kind,w,h);return;}
    if(count>=4||total+bytes>8u*1024u*1024u)return;
    uint32_t hash=2166136261u;size_t rgb=0,alpha=0;int varied=0;
    for(size_t i=0;i<bytes;i++){hash=(hash^pixels[i])*16777619u;}
    for(size_t i=0;i<bytes;i+=4){if(pixels[i]||pixels[i+1]||pixels[i+2])rgb++;if(pixels[i+3])alpha++;if(memcmp(pixels,pixels+i,4))varied=1;}
    /* Keep first startup frame, then reserve remaining slots for real content. */
    if(count && (!varied||!rgb))return;
    int saved=errno;
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(d<0)t3_fail("frame-directory",89);
    char name[128];int n=snprintf(name,sizeof name,"t3-frame.%ld.%u.%s.%ux%u.rgba",(long)getpid(),count,kind,w,h);
    if(n<1||n>=(int)sizeof name)t3_fail("frame-name",89);
    int fd=openat(d,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);close(d);
    if(fd<0)t3_fail("frame-open",89);
    size_t done=0;
    while(done<bytes){ssize_t rc=write(fd,pixels+done,bytes-done);if(rc<0&&errno==EINTR)continue;if(rc<=0)break;done+=(size_t)rc;}
    int rc=close(fd);if(done!=bytes||rc)t3_fail("frame-write",89);
    struct timespec mono,boot,real,raw;clock_gettime(CLOCK_MONOTONIC,&mono);clock_gettime(CLOCK_BOOTTIME,&boot);clock_gettime(CLOCK_REALTIME,&real);clock_gettime(CLOCK_MONOTONIC_RAW,&raw);
    printf("M9R_FRAME_CAPTURED pid=%ld kind=%s xid=0x%lx file=%s w=%u h=%u bytes=%lu rgb_nonzero=%lu alpha_nonzero=%lu rgba_fnv=0x%08x mono=%ld.%09ld boot=%ld.%09ld raw=%ld.%09ld utc=%ld.%09ld origin=bottom-left\n",
        (long)getpid(),kind,(unsigned long)xid,name,w,h,(unsigned long)bytes,(unsigned long)rgb,(unsigned long)alpha,hash,
        (long)mono.tv_sec,mono.tv_nsec,(long)boot.tv_sec,boot.tv_nsec,(long)raw.tv_sec,raw.tv_nsec,(long)real.tv_sec,real.tv_nsec);
    count++;total+=bytes;errno=saved;
}

/* M9R opt-in transfer, never a CPU renderer. CEF GPU renders with real Zink;
 * upload its child-window pixels for CBrowserComposerGL texture_from_pixmap.
 * Only the browser's final top-level framebuffer uses T3 OPAQUE_FD. */
static int
m9r_child_upload_enabled(void)
{
    const char *e = getenv("M9R_CEF_CHILD_UPLOAD");
    if (!e || strcmp(e, "1"))
        return 0;
    char args[8192];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) t3_fail("child-cmdline-open", 88);
    ssize_t n = read(fd, args, sizeof(args));
    close(fd);
    if (n <= 0 || n == sizeof(args) || args[n-1] != 0)
        t3_fail("child-cmdline-bounds", 88);
    for (ssize_t i = 0; i < n; i += strlen(args+i)+1)
        if (!strcmp(args+i, "--type=gpu-process")) return 1;
    return 0;
}

static int
m9r_checked(xcb_connection_t *c, xcb_void_cookie_t cookie)
{
    xcb_generic_error_t *e = xcb_request_check(c, cookie);
    int ok = !e && !xcb_connection_has_error(c);
    if (e) {
        fprintf(stderr,"M9R_CHILD_XERROR code=%u major=%u minor=%u\n",
                e->error_code,e->major_code,e->minor_code);
        free(e);
    }
    return ok;
}

/* Required, honest format contract: actual server image byte-order, visual
 * masks and depth/pixmap-format. No guessed layout or advertised capability. */
static int
m9r_child_format(xcb_connection_t *c, xcb_visualid_t vid, unsigned depth)
{
    const xcb_setup_t *s = xcb_get_setup(c);
    if (!s || s->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST) return 0;
    int fmt = 0, visual = 0;
    for (xcb_format_iterator_t f=xcb_setup_pixmap_formats_iterator(s); f.rem; xcb_format_next(&f))
        if (f.data->depth == depth && f.data->bits_per_pixel == 32 && f.data->scanline_pad == 32) fmt=1;
    for (xcb_screen_iterator_t sc=xcb_setup_roots_iterator(s); sc.rem; xcb_screen_next(&sc))
        for (xcb_depth_iterator_t d=xcb_screen_allowed_depths_iterator(sc.data); d.rem; xcb_depth_next(&d))
            for (xcb_visualtype_iterator_t v=xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v))
                if (v.data->visual_id==vid && d.data->depth==depth &&
                    v.data->_class==XCB_VISUAL_CLASS_TRUE_COLOR &&
                    v.data->red_mask==0xff0000 && v.data->green_mask==0xff00 && v.data->blue_mask==0xff) visual=1;
    return fmt && visual && (depth==24 || depth==32);
}

static void
m9r_rgba_to_x(uint8_t *dst, const uint8_t *src, unsigned w, unsigned h)
{
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        size_t di=((size_t)y*w+x)*4, si=((size_t)(h-1-y)*w+x)*4;
        dst[di]=src[si+2];dst[di+1]=src[si+1];dst[di+2]=src[si];dst[di+3]=src[si+3];
    }
}

static int
m9r_child_upload(__GLXDRIdrawable *p)
{
    xcb_connection_t *c=XGetXCBConnection(p->psc->dpy);
    xcb_generic_error_t *err=NULL;
    xcb_query_tree_reply_t *tree=xcb_query_tree_reply(c,xcb_query_tree(c,p->xDrawable),&err);
    if (err || !tree) {free(err);free(tree);t3_fail("child-tree",88);}
    xcb_window_t parent=tree->parent, root=tree->root;
    free(tree);
    if (parent==root) return 0; /* Actual top-level still follows accepted T3. */
    xcb_get_window_attributes_reply_t *attr=xcb_get_window_attributes_reply(c,xcb_get_window_attributes(c,p->xDrawable),&err);
    if (err || !attr) {free(err);free(attr);t3_fail("child-attributes",88);}
    xcb_visualid_t vid=attr->visual;unsigned mapped=attr->map_state;
    free(attr);
    xcb_get_geometry_reply_t *geo=xcb_get_geometry_reply(c,xcb_get_geometry(c,p->xDrawable),&err);
    if (err || !geo) {free(err);free(geo);t3_fail("child-geometry",88);}
    unsigned w=geo->width,h=geo->height,depth=geo->depth;
    free(geo);
    if (!w || !h || w>4096 || h>4096 || !m9r_child_format(c,vid,depth))
        t3_fail("child-format-or-size",88);
    size_t bytes=(size_t)w*h*4;
    struct t3_drawable d={.dpy=p->psc->dpy,.xid=p->xDrawable};
    d.pixels_cap=bytes+M7G_T3_CANARY;d.pixels=malloc(d.pixels_cap);
    uint8_t *upload=malloc(bytes);
    if (!d.pixels || !upload) t3_fail("child-allocate",88);
    pthread_mutex_lock(&t3_lock);
    t3_readback_default_fb(&d,w,h);
    m9r_capture_frame("child",d.xid,w,h,d.pixels);
    m9r_rgba_to_x(upload,d.pixels,w,h);
    xcb_gcontext_t gc=xcb_generate_id(c);
    uint32_t values[]={0}; /* graphics_exposures=false */
    if (!m9r_checked(c,xcb_create_gc_checked(c,gc,p->xDrawable,XCB_GC_GRAPHICS_EXPOSURES,values)))
        t3_fail("child-create-gc",88);
    uint32_t max=xcb_get_maximum_request_length(c);
    /* Conservative core request framing; bounded full rows, no truncation. */
    unsigned rows=(max>16 ? ((uint64_t)max*4-64)/((uint64_t)w*4) : 0);
    if (!rows) t3_fail("child-request-size",88);
    for (unsigned y=0;y<h;) {
        unsigned count=h-y;if(count>rows)count=rows;
        if (!m9r_checked(c,xcb_put_image_checked(c,XCB_IMAGE_FORMAT_Z_PIXMAP,p->xDrawable,gc,w,count,0,y,0,depth,(uint32_t)((size_t)count*w*4),upload+(size_t)y*w*4)))
            t3_fail("child-put-image",88);
        y+=count;
    }
    if (!m9r_checked(c,xcb_free_gc_checked(c,gc))) t3_fail("child-free-gc",88);
    static unsigned copies;
    uint32_t hash=2166136261u;
    for (size_t i=0;i<bytes;i++) hash=(hash^d.pixels[i])*16777619u;
    if (++copies<=8 || copies%120==0)
        printf("M9R_CHILD_UPLOAD_OK pid=%ld xid=0x%lx parent=0x%x root=0x%x w=%u h=%u depth=%u mapped=%u count=%u rgba_fnv=0x%08x t_ns=%llu\n",
               (long)getpid(),(unsigned long)p->xDrawable,parent,root,w,h,depth,mapped,copies,hash,t3_now_ns());
    free(upload);free(d.pixels);
    pthread_mutex_unlock(&t3_lock);
    return 1;
}

/* ----------------------------------------------------------------- swap */
int
m7g_t3_adapter_swap(void *pdraw_ptr, int kopper)
{
    __GLXDRIdrawable *pdraw = (__GLXDRIdrawable *)pdraw_ptr;
    struct glx_context *gc = __glXGetCurrentContext();
    struct t3_drawable *d = NULL;
    unsigned w = 0, h = 0;
    int q;
    int rc;

    if (!kopper)
        t3_fail("screen-not-kopper", 40);

    const char *drain=getenv("M9W_DRAIN_FILE");
    if(m9w_enabled() && drain && access(drain,F_OK)==0) {
        m9w_before_switch(__glXGetCurrentContext(),0);
        static int acknowledged=0;
        if(!acknowledged){acknowledged=1;m9w_log("GPU_DRAIN_ACK");}
        return 0;
    }
    /* real contract: this context current on THIS drawable */
    if (!gc || gc->currentDpy != pdraw->psc->dpy ||
        gc->currentDrawable != pdraw->drawable)
        t3_fail("current-drawable", 44);
    /* separate read drawable: FBO 0's read side would not be this window */
    if (gc->currentReadable != pdraw->drawable)
        t3_fail("current-read-drawable", 45);

    setvbuf(stdout, NULL, _IONBF, 0);

    /* CEF GPU child windows are backing sources for the browser's GL
     * compositor. They must never take the top-level content override. */
    if (!m9w_enabled() && m9r_child_upload_enabled() && m9r_child_upload(pdraw))
        return 0;

    pthread_mutex_lock(&t3_lock);

    q = t3_query_window(pdraw->psc->dpy, pdraw->xDrawable, &w, &h);
    if (q == -1) {
        pthread_mutex_unlock(&t3_lock);
        t3_fail("window-query", 41);
    }
    if (q == 0) {
        /* pixmap/unmapped: no presentation for this drawable */
        if (!t3_nonwindow_logged) {
            t3_nonwindow_logged = 1;
            printf("T3_SKIP_NONWINDOW xid=0x%lx (offscreen/pixmap/unmapped; no transport)\n",
                   (unsigned long)pdraw->xDrawable);
        }
        pthread_mutex_unlock(&t3_lock);
        return 0;
    }

    XID gpu_parent=0;
    if(m9w_enabled()) {
        gpu_parent=m9w_full_parent(pdraw->psc->dpy,pdraw->xDrawable);
        if(!gpu_parent){pthread_mutex_unlock(&t3_lock);return 0;}
        for(unsigned j=0;j<M7G_T3_MAX_DRAWABLES;j++) {
            struct t3_drawable *other=&t3_slots[j];unsigned ow=0,oh=0;
            if(other->xp && other->xid!=pdraw->xDrawable && other->gpu_parent==gpu_parent &&
               t3_query_window(other->dpy,other->xid,&ow,&oh)==1) {
                m9w_log("GPU_FAIL stage=parent-collision parent=0x%lx old=0x%lx new=0x%lx",(unsigned long)gpu_parent,(unsigned long)other->xid,(unsigned long)pdraw->xDrawable);
                t3_fail("gpu-parent-collision",96);
            }
        }
    }
    for (unsigned i = 0; i < M7G_T3_MAX_DRAWABLES; i++) {
        if (t3_slots[i].dpy == pdraw->psc->dpy &&
            t3_slots[i].xid == pdraw->xDrawable) {
            d = &t3_slots[i];
            break;
        }
    }
    if (!d) {
        d = t3_get_free_slot();
        if (!d) {
            pthread_mutex_unlock(&t3_lock);
            t3_fail("drawable-table-full", 40);
        }
        memset(d, 0, sizeof(*d));
        d->dpy = pdraw->psc->dpy;
        d->xid = pdraw->xDrawable;
        t3_open_transport(d, w, h);
        d->open_logged = 1;
        printf("T3_DRAWABLE xid=0x%lx size=%ux%u\n", (unsigned long)d->xid, w, h);
    } else {
        if (w != d->w || h != d->h) {
            printf("T3_RESIZE xid=0x%lx old=%ux%u new=%ux%u\n",
                   (unsigned long)d->xid, d->w, d->h, w, h);
            d->retired_burst = 0;
            t3_open_transport(d, w, h);
            d->open_logged = 1;
        }
    }

    d->gpu_parent=gpu_parent;
    if (!d->xp) t3_open_transport(d,w,h);
    /* bounded event progress (release/returned/retired/protocol) */
    rc = m7g_t3_xport_service(d->xp, 0);
    if (rc != 0) {
        pthread_mutex_unlock(&t3_lock);
        t3_fail("service-pre", rc);
    }

    if (m7g_t3_xport_retired(d->xp)) {
        /* one bounded recreate attempt per retirement burst */
        if (d->retired_burst == 0) {
            d->retired_burst = 1;
            printf("T3_RETIRED_RECOVER xid=0x%lx gen=%u epoch=%u\n",
                   (unsigned long)d->xid, d->gen, d->epoch);
            t3_open_transport(d, w, h);
        } else {
            pthread_mutex_unlock(&t3_lock);
            t3_fail("swapchain-retired", M7G_T3_XS_RETIRED);
        }
    }

    if (d->gpu) {
        rc=m9w_gpu_write(d->gpu,d->gen%3u,__glXGetCurrentContext());
        if(!rc)rc=m9w_xport_commit(d->xp,d->gen%3u,d->gen);
    } else {
        t3_readback_default_fb(d,w,h);
        m9r_capture_frame("final",d->xid,w,h,d->pixels);
        rc=m7g_t3_xport_commit(d->xp,d->gen%3u,d->gen,d->pixels);
    }
    if (rc != 0) {
        pthread_mutex_unlock(&t3_lock);
        t3_fail("transport-commit", rc);
    }

    /* progress again so releases/retired arrive during the stream, uniformly */
    rc = m7g_t3_xport_service(d->xp, 0);
    if (rc != 0) {
        pthread_mutex_unlock(&t3_lock);
        t3_fail("service-post", rc);
    }

    if (!d->gpu) {
        unsigned fnv = 2166136261u;
        size_t n = (size_t)w * h * 4u;
        for (size_t i = 0; i < n; i++) {
            fnv ^= d->pixels[i];
            fnv *= 16777619u;
        }
        printf("T3_SWAP xid=0x%lx gen=%u epoch=%u w=%u h=%u t_ns=%llu fnv=0x%08x commit=1\n",
               (unsigned long)d->xid, d->gen, d->epoch, w, h, t3_now_ns(), fnv);
    }
    d->gen++;
    d->swaps++;
    d->retired_burst = 0;
    t3_total_swaps++;
    pthread_mutex_unlock(&t3_lock);
    return 0;
}

/* ------------------------------------------------------------- teardown */
void
m7g_t3_adapter_drawable_destroyed(void *pdraw_ptr)
{
    __GLXDRIdrawable *pdraw = (__GLXDRIdrawable *)pdraw_ptr;
    struct t3_drawable *d = NULL;

    pthread_mutex_lock(&t3_lock);
    for (unsigned i = 0; i < M7G_T3_MAX_DRAWABLES; i++) {
        if (t3_slots[i].dpy == pdraw->psc->dpy &&
            t3_slots[i].xid == pdraw->xDrawable) {
            d = &t3_slots[i];
            break;
        }
    }
    if (d) {
        m9w_log("M10K_LIFE event=destroy-begin xid=0x%lx epoch=%u gen=%u extent=%ux%u",(unsigned long)d->xid,d->epoch,d->gen,d->w,d->h);
        m7g_t3_xport_counters c;
        int rc;
        if (d->xp) {
            rc = m7g_t3_xport_drain(d->xp);
            if (rc != 0) {
                pthread_mutex_unlock(&t3_lock);
                printf("T3_FAIL drain rc=%d\n", rc);
                _exit(rc);
            }
            m7g_t3_xport_get_counters(d->xp, &c);
            printf("T3_DRAWABLE_DONE xid=0x%lx swaps=%u gen=%u epoch=%u submitted=%u commits=%u releases=%u reuse_waits=%u refresh_cycle=%u past_present=%u retired=%u\n",
                   (unsigned long)d->xid, d->swaps, d->gen, d->epoch, c.submitted,
                   c.commits, c.releases, c.reuse_waits, c.refresh_cycle,
                   c.past_present_timing, c.retired);
            m9w_close_gpu(d);
            rc = m7g_t3_xport_destroy(d->xp);
            d->xp = NULL;
            if (rc) {
                pthread_mutex_unlock(&t3_lock);
                t3_fail("teardown", rc);
            }
        }
        free(d->pixels);
        memset(d, 0, sizeof(*d));
    }
    printf("T3_TOTAL swaps=%u\n", t3_total_swaps);
    pthread_mutex_unlock(&t3_lock);
}

/* GL API must still be current: called before drisw core unbindContext. */
void m9w_before_switch(void *owner,int keep) {
 if(!m9w_enabled())return;
 pthread_mutex_lock(&t3_lock);
 for(unsigned i=0;i<M7G_T3_MAX_DRAWABLES;i++) {
  struct t3_drawable *d=&t3_slots[i];
  if(m9w_gpu_owned(d->gpu,owner)) {
   /* A drawable switch retains the same GL context and its imported objects.
    * Waiting here can block startup before XWM consumes the first commit.
    * Reuse still waits for returned; actual context retirement drains below. */
   if(keep) { if(d->gen<16 || d->gen%120==0)m9w_log("M10K_LIFE event=drawable-switch-keep xid=0x%lx epoch=%u gen=%u owner=%p",(unsigned long)d->xid,d->epoch,d->gen,owner); continue; }
   m9w_close_gpu(d);
   int rc=m7g_t3_xport_destroy(d->xp);if(rc)t3_fail("gpu-unbind-transport",rc);
   d->xp=NULL;m9w_log("GPU_UNBIND_RETIRE xid=0x%lx",(unsigned long)d->xid);
  }
 }
 pthread_mutex_unlock(&t3_lock);
}
