#define _POSIX_C_SOURCE 200809L
#include "webrtc_turn.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef HAVE_OPENSSL_VAULT
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

#define TURN_MAGIC_COOKIE 0x2112a442u
#define TURN_HEADER 20u
#define TURN_MAX_PACKET 65535u
#define TURN_QUEUE_LIMIT 64u
#define TURN_QUEUE_BYTES_LIMIT (256u * 1024u)
#define TURN_PERMISSION_LIFETIME_MS 300000LL
#define TURN_CHANNEL_LIFETIME_MS 600000LL
#define TURN_FINGERPRINT_XOR 0x5354554eu

/* STUN methods/classes (RFC 5766). */
#define TURN_ALLOCATE_REQUEST       0x0003u
#define TURN_ALLOCATE_SUCCESS       0x0103u
#define TURN_ALLOCATE_ERROR         0x0113u
#define TURN_REFRESH_REQUEST        0x0004u
#define TURN_REFRESH_SUCCESS        0x0104u
#define TURN_REFRESH_ERROR          0x0114u
#define TURN_CREATE_PERMISSION_REQ  0x0008u
#define TURN_CREATE_PERMISSION_OK   0x0108u
#define TURN_CREATE_PERMISSION_ERR  0x0118u
#define TURN_CHANNEL_BIND_REQUEST   0x0009u
#define TURN_CHANNEL_BIND_SUCCESS   0x0109u
#define TURN_CHANNEL_BIND_ERROR     0x0119u
#define TURN_SEND_INDICATION        0x0016u
#define TURN_DATA_INDICATION        0x0017u

#define TURN_ATTR_USERNAME          0x0006u
#define TURN_ATTR_MESSAGE_INTEGRITY 0x0008u
#define TURN_ATTR_ERROR_CODE        0x0009u
#define TURN_ATTR_CHANNEL_NUMBER    0x000cu
#define TURN_ATTR_LIFETIME          0x000du
#define TURN_ATTR_XOR_PEER_ADDRESS  0x0012u
#define TURN_ATTR_DATA              0x0013u
#define TURN_ATTR_REALM             0x0014u
#define TURN_ATTR_NONCE             0x0015u
#define TURN_ATTR_XOR_RELAYED_ADDR  0x0016u
#define TURN_ATTR_REQUESTED_TRANSPORT 0x0019u
#define TURN_ATTR_XOR_MAPPED_ADDR   0x0020u
#define TURN_ATTR_FINGERPRINT       0x8028u

typedef struct TurnPeer {
    struct sockaddr_storage address;
    socklen_t address_length;
    char text[46];
    uint16_t port;
} TurnPeer;

typedef struct TurnPermission {
    TurnPeer peer;
    int64_t expires_ms;
    int64_t refresh_ms;
    int active;
} TurnPermission;

typedef struct TurnChannel {
    TurnPeer peer;
    uint16_t number;
    int64_t expires_ms;
    int64_t refresh_ms;
    int active;
} TurnChannel;

typedef struct TurnQueuedPacket {
    char peer[46];
    uint16_t port;
    size_t length;
    uint8_t *data;
    struct TurnQueuedPacket *next;
} TurnQueuedPacket;

struct MlbTurnClient {
    int fd;
    struct sockaddr_storage server;
    socklen_t server_length;
    char username[257];
    char password[257];
    char realm[257];
    char nonce[513];
    uint8_t integrity_key[16];
    int have_credentials;
    unsigned timeout_ms;
    uint32_t requested_lifetime;
    MlbTurnAllocationInfo allocation;
    int64_t allocation_expires_ms;
    int64_t allocation_refresh_ms;
    TurnPermission permissions[32];
    TurnChannel channels[MLB_TURN_MAX_CHANNELS];
    uint16_t next_channel;
    TurnQueuedPacket *queue_head;
    TurnQueuedPacket *queue_tail;
    size_t queue_count;
    size_t queue_bytes;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int64_t turn_now_ms(void) {
#ifndef _WIN32
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return -1; /* TURN is currently disabled on Win32 builds. */
#endif
}

static int turn_printable_ascii(const char *text, size_t maximum) {
    size_t n;
    if (!text || !(n = strnlen(text, maximum + 1u)) || n > maximum) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20u || c > 0x7eu) return 0;
    }
    return 1;
}

#ifdef HAVE_OPENSSL_VAULT
static int turn_random(uint8_t *out, size_t length) {
    return out && length && length <= INT_MAX && RAND_bytes(out, (int)length) == 1 ? 0 : -1;
}
static int turn_derive_key(const char *username, const char *realm, const char *password,
                           uint8_t out[16]) {
    char material[800];
    unsigned int digest_length = 0;
    int n;
    if (!out || !turn_printable_ascii(username, 256) ||
        !turn_printable_ascii(realm, 256) || !turn_printable_ascii(password, 256)) return -1;
    n = snprintf(material, sizeof material, "%s:%s:%s", username, realm, password);
    if (n < 0 || (size_t)n >= sizeof material) return -1;
    if (EVP_Digest(material, (size_t)n, out, &digest_length, EVP_md5(), NULL) != 1 || digest_length != 16) {
        memset(material, 0, sizeof material); return -1;
    }
    memset(material, 0, sizeof material);
    return 0;
}
static int turn_hmac_sha1(const uint8_t key[16], const uint8_t *data,
                          size_t length, uint8_t out[20]) {
    unsigned int out_length = 0;
    if (!key || !data || length > INT_MAX ||
        !HMAC(EVP_sha1(), key, 16, data, length, out, &out_length) || out_length != 20) return -1;
    return 0;
}
static int turn_constant_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
#else
static int turn_random(uint8_t *out, size_t length) { (void)out; (void)length; return -1; }
static int turn_derive_key(const char *u, const char *r, const char *p, uint8_t out[16]) {
    (void)u; (void)r; (void)p; (void)out; return -1;
}
static int turn_hmac_sha1(const uint8_t key[16], const uint8_t *data, size_t n, uint8_t out[20]) {
    (void)key; (void)data; (void)n; (void)out; return -1;
}
static int turn_constant_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    (void)a; (void)b; (void)n; return 0;
}
#endif

static int turn_consume_payload(MlbTurnClient *client, const uint8_t *packet, size_t length);

