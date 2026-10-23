#include "connection.h"

typedef struct {
    uint32_t size;
    wchar_t *reserved, *desktop, *title;
    uint32_t x, y, width, height, columns, rows, fill, flags;
    uint16_t show, reserved_size;
    void *reserved_bytes, *input, *output, *error;
} StartupInfo;
typedef struct { void *process, *thread; uint32_t pid, tid; } ProcessInfo;
_Static_assert(sizeof(StartupInfo) == 104, "Win64 STARTUPINFOW ABI");
_Static_assert(sizeof(ProcessInfo) == 24, "Win64 PROCESS_INFORMATION ABI");

static void clear(void *memory, size_t bytes) {
    unsigned char *p = memory;
    while (bytes--) *p++ = 0;
}

void mv_connection_stop(MvConnection *c) {
    if (c->process) {
        c->terminate_process(c->process, 1);
        c->native->close(c->process);
        c->process = 0;
    }
}

static int append(wchar_t **out, wchar_t *end, const wchar_t *text) {
    while (*text) {
        if (*out == end) return 0;
        *(*out)++ = *text++;
    }
    **out = 0;
    return 1;
}

static int launch(MvConnection *c, uint64_t now) {
    wchar_t *out = c->command, *end = c->command + 1535;
    StartupInfo startup;
    ProcessInfo process;
    clear(&startup, sizeof(startup)); clear(&process, sizeof(process));
    startup.size = sizeof(startup);
    if (!append(&out, end, L"\"") || !append(&out, end, c->adb) ||
        !append(&out, end, c->stage == 1 ? L"\" connect \"" : L"\" -s \"") ||
        !append(&out, end, c->serial) || !append(&out, end, L"\"")) return -1;
    if (c->stage == 2 && !append(&out, end, L" forward --no-rebind tcp:29743 tcp:29743")) return -1;
    /* Direct executable launch; never cmd.exe/PowerShell, no inherited handles. */
    if (!c->create_process(c->adb, c->command, 0, 0, 0, 0x08000000u,
                           0, 0, &startup, &process)) return -1;
    c->native->close(process.thread);
    c->process = process.process;
    c->deadline = now + 5000;
    return 0;
}

int mv_connection_start(MvConnection *c, const MvNativeApi *native,
                        MvHeadsetGetProc get_proc, const wchar_t *path, uint64_t now) {
    clear(c, sizeof(*c)); c->native = native;
    void *file = native->open_file(path, 0x80000000u, 7, 0, 3, 0x80, 0);
    if (file == (void *)(intptr_t)-1) {
        uint32_t error = native->last_error();
        return error == 2 || error == 3 ? 1 : -1;
    }
    uint32_t bytes = 0;
    int ok = native->read_file(file, c->config, sizeof(c->config)-2, &bytes, 0);
    native->close(file);
    if (!ok || bytes < 8 || bytes % 2 || bytes == sizeof(c->config)-2 || c->config[0] != 0xfeff) return -1;
    wchar_t *p = c->config + 1;
    c->adb = p;
    while (*p && *p != L'\r' && *p != L'\n') {
        if (*p == L'"') return -1;
        ++p;
    }
    if (!*p || p == c->adb) return -1;
    wchar_t separator = *p; *p++ = 0;
    if (separator == L'\r' && *p == L'\n') ++p;
    c->serial = p;
    int wireless = 0;
    while (*p && *p != L'\r' && *p != L'\n') {
        wchar_t value = *p;
        if (!((value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z') ||
              (value >= L'0' && value <= L'9') || value == L'.' || value == L':' || value == L'_' || value == L'-')) return -1;
        if (value == L':') wireless = 1;
        ++p;
    }
    if (p == c->serial) return -1;
    *p = 0;
    void *kernel = native->module(L"kernel32.dll");
    c->create_process = (int32_t (*)(const wchar_t *, wchar_t *, void *, void *, int32_t,
                                    uint32_t, void *, const wchar_t *, void *, void *))get_proc(kernel, "CreateProcessW");
    c->terminate_process = (int32_t (*)(void *, uint32_t))get_proc(kernel, "TerminateProcess");
    if (!c->create_process || !c->terminate_process) return -1;
    c->stage = wireless ? 1 : 2;
    return launch(c, now);
}

int mv_connection_tick(MvConnection *c, uint64_t now) {
    if (!c->process) return 1;
    uint32_t status = c->native->wait(c->process, 0);
    if (status == 258 && now < c->deadline) return 0;
    if (status != 0) { mv_connection_stop(c); return -1; }
    c->native->close(c->process); c->process = 0;
    if (c->stage++ == 1) return launch(c, now);
    /* --no-rebind may find an existing forward. The authenticated status reply,
     * not adb's exit code, determines whether it reaches our paired headset. */
    return 1;
}
