#define _POSIX_C_SOURCE 200809L
#include "webrtc_datachannel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAVE_OPENSSL_VAULT
struct MlbDataChannelManager { int unavailable; };
int mlb_datachannel_available(void) { return 0; }
MlbDataChannelManager *mlb_datachannel_manager_create(const MlbDataChannelConfig *c) { (void)c; return NULL; }
void mlb_datachannel_manager_destroy(MlbDataChannelManager *m) { free(m); }
int mlb_datachannel_create(MlbDataChannelManager *m,const char*l,const char*p,int o,int n,int id,int hl,unsigned ml,int hr,unsigned mr,unsigned pr) { (void)m;(void)l;(void)p;(void)o;(void)n;(void)id;(void)hl;(void)ml;(void)hr;(void)mr;(void)pr;return -1; }
int mlb_datachannel_send(MlbDataChannelManager *m,uint16_t id,const void*d,size_t n,int b) { (void)m;(void)id;(void)d;(void)n;(void)b;return -1; }
int mlb_datachannel_poll(MlbDataChannelManager *m,unsigned t) { (void)m;(void)t;return -1; }
int mlb_datachannel_state(const MlbDataChannelManager *m,uint16_t id) { (void)m;(void)id;return MLB_DC_STATE_FAILED; }
size_t mlb_datachannel_buffered_amount(const MlbDataChannelManager *m,uint16_t id) { (void)m;(void)id;return 0; }
int mlb_datachannel_id_is_open(const MlbDataChannelManager *m,uint16_t id) { (void)m;(void)id;return 0; }
int mlb_datachannel_close(MlbDataChannelManager *m,uint16_t id) { (void)m;(void)id;return -1; }
MlbDataChannelEvent *mlb_datachannel_next_event(MlbDataChannelManager *m) { (void)m;return NULL; }
void mlb_datachannel_event_free(MlbDataChannelEvent *e) { if(e){free(e->label);free(e->protocol);free(e->data);free(e);} }
const char *mlb_datachannel_last_error(const MlbDataChannelManager *m) { (void)m;return "WebRTC DataChannels require the OpenSSL provider"; }
#else
#include <openssl/crypto.h>

#define DCEP_OPEN 0x03u
#define DCEP_ACK  0x02u
#define DCEP_RELIABLE 0x00u
#define DCEP_RELIABLE_UNORDERED 0x80u
#define DCEP_PARTIAL_RELIABLE_REXMIT 0x01u
#define DCEP_PARTIAL_RELIABLE_TIMED 0x02u
#define DC_SEND_QUEUE_MAX 256u
#define DC_RX_BURST 64u
#define DC_EVENT_QUEUE_MAX 256u
#define DC_EVENT_BYTES_MAX (1024u * 1024u)
#define DC_PENDING_IN_MAX (256u * 1024u)

typedef struct DcPendingInbound {
    struct DcPendingInbound *next;
    int binary;
    size_t length;
    uint8_t data[];
} DcPendingInbound;

typedef struct DcChannel {
    int used;
    int remote;
    int negotiated;
    int ordered;
    int has_max_packet_lifetime;
    unsigned max_packet_lifetime;
    int has_max_retransmits;
    unsigned max_retransmits;
    int state;
    int open_pending;
    int ack_pending;
    int close_requested;
    int reset_request_started;
    int incoming_reset_done;
    int outgoing_reset_done;
    int close_event_queued;
    uint16_t id;
    uint16_t priority;
    char *label;
    char *protocol;
    size_t buffered;
    DcPendingInbound *pending_rx_head, *pending_rx_tail;
    size_t pending_rx_bytes;
} DcChannel;

typedef struct DcQueuedMessage {
    struct DcQueuedMessage *next;
    uint16_t id;
    uint32_t ppid;
    int unordered;
    size_t length;
    size_t application_length;
    uint8_t data[];
} DcQueuedMessage;

struct MlbDataChannelManager {
    MlbSctpAssociation *association;
    enum MlbDataChannelRole role;
    uint16_t stream_limit;
    uint16_t inbound_stream_limit;
    uint16_t next_id;
    uint16_t max_channels;
    size_t max_buffered_bytes;
    size_t buffered_bytes;
    size_t event_bytes;
    unsigned event_count;
    size_t pending_rx_bytes;
    DcChannel channels[MLB_DC_MAX_CHANNELS];
    unsigned channel_count;
    DcQueuedMessage *send_head, *send_tail;
    unsigned send_count;
    MlbDataChannelEvent *event_head, *event_tail;
    char error[192];
};

static void dc_set_error(MlbDataChannelManager *m, const char *s) {
    if (m && s) snprintf(m->error, sizeof m->error, "%s", s);
}

