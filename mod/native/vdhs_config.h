#ifndef VDHS_CONFIG_H
#define VDHS_CONFIG_H

#include <stdio.h>

// One digit after optional blank/comment lines. Streaming keeps mode decisions
// identical before specialization and inside the payload, even with long comments.
static inline int vdhs_config_read(FILE* stream, int fallback) {
    int c;
    for (;;) {
        c = fgetc(stream);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c != '#') break;
        while (c != EOF && c != '\n') c = fgetc(stream);
    }
    if (c < '0' || c > '2') return fallback;
    int value = c - '0';
    do { c = fgetc(stream); } while (c == ' ' || c == '\t' || c == '\r');
    return (c == EOF || c == '\n' || c == '#') ? value : fallback;
}

#endif
