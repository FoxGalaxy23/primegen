#ifndef MLBROWSER_WEBRTC_ICE_H
#define MLBROWSER_WEBRTC_ICE_H

#include <stddef.h>
#include <stdint.h>
#include "webrtc_stun.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLB_ICE_MAX_FOUNDATION 32u
#define MLB_ICE_MAX_ADDRESS 253u
#define MLB_ICE_MAX_CANDIDATE_TEXT 512u
#define MLB_ICE_MAX_UFRAG 256u
#define MLB_ICE_MAX_PASSWORD 256u
#define MLB_ICE_STUN_MAX_MESSAGE 4096u

enum MlbIceTransport { MLB_ICE_UDP = 1, MLB_ICE_TCP = 2 };
enum MlbIceCandidateType {
    MLB_ICE_CANDIDATE_HOST = 1,
    MLB_ICE_CANDIDATE_SRFLX = 2,
    MLB_ICE_CANDIDATE_PRFLX = 3,
    MLB_ICE_CANDIDATE_RELAY = 4
};
enum MlbIceRole { MLB_ICE_ROLE_CONTROLLED = 0, MLB_ICE_ROLE_CONTROLLING = 1 };

typedef struct MlbIceCandidate {
    char foundation[MLB_ICE_MAX_FOUNDATION + 1u];
    uint16_t component;
    int transport;
    uint32_t priority;
    char address[MLB_ICE_MAX_ADDRESS + 1u];
    uint16_t port;
    int type;
    char related_address[MLB_ICE_MAX_ADDRESS + 1u];
    uint16_t related_port;
    char tcp_type[8];
} MlbIceCandidate;

typedef struct MlbIceCheckInfo {
    uint32_t priority;
    uint64_t peer_tie_breaker;
    int peer_role;
    int use_candidate;
} MlbIceCheckInfo;

/* Returns non-zero only when the secure random/HMAC provider is available. */
int mlb_ice_crypto_available(void);

/* Generates RFC 8445 credentials using an OS cryptographic random source.
 * Outputs are NUL-terminated; ufrag is 8 chars and password is 32 chars. */
int mlb_ice_generate_credentials(char *ufrag, size_t ufrag_capacity,
                                 char *password, size_t password_capacity);

/* Parse an SDP candidate attribute (with or without the leading "a=").
 * Unknown extension name/value pairs are ignored, malformed known values fail. */
int mlb_ice_parse_candidate(const char *text, MlbIceCandidate *out);

/* Construct an authenticated ICE connectivity-check Binding Request.
 * username is remote_ufrag:local_ufrag and integrity_key is the remote ICE pwd.
 * Fingerprint and MESSAGE-INTEGRITY are appended as required by ICE. */
int mlb_ice_build_check_request(uint8_t *out, size_t capacity,
                                const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                const char *username, const char *integrity_key,
                                uint32_t priority, uint64_t tie_breaker,
                                int role, int nominate, size_t *out_length);

/* Validate an authenticated inbound ICE Binding Request. The expected username
 * is local_ufrag:remote_ufrag and integrity_key is the local ICE password. */
int mlb_ice_verify_check_request(const uint8_t *packet, size_t packet_length,
                                 const char *expected_username, const char *integrity_key,
                                 MlbIceCheckInfo *info);

/* Construct and validate authenticated Binding success responses. */
int mlb_ice_build_check_success(uint8_t *out, size_t capacity,
                                const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                const char *integrity_key,
                                const MlbStunMappedAddress *mapped, size_t *out_length);
int mlb_ice_verify_check_success(const uint8_t *packet, size_t packet_length,
                                 const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                 const char *integrity_key,
                                 MlbStunMappedAddress *mapped);

/* RFC 8445 487 Role Conflict error response helpers. */
int mlb_ice_build_role_conflict(uint8_t *out, size_t capacity,
                                const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                const char *integrity_key, size_t *out_length);
int mlb_ice_verify_role_conflict(const uint8_t *packet, size_t packet_length,
                                 const uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE],
                                 const char *integrity_key);

#ifdef __cplusplus
}
#endif
#endif