static uint32_t turn_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static int turn_append_attr(uint8_t *packet, size_t capacity, size_t *length,
                            uint16_t type, const void *value, uint16_t value_length) {
    size_t padded = ((size_t)value_length + 3u) & ~(size_t)3u;
    if (!packet || !length || *length > capacity || capacity - *length < 4u + padded) return -1;
    wr16(packet + *length, type); wr16(packet + *length + 2, value_length);
    if (value_length && value) memcpy(packet + *length + 4, value, value_length);
    if (padded > value_length) memset(packet + *length + 4 + value_length, 0, padded - value_length);
    *length += 4u + padded;
    return 0;
}

static int turn_finish_authenticated(uint8_t *packet, size_t capacity, size_t *length,
                                     const uint8_t key[16]) {
    uint8_t digest[20];
    size_t mi_offset;
    if (!packet || !length || *length > capacity || capacity - *length < 32u) return -1;
    mi_offset = *length;
    wr16(packet + mi_offset, TURN_ATTR_MESSAGE_INTEGRITY);
    wr16(packet + mi_offset + 2, 20);
    *length += 24u;
    wr16(packet + 2, (uint16_t)(*length - TURN_HEADER));
    if (turn_hmac_sha1(key, packet, mi_offset, digest) != 0) return -1;
    memcpy(packet + mi_offset + 4, digest, sizeof digest);
    memset(digest, 0, sizeof digest);
    if (turn_append_attr(packet, capacity, length, TURN_ATTR_FINGERPRINT, NULL, 4) != 0) return -1;
    wr16(packet + 2, (uint16_t)(*length - TURN_HEADER));
    wr32(packet + *length - 4, turn_crc32(packet, *length - 8) ^ TURN_FINGERPRINT_XOR);
    return 0;
}

static int turn_init_packet(uint8_t *packet, size_t capacity, uint16_t type,
                            const uint8_t tid[12], size_t *length) {
    if (!packet || capacity < TURN_HEADER || !tid || !length) return -1;
    memset(packet, 0, TURN_HEADER);
    wr16(packet, type); wr32(packet + 4, TURN_MAGIC_COOKIE); memcpy(packet + 8, tid, 12);
    *length = TURN_HEADER;
    return 0;
}

static int turn_encode_xor_address(uint8_t out[20], size_t *out_length,
                                   const uint8_t tid[12], const TurnPeer *peer) {
    uint8_t value[20], mask[16];
    size_t address_length;
    uint16_t port;
    if (!out || !out_length || !tid || !peer) return -1;
    memset(value, 0, sizeof value);
    if (peer->address.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&peer->address;
        value[1] = 0x01; address_length = 4;
        memcpy(value + 4, &a->sin_addr, 4);
        port = ntohs(a->sin_port);
    } else if (peer->address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&peer->address;
        value[1] = 0x02; address_length = 16;
        memcpy(value + 4, &a->sin6_addr, 16);
        port = ntohs(a->sin6_port);
    } else return -1;
    wr16(value + 2, (uint16_t)(port ^ (TURN_MAGIC_COOKIE >> 16)));
    wr32(mask, TURN_MAGIC_COOKIE); memcpy(mask + 4, tid, 12);
    for (size_t i = 0; i < address_length; i++) value[4 + i] ^= mask[i];
    memcpy(out, value, 4 + address_length); *out_length = 4 + address_length;
    return 0;
}

static int turn_decode_xor_address(const uint8_t *value, size_t length,
                                   const uint8_t tid[12], TurnPeer *peer) {
    uint8_t address[16], mask[16];
    size_t address_length;
    uint16_t port;
    if (!value || !tid || !peer || length < 8 || value[0] != 0) return -1;
    memset(peer, 0, sizeof *peer);
    if (value[1] == 0x01 && length == 8) { peer->address.ss_family = AF_INET; address_length = 4; }
    else if (value[1] == 0x02 && length == 20) { peer->address.ss_family = AF_INET6; address_length = 16; }
    else return -1;
    port = (uint16_t)(rd16(value + 2) ^ (TURN_MAGIC_COOKIE >> 16));
    if (!port) return -1;
    memcpy(address, value + 4, address_length);
    wr32(mask, TURN_MAGIC_COOKIE); memcpy(mask + 4, tid, 12);
    for (size_t i = 0; i < address_length; i++) address[i] ^= mask[i];
    if (peer->address.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&peer->address;
        a->sin_family = AF_INET; a->sin_port = htons(port); memcpy(&a->sin_addr, address, 4);
        peer->address_length = sizeof *a;
        if (!inet_ntop(AF_INET, &a->sin_addr, peer->text, sizeof peer->text)) return -1;
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&peer->address;
        a->sin6_family = AF_INET6; a->sin6_port = htons(port); memcpy(&a->sin6_addr, address, 16);
        peer->address_length = sizeof *a;
        if (!inet_ntop(AF_INET6, &a->sin6_addr, peer->text, sizeof peer->text)) return -1;
    }
    peer->port = port;
    return 0;
}

static int mlb_turn_internal_peer_operation(MlbTurnClient *client, uint16_t method,
                                            const TurnPeer *peer, uint16_t channel_number);

static int turn_peer_from_text(const char *text, uint16_t port, TurnPeer *peer) {
    struct sockaddr_in *a4;
    struct sockaddr_in6 *a6;
    if (!text || !*text || !port || !peer || strlen(text) >= sizeof peer->text) return -1;
    memset(peer, 0, sizeof *peer);
    a4 = (struct sockaddr_in *)&peer->address;
    a6 = (struct sockaddr_in6 *)&peer->address;
    if (inet_pton(AF_INET, text, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET; a4->sin_port = htons(port);
        peer->address_length = sizeof *a4;
    } else if (inet_pton(AF_INET6, text, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6; a6->sin6_port = htons(port);
        peer->address_length = sizeof *a6;
    } else return -1;
    memcpy(peer->text, text, strlen(text) + 1); peer->port = port;
    return 0;
}

static int turn_peer_equal(const TurnPeer *a, const TurnPeer *b) {
    if (!a || !b || a->address.ss_family != b->address.ss_family || a->port != b->port) return 0;
    if (a->address.ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)&a->address;
        const struct sockaddr_in *y = (const struct sockaddr_in *)&b->address;
        return x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)&a->address;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)&b->address;
        return x->sin6_scope_id == y->sin6_scope_id &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
    }
    return 0;
}