static int utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t cp; unsigned need; uint8_t c = s[i++];
        if (c < 0x80u) continue;
        if (c >= 0xc2u && c <= 0xdfu) { cp = c & 0x1fu; need = 1u; }
        else if (c >= 0xe0u && c <= 0xefu) { cp = c & 0x0fu; need = 2u; }
        else if (c >= 0xf0u && c <= 0xf4u) { cp = c & 0x07u; need = 3u; }
        else return 0;
        if (need > n - i) return 0;
        for (unsigned j = 0; j < need; j++) {
            uint8_t d = s[i++]; if ((d & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (d & 0x3fu);
        }
        if ((need == 1u && cp < 0x80u) || (need == 2u && cp < 0x800u) ||
            (need == 3u && cp < 0x10000u) || cp > 0x10ffffu ||
            (cp >= 0xd800u && cp <= 0xdfffu)) return 0;
    }
    return 1;
}
static uint16_t dc_get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t dc_get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void dc_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void dc_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static DcChannel *find_channel(MlbDataChannelManager *m, uint16_t id) {
    if (!m) return NULL;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++)
        if (m->channels[i].used && m->channels[i].id == id) return &m->channels[i];
    return NULL;
}
static DcChannel *alloc_channel(MlbDataChannelManager *m) {
    if (!m || m->channel_count >= m->max_channels) return NULL;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++) if (!m->channels[i].used) {
        memset(&m->channels[i], 0, sizeof m->channels[i]);
        m->channels[i].used = 1; m->channels[i].state = MLB_DC_STATE_CONNECTING;
        m->channel_count++; return &m->channels[i];
    }
    return NULL;
}
static char *dc_dup(const char *s) {
    size_t n = s ? strlen(s) : 0u;
    char *p = (char *)malloc(n + 1u);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}
static int queue_event(MlbDataChannelManager *m, enum MlbDataChannelEventKind kind,
                       const DcChannel *c, const void *data, size_t length, int binary) {
    MlbDataChannelEvent *e;
    if (!m || m->event_count >= DC_EVENT_QUEUE_MAX ||
        length > DC_EVENT_BYTES_MAX - (m->event_bytes > DC_EVENT_BYTES_MAX ? DC_EVENT_BYTES_MAX : m->event_bytes)) {
        if (m) dc_set_error(m, "DataChannel event queue limit reached");
        return -1;
    }
    e = (MlbDataChannelEvent *)calloc(1, sizeof *e);
    if (!e) return -1;
    e->kind = kind; e->id = c ? c->id : 0; e->binary = binary; e->ordered = c ? c->ordered : 1;
    e->priority = c ? c->priority : 256u; e->remote = c ? c->remote : 0;
    if (c) {
        e->has_max_packet_lifetime = c->has_max_packet_lifetime;
        e->max_packet_lifetime = c->max_packet_lifetime;
        e->has_max_retransmits = c->has_max_retransmits;
        e->max_retransmits = c->max_retransmits;
    }
    if (c && c->label && !(e->label = dc_dup(c->label))) goto fail;
    if (c && c->protocol && !(e->protocol = dc_dup(c->protocol))) goto fail;
    if (length) {
        e->data = (uint8_t *)malloc(length);
        if (!e->data) goto fail;
        memcpy(e->data, data, length); e->length = length;
    }
    if (m->event_tail) m->event_tail->next = e; else m->event_head = e;
    m->event_tail = e; m->event_count++; m->event_bytes += length; return 0;
fail:
    mlb_datachannel_event_free(e); return -1;
}
static void free_send_queue(MlbDataChannelManager *m) {
    DcQueuedMessage *q = m->send_head;
    while (q) { DcQueuedMessage *next = q->next; free(q); q = next; }
    m->send_head = m->send_tail = NULL; m->send_count = 0;
}
static void free_pending_rx(DcChannel *c) {
    DcPendingInbound *p = c ? c->pending_rx_head : NULL;
    while (p) { DcPendingInbound *next = p->next; free(p); p = next; }
    if (c) { c->pending_rx_head = c->pending_rx_tail = NULL; c->pending_rx_bytes = 0; }
}
static int queue_pending_rx(MlbDataChannelManager *m, DcChannel *c,
                            const void *data, size_t length, int binary) {
    DcPendingInbound *p;
    if (!m || !c || length > DC_PENDING_IN_MAX -
        (m->pending_rx_bytes > DC_PENDING_IN_MAX ? DC_PENDING_IN_MAX : m->pending_rx_bytes)) {
        if (m) dc_set_error(m, "pre-open DataChannel receive queue limit reached");
        return -1;
    }
    p = (DcPendingInbound *)malloc(sizeof *p + length);
    if (!p) return -1;
    memset(p, 0, sizeof *p); p->binary = binary; p->length = length;
    if (length) memcpy(p->data, data, length);
    if (c->pending_rx_tail) c->pending_rx_tail->next = p; else c->pending_rx_head = p;
    c->pending_rx_tail = p; c->pending_rx_bytes += length; m->pending_rx_bytes += length;
    return 0;
}
static int flush_pending_rx(MlbDataChannelManager *m, DcChannel *c) {
    DcPendingInbound *p;
    while (c && (p = c->pending_rx_head) != NULL) {
        if (queue_event(m, MLB_DC_EVENT_MESSAGE, c, p->data, p->length, p->binary) != 0) return -1;
        c->pending_rx_head = p->next; if (!c->pending_rx_head) c->pending_rx_tail = NULL;
        c->pending_rx_bytes -= p->length;
        m->pending_rx_bytes = m->pending_rx_bytes >= p->length ? m->pending_rx_bytes - p->length : 0u;
        free(p);
    }
    return 0;
}

