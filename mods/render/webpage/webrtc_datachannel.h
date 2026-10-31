#ifndef MLBROWSER_WEBRTC_DATACHANNEL_H
#define MLBROWSER_WEBRTC_DATACHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include "webrtc_sctp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLB_DC_MAX_CHANNELS 128u
#define MLB_DC_MAX_LABEL_BYTES 1024u
#define MLB_DC_MAX_PROTOCOL_BYTES 1024u
#define MLB_DC_MAX_BUFFERED_BYTES (1024u * 1024u)

enum MlbDataChannelRole {
    MLB_DC_ROLE_DTLS_CLIENT = 0,
    MLB_DC_ROLE_DTLS_SERVER = 1
};

enum MlbDataChannelState {
    MLB_DC_STATE_CONNECTING = 0,
    MLB_DC_STATE_OPEN,
    MLB_DC_STATE_CLOSING,
    MLB_DC_STATE_CLOSED,
    MLB_DC_STATE_FAILED
};

enum MlbDataChannelEventKind {
    MLB_DC_EVENT_DATA_CHANNEL = 1,
    MLB_DC_EVENT_OPEN,
    MLB_DC_EVENT_MESSAGE,
    MLB_DC_EVENT_ERROR,
    MLB_DC_EVENT_CLOSE
};

typedef struct MlbDataChannelManager MlbDataChannelManager;
typedef struct MlbDataChannelEvent MlbDataChannelEvent;

typedef struct MlbDataChannelConfig {
    MlbSctpAssociation *association; /* Borrowed; must outlive this manager. */
    enum MlbDataChannelRole role;    /* Determines even/odd stream-ID allocation. */
    uint16_t max_channels;           /* Zero selects MLB_DC_MAX_CHANNELS. */
    size_t max_buffered_bytes;       /* Zero selects MLB_DC_MAX_BUFFERED_BYTES. */
} MlbDataChannelConfig;

struct MlbDataChannelEvent {
    enum MlbDataChannelEventKind kind;
    uint16_t id;                     /* SCTP stream ID / DataChannel ID. */
    int binary;
    int ordered;
    uint16_t priority;               /* DCEP priority / RFC 8260 WFQ weight. */
    int has_max_packet_lifetime;
    unsigned max_packet_lifetime;
    int has_max_retransmits;
    unsigned max_retransmits;
    int remote;
    char *label;
    char *protocol;
    uint8_t *data;
    size_t length;
    MlbDataChannelEvent *next;
};

int mlb_datachannel_available(void);
MlbDataChannelManager *mlb_datachannel_manager_create(const MlbDataChannelConfig *config);
void mlb_datachannel_manager_destroy(MlbDataChannelManager *manager);

/* Creates a channel. In-band channels use DCEP OPEN/ACK. Negotiated channels
 * skip DCEP and must be created with the same ID on both peers. Stream IDs
 * follow WebRTC parity: DTLS client even, DTLS server odd. Partial-reliability
 * options require negotiated PR-SCTP/Forward-TSN.
 * Returns the assigned channel ID, or -1 on error. */
int mlb_datachannel_create(MlbDataChannelManager *manager,
                           const char *label, const char *protocol,
                           int ordered, int negotiated, int requested_id,
                           int has_max_packet_lifetime, unsigned max_packet_lifetime,
                           int has_max_retransmits, unsigned max_retransmits,
                           unsigned priority);

/* Queues a string or binary application message. Returns 0 when accepted,
 * 1 when bounded queue backpressure prevents acceptance, -1 on error. */
int mlb_datachannel_send(MlbDataChannelManager *manager, uint16_t channel_id,
                         const void *data, size_t length, int binary);

/* Drives SCTP and DCEP, drains received DATA, completes OPEN handshakes and
 * flushes queued messages. timeout_ms is forwarded to the SCTP poller. */
int mlb_datachannel_poll(MlbDataChannelManager *manager, unsigned timeout_ms);
int mlb_datachannel_state(const MlbDataChannelManager *manager, uint16_t channel_id);
size_t mlb_datachannel_buffered_amount(const MlbDataChannelManager *manager,
                                       uint16_t channel_id);
int mlb_datachannel_id_is_open(const MlbDataChannelManager *manager,
                               uint16_t channel_id);

/* Starts WebRTC DataChannel closing. The channel becomes CLOSED only after
 * the reciprocal incoming/outgoing SCTP stream resets complete (RFC 8831/6525).
 * Repeated calls while CLOSING/CLOSED are idempotent. */
int mlb_datachannel_close(MlbDataChannelManager *manager, uint16_t channel_id);

MlbDataChannelEvent *mlb_datachannel_next_event(MlbDataChannelManager *manager);
void mlb_datachannel_event_free(MlbDataChannelEvent *event);
const char *mlb_datachannel_last_error(const MlbDataChannelManager *manager);

#ifdef __cplusplus
}
#endif
#endif
