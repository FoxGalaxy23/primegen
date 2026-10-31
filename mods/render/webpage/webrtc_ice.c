#define _POSIX_C_SOURCE 200809L
#include "webrtc_ice.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <strings.h>
#include <arpa/inet.h>
#endif

#ifdef HAVE_OPENSSL_VAULT
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_BINDING_REQUEST 0x0001u
#define STUN_BINDING_SUCCESS 0x0101u
#define STUN_BINDING_ERROR 0x0111u
#define STUN_ATTR_USERNAME 0x0006u
#define STUN_ATTR_MESSAGE_INTEGRITY 0x0008u
#define STUN_ATTR_ERROR_CODE 0x0009u
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020u
#define STUN_ATTR_PRIORITY 0x0024u
#define STUN_ATTR_USE_CANDIDATE 0x0025u
#define STUN_ATTR_ICE_CONTROLLED 0x8029u
#define STUN_ATTR_ICE_CONTROLLING 0x802au
#define STUN_ATTR_FINGERPRINT 0x8028u
#define STUN_FINGERPRINT_XOR 0x5354554eu

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v);
}

int mlb_ice_crypto_available(void) {
#ifdef HAVE_OPENSSL_VAULT
    return 1;
#else
    return 0;
#endif
}

static int secure_random(uint8_t *out, size_t length) {
#ifdef HAVE_OPENSSL_VAULT
    if (!out || !length || length > INT_MAX) return -1;
    return RAND_bytes(out, (int)length) == 1 ? 0 : -1;
#else
    (void)out; (void)length;
    return -1;
#endif
}

int mlb_ice_generate_credentials(char *ufrag, size_t ufrag_capacity,
                                 char *password, size_t password_capacity) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t random[40];
    if (!ufrag || !password || ufrag_capacity < 9 || password_capacity < 33) return -1;
    if (secure_random(random, sizeof random) != 0) return -1;
    for (size_t i = 0; i < 8; i++) ufrag[i] = alphabet[random[i] & 63u];
    ufrag[8] = '\0';
    for (size_t i = 0; i < 32; i++) password[i] = alphabet[random[8 + i] & 63u];
    password[32] = '\0';
    memset(random, 0, sizeof random);
    return 0;
}

static int parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out) {
    char *end = NULL;
    unsigned long v;
    if (!s || !*s || !out || *s == '-' || *s == '+') return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || !end || *end || v < min || v > max) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int valid_foundation(const char *s) {
    if (!s || !*s || strlen(s) > MLB_ICE_MAX_FOUNDATION) return 0;
    for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '+' || *s == '/')) return 0;
    return 1;
}

static int valid_address_token(const char *s) {
    struct in_addr a4;
    struct in6_addr a6;
    size_t n;
    int label_len = 0;
    if (!s || !(n = strlen(s)) || n > MLB_ICE_MAX_ADDRESS) return 0;
    if (inet_pton(AF_INET, s, &a4) == 1 || inet_pton(AF_INET6, s, &a6) == 1) return 1;
    /* ICE permits mDNS host candidates. Accept DNS-style names, but no URI
       punctuation, empty labels, leading/trailing hyphens or control bytes. */
    if (n > 1 && s[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (label_len == 0 || label_len > 63 || s[i - 1] == '-') return 0;
            label_len = 0; continue;
        }
        if (!(isalnum(c) || c == '-') || (label_len == 0 && c == '-')) return 0;
        if (++label_len > 63) return 0;
    }
    return label_len > 0 && s[n - 1] != '-';
}

