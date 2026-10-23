#include "phase_control.h"

static int word(const char *data, size_t count, const char *expected) {
    size_t n = 0;
    while (expected[n]) ++n;
    if (count != n) return 0;
    for (size_t i = 0; i < n; ++i) if (data[i] != expected[i]) return 0;
    return 1;
}

int mv_parse_phase(const char *data, size_t bytes, uint64_t *sequence, uint32_t *phase) {
    size_t at = 0;
    uint64_t seq = 0;
    if (!bytes || data[0] < '0' || data[0] > '9') return 0;
    while (at < bytes && data[at] >= '0' && data[at] <= '9') {
        const uint64_t digit = (uint64_t)(data[at] - '0');
        if (seq > (UINT64_MAX - digit) / 10) return 0;
        seq = seq * 10 + digit;
        ++at;
    }
    if (at == bytes || data[at++] != ' ') return 0;
    const size_t begin = at;
    while (at < bytes && data[at] >= 'a' && data[at] <= 'z') ++at;
    const size_t count = at - begin;
    uint32_t value;
    if (word(data + begin, count, "unknown")) value = 0;
    else if (word(data + begin, count, "index")) value = 1;
    else if (word(data + begin, count, "hands")) value = 2;
    else if (word(data + begin, count, "stop")) value = 3;
    else if (word(data + begin, count, "auto")) value = 4;
    else if (word(data + begin, count, "restore")) value = 5;
    else return 0;
    while (at < bytes && (data[at] == '\r' || data[at] == '\n')) ++at;
    if (at != bytes) return 0;
    *sequence = seq;
    *phase = value;
    return 1;
}