static int send_dcep(MlbDataChannelManager *m, DcChannel *c, int ack) {
    uint8_t packet[12u + MLB_DC_MAX_LABEL_BYTES + MLB_DC_MAX_PROTOCOL_BYTES];
    size_t label_len, protocol_len, length;
    int rc;
    if (!m || !c) return -1;
    if (ack) {
        packet[0] = DCEP_ACK;
        rc = mlb_sctp_association_send(m->association, c->id, MLB_SCTP_PPID_DCEP,
                                       0, packet, 1u);
        return rc;
    }
    label_len = strlen(c->label); protocol_len = strlen(c->protocol);
    length = 12u + label_len + protocol_len;
    packet[0] = DCEP_OPEN;
    packet[1] = (uint8_t)(c->ordered ? 0u : 0x80u);
    if (c->has_max_retransmits) packet[1] |= DCEP_PARTIAL_RELIABLE_REXMIT;
    else if (c->has_max_packet_lifetime) packet[1] |= DCEP_PARTIAL_RELIABLE_TIMED;
    dc_put16(packet + 2u, c->priority ? c->priority : 256u);
    dc_put32(packet + 4u, c->has_max_retransmits ? c->max_retransmits :
                     (c->has_max_packet_lifetime ? c->max_packet_lifetime : 0u));
    dc_put16(packet + 8u, (uint16_t)label_len);
    dc_put16(packet + 10u, (uint16_t)protocol_len);
    memcpy(packet + 12u, c->label, label_len);
    memcpy(packet + 12u + label_len, c->protocol, protocol_len);
    rc = mlb_sctp_association_send(m->association, c->id, MLB_SCTP_PPID_DCEP,
                                   0, packet, length);
    return rc;
}

static int parse_dcep_open(const uint8_t *data, size_t length,
                           int *ordered, uint16_t *priority,
                           int *has_lifetime, unsigned *lifetime,
                           int *has_retransmits, unsigned *retransmits,
                           const char **label, size_t *label_len,
                           const char **protocol, size_t *protocol_len) {
    uint8_t channel_type, reliability_type;
    uint32_t reliability;
    uint16_t ll, pl;
    if (!data || length < 12u || data[0] != DCEP_OPEN || !ordered || !priority ||
        !has_lifetime || !lifetime || !has_retransmits || !retransmits ||
        !label || !label_len || !protocol || !protocol_len) return -1;
    channel_type = data[1];
    reliability_type = channel_type & 0x7fu;
    if (reliability_type != DCEP_RELIABLE &&
        reliability_type != DCEP_PARTIAL_RELIABLE_REXMIT &&
        reliability_type != DCEP_PARTIAL_RELIABLE_TIMED) return -1;
    reliability = dc_get32(data + 4u);
    *has_lifetime = 0; *lifetime = 0u;
    *has_retransmits = 0; *retransmits = 0u;
    if (reliability_type == DCEP_PARTIAL_RELIABLE_REXMIT) {
        if (reliability > 65535u) return -1;
        *has_retransmits = 1; *retransmits = (unsigned)reliability;
    } else if (reliability_type == DCEP_PARTIAL_RELIABLE_TIMED) {
        if (reliability > 65535u) return -1;
        *has_lifetime = 1; *lifetime = (unsigned)reliability;
    }
    /* The Reliability Parameter is ignored for reliable channel types. */
    ll = dc_get16(data + 8u); pl = dc_get16(data + 10u);
    if (ll > MLB_DC_MAX_LABEL_BYTES || pl > MLB_DC_MAX_PROTOCOL_BYTES ||
        12u + (size_t)ll + (size_t)pl != length) return -1;
    if (!utf8_valid(data + 12u, ll) || !utf8_valid(data + 12u + ll, pl)) return -1;
    *ordered = (channel_type & 0x80u) == 0u;
    *priority = dc_get16(data + 2u);
    *label = (const char *)(data + 12u); *label_len = ll;
    *protocol = (const char *)(data + 12u + ll); *protocol_len = pl;
    return 0;
}