int mlb_ice_parse_candidate(const char *text, MlbIceCandidate *out) {
    char copy[MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
    char *tokens[32];
    size_t count = 0;
    char *p;
    uint32_t value;
    MlbIceCandidate c;
    if (!text || !out) return -1;
    size_t n = strnlen(text, sizeof copy);
    if (!n || n >= sizeof copy) return -1;
    memcpy(copy, text, n + 1);
    p = copy;
    if (!strncmp(p, "a=", 2)) p += 2;
    if (!strncmp(p, "candidate:", 10)) p += 10;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (count == sizeof tokens / sizeof tokens[0]) return -1;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    if (count < 8 || strcmp(tokens[6], "typ") != 0 || !valid_foundation(tokens[0])) return -1;
    memset(&c, 0, sizeof c);
    memcpy(c.foundation, tokens[0], strlen(tokens[0]) + 1);
    if (parse_u32(tokens[1], 1, 256, &value) != 0) return -1;
    c.component = (uint16_t)value;
    if (!strcasecmp(tokens[2], "udp")) c.transport = MLB_ICE_UDP;
    else if (!strcasecmp(tokens[2], "tcp")) c.transport = MLB_ICE_TCP;
    else return -1;
    if (parse_u32(tokens[3], 1, UINT32_MAX, &c.priority) != 0 ||
        !valid_address_token(tokens[4]) || parse_u32(tokens[5], 1, 65535, &value) != 0) return -1;
    memcpy(c.address, tokens[4], strlen(tokens[4]) + 1);
    c.port = (uint16_t)value;
    if (!strcmp(tokens[7], "host")) c.type = MLB_ICE_CANDIDATE_HOST;
    else if (!strcmp(tokens[7], "srflx")) c.type = MLB_ICE_CANDIDATE_SRFLX;
    else if (!strcmp(tokens[7], "prflx")) c.type = MLB_ICE_CANDIDATE_PRFLX;
    else if (!strcmp(tokens[7], "relay")) c.type = MLB_ICE_CANDIDATE_RELAY;
    else return -1;
    if (((count - 8u) & 1u) != 0) return -1;
    for (size_t i = 8; i < count; i += 2) {
        if (!strcmp(tokens[i], "raddr")) {
            if (c.related_address[0] || !valid_address_token(tokens[i + 1])) return -1;
            memcpy(c.related_address, tokens[i + 1], strlen(tokens[i + 1]) + 1);
        } else if (!strcmp(tokens[i], "rport")) {
            if (c.related_port || parse_u32(tokens[i + 1], 1, 65535, &value) != 0) return -1;
            c.related_port = (uint16_t)value;
        } else if (!strcmp(tokens[i], "tcptype")) {
            if (c.tcp_type[0] || (strcmp(tokens[i + 1], "active") &&
                strcmp(tokens[i + 1], "passive") && strcmp(tokens[i + 1], "so"))) return -1;
            memcpy(c.tcp_type, tokens[i + 1], strlen(tokens[i + 1]) + 1);
        } else if (!strcmp(tokens[i], "generation") || !strcmp(tokens[i], "network-id") ||
                   !strcmp(tokens[i], "network-cost")) {
            if (parse_u32(tokens[i + 1], 0, UINT32_MAX, &value) != 0) return -1;
        } /* Unknown extension attributes are intentionally forward-compatible. */
    }
    if ((c.type == MLB_ICE_CANDIDATE_HOST && (c.related_address[0] || c.related_port)) ||
        ((c.related_address[0] != '\0') != (c.related_port != 0)) ||
        (c.transport == MLB_ICE_TCP && !c.tcp_type[0]) ||
        (c.transport == MLB_ICE_UDP && c.tcp_type[0])) return -1;
    *out = c;
    return 0;
}

#ifdef HAVE_OPENSSL_VAULT
static int hmac_sha1(const char *key, const uint8_t *data, size_t length, uint8_t out[20]) {
    unsigned int out_len = 0;
    if (!key || !*key || strlen(key) > MLB_ICE_MAX_PASSWORD || length > INT_MAX) return -1;
    if (!HMAC(EVP_sha1(), key, (int)strlen(key), data, length, out, &out_len) || out_len != 20) return -1;
    return 0;
}
static uint32_t crc32_stun(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}
static int secure_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
#else
static int hmac_sha1(const char *key, const uint8_t *data, size_t length, uint8_t out[20]) {
    (void)key; (void)data; (void)length; (void)out; return -1;
}
static uint32_t crc32_stun(const uint8_t *data, size_t length) {
    (void)data; (void)length; return 0;
}
static int secure_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    (void)a; (void)b; (void)n; return 0;
}
#endif

