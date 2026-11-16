/* Stands in for libgstducati.so: links libdce.so.1 and creates its decoders the way gst-ducati
 * does, with displayDelay = IVIDDEC3_DISPLAY_DELAY_AUTO (-1). */
#include <stdint.h>
#include <string.h>

void *VIDDEC3_create(void *engine, char *name, void *params);

int plugin_create(const char *codec) {
    int32_t params[32];
    char name[32];
    memset(params, 0, sizeof(params));
    params[0] = sizeof(params);
    params[8] = -1; /* displayDelay */
    strcpy(name, codec);
    return VIDDEC3_create((void *) 1, name, params) != 0;
}