static int process_dcep(MlbDataChannelManager *m, uint16_t stream,
                        int unordered, const uint8_t *data, size_t length) {
    DcChannel *c = find_channel(m, stream);
    if (unordered) { dc_set_error(m, "DCEP control messages must be SCTP ordered"); return -1; }
    if (length == 1u && data[0] == DCEP_ACK) {
        if (!c || c->remote || c->negotiated || c->state != MLB_DC_STATE_CONNECTING || c->open_pending) {
            dc_set_error(m, "unexpected or duplicate DCEP ACK"); return -1;
        }
        c->state = MLB_DC_STATE_OPEN;
        return queue_event(m, MLB_DC_EVENT_OPEN, c, NULL, 0, 0);
    }
    {
        int ordered = 1, parsed, has_lifetime = 0, has_retransmits = 0;
        unsigned lifetime = 0u, retransmits = 0u;
        uint16_t priority = 0;
        const char *label = NULL, *protocol = NULL;
        size_t ll = 0, pl = 0;
        int remote_parity = (m->role == MLB_DC_ROLE_DTLS_CLIENT) ? 1 : 0;
        parsed = parse_dcep_open(data, length, &ordered, &priority,
                                 &has_lifetime, &lifetime,
                                 &has_retransmits, &retransmits,
                                 &label, &ll, &protocol, &pl);
        if (parsed == 0 && (has_lifetime || has_retransmits) &&
            !mlb_sctp_association_partial_reliability_available(m->association)) {
            dc_set_error(m, "partial-reliability DCEP channel was not negotiated at SCTP level");
            return -1;
        }
        if (parsed != 0 || (stream & 1u) != (unsigned)remote_parity ||
            stream >= m->inbound_stream_limit || stream >= m->stream_limit) {
            dc_set_error(m, "malformed DCEP OPEN or invalid remote stream ID"); return -1;
        }
        if (c) {
            if (!c->remote || c->negotiated || !c->label || !c->protocol ||
                strlen(c->label) != ll || memcmp(c->label, label, ll) != 0 ||
                strlen(c->protocol) != pl || memcmp(c->protocol, protocol, pl) != 0 ||
                c->ordered != ordered || c->has_max_packet_lifetime != has_lifetime ||
                c->max_packet_lifetime != lifetime || c->has_max_retransmits != has_retransmits ||
                c->max_retransmits != retransmits) {
                dc_set_error(m, "conflicting DCEP OPEN for an existing stream"); return -1;
            }
            c->ack_pending = 1;
            return 0; /* Duplicate OPEN: send another ACK, without another open event. */
        }
        c = alloc_channel(m);
        if (!c) { dc_set_error(m, "DataChannel limit reached"); return -1; }
        c->id = stream; c->remote = 1; c->ordered = ordered;
        c->has_max_packet_lifetime = has_lifetime; c->max_packet_lifetime = lifetime;
        c->has_max_retransmits = has_retransmits; c->max_retransmits = retransmits;
        c->priority = priority ? priority : 1u;
        if (mlb_sctp_association_set_stream_weight(m->association, stream, c->priority) != 0) {
            memset(c, 0, sizeof *c); m->channel_count--;
            dc_set_error(m, "could not apply remote DCEP priority to SCTP scheduler"); return -1;
        }
        c->label = (char *)malloc(ll + 1u); c->protocol = (char *)malloc(pl + 1u);
        if (!c->label || !c->protocol) { free(c->label); free(c->protocol); memset(c, 0, sizeof *c); m->channel_count--; dc_set_error(m, "out of memory parsing DCEP OPEN"); return -1; }
        if (ll) memcpy(c->label, label, ll);
        c->label[ll] = '\0';
        if (pl) memcpy(c->protocol, protocol, pl);
        c->protocol[pl] = '\0';
        c->ack_pending = 1;
        if (queue_event(m, MLB_DC_EVENT_DATA_CHANNEL, c, NULL, 0, 0) != 0) {
            dc_set_error(m, "out of memory queuing datachannel event"); return -1;
        }
        return 1;
    }
}

