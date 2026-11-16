// SPDX-License-Identifier: GPL-3.0-or-later
#include "ilm_crop.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// The video surface's rectangles, set through the GENIVI ILM control library on the unit
// (/usr/lib/libilmControl.so.1.9.1, ilm_init in its libilmCommon), for the CarLife shim
// (livi-carlife-shim in aa-headless), a static musl binary that cannot load the unit's glibc
// libraries. SVP_iLMService::ShowSurface sets visibility, opacity and a destination rectangle
// (the surface's own size), never the source rectangle, and the decoder's buffers are padded
// (896x576 for an 800x480 picture, which starts at 32,24: gst-ducati's PADX/PADY), so without
// them the picture is off-centre with a green edge on the left and its bottom cut off.
//
// Two ways in:
//  * ilm_crop_serve(): a thread of the running bridge with the library loaded and initialised
//    once, taking "crop <surface id> <source x,y,w,h|-> <destination x,y,w,h|->" datagrams on
//    AA_BRIDGE_CTL (default /tmp/run/aa/bridge.sock). Each takes a few milliseconds, as the C++
//    shim's in-process calls did: the shim sends one before ShowSurface and one after it.
//  * `aa-bridge --ilm-crop <id> <source> <destination>`: one process per call, the shim's
//    fallback when the bridge's socket is not there. It loads the library, connects and exits,
//    which took long enough on the unit to show the uncropped picture (its green left column)
//    for a few frames at each start.

namespace {

bool parse(const char* s, int r[4]) {
    return std::sscanf(s, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4 && r[2] > 0 && r[3] > 0;
}

typedef int (*InitFn)();
typedef int (*RectFn)(unsigned int, int, int, int, int);
typedef int (*CommitFn)();
typedef int (*DestroyFn)();

struct Ilm {
    void* lib = nullptr;
    RectFn setSource = nullptr;
    RectFn setDest = nullptr;
    CommitFn commit = nullptr;
    DestroyFn destroy = nullptr;
    bool ready = false;

    // 0 ready; 2 the library is missing or lacks the functions; 3 ilm_init failed. `why` says so.
    int load(std::string& why) {
        if (ready) return 0;
        if (!lib) {
            const char* names[] = {"libilmControl.so.1.9.1", "libilmControl.so.1", "libilmControl.so"};
            for (const char* n : names) {
                if ((lib = dlopen(n, RTLD_NOW | RTLD_GLOBAL))) break;
            }
            if (!lib) {
                why = std::string("ILM control: libilmControl not available (") + dlerror() +
                      "); surface rectangles left as they are";
                return 2;
            }
        }
        InitFn init = reinterpret_cast<InitFn>(dlsym(lib, "ilm_init"));
        setSource = reinterpret_cast<RectFn>(dlsym(lib, "ilm_surfaceSetSourceRectangle"));
        setDest = reinterpret_cast<RectFn>(dlsym(lib, "ilm_surfaceSetDestinationRectangle"));
        commit = reinterpret_cast<CommitFn>(dlsym(lib, "ilm_commitChanges"));
        destroy = reinterpret_cast<DestroyFn>(dlsym(lib, "ilm_destroy"));
        if (!init || !setSource || !setDest || !commit) {
            why = "ILM control: libilmControl lacks the functions needed";
            return 2;
        }
        const int r = init();
        if (r != 0) {
            why = "ILM control: ilm_init -> " + std::to_string(r);
            return 3;
        }
        ready = true;
        return 0;
    }

    // The line to log.
    std::string set(unsigned id, const char* source, const char* dest) {
        int src[4], dst[4];
        const bool hasSrc = parse(source, src), hasDst = parse(dest, dst);
        char line[256];
        if (!hasSrc && !hasDst) {
            std::snprintf(line, sizeof line, "ILM control: surface %u: no rectangle to set", id);
            return line;
        }
        const int a = hasSrc ? setSource(id, src[0], src[1], src[2], src[3]) : 0;
        const int b = hasDst ? setDest(id, dst[0], dst[1], dst[2], dst[3]) : 0;
        const int c = commit();
        std::snprintf(line, sizeof line, "ILM control: surface %u source %s, destination %s -> %d/%d/%d", id, source,
                      dest, a, b, c);
        return line;
    }
};

}  // namespace

int ilm_crop_main(int argc, char** argv) {
    if (argc != 5) {
        std::printf("usage: aa-bridge --ilm-crop <surface id> <x,y,w,h|-> <x,y,w,h|->\n");
        return 1;
    }
    Ilm ilm;
    std::string why;
    const int r = ilm.load(why);
    if (r != 0) {
        std::printf("%s\n", why.c_str());
        return r;
    }
    std::printf("%s\n", ilm.set(static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)), argv[3], argv[4]).c_str());
    if (ilm.destroy) ilm.destroy();
    return 0;
}

void ilm_crop_serve() {
    const char* env = std::getenv("AA_BRIDGE_CTL");
    const std::string path = env && *env ? env : "/tmp/run/aa/bridge.sock";
    if (path == "-") return;
    std::thread([path] {
        const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        if (fd < 0 || path.size() >= sizeof addr.sun_path) {
            std::printf("ILM control: no crop socket %s\n", path.c_str());
            if (fd >= 0) ::close(fd);
            return;
        }
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        ::unlink(path.c_str());
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            std::printf("ILM control: crop socket %s: %s\n", path.c_str(), std::strerror(errno));
            ::close(fd);
            return;
        }
        Ilm ilm;
        std::string why;
        // Loaded and connected now, so the first show is cropped at once; tried again at a
        // request when it failed (the compositor not up yet), at most every 5 s.
        int state = ilm.load(why);
        std::printf("ILM control: crop socket %s, %s\n", path.c_str(), state == 0 ? "ready" : why.c_str());
        auto last = std::chrono::steady_clock::now();
        char buf[256];
        for (;;) {
            const ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            buf[n] = '\0';
            unsigned id = 0;
            char src[64], dst[64];
            if (std::sscanf(buf, "crop %u %63s %63s", &id, src, dst) != 3) continue;
            if (state != 0) {
                const auto now = std::chrono::steady_clock::now();
                if (state == 2 || now - last < std::chrono::seconds(5)) continue;
                last = now;
                state = ilm.load(why);
                if (state != 0) {
                    std::printf("%s\n", why.c_str());
                    continue;
                }
            }
            std::printf("%s\n", ilm.set(id, src, dst).c_str());
        }
        ::close(fd);
    }).detach();
}
