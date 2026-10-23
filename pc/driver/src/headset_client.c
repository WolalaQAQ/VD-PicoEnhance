#include "headset_client.h"

static void wipe(void *memory, uint32_t size) {
    volatile unsigned char *p = (volatile unsigned char *)memory;
    while (size--) *p++ = 0;
}

static int lower_hex(const char *text, uint32_t size) {
    uint32_t i;
    if (!text) return 0;
    for (i = 0; i < size; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
    return 1;
}

static void report(MvHeadsetClient *c, uint32_t kind, uint32_t command, uint32_t reason,
                   uint64_t sequence, int32_t mode, int32_t setting, uint32_t vd, uint32_t ok) {
    MvHeadsetEvent e;
    wipe(&e, sizeof(e));
    e.sequence = sequence;
    e.kind = kind; e.command = command; e.reason = reason;
    e.mode = mode; e.setting = setting; e.vd_phase = vd; e.helper_ok = ok;
    if (c->io.event) c->io.event(c->io.user, &e);
}

static void release(MvHeadsetClient *c, uint32_t state) {
    if (c->opened) { c->io.close(c->io.user); c->opened = 0; }
    wipe(c->token, sizeof(c->token));
    wipe(c->session, sizeof(c->session));
    wipe(c->outgoing, sizeof(c->outgoing));
    wipe(c->incoming, sizeof(c->incoming));
    c->outgoing_size = c->outgoing_offset = c->incoming_size = 0;
    c->pending_command = c->desired = MV_HEADSET_NONE;
    c->command_due = c->status_due = 0;
    c->state = state;
}

static void fail(MvHeadsetClient *c, uint32_t reason) {
    if (c->stopping && !c->restore_confirmed)
        report(c, MV_HEADSET_RESTORE_UNCONFIRMED, MV_HEADSET_RESTORE, reason,
               c->sequence, -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
    report(c, MV_HEADSET_DISCONNECTED, c->desired, reason, c->sequence,
           -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
    release(c, MV_HEADSET_FAILED);
}

int mv_headset_init(MvHeadsetClient *c, const MvHeadsetBindings *io,
                    uint16_t port, const char *token, const char *session) {
    unsigned char random[16];
    static const char hex[] = "0123456789abcdef";
    uint32_t i;
    wipe(c, sizeof(*c));
    c->state = MV_HEADSET_FAILED;
    if (!io || !io->open || !io->poll || !io->send || !io->recv || !io->close ||
        !io->clock_ms || !port || !lower_hex(token, 64) ||
        (session && !lower_hex(session, 32)) || (!session && !io->random)) return 0;
    c->io = *io;
    if (!session) {
        if (io->random(io->user, random, sizeof(random)) != 1) {
            wipe(random, sizeof(random));
            return 0;
        }
        for (i = 0; i < 16; ++i) {
            c->session[2 * i] = hex[random[i] >> 4];
            c->session[2 * i + 1] = hex[random[i] & 15];
        }
        wipe(random, sizeof(random));
    } else for (i = 0; i < 32; ++i) c->session[i] = session[i];
    for (i = 0; i < 64; ++i) c->token[i] = token[i];
    c->port = port;
    c->last_tick = io->clock_ms(io->user);
    c->state = MV_HEADSET_IDLE;
    return 1;
}

int mv_headset_request(MvHeadsetClient *c, uint32_t command) {
    if (c->state < MV_HEADSET_IDLE || c->state > MV_HEADSET_ONLINE || c->stopping ||
        command < MV_HEADSET_INDEX || command > MV_HEADSET_STATUS) return 0;
    if (command == MV_HEADSET_STATUS) c->status_due = 1;
    else {
        if (c->desired == command) return 1;
        c->desired = command;
        c->command_due = 1;
        c->status_due = 0;
    }
    report(c, MV_HEADSET_REQUESTED, command, MV_HEADSET_OK, 0,
           -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
    return 1;
}

static const char *command_text(uint32_t command) {
    if (command == MV_HEADSET_INDEX) return "index";
    if (command == MV_HEADSET_HANDS) return "hands";
    if (command == MV_HEADSET_RESTORE) return "restore";
    return "status";
}

static void append(MvHeadsetClient *c, const char *text, uint32_t size) {
    uint32_t i;
    for (i = 0; i < size; ++i) c->outgoing[c->outgoing_size++] = text[i];
}

static void begin_send(MvHeadsetClient *c, uint32_t command, uint64_t now) {
    char digits[20];
    uint32_t count = 0;
    uint64_t number;
    const char *text = command_text(command);
    uint32_t length = 0;
    if (c->sequence == UINT64_MAX) { fail(c, MV_HEADSET_SEQUENCE_EXHAUSTED); return; }
    number = ++c->sequence;
    do { digits[count++] = (char)('0' + number % 10); number /= 10; } while (number);
    c->outgoing_size = c->outgoing_offset = 0;
    append(c, "VDM1 ", 5); append(c, c->token, 64); append(c, " ", 1);
    append(c, c->session, 32); append(c, " ", 1);
    while (count) append(c, &digits[--count], 1);
    append(c, " ", 1);
    while (text[length]) ++length;
    append(c, text, length); append(c, " 3000\n", 6);
    c->pending_command = command;
    c->pending_sequence = c->sequence;
    c->pending_at = now;
    if (command != MV_HEADSET_STATUS) c->next_send_at = now + MV_HEADSET_HEARTBEAT_MS;
}

typedef struct { const char *text; uint32_t size; } Word;

static int equal(Word w, const char *text) {
    uint32_t i;
    for (i = 0; i < w.size; ++i) if (!text[i] || w.text[i] != text[i]) return 0;
    return text[w.size] == 0;
}

static int decimal(Word w, uint64_t *number) {
    uint32_t i;
    uint64_t n = 0;
    if (!w.size) return 0;
    for (i = 0; i < w.size; ++i) {
        uint32_t digit = (uint32_t)(unsigned char)w.text[i] - '0';
        if (digit > 9 || n > (UINT64_MAX - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    *number = n;
    return 1;
}

static int reply(MvHeadsetClient *c) {
    Word words[6];
    uint32_t i = 0, count = 0, command = c->pending_command, vd, ok;
    uint64_t seq;
    int32_t mode, setting;
    while (i < c->incoming_size) {
        uint32_t start;
        while (i < c->incoming_size && c->incoming[i] == ' ') ++i;
        if (i == c->incoming_size) break;
        if (count == 6) goto invalid;
        start = i;
        while (i < c->incoming_size && c->incoming[i] != ' ') ++i;
        words[count].text = c->incoming + start;
        words[count++].size = i - start;
    }
    if (count != 6 || !equal(words[0], "VDR1") || !decimal(words[1], &seq) ||
        !command || c->outgoing_size || seq != c->pending_sequence) goto invalid;
    if (equal(words[2], "0")) mode = 0;
    else if (equal(words[2], "1")) mode = 1;
    else if (equal(words[2], "2")) mode = 2;
    else goto invalid;
    if (equal(words[3], "0")) setting = MV_HEADSET_SETTING_OFF;
    else if (equal(words[3], "1")) setting = MV_HEADSET_SETTING_ON;
    else if (equal(words[3], "null")) setting = MV_HEADSET_SETTING_NULL;
    else if (equal(words[3], "error")) setting = MV_HEADSET_SETTING_ERROR;
    else goto invalid;
    if (equal(words[4], "unobserved")) vd = MV_HEADSET_UNOBSERVED;
    else if (equal(words[4], "blocked")) vd = MV_HEADSET_BLOCKED;
    else if (equal(words[4], "warming")) vd = MV_HEADSET_WARMING;
    else if (equal(words[4], "ready")) vd = MV_HEADSET_READY;
    else goto invalid;
    if (equal(words[5], "ok")) ok = 1;
    else if (equal(words[5], "error")) ok = 0;
    else goto invalid;
    if (ok && command != MV_HEADSET_STATUS &&
        (mode != (command == MV_HEADSET_RESTORE ? 0 : (int32_t)command) ||
         (command == MV_HEADSET_INDEX && setting != MV_HEADSET_SETTING_OFF) ||
         (command == MV_HEADSET_HANDS && setting != MV_HEADSET_SETTING_ON) ||
         setting == MV_HEADSET_SETTING_ERROR)) {
        fail(c, MV_HEADSET_MODE_MISMATCH); return 0;
    }
    c->pending_command = MV_HEADSET_NONE;
    if (ok && command == MV_HEADSET_RESTORE) c->restore_confirmed = 1;
    report(c, MV_HEADSET_ACKNOWLEDGED, command, MV_HEADSET_OK, seq, mode, setting, vd, ok);
    if (!ok) { fail(c, MV_HEADSET_HELPER_ERROR); return 0; }
    return 1;
invalid:
    fail(c, MV_HEADSET_BAD_ACK);
    return 0;
}

static int pump(MvHeadsetClient *c) {
    uint32_t pass;
    if (c->outgoing_size) {
        uint32_t remaining = c->outgoing_size - c->outgoing_offset;
        int sent = c->io.send(c->io.user, c->outgoing + c->outgoing_offset, remaining);
        if (sent != MV_HEADSET_IO_WAIT) {
            if (sent <= 0 || (uint32_t)sent > remaining) {
                fail(c, sent == 0 ? MV_HEADSET_PEER_CLOSED : MV_HEADSET_SEND_ERROR); return 0;
            }
            c->outgoing_offset += (uint32_t)sent;
            if (c->outgoing_offset == c->outgoing_size) {
                wipe(c->outgoing, sizeof(c->outgoing));
                c->outgoing_size = c->outgoing_offset = 0;
            }
        }
    }
    for (pass = 0; pass < 8; ++pass) {
        char bytes[256];
        uint32_t i;
        int received = c->io.recv(c->io.user, bytes, sizeof(bytes));
        if (received == MV_HEADSET_IO_WAIT) break;
        if (received <= 0 || (uint32_t)received > sizeof(bytes)) {
            fail(c, received == 0 ? MV_HEADSET_PEER_CLOSED : MV_HEADSET_RECEIVE_ERROR); return 0;
        }
        for (i = 0; i < (uint32_t)received; ++i) {
            unsigned char ch = (unsigned char)bytes[i];
            if (ch == '\n') {
                if (!reply(c)) return 0;
                wipe(c->incoming, sizeof(c->incoming)); c->incoming_size = 0;
            } else if (ch < 32 || ch > 126 || c->incoming_size >= 127) {
                fail(c, MV_HEADSET_BAD_FRAMING); return 0;
            } else c->incoming[c->incoming_size++] = (char)ch;
        }
    }
    return 1;
}

void mv_headset_tick(MvHeadsetClient *c, uint64_t now) {
    uint32_t command = MV_HEADSET_NONE;
    int status;
    if (c->state < MV_HEADSET_IDLE || c->state > MV_HEADSET_ONLINE) return;
    if (c->opened && (now < c->last_tick || now - c->last_tick >= MV_HEADSET_LEASE_MS)) {
        fail(c, now < c->last_tick ? MV_HEADSET_CLOCK_REVERSED : MV_HEADSET_PROCESS_PAUSE); return;
    }
    c->last_tick = now;
    if (c->stopping && now - c->shutdown_at >= MV_HEADSET_ACK_TIMEOUT_MS) {
        fail(c, MV_HEADSET_SHUTDOWN_TIMEOUT); return;
    }
    if (c->pending_command && now - c->pending_at >= MV_HEADSET_ACK_TIMEOUT_MS) {
        fail(c, MV_HEADSET_ACK_TIMEOUT); return;
    }
    if (c->state == MV_HEADSET_IDLE) {
        if (!c->command_due && !c->status_due) return;
        c->opened = 1; c->connect_at = now;
        status = c->io.open(c->io.user, c->port);
        if (status < 0) { fail(c, MV_HEADSET_CONNECT_ERROR); return; }
        c->state = status ? MV_HEADSET_ONLINE : MV_HEADSET_CONNECTING;
        if (status) report(c, MV_HEADSET_CONNECTED, c->desired, MV_HEADSET_OK, 0,
                           -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
    } else if (c->state == MV_HEADSET_CONNECTING) {
        if (now - c->connect_at >= MV_HEADSET_CONNECT_TIMEOUT_MS) {
            fail(c, MV_HEADSET_CONNECT_TIMEOUT); return;
        }
        status = c->io.poll(c->io.user);
        if (status < 0) { fail(c, MV_HEADSET_CONNECT_ERROR); return; }
        if (!status) return;
        c->state = MV_HEADSET_ONLINE;
        report(c, MV_HEADSET_CONNECTED, c->desired, MV_HEADSET_OK, 0,
               -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
    }
    if (c->state != MV_HEADSET_ONLINE || !pump(c)) return;
    if (c->stopping && c->restore_confirmed && !c->pending_command) {
        release(c, MV_HEADSET_CLOSED);
        report(c, MV_HEADSET_CLOSE_EVENT, MV_HEADSET_RESTORE, MV_HEADSET_OK,
               c->sequence, 0, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 1);
        return;
    }
    if (c->pending_command) return;
    if (c->stopping) command = MV_HEADSET_RESTORE;
    else if (c->command_due) { command = c->desired; c->command_due = 0; }
    else if ((c->desired == MV_HEADSET_INDEX || c->desired == MV_HEADSET_HANDS) && now >= c->next_send_at)
        command = c->desired;
    else if (c->status_due) { command = MV_HEADSET_STATUS; c->status_due = 0; }
    if (command) {
        if (command == MV_HEADSET_INDEX || command == MV_HEADSET_HANDS) c->restore_confirmed = 0;
        begin_send(c, command, now);
        if (c->state == MV_HEADSET_ONLINE) (void)pump(c);
    }
}

void mv_headset_shutdown_begin(MvHeadsetClient *c, uint64_t now) {
    if (c->state < MV_HEADSET_IDLE || c->state > MV_HEADSET_ONLINE || c->stopping) return;
    c->stopping = 1; c->shutdown_at = now;
    c->command_due = c->status_due = 0;
    if (!c->opened || c->state == MV_HEADSET_CONNECTING) {
        release(c, MV_HEADSET_CLOSED);
        return;
    }
    /* Do not clear a partial outgoing frame or its pending ACK. */
    mv_headset_tick(c, now);
}

int mv_headset_shutdown_tick(MvHeadsetClient *c, uint64_t now) {
    if (!c->stopping) mv_headset_shutdown_begin(c, now);
    else mv_headset_tick(c, now);
    return c->state == MV_HEADSET_CLOSED || c->state == MV_HEADSET_FAILED ||
           c->state == MV_HEADSET_UNINITIALIZED;
}

void mv_headset_close(MvHeadsetClient *c) {
    if (c->state < MV_HEADSET_IDLE || c->state > MV_HEADSET_ONLINE) return;
    if (c->opened && !c->restore_confirmed && c->desired != MV_HEADSET_NONE)
        report(c, MV_HEADSET_RESTORE_UNCONFIRMED, MV_HEADSET_RESTORE, MV_HEADSET_FORCED_CLOSE,
               c->sequence, -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
    release(c, MV_HEADSET_CLOSED);
    report(c, MV_HEADSET_CLOSE_EVENT, MV_HEADSET_NONE, MV_HEADSET_FORCED_CLOSE,
           c->sequence, -1, MV_HEADSET_SETTING_UNKNOWN, MV_HEADSET_UNOBSERVED, 0);
}