static int process_application_message(MlbDataChannelManager *m, uint16_t stream,
                                       uint32_t ppid, int unordered,
                                       const uint8_t *data, size_t length) {
    DcChannel *c = find_channel(m, stream);
    int binary = 0; size_t out_len = length; const uint8_t *out = data;
    int preopen = c && c->remote && c->state == MLB_DC_STATE_CONNECTING && c->ack_pending;
    if (!c || (c->state != MLB_DC_STATE_OPEN && !preopen)) return 0;
    if (preopen ? unordered : (unordered != !c->ordered)) {
        dc_set_error(m, "SCTP delivery ordering does not match DataChannel negotiation state"); return -1;
    }
    switch (ppid) {
        case MLB_SCTP_PPID_WEBRTC_STRING: binary = 0; break;
        case MLB_SCTP_PPID_WEBRTC_BINARY: binary = 1; break;
        case MLB_SCTP_PPID_WEBRTC_STRING_EMPTY:
            if (length != 1u || data[0] != 0u) return -1;
            out_len = 0; out = NULL; binary = 0; break;
        case MLB_SCTP_PPID_WEBRTC_BINARY_EMPTY:
            if (length != 1u || data[0] != 0u) return -1;
            out_len = 0; out = NULL; binary = 1; break;
        default: return 0;
    }
    if (!binary && !utf8_valid(out, out_len)) {
        dc_set_error(m, "received DataChannel string is not valid UTF-8"); return -1;
    }
    if (preopen) return queue_pending_rx(m, c, out, out_len, binary);
    return queue_event(m, MLB_DC_EVENT_MESSAGE, c, out, out_len, binary);
}

static int flush_control(MlbDataChannelManager *m) {
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++) {
        DcChannel *c = &m->channels[i]; int rc;
        if (!c->used) continue;
        if (c->ack_pending) {
            rc = send_dcep(m, c, 1);
            if (rc == 1) return 0;
            if (rc < 0) { dc_set_error(m, "unable to queue DCEP ACK"); return -1; }
            c->ack_pending = 0;
            if (c->state != MLB_DC_STATE_OPEN) {
                c->state = MLB_DC_STATE_OPEN;
                if (queue_event(m, MLB_DC_EVENT_OPEN, c, NULL, 0, 0) != 0 ||
                    flush_pending_rx(m, c) != 0) return -1;
            }
            return 1;
        }
        if (c->open_pending) {
            rc = send_dcep(m, c, 0);
            if (rc == 1) return 0;
            if (rc < 0) { dc_set_error(m, "unable to queue DCEP OPEN"); return -1; }
            c->open_pending = 0;
            return 1;
        }
    }
    return 0;
}

static void update_inflight(MlbDataChannelManager *m) {
    size_t total = 0u;
    if (!m) return;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++) {
        DcChannel *c = &m->channels[i];
        size_t pending;
        if (!c->used) continue;
        pending = mlb_sctp_association_buffered_bytes(m->association, c->id);
        for (const DcQueuedMessage *q = m->send_head; q; q = q->next) {
            if (q->id == c->id && q->application_length <= SIZE_MAX - pending)
                pending += q->application_length;
        }
        c->buffered = pending;
        if (pending <= SIZE_MAX - total) total += pending;
        else total = SIZE_MAX;
    }
    m->buffered_bytes = total;
}

static int channel_has_queued_messages(const MlbDataChannelManager *m, uint16_t id) {
    for (const DcQueuedMessage *q = m ? m->send_head : NULL; q; q = q->next)
        if (q->id == id) return 1;
    return 0;
}

static int finish_channel_close(MlbDataChannelManager *m, DcChannel *c) {
    if (!m || !c || c->state == MLB_DC_STATE_CLOSED || c->close_event_queued ||
        !c->incoming_reset_done || !c->outgoing_reset_done) return 0;
    c->state = MLB_DC_STATE_CLOSED;
    c->buffered = 0u;
    free_pending_rx(c);
    if (queue_event(m, MLB_DC_EVENT_CLOSE, c, NULL, 0u, 0) != 0) return -1;
    c->close_event_queued = 1;
    return 1;
}

static int drain_reset_events(MlbDataChannelManager *m) {
    uint16_t stream_id;
    int direction, result, progress = 0;
    while (mlb_sctp_association_next_reset_event(m->association, &stream_id,
                                                  &direction, &result) > 0) {
        DcChannel *c = find_channel(m, stream_id);
        if (!c) continue;
        if (result != 0 && result != 1) {
            c->state = MLB_DC_STATE_FAILED;
            dc_set_error(m, "SCTP rejected DataChannel stream reset");
            if (queue_event(m, MLB_DC_EVENT_ERROR, c, NULL, 0u, 0) != 0) return -1;
            progress = 1;
            continue;
        }
        if (direction == MLB_SCTP_RESET_INCOMING) {
            c->incoming_reset_done = 1;
            c->close_requested = 1;
            if (c->state != MLB_DC_STATE_CLOSED) c->state = MLB_DC_STATE_CLOSING;
        } else if (direction == MLB_SCTP_RESET_OUTGOING) {
            c->outgoing_reset_done = 1;
            c->reset_request_started = 0;
        }
        if (finish_channel_close(m, c) < 0) return -1;
        progress = 1;
    }
    return progress;
}

