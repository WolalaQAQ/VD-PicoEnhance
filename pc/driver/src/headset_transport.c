#include "headset_transport.h"

static void wipe(void *memory, uint32_t size) {
    volatile unsigned char *p = (volatile unsigned char *)memory;
    while (size--) *p++ = 0;
}

static void transport_close(void *user) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    if (t->has_socket) { t->api.close_socket(t->socket); t->has_socket = 0; }
}

static int transport_open(void *user, uint16_t port) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    uint32_t nonblocking = 1;
    /* sockaddr_in: AF_INET, network-order port, loopback, eight zero bytes. */
    unsigned char address[16] = {2, 0, 0, 0, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    t->socket = t->api.socket(2, 1, 6);
    if (t->socket == UINTPTR_MAX) return -1;
    t->has_socket = 1;
    if (t->api.nonblocking(t->socket, 0x8004667eu, &nonblocking) != 0) return -1;
    address[2] = (unsigned char)(port >> 8); address[3] = (unsigned char)port;
    if (t->api.connect(t->socket, address, sizeof(address)) == 0) return 1;
    return t->api.last_error() == 10035 ? 0 : -1;
}

static int transport_poll(void *user) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    struct { uint32_t count; uintptr_t sockets[64]; } writable, errors;
    struct { int32_t seconds, microseconds; } timeout = {0, 0};
    int32_t status, error = 0, length = sizeof(error);
    /* Only count entries are read by select; do not create large stack copies. */
    writable.count = errors.count = 1;
    writable.sockets[0] = errors.sockets[0] = t->socket;
    status = t->api.select(0, 0, &writable, &errors, &timeout);
    if (status < 0 || errors.count) return -1;
    if (!status || !writable.count) return 0;
    if (t->api.get_option(t->socket, 0xffff, 0x1007, (char *)&error, &length) != 0 || error) return -1;
    return 1;
}

static int transport_send(void *user, const char *bytes, uint32_t size) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    int32_t result = t->api.send(t->socket, bytes, (int32_t)size, 0);
    if (result < 0) return t->api.last_error() == 10035 ? MV_HEADSET_IO_WAIT : MV_HEADSET_IO_ERROR;
    return result;
}

static int transport_recv(void *user, char *bytes, uint32_t size) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    int32_t result = t->api.recv(t->socket, bytes, (int32_t)size, 0);
    if (result < 0) return t->api.last_error() == 10035 ? MV_HEADSET_IO_WAIT : MV_HEADSET_IO_ERROR;
    return result;
}

static int transport_random(void *user, unsigned char *bytes, uint32_t size) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    return t->api.random(0, bytes, size, 2) == 0;
}

static uint64_t transport_clock(void *user) {
    return ((MvHeadsetTransport *)user)->native->clock_ms();
}

static void transport_event(void *user, const MvHeadsetEvent *event) {
    MvHeadsetTransport *t = (MvHeadsetTransport *)user;
    if (t->sink) t->sink(t->sink_user, event);
}

static int read_token(const MvNativeApi *n, const wchar_t *path, char bytes[67]) {
    uint32_t total = 0;
    int ok = 1;
    void *file = n->open_file(path, 0x80000000u, 7, 0, 3, 0x80, 0);
    if (file == (void *)UINTPTR_MAX || !file) return 0;
    while (total < 67) {
        uint32_t count = 0;
        if (!n->read_file(file, bytes + total, 67 - total, &count, 0) || count > 67 - total) {
            ok = 0; break;
        }
        if (!count) break;
        total += count;
    }
    n->close(file);
    return ok && (total == 64 || (total == 65 && bytes[64] == '\n') ||
                  (total == 66 && bytes[64] == '\r' && bytes[65] == '\n'));
}

void mv_headset_transport_cleanup(MvHeadsetTransport *t) {
    transport_close(t);
    if (t->started) { t->api.cleanup(); t->started = 0; }
    if (t->bcrypt_module) { t->api.free_library(t->bcrypt_module); t->bcrypt_module = 0; }
    if (t->winsock_module) { t->api.free_library(t->winsock_module); t->winsock_module = 0; }
    wipe(&t->api, sizeof(t->api));
}