static int turn_source_matches(const struct sockaddr_storage *source,
                               const struct sockaddr_storage *target) {
    TurnPeer a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.address = *source; b.address = *target;
    if (source->ss_family == AF_INET) {
        a.port = ntohs(((const struct sockaddr_in *)source)->sin_port);
        b.port = ntohs(((const struct sockaddr_in *)target)->sin_port);
    } else if (source->ss_family == AF_INET6) {
        a.port = ntohs(((const struct sockaddr_in6 *)source)->sin6_port);
        b.port = ntohs(((const struct sockaddr_in6 *)target)->sin6_port);
    }
    return turn_peer_equal(&a, &b);
}

static int turn_parse_attrs(const uint8_t *packet, size_t length, size_t *mi_offset,
                            size_t *fp_offset, int *has_mi, int *has_fp) {
    size_t off = TURN_HEADER;
    int seen_mi = 0, seen_fp = 0;
    if (!packet || length < TURN_HEADER || (rd16(packet) & 0xc000u) != 0 ||
        rd32(packet + 4) != TURN_MAGIC_COOKIE || (rd16(packet + 2) & 3u) != 0 ||
        (size_t)rd16(packet + 2) != length - TURN_HEADER) return -1;
    while (off < length) {
        uint16_t type, alen;
        size_t padded;
        if (length - off < 4) return -1;
        type = rd16(packet + off); alen = rd16(packet + off + 2);
        padded = ((size_t)alen + 3u) & ~(size_t)3u;
        if (padded > length - off - 4u) return -1;
        if (seen_mi && type != TURN_ATTR_FINGERPRINT) return -1;
        if (type == TURN_ATTR_MESSAGE_INTEGRITY) {
            if (seen_mi || alen != 20 || seen_fp) return -1;
            seen_mi = 1; if (mi_offset) *mi_offset = off;
        } else if (type == TURN_ATTR_FINGERPRINT) {
            if (seen_fp || alen != 4 || off + 8u != length) return -1;
            seen_fp = 1; if (fp_offset) *fp_offset = off;
        }
        /* RFC 5766 integrity covers all attributes before MESSAGE-INTEGRITY;
           unknown attributes are still parsed and their padding is bounded. */
        off += 4u + padded;
    }
    if (off != length) return -1;
    if (has_mi) *has_mi = seen_mi;
    if (has_fp) *has_fp = seen_fp;
    return 0;
}

static int turn_verify_integrity(const uint8_t *packet, size_t length,
                                 size_t mi_offset, const uint8_t key[16]) {
    uint8_t *header;
    uint8_t digest[20];
    int ok = 0;
    if (!packet || !key || mi_offset < TURN_HEADER || mi_offset + 24u > length) return -1;
    header = (uint8_t *)malloc(mi_offset);
    if (!header) return -1;
    memcpy(header, packet, mi_offset);
    wr16(header + 2, (uint16_t)(mi_offset + 24u - TURN_HEADER));
    if (turn_hmac_sha1(key, header, mi_offset, digest) == 0 &&
        turn_constant_equal(digest, packet + mi_offset + 4, sizeof digest)) ok = 1;
    memset(digest, 0, sizeof digest); memset(header, 0, mi_offset); free(header);
    return ok ? 0 : -1;
}

static int turn_verify_fingerprint(const uint8_t *packet, size_t length, size_t fp_offset) {
    if (!packet || fp_offset + 8u != length ||
        rd32(packet + fp_offset + 4) != (turn_crc32(packet, fp_offset) ^ TURN_FINGERPRINT_XOR)) return -1;
    return 0;
}

typedef struct TurnResponse {
    uint16_t type;
    int error_code;
    char realm[257];
    char nonce[513];
    TurnPeer relayed;
    TurnPeer mapped;
    TurnPeer peer;
    uint8_t *data;
    size_t data_length;
    uint32_t lifetime;
    int have_relayed, have_mapped, have_peer, have_data, have_lifetime;
} TurnResponse;

static int turn_parse_response(MlbTurnClient *client, const uint8_t *packet, size_t length,
                               uint16_t expected_success, uint16_t expected_error,
                               int authenticated_request, TurnResponse *response) {
    size_t mi_offset = 0, fp_offset = 0, off = TURN_HEADER;
    int has_mi = 0, has_fp = 0;
    if (!client || !packet || !response || length > TURN_MAX_PACKET || length < TURN_HEADER ||
        (rd16(packet) != expected_success && rd16(packet) != expected_error) ||
        turn_parse_attrs(packet, length, &mi_offset, &fp_offset, &has_mi, &has_fp) != 0) return -1;
    if (authenticated_request && !has_mi) return -1;
    if (has_mi && (!client->have_credentials ||
        turn_verify_integrity(packet, length, mi_offset, client->integrity_key) != 0)) return -1;
    if (has_fp && turn_verify_fingerprint(packet, length, fp_offset) != 0) return -1;
    memset(response, 0, sizeof *response); response->type = rd16(packet);
    while (off < length) {
        uint16_t type = rd16(packet + off), alen = rd16(packet + off + 2);
        size_t padded = ((size_t)alen + 3u) & ~(size_t)3u;
        const uint8_t *v = packet + off + 4;
        if (type == TURN_ATTR_REALM) {
            if (!alen || alen >= sizeof response->realm || response->realm[0] ||
                memchr(v, '\0', alen)) return -1;
            for (uint16_t i = 0; i < alen; i++) if (v[i] < 0x20u || v[i] > 0x7eu) return -1;
            memcpy(response->realm, v, alen); response->realm[alen] = 0;
        } else if (type == TURN_ATTR_NONCE) {
            if (!alen || alen >= sizeof response->nonce || response->nonce[0] ||
                memchr(v, '\0', alen)) return -1;
            for (uint16_t i = 0; i < alen; i++) if (v[i] < 0x20u || v[i] > 0x7eu) return -1;
            memcpy(response->nonce, v, alen); response->nonce[alen] = 0;
        } else if (type == TURN_ATTR_ERROR_CODE) {
            if (response->error_code || alen < 4 || v[0] || v[1] || v[2] > 6 || v[3] > 99) return -1;
            response->error_code = (int)v[2] * 100 + v[3];
        } else if (type == TURN_ATTR_XOR_RELAYED_ADDR) {
            if (response->have_relayed || turn_decode_xor_address(v, alen, packet + 8, &response->relayed) != 0) return -1;
            response->have_relayed = 1;
        } else if (type == TURN_ATTR_XOR_MAPPED_ADDR) {
            if (response->have_mapped || turn_decode_xor_address(v, alen, packet + 8, &response->mapped) != 0) return -1;
            response->have_mapped = 1;
        } else if (type == TURN_ATTR_XOR_PEER_ADDRESS) {
            if (response->have_peer || turn_decode_xor_address(v, alen, packet + 8, &response->peer) != 0) return -1;
            response->have_peer = 1;
        } else if (type == TURN_ATTR_LIFETIME) {
            if (response->have_lifetime || alen != 4) return -1;
            response->lifetime = rd32(v); response->have_lifetime = 1;
        } else if (type == TURN_ATTR_DATA) {
            if (response->have_data) return -1;
            response->data = (uint8_t *)v; response->data_length = alen; response->have_data = 1;
        }
        off += 4u + padded;
    }
    if (response->type == expected_error && !response->error_code) return -1;
    if (has_mi && response->realm[0] && client->realm[0] &&
        strcmp(response->realm, client->realm) != 0) return -1;
    return 0;
}