static int start_ready_stream_resets(MlbDataChannelManager *m) {
    int progress = 0;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++) {
        DcChannel *c = &m->channels[i];
        int rc;
        if (!c->used || !c->close_requested || c->reset_request_started ||
            c->outgoing_reset_done || c->state == MLB_DC_STATE_FAILED) continue;
        if (channel_has_queued_messages(m, c->id) ||
            mlb_sctp_association_buffered_bytes(m->association, c->id) != 0u)
            continue;
        rc = mlb_sctp_association_request_stream_reset(m->association, c->id);
        if (rc == 1) continue; /* Another stream reset is currently in flight. */
        if (rc < 0) {
            c->state = MLB_DC_STATE_FAILED;
            dc_set_error(m, "could not request SCTP stream reset for DataChannel");
            if (queue_event(m, MLB_DC_EVENT_ERROR, c, NULL, 0u, 0) != 0) return -1;
            return -1;
        }
        c->reset_request_started = 1;
        progress = 1;
    }
    return progress;
}

static int flush_application(MlbDataChannelManager *m) {
    int sent = 0;
    DcQueuedMessage *q;
    if (!m) return -1;
    update_inflight(m);
    while ((q = m->send_head) != NULL) {
        DcChannel *c = find_channel(m, q->id);
        int rc;
        if (!c || (c->state != MLB_DC_STATE_OPEN && c->state != MLB_DC_STATE_CLOSING)) {
            dc_set_error(m, "queued message belongs to a non-open DataChannel"); return -1;
        }
        enum MlbSctpReliabilityPolicy policy = MLB_SCTP_RELIABLE;
        uint32_t reliability_value = 0u;
        if (c->has_max_retransmits) {
            policy = MLB_SCTP_RELIABILITY_RTX;
            reliability_value = c->max_retransmits;
        } else if (c->has_max_packet_lifetime) {
            policy = MLB_SCTP_RELIABILITY_TIMED;
            reliability_value = c->max_packet_lifetime;
        }
        rc = mlb_sctp_association_send_pr(m->association, q->id, q->ppid,
                                          q->unordered, q->data, q->length,
                                          policy, reliability_value);
        if (rc == 1) break;
        if (rc < 0) { dc_set_error(m, "SCTP rejected a queued DataChannel message"); return -1; }
        m->send_head = q->next; if (!m->send_head) m->send_tail = NULL;
        if (m->send_count) m->send_count--;
        free(q);
        sent++;
    }
    update_inflight(m);
    return sent;
}

int mlb_datachannel_available(void) { return mlb_sctp_available(); }
MlbDataChannelManager *mlb_datachannel_manager_create(const MlbDataChannelConfig *config) {
    MlbDataChannelManager *m;
    if (!config || !config->association || !mlb_datachannel_available() ||
        mlb_sctp_association_state(config->association) != MLB_SCTP_STATE_ESTABLISHED ||
        (config->role != MLB_DC_ROLE_DTLS_CLIENT && config->role != MLB_DC_ROLE_DTLS_SERVER)) return NULL;
    m = (MlbDataChannelManager *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->association = config->association; m->role = config->role;
    m->stream_limit = mlb_sctp_association_stream_limit(config->association);
    m->inbound_stream_limit = mlb_sctp_association_inbound_stream_limit(config->association);
    m->max_channels = config->max_channels ? config->max_channels : MLB_DC_MAX_CHANNELS;
    if (m->max_channels > MLB_DC_MAX_CHANNELS) m->max_channels = MLB_DC_MAX_CHANNELS;
    m->max_buffered_bytes = config->max_buffered_bytes ? config->max_buffered_bytes : MLB_DC_MAX_BUFFERED_BYTES;
    if (!m->stream_limit || !m->inbound_stream_limit || !m->max_channels || !m->max_buffered_bytes) { free(m); return NULL; }
    m->next_id = m->role == MLB_DC_ROLE_DTLS_CLIENT ? 0u : 1u;
    return m;
}
void mlb_datachannel_manager_destroy(MlbDataChannelManager *m) {
    MlbDataChannelEvent *e;
    if (!m) return;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++) {
        free(m->channels[i].label); free(m->channels[i].protocol);
        free_pending_rx(&m->channels[i]);
    }
    free_send_queue(m);
    while ((e = m->event_head) != NULL) { m->event_head = e->next; mlb_datachannel_event_free(e); }
    OPENSSL_cleanse(m, sizeof *m); free(m);
}

