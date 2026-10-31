#ifndef MLBROWSER_WEBRTC_STUN_H
#define MLBROWSER_WEBRTC_STUN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 8489 STUN base header and Binding method. */
#define MLB_STUN_HEADER_SIZE 20u
#define MLB_STUN_TRANSACTION_ID_SIZE 12u
#define MLB_STUN_MAX_MESSAGE_SIZE 4096u

typedef struct MlbStunMappedAddress {
    /* AF_INET or AF_INET6 on POSIX; intentionally kept as an int in this API. */
    int family;
    uint16_t port;                 /* Host byte order. */
    char address[46];              /* inet_ntop representation, including IPv6. */
} MlbStunMappedAddress;

/* Build a STUN Binding Request with a caller-supplied 96-bit transaction ID.
 * Returns 0 on success and -1 on invalid arguments or insufficient capacity. */
int mlb_stun_build_binding_request(uint8_t *out, size_t capacity,
                                   const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                   size_t *out_length);

/* Parse a Binding Success Response for the matching transaction ID.
 * Validates the complete STUN header and all TLV boundaries. Prefers
 * XOR-MAPPED-ADDRESS and accepts MAPPED-ADDRESS as a compatibility fallback.
 * Returns 0 on success and -1 for a malformed, unrelated, or unsuccessful reply. */
int mlb_stun_parse_binding_response(const uint8_t *packet, size_t packet_length,
                                    const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                    MlbStunMappedAddress *mapped);

/* Perform one bounded UDP STUN Binding transaction against host:port.
 * timeout_ms must be in [50, 10000] and bounds the complete UDP transaction. This is a primitive for ICE gathering,
 * not an ICE agent: it discovers only the server-reflexive address visible to
 * the selected STUN server. Returns 0 on success, -1 on failure/timeout.
 * On builds whose platform backend is not yet implemented, returns -1. */
int mlb_stun_query(const char *host, uint16_t port, unsigned timeout_ms,
                   MlbStunMappedAddress *mapped);

#ifdef __cplusplus
}
#endif
#endif