static int turn_build_request(MlbTurnClient *client, uint16_t method,
                              const uint8_t tid[12], int authenticate,
                              const uint8_t *extra, size_t extra_length,
                              uint8_t *packet, size_t capacity, size_t *packet_length) {
    size_t n;
    if (turn_init_packet(packet, capacity, method, tid, &n) != 0) return -1;
    if (authenticate) {
        if (!client->have_credentials || !client->realm[0] || !client->nonce[0] ||
            turn_append_attr(packet, capacity, &n, TURN_ATTR_USERNAME, client->username,
                             (uint16_t)strlen(client->username)) != 0 ||
            turn_append_attr(packet, capacity, &n, TURN_ATTR_REALM, client->realm,
                             (uint16_t)strlen(client->realm)) != 0 ||
            turn_append_attr(packet, capacity, &n, TURN_ATTR_NONCE, client->nonce,
                             (uint16_t)strlen(client->nonce)) != 0) return -1;
    }
    if (extra_length && (!extra || n > capacity || extra_length > capacity - n)) return -1;
    if (extra_length) { memcpy(packet + n, extra, extra_length); n += extra_length; }
    if (authenticate && turn_finish_authenticated(packet, capacity, &n, client->integrity_key) != 0) return -1;
    wr16(packet + 2, (uint16_t)(n - TURN_HEADER));
    *packet_length = n;
    return 0;
}

static int turn_transaction(MlbTurnClient *client, uint16_t request_type,
                            uint16_t success_type, uint16_t error_type,
                            const uint8_t *extra, size_t extra_length,
                            TurnResponse *response, int initially_authenticated) {
#ifndef _WIN32
    uint8_t packet[TURN_MAX_PACKET], incoming[TURN_MAX_PACKET];
    int authenticated = initially_authenticated;
    int challenge_retries = 0;
    for (;;) {
        uint8_t tid[12]; size_t packet_length = 0;
        if (turn_random(tid, sizeof tid) != 0 ||
            turn_build_request(client, request_type, tid, authenticated, extra, extra_length,
                               packet, sizeof packet, &packet_length) != 0) return -1;
        if (sendto(client->fd, packet, packet_length, 0,
                   (struct sockaddr *)&client->server, client->server_length) != (ssize_t)packet_length) return -1;
        int64_t start = turn_now_ms();
        if (start < 0) return -1;
        int64_t deadline = start + client->timeout_ms;
        int got_response = 0;
        while (!got_response) {
            int64_t now = turn_now_ms();
            if (now < 0 || now >= deadline) return -1;
            int remain = (int)(deadline - now);
            struct pollfd pfd = { client->fd, POLLIN, 0 };
            int pr = poll(&pfd, 1, remain);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) return -1;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            if (!(pfd.revents & POLLIN)) continue;
            struct sockaddr_storage source; socklen_t source_length = sizeof source;
            ssize_t n = recvfrom(client->fd, incoming, sizeof incoming, 0,
                                 (struct sockaddr *)&source, &source_length);
            if (n < 0) { if (errno == EINTR) continue; return -1; }
            if (!turn_source_matches(&source, &client->server)) continue;
            if ((size_t)n >= 4 && (((rd16(incoming) & 0xc000u) == 0x4000u) ||
                ((rd16(incoming) & 0xc000u) == 0 && rd16(incoming) == TURN_DATA_INDICATION))) {
                (void)turn_consume_payload(client, incoming, (size_t)n);
                continue;
            }
            if ((size_t)n < TURN_HEADER || memcmp(incoming + 8, tid, sizeof tid) != 0) continue;
            if (turn_parse_response(client, incoming, (size_t)n, success_type, error_type,
                                    authenticated, response) != 0) continue;
            got_response = 1;
        }
        if (response->type == success_type) return authenticated ? 0 : -1;
        if ((response->error_code == 401 && !authenticated) ||
            (response->error_code == 438 && authenticated)) {
            if (++challenge_retries > 3 || !response->realm[0] || !response->nonce[0]) return -1;
            if (authenticated && strcmp(response->realm, client->realm) != 0) return -1;
            if (!authenticated) {
                memcpy(client->realm, response->realm, strlen(response->realm) + 1);
                if (turn_derive_key(client->username, client->realm, client->password,
                                    client->integrity_key) != 0) return -1;
                client->have_credentials = 1;
                authenticated = 1;
            }
            memcpy(client->nonce, response->nonce, strlen(response->nonce) + 1);
            continue;
        }
        return -1;
    }
#else
    (void)client; (void)request_type; (void)success_type; (void)error_type;
    (void)extra; (void)extra_length; (void)response; (void)initially_authenticated;
    return -1;
#endif
}

int mlb_turn_available(void) {
#ifdef HAVE_OPENSSL_VAULT
#ifndef _WIN32
    return 1;
#else
    return 0;
#endif
#else
    return 0;
#endif
}