int mlb_datachannel_create(MlbDataChannelManager *m, const char *label, const char *protocol,
                           int ordered, int negotiated, int requested_id,
                           int has_lifetime, unsigned lifetime,
                           int has_retransmits, unsigned retransmits,
                           unsigned priority) {
    DcChannel *c; uint16_t id; size_t ll, pl;
    if (!m || !label || !protocol || (has_lifetime && has_retransmits) ||
        (!has_lifetime && lifetime != 0u) || (!has_retransmits && retransmits != 0u) ||
        lifetime > 65535u || retransmits > 65535u ||
        ((has_lifetime || has_retransmits) &&
         !mlb_sctp_association_partial_reliability_available(m->association)) ||
        priority == 0u || priority > UINT16_MAX) {
        if (m) dc_set_error(m, "invalid or unnegotiated DataChannel partial-reliability options");
        return -1;
    }
    ll = strlen(label); pl = strlen(protocol);
    if (ll > MLB_DC_MAX_LABEL_BYTES || pl > MLB_DC_MAX_PROTOCOL_BYTES ||
        !utf8_valid((const uint8_t *)label, ll) || !utf8_valid((const uint8_t *)protocol, pl)) {
        dc_set_error(m, "DataChannel label/protocol length or UTF-8 is invalid"); return -1;
    }
    if (negotiated) {
        if (requested_id < 0 || requested_id > 65534) { dc_set_error(m, "negotiated DataChannel requires a valid stream ID"); return -1; }
        id = (uint16_t)requested_id;
    } else {
        if (requested_id >= 0) { dc_set_error(m, "in-band DataChannel stream ID is allocated automatically"); return -1; }
        id = m->next_id;
        while (id < m->stream_limit && find_channel(m, id)) {
            if (id > 65531u) break;
            id = (uint16_t)(id + 2u);
        }
        if (id >= m->stream_limit) { dc_set_error(m, "no available SCTP stream IDs remain"); return -1; }
    }
    if ((!negotiated && (id & 1u) != (m->role == MLB_DC_ROLE_DTLS_CLIENT ? 0u : 1u)) ||
        id >= m->stream_limit || id >= m->inbound_stream_limit || find_channel(m, id)) {
        dc_set_error(m, "DataChannel stream ID has invalid DTLS-role parity, range, or is already used"); return -1;
    }
    c = alloc_channel(m); if (!c) { dc_set_error(m, "DataChannel limit reached"); return -1; }
    c->id = id; c->remote = 0; c->negotiated = negotiated != 0; c->ordered = ordered != 0;
    c->has_max_packet_lifetime = has_lifetime; c->max_packet_lifetime = lifetime;
    c->has_max_retransmits = has_retransmits; c->max_retransmits = retransmits;
    c->priority = (uint16_t)priority;
    if (mlb_sctp_association_set_stream_weight(m->association, id, c->priority) != 0) {
        memset(c, 0, sizeof *c); m->channel_count--;
        dc_set_error(m, "could not set SCTP stream scheduling weight"); return -1;
    }
    c->label = dc_dup(label); c->protocol = dc_dup(protocol);
    if (!c->label || !c->protocol) { free(c->label); free(c->protocol); memset(c, 0, sizeof *c); m->channel_count--; dc_set_error(m, "out of memory creating DataChannel"); return -1; }
    if (negotiated) {
        c->state = MLB_DC_STATE_OPEN;
        if (queue_event(m, MLB_DC_EVENT_OPEN, c, NULL, 0, 0) != 0) {
            free(c->label); free(c->protocol); memset(c, 0, sizeof *c); m->channel_count--;
            dc_set_error(m, "out of memory queuing DataChannel open event"); return -1;
        }
    } else {
        int frc;
        c->open_pending = 1;
        m->next_id = id <= 65531u ? (uint16_t)(id + 2u) : 65535u;
        frc = flush_control(m);
        if (frc < 0) {
            free(c->label); free(c->protocol); memset(c, 0, sizeof *c); m->channel_count--;
            return -1;
        }
    }
    return (int)id;
}

