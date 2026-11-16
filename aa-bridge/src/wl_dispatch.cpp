// SPDX-License-Identifier: GPL-3.0-or-later
//
// wl_display_dispatch() with a time limit, interposed for the unit's waylandsink (device only).
//
// The SVP waylandsink (/svp/lib/gstreamer, Cidana/TI build) renders its first frame like this:
// wl_surface attach + damage + frame callback + commit, then wl_display_dispatch(), and only
// AFTER that call returns does it create its ILM surface (ilm_surfaceCreate, id = getpid() +
// 20000000, the platform's convention). Under ivi-shell a wl_surface is not shown until it is
// an ILM surface, so weston never sends that frame callback, wl_display_dispatch() never
// returns, the video pipeline stalls on its first frame and no surface ever appears (step 3 on
// the unit, both waylandsink and waylandsink2).
//
// The bridge exports its own wl_display_dispatch (build.sh: --dynamic-list), which the plugin's
// PLT binds to ahead of libwayland-client. It does what the real one does (prepare_read, flush,
// wait on the socket, read_events, dispatch_pending), except that it stops waiting after
// AA_WL_DISPATCH_MS (default 50) and returns 0 events. Once the surface is visible, frame
// callbacks arrive in time and pace the sink as before; while it is hidden, the limit does.

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <dlfcn.h>
#include <poll.h>

struct wl_display;

namespace {

struct Wayland {
    int (*prepare_read)(wl_display *);
    int (*dispatch_pending)(wl_display *);
    int (*flush)(wl_display *);
    int (*get_fd)(wl_display *);
    int (*read_events)(wl_display *);
    void (*cancel_read)(wl_display *);
    int (*dispatch)(wl_display *); // the real one, if anything is missing
    int timeout_ms;
    bool ok;
};

template <typename F> void sym(void *lib, const char *name, F &out) {
    out = reinterpret_cast<F>(dlsym(lib, name));
}

Wayland load() {
    Wayland w = {};
    // The plugin has loaded libwayland-client by the time it calls us; it may be private to the
    // plugin (RTLD_LOCAL), so ask for it by name rather than through RTLD_NEXT.
    void *lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!lib) {
        std::fprintf(stderr, "[aa-bridge] wl_display_dispatch: libwayland-client.so.0: %s\n", dlerror());
        return w;
    }
    sym(lib, "wl_display_prepare_read", w.prepare_read);
    sym(lib, "wl_display_dispatch_pending", w.dispatch_pending);
    sym(lib, "wl_display_flush", w.flush);
    sym(lib, "wl_display_get_fd", w.get_fd);
    sym(lib, "wl_display_read_events", w.read_events);
    sym(lib, "wl_display_cancel_read", w.cancel_read);
    sym(lib, "wl_display_dispatch", w.dispatch);
    const char *v = std::getenv("AA_WL_DISPATCH_MS");
    w.timeout_ms = (v && *v) ? std::atoi(v) : 50;
    w.ok = w.prepare_read && w.dispatch_pending && w.flush && w.get_fd && w.read_events && w.cancel_read;
    std::fprintf(stderr, "[aa-bridge] wl_display_dispatch interposed: %s, limit %d ms\n",
                 w.ok ? "active" : "missing symbols, passing through", w.timeout_ms);
    return w;
}

std::atomic<unsigned> g_timeouts(0);

} // namespace

// How many times the limit ran out; the pipeline reports it with its frame counters (a weak
// reference there, as the desktop build has no interposer).
extern "C" unsigned aa_wl_dispatch_timeouts() { return g_timeouts.load(); }

extern "C" int wl_display_dispatch(wl_display *display) {
    static const Wayland w = load();
    if (!w.ok) return w.dispatch ? w.dispatch(display) : -1;

    // Events already queued: dispatch them, as the real call does.
    if (w.prepare_read(display) != 0) return w.dispatch_pending(display);

    if (w.flush(display) < 0 && errno != EAGAIN) {
        w.cancel_read(display);
        return -1;
    }
    pollfd p = {w.get_fd(display), POLLIN, 0};
    int r;
    do {
        r = poll(&p, 1, w.timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
        w.cancel_read(display);
        if (r < 0) return -1;
        g_timeouts.fetch_add(1);
        static bool told = false;
        if (!told) {
            told = true;
            std::fprintf(stderr, "[aa-bridge] wl_display_dispatch: no event within %d ms, carrying on "
                                 "(the surface is not on screen yet)\n", w.timeout_ms);
        }
        return w.dispatch_pending(display);
    }
    if (w.read_events(display) < 0) return -1;
    return w.dispatch_pending(display);
}
