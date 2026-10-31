#define _POSIX_C_SOURCE 200809L
#include "webrtc_stun.h"

#include <string.h>
#include <limits.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_BINDING_REQUEST 0x0001u
#define STUN_BINDING_SUCCESS 0x0101u
#define STUN_ATTR_MAPPED_ADDRESS 0x0001u
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020u

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

int mlb_stun_build_binding_request(uint8_t *out, size_t capacity,
                                   const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                   size_t *out_length) {
    if (!out || !transaction_id || !out_length || capacity < MLB_STUN_HEADER_SIZE)
        return -1;
    memset(out, 0, MLB_STUN_HEADER_SIZE);
    write_be16(out, STUN_BINDING_REQUEST);
    write_be16(out + 2, 0);
    write_be32(out + 4, STUN_MAGIC_COOKIE);
    memcpy(out + 8, transaction_id, MLB_STUN_TRANSACTION_ID_SIZE);
    *out_length = MLB_STUN_HEADER_SIZE;
    return 0;
}

static int decode_address(const uint8_t *value, size_t value_length, int is_xor,
                          const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                          MlbStunMappedAddress *out) {
    uint8_t address[16];
    uint16_t port;
    int family;
    size_t address_length;
    if (!value || !out || value_length < 4) return -1; /* Reserved byte is ignored per RFC 8489. */
    family = value[1] == 0x01 ? AF_INET : value[1] == 0x02 ? AF_INET6 : 0;
    if (family == AF_INET) address_length = 4;
    else if (family == AF_INET6) address_length = 16;
    else return -1;
    if (value_length != 4 + address_length) return -1;
    port = read_be16(value + 2);
    if (is_xor) port ^= (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    memcpy(address, value + 4, address_length);
    if (is_xor) {
        uint8_t mask[16];
        write_be32(mask, STUN_MAGIC_COOKIE);
        memcpy(mask + 4, transaction_id, 12);
        for (size_t i = 0; i < address_length; i++) address[i] ^= mask[i];
    }
    memset(out, 0, sizeof *out);
    out->family = family;
    out->port = port;
    if (!inet_ntop(family, address, out->address, sizeof out->address))
        return -1;
    return 0;
}

int mlb_stun_parse_binding_response(const uint8_t *packet, size_t packet_length,
                                    const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                    MlbStunMappedAddress *mapped) {
    MlbStunMappedAddress xor_mapped, plain_mapped;
    int have_xor = 0, have_plain = 0;
    size_t offset, body_length;
    if (!packet || !transaction_id || !mapped || packet_length < MLB_STUN_HEADER_SIZE ||
        packet_length > MLB_STUN_MAX_MESSAGE_SIZE) return -1;
    /* RFC 8489: first two bits are zero; exactly one Binding Success response. */
    if ((packet[0] & 0xc0u) != 0 || read_be16(packet) != STUN_BINDING_SUCCESS)
        return -1;
    body_length = read_be16(packet + 2);
    if ((body_length & 3u) != 0 || body_length != packet_length - MLB_STUN_HEADER_SIZE ||
        read_be32(packet + 4) != STUN_MAGIC_COOKIE ||
        memcmp(packet + 8, transaction_id, MLB_STUN_TRANSACTION_ID_SIZE) != 0)
        return -1;

    offset = MLB_STUN_HEADER_SIZE;
    while (offset < packet_length) {
        uint16_t type, length;
        size_t padded;
        const uint8_t *value;
        if (packet_length - offset < 4) return -1;
        type = read_be16(packet + offset);
        length = read_be16(packet + offset + 2);
        padded = ((size_t)length + 3u) & ~(size_t)3u;
        if (padded > packet_length - offset - 4) return -1;
        value = packet + offset + 4;
        if (type == STUN_ATTR_XOR_MAPPED_ADDRESS && !have_xor) {
            if (decode_address(value, length, 1, transaction_id, &xor_mapped) != 0)
                return -1;
            have_xor = 1;
        } else if (type == STUN_ATTR_MAPPED_ADDRESS && !have_plain) {
            if (decode_address(value, length, 0, transaction_id, &plain_mapped) != 0)
                return -1;
            have_plain = 1;
        }
        offset += 4 + padded;
    }
    if (offset != packet_length) return -1;
    if (have_xor) *mapped = xor_mapped;
    else if (have_plain) *mapped = plain_mapped;
    else return -1;
    return 0;
}

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int random_transaction_id(uint8_t out[MLB_STUN_TRANSACTION_ID_SIZE]) {
    size_t have = 0;
    int fd = open("/dev/urandom", O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
    );
    if (fd < 0) return -1;
    while (have < MLB_STUN_TRANSACTION_ID_SIZE) {
        ssize_t n = read(fd, out + have, MLB_STUN_TRANSACTION_ID_SIZE - have);
        if (n > 0) have += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { close(fd); return -1; }
    }
    close(fd);
    return 0;
}

static int source_matches(const struct sockaddr *source, const struct sockaddr *target) {
    if (!source || !target || source->sa_family != target->sa_family) return 0;
    if (source->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)source;
        const struct sockaddr_in *b = (const struct sockaddr_in *)target;
        return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (source->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)source;
        const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)target;
        return a->sin6_port == b->sin6_port && a->sin6_scope_id == b->sin6_scope_id &&
               memcmp(&a->sin6_addr, &b->sin6_addr, sizeof a->sin6_addr) == 0;
    }
    return 0;
}

