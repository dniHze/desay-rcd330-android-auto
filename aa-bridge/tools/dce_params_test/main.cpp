// Opens the stand-in plugin as GStreamer opens plugins (RTLD_LOCAL), and has it create an H.264
// and an MPEG-4 decoder.
#include <cstdio>
#include <dlfcn.h>

int main() {
    void *p = dlopen("./libplugin.so", RTLD_LAZY | RTLD_LOCAL);
    if (!p) {
        std::printf("dlopen: %s\n", dlerror());
        return 1;
    }
    int (*create)(const char *) = reinterpret_cast<int (*)(const char *)>(dlsym(p, "plugin_create"));
    create("ivahd_h264dec");
    create("ivahd_mpeg4dec");
    return 0;
}
