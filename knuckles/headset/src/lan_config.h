// Host-order IPv4 policy and paired discovery. One active PC, explicit LAN bind.
#ifndef PICO_MIXEDVR_LAN_CONFIG_H
#define PICO_MIXEDVR_LAN_CONFIG_H
#include <stdbool.h>
#include <stdint.h>

struct lan_config { uint32_t listen, peer; };

enum { LAN_DISCOVERY_SIZE = 102, LAN_DISCOVERY_PORT = 29744 };
#define LAN_DISCOVERY_GROUP "239.255.255.250"

/* Discovery uses the existing plaintext pairing key on the trusted LAN.
 * Echo a fresh nonce so a reply from an earlier search is not selected. */
static inline void lan_discovery_message(char out[LAN_DISCOVERY_SIZE], bool reply,
                                         const char *key, const char *nonce) {
    const char *prefix = reply ? "PKA1 " : "PKD1 ";
    for (unsigned i = 0; i < 5; ++i) out[i] = prefix[i];
    for (unsigned i = 0; i < 64; ++i) out[5 + i] = key[i];
    out[69] = ' ';
    for (unsigned i = 0; i < 32; ++i) out[70 + i] = nonce[i];
}

static inline bool lan_discovery_matches(const char *data, unsigned size, bool reply,
                                         const char *key, const char *nonce) {
    if (size != LAN_DISCOVERY_SIZE || data[69] != ' ') return false;
    const char *prefix = reply ? "PKA1 " : "PKD1 ";
    unsigned difference = 0;
    for (unsigned i = 0; i < 5; ++i) difference |= (unsigned char)data[i] ^ (unsigned char)prefix[i];
    for (unsigned i = 0; i < 64; ++i) difference |= (unsigned char)data[5 + i] ^ (unsigned char)key[i];
    for (unsigned i = 0; i < 32; ++i) {
        const char c = data[70 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        if (nonce) difference |= (unsigned char)c ^ (unsigned char)nonce[i];
    }
    return difference == 0;
}

static inline bool lan_private(uint32_t address) {
    return (address >> 24) == 10 || (address >> 20) == 0xac1 ||
           (address >> 16) == 0xc0a8;
}

static inline bool lan_ipv4(const char **cursor, uint32_t *address) {
    const char *p = *cursor;
    uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits) return false;
        result = (result << 8) | value;
        if (i < 3 && *p++ != '.') return false;
    }
    *cursor = p;
    *address = result;
    return true;
}

static inline bool lan_parse(const char *text, struct lan_config *out) {
    struct lan_config value;
    const char *p = text;
    if (!lan_ipv4(&p, &value.listen) || *p != ' ') return false;
    ++p;
    if (!lan_ipv4(&p, &value.peer) || *p || !lan_private(value.listen) ||
        !lan_private(value.peer) || value.listen == value.peer) return false;
    *out = value;
    return true;
}
#endif
