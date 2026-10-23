#include "connection.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

static int scenario, creates, terminates, process_closes, thread_closes, file_closes;
static wchar_t commands[2][1536];
static int same(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int wide_same(const wchar_t *a, const wchar_t *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static size_t length(const wchar_t *p) { size_t n = 0; while (p[n]) ++n; return n; }
static void *open_file(const wchar_t *p, uint32_t a, uint32_t b, void *c, uint32_t d, uint32_t e, void *f) {
    (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return scenario < 2 ? (void *)(intptr_t)-1 : (void *)0x33;
}
static uint32_t error(void) { return scenario == 1 ? 5 : 2; }
static int32_t read_file(void *file, void *out, uint32_t capacity, uint32_t *count, void *overlap) {
    (void)file; (void)overlap;
    const wchar_t *text = L"\xfeff" L"C:\\Program Files\\\u5de5\u5177\\adb.exe\nUSB123\n";
    if (scenario == 3) text = L"\xfeff" L"C:\\adb.exe\r\n192.0.2.10:5555\r\n";
    if (scenario == 8) text = L"C:\\adb.exe\nUSB123\n";
    if (scenario == 9) text = L"\xfeff" L"C:\\adb.exe\n\n";
    if (scenario == 10) text = L"\xfeff" L"C:\\bad\"path\\adb.exe\nUSB123\n";
    if (scenario == 11) text = L"\xfeff" L"C:\\adb.exe\nUSB123 -s other\n";
    *count = (uint32_t)(length(text)*2);
    if (*count > capacity) return 0;
    unsigned char *target = out;
    const unsigned char *source = (const unsigned char *)text;
    for (uint32_t i=0; i<*count; ++i) target[i] = source[i];
    return scenario != 13;
}
static int32_t close_handle(void *handle) {
    if (handle == (void *)0x33) ++file_closes;
    if (handle == (void *)0x71) ++process_closes;
    if (handle == (void *)0x72) ++thread_closes;
    return 1;
}
static uint32_t wait_process(void *handle, uint32_t timeout) {
    (void)handle; (void)timeout;
    return scenario == 6 ? 258 : scenario == 12 ? UINT32_MAX : 0;
}
static int32_t terminate(void *handle, uint32_t code) { (void)handle; (void)code; ++terminates; return 1; }
static int32_t create(const wchar_t *app, wchar_t *command, void *pa, void *ta, int32_t inherit,
                       uint32_t flags, void *env, const wchar_t *dir, void *startup, void *info) {
    (void)app; (void)pa; (void)ta; (void)env; (void)dir;
    if (inherit || flags != 0x08000000u || *(uint32_t *)startup != 104) return 0;
    for (size_t i=0; i<=length(command); ++i) commands[creates][i] = command[i];
    ++creates;
    if (scenario == 4) return 0;
    ((void **)info)[0] = (void *)0x71;
    ((void **)info)[1] = (void *)0x72;
    return 1;
}
static void *module(const wchar_t *name) { (void)name; return (void *)0x77; }
static void *resolve(void *handle, const char *name) {
    (void)handle;
    if (same(name, "CreateProcessW")) return scenario == 5 ? 0 : (void *)create;
    if (same(name, "TerminateProcess")) return (void *)terminate;
    return 0;
}

__declspec(dllexport) int connection_case(int value) {
    scenario = value; creates = terminates = process_closes = thread_closes = file_closes = 0;
    MvNativeApi native = {0};
    native.open_file = open_file; native.read_file = read_file; native.close = close_handle;
    native.last_error = error; native.module = module; native.wait = wait_process;
    MvConnection connection;
    int result = mv_connection_start(&connection, &native, resolve, L"connection.txt", 100);
    if (scenario == 0) { CHECK(result == 1 && !creates && !file_closes); return 0; }
    if (scenario == 1) { CHECK(result == -1 && !creates); return 0; }
    if (scenario == 4 || scenario == 5 || (scenario >= 8 && scenario <= 11) || scenario == 13) {
        CHECK(result == -1 && file_closes == 1 && !process_closes && !thread_closes);
        return 0;
    }
    CHECK(result == 0 && creates == 1 && thread_closes == 1);
    if (scenario == 7 || scenario == 14) {
        mv_connection_stop(&connection); mv_connection_stop(&connection);
        CHECK(terminates == 1 && process_closes == 1); return 0;
    }
    if (scenario == 6) {
        CHECK(mv_connection_tick(&connection, 200) == 0);
        CHECK(mv_connection_tick(&connection, 5100) == -1);
        CHECK(terminates == 1 && process_closes == 1); return 0;
    }
    if (scenario == 12) {
        CHECK(mv_connection_tick(&connection, 200) == -1);
        CHECK(terminates == 1 && process_closes == 1); return 0;
    }
    if (scenario == 3) {
        CHECK(wide_same(commands[0], L"\"C:\\adb.exe\" connect \"192.0.2.10:5555\""));
        CHECK(mv_connection_tick(&connection, 200) == 0 && creates == 2);
        CHECK(wide_same(commands[1], L"\"C:\\adb.exe\" -s \"192.0.2.10:5555\" forward --no-rebind tcp:29743 tcp:29743"));
    } else {
        CHECK(wide_same(commands[0], L"\"C:\\Program Files\\\u5de5\u5177\\adb.exe\" -s \"USB123\" forward --no-rebind tcp:29743 tcp:29743"));
    }
    CHECK(mv_connection_tick(&connection, 300) == 1);
    CHECK(process_closes == creates && thread_closes == creates && !terminates);
    mv_connection_stop(&connection);
    CHECK(!terminates);
    return 0;
}
