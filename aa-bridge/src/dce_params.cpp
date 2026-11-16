// SPDX-License-Identifier: GPL-3.0-or-later
//
// VIDDEC3_create() with the display delay the unit's own player uses, interposed for the
// ducati H.264 decoder (device only).
//
// gst-ducati (libgstducati.so) creates the IVA-HD H.264 decoder with displayDelay =
// IVIDDEC3_DISPLAY_DELAY_AUTO (-1, set in its h264dec allocate_params): the codec then decides
// itself how many decoded pictures to keep before it hands one out. The unit's own CarPlay and
// CarLife player (svp/lib/libcig5vdec.so, g5_open) creates the same codec ("ivahd_h264dec",
// through libdce) with displayDelay = IVIDDEC3_DECODE_ORDER (0): each picture comes out of the
// process call that decoded it. Android Auto video has no B-frames, so decode order is display
// order.
//
// The bridge exports its own VIDDEC3_create (build.sh: --dynamic-list), which the plugin's PLT
// binds to ahead of libdce's. It sets displayDelay in the caller's parameters and calls
// libdce's. AA_VIDEO_DISPLAY_DELAY sets the value (default 0; "auto" passes the plugin's
// value through).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

namespace {

// The head of XDM's IVIDDEC3_Params (ti/xdais/dm/ividdec3.h), which every codec's extended
// parameters (IH264VDEC_Params, ...) start with.
struct Viddec3ParamsHead {
    std::int32_t size;
    std::int32_t maxHeight;
    std::int32_t maxWidth;
    std::int32_t maxFrameRate;
    std::int32_t maxBitRate;
    std::int32_t dataEndianness;
    std::int32_t forceChromaFormat;
    std::int32_t operatingMode;
    std::int32_t displayDelay;
};
static_assert(offsetof(Viddec3ParamsHead, displayDelay) == 32, "IVIDDEC3_Params.displayDelay");

typedef void *(*CreateFn)(void *engine, char *name, void *params);

CreateFn load() {
    // libdce came in with the plugin, which GStreamer opens RTLD_LOCAL: ask for it by name
    // rather than through RTLD_NEXT.
    void *lib = dlopen("libdce.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libdce.so.1", RTLD_LAZY);
    CreateFn fn = lib ? reinterpret_cast<CreateFn>(dlsym(lib, "VIDDEC3_create")) : nullptr;
    if (!fn) std::fprintf(stderr, "[aa-bridge] VIDDEC3_create: libdce.so.1: %s\n", dlerror());
    return fn;
}

} // namespace

extern "C" void *VIDDEC3_create(void *engine, char *name, void *params) {
    static const CreateFn create = load();
    if (!create) return nullptr;

    const char *v = std::getenv("AA_VIDEO_DISPLAY_DELAY");
    const bool keep = v && !std::strcmp(v, "auto");
    if (params && name && std::strstr(name, "h264") && !keep) {
        Viddec3ParamsHead *p = static_cast<Viddec3ParamsHead *>(params);
        const int want = (v && *v) ? std::atoi(v) : 0;
        std::fprintf(stderr, "[aa-bridge] VIDDEC3_create(%s): displayDelay %d -> %d\n", name,
                     (int) p->displayDelay, want);
        p->displayDelay = want;
    }
    void *h = create(engine, name, params);
    if (!h) std::fprintf(stderr, "[aa-bridge] VIDDEC3_create(%s) failed\n", name ? name : "?");
    return h;
}
