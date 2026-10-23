#include "lan_transport.h"
#include <windows.h>
#include <cstdio>
#include <initializer_list>
#include <cstring>

namespace {
unsigned acknowledged;
bool bad_receipt;
void event(void *, const MvHeadsetEvent *e) {
    if (e->kind == MV_HEADSET_ACKNOWLEDGED) { ++acknowledged; bad_receipt |= !e->helper_ok; }
}
bool wait(MvHeadsetClient *client, unsigned target) {
    const auto until = GetTickCount64() + 2500;
    while (GetTickCount64() < until && client->state != MV_HEADSET_FAILED && acknowledged < target) {
        mv_headset_tick(client, GetTickCount64()); Sleep(5);
    }
    return acknowledged == target && !bad_receipt;
}
}
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 1;
    MvLanTransport transport;
    if (!mv_lan_prepare(&transport, argv[1], event, nullptr)) return 2;
    const auto original = transport.peer.sin_addr.s_addr;
    if (argc == 3) {
        if (!mv_lan_discover_begin(&transport,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", GetTickCount64())) return 4;
        if (!strcmp(argv[2], "expired-discovery")) {
            fd_set readable; FD_ZERO(&readable); FD_SET(transport.discovery, &readable);
            timeval timeout{1, 0};
            if (select(0, &readable, nullptr, nullptr, &timeout) != 1 ||
                mv_lan_discover_poll(&transport, GetTickCount64() + 1001) != -1 ||
                transport.peer.sin_addr.s_addr != original) return 6;
            mv_lan_cleanup(&transport);
            printf("{\"passed\":true,\"expired_queued_reply_rejected\":true}\n");
            return 0;
        }
        int found = 0;
        while (!found) { found = mv_lan_discover_poll(&transport, GetTickCount64()); Sleep(5); }
        if (found != 1 || original == transport.peer.sin_addr.s_addr) return 5;
    }
    const auto bindings = mv_lan_bindings(&transport);
    MvHeadsetClient client{};
    bool ok = mv_headset_init(&client, &bindings, ntohs(transport.peer.sin_port),
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", nullptr) != 0;
    unsigned target = 0;
    for (const auto command : {MV_HEADSET_STATUS, MV_HEADSET_INDEX, MV_HEADSET_HANDS, MV_HEADSET_RESTORE}) {
        ok = ok && mv_headset_request(&client, command) && wait(&client, ++target);
    }
    mv_headset_shutdown_begin(&client, GetTickCount64());
    const auto until = GetTickCount64() + 2500;
    while (!mv_headset_shutdown_tick(&client, GetTickCount64()) && GetTickCount64() < until) Sleep(5);
    ok = ok && client.state == MV_HEADSET_CLOSED;
    mv_lan_cleanup(&transport);
    if (!ok) return 3;
    printf("{\"passed\":true,\"acknowledged\":%u,\"transport\":\"real Windows Winsock loopback\"}\n", acknowledged);
}
