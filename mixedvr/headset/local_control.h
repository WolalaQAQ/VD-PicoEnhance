// Short-lived local owner of the existing apply/restore backend. No new backend.
#define LOCAL_LEASE 3000

struct local_request { char session[33]; uint64_t sequence; int command; };
static struct {
    char session[33];
    uint64_t sequence, until;
    bool ready, notice;
} local_control;

static bool local_parse(const char *text, size_t size, struct local_request *request) {
    if (size < 41 || size >= 128 || memcmp(text, "VDL1 ", 5)) return false;
    for (size_t i = 5; i < 37; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    if (text[37] != ' ') return false;
    size_t i = 38;
    uint64_t seq = 0;
    if (text[i] < '1' || text[i] > '9') return false;
    for (; i < size && text[i] >= '0' && text[i] <= '9'; ++i) {
        unsigned digit = (unsigned)(text[i] - '0');
        if (seq > (UINT64_MAX - digit) / 10) return false;
        seq = seq * 10 + digit;
    }
    if (i >= size || text[i++] != ' ') return false;
    static const char *const commands[] = {"status\n", "index\n", "hands\n", "restore\n"};
    for (int command = 0; command < 4; ++command) {
        size_t n = strlen(commands[command]);
        if (size - i != n || memcmp(text + i, commands[command], n)) continue;
        memcpy(request->session, text + 5, 32); request->session[32] = 0;
        request->sequence = seq; request->command = command;
        return true;
    }
    return false;
}

static void local_drop(bool *recovery_failed) {
    if (local_control.until) {
        *recovery_failed = !recover_if_owned();
        local_control.notice = true;
    }
    local_control.until = 0;
    local_control.ready = false;
}

static bool local_reply(const struct local_source *source, bool ok) {
    char setting[8];
    bool observed = setting_get(setting);
    char text[128];
    snprintf(text, sizeof text, "VDRL1 %s %" PRIu64 " %d %s %s\n",
             local_control.session, local_control.sequence, mode,
             observed ? setting : "-1", ok && observed ? "ok" : "error");
    bool published = local_write(source, text);
    return observed && published;
}

static void local_tick(int client, bool *recovery_failed, uint64_t *last_control) {
    if (local_control.until && now_ms() >= local_control.until) local_drop(recovery_failed);
    struct local_source source;
    char text[128]; size_t size = 0; bool changed = false;
    if (!local_read(&source, text, &size, &changed)) { local_drop(recovery_failed); return; }
    struct local_request request;
    if (!local_parse(text, size, &request)) {
        close(source.dir); local_drop(recovery_failed); return;
    }
    if (changed || strcmp(local_control.session, request.session)) {
        local_drop(recovery_failed);
        memcpy(local_control.session, request.session, sizeof request.session);
        // A residual mode record after process/daemon restart is not a command.
        local_control.sequence = request.sequence;
        local_control.ready = false;
    } else if (request.sequence <= local_control.sequence) {
        if (local_control.notice) {
            if (local_reply(&source, false)) local_control.notice = false;
        }
        close(source.dir); return;
    }
    local_control.sequence = request.sequence; // Busy/rejected records are consumed too.
    local_control.notice = false;
    bool ok = client < 0 && !*recovery_failed &&
              (local_control.ready || request.command == 0);
    if (ok) {
        local_control.until = now_ms() + LOCAL_LEASE;
        local_control.ready = true;
        if (request.command == 1 || request.command == 2) {
            ok = apply(request.command, LOCAL_LEASE);
            if (ok) *last_control = now_ms();
        } else if (request.command == 3) ok = restore();
    }
    if (!ok) local_drop(recovery_failed);
    if (local_reply(&source, ok)) local_control.notice = false;
    else if (local_control.until) local_drop(recovery_failed);
    close(source.dir);
}

static void local_shutdown_reply(void) {
    if (!local_control.session[0]) return;
    struct local_source source;
    char text[128]; size_t size; bool changed;
    if (local_read(&source, text, &size, &changed)) {
        local_reply(&source, false);
        close(source.dir);
    }
    if (local_process >= 0) { close(local_process); local_process = -1; }
}