int mlb_datachannel_send(MlbDataChannelManager *m, uint16_t id,
                         const void *data, size_t length, int binary) {
    DcChannel *c; DcQueuedMessage *q; uint32_t ppid; size_t wire_len = length, application_len = length;
    uint8_t empty_marker = 0;
    if (!m || (!data && length) || length > MLB_SCTP_MAX_MESSAGE) return -1;
    c = find_channel(m, id);
    if (!c || c->state != MLB_DC_STATE_OPEN) { dc_set_error(m, "DataChannel is not open"); return -1; }
    if (!binary && !utf8_valid((const uint8_t *)data, length)) { dc_set_error(m, "outgoing DataChannel string is not valid UTF-8"); return -1; }
    if (length == 0u) { ppid = binary ? MLB_SCTP_PPID_WEBRTC_BINARY_EMPTY : MLB_SCTP_PPID_WEBRTC_STRING_EMPTY; data = &empty_marker; wire_len = 1u; }
    else ppid = binary ? MLB_SCTP_PPID_WEBRTC_BINARY : MLB_SCTP_PPID_WEBRTC_STRING;
    if (m->send_count >= DC_SEND_QUEUE_MAX || application_len > m->max_buffered_bytes - (m->buffered_bytes > m->max_buffered_bytes ? m->max_buffered_bytes : m->buffered_bytes)) return 1;
    q = (DcQueuedMessage *)malloc(sizeof *q + wire_len);
    if (!q) { dc_set_error(m, "out of memory queueing DataChannel message"); return -1; }
    memset(q, 0, sizeof *q); q->id = id; q->ppid = ppid; q->unordered = !c->ordered;
    q->length = wire_len; q->application_length = application_len;
    if (wire_len) memcpy(q->data, data, wire_len);
    if (m->send_tail) m->send_tail->next = q; else m->send_head = q;
    m->send_tail = q; m->send_count++; c->buffered += application_len; m->buffered_bytes += application_len;
    if (flush_application(m) < 0) return -1;
    return 0;
}

int mlb_datachannel_poll(MlbDataChannelManager *m, unsigned timeout_ms) {
    uint8_t data[MLB_SCTP_MAX_MESSAGE];
    uint16_t stream; uint32_t ppid; int unordered; size_t length;
    int progress = 0, rc;
    if (!m) return -1;
    rc = mlb_sctp_association_poll(m->association, timeout_ms);
    if (rc < 0) { dc_set_error(m, "SCTP association failed while polling DataChannels"); return -1; }
    if (rc > 0) progress = 1;
    rc = drain_reset_events(m);
    if (rc < 0) return -1;
    if (rc > 0) progress = 1;
    for (unsigned i = 0; i < DC_RX_BURST; i++) {
        length = 0;
        if (mlb_sctp_association_receive(m->association, &stream, &ppid, &unordered,
                                         data, sizeof data, &length) != 0) break;
        if (ppid == MLB_SCTP_PPID_DCEP)
            rc = process_dcep(m, stream, unordered, data, length);
        else
            rc = process_application_message(m, stream, ppid, unordered, data, length);
        if (rc < 0) { if (!m->error[0]) dc_set_error(m, "invalid SCTP DataChannel payload"); return -1; }
        progress = 1;
    }
    update_inflight(m);
    rc = flush_control(m); if (rc < 0) return -1; if (rc > 0) progress = 1;
    rc = flush_application(m); if (rc < 0) return -1; if (rc > 0) progress = 1;
    rc = start_ready_stream_resets(m); if (rc < 0) return -1; if (rc > 0) progress = 1;
    return progress;
}

int mlb_datachannel_state(const MlbDataChannelManager *m, uint16_t id) {
    if (!m) return MLB_DC_STATE_FAILED;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++)
        if (m->channels[i].used && m->channels[i].id == id) return m->channels[i].state;
    return MLB_DC_STATE_CLOSED;
}
size_t mlb_datachannel_buffered_amount(const MlbDataChannelManager *m, uint16_t id) {
    if (!m) return 0;
    for (unsigned i = 0; i < MLB_DC_MAX_CHANNELS; i++)
        if (m->channels[i].used && m->channels[i].id == id) return m->channels[i].buffered;
    return 0;
}
int mlb_datachannel_id_is_open(const MlbDataChannelManager *m, uint16_t id) {
    return mlb_datachannel_state(m, id) == MLB_DC_STATE_OPEN;
}
int mlb_datachannel_close(MlbDataChannelManager *m, uint16_t id) {
    DcChannel *c;
    if (!m || !(c = find_channel(m, id))) return -1;
    if (c->state == MLB_DC_STATE_CLOSED || c->state == MLB_DC_STATE_CLOSING) return 0;
    if (c->state == MLB_DC_STATE_FAILED) return -1;
    c->close_requested = 1;
    c->state = MLB_DC_STATE_CLOSING;
    return 0;
}
MlbDataChannelEvent *mlb_datachannel_next_event(MlbDataChannelManager *m) {
    MlbDataChannelEvent *e;
    if (!m || !(e = m->event_head)) return NULL;
    m->event_head = e->next; if (!m->event_head) m->event_tail = NULL; e->next = NULL;
    if (m->event_count) m->event_count--;
    m->event_bytes = m->event_bytes >= e->length ? m->event_bytes - e->length : 0u;
    return e;
}
void mlb_datachannel_event_free(MlbDataChannelEvent *e) {
    if (!e) return;
    free(e->label); free(e->protocol); free(e->data); free(e);
}
const char *mlb_datachannel_last_error(const MlbDataChannelManager *m) {
    return m && m->error[0] ? m->error : "";
}
#endif
