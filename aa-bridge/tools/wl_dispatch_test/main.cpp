// SPDX-License-Identifier: GPL-3.0-or-later
// Loads ./libsink.so RTLD_LOCAL (as GStreamer loads a plugin) and times its first frame.
// Built twice: with ../../src/wl_dispatch.cpp exported (the bridge) and without (control).
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

static void timeout(int) {
    std::printf("RESULT blocked: wl_display_dispatch did not return within 3 s\n");
    std::fflush(stdout);
    std::_Exit(2);
}

int main() {
    std::signal(SIGALRM, timeout);
    alarm(3);
    void *lib = dlopen("./libsink.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    auto first = reinterpret_cast<long (*)()>(dlsym(lib, "sink_first_frame"));
    long ms = first();
    std::printf("RESULT returned after %ld ms\n", ms);
    return ms < 0 ? 1 : 0;
}