typedef struct StunAttrLocation {
    size_t mi_offset;
    size_t fp_offset;
    const uint8_t *username;
    size_t username_length;
    int have_mi;
    int have_fp;
} StunAttrLocation;

static int scan_stun_attrs(const uint8_t *packet, size_t length, StunAttrLocation *loc) {
    size_t off = MLB_STUN_HEADER_SIZE;
    memset(loc, 0, sizeof *loc);
    while (off < length) {
        uint16_t type, alen;
        size_t padded;
        if (length - off < 4) return -1;
        type = rd16(packet + off); alen = rd16(packet + off + 2);
        padded = ((size_t)alen + 3u) & ~(size_t)3u;
        if (padded > length - off - 4) return -1;
        /* MESSAGE-INTEGRITY authenticates only attributes before itself.
           ICE check fields after it would otherwise be attacker-controlled. */
        if (loc->have_mi && type != STUN_ATTR_FINGERPRINT) return -1;
        if (type == STUN_ATTR_USERNAME) {
            if (loc->username || !alen || alen > 512) return -1;
            loc->username = packet + off + 4; loc->username_length = alen;
        } else if (type == STUN_ATTR_MESSAGE_INTEGRITY) {
            if (loc->have_mi || alen != 20 || loc->have_fp) return -1;
            loc->have_mi = 1; loc->mi_offset = off;
        } else if (type == STUN_ATTR_FINGERPRINT) {
            if (loc->have_fp || alen != 4 || off + 8 != length) return -1;
            loc->have_fp = 1; loc->fp_offset = off;
        }
        off += 4 + padded;
    }
    return off == length ? 0 : -1;
}

static int validate_stun_envelope(const uint8_t *packet, size_t length, uint16_t expected_type) {
    if (!packet || length < MLB_STUN_HEADER_SIZE || length > MLB_ICE_STUN_MAX_MESSAGE ||
        (packet[0] & 0xc0u) != 0 || rd16(packet) != expected_type ||
        rd32(packet + 4) != STUN_MAGIC_COOKIE ||
        (rd16(packet + 2) & 3u) != 0 || (size_t)rd16(packet + 2) != length - MLB_STUN_HEADER_SIZE)
        return -1;
    return 0;
}

static int append_attr(uint8_t *out, size_t capacity, size_t *length,
                       uint16_t type, const void *value, uint16_t value_length) {
    size_t padded = ((size_t)value_length + 3u) & ~(size_t)3u;
    if (!out || !length || *length > capacity || capacity - *length < 4u + padded) return -1;
    wr16(out + *length, type); wr16(out + *length + 2, value_length);
    if (value_length && value) memcpy(out + *length + 4, value, value_length);
    if (padded > value_length) memset(out + *length + 4 + value_length, 0, padded - value_length);
    *length += 4u + padded;
    return 0;
}

static int finish_integrity_and_fingerprint(uint8_t *out, size_t capacity, size_t *length,
                                            const char *integrity_key) {
    uint8_t digest[20];
    size_t mi_offset;
    if (!out || !length || *length > capacity || capacity - *length < 32u) return -1;
    mi_offset = *length;
    wr16(out + mi_offset, STUN_ATTR_MESSAGE_INTEGRITY); wr16(out + mi_offset + 2, 20);
    *length += 24;
    wr16(out + 2, (uint16_t)(*length - MLB_STUN_HEADER_SIZE));
    /* Per RFC 8489, HMAC covers the header and attributes preceding
       MESSAGE-INTEGRITY, while the header length includes the MI attribute. */
    if (hmac_sha1(integrity_key, out, mi_offset, digest) != 0) return -1;
    memcpy(out + mi_offset + 4, digest, sizeof digest);
    if (append_attr(out, capacity, length, STUN_ATTR_FINGERPRINT, NULL, 4) != 0) return -1;
    wr16(out + 2, (uint16_t)(*length - MLB_STUN_HEADER_SIZE));
    wr32(out + *length - 4, crc32_stun(out, *length - 8) ^ STUN_FINGERPRINT_XOR);
    memset(digest, 0, sizeof digest);
    return 0;
}

