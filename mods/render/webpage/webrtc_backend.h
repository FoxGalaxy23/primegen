#ifndef MLBROWSER_WEBRTC_BACKEND_H
#define MLBROWSER_WEBRTC_BACKEND_H
#include <stddef.h>
#include <stdint.h>

typedef struct MlbWebRtcPeer MlbWebRtcPeer;
typedef struct MlbWebRtcEvent MlbWebRtcEvent;

enum MlbWebRtcEventKind {
    MLB_RTC_EVENT_DESCRIPTION = 1,
    MLB_RTC_EVENT_CANDIDATE,
    MLB_RTC_EVENT_CONNECTION_STATE,
    MLB_RTC_EVENT_ICE_STATE,
    MLB_RTC_EVENT_GATHERING_STATE,
    MLB_RTC_EVENT_SIGNALING_STATE,
    MLB_RTC_EVENT_DATA_CHANNEL,
    MLB_RTC_EVENT_CHANNEL_OPEN,
    MLB_RTC_EVENT_CHANNEL_CLOSED,
    MLB_RTC_EVENT_CHANNEL_ERROR,
    MLB_RTC_EVENT_MESSAGE
};

struct MlbWebRtcEvent {
    int kind;
    int id;                 /* PeerConnection ID or DataChannel ID, depending on kind. */
    int state;
    int binary;
    int ordered;
    uint16_t priority;
    int has_max_packet_lifetime;
    unsigned max_packet_lifetime;
    int has_max_retransmits;
    unsigned max_retransmits;
    char *text;             /* SDP, candidate, error, label, or message payload. */
    char *mid;              /* SDP type, candidate MID, or data-channel protocol. */
    size_t length;          /* Payload byte length; text can contain embedded NUL for binary messages. */
    MlbWebRtcEvent *next;
};

int mlb_webrtc_available(void);
MlbWebRtcPeer *mlb_webrtc_peer_create(const char *ice_servers_newline_separated, int relay_only);
int mlb_webrtc_peer_id(const MlbWebRtcPeer *peer);
MlbWebRtcPeer *mlb_webrtc_peer_next(MlbWebRtcPeer *peer);
int mlb_webrtc_set_local_description(MlbWebRtcPeer *peer, const char *type);
int mlb_webrtc_set_remote_description(MlbWebRtcPeer *peer, const char *sdp, const char *type);
int mlb_webrtc_add_candidate(MlbWebRtcPeer *peer, const char *candidate, const char *mid);
int mlb_webrtc_create_data_channel(MlbWebRtcPeer *peer, const char *label, const char *protocol,
                                   int ordered, int negotiated, int id,
                                   int has_max_packet_lifetime, unsigned max_packet_lifetime,
                                   int has_max_retransmits, unsigned max_retransmits,
                                   unsigned priority);
int mlb_webrtc_channel_send(MlbWebRtcPeer *peer, int channel_id, const void *data, size_t length, int binary);
int mlb_webrtc_channel_close(MlbWebRtcPeer *peer, int channel_id);
int mlb_webrtc_channel_buffered_amount(MlbWebRtcPeer *peer, int channel_id);
int mlb_webrtc_channel_stream(MlbWebRtcPeer *peer, int channel_id);
int mlb_webrtc_channel_is_open(MlbWebRtcPeer *peer, int channel_id);
MlbWebRtcEvent *mlb_webrtc_next_event(MlbWebRtcPeer *peer);
void mlb_webrtc_event_free(MlbWebRtcEvent *event);
void mlb_webrtc_peer_destroy(MlbWebRtcPeer *peer);

#endif
