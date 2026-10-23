#include <stddef.h>

/* native_entry.c already supplies memset/_fltused. No CRT is imported. */
void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}
