#ifndef VDHS_SHELL_ABI_H
#define VDHS_SHELL_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Default 64-bit Android libc++ ABI; checked against the Shell's existing list.
static inline size_t vdhs_shell_string_size(const char* value) {
    if (!((unsigned char)value[0] & 1)) return (size_t)((unsigned char)value[0] >> 1);
    size_t length;
    memcpy(&length, value + 8, sizeof length);
    return length;
}

static inline const char* vdhs_shell_string_data(const char* value) {
    if (!((unsigned char)value[0] & 1)) return value + 1;
    const char* data;
    memcpy(&data, value + 16, sizeof data);
    return data;
}

static inline int vdhs_shell_string_is(const char* value, const char* expected) {
    size_t length = strlen(expected);
    return vdhs_shell_string_size(value) == length &&
           memcmp(vdhs_shell_string_data(value), expected, length) == 0;
}

static inline int vdhs_shell_known_list_layout(const void* list, const char* expected) {
    uintptr_t bounds[2];
    memcpy(bounds, list, sizeof bounds);
    uintptr_t begin = bounds[0], end = bounds[1];
    if (!begin || end < begin || (end - begin) % 24 != 0) return 0;
    size_t count = (end - begin) / 24;
    if (!count || count > 16) return 0;
    for (size_t i = 0; i < count; ++i)
        if (vdhs_shell_string_is((const char*)(begin + i * 24), expected)) return 1;
    return 0;
}

#endif