int mv_headset_transport_init(MvHeadsetTransport *t, MvHeadsetClient *c,
                             const MvNativeApi *native, MvHeadsetGetProc get_proc,
                             const wchar_t *path, MvHeadsetEventSink sink, void *sink_user) {
    MvHeadsetBindings bindings;
    char token[67];
    union { uint64_t alignment; unsigned char bytes[512]; } wsa_data;
    void *kernel;
    int ok = 0;
    wipe(t, sizeof(*t)); wipe(c, sizeof(*c));
    c->state = MV_HEADSET_FAILED;
    if (!native || !native->module || !native->open_file || !native->read_file ||
        !native->close || !native->clock_ms || !get_proc || !path) return 0;
    t->native = native; t->sink = sink; t->sink_user = sink_user;
    if (!read_token(native, path, token)) goto done;
    kernel = native->module(L"kernel32.dll");
    if (!kernel) goto done;
    t->api.load_library_ex = (void *(*)(const wchar_t *, void *, uint32_t))get_proc(kernel, "LoadLibraryExW");
    t->api.free_library = (int32_t (*)(void *))get_proc(kernel, "FreeLibrary");
    if (!t->api.load_library_ex || !t->api.free_library) goto done;
    t->winsock_module = t->api.load_library_ex(L"ws2_32.dll", 0, 0x800);
    if (!t->winsock_module) goto done;
    t->bcrypt_module = t->api.load_library_ex(L"bcrypt.dll", 0, 0x800);
    if (!t->bcrypt_module) goto done;
#define RESOLVE(field, type, module, name) \
    t->api.field = (type)get_proc(module, name); if (!t->api.field) goto done
    RESOLVE(startup, int32_t (*)(uint16_t, void *), t->winsock_module, "WSAStartup");
    RESOLVE(cleanup, int32_t (*)(void), t->winsock_module, "WSACleanup");
    RESOLVE(socket, uintptr_t (*)(int32_t, int32_t, int32_t), t->winsock_module, "socket");
    RESOLVE(nonblocking, int32_t (*)(uintptr_t, uint32_t, uint32_t *), t->winsock_module, "ioctlsocket");
    RESOLVE(connect, int32_t (*)(uintptr_t, const void *, int32_t), t->winsock_module, "connect");
    RESOLVE(select, int32_t (*)(int32_t, void *, void *, void *, const void *), t->winsock_module, "select");
    RESOLVE(get_option, int32_t (*)(uintptr_t, int32_t, int32_t, char *, int32_t *), t->winsock_module, "getsockopt");
    RESOLVE(send, int32_t (*)(uintptr_t, const char *, int32_t, int32_t), t->winsock_module, "send");
    RESOLVE(recv, int32_t (*)(uintptr_t, char *, int32_t, int32_t), t->winsock_module, "recv");
    RESOLVE(close_socket, int32_t (*)(uintptr_t), t->winsock_module, "closesocket");
    RESOLVE(last_error, int32_t (*)(void), t->winsock_module, "WSAGetLastError");
    RESOLVE(random, int32_t (*)(void *, unsigned char *, uint32_t, uint32_t), t->bcrypt_module, "BCryptGenRandom");
#undef RESOLVE
    wipe(&wsa_data, sizeof(wsa_data));
    if (t->api.startup(0x0202, wsa_data.bytes) != 0) goto done;
    t->started = 1;
    wipe(&bindings, sizeof(bindings));
    bindings.user = t;
    bindings.open = transport_open; bindings.poll = transport_poll;
    bindings.send = transport_send; bindings.recv = transport_recv; bindings.close = transport_close;
    bindings.random = transport_random; bindings.clock_ms = transport_clock; bindings.event = transport_event;
    ok = mv_headset_init(c, &bindings, 29743, token, 0);
done:
    wipe(token, sizeof(token)); wipe(&wsa_data, sizeof(wsa_data));
    if (!ok) mv_headset_transport_cleanup(t);
    return ok;
}