MlbTurnClient *mlb_turn_client_create(const MlbTurnConfig *config) {
#ifndef _WIN32
    struct addrinfo hints, *results = NULL;
    char port_text[8];
    MlbTurnClient *client;
    if (!mlb_turn_available() || !config || !config->server_host || !*config->server_host ||
        !config->server_port || !config->username || !*config->username ||
        !config->password || !turn_printable_ascii(config->username, 256) ||
        !turn_printable_ascii(config->password, 256)) return NULL;
    client = (MlbTurnClient *)calloc(1, sizeof *client);
    if (!client) return NULL;
    client->fd = -1;
    client->timeout_ms = config->timeout_ms ? config->timeout_ms : 1500;
    if (client->timeout_ms < 50 || client->timeout_ms > 10000) { free(client); return NULL; }
    client->requested_lifetime = config->requested_lifetime ? config->requested_lifetime : MLB_TURN_DEFAULT_LIFETIME;
    if (client->requested_lifetime > 86400) { free(client); return NULL; }
    memcpy(client->username, config->username, strlen(config->username) + 1);
    memcpy(client->password, config->password, strlen(config->password) + 1);
    snprintf(port_text, sizeof port_text, "%u", config->server_port);
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(config->server_host, port_text, &hints, &results) != 0) { free(client); return NULL; }
    for (struct addrinfo *ai = results; ai; ai = ai->ai_next) {
        if (!ai->ai_addr || (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)) continue;
        int fd = socket(ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) continue;
        if (ai->ai_addrlen > sizeof client->server) { close(fd); continue; }
        memcpy(&client->server, ai->ai_addr, ai->ai_addrlen);
        client->server_length = (socklen_t)ai->ai_addrlen;
        client->fd = fd; break;
    }
    freeaddrinfo(results);
    if (client->fd < 0) { memset(client, 0, sizeof *client); free(client); return NULL; }
    client->next_channel = 0x4000u;
    return client;
#else
    (void)config; return NULL;
#endif
}

static void turn_queue_clear(MlbTurnClient *client) {
    TurnQueuedPacket *q = client ? client->queue_head : NULL;
    while (q) { TurnQueuedPacket *next = q->next; if (q->data) { memset(q->data, 0, q->length); free(q->data); } free(q); q = next; }
    if (client) { client->queue_head = client->queue_tail = NULL; client->queue_count = client->queue_bytes = 0; }
}

void mlb_turn_client_destroy(MlbTurnClient *client) {
    if (!client) return;
    turn_queue_clear(client);
#ifndef _WIN32
    if (client->fd >= 0) close(client->fd);
#endif
    memset(client->password, 0, sizeof client->password);
    memset(client->integrity_key, 0, sizeof client->integrity_key);
    memset(client->nonce, 0, sizeof client->nonce);
    free(client);
}

static void turn_mapped_to_public(const TurnPeer *peer, MlbStunMappedAddress *out) {
    if (!peer || !out) return;
    memset(out, 0, sizeof *out);
    out->family = peer->address.ss_family; out->port = peer->port;
    memcpy(out->address, peer->text, sizeof out->address);
}

int mlb_turn_allocate(MlbTurnClient *client) {
    uint8_t extra[16] = { 0 };
    TurnResponse response;
    if (!client || client->fd < 0 || client->allocation.active) return -1;
    /* REQUESTED-TRANSPORT: UDP (17), followed by three reserved octets. */
    wr16(extra, TURN_ATTR_REQUESTED_TRANSPORT); wr16(extra + 2, 4);
    extra[4] = 17;
    wr16(extra + 8, TURN_ATTR_LIFETIME); wr16(extra + 10, 4);
    wr32(extra + 12, client->requested_lifetime);
    if (turn_transaction(client, TURN_ALLOCATE_REQUEST, TURN_ALLOCATE_SUCCESS,
                         TURN_ALLOCATE_ERROR, extra, sizeof extra, &response, 0) != 0 ||
        !response.have_relayed || !response.have_lifetime) return -1;
    turn_mapped_to_public(&response.relayed, &client->allocation.relayed_address);
    if (response.have_mapped) turn_mapped_to_public(&response.mapped, &client->allocation.mapped_address);
    client->allocation.lifetime = response.have_lifetime ? response.lifetime : client->requested_lifetime;
    if (!client->allocation.lifetime) return -1;
    client->allocation.active = 1;
    int64_t now = turn_now_ms();
    client->allocation_expires_ms = now < 0 ? 0 : now + (int64_t)client->allocation.lifetime * 1000;
    client->allocation_refresh_ms = now < 0 ? 0 : now + ((int64_t)client->allocation.lifetime * 750);
    return 0;
}

int mlb_turn_refresh(MlbTurnClient *client, uint32_t lifetime_seconds) {
    uint8_t extra[8] = { 0 };
    TurnResponse response;
    if (!client || !client->allocation.active || lifetime_seconds > 86400) return -1;
    wr16(extra, TURN_ATTR_LIFETIME); wr16(extra + 2, 4); wr32(extra + 4, lifetime_seconds);
    if (turn_transaction(client, TURN_REFRESH_REQUEST, TURN_REFRESH_SUCCESS,
                         TURN_REFRESH_ERROR, extra, sizeof extra, &response, 1) != 0 ||
        !response.have_lifetime) return -1;
    client->allocation.lifetime = response.lifetime;
    if (!client->allocation.lifetime) {
        client->allocation.active = 0; client->allocation_expires_ms = 0;
        client->allocation_refresh_ms = 0;
        memset(client->permissions, 0, sizeof client->permissions);
        memset(client->channels, 0, sizeof client->channels);
        turn_queue_clear(client);
        return 0;
    }
    int64_t now = turn_now_ms();
    client->allocation_expires_ms = now < 0 ? 0 : now + (int64_t)client->allocation.lifetime * 1000;
    client->allocation_refresh_ms = now < 0 ? 0 : now + ((int64_t)client->allocation.lifetime * 750);
    return 0;
}

static int turn_allocation_live(MlbTurnClient *client) {
    int64_t now;
    if (!client || !client->allocation.active) return 0;
    now = turn_now_ms();
    if (now < 0 || (client->allocation_expires_ms > 0 && now >= client->allocation_expires_ms)) {
        client->allocation.active = 0;
        client->allocation_refresh_ms = 0;
        memset(client->permissions, 0, sizeof client->permissions);
        memset(client->channels, 0, sizeof client->channels);
        return 0;
    }
    return 1;
}