static int verify_integrity_and_fingerprint(const uint8_t *packet, size_t length,
                                            const char *key, const StunAttrLocation *loc) {
    uint8_t digest[20];
    uint8_t header[MLB_ICE_STUN_MAX_MESSAGE];
    if (!loc->have_mi || !loc->have_fp || loc->mi_offset >= loc->fp_offset) return -1;
    memcpy(header, packet, loc->mi_offset);
    /* HMAC's header length is the end of MESSAGE-INTEGRITY, not FINGERPRINT. */
    wr16(header + 2, (uint16_t)(loc->mi_offset + 24u - MLB_STUN_HEADER_SIZE));
    if (hmac_sha1(key, header, loc->mi_offset, digest) != 0 ||
        !secure_equal(digest, packet + loc->mi_offset + 4, sizeof digest)) {
        memset(digest, 0, sizeof digest); return -1;
    }
    if (rd32(packet + loc->fp_offset + 4) !=
        (crc32_stun(packet, loc->fp_offset) ^ STUN_FINGERPRINT_XOR)) {
        memset(digest, 0, sizeof digest); return -1;
    }
    memset(digest, 0, sizeof digest);
    (void)length;
    return 0;
}

static int init_stun(uint8_t *out, size_t capacity, uint16_t type,
                     const uint8_t transaction_id[12], size_t *length) {
    if (!out || !transaction_id || !length || capacity < MLB_STUN_HEADER_SIZE) return -1;
    memset(out, 0, MLB_STUN_HEADER_SIZE);
    wr16(out, type); wr32(out + 4, STUN_MAGIC_COOKIE); memcpy(out + 8, transaction_id, 12);
    *length = MLB_STUN_HEADER_SIZE;
    return 0;
}

int mlb_ice_build_check_request(uint8_t *out, size_t capacity,
                                const uint8_t transaction_id[12], const char *username,
                                const char *integrity_key, uint32_t priority,
                                uint64_t tie_breaker, int role, int nominate,
                                size_t *out_length) {
    size_t n, un;
    uint8_t number[8];
    if (!out_length || !mlb_ice_crypto_available() || !username || !(un = strlen(username)) || un > 512 ||
        !integrity_key || !*integrity_key || strlen(integrity_key) > MLB_ICE_MAX_PASSWORD ||
        !priority || !tie_breaker || (role != MLB_ICE_ROLE_CONTROLLING && role != MLB_ICE_ROLE_CONTROLLED) ||
        (nominate && role != MLB_ICE_ROLE_CONTROLLING) || init_stun(out, capacity, STUN_BINDING_REQUEST, transaction_id, &n) != 0)
        return -1;
    if (append_attr(out, capacity, &n, STUN_ATTR_USERNAME, username, (uint16_t)un) != 0) return -1;
    wr32(number, priority);
    if (append_attr(out, capacity, &n, STUN_ATTR_PRIORITY, number, 4) != 0) return -1;
    wr64(number, tie_breaker);
    if (append_attr(out, capacity, &n, role == MLB_ICE_ROLE_CONTROLLING ?
                    STUN_ATTR_ICE_CONTROLLING : STUN_ATTR_ICE_CONTROLLED, number, 8) != 0) return -1;
    if (nominate && append_attr(out, capacity, &n, STUN_ATTR_USE_CANDIDATE, NULL, 0) != 0) return -1;
    if (finish_integrity_and_fingerprint(out, capacity, &n, integrity_key) != 0) return -1;
    *out_length = n;
    return 0;
}

