#ifndef MLBROWSER_WEBRTC_DTLS_H
#define MLBROWSER_WEBRTC_DTLS_H

#include <stddef.h>
#include <stdint.h>
#include "webrtc_ice_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLB_DTLS_MAX_DATAGRAM 16384u
#define MLB_DTLS_DEFAULT_MTU 1200u

enum MlbDtlsRole {
    MLB_DTLS_ROLE_CLIENT = 0,
    MLB_DTLS_ROLE_SERVER = 1
};

enum MlbDtlsState {
    MLB_DTLS_STATE_NEW = 0,
    MLB_DTLS_STATE_HANDSHAKING,
    MLB_DTLS_STATE_CONNECTED,
    MLB_DTLS_STATE_FAILED,
    MLB_DTLS_STATE_CLOSED
};

typedef struct MlbDtlsPeer MlbDtlsPeer;

typedef struct MlbDtlsConfig {
    MlbIceAgent *ice_agent;          /* Borrowed; must outlive the DTLS peer. */
    enum MlbDtlsRole role;           /* Derived from SDP setup: active/client or passive/server. */
    const char *remote_fingerprint;  /* Optional at creation; set before start from authenticated SDP. */
    unsigned mtu;                    /* 576..1200; zero selects 1200. */
    unsigned handshake_timeout_ms;   /* 1000..120000; zero selects 30000. */
} MlbDtlsConfig;

typedef struct MlbDtlsStats {
    enum MlbDtlsState state;
    uint64_t handshake_started_ms;
    uint64_t handshake_completed_ms;
    uint64_t handshake_retransmissions;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t application_bytes_sent;
    uint64_t application_bytes_received;
    uint64_t pending_outbound_messages;
    uint64_t pending_outbound_bytes;
} MlbDtlsStats;

/* DTLS 1.2 over the nominated ICE datagram path. The implementation uses
 * OpenSSL for DTLS and cryptographic primitives, not a WebRTC stack. It pins
 * the peer's self-signed certificate to the mandatory SDP SHA-256 fingerprint.
 * All methods must be called from one event-loop thread per peer. */
int mlb_dtls_available(void);
MlbDtlsPeer *mlb_dtls_peer_create(const MlbDtlsConfig *config);
void mlb_dtls_peer_destroy(MlbDtlsPeer *peer);

/* Returns local certificate fingerprint in SDP form: "sha-256 XX:...". */
int mlb_dtls_peer_local_fingerprint(const MlbDtlsPeer *peer, char *out, size_t capacity);

/* Pins the remote self-signed certificate using the SDP SHA-256 fingerprint.
 * May be called once or replaced while NEW; never accepted after start. */
int mlb_dtls_peer_set_remote_fingerprint(MlbDtlsPeer *peer, const char *fingerprint);
/* Selects the DTLS role before handshake after SDP setup negotiation. */
int mlb_dtls_peer_set_role(MlbDtlsPeer *peer, enum MlbDtlsRole role);

/* Starts the DTLS handshake. The ICE Agent must already be CONNECTED and a
 * remote fingerprint must have been pinned. */
int mlb_dtls_peer_start(MlbDtlsPeer *peer);

/* Drives ICE receive, DTLS handshake retransmission timers, and decrypted
 * application-data delivery. timeout_ms is clamped to 1000 ms. Returns 1 on
 * state/progress change, 0 on no progress, and -1 on terminal error. */
int mlb_dtls_peer_poll(MlbDtlsPeer *peer, unsigned timeout_ms);
int mlb_dtls_peer_state(const MlbDtlsPeer *peer);

/* Returns the maximum plaintext size of one current DTLS record. Larger
 * payloads must be fragmented by the upper protocol (e.g. SCTP). */
size_t mlb_dtls_peer_max_message_size(const MlbDtlsPeer *peer);

/* Queues one DTLS application-data message in a bounded FIFO. Returns 0 when
 * accepted (it may be retried internally after WANT_READ/WANT_WRITE), and -1
 * on invalid input, queue exhaustion, or fatal transport/crypto error. poll()
 * flushes queued writes in order. */
int mlb_dtls_peer_send(MlbDtlsPeer *peer, const void *data, size_t length);
/* Receives one decrypted DTLS application-data message. Returns 0 on success,
 * -1 if no message is queued or the caller's buffer is too small. */
int mlb_dtls_peer_receive(MlbDtlsPeer *peer, void *data, size_t capacity,
                          size_t *received_length);

int mlb_dtls_peer_get_stats(const MlbDtlsPeer *peer, MlbDtlsStats *stats);
int mlb_dtls_peer_cipher(const MlbDtlsPeer *peer, char *out, size_t capacity);
int mlb_dtls_peer_last_error(const MlbDtlsPeer *peer, char *out, size_t capacity);
/* Sends close_notify once and transitions to CLOSED. */
int mlb_dtls_peer_shutdown(MlbDtlsPeer *peer);

#ifdef __cplusplus
}
#endif
#endif