int mlb_turn_get_allocation(const MlbTurnClient *client, MlbTurnAllocationInfo *info) {
    int64_t now;
    if (!client || !info || !client->allocation.active) return -1;
    now = turn_now_ms();
    if (now < 0 || (client->allocation_expires_ms > 0 && now >= client->allocation_expires_ms)) return -1;
    *info = client->allocation;
    return 0;
}

int mlb_turn_native_fd(const MlbTurnClient *client) {
#ifndef _WIN32
    return client ? client->fd : -1;
#else
    (void)client; return -1;
#endif
}

static int turn_peer_ip_equal(const TurnPeer *a, const TurnPeer *b) {
    if (!a || !b || a->address.ss_family != b->address.ss_family) return 0;
    if (a->address.ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)&a->address;
        const struct sockaddr_in *y = (const struct sockaddr_in *)&b->address;
        return x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)&a->address;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)&b->address;
        return memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
    }
    return 0;
}

static TurnPermission *turn_find_permission(MlbTurnClient *client, const TurnPeer *peer,
                                            int64_t now, int include_expired) {
    for (size_t i = 0; i < sizeof client->permissions / sizeof client->permissions[0]; i++) {
        TurnPermission *p = &client->permissions[i];
        if (p->active && turn_peer_ip_equal(&p->peer, peer) &&
            (include_expired || p->expires_ms > now)) return p;
    }
    return NULL;
}

int mlb_turn_create_permission(MlbTurnClient *client, const char *peer_address, uint16_t peer_port) {
    TurnPeer peer;
    TurnPermission *slot = NULL;
    int64_t now;
    if (!turn_allocation_live(client) || turn_peer_from_text(peer_address, peer_port, &peer) != 0) return -1;
    now = turn_now_ms();
    if (now < 0) return -1;
    if (turn_find_permission(client, &peer, now, 0)) return 0;
    for (size_t i = 0; i < sizeof client->permissions / sizeof client->permissions[0]; i++) {
        if (!client->permissions[i].active || client->permissions[i].expires_ms <= now) { slot = &client->permissions[i]; break; }
    }
    if (!slot) return -1;
    if (mlb_turn_internal_peer_operation(client, TURN_CREATE_PERMISSION_REQ, &peer, 0) != 0) return -1;
    slot->peer = peer; slot->expires_ms = now + TURN_PERMISSION_LIFETIME_MS;
    slot->refresh_ms = now + (TURN_PERMISSION_LIFETIME_MS * 4 / 5); slot->active = 1;
    return 0;
}

static TurnChannel *turn_find_channel(MlbTurnClient *client, const TurnPeer *peer,
                                      int64_t now, int include_expired) {
    for (size_t i = 0; i < MLB_TURN_MAX_CHANNELS; i++) {
        TurnChannel *c = &client->channels[i];
        if (c->active && turn_peer_equal(&c->peer, peer) &&
            (include_expired || c->expires_ms > now)) return c;
    }
    return NULL;
}

int mlb_turn_channel_bind(MlbTurnClient *client, const char *peer_address,
                          uint16_t peer_port, uint16_t *channel_number) {
    TurnPeer peer; TurnChannel *slot = NULL; int64_t now;
    uint16_t channel;
    if (!turn_allocation_live(client) || !channel_number ||
        turn_peer_from_text(peer_address, peer_port, &peer) != 0) return -1;
    now = turn_now_ms(); if (now < 0) return -1;
    if (mlb_turn_create_permission(client, peer_address, peer_port) != 0) return -1;
    TurnChannel *existing = turn_find_channel(client, &peer, now, 0);
    if (existing) {
        if (*channel_number && *channel_number != existing->number) return -1;
        *channel_number = existing->number;
        return 0;
    }
    for (size_t i = 0; i < MLB_TURN_MAX_CHANNELS; i++) {
        if (!client->channels[i].active || client->channels[i].expires_ms <= now) { slot = &client->channels[i]; break; }
    }
    if (!slot) return -1;
    channel = *channel_number;
    if (!channel) {
        channel = client->next_channel++;
        if (client->next_channel > 0x7fffu) client->next_channel = 0x4000u;
    }
    if (channel < 0x4000u || channel > 0x7fffu) return -1;
    for (size_t i = 0; i < MLB_TURN_MAX_CHANNELS; i++) {
        TurnChannel *used = &client->channels[i];
        if (used->active && used->expires_ms > now && used->number == channel &&
            !turn_peer_equal(&used->peer, &peer)) return -1;
    }
    if (mlb_turn_internal_peer_operation(client, TURN_CHANNEL_BIND_REQUEST, &peer, channel) != 0) return -1;
    slot->peer = peer; slot->number = channel; slot->expires_ms = now + TURN_CHANNEL_LIFETIME_MS;
    slot->refresh_ms = now + (TURN_CHANNEL_LIFETIME_MS * 4 / 5); slot->active = 1;
    *channel_number = channel;
    return 0;
}

/* Peer-address operations need a fresh transaction ID to compute XOR-PEER-ADDRESS.
 * Keep this helper in the same file so the raw sockaddr never escapes the API. */
