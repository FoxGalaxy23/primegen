#ifndef MLBROWSER_WEBRTC_SCTP_H
#define MLBROWSER_WEBRTC_SCTP_H

#include <stddef.h>
#include <stdint.h>
#include "webrtc_dtls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLB_SCTP_DEFAULT_PORT 5000u
#define MLB_SCTP_MAX_PACKET 16384u
#define MLB_SCTP_MAX_MESSAGE 16384u
#define MLB_SCTP_MAX_STREAMS 65535u

/* WebRTC Data Channel PPIDs from RFC 8831. Empty application messages are
 * represented by one zero byte with the corresponding Empty PPID. */
#define MLB_SCTP_PPID_DCEP 50u
#define MLB_SCTP_PPID_WEBRTC_STRING 51u
#define MLB_SCTP_PPID_WEBRTC_BINARY 53u
#define MLB_SCTP_PPID_WEBRTC_STRING_EMPTY 56u
#define MLB_SCTP_PPID_WEBRTC_BINARY_EMPTY 57u

enum MlbSctpState {
    MLB_SCTP_STATE_CLOSED = 0,
    MLB_SCTP_STATE_LISTENING,
    MLB_SCTP_STATE_COOKIE_WAIT,
    MLB_SCTP_STATE_COOKIE_ECHOED,
    MLB_SCTP_STATE_ESTABLISHED,
    MLB_SCTP_STATE_FAILED,
    MLB_SCTP_STATE_SHUTDOWN
};

typedef struct MlbSctpAssociation MlbSctpAssociation;

enum MlbSctpReliabilityPolicy {
    MLB_SCTP_RELIABLE = 0,
    MLB_SCTP_RELIABILITY_TIMED = 1,
    MLB_SCTP_RELIABILITY_RTX = 2
};

typedef struct MlbSctpConfig {
    MlbDtlsPeer *dtls_peer;      /* Borrowed; must outlive the association. */
    int active;                  /* Exactly one peer starts active (sends INIT). */
    uint16_t local_port;         /* Zero selects the WebRTC default SCTP port. */
    uint16_t remote_port;        /* Zero selects the WebRTC default SCTP port. */
    uint16_t outbound_streams;  /* Zero selects 65535. */
    uint16_t inbound_streams;   /* Zero selects 65535. */
    int enable_interleaving;     /* Advertise RFC 8260 I-DATA support. */
    unsigned initial_rto_ms;     /* 200..60000; zero selects 1000. */
    unsigned max_retransmits;    /* 1..16; zero selects 8. */
    size_t receive_window_bytes; /* 256..1 MiB; zero selects 1 MiB. */
} MlbSctpConfig;

typedef struct MlbSctpStats {
    enum MlbSctpState state;
    uint32_t local_verification_tag;
    uint32_t remote_verification_tag;
    uint32_t next_tsn;
    uint32_t cumulative_tsn_ack;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t data_chunks_sent;
    uint64_t data_chunks_received;
    uint64_t retransmissions;
    uint64_t application_bytes_sent;
    uint64_t application_bytes_received;
    size_t congestion_window_bytes;
    size_t slow_start_threshold_bytes;
    size_t flight_bytes;
    size_t local_rwnd_bytes;
    size_t peer_rwnd_bytes;
    unsigned rto_ms;
    unsigned srtt_ms;
    unsigned rttvar_ms;
    uint64_t rtt_samples;
    uint64_t zero_window_probes;
    unsigned outstanding_chunks;
    unsigned receive_reorder_chunks;
    uint64_t out_of_order_data_chunks_received;
    uint64_t duplicate_data_chunks_received;
    uint64_t gap_sacks_sent;
    uint64_t gap_sacks_received;
    uint64_t fast_retransmits;
    uint64_t reneged_data_chunks_detected;
    uint64_t i_data_chunks_sent;
    uint64_t i_data_chunks_received;
    unsigned active_rx_messages;
    int interleaving_negotiated;
    int partial_reliability_negotiated;
    int i_forward_tsn_negotiated;
    uint64_t pr_messages_abandoned;
    uint64_t forward_tsn_sent;
    uint64_t forward_tsn_received;
} MlbSctpStats;

enum MlbSctpResetDirection {
    MLB_SCTP_RESET_INCOMING = 1,
    MLB_SCTP_RESET_OUTGOING = 2
};

int mlb_sctp_available(void);
MlbSctpAssociation *mlb_sctp_association_create(const MlbSctpConfig *config);
void mlb_sctp_association_destroy(MlbSctpAssociation *association);

