// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for the unit's waylandsink first frame (loaded RTLD_LOCAL, like a GStreamer
// plugin): a surface with no shell role, a frame callback, commit, then ONE
// wl_display_dispatch(). Under a shell that never shows role-less surfaces the frame callback
// never comes, so the real wl_display_dispatch() blocks. Returns the ms that call took.
#define _GNU_SOURCE
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    (void)d; (void)v;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
    if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg = {global, global_remove};

static void done(void *d, struct wl_callback *cb, uint32_t t) { (void)t; *(int *)d = 1; wl_callback_destroy(cb); }
static const struct wl_callback_listener frame_listener = {done};

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long sink_first_frame(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return -1;
    struct wl_registry *r = wl_display_get_registry(dpy);
    wl_registry_add_listener(r, &reg, NULL);
    wl_display_roundtrip(dpy);
    if (!compositor || !shm) return -2;
    int fd = memfd_create("buf", 0);
    ftruncate(fd, 64 * 64 * 4);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 64 * 64 * 4);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 64, 64, 64 * 4, WL_SHM_FORMAT_XRGB8888);
    struct wl_surface *s = wl_compositor_create_surface(compositor);
    int framed = 0;
    // The unit's sink attaches a wl_drm buffer, which weston keeps; an shm buffer here would be
    // released at once and that wl_buffer.release event alone would end the dispatch. So: no
    // buffer, just the frame callback, and a roundtrip first so nothing else is pending.
    (void)buf;
    wl_display_roundtrip(dpy);
    wl_callback_add_listener(wl_surface_frame(s), &frame_listener, &framed);
    wl_surface_commit(s);
    long t0 = now_ms();
    wl_display_dispatch(dpy); // the sink's call
    return now_ms() - t0;
}
