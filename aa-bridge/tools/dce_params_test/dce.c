/* Stands in for the unit's libdce.so.1: prints the display delay it is created with. */
#include <stdint.h>
#include <stdio.h>

void *VIDDEC3_create(void *engine, char *name, void *params) {
    (void) engine;
    printf("libdce: VIDDEC3_create(%s) displayDelay %d\n", name, (int) ((int32_t *) params)[8]);
    return params;
}