struct StunAttempt {
    int fd;
    const struct addrinfo *address;
};

static int query_addresses(const struct addrinfo *first, const uint8_t *request,
                           const uint8_t *transaction_id, unsigned timeout_ms,
                           MlbStunMappedAddress *mapped) {
    struct StunAttempt attempts[4];
    struct pollfd pollfds[4];
    size_t count = 0;
    unsigned active = 0, received = 0;
    int rc = -1;
    memset(attempts, 0, sizeof attempts);
    for (size_t i = 0; i < 4; i++) attempts[i].fd = -1;

    /* Race a small, bounded set of A/AAAA results so a dead IPv6 route does
       not consume the entire timeout before the usable IPv4 route is tried. */
    for (const struct addrinfo *ai = first; ai && count < 4; ai = ai->ai_next) {
        if (!ai->ai_addr || (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)) continue;
        int fd = socket(ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) continue;
        ssize_t sent = sendto(fd, (const char *)request, MLB_STUN_HEADER_SIZE, 0,
                              ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (sent != (ssize_t)MLB_STUN_HEADER_SIZE) { close(fd); continue; }
        attempts[count].fd = fd;
        attempts[count].address = ai;
        pollfds[count].fd = fd;
        pollfds[count].events = POLLIN;
        pollfds[count].revents = 0;
        count++;
        active++;
    }
    if (!active) goto done;

    int64_t start_ms = monotonic_ms();
    if (start_ms < 0) goto done;
    int64_t deadline_ms = start_ms + (int64_t)timeout_ms;
    while (active && received < 32) {
        int64_t now_ms = monotonic_ms();
        if (now_ms < 0 || now_ms >= deadline_ms) break;
        int64_t remain_ms = deadline_ms - now_ms;
        int wait_ms = remain_ms > INT_MAX ? INT_MAX : (int)remain_ms;
        int pr = poll(pollfds, (nfds_t)count, wait_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        for (size_t i = 0; i < count; i++) {
            if (attempts[i].fd < 0 || !pollfds[i].revents) continue;
            short events = pollfds[i].revents;
            pollfds[i].revents = 0;
            if (events & (POLLERR | POLLHUP | POLLNVAL)) {
                close(attempts[i].fd); attempts[i].fd = -1; pollfds[i].fd = -1; active--; continue;
            }
            if (!(events & POLLIN)) continue;
            uint8_t response[MLB_STUN_MAX_MESSAGE_SIZE + 1u];
            struct sockaddr_storage from;
            socklen_t from_len = sizeof from;
            ssize_t n = recvfrom(attempts[i].fd, response, sizeof response, 0,
                                 (struct sockaddr *)&from, &from_len);
            if (n < 0) {
                if (errno != EINTR) { close(attempts[i].fd); attempts[i].fd = -1; pollfds[i].fd = -1; active--; }
                continue;
            }
            received++;
            if ((size_t)n > MLB_STUN_MAX_MESSAGE_SIZE ||
                !source_matches((const struct sockaddr *)&from, attempts[i].address->ai_addr))
                continue;
            if (mlb_stun_parse_binding_response(response, (size_t)n, transaction_id, mapped) == 0) {
                rc = 0; goto done;
            }
        }
    }

done:
    for (size_t i = 0; i < 4; i++) if (attempts[i].fd >= 0) close(attempts[i].fd);
    return rc;
}

int mlb_stun_query(const char *host, uint16_t port, unsigned timeout_ms,
                   MlbStunMappedAddress *mapped) {
    struct addrinfo hints, *result = NULL;
    char service[6];
    uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE], request[MLB_STUN_HEADER_SIZE];
    size_t request_length = 0;
    int rc = -1;
    size_t host_length;
    if (!host || !mapped || !port || timeout_ms < 50 || timeout_ms > 10000) return -1;
    host_length = strnlen(host, 254);
    if (host_length == 0 || host_length >= 254) return -1;
    for (size_t i = 0; i < host_length; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c <= 0x20 || c == 0x7f || c == '/' || c == '\\' || c == '@') return -1;
    }
    if (random_transaction_id(transaction_id) != 0 ||
        mlb_stun_build_binding_request(request, sizeof request, transaction_id, &request_length) != 0)
        return -1;
    (void)request_length;
    (void)snprintf(service, sizeof service, "%u", (unsigned)port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, service, &hints, &result) != 0) return -1;
    /* Use a global timeout and a maximum of four DNS results, raced in parallel. */
    rc = query_addresses(result, request, transaction_id, timeout_ms, mapped);
    freeaddrinfo(result);
    return rc;
}
#else
int mlb_stun_query(const char *host, uint16_t port, unsigned timeout_ms,
                   MlbStunMappedAddress *mapped) {
    (void)host; (void)port; (void)timeout_ms; (void)mapped;
    return -1; /* Native Windows transport is a later porting step. */
}
#endif
