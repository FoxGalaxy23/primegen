#ifndef MLBROWSER_WEBRTC_TURN_H
#define MLBROWSER_WEBRTC_TURN_H

#include <stddef.h>
#include <stdint.h>
#include "webrtc_stun.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLB_TURN_MAX_DATAGRAM 65507u
#define MLB_TURN_MAX_CHANNELS 16u
#define MLB_TURN_DEFAULT_LIFETIME 600u

typedef struct MlbTurnClient MlbTurnClient;

typedef struct MlbTurnConfig {
    const char *server_host;       /* DNS name or numeric IP address. */
    uint16_t server_port;          /* Usually 3478 for TURN/UDP. */
    const char *username;          /* Printable ASCII for this initial implementation. */
    const char *password;          /* Printable ASCII; credentials use RFC 5766 long-term MD5 key. */
    unsigned timeout_ms;           /* Per transaction, 50..10000; default 1500. */
    uint32_t requested_lifetime;   /* Seconds; 0 selects 600. */
} MlbTurnConfig;

typedef struct MlbTurnAllocationInfo {
    MlbStunMappedAddress relayed_address;
    MlbStunMappedAddress mapped_address;
    uint32_t lifetime;
    int active;
} MlbTurnAllocationInfo;

/* This module implements the TURN-over-UDP client (RFC 5766 / RFC 8656 wire
 * format). It remains an independently-owned transport object, but can be
 * attached to the native ICE Agent as a relay candidate. OpenSSL is required
 * for long-term credential authentication. */
int mlb_turn_available(void);
MlbTurnClient *mlb_turn_client_create(const MlbTurnConfig *config);
void mlb_turn_client_destroy(MlbTurnClient *client);

/* Performs Allocate, including 401 realm/nonce challenge and authenticated
 * retry. Returns 0 only after a verified Allocate success response. */
int mlb_turn_allocate(MlbTurnClient *client);
int mlb_turn_refresh(MlbTurnClient *client, uint32_t lifetime_seconds);
/* Refreshes an allocation before expiry and renews permissions before their
 * 300-second lifetime. Returns 0 while healthy, -1 after loss or refresh failure. */
int mlb_turn_maintain(MlbTurnClient *client);
int mlb_turn_get_allocation(const MlbTurnClient *client, MlbTurnAllocationInfo *info);
/* Exposes the UDP socket for integration into a shared poll()/epoll() loop.
 * Do not read from it directly; call mlb_turn_poll(client, 0) after readiness. */
int mlb_turn_native_fd(const MlbTurnClient *client);

/* Peer addresses must be numeric IPv4/IPv6 literals. Permissions expire after
 * 300 seconds; channel bindings after 600 seconds. Channel numbers are
 * allocated from 0x4000..0x7fff when *channel_number is zero. */
int mlb_turn_create_permission(MlbTurnClient *client, const char *peer_address,
                               uint16_t peer_port);
int mlb_turn_channel_bind(MlbTurnClient *client, const char *peer_address,
                          uint16_t peer_port, uint16_t *channel_number);

/* Sends a datagram through a live allocation. Uses ChannelData for a bound
 * peer and Send Indication otherwise (a live permission is required). */
int mlb_turn_send_to(MlbTurnClient *client, const char *peer_address,
                     uint16_t peer_port, const void *data, size_t length);

/* Processes one UDP packet from the TURN server and queues Data Indication or
 * ChannelData payloads. Returns 1 when a payload is queued, 0 on timeout or
 * irrelevant packet, and -1 on socket/protocol error. */
int mlb_turn_poll(MlbTurnClient *client, unsigned timeout_ms);
int mlb_turn_receive(MlbTurnClient *client, char *peer_address,
                     size_t peer_address_capacity, uint16_t *peer_port,
                     void *data, size_t capacity, size_t *received_length);

#ifdef __cplusplus
}
#endif
#endif
