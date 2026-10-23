#include "lan_transport.h"
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <cstdio>
#include <cstring>
#include <vector>

static void close_peer(void *user) {
    auto *t = static_cast<MvLanTransport *>(user);
    if (t->socket != INVALID_SOCKET) closesocket(t->socket);
    t->socket = INVALID_SOCKET;
}
static int open_peer(void *user, uint16_t port) {
    auto *t = static_cast<MvLanTransport *>(user);
    t->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->socket == INVALID_SOCKET) return -1;
    u_long nonblocking = 1;
    if (ioctlsocket(t->socket, FIONBIO, &nonblocking)) return -1;
    t->peer.sin_port = htons(port);
    if (!connect(t->socket, reinterpret_cast<sockaddr *>(&t->peer), sizeof(t->peer))) return 1;
    return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
}
static int poll_peer(void *user) {
    auto *t = static_cast<MvLanTransport *>(user);
    fd_set writable, errors;
    FD_ZERO(&writable); FD_ZERO(&errors);
    FD_SET(t->socket, &writable); FD_SET(t->socket, &errors);
    timeval timeout{};
    const int result = select(0, nullptr, &writable, &errors, &timeout);
    if (result < 0 || FD_ISSET(t->socket, &errors)) return -1;
    if (!result) return 0;
    int error = 0, size = sizeof(error);
    return getsockopt(t->socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) || error ? -1 : 1;
}
static int send_peer(void *user, const char *data, uint32_t bytes) {
    const int n = send(static_cast<MvLanTransport *>(user)->socket, data, static_cast<int>(bytes), 0);
    return n == SOCKET_ERROR ? (WSAGetLastError() == WSAEWOULDBLOCK ? MV_HEADSET_IO_WAIT : MV_HEADSET_IO_ERROR) : n;
}
static int receive_peer(void *user, char *data, uint32_t bytes) {
    const int n = recv(static_cast<MvLanTransport *>(user)->socket, data, static_cast<int>(bytes), 0);
    return n == SOCKET_ERROR ? (WSAGetLastError() == WSAEWOULDBLOCK ? MV_HEADSET_IO_WAIT : MV_HEADSET_IO_ERROR) : n;
}
static int random_bytes(void *, unsigned char *data, uint32_t bytes) {
    return BCryptGenRandom(nullptr, data, bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
static uint64_t clock_ms(void *) { return GetTickCount64(); }
static void report(void *user, const MvHeadsetEvent *event) {
    auto *t = static_cast<MvLanTransport *>(user);
    if (t->event_sink) t->event_sink(t->event_user, event);
}
bool mv_lan_prepare(MvLanTransport *t, const char *endpoint,
                    void (*event)(void *, const MvHeadsetEvent *), void *user) {
    char ip[16]{};
    unsigned port = 0;
    int consumed = 0;
    if (sscanf(endpoint, "%15[0-9.]:%u%n", ip, &port, &consumed) != 2 ||
        endpoint[consumed] || !port || port > 65535) return false;
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<u_short>(port));
    if (InetPtonA(AF_INET, ip, &peer.sin_addr) != 1) return false;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
    t->peer = peer;
    t->started = true;
    t->event_sink = event;
    t->event_user = user;
    return true;
}
MvHeadsetBindings mv_lan_bindings(MvLanTransport *t) {
    return {t, open_peer, poll_peer, send_peer, receive_peer, close_peer, random_bytes, clock_ms, report};
}
void mv_lan_discover_cancel(MvLanTransport *t) {
    if (t->discovery != INVALID_SOCKET) closesocket(t->discovery);
    t->discovery = INVALID_SOCKET;
    SecureZeroMemory(t->query, sizeof(t->query));
    t->discovery_until = 0;
}

bool mv_lan_discover_begin(MvLanTransport *t, const char *token, uint64_t now) {
    mv_lan_discover_cancel(t);
    t->discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->discovery == INVALID_SOCKET) return false;
    u_long nonblocking = 1;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (ioctlsocket(t->discovery, FIONBIO, &nonblocking) ||
        bind(t->discovery, reinterpret_cast<sockaddr *>(&local), sizeof(local))) {
        mv_lan_discover_cancel(t); return false;
    }
    unsigned char random[16];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
        mv_lan_discover_cancel(t); return false;
    }
    const char hex[] = "0123456789abcdef";
    char nonce[32];
    for (unsigned i = 0; i < sizeof(random); ++i) {
        nonce[2 * i] = hex[random[i] >> 4]; nonce[2 * i + 1] = hex[random[i] & 15];
    }
    lan_discovery_message(t->query, false, token, nonce);
    t->discovery_until = now + 1000;
    // The saved address is a fast path and also preserves old-daemon support.
    sendto(t->discovery, t->query, sizeof(t->query), 0,
           reinterpret_cast<const sockaddr *>(&t->peer), sizeof(t->peer));
    // Owned loopback fixtures never send a discovery packet to the LAN.
    if ((ntohl(t->peer.sin_addr.s_addr) >> 24) == 127) return true;
    ULONG size = 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return true;
    std::vector<unsigned char> buffer(size);
    auto *cards = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, cards, &size) != NO_ERROR) return true;
    sockaddr_in group = t->peer;
    InetPtonA(AF_INET, LAN_DISCOVERY_GROUP, &group.sin_addr);
    const DWORD ttl = 1;
    setsockopt(t->discovery, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char *>(&ttl), sizeof(ttl));
    for (auto *card = cards; card; card = card->Next) {
        if (card->OperStatus != IfOperStatusUp || (card->Flags & IP_ADAPTER_NO_MULTICAST) ||
            (card->IfType != IF_TYPE_ETHERNET_CSMACD && card->IfType != IF_TYPE_IEEE80211)) continue;
        for (auto *address = card->FirstUnicastAddress; address; address = address->Next) {
            if (address->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto ip = reinterpret_cast<const sockaddr_in *>(address->Address.lpSockaddr)->sin_addr;
            if (!lan_private(ntohl(ip.s_addr))) continue;
            if (!setsockopt(t->discovery, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char *>(&ip), sizeof(ip)))
                sendto(t->discovery, t->query, sizeof(t->query), 0,
                       reinterpret_cast<const sockaddr *>(&group), sizeof(group));
        }
    }
    return true;
}

int mv_lan_discover_poll(MvLanTransport *t, uint64_t now) {
    if (t->discovery == INVALID_SOCKET) return -1;
    if (now >= t->discovery_until) { mv_lan_discover_cancel(t); return -1; }
    for (unsigned pass = 0; pass < 8; ++pass) {
        char message[LAN_DISCOVERY_SIZE + 1];
        sockaddr_in sender{};
        int size = sizeof(sender);
        const int n = recvfrom(t->discovery, message, sizeof(message), 0,
                               reinterpret_cast<sockaddr *>(&sender), &size);
        if (n == SOCKET_ERROR) break;
        const auto ip = ntohl(sender.sin_addr.s_addr);
        const bool loopback = (ntohl(t->peer.sin_addr.s_addr) >> 24) == 127 && (ip >> 24) == 127;
        if (size != sizeof(sender) || sender.sin_family != AF_INET || sender.sin_port != t->peer.sin_port ||
            (!lan_private(ip) && !loopback) ||
            !lan_discovery_matches(message, static_cast<unsigned>(n), true, t->query + 5, t->query + 70)) continue;
        t->peer = sender;
        mv_lan_discover_cancel(t);
        return 1;
    }
    return 0;
}

void mv_lan_cleanup(MvLanTransport *t) {
    mv_lan_discover_cancel(t);
    close_peer(t);
    if (t->started) WSACleanup();
    t->started = false;
}