int mlb_ice_verify_check_request(const uint8_t *packet, size_t packet_length,
                                 const char *expected_username, const char *integrity_key,
                                 MlbIceCheckInfo *info) {
    StunAttrLocation loc;
    MlbIceCheckInfo parsed;
    int have_priority = 0, have_role = 0, have_use_candidate = 0;
    size_t off = MLB_STUN_HEADER_SIZE;
    if (!mlb_ice_crypto_available() || !expected_username || strlen(expected_username) > 512 ||
        !integrity_key || !info ||
        validate_stun_envelope(packet, packet_length, STUN_BINDING_REQUEST) != 0 ||
        scan_stun_attrs(packet, packet_length, &loc) != 0 || !loc.username ||
        loc.username_length != strlen(expected_username) ||
        memcmp(loc.username, expected_username, loc.username_length) != 0 ||
        verify_integrity_and_fingerprint(packet, packet_length, integrity_key, &loc) != 0) return -1;
    memset(&parsed, 0, sizeof parsed);
    while (off < packet_length) {
        uint16_t type = rd16(packet + off), alen = rd16(packet + off + 2);
        size_t padded = ((size_t)alen + 3u) & ~(size_t)3u;
        const uint8_t *v = packet + off + 4;
        if (type == STUN_ATTR_PRIORITY) {
            if (have_priority || alen != 4 || !(parsed.priority = rd32(v))) return -1;
            have_priority = 1;
        } else if (type == STUN_ATTR_ICE_CONTROLLING || type == STUN_ATTR_ICE_CONTROLLED) {
            if (have_role || alen != 8 || !(parsed.peer_tie_breaker = rd64(v))) return -1;
            parsed.peer_role = type == STUN_ATTR_ICE_CONTROLLING ? MLB_ICE_ROLE_CONTROLLING : MLB_ICE_ROLE_CONTROLLED;
            have_role = 1;
        } else if (type == STUN_ATTR_USE_CANDIDATE) {
            if (have_use_candidate || alen != 0) return -1;
            parsed.use_candidate = 1; have_use_candidate = 1;
        }
        off += 4 + padded;
    }
    if (!have_priority || !have_role ||
        (parsed.use_candidate && parsed.peer_role != MLB_ICE_ROLE_CONTROLLING)) return -1;
    *info = parsed;
    return 0;
}

static int encode_xor_mapped(uint8_t out[20], size_t *length,
                             const uint8_t tid[12], const MlbStunMappedAddress *mapped) {
    uint8_t v[20];
    uint16_t port;
    size_t addrlen;
    if (!mapped || !length || !memchr(mapped->address, '\0', sizeof mapped->address)) return -1;
    memset(v, 0, sizeof v);
    if (mapped->family == AF_INET) { v[1] = 0x01; addrlen = 4; }
    else if (mapped->family == AF_INET6) { v[1] = 0x02; addrlen = 16; }
    else return -1;
    port = mapped->port ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16); wr16(v + 2, port);
    if (inet_pton(mapped->family, mapped->address, v + 4) != 1) return -1;
    uint8_t mask[16]; wr32(mask, STUN_MAGIC_COOKIE); memcpy(mask + 4, tid, 12);
    for (size_t i = 0; i < addrlen; i++) v[4 + i] ^= mask[i];
    *length = 4 + addrlen;
    memcpy(out, v, *length);
    return 0;
}

int mlb_ice_build_check_success(uint8_t *out, size_t capacity,
                                const uint8_t transaction_id[12], const char *integrity_key,
                                const MlbStunMappedAddress *mapped, size_t *out_length) {
    size_t n, value_length;
    uint8_t value[20];
    if (!out_length || !mlb_ice_crypto_available() || !integrity_key || !*integrity_key ||
        strlen(integrity_key) > MLB_ICE_MAX_PASSWORD ||
        init_stun(out, capacity, STUN_BINDING_SUCCESS, transaction_id, &n) != 0 ||
        encode_xor_mapped(value, &value_length, transaction_id, mapped) != 0 ||
        append_attr(out, capacity, &n, STUN_ATTR_XOR_MAPPED_ADDRESS, value, (uint16_t)value_length) != 0 ||
        finish_integrity_and_fingerprint(out, capacity, &n, integrity_key) != 0) return -1;
    *out_length = n;
    return 0;
}