/* Start the association after DTLS is CONNECTED. The active side sends INIT;
 * the passive side waits for INIT. Exactly one side must be active. */
int mlb_sctp_association_start(MlbSctpAssociation *association);

/* Drive DTLS input, SCTP state transitions, SACK processing and T1/T3-style
 * retransmission timers. timeout_ms is clamped to 1000 ms. */
int mlb_sctp_association_poll(MlbSctpAssociation *association, unsigned timeout_ms);
int mlb_sctp_association_state(const MlbSctpAssociation *association);
/* Bytes in the currently active reliable message on stream_id, or zero. */
size_t mlb_sctp_association_buffered_bytes(const MlbSctpAssociation *association,
                                           uint16_t stream_id);
int mlb_sctp_association_has_pending_data(const MlbSctpAssociation *association,
                                           uint16_t stream_id);
/* Configure the outgoing stream's RFC 8260 weighted-fair-queueing weight.
 * Zero is invalid; WebRTC DataChannel priorities map directly to weights. */
int mlb_sctp_association_set_stream_weight(MlbSctpAssociation *association,
                                           uint16_t stream_id, uint16_t weight);
uint16_t mlb_sctp_association_stream_limit(const MlbSctpAssociation *association);
uint16_t mlb_sctp_association_inbound_stream_limit(const MlbSctpAssociation *association);

/* Reliable DATA/I-DATA is transmitted using a bounded congestion window (up
 * to eight DTLS records in flight). If RFC 8260 is negotiated, I-DATA chunks
 * are used exclusively and messages on different streams may be interleaved.
 * The receiver buffers a bounded number of out-of-order TSNs and concurrently
 * reassembles a bounded number of interleaved messages. The user-message limit
 * is 16 KiB.
 * Empty DataChannel messages use the RFC 8831 one-zero-byte PPID markers.
 * Returns 1 on backpressure, 0 when accepted by the DTLS FIFO, -1 on error. */
int mlb_sctp_association_send(MlbSctpAssociation *association,
                              uint16_t stream_id, uint32_t ppid,
                              int unordered, const void *data, size_t length);
/* Queues a user message with RFC 3758/RFC 7496 partial reliability. TIMED
 * expires max_value milliseconds after enqueue; RTX permits max_value
 * retransmissions per DATA/I-DATA fragment. A value of zero is valid.
 * The association must have negotiated Forward-TSN (and I-FORWARD-TSN when
 * I-DATA is in use). */
int mlb_sctp_association_send_pr(MlbSctpAssociation *association,
                                 uint16_t stream_id, uint32_t ppid,
                                 int unordered, const void *data, size_t length,
                                 enum MlbSctpReliabilityPolicy policy,
                                 uint32_t max_value);
int mlb_sctp_association_partial_reliability_available(
                                 const MlbSctpAssociation *association);
#ifdef MLB_SCTP_TESTING
/* Test-only fault injection: abandons the first assigned PR message on a
 * stream so Forward-TSN receive/reassembly can be exercised deterministically. */
int mlb_sctp_association_test_abandon_pr_message(MlbSctpAssociation *association,
                                                  uint16_t stream_id);
#endif

/* Receives one complete DATA user message. Returns 0 on success, -1 if empty
 * or the supplied buffer is too small. */
int mlb_sctp_association_receive(MlbSctpAssociation *association,
                                 uint16_t *stream_id, uint32_t *ppid,
                                 int *unordered, void *data, size_t capacity,
                                 size_t *received_length);
int mlb_sctp_association_get_stats(const MlbSctpAssociation *association,
                                   MlbSctpStats *stats);
int mlb_sctp_association_last_error(const MlbSctpAssociation *association,
                                    char *out, size_t capacity);

/* RFC 6525 stream reset. A request is accepted and sent once the stream has
 * no outstanding DATA. Returns 0 when queued, 1 when a previous reset is
 * pending, and -1 when the extension was not negotiated or input is invalid. */
int mlb_sctp_association_request_stream_reset(MlbSctpAssociation *association,
                                              uint16_t stream_id);
/* Pop one completed incoming/outgoing reset notification: 1 event, 0 empty. */
int mlb_sctp_association_next_reset_event(MlbSctpAssociation *association,
                                          uint16_t *stream_id,
                                          int *direction, int *result);

/* Graceful association shutdown is not implemented; abort drops local state. */
void mlb_sctp_association_abort(MlbSctpAssociation *association);

/* Exposed for deterministic protocol unit tests. */
uint32_t mlb_sctp_crc32c(const void *data, size_t length);

#ifdef __cplusplus
}
#endif
#endif
