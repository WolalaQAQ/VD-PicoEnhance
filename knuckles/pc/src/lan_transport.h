#pragma once
#include <winsock2.h>
#include "headset_client.h"
#include "../../headset/src/lan_config.h"

struct MvLanTransport {
    SOCKET socket = INVALID_SOCKET;
    SOCKET discovery = INVALID_SOCKET;
    sockaddr_in peer{};
    uint64_t discovery_until = 0;
    char query[LAN_DISCOVERY_SIZE]{};
    bool started = false;
    void *event_user = nullptr;
    void (*event_sink)(void *, const MvHeadsetEvent *) = nullptr;
};
bool mv_lan_prepare(MvLanTransport *, const char *endpoint,
                    void (*event)(void *, const MvHeadsetEvent *), void *user);
MvHeadsetBindings mv_lan_bindings(MvLanTransport *);
bool mv_lan_discover_begin(MvLanTransport *, const char *token, uint64_t now);
int mv_lan_discover_poll(MvLanTransport *, uint64_t now);
void mv_lan_discover_cancel(MvLanTransport *);
void mv_lan_cleanup(MvLanTransport *);