static int mlb_turn_internal_peer_operation(MlbTurnClient *client, uint16_t method,
                                             const TurnPeer *peer, uint16_t channel_number) {
#ifndef _WIN32
    uint8_t packet[TURN_MAX_PACKET], incoming[TURN_MAX_PACKET];
    uint16_t success = method == TURN_CHANNEL_BIND_REQUEST ? TURN_CHANNEL_BIND_SUCCESS : TURN_CREATE_PERMISSION_OK;
    uint16_t error = method == TURN_CHANNEL_BIND_REQUEST ? TURN_CHANNEL_BIND_ERROR : TURN_CREATE_PERMISSION_ERR;
    if (!client || !peer || (method != TURN_CHANNEL_BIND_REQUEST && method != TURN_CREATE_PERMISSION_REQ)) return -1;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        uint8_t tid[12], extra[32], address_value[20];
        size_t n = 0, address_length = 0, packet_length = 0;
        TurnResponse parsed;
        if (turn_random(tid, sizeof tid) != 0 ||
            turn_encode_xor_address(address_value, &address_length, tid, peer) != 0) return -1;
        if (method == TURN_CHANNEL_BIND_REQUEST) {
            wr16(extra, TURN_ATTR_CHANNEL_NUMBER); wr16(extra + 2, 4);
            wr16(extra + 4, channel_number); extra[6] = extra[7] = 0; n = 8;
        }
        wr16(extra + n, TURN_ATTR_XOR_PEER_ADDRESS); wr16(extra + n + 2, (uint16_t)address_length);
        memcpy(extra + n + 4, address_value, address_length);
        size_t padded = (address_length + 3u) & ~(size_t)3u;
        if (padded > address_length) memset(extra + n + 4 + address_length, 0, padded - address_length);
        n += 4u + padded;
        if (turn_build_request(client, method, tid, 1, extra, n,
                               packet, sizeof packet, &packet_length) != 0 ||
            sendto(client->fd, packet, packet_length, 0,
                   (struct sockaddr *)&client->server, client->server_length) != (ssize_t)packet_length) return -1;
        int64_t start = turn_now_ms();
        if (start < 0) return -1;
        int64_t deadline = start + client->timeout_ms;
        for (;;) {
            int64_t now = turn_now_ms();
            if (now < 0 || now >= deadline) return -1;
            struct pollfd pfd = { client->fd, POLLIN, 0 };
            int pr = poll(&pfd, 1, (int)(deadline - now));
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
            struct sockaddr_storage source; socklen_t source_len = sizeof source;
            ssize_t rn = recvfrom(client->fd, incoming, sizeof incoming, 0,
                                  (struct sockaddr *)&source, &source_len);
            if (rn < 0) { if (errno == EINTR) continue; return -1; }
            if (!turn_source_matches(&source, &client->server)) continue;
            if ((size_t)rn >= 4 && (((rd16(incoming) & 0xc000u) == 0x4000u) ||
                ((rd16(incoming) & 0xc000u) == 0 && rd16(incoming) == TURN_DATA_INDICATION))) {
                (void)turn_consume_payload(client, incoming, (size_t)rn);
                continue;
            }
            if ((size_t)rn < TURN_HEADER || memcmp(incoming + 8, tid, 12) != 0) continue;
            if (turn_parse_response(client, incoming, (size_t)rn, success, error, 1, &parsed) != 0) continue;
            if (parsed.type == success) return 0;
            if (parsed.error_code != 438 || !parsed.nonce[0] ||
                (parsed.realm[0] && strcmp(parsed.realm, client->realm) != 0) || attempt == 3) return -1;
            memcpy(client->nonce, parsed.nonce, strlen(parsed.nonce) + 1);
            break; /* Retry with a fresh transaction ID and fresh XOR address. */
        }
    }
    return -1;
#else
    (void)client; (void)method; (void)peer; (void)channel_number; return -1;
#endif
}

static int turn_has_live_permission(MlbTurnClient *client, const TurnPeer *peer, int64_t now) {
    return turn_find_permission(client, peer, now, 0) != NULL;
}

int mlb_turn_send_to(MlbTurnClient *client, const char *peer_address,
                     uint16_t peer_port, const void *data, size_t length) {
#ifndef _WIN32
    TurnPeer peer; int64_t now; TurnChannel *channel;
    uint8_t packet[TURN_MAX_PACKET]; size_t packet_length;
    if (!turn_allocation_live(client) || (!data && length) || !length ||
        length > MLB_TURN_MAX_DATAGRAM - 64u || turn_peer_from_text(peer_address, peer_port, &peer) != 0) return -1;
    now = turn_now_ms(); if (now < 0) return -1;
    if (!turn_has_live_permission(client, &peer, now) &&
        mlb_turn_create_permission(client, peer_address, peer_port) != 0) return -1;
    channel = turn_find_channel(client, &peer, now, 0);
    if (channel) {
        if (length > 65535u || length + 4u > sizeof packet) return -1;
        wr16(packet, channel->number); wr16(packet + 2, (uint16_t)length);
        memcpy(packet + 4, data, length); packet_length = length + 4u;
    } else {
        uint8_t tid[12], address_value[20]; size_t address_length = 0, n;
        if (turn_random(tid, sizeof tid) != 0 || turn_init_packet(packet, sizeof packet, TURN_SEND_INDICATION, tid, &n) != 0 ||
            turn_encode_xor_address(address_value, &address_length, tid, &peer) != 0 ||
            turn_append_attr(packet, sizeof packet, &n, TURN_ATTR_XOR_PEER_ADDRESS,
                             address_value, (uint16_t)address_length) != 0 ||
            turn_append_attr(packet, sizeof packet, &n, TURN_ATTR_DATA, data, (uint16_t)length) != 0) return -1;
        wr16(packet + 2, (uint16_t)(n - TURN_HEADER)); packet_length = n;
    }
    return sendto(client->fd, packet, packet_length, 0,
                  (struct sockaddr *)&client->server, client->server_length) == (ssize_t)packet_length ? 0 : -1;
#else
    (void)client; (void)peer_address; (void)peer_port; (void)data; (void)length; return -1;
#endif
}

static int turn_queue_packet(MlbTurnClient *client, const TurnPeer *peer,
                             const uint8_t *data, size_t length) {
    TurnQueuedPacket *q;
    if (!client || !peer || !data || !length || length > MLB_TURN_MAX_DATAGRAM ||
        client->queue_count >= TURN_QUEUE_LIMIT || client->queue_bytes > TURN_QUEUE_BYTES_LIMIT - length) return -1;
    q = (TurnQueuedPacket *)calloc(1, sizeof *q);
    if (!q) return -1;
    q->data = (uint8_t *)malloc(length);
    if (!q->data) { free(q); return -1; }
    memcpy(q->data, data, length); q->length = length;
    memcpy(q->peer, peer->text, strlen(peer->text) + 1); q->port = peer->port;
    if (client->queue_tail) client->queue_tail->next = q; else client->queue_head = q;
    client->queue_tail = q; client->queue_count++; client->queue_bytes += length;
    return 0;
}