int mlb_ice_verify_check_success(const uint8_t *packet, size_t packet_length,
                                 const uint8_t transaction_id[12], const char *integrity_key,
                                 MlbStunMappedAddress *mapped) {
    StunAttrLocation loc;
    uint8_t address_value[20]; size_t address_length = 0;
    if (!mlb_ice_crypto_available() || !transaction_id || !integrity_key || !mapped ||
        validate_stun_envelope(packet, packet_length, STUN_BINDING_SUCCESS) != 0 ||
        memcmp(packet + 8, transaction_id, 12) != 0 ||
        scan_stun_attrs(packet, packet_length, &loc) != 0 ||
        verify_integrity_and_fingerprint(packet, packet_length, integrity_key, &loc) != 0) return -1;
    size_t off = MLB_STUN_HEADER_SIZE;
    while (off < packet_length) {
        uint16_t type = rd16(packet + off), alen = rd16(packet + off + 2);
        size_t padded = ((size_t)alen + 3u) & ~(size_t)3u;
        if (type == STUN_ATTR_XOR_MAPPED_ADDRESS) {
            if (address_length || alen > sizeof address_value) return -1;
            memcpy(address_value, packet + off + 4, alen); address_length = alen;
        }
        off += 4 + padded;
    }
    if (!address_length) return -1;
    /* Reuse the strict RFC 8489 address decoder by wrapping this attribute in
       a minimal synthetic Binding Success response. */
    uint8_t synthetic[MLB_STUN_HEADER_SIZE + 24];
    size_t synthetic_length = MLB_STUN_HEADER_SIZE + 4 + ((address_length + 3u) & ~(size_t)3u);
    memset(synthetic, 0, sizeof synthetic);
    wr16(synthetic, STUN_BINDING_SUCCESS); wr16(synthetic + 2, (uint16_t)(synthetic_length - MLB_STUN_HEADER_SIZE));
    wr32(synthetic + 4, STUN_MAGIC_COOKIE); memcpy(synthetic + 8, transaction_id, 12);
    wr16(synthetic + 20, STUN_ATTR_XOR_MAPPED_ADDRESS); wr16(synthetic + 22, (uint16_t)address_length);
    memcpy(synthetic + 24, address_value, address_length);
    return mlb_stun_parse_binding_response(synthetic, synthetic_length, transaction_id, mapped);
}


int mlb_ice_build_role_conflict(uint8_t *out, size_t capacity,
                                const uint8_t transaction_id[12],
                                const char *integrity_key, size_t *out_length) {
    static const char reason[] = "Role Conflict";
    uint8_t value[4 + sizeof reason - 1u];
    size_t n;
    if (!out_length || !mlb_ice_crypto_available() || !integrity_key || !*integrity_key ||
        strlen(integrity_key) > MLB_ICE_MAX_PASSWORD ||
        init_stun(out, capacity, STUN_BINDING_ERROR, transaction_id, &n) != 0) return -1;
    memset(value, 0, sizeof value);
    value[2] = 4; value[3] = 87;
    memcpy(value + 4, reason, sizeof reason - 1u);
    if (append_attr(out, capacity, &n, STUN_ATTR_ERROR_CODE, value, (uint16_t)sizeof value) != 0 ||
        finish_integrity_and_fingerprint(out, capacity, &n, integrity_key) != 0) return -1;
    *out_length = n;
    return 0;
}

int mlb_ice_verify_role_conflict(const uint8_t *packet, size_t packet_length,
                                 const uint8_t transaction_id[12], const char *integrity_key) {
    StunAttrLocation loc;
    int found = 0;
    if (!mlb_ice_crypto_available() || !transaction_id || !integrity_key ||
        validate_stun_envelope(packet, packet_length, STUN_BINDING_ERROR) != 0 ||
        memcmp(packet + 8, transaction_id, 12) != 0 ||
        scan_stun_attrs(packet, packet_length, &loc) != 0 ||
        verify_integrity_and_fingerprint(packet, packet_length, integrity_key, &loc) != 0) return -1;
    for (size_t off = MLB_STUN_HEADER_SIZE; off < packet_length;) {
        uint16_t type = rd16(packet + off), alen = rd16(packet + off + 2);
        size_t padded = ((size_t)alen + 3u) & ~(size_t)3u;
        if (type == STUN_ATTR_ERROR_CODE) {
            const uint8_t *v = packet + off + 4;
            if (found || alen < 4 || v[0] != 0 || v[1] != 0 || v[2] != 4 || v[3] != 87)
                return -1;
            found = 1;
        }
        off += 4 + padded;
    }
    return found ? 0 : -1;
}