static int turn_parse_data_indication(MlbTurnClient *client, const uint8_t *packet, size_t length) {
    size_t off = TURN_HEADER; TurnPeer peer; int have_peer = 0, have_data = 0;
    const uint8_t *data = NULL; size_t data_length = 0;
    if (length < TURN_HEADER || rd16(packet) != TURN_DATA_INDICATION ||
        rd32(packet + 4) != TURN_MAGIC_COOKIE || (size_t)rd16(packet + 2) != length - TURN_HEADER ||
        (rd16(packet + 2) & 3u)) return 0;
    while (off < length) {
        uint16_t type, alen; size_t padded;
        if (length - off < 4) return 0;
        type = rd16(packet + off); alen = rd16(packet + off + 2);
        padded = ((size_t)alen + 3u) & ~(size_t)3u;
        if (padded > length - off - 4u) return 0;
        if (type == TURN_ATTR_XOR_PEER_ADDRESS) {
            if (have_peer || turn_decode_xor_address(packet + off + 4, alen, packet + 8, &peer) != 0) return 0;
            have_peer = 1;
        } else if (type == TURN_ATTR_DATA) {
            if (have_data) return 0;
            data = packet + off + 4; data_length = alen; have_data = 1;
        }
        off += 4u + padded;
    }
    if (!have_peer || !have_data || !data_length) return 0;
    return turn_queue_packet(client, &peer, data, data_length) == 0 ? 1 : -1;
}

static int turn_parse_channel_data(MlbTurnClient *client, const uint8_t *packet, size_t length) {
    uint16_t channel_number, data_length;
    size_t exact, padded;
    int64_t now;
    if (!client || !packet || length < 4 || (rd16(packet) & 0xc000u) != 0x4000u) return 0;
    channel_number = rd16(packet); data_length = rd16(packet + 2);
    exact = (size_t)data_length + 4u; padded = (exact + 3u) & ~(size_t)3u;
    if ((length != exact && length != padded) || data_length == 0 || length < exact) return 0;
    if (length == padded && padded > exact) {
        for (size_t i = exact; i < padded; i++) if (packet[i] != 0) return 0;
    }
    now = turn_now_ms();
    for (size_t i = 0; i < MLB_TURN_MAX_CHANNELS; i++) {
        TurnChannel *channel = &client->channels[i];
        if (channel->active && channel->number == channel_number && channel->expires_ms > now)
            return turn_queue_packet(client, &channel->peer, packet + 4, data_length) == 0 ? 1 : -1;
    }
    return 0;
}

static int turn_consume_payload(MlbTurnClient *client, const uint8_t *packet, size_t length) {
    if (!client || !packet || length < 4) return 0;
    if ((rd16(packet) & 0xc000u) == 0x4000u)
        return turn_parse_channel_data(client, packet, length);
    if (length >= TURN_HEADER && rd16(packet) == TURN_DATA_INDICATION)
        return turn_parse_data_indication(client, packet, length);
    return 0;
}

int mlb_turn_poll(MlbTurnClient *client, unsigned timeout_ms) {
#ifndef _WIN32
    uint8_t packet[TURN_MAX_PACKET]; struct pollfd pfd;
    struct sockaddr_storage source; socklen_t source_length = sizeof source;
    if (!client || client->fd < 0 || !turn_allocation_live(client)) return -1;
    if (timeout_ms > 10000) timeout_ms = 10000;
    pfd.fd = client->fd; pfd.events = POLLIN; pfd.revents = 0;
    int pr;
    do { pr = poll(&pfd, 1, (int)timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr == 0) return 0;
    if (pr < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
    ssize_t n = recvfrom(client->fd, packet, sizeof packet, 0, (struct sockaddr *)&source, &source_length);
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (!turn_source_matches(&source, &client->server)) return 0;
    return turn_consume_payload(client, packet, (size_t)n);
#else
    (void)client; (void)timeout_ms; return -1;
#endif
}

int mlb_turn_receive(MlbTurnClient *client, char *peer_address,
                     size_t peer_address_capacity, uint16_t *peer_port,
                     void *data, size_t capacity, size_t *received_length) {
    TurnQueuedPacket *q;
    if (!client || !peer_address || !peer_address_capacity || !peer_port || !data || !received_length ||
        !(q = client->queue_head)) return -1;
    if (strlen(q->peer) + 1 > peer_address_capacity || q->length > capacity) return -1;
    memcpy(peer_address, q->peer, strlen(q->peer) + 1); *peer_port = q->port;
    memcpy(data, q->data, q->length); *received_length = q->length;
    client->queue_head = q->next; if (!client->queue_head) client->queue_tail = NULL;
    client->queue_count--; client->queue_bytes -= q->length;
    memset(q->data, 0, q->length); free(q->data); free(q);
    return 0;
}


int mlb_turn_maintain(MlbTurnClient *client) {
#ifndef _WIN32
    int64_t now;
    if (!turn_allocation_live(client)) return -1;
    now = turn_now_ms();
    if (now < 0) return -1;
    if (client->allocation_refresh_ms > 0 && now >= client->allocation_refresh_ms) {
        if (mlb_turn_refresh(client, client->requested_lifetime) != 0) return -1;
        now = turn_now_ms();
        if (now < 0) return -1;
    }
    for (size_t i = 0; i < sizeof client->permissions / sizeof client->permissions[0]; i++) {
        TurnPermission *permission = &client->permissions[i];
        if (!permission->active || permission->refresh_ms <= 0 || now < permission->refresh_ms) continue;
        char ip[46]; uint16_t port = permission->peer.port;
        int64_t old_expiry = permission->expires_ms, old_refresh = permission->refresh_ms;
        memcpy(ip, permission->peer.text, sizeof ip);
        /* Expire only the local cache entry to force a fresh CreatePermission.
           Restore its old deadline on failure so the caller can retry safely. */
        permission->expires_ms = now;
        if (mlb_turn_create_permission(client, ip, port) != 0) {
            permission->expires_ms = old_expiry; permission->refresh_ms = old_refresh;
            return -1;
        }
        now = turn_now_ms();
        if (now < 0) return -1;
    }
    for (size_t i = 0; i < MLB_TURN_MAX_CHANNELS; i++) {
        TurnChannel *channel = &client->channels[i];
        if (!channel->active || channel->refresh_ms <= 0 || now < channel->refresh_ms) continue;
        char ip[46]; uint16_t port = channel->peer.port, number = channel->number;
        int64_t old_expiry = channel->expires_ms, old_refresh = channel->refresh_ms;
        memcpy(ip, channel->peer.text, sizeof ip);
        channel->expires_ms = now;
        if (mlb_turn_channel_bind(client, ip, port, &number) != 0) {
            channel->expires_ms = old_expiry; channel->refresh_ms = old_refresh;
            return -1;
        }
        now = turn_now_ms();
        if (now < 0) return -1;
    }
    return 0;
#else
    (void)client; return -1;
#endif
}
