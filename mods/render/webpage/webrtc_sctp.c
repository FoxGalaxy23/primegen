#define _POSIX_C_SOURCE 200809L
#include "webrtc_sctp.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) || !defined(HAVE_OPENSSL_VAULT)
struct MlbSctpAssociation { int unavailable; };
int mlb_sctp_available(void) { return 0; }
MlbSctpAssociation *mlb_sctp_association_create(const MlbSctpConfig *c) { (void)c; return NULL; }
void mlb_sctp_association_destroy(MlbSctpAssociation *a) { (void)a; }
int mlb_sctp_association_start(MlbSctpAssociation *a) { (void)a; return -1; }
int mlb_sctp_association_poll(MlbSctpAssociation *a, unsigned t) { (void)a; (void)t; return -1; }
int mlb_sctp_association_state(const MlbSctpAssociation *a) { (void)a; return MLB_SCTP_STATE_SHUTDOWN; }
size_t mlb_sctp_association_buffered_bytes(const MlbSctpAssociation *a, uint16_t s) { (void)a; (void)s; return 0; }
int mlb_sctp_association_has_pending_data(const MlbSctpAssociation *a, uint16_t s) { (void)a; (void)s; return 0; }
uint16_t mlb_sctp_association_stream_limit(const MlbSctpAssociation *a) { (void)a; return 0; }
uint16_t mlb_sctp_association_inbound_stream_limit(const MlbSctpAssociation *a) { (void)a; return 0; }
int mlb_sctp_association_send(MlbSctpAssociation *a, uint16_t s, uint32_t p, int u, const void *d, size_t n) { (void)a; (void)s; (void)p; (void)u; (void)d; (void)n; return -1; }
int mlb_sctp_association_send_pr(MlbSctpAssociation *a, uint16_t s, uint32_t p, int u, const void *d, size_t n, enum MlbSctpReliabilityPolicy policy, uint32_t value) { (void)a; (void)s; (void)p; (void)u; (void)d; (void)n; (void)policy; (void)value; return -1; }
int mlb_sctp_association_partial_reliability_available(const MlbSctpAssociation *a) { (void)a; return 0; }
#ifdef MLB_SCTP_TESTING
int mlb_sctp_association_test_abandon_pr_message(MlbSctpAssociation *a, uint16_t s) { (void)a; (void)s; return -1; }
#endif
int mlb_sctp_association_receive(MlbSctpAssociation *a, uint16_t *s, uint32_t *p, int *u, void *d, size_t c, size_t *n) { (void)a; (void)s; (void)p; (void)u; (void)d; (void)c; (void)n; return -1; }
int mlb_sctp_association_get_stats(const MlbSctpAssociation *a, MlbSctpStats *s) { (void)a; (void)s; return -1; }
int mlb_sctp_association_request_stream_reset(MlbSctpAssociation *a, uint16_t s) { (void)a; (void)s; return -1; }
int mlb_sctp_association_next_reset_event(MlbSctpAssociation *a, uint16_t *s, int *d, int *r) { (void)a; (void)s; (void)d; (void)r; return 0; }
int mlb_sctp_association_last_error(const MlbSctpAssociation *a, char *o, size_t n) { (void)a; (void)o; (void)n; return -1; }
void mlb_sctp_association_abort(MlbSctpAssociation *a) { (void)a; }
uint32_t mlb_sctp_crc32c(const void *data, size_t length) { (void)data; (void)length; return 0; }
#else
#include <openssl/crypto.h>
#include <openssl/rand.h>

#define SCTP_COMMON_HEADER 12u
#define SCTP_CHUNK_HEADER 4u
#define SCTP_INIT_FIXED 16u
#define SCTP_DATA_FIXED 12u
#define SCTP_I_DATA_FIXED 16u
#define SCTP_SACK_FIXED 12u
#define SCTP_COOKIE_BYTES 32u
#define SCTP_QUEUE_MAX_BYTES (1024u * 1024u)
#define SCTP_RTO_MIN_MS 200u
#define SCTP_RTO_MAX_MS 60000u
#define SCTP_RECV_BURST 64u
#define SCTP_MAX_RETRANSMITS_DEFAULT 8u
#define SCTP_MAX_RETRANSMITS_LIMIT 16u

#define SCTP_CHUNK_DATA       0u
#define SCTP_CHUNK_I_DATA     64u
#define SCTP_CHUNK_INIT       1u
#define SCTP_CHUNK_INIT_ACK   2u
#define SCTP_CHUNK_SACK       3u
#define SCTP_CHUNK_ABORT      6u
#define SCTP_CHUNK_COOKIE_ECHO 10u
#define SCTP_CHUNK_COOKIE_ACK 11u
#define SCTP_CHUNK_RE_CONFIG 130u
#define SCTP_CHUNK_FORWARD_TSN 192u
#define SCTP_CHUNK_I_FORWARD_TSN 194u
#define SCTP_PARAM_STATE_COOKIE 7u
#define SCTP_PARAM_SUPPORTED_EXTENSIONS 0x8008u
#define SCTP_PARAM_FORWARD_TSN_SUPPORTED 0xc000u
#define SCTP_PARAM_OUTGOING_SSN_RESET 13u
#define SCTP_PARAM_RECONFIG_RESPONSE 16u
#define SCTP_RECONFIG_SUCCESS_NOTHING 0u
#define SCTP_RECONFIG_SUCCESS_PERFORMED 1u
#define SCTP_RECONFIG_DENIED 2u
#define SCTP_DATA_FLAG_U 0x04u
#define SCTP_DATA_FLAG_B 0x02u
#define SCTP_DATA_FLAG_E 0x01u

#define SCTP_PACKET_BUFFER MLB_SCTP_MAX_PACKET
#define SCTP_DEFAULT_RWND (1024u * 1024u)
#define SCTP_MIN_RWND 256u
#define SCTP_MAX_FLIGHT 16u
#define SCTP_MAX_CWND_BYTES (1024u * 1024u)
#define SCTP_RECONFIG_MAX_RETRIES SCTP_MAX_RETRANSMITS_DEFAULT
#define SCTP_RESET_EVENT_LIMIT 128u
#define SCTP_RECONFIG_MAX_STREAMS 128u
#define SCTP_DATA_FLIGHT_MAX 8u
#define SCTP_RX_REORDER_MAX 64u
#define SCTP_RX_REORDER_MAX_BYTES (512u * 1024u)
#define SCTP_DUPLICATE_TSN_MAX 16u
#define SCTP_FAST_RETRANSMIT_THRESHOLD 3u
#define SCTP_IDATA_RX_MESSAGE_MAX 32u
#define SCTP_WFQ_SCALE UINT64_C(1048576)

struct SctpMessage {
    struct SctpMessage *next;
    uint16_t stream_id;
    uint16_t stream_sequence;
    uint32_t ppid;
    int unordered;
    size_t length;
    uint8_t data[];
};

typedef struct SctpTxMessage {
    struct SctpTxMessage *next;
    uint16_t stream_id;
    uint16_t stream_sequence;
    uint32_t message_id;
    uint32_t fragment_sequence;
    uint32_t ppid;
    int unordered;
    enum MlbSctpReliabilityPolicy reliability_policy;
    uint32_t reliability_value;
    uint64_t enqueued_ms;
    size_t length;
    size_t offset;
    uint32_t first_tsn;
    uint32_t last_tsn;
    uint32_t final_tsn;
    int sequence_assigned;
    int all_sent;
    int abandoned;
    uint8_t data[];
} SctpTxMessage;

typedef struct SctpRxAssembly {
    uint8_t *data;
    size_t length;
    size_t capacity;
    uint16_t stream_id;
    uint32_t message_id;
    uint32_t next_fragment_sequence;
    uint32_t ppid;
    int unordered;
    int active;
} SctpRxAssembly;

typedef struct SctpFlightSlot {
    uint8_t packet[SCTP_PACKET_BUFFER];
    size_t length;
    uint32_t tsn;
    uint64_t sent_ms;
    unsigned retries;
    unsigned miss_indications;
    int fast_retransmitted;
    int sack_acked; /* Keep the TSN/packet until cumulative acknowledgement. */
    int abandoned;
    size_t data_length;
    unsigned rtt_epoch;
    int zero_window_probe;
    SctpTxMessage *owner;
    int active;
} SctpFlightSlot;

typedef struct SctpRxSlot {
    uint8_t *chunk;
    size_t length;
    uint32_t tsn;
    size_t payload_length;
} SctpRxSlot;

typedef struct SctpResetEventNode {
    struct SctpResetEventNode *next;
    uint16_t stream_id;
    int direction;
    int result;
} SctpResetEventNode;

struct MlbSctpAssociation {
    MlbDtlsPeer *dtls;
    enum MlbSctpState state;
    uint16_t local_port, remote_port;
    uint16_t outbound_streams, inbound_streams;
    uint16_t peer_outbound_streams, peer_inbound_streams;
    uint32_t local_tag, remote_tag;
    uint32_t local_initial_tsn, next_tsn;
    uint32_t peer_initial_tsn, cumulative_rx_tsn;
    uint32_t cumulative_tx_ack;
    uint16_t *tx_ssn;
    uint16_t *rx_ssn;
    uint32_t *tx_mid_ordered;
    uint32_t *tx_mid_unordered;
    uint32_t *rx_mid_ordered;
    uint32_t *rx_mid_unordered;
    uint16_t *tx_stream_weight;
    uint64_t *tx_stream_finish;
    uint64_t *tx_stream_pending_finish;
    uint32_t *tx_stream_pending_offset;
    uint64_t wfq_virtual_time;
    uint8_t cookie[SCTP_COOKIE_BYTES];
    size_t cookie_length;
    uint64_t timer_started_ms;
    unsigned rto_ms;
    unsigned initial_rto_ms;
    unsigned max_retransmits;
    unsigned retransmits;
    uint64_t srtt_scaled8;
    uint64_t rttvar_scaled4;
    uint64_t last_rtt_sample_ms;
    uint64_t rtt_samples;
    unsigned rtt_epoch;
    int rtt_initialized;
    SctpFlightSlot flight[SCTP_MAX_FLIGHT];
    unsigned flight_count;
    size_t flight_bytes;
    size_t peer_rwnd; /* Current peer rwnd after subtracting outstanding DATA. */
    size_t local_rwnd_capacity;
    size_t rx_reorder_storage_bytes;
    int window_update_pending;
    uint64_t zero_window_probe_due_ms;
    unsigned zero_window_probe_rto_ms;
    uint64_t zero_window_probes;
    SctpRxSlot rx_reorder[SCTP_RX_REORDER_MAX];
    unsigned rx_reorder_count;
    size_t rx_reorder_bytes;
    uint32_t duplicate_tsns[SCTP_DUPLICATE_TSN_MAX];
    unsigned duplicate_tsn_count;
    int fast_recovery;
    uint32_t fast_recovery_exit_tsn;
    size_t path_mtu;
    size_t congestion_window;
    size_t slow_start_threshold;
    size_t partial_bytes_acked;
    unsigned timeout_recovery;
    SctpTxMessage *tx_head, *tx_tail, *tx_cursor;
    size_t tx_queue_bytes;
    unsigned tx_queue_count;
    struct SctpMessage *queue_head, *queue_tail;
    size_t queue_bytes;
    uint8_t *rx_fragment_buffer;
    size_t rx_fragment_length, rx_fragment_capacity;
    uint16_t rx_fragment_stream, rx_fragment_ssn;
    uint32_t rx_fragment_ppid;
    int rx_fragment_unordered;
    int rx_fragment_active;
    int rx_fragment_discard;
    SctpRxAssembly rx_idata[SCTP_IDATA_RX_MESSAGE_MAX];
    unsigned rx_idata_count;
    int local_interleaving_enabled;
    int peer_idata_supported;
    int idata_negotiated;
    int peer_i_forward_tsn_supported;
    int i_forward_tsn_negotiated;
    int local_forward_tsn_enabled;
    int peer_forward_tsn_supported;
    int forward_tsn_negotiated;
    uint32_t advanced_peer_ack_point;
    uint32_t last_forward_tsn_sent;
    int forward_tsn_pending;
    uint8_t forward_tsn_chunk[SCTP_PACKET_BUFFER];
    size_t forward_tsn_chunk_length;
    uint32_t forward_tsn_point;
    uint64_t forward_tsn_sent_ms;
    unsigned forward_tsn_retries;
    unsigned forward_tsn_rto_ms;
    uint64_t pr_messages_abandoned;
    uint64_t forward_tsn_sent_count;
    uint64_t forward_tsn_received_count;
    int peer_reconfig_supported;
    uint32_t local_reconfig_sequence;
    uint32_t peer_reconfig_sequence;
    uint32_t last_peer_reconfig_sequence;
    int have_last_peer_reconfig_sequence;
    int deferred_peer_reset;
    uint32_t deferred_peer_reset_sequence;
    uint32_t deferred_peer_reset_last_tsn;
    size_t deferred_peer_reset_stream_count;
    uint16_t deferred_peer_reset_streams[SCTP_RECONFIG_MAX_STREAMS];
    int reconfig_requested;
    int reconfig_sent;
    uint16_t reconfig_stream_id;
    uint32_t reconfig_request_sequence;
    uint64_t reconfig_sent_ms;
    unsigned reconfig_retries;
    unsigned reconfig_rto_ms;
    uint8_t reconfig_packet[64];
    size_t reconfig_packet_length;
    SctpResetEventNode *reset_event_head, *reset_event_tail;
    unsigned reset_event_count;
    char error[192];
    uint64_t packets_sent, packets_received;
    uint64_t data_chunks_sent, data_chunks_received;
    uint64_t retransmissions;
    uint64_t out_of_order_data_chunks_received;
    uint64_t duplicate_data_chunks_received;
    uint64_t gap_sacks_sent;
    uint64_t gap_sacks_received;
    uint64_t fast_retransmits;
    uint64_t reneged_data_chunks_detected;
    uint64_t i_data_chunks_sent;
    uint64_t i_data_chunks_received;
    uint64_t application_bytes_sent, application_bytes_received;
};

static uint64_t sctp_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static size_t pad4(size_t n) { return (n + 3u) & ~(size_t)3u; }
static int tsn_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int complete_deferred_peer_reset(MlbSctpAssociation *a);
static int maybe_advance_forward_tsn(MlbSctpAssociation *a);
static int abandon_tx_message(MlbSctpAssociation *a, SctpTxMessage *message);

/* SCTP CRC32c: reflected Castagnoli algorithm; the wire checksum bytes are
 * the little-endian octets of the reflected result, as required by RFC 9260. */
uint32_t mlb_sctp_crc32c(const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    if (!p && length) return 0;
    for (size_t i = 0; i < length; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8u; bit++)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0x82f63b78u : 0u);
    }
    return ~crc;
}

static uint32_t bswap32_sctp(uint32_t v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}
static void write_sctp_crc(uint8_t *packet, size_t length) {
    uint32_t crc = mlb_sctp_crc32c(packet, length);
    put32(packet + 8u, bswap32_sctp(crc));
}
static int verify_sctp_crc(const uint8_t *packet, size_t length) {
    uint8_t copy[SCTP_PACKET_BUFFER];
    uint32_t got;
    if (!packet || length < SCTP_COMMON_HEADER || length > sizeof copy) return 0;
    memcpy(copy, packet, length);
    got = get32(copy + 8u);
    memset(copy + 8u, 0, 4u);
    return got == bswap32_sctp(mlb_sctp_crc32c(copy, length));
}

static void set_error(MlbSctpAssociation *a, const char *message) {
    if (a && message) snprintf(a->error, sizeof a->error, "%s", message);
}
static void free_messages(MlbSctpAssociation *a) {
    struct SctpMessage *m = a->queue_head;
    while (m) { struct SctpMessage *next = m->next; free(m); m = next; }
    a->queue_head = a->queue_tail = NULL;
    a->queue_bytes = 0;
}
static int random_u32(uint32_t *out) {
    uint32_t v = 0;
    if (!out || RAND_bytes((unsigned char *)&v, sizeof v) != 1) return -1;
    if (!v) v = 1;
    *out = v;
    return 0;
}

static int send_sctp_packet(MlbSctpAssociation *a, uint32_t verification_tag,
                            const uint8_t *chunk, size_t chunk_length) {
    uint8_t packet[SCTP_PACKET_BUFFER];
    size_t total, max_message;
    int rc;
    if (!a || !chunk || chunk_length < SCTP_CHUNK_HEADER ||
        chunk_length > sizeof packet - SCTP_COMMON_HEADER) return -1;
    total = SCTP_COMMON_HEADER + pad4(chunk_length);
    max_message = mlb_dtls_peer_max_message_size(a->dtls);
    if (a->path_mtu && max_message > a->path_mtu) max_message = a->path_mtu;
    if (total > max_message) return -1;
    memset(packet, 0, total);
    put16(packet, a->local_port);
    put16(packet + 2u, a->remote_port);
    put32(packet + 4u, verification_tag);
    memcpy(packet + SCTP_COMMON_HEADER, chunk, chunk_length);
    write_sctp_crc(packet, total);
    rc = mlb_dtls_peer_send(a->dtls, packet, total);
    if (rc == 0) a->packets_sent++;
    return rc;
}

static size_t make_init_chunk(MlbSctpAssociation *a, uint8_t type,
                              const uint8_t *cookie, size_t cookie_length,
                              uint8_t *out, size_t capacity) {
    size_t base = SCTP_CHUNK_HEADER + SCTP_INIT_FIXED;
    size_t cookie_param = 0u;
    size_t extension_param;
    size_t extension_count;
    size_t total;
    size_t offset;
    if (!a || !out || capacity < base ||
        (type != SCTP_CHUNK_INIT && type != SCTP_CHUNK_INIT_ACK)) return 0;
    if (type == SCTP_CHUNK_INIT_ACK) {
        if (!cookie || !cookie_length || cookie_length > 256u) return 0;
        cookie_param = pad4(4u + cookie_length);
    }
    /* WebRTC requires PR-SCTP. Advertise Forward-TSN using the dedicated
       0xC000 parameter; I-FORWARD-TSN is a Supported Extensions chunk ID and
       is only advertised alongside I-DATA. */
    extension_count = 1u + (a->local_interleaving_enabled ? 2u : 0u);
    extension_param = pad4(4u + extension_count);
    total = base + cookie_param + extension_param + 4u;
    if (total > capacity || total > UINT16_MAX) return 0;
    memset(out, 0, total);
    out[0] = type;
    put16(out + 2u, (uint16_t)total);
    put32(out + 4u, a->local_tag);
    put32(out + 8u, (uint32_t)a->local_rwnd_capacity);
    put16(out + 12u, a->outbound_streams);
    put16(out + 14u, a->inbound_streams);
    put32(out + 16u, a->local_initial_tsn);
    offset = base;
    if (cookie_param) {
        put16(out + offset, SCTP_PARAM_STATE_COOKIE);
        put16(out + offset + 2u, (uint16_t)(4u + cookie_length));
        memcpy(out + offset + 4u, cookie, cookie_length);
        offset += cookie_param;
    }
    put16(out + offset, SCTP_PARAM_SUPPORTED_EXTENSIONS);
    put16(out + offset + 2u, (uint16_t)(4u + extension_count));
    out[offset + 4u] = SCTP_CHUNK_RE_CONFIG;
    if (a->local_interleaving_enabled) {
        out[offset + 5u] = SCTP_CHUNK_I_DATA;
        out[offset + 6u] = SCTP_CHUNK_I_FORWARD_TSN;
    }
    offset += extension_param;
    put16(out + offset, SCTP_PARAM_FORWARD_TSN_SUPPORTED);
    put16(out + offset + 2u, 4u);
    return total;
}
static int send_init(MlbSctpAssociation *a) {
    uint8_t chunk[96];
    size_t length = make_init_chunk(a, SCTP_CHUNK_INIT, NULL, 0, chunk, sizeof chunk);
    return length ? send_sctp_packet(a, 0u, chunk, length) : -1;
}
static int send_init_ack(MlbSctpAssociation *a) {
    uint8_t chunk[128];
    size_t length = make_init_chunk(a, SCTP_CHUNK_INIT_ACK, a->cookie,
                                    a->cookie_length, chunk, sizeof chunk);
    return length ? send_sctp_packet(a, a->remote_tag, chunk, length) : -1;
}
static int send_cookie_echo(MlbSctpAssociation *a) {
    uint8_t chunk[4u + SCTP_COOKIE_BYTES];
    size_t n = 4u + a->cookie_length;
    memset(chunk, 0, n);
    chunk[0] = SCTP_CHUNK_COOKIE_ECHO;
    put16(chunk + 2u, (uint16_t)n);
    memcpy(chunk + 4u, a->cookie, a->cookie_length);
    return send_sctp_packet(a, a->remote_tag, chunk, n);
}
static int send_cookie_ack(MlbSctpAssociation *a) {
    uint8_t chunk[4] = {SCTP_CHUNK_COOKIE_ACK, 0, 0, 4};
    return send_sctp_packet(a, a->remote_tag, chunk, sizeof chunk);
}
static void remember_duplicate_tsn(MlbSctpAssociation *a, uint32_t tsn) {
    if (!a) return;
    for (unsigned i = 0; i < a->duplicate_tsn_count; i++)
        if (a->duplicate_tsns[i] == tsn) return;
    if (a->duplicate_tsn_count == SCTP_DUPLICATE_TSN_MAX) {
        memmove(a->duplicate_tsns, a->duplicate_tsns + 1u,
                (SCTP_DUPLICATE_TSN_MAX - 1u) * sizeof a->duplicate_tsns[0]);
        a->duplicate_tsn_count--;
    }
    a->duplicate_tsns[a->duplicate_tsn_count++] = tsn;
}

static size_t local_rx_buffer_used(const MlbSctpAssociation *a) {
    size_t used;
    if (!a) return SIZE_MAX;
    used = a->queue_bytes;
#define SCTP_ADD_USED(value) do { \
        size_t _v = (value); \
        if (_v > SIZE_MAX - used) return SIZE_MAX; \
        used += _v; \
    } while (0)
    SCTP_ADD_USED(a->rx_fragment_length);
    SCTP_ADD_USED(a->rx_reorder_bytes);
    for (unsigned i = 0; i < SCTP_IDATA_RX_MESSAGE_MAX; i++)
        if (a->rx_idata[i].active) SCTP_ADD_USED(a->rx_idata[i].length);
#undef SCTP_ADD_USED
    return used;
}

static size_t local_rx_window_available(const MlbSctpAssociation *a) {
    size_t used = local_rx_buffer_used(a);
    if (!a || used >= a->local_rwnd_capacity) return 0u;
    return a->local_rwnd_capacity - used;
}

static int flight_table_empty(const MlbSctpAssociation *a) {
    if (!a) return 1;
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++)
        if (a->flight[i].active) return 0;
    return 1;
}

static void update_rtt_estimator(MlbSctpAssociation *a, uint64_t sample_ms,
                                 uint64_t now_ms) {
    uint64_t old_srtt_ms, diff, rto;
    if (!a) return;
    if (!sample_ms) sample_ms = 1u;
    if (sample_ms > UINT_MAX) sample_ms = UINT_MAX;
    if (a->rtt_initialized && a->last_rtt_sample_ms) {
        uint64_t spacing = a->srtt_scaled8 / 8u;
        if (!spacing) spacing = 1u;
        if (now_ms <= a->last_rtt_sample_ms ||
            now_ms - a->last_rtt_sample_ms < spacing) return;
    }
    if (!a->rtt_initialized) {
        a->srtt_scaled8 = sample_ms * 8u;
        a->rttvar_scaled4 = sample_ms * 2u; /* 4 * (R / 2). */
        a->rtt_initialized = 1;
    } else {
        old_srtt_ms = a->srtt_scaled8 / 8u;
        diff = old_srtt_ms > sample_ms ? old_srtt_ms - sample_ms : sample_ms - old_srtt_ms;
        /* RTTVAR = 3/4 * RTTVAR + 1/4 * |old SRTT - R|. */
        a->rttvar_scaled4 = (3u * a->rttvar_scaled4 + 4u * diff + 2u) / 4u;
        a->srtt_scaled8 = (7u * a->srtt_scaled8 + 8u * sample_ms + 4u) / 8u;
    }
    if (!a->rttvar_scaled4) a->rttvar_scaled4 = 1u; /* G = 1 ms. */
    rto = (a->srtt_scaled8 + 7u) / 8u;
    if (a->rttvar_scaled4 > UINT64_MAX - rto) rto = UINT64_MAX;
    else rto += a->rttvar_scaled4; /* SRTT + 4 * RTTVAR. */
    if (rto < SCTP_RTO_MIN_MS) rto = SCTP_RTO_MIN_MS;
    if (rto > SCTP_RTO_MAX_MS) rto = SCTP_RTO_MAX_MS;
    a->rto_ms = (unsigned)rto;
    a->last_rtt_sample_ms = now_ms;
    a->rtt_samples++;
}

static void refresh_peer_rwnd(MlbSctpAssociation *a, uint32_t advertised_rwnd,
                              uint64_t now_ms) {
    size_t outstanding = 0u;
    if (!a) return;
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
        const SctpFlightSlot *slot = &a->flight[i];
        if (!slot->active || slot->sack_acked) continue;
        if (slot->data_length > SIZE_MAX - outstanding) {
            outstanding = SIZE_MAX;
            break;
        }
        outstanding += slot->data_length;
    }
    a->peer_rwnd = advertised_rwnd > outstanding ?
        (size_t)advertised_rwnd - outstanding : 0u;
    if (!a->peer_rwnd) {
        if (!a->zero_window_probe_due_ms) {
            a->zero_window_probe_rto_ms = a->rto_ms ? a->rto_ms : a->initial_rto_ms;
            if (a->zero_window_probe_rto_ms < SCTP_RTO_MIN_MS)
                a->zero_window_probe_rto_ms = SCTP_RTO_MIN_MS;
            a->zero_window_probe_due_ms = now_ms + a->zero_window_probe_rto_ms;
        }
    } else {
        a->zero_window_probe_due_ms = 0u;
        a->zero_window_probe_rto_ms = a->rto_ms ? a->rto_ms : a->initial_rto_ms;
    }
}

static int send_sack(MlbSctpAssociation *a) {
    uint8_t chunk[16u + SCTP_RX_REORDER_MAX * 4u + SCTP_DUPLICATE_TSN_MAX * 4u];
    uint16_t offsets[SCTP_RX_REORDER_MAX];
    unsigned offset_count = 0u, gap_count = 0u, duplicate_count = 0u;
    size_t chunk_length, used, available, mtu, max_variable;
    if (!a) return -1;
    for (unsigned i = 0; i < SCTP_RX_REORDER_MAX; i++) {
        if (!a->rx_reorder[i].chunk) continue;
        uint32_t delta = a->rx_reorder[i].tsn - a->cumulative_rx_tsn;
        if (delta == 0u || delta > UINT16_MAX) continue;
        offsets[offset_count++] = (uint16_t)delta;
    }
    /* The reorder table is small; insertion sort keeps the SACK builder
       deterministic and avoids allocating memory in the receive path. */
    for (unsigned i = 1; i < offset_count; i++) {
        uint16_t value = offsets[i];
        unsigned j = i;
        while (j && offsets[j - 1u] > value) {
            offsets[j] = offsets[j - 1u];
            j--;
        }
        offsets[j] = value;
    }

    mtu = a->path_mtu ? a->path_mtu : mlb_dtls_peer_max_message_size(a->dtls);
    if (mtu > sizeof chunk + SCTP_COMMON_HEADER) mtu = sizeof chunk + SCTP_COMMON_HEADER;
    /* SACK has a 16-byte fixed body plus four bytes per Gap Ack Block or
       Duplicate TSN. Reserve the fixed body and SCTP common header first. */
    if (mtu <= SCTP_COMMON_HEADER + 16u) return -1;
    max_variable = (mtu - SCTP_COMMON_HEADER - 16u) / 4u;

    memset(chunk, 0, sizeof chunk);
    chunk[0] = SCTP_CHUNK_SACK;
    put32(chunk + 4u, a->cumulative_rx_tsn);
    used = local_rx_buffer_used(a);
    available = used >= a->local_rwnd_capacity ? 0u : a->local_rwnd_capacity - used;
    put32(chunk + 8u, available > UINT32_MAX ? UINT32_MAX : (uint32_t)available);
    {
        unsigned i = 0u;
        while (i < offset_count) {
            uint16_t first = offsets[i], last = first;
            while (i + 1u < offset_count && offsets[i + 1u] == (uint16_t)(last + 1u)) {
                i++;
                last = offsets[i];
            }
            /* RFC 9260 section 6.7 requires the lowest-to-highest blocks
               that fit in one PMTU-bounded SACK, leaving the rest unacked. */
            if (gap_count >= max_variable) break;
            put16(chunk + 16u + gap_count * 4u, first);
            put16(chunk + 18u + gap_count * 4u, last);
            gap_count++;
            i++;
        }
    }
    duplicate_count = a->duplicate_tsn_count;
    if (duplicate_count > max_variable - gap_count)
        duplicate_count = (unsigned)(max_variable - gap_count);
    put16(chunk + 12u, (uint16_t)gap_count);
    put16(chunk + 14u, (uint16_t)duplicate_count);
    for (unsigned i = 0; i < duplicate_count; i++)
        put32(chunk + 16u + gap_count * 4u + i * 4u, a->duplicate_tsns[i]);
    chunk_length = 16u + gap_count * 4u + duplicate_count * 4u;
    put16(chunk + 2u, (uint16_t)chunk_length);
    if (SCTP_COMMON_HEADER + pad4(chunk_length) > mtu) return -1;
    if (send_sctp_packet(a, a->remote_tag, chunk, chunk_length) != 0) {
        a->window_update_pending = 1;
        return -1;
    }
    a->window_update_pending = 0;
    if (gap_count) a->gap_sacks_sent++;
    /* Duplicate TSN reports are advisory; discard the entries that could
       not fit as well as those sent, and report newly seen duplicates next. */
    a->duplicate_tsn_count = 0u;
    return 0;
}

static int parse_init_parameters(const uint8_t *chunk, size_t length,
                                 const uint8_t **cookie, size_t *cookie_length,
                                 int *reconfig_supported, int *idata_supported,
                                 int *i_forward_tsn_supported,
                                 int *forward_tsn_supported) {
    size_t offset = SCTP_CHUNK_HEADER + SCTP_INIT_FIXED;
    if (cookie) *cookie = NULL;
    if (cookie_length) *cookie_length = 0;
    if (reconfig_supported) *reconfig_supported = 0;
    if (idata_supported) *idata_supported = 0;
    if (i_forward_tsn_supported) *i_forward_tsn_supported = 0;
    if (forward_tsn_supported) *forward_tsn_supported = 0;
    while (offset < length) {
        uint16_t type, parameter_length;
        size_t padded;
        if (length - offset < 4u) return -1;
        type = get16(chunk + offset);
        parameter_length = get16(chunk + offset + 2u);
        if (parameter_length < 4u || parameter_length > length - offset) return -1;
        padded = pad4(parameter_length);
        if (padded > length - offset) return -1;
        if (type == SCTP_PARAM_STATE_COOKIE) {
            if (!cookie || !cookie_length || parameter_length <= 4u || *cookie) return -1;
            *cookie = chunk + offset + 4u;
            *cookie_length = parameter_length - 4u;
        } else if (type == SCTP_PARAM_SUPPORTED_EXTENSIONS) {
            if (parameter_length < 5u) return -1;
            for (size_t i = 4u; i < parameter_length; i++)
                if (chunk[offset + i] == SCTP_CHUNK_RE_CONFIG && reconfig_supported)
                    *reconfig_supported = 1;
                else if (chunk[offset + i] == SCTP_CHUNK_I_DATA && idata_supported)
                    *idata_supported = 1;
                else if (chunk[offset + i] == SCTP_CHUNK_I_FORWARD_TSN && i_forward_tsn_supported)
                    *i_forward_tsn_supported = 1;
        } else if (type == SCTP_PARAM_FORWARD_TSN_SUPPORTED) {
            if (parameter_length != 4u) return -1;
            if (forward_tsn_supported) *forward_tsn_supported = 1;
        }
        /* RFC 8261 forbids IPv4/IPv6 address parameters in INIT/INIT-ACK.
         * This single-homed DTLS transport rejects them instead of leaking or
         * depending on local address information. */
        if (type == 5u || type == 6u || type == 12u) return -1;
        offset += padded;
    }
    return offset == length ? 0 : -1;
}
static int queue_user_message(MlbSctpAssociation *a, uint16_t stream_id,
                              uint16_t ssn, uint32_t ppid, int unordered,
                              const uint8_t *data, size_t length) {
    struct SctpMessage *m;
    if (!length || length > SCTP_QUEUE_MAX_BYTES - a->queue_bytes) return -1;
    m = (struct SctpMessage *)malloc(sizeof *m + length);
    if (!m) return -1;
    m->next = NULL; m->stream_id = stream_id; m->stream_sequence = ssn;
    m->ppid = ppid; m->unordered = unordered; m->length = length;
    memcpy(m->data, data, length);
    if (a->queue_tail) a->queue_tail->next = m; else a->queue_head = m;
    a->queue_tail = m; a->queue_bytes += length;
    a->application_bytes_received += length;
    return 0;
}

static int append_rx_fragment(MlbSctpAssociation *a, const uint8_t *data, size_t length) {
    size_t needed, capacity;
    uint8_t *grown;
    if (length > MLB_SCTP_MAX_MESSAGE - a->rx_fragment_length) return -1;
    needed = a->rx_fragment_length + length;
    if (needed > a->rx_fragment_capacity) {
        capacity = a->rx_fragment_capacity ? a->rx_fragment_capacity : 1024u;
        while (capacity < needed) {
            if (capacity > SCTP_QUEUE_MAX_BYTES / 2u) { capacity = SCTP_QUEUE_MAX_BYTES; break; }
            capacity *= 2u;
        }
        grown = (uint8_t *)realloc(a->rx_fragment_buffer, capacity);
        if (!grown) return -1;
        a->rx_fragment_buffer = grown;
        a->rx_fragment_capacity = capacity;
    }
    memcpy(a->rx_fragment_buffer + a->rx_fragment_length, data, length);
    a->rx_fragment_length = needed;
    return 0;
}

static int append_idata_assembly(SctpRxAssembly *assembly,
                                 const uint8_t *data, size_t length) {
    size_t needed, capacity;
    uint8_t *grown;
    if (!assembly || (!data && length) ||
        length > MLB_SCTP_MAX_MESSAGE - assembly->length) return -1;
    needed = assembly->length + length;
    if (needed > assembly->capacity) {
        capacity = assembly->capacity ? assembly->capacity : 1024u;
        while (capacity < needed) {
            if (capacity > MLB_SCTP_MAX_MESSAGE / 2u) {
                capacity = MLB_SCTP_MAX_MESSAGE;
                break;
            }
            capacity *= 2u;
        }
        grown = (uint8_t *)realloc(assembly->data, capacity);
        if (!grown) return -1;
        assembly->data = grown;
        assembly->capacity = capacity;
    }
    if (length) memcpy(assembly->data + assembly->length, data, length);
    assembly->length = needed;
    return 0;
}

static void clear_idata_assembly(SctpRxAssembly *assembly) {
    if (!assembly) return;
    if (assembly->data && assembly->capacity)
        OPENSSL_cleanse(assembly->data, assembly->capacity);
    free(assembly->data);
    memset(assembly, 0, sizeof *assembly);
}

static SctpRxAssembly *find_idata_assembly(MlbSctpAssociation *a,
                                          uint16_t stream_id, int unordered,
                                          uint32_t message_id) {
    for (unsigned i = 0; i < SCTP_IDATA_RX_MESSAGE_MAX; i++) {
        SctpRxAssembly *assembly = &a->rx_idata[i];
        if (assembly->active && assembly->stream_id == stream_id &&
            assembly->unordered == unordered && assembly->message_id == message_id)
            return assembly;
    }
    return NULL;
}

static SctpRxAssembly *find_idata_stream_assembly(MlbSctpAssociation *a,
                                                  uint16_t stream_id) {
    for (unsigned i = 0; i < SCTP_IDATA_RX_MESSAGE_MAX; i++) {
        SctpRxAssembly *assembly = &a->rx_idata[i];
        if (assembly->active && assembly->stream_id == stream_id) return assembly;
    }
    return NULL;
}

static SctpRxAssembly *alloc_idata_assembly(MlbSctpAssociation *a) {
    for (unsigned i = 0; i < SCTP_IDATA_RX_MESSAGE_MAX; i++)
        if (!a->rx_idata[i].active) return &a->rx_idata[i];
    return NULL;
}

static int process_idata_in_order(MlbSctpAssociation *a,
                                  const uint8_t *chunk, size_t length) {
    uint32_t tsn, mid, ppid_or_fsn;
    uint16_t sid;
    int unordered, begin, end;
    size_t payload_length;
    const uint8_t *payload;
    SctpRxAssembly *assembly;
    uint32_t *expected_mid;

    if (length < SCTP_CHUNK_HEADER + SCTP_I_DATA_FIXED) return -1;
    tsn = get32(chunk + 4u);
    sid = get16(chunk + 8u);
    /* The 16-bit reserved field at offset 10 is sent as zero and ignored. */
    mid = get32(chunk + 12u);
    ppid_or_fsn = get32(chunk + 16u);
    unordered = (chunk[1] & SCTP_DATA_FLAG_U) != 0;
    begin = (chunk[1] & SCTP_DATA_FLAG_B) != 0;
    end = (chunk[1] & SCTP_DATA_FLAG_E) != 0;
    payload_length = length - (SCTP_CHUNK_HEADER + SCTP_I_DATA_FIXED);
    payload = chunk + SCTP_CHUNK_HEADER + SCTP_I_DATA_FIXED;
    if (sid >= a->inbound_streams || sid >= a->peer_outbound_streams ||
        (chunk[1] & 0xf0u) != 0u || !payload_length ||
        tsn != a->cumulative_rx_tsn + 1u) return -1;

    expected_mid = unordered ? &a->rx_mid_unordered[sid] : &a->rx_mid_ordered[sid];
    assembly = find_idata_assembly(a, sid, unordered, mid);
    if ((int32_t)(mid - *expected_mid) < 0) {
        /* A delayed fragment for a message skipped by I-FORWARD-TSN is
           acknowledged at the TSN layer but never delivered to the app. */
        a->cumulative_rx_tsn = tsn;
        a->data_chunks_received++;
        a->i_data_chunks_received++;
        return 0;
    }
    if (mid != *expected_mid && !unordered) return -1;
    if (begin) {
        /* RFC 8260 permits only one in-progress user message per stream.
           Different streams can be interleaved freely. */
        if (assembly || find_idata_stream_assembly(a, sid) || mid != *expected_mid)
            return -1;
        if (end) {
            if (queue_user_message(a, sid, (uint16_t)mid, ppid_or_fsn,
                                   unordered, payload, payload_length) != 0) return -1;
            (*expected_mid)++;
        } else {
            assembly = alloc_idata_assembly(a);
            if (!assembly || a->rx_idata_count >= SCTP_IDATA_RX_MESSAGE_MAX)
                return -1;
            memset(assembly, 0, sizeof *assembly);
            assembly->active = 1;
            assembly->stream_id = sid;
            assembly->unordered = unordered;
            assembly->message_id = mid;
            assembly->ppid = ppid_or_fsn;
            assembly->next_fragment_sequence = 1u;
            if (append_idata_assembly(assembly, payload, payload_length) != 0) {
                clear_idata_assembly(assembly);
                return -1;
            }
            a->rx_idata_count++;
        }
    } else {
        uint32_t fsn = ppid_or_fsn;
        if (!assembly || fsn != assembly->next_fragment_sequence ||
            append_idata_assembly(assembly, payload, payload_length) != 0)
            return -1;
        assembly->next_fragment_sequence++;
        if (end) {
            if (mid != *expected_mid ||
                queue_user_message(a, sid, (uint16_t)mid, assembly->ppid,
                                   unordered, assembly->data, assembly->length) != 0) {
                clear_idata_assembly(assembly);
                if (a->rx_idata_count) a->rx_idata_count--;
                return -1;
            }
            (*expected_mid)++;
            clear_idata_assembly(assembly);
            if (a->rx_idata_count) a->rx_idata_count--;
        }
    }
    a->cumulative_rx_tsn = tsn;
    a->data_chunks_received++;
    a->i_data_chunks_received++;
    return 0;
}

static void clear_rx_fragment(MlbSctpAssociation *a) {
    if (!a) return;
    if (a->rx_fragment_buffer && a->rx_fragment_capacity)
        OPENSSL_cleanse(a->rx_fragment_buffer, a->rx_fragment_capacity);
    free(a->rx_fragment_buffer);
    a->rx_fragment_buffer = NULL;
    a->rx_fragment_length = a->rx_fragment_capacity = 0;
    a->rx_fragment_stream = a->rx_fragment_ssn = 0;
    a->rx_fragment_ppid = 0;
    a->rx_fragment_unordered = 0;
    a->rx_fragment_active = 0;
    a->rx_fragment_discard = 0;
}

static int process_data_in_order(MlbSctpAssociation *a,
                                 const uint8_t *chunk, size_t length) {
    uint32_t tsn, ppid;
    uint16_t sid, ssn;
    int unordered, begin, end;
    size_t payload_length;
    const uint8_t *payload;
    if (chunk[0] == SCTP_CHUNK_I_DATA)
        return process_idata_in_order(a, chunk, length);
    if (length < SCTP_CHUNK_HEADER + SCTP_DATA_FIXED) return -1;
    tsn = get32(chunk + 4u);
    sid = get16(chunk + 8u);
    ssn = get16(chunk + 10u);
    ppid = get32(chunk + 12u);
    unordered = (chunk[1] & SCTP_DATA_FLAG_U) != 0;
    begin = (chunk[1] & SCTP_DATA_FLAG_B) != 0;
    end = (chunk[1] & SCTP_DATA_FLAG_E) != 0;
    payload_length = length - (SCTP_CHUNK_HEADER + SCTP_DATA_FIXED);
    payload = chunk + SCTP_CHUNK_HEADER + SCTP_DATA_FIXED;
    if (sid >= a->inbound_streams || sid >= a->peer_outbound_streams ||
        (chunk[1] & ~(SCTP_DATA_FLAG_U | SCTP_DATA_FLAG_B | SCTP_DATA_FLAG_E)) ||
        !payload_length || tsn != a->cumulative_rx_tsn + 1u) return -1;

    if (begin) {
        if (a->rx_fragment_active) return -1; /* DATA interleaving is not implemented. */
        a->rx_fragment_discard = 0;
        if (!unordered && ssn != a->rx_ssn[sid]) {
            if ((int16_t)(ssn - a->rx_ssn[sid]) >= 0) return -1;
            if (!end) {
                a->rx_fragment_active = 1;
                a->rx_fragment_discard = 1;
                a->rx_fragment_stream = sid;
                a->rx_fragment_ssn = ssn;
                a->rx_fragment_unordered = 0;
            }
            a->cumulative_rx_tsn = tsn;
            a->data_chunks_received++;
            return 0;
        }
        if (unordered && ssn != 0u) return -1;
        if (end) {
            if (queue_user_message(a, sid, ssn, ppid, unordered, payload, payload_length) != 0)
                return -1;
            if (!unordered) a->rx_ssn[sid]++;
        } else {
            a->rx_fragment_active = 1;
            a->rx_fragment_stream = sid;
            a->rx_fragment_ssn = ssn;
            a->rx_fragment_ppid = ppid;
            a->rx_fragment_unordered = unordered;
            if (append_rx_fragment(a, payload, payload_length) != 0) {
                clear_rx_fragment(a);
                return -1;
            }
        }
    } else {
        if (a->rx_fragment_discard) {
            if (end) {
                clear_rx_fragment(a);
                a->rx_fragment_discard = 0;
            }
            a->cumulative_rx_tsn = tsn;
            a->data_chunks_received++;
            return 0;
        }
        if (!a->rx_fragment_active && !unordered && ssn != a->rx_ssn[sid] &&
            (int16_t)(ssn - a->rx_ssn[sid]) < 0) {
            if (!end) {
                a->rx_fragment_active = 1;
                a->rx_fragment_discard = 1;
                a->rx_fragment_stream = sid;
                a->rx_fragment_ssn = ssn;
                a->rx_fragment_unordered = 0;
            }
            a->cumulative_rx_tsn = tsn;
            a->data_chunks_received++;
            return 0;
        }
        if (!a->rx_fragment_active || a->rx_fragment_stream != sid ||
            a->rx_fragment_ssn != ssn || a->rx_fragment_ppid != ppid ||
            a->rx_fragment_unordered != unordered ||
            append_rx_fragment(a, payload, payload_length) != 0) return -1;
        if (end) {
            if (queue_user_message(a, sid, ssn, ppid, unordered,
                                   a->rx_fragment_buffer, a->rx_fragment_length) != 0) {
                clear_rx_fragment(a);
                return -1;
            }
            if (!unordered) a->rx_ssn[sid]++;
            clear_rx_fragment(a);
        }
    }
    a->cumulative_rx_tsn = tsn;
    a->data_chunks_received++;
    return 0;
}

static int find_rx_reorder_slot(const MlbSctpAssociation *a, uint32_t tsn) {
    for (unsigned i = 0; i < SCTP_RX_REORDER_MAX; i++)
        if (a->rx_reorder[i].chunk && a->rx_reorder[i].tsn == tsn) return (int)i;
    return -1;
}

static int process_data(MlbSctpAssociation *a, const uint8_t *chunk, size_t length) {
    uint32_t tsn, delta;
    int slot_index;
    if (length < SCTP_CHUNK_HEADER +
                 (chunk[0] == SCTP_CHUNK_I_DATA ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED))
        return -1;
    tsn = get32(chunk + 4u);
    delta = tsn - a->cumulative_rx_tsn;

    if (tsn_leq(tsn, a->cumulative_rx_tsn)) {
        remember_duplicate_tsn(a, tsn);
        a->duplicate_data_chunks_received++;
        return send_sack(a) == 0 ? 0 : -1;
    }

    if (delta > 1u) {
        /* SCTP Gap Ack Blocks use 16-bit offsets from the cumulative TSN.
           Keep the receive window bounded to that representable range. */
        slot_index = find_rx_reorder_slot(a, tsn);
        if (slot_index >= 0) {
            remember_duplicate_tsn(a, tsn);
            a->duplicate_data_chunks_received++;
            return send_sack(a) == 0 ? 0 : -1;
        }
        {
            size_t fixed = chunk[0] == SCTP_CHUNK_I_DATA ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED;
            size_t payload_length = length - SCTP_CHUNK_HEADER - fixed;
            if (payload_length > local_rx_window_available(a))
                return send_sack(a) == 0 ? 0 : -1;
        }
        if (delta <= UINT16_MAX && a->rx_reorder_count < SCTP_RX_REORDER_MAX &&
            length <= SCTP_RX_REORDER_MAX_BYTES - a->rx_reorder_storage_bytes) {
            for (unsigned i = 0; i < SCTP_RX_REORDER_MAX; i++) {
                SctpRxSlot *slot = &a->rx_reorder[i];
                if (slot->chunk) continue;
                slot->chunk = (uint8_t *)malloc(length);
                if (!slot->chunk) return -1;
                memcpy(slot->chunk, chunk, length);
                slot->length = length;
                slot->tsn = tsn;
                slot->payload_length = length - SCTP_CHUNK_HEADER -
                    (chunk[0] == SCTP_CHUNK_I_DATA ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED);
                a->rx_reorder_count++;
                a->rx_reorder_bytes += slot->payload_length;
                a->rx_reorder_storage_bytes += length;
                a->out_of_order_data_chunks_received++;
                break;
            }
        }
        /* If the bounded queue is full, do not acknowledge the missing
           chunk. A later retransmission can be accepted after the window drains. */
        return send_sack(a) == 0 ? 0 : -1;
    }

    {
        size_t fixed = chunk[0] == SCTP_CHUNK_I_DATA ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED;
        size_t payload_length = length - SCTP_CHUNK_HEADER - fixed;
        if (payload_length > local_rx_window_available(a))
            return send_sack(a) == 0 ? 0 : -1;
    }
    if (process_data_in_order(a, chunk, length) != 0) return -1;
    for (;;) {
        uint32_t next_tsn = a->cumulative_rx_tsn + 1u;
        slot_index = find_rx_reorder_slot(a, next_tsn);
        if (slot_index < 0) break;
        SctpRxSlot *slot = &a->rx_reorder[slot_index];
        uint8_t *saved_chunk = slot->chunk;
        size_t saved_length = slot->length;
        size_t saved_payload = slot->payload_length;
        slot->chunk = NULL;
        slot->length = 0u;
        slot->tsn = 0u;
        slot->payload_length = 0u;
        if (a->rx_reorder_count) a->rx_reorder_count--;
        a->rx_reorder_bytes = a->rx_reorder_bytes >= saved_payload ?
            a->rx_reorder_bytes - saved_payload : 0u;
        a->rx_reorder_storage_bytes = a->rx_reorder_storage_bytes >= saved_length ?
            a->rx_reorder_storage_bytes - saved_length : 0u;
        if (process_data_in_order(a, saved_chunk, saved_length) != 0) {
            OPENSSL_cleanse(saved_chunk, saved_length);
            free(saved_chunk);
            return -1;
        }
        OPENSSL_cleanse(saved_chunk, saved_length);
        free(saved_chunk);
    }
    if (send_sack(a) != 0) return -1;
    if (complete_deferred_peer_reset(a) < 0) return -1;
    return 0;
}

static int process_forward_tsn(MlbSctpAssociation *a,
                               const uint8_t *chunk, size_t length, int idata) {
    uint32_t new_cumulative;
    size_t entry_size = idata ? 8u : 4u;
    size_t offset;
    if (!a || !chunk || length < 8u || ((length - 8u) % entry_size) != 0u) return -1;
    if (!a->forward_tsn_negotiated || (idata && !a->i_forward_tsn_negotiated) ||
        (!idata && a->idata_negotiated)) return -1;
    new_cumulative = get32(chunk + 4u);
    /* A Forward-TSN whose cumulative point is not newer MUST NOT move the
       receiver's TSN point backwards. Its stream-skip entries still matter:
       a message may be incomplete because not all of its fragments had
       acquired TSNs before the sender abandoned it. In that case every
       transmitted fragment can already be cumulatively acknowledged while
       the ordered MID/SSN still needs to be skipped. Apply those entries
       monotonically below, then SACK without changing cumulative_rx_tsn. */
    int advances_cumulative = !tsn_leq(new_cumulative, a->cumulative_rx_tsn);
    if (advances_cumulative &&
        (uint32_t)(new_cumulative - a->cumulative_rx_tsn) >
        (SCTP_RX_REORDER_MAX + SCTP_MAX_FLIGHT + 1u)) return -1;

    /* Apply skipped stream sequence numbers before draining gap-acked chunks.
       Late chunks for the skipped MID/SSN are then acknowledged but ignored. */
    for (offset = 8u; offset < length; offset += entry_size) {
        uint16_t sid = get16(chunk + offset);
        if (sid >= a->inbound_streams || sid >= a->peer_outbound_streams) return -1;
        if (idata) {
            uint16_t flags = get16(chunk + offset + 2u);
            uint32_t mid = get32(chunk + offset + 4u);
            int unordered = (flags & 1u) != 0u;
            uint32_t *expected = unordered ? &a->rx_mid_unordered[sid] : &a->rx_mid_ordered[sid];
            if ((flags & 0xfffeu) != 0u) return -1;
            if ((int32_t)(mid - *expected) >= 0) *expected = mid + 1u;
            for (unsigned i = 0; i < SCTP_IDATA_RX_MESSAGE_MAX; i++) {
                SctpRxAssembly *assembly = &a->rx_idata[i];
                if (assembly->active && assembly->stream_id == sid &&
                    assembly->unordered == unordered &&
                    (int32_t)(assembly->message_id - mid) <= 0) {
                    clear_idata_assembly(assembly);
                    if (a->rx_idata_count) a->rx_idata_count--;
                }
            }
        } else {
            uint16_t ssn = get16(chunk + offset + 2u);
            if ((int16_t)(ssn - a->rx_ssn[sid]) >= 0)
                a->rx_ssn[sid] = (uint16_t)(ssn + 1u);
            if (a->rx_fragment_active && a->rx_fragment_stream == sid &&
                (int16_t)(a->rx_fragment_ssn - a->rx_ssn[sid]) < 0) {
                clear_rx_fragment(a);
                a->rx_fragment_discard = 1;
            }
        }
    }

    if (!advances_cumulative) {
        a->forward_tsn_received_count++;
        if (complete_deferred_peer_reset(a) < 0) return -1;
        return send_sack(a);
    }

    /* Preserve and deliver already-received chunks covered by the new
       cumulative point, while skipping holes that Forward-TSN explicitly
       abandons. This keeps unrelated streams from losing complete messages. */
    for (;;) {
        int best = -1;
        uint32_t best_tsn = 0u;
        for (unsigned i = 0; i < SCTP_RX_REORDER_MAX; i++) {
            SctpRxSlot *slot = &a->rx_reorder[i];
            if (!slot->chunk || !tsn_leq(slot->tsn, new_cumulative)) continue;
            if (best < 0 || tsn_leq(slot->tsn, best_tsn)) {
                best = (int)i;
                best_tsn = slot->tsn;
            }
        }
        if (best < 0) break;
        SctpRxSlot *slot = &a->rx_reorder[best];
        uint8_t *saved = slot->chunk;
        size_t saved_length = slot->length;
        size_t saved_payload = slot->payload_length;
        if (tsn_leq(best_tsn, a->cumulative_rx_tsn)) {
            OPENSSL_cleanse(saved, saved_length);
            free(saved);
        } else {
            if ((uint32_t)(best_tsn - a->cumulative_rx_tsn) > 1u) {
                if (!idata) clear_rx_fragment(a);
                a->cumulative_rx_tsn = best_tsn - 1u;
            }
            if (process_data_in_order(a, saved, saved_length) != 0) {
                OPENSSL_cleanse(saved, saved_length);
                free(saved);
                return -1;
            }
            OPENSSL_cleanse(saved, saved_length);
            free(saved);
        }
        slot->chunk = NULL;
        slot->length = 0u;
        slot->tsn = 0u;
        slot->payload_length = 0u;
        if (a->rx_reorder_count) a->rx_reorder_count--;
        a->rx_reorder_bytes = a->rx_reorder_bytes >= saved_payload ?
            a->rx_reorder_bytes - saved_payload : 0u;
        a->rx_reorder_storage_bytes = a->rx_reorder_storage_bytes >= saved_length ?
            a->rx_reorder_storage_bytes - saved_length : 0u;
    }
    if (!idata && a->rx_fragment_active) {
        /* A legacy DATA message cannot be interleaved with another one. Any
           unresolved partial assembly crossed by this cumulative skip is stale. */
        clear_rx_fragment(a);
        a->rx_fragment_discard = 1;
    }
    a->cumulative_rx_tsn = new_cumulative;
    a->forward_tsn_received_count++;
    if (complete_deferred_peer_reset(a) < 0) return -1;
    return send_sack(a);
}

static void clear_tx_message(MlbSctpAssociation *a) {
    SctpTxMessage *message;
    if (!a) return;
    while ((message = a->tx_head) != NULL) {
        a->tx_head = message->next;
        OPENSSL_cleanse(message->data, message->length);
        free(message);
    }
    a->tx_tail = a->tx_cursor = NULL;
    a->tx_queue_bytes = 0u;
    a->tx_queue_count = 0u;
}

static void reap_acked_tx_messages(MlbSctpAssociation *a, uint32_t cumulative_tsn) {
    if (!a) return;
    while (a->tx_head && a->tx_head->all_sent &&
           tsn_leq(a->tx_head->final_tsn, cumulative_tsn)) {
        SctpTxMessage *message = a->tx_head;
        a->tx_head = message->next;
        if (!a->tx_head) a->tx_tail = NULL;
        if (a->tx_cursor == message) a->tx_cursor = message->next;
        a->tx_queue_bytes = a->tx_queue_bytes >= message->length ?
            a->tx_queue_bytes - message->length : 0u;
        if (a->tx_queue_count) a->tx_queue_count--;
        OPENSSL_cleanse(message->data, message->length);
        free(message);
    }
}

static size_t idata_payload_capacity(const MlbSctpAssociation *a) {
    size_t mtu;
    if (!a) return 0u;
    mtu = a->path_mtu ? a->path_mtu : mlb_dtls_peer_max_message_size(a->dtls);
    if (mtu > mlb_dtls_peer_max_message_size(a->dtls))
        mtu = mlb_dtls_peer_max_message_size(a->dtls);
    if (mtu <= SCTP_COMMON_HEADER + SCTP_CHUNK_HEADER + SCTP_I_DATA_FIXED) return 0u;
    return (mtu - (SCTP_COMMON_HEADER + SCTP_CHUNK_HEADER + SCTP_I_DATA_FIXED)) & ~(size_t)3u;
}

static uint64_t idata_wfq_finish(MlbSctpAssociation *a,
                                 const SctpTxMessage *message,
                                 size_t payload_length) {
    uint16_t weight;
    uint64_t start, service, finish;
    uint16_t stream_id;
    if (!a || !message || message->stream_id >= MLB_SCTP_MAX_STREAMS) return UINT64_MAX;
    stream_id = message->stream_id;
    if (a->tx_stream_pending_finish[stream_id] &&
        a->tx_stream_pending_offset[stream_id] == message->offset)
        return a->tx_stream_pending_finish[stream_id];
    weight = a->tx_stream_weight[stream_id];
    if (!weight) weight = 256u;
    start = a->tx_stream_finish[stream_id];
    if (start < a->wfq_virtual_time) start = a->wfq_virtual_time;
    service = ((uint64_t)payload_length * SCTP_WFQ_SCALE + (uint64_t)weight - 1u) / weight;
    if (!service) service = 1u;
    if (UINT64_MAX - start < service) finish = UINT64_MAX;
    else finish = start + service;
    a->tx_stream_pending_offset[stream_id] = (uint32_t)message->offset;
    a->tx_stream_pending_finish[stream_id] = finish;
    return finish;
}

static void refresh_tx_cursor(MlbSctpAssociation *a) {
    a->tx_cursor = NULL;
    for (SctpTxMessage *message = a->tx_head; message; message = message->next) {
        if (!message->all_sent && message->offset < message->length) {
            a->tx_cursor = message;
            return;
        }
    }
}

/* RFC 8260 weighted-fair-queueing scheduler. The virtual finish tag charges
 * each I-DATA fragment by its payload bytes divided by the stream weight.
 * This preserves stream-local ordering while allowing streams with larger
 * DataChannel priority weights to receive a proportional share. */
static SctpTxMessage *choose_idata_tx_message(MlbSctpAssociation *a) {
    SctpTxMessage *best = NULL;
    uint64_t best_finish = UINT64_MAX;
    size_t max_payload = idata_payload_capacity(a);
    if (!a || !max_payload) return NULL;
    for (SctpTxMessage *message = a->tx_head; message; message = message->next) {
        int blocked = 0;
        size_t remaining, payload;
        uint64_t finish;
        if (message->all_sent || message->offset >= message->length) continue;
        for (SctpTxMessage *prior = a->tx_head; prior != message; prior = prior->next) {
            if (prior->stream_id == message->stream_id && !prior->all_sent) {
                blocked = 1;
                break;
            }
        }
        if (blocked) continue;
        remaining = message->length - message->offset;
        payload = remaining > max_payload ? max_payload : remaining;
        finish = idata_wfq_finish(a, message, payload);
        if (!best || finish < best_finish ||
            (finish == best_finish && message->stream_id < best->stream_id)) {
            best = message;
            best_finish = finish;
        }
    }
    return best;
}

static int tx_message_expired(const SctpTxMessage *message, uint64_t now) {
    if (!message || message->reliability_policy != MLB_SCTP_RELIABILITY_TIMED) return 0;
    return now >= message->enqueued_ms && now - message->enqueued_ms >= message->reliability_value;
}

static void unlink_tx_message(MlbSctpAssociation *a, SctpTxMessage *message) {
    SctpTxMessage **pp;
    if (!a || !message) return;
    pp = &a->tx_head;
    while (*pp && *pp != message) pp = &(*pp)->next;
    if (*pp != message) return;
    *pp = message->next;
    if (a->tx_tail == message) {
        a->tx_tail = NULL;
        for (SctpTxMessage *it = a->tx_head; it; it = it->next) a->tx_tail = it;
    }
    if (a->tx_cursor == message) a->tx_cursor = message->next;
    a->tx_queue_bytes = a->tx_queue_bytes >= message->length ?
        a->tx_queue_bytes - message->length : 0u;
    if (a->tx_queue_count) a->tx_queue_count--;
    OPENSSL_cleanse(message->data, message->length);
    free(message);
}

static int abandon_tx_message(MlbSctpAssociation *a, SctpTxMessage *message) {
    if (!a || !message || message->abandoned) return 0;
    if (!message->sequence_assigned || !message->first_tsn) {
        a->pr_messages_abandoned++;
        unlink_tx_message(a, message);
        return 1;
    }
    /* A cumulative acknowledgement proves that the complete message reached
       the peer; it must not be retracted by a later lifetime/retry expiry. */
    if (message->all_sent && tsn_leq(message->final_tsn, a->cumulative_tx_ack)) return 0;
    message->abandoned = 1;
    message->all_sent = 1;
    message->offset = message->length;
    message->final_tsn = message->last_tsn;
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
        SctpFlightSlot *slot = &a->flight[i];
        if (!slot->active || slot->owner != message) continue;
        slot->abandoned = 1;
        if (!slot->sack_acked) {
            a->flight_bytes = a->flight_bytes >= slot->length ?
                a->flight_bytes - slot->length : 0u;
            if (a->flight_count) a->flight_count--;
        }
    }
    a->pr_messages_abandoned++;
    if (a->tx_cursor == message) a->tx_cursor = message->next;
    return maybe_advance_forward_tsn(a);
}

static int expire_pr_messages(MlbSctpAssociation *a) {
    SctpTxMessage *message, *next;
    uint64_t now;
    int progress = 0;
    if (!a || !a->forward_tsn_negotiated || (a->idata_negotiated && !a->i_forward_tsn_negotiated))
        return 0;
    now = sctp_now_ms();
    for (message = a->tx_head; message; message = next) {
        next = message->next;
        if (message->reliability_policy == MLB_SCTP_RELIABLE ||
            !tx_message_expired(message, now)) continue;
        if (abandon_tx_message(a, message) < 0) return -1;
        progress = 1;
    }
    return progress;
}

static SctpFlightSlot *find_flight_tsn(MlbSctpAssociation *a, uint32_t tsn) {
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++)
        if (a->flight[i].active && a->flight[i].tsn == tsn) return &a->flight[i];
    return NULL;
}

typedef struct SctpForwardEntry {
    uint16_t stream_id;
    uint16_t ssn;
    uint32_t mid;
    int unordered;
} SctpForwardEntry;

static int forward_entry_update(SctpForwardEntry *entries, unsigned *count,
                                uint16_t sid, uint16_t ssn, uint32_t mid,
                                int unordered, int idata) {
    for (unsigned i = 0; i < *count; i++) {
        if (entries[i].stream_id == sid && (!idata || entries[i].unordered == unordered)) {
            if (idata) {
                if ((int32_t)(mid - entries[i].mid) > 0) entries[i].mid = mid;
            } else if ((int16_t)(ssn - entries[i].ssn) > 0) entries[i].ssn = ssn;
            return 0;
        }
    }
    if (*count >= SCTP_MAX_FLIGHT) return -1;
    entries[*count].stream_id = sid;
    entries[*count].ssn = ssn;
    entries[*count].mid = mid;
    entries[*count].unordered = unordered;
    (*count)++;
    return 0;
}

static int send_forward_tsn_chunk(MlbSctpAssociation *a, uint32_t point) {
    SctpForwardEntry entries[SCTP_MAX_FLIGHT];
    unsigned count = 0u;
    int idata;
    size_t fixed, chunk_length, max_chunk;
    uint8_t chunk[SCTP_PACKET_BUFFER];
    if (!a || !a->forward_tsn_negotiated) return -1;
    idata = a->idata_negotiated;
    if (idata && !a->i_forward_tsn_negotiated) return -1;
    memset(entries, 0, sizeof entries);
    for (SctpTxMessage *m = a->tx_head; m; m = m->next) {
        if (!m->abandoned || !m->sequence_assigned || !m->last_tsn ||
            !tsn_leq(m->last_tsn, point) || tsn_leq(m->last_tsn, a->cumulative_tx_ack))
            continue;
        if (idata) {
            if (forward_entry_update(entries, &count, m->stream_id, 0u,
                                     m->message_id, m->unordered, 1) != 0) return -1;
        } else if (!m->unordered) {
            if (forward_entry_update(entries, &count, m->stream_id,
                                     m->stream_sequence, 0u, 0, 0) != 0) return -1;
        }
    }
    fixed = idata ? 8u : 8u;
    chunk_length = fixed + (idata ? (size_t)count * 8u : (size_t)count * 4u);
    max_chunk = a->path_mtu ? a->path_mtu - SCTP_COMMON_HEADER : 0u;
    if (!max_chunk || chunk_length > max_chunk || chunk_length > sizeof chunk) return -1;
    memset(chunk, 0, chunk_length);
    chunk[0] = idata ? SCTP_CHUNK_I_FORWARD_TSN : SCTP_CHUNK_FORWARD_TSN;
    put16(chunk + 2u, (uint16_t)chunk_length);
    put32(chunk + 4u, point);
    for (unsigned i = 0; i < count; i++) {
        size_t off = 8u + (idata ? (size_t)i * 8u : (size_t)i * 4u);
        put16(chunk + off, entries[i].stream_id);
        if (idata) {
            put16(chunk + off + 2u, entries[i].unordered ? 1u : 0u);
            put32(chunk + off + 4u, entries[i].mid);
        } else {
            put16(chunk + off + 2u, entries[i].ssn);
        }
    }
    if (send_sctp_packet(a, a->remote_tag, chunk, chunk_length) != 0) return -1;
    memcpy(a->forward_tsn_chunk, chunk, chunk_length);
    a->forward_tsn_chunk_length = chunk_length;
    a->forward_tsn_point = point;
    a->last_forward_tsn_sent = point;
    a->forward_tsn_pending = 1;
    a->forward_tsn_sent_ms = sctp_now_ms();
    a->forward_tsn_retries = 0u;
    a->forward_tsn_rto_ms = a->initial_rto_ms;
    a->forward_tsn_sent_count++;
    return 1;
}

/* Advanced.Peer.Ack.Point can pass cumulatively or selectively acknowledged
 * chunks and chunks deliberately abandoned by PR-SCTP, but never a live gap. */
static int maybe_advance_forward_tsn(MlbSctpAssociation *a) {
    uint32_t point;
    int crossed_abandoned = 0;
    if (!a || !a->forward_tsn_negotiated || (a->idata_negotiated && !a->i_forward_tsn_negotiated))
        return 0;
    point = a->advanced_peer_ack_point;
    if (tsn_leq(point, a->cumulative_tx_ack)) point = a->cumulative_tx_ack;
    while (point != a->next_tsn - 1u) {
        uint32_t next = point + 1u;
        SctpFlightSlot *slot = find_flight_tsn(a, next);
        if (!slot) break;
        if (slot->abandoned) crossed_abandoned = 1;
        if (!slot->abandoned && !slot->sack_acked) break;
        point = next;
    }
    if (point == a->advanced_peer_ack_point || tsn_leq(point, a->advanced_peer_ack_point)) {
        if (tsn_leq(a->advanced_peer_ack_point, a->cumulative_tx_ack))
            a->advanced_peer_ack_point = a->cumulative_tx_ack;
        return 0;
    }
    a->advanced_peer_ack_point = point;
    if (!crossed_abandoned || tsn_leq(point, a->cumulative_tx_ack)) return 0;
    return send_forward_tsn_chunk(a, point);
}

static int retransmit_forward_tsn(MlbSctpAssociation *a) {
    int rc;
    uint64_t now;
    if (!a || !a->forward_tsn_pending || !a->forward_tsn_chunk_length) return 0;
    now = sctp_now_ms();
    if (now - a->forward_tsn_sent_ms < a->forward_tsn_rto_ms) return 0;
    if (a->forward_tsn_retries >= a->max_retransmits) {
        set_error(a, "SCTP Forward-TSN acknowledgement timed out");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    rc = send_sctp_packet(a, a->remote_tag, a->forward_tsn_chunk,
                          a->forward_tsn_chunk_length);
    if (rc != 0) return rc < 0 ? -1 : 0;
    a->forward_tsn_retries++;
    a->forward_tsn_sent_ms = now;
    if (a->forward_tsn_rto_ms < SCTP_RTO_MAX_MS / 2u)
        a->forward_tsn_rto_ms *= 2u;
    else a->forward_tsn_rto_ms = SCTP_RTO_MAX_MS;
    a->retransmissions++;
    return 1;
}

/* SACK acknowledgement. The bounded flight table permits multiple records
 * in flight while preventing unbounded DTLS queue growth. */
static int send_next_data_fragment(MlbSctpAssociation *a) {
    uint8_t packet[SCTP_PACKET_BUFFER], *chunk;
    size_t mtu, max_payload, remaining, payload_length, chunk_length, packet_length;
    uint32_t tsn;
    int first, last, rc, use_idata;
    SctpFlightSlot *slot = NULL;
    SctpTxMessage *message;
    if (!a) return -1;
    use_idata = a->idata_negotiated;
    message = use_idata ? choose_idata_tx_message(a) : a->tx_cursor;
    if (!message || message->offset >= message->length) return 1;
    if (tx_message_expired(message, sctp_now_ms())) {
        if (abandon_tx_message(a, message) < 0) return -1;
        return 0;
    }
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++)
        if (!a->flight[i].active) { slot = &a->flight[i]; break; }
    if (!slot || a->flight_count >= SCTP_DATA_FLIGHT_MAX ||
        a->flight_bytes >= a->congestion_window)
        return 1;
    if (a->timeout_recovery && a->flight_count != 0u) return 1;
    mtu = a->path_mtu ? a->path_mtu : mlb_dtls_peer_max_message_size(a->dtls);
    if (mtu > mlb_dtls_peer_max_message_size(a->dtls)) mtu = mlb_dtls_peer_max_message_size(a->dtls);
    if (mtu <= SCTP_COMMON_HEADER + SCTP_CHUNK_HEADER +
               (use_idata ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED)) return -1;
    max_payload = mtu - (SCTP_COMMON_HEADER + SCTP_CHUNK_HEADER +
                         (use_idata ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED));
    max_payload &= ~(size_t)3u;
    if (!max_payload) return -1;
    remaining = message->length - message->offset;
    payload_length = remaining > max_payload ? max_payload : remaining;
    if (a->peer_rwnd == 0u) {
        uint64_t now = sctp_now_ms();
        if (!flight_table_empty(a)) return 1;
        if (!a->zero_window_probe_due_ms) {
            a->zero_window_probe_rto_ms = a->rto_ms ? a->rto_ms : a->initial_rto_ms;
            if (a->zero_window_probe_rto_ms < SCTP_RTO_MIN_MS)
                a->zero_window_probe_rto_ms = SCTP_RTO_MIN_MS;
            a->zero_window_probe_due_ms = now + a->zero_window_probe_rto_ms;
        }
        if (now < a->zero_window_probe_due_ms) return 1;
        if (payload_length > 1u) payload_length = 1u;
    } else if (payload_length > a->peer_rwnd) {
        /* A non-zero but smaller advertised window is still usable. Split
           this fragment to the available DATA payload instead of stalling
           until the receiver advertises a full PMTU-sized window. */
        payload_length = a->peer_rwnd;
    }
    first = message->offset == 0u;
    last = payload_length == remaining;
    chunk_length = SCTP_CHUNK_HEADER +
                   (use_idata ? SCTP_I_DATA_FIXED : SCTP_DATA_FIXED) + payload_length;
    packet_length = SCTP_COMMON_HEADER + pad4(chunk_length);
    if (packet_length > sizeof packet || packet_length > mtu || chunk_length > UINT16_MAX)
        return -1;
    if (a->flight_bytes + packet_length > a->congestion_window) return 1;
    memset(packet, 0, packet_length);
    put16(packet, a->local_port);
    put16(packet + 2u, a->remote_port);
    put32(packet + 4u, a->remote_tag);
    chunk = packet + SCTP_COMMON_HEADER;
    chunk[0] = use_idata ? SCTP_CHUNK_I_DATA : SCTP_CHUNK_DATA;
    chunk[1] = (uint8_t)((first ? SCTP_DATA_FLAG_B : 0u) |
                         (last ? SCTP_DATA_FLAG_E : 0u) |
                         (message->unordered ? SCTP_DATA_FLAG_U : 0u));
    put16(chunk + 2u, (uint16_t)chunk_length);
    tsn = a->next_tsn;
    put32(chunk + 4u, tsn);
    put16(chunk + 8u, message->stream_id);
    if (first && !message->sequence_assigned) {
        if (use_idata) {
            uint32_t *next_mid = message->unordered ? &a->tx_mid_unordered[message->stream_id] :
                                                     &a->tx_mid_ordered[message->stream_id];
            message->message_id = *next_mid;
        } else {
            message->stream_sequence = message->unordered ? 0u : a->tx_ssn[message->stream_id];
        }
    }
    if (use_idata) {
        put16(chunk + 10u, 0u);
        put32(chunk + 12u, message->message_id);
        put32(chunk + 16u, first ? message->ppid : message->fragment_sequence);
        memcpy(chunk + 20u, message->data + message->offset, payload_length);
    } else {
        put16(chunk + 10u, message->unordered ? 0u : message->stream_sequence);
        put32(chunk + 12u, message->ppid);
        memcpy(chunk + 16u, message->data + message->offset, payload_length);
    }
    write_sctp_crc(packet, packet_length);
    rc = mlb_dtls_peer_send(a->dtls, packet, packet_length);
    if (rc != 0) return rc < 0 ? -1 : 1;
    memcpy(slot->packet, packet, packet_length);
    slot->length = packet_length;
    slot->tsn = tsn;
    slot->sent_ms = sctp_now_ms();
    slot->retries = 0;
    slot->data_length = payload_length;
    slot->rtt_epoch = a->rtt_epoch;
    slot->zero_window_probe = a->peer_rwnd == 0u;
    slot->owner = message;
    slot->active = 1;
    if (slot->zero_window_probe) {
        a->zero_window_probes++;
        a->zero_window_probe_due_ms = slot->sent_ms + a->zero_window_probe_rto_ms;
    } else {
        a->peer_rwnd = a->peer_rwnd >= payload_length ?
            a->peer_rwnd - payload_length : 0u;
    }
    if (first && !message->sequence_assigned) {
        if (use_idata) {
            if (message->unordered) a->tx_mid_unordered[message->stream_id]++;
            else a->tx_mid_ordered[message->stream_id]++;
        } else if (!message->unordered) {
            a->tx_ssn[message->stream_id]++;
        }
        message->sequence_assigned = 1;
    }
    if (!message->first_tsn) message->first_tsn = tsn;
    message->last_tsn = tsn;
    a->flight_count++;
    a->flight_bytes += packet_length;
    a->next_tsn++;
    message->offset += payload_length;
    if (use_idata && !first) message->fragment_sequence++;
    else if (use_idata && first) message->fragment_sequence = 1u;
    if (last) {
        message->all_sent = 1;
        message->final_tsn = tsn;
    }
    a->data_chunks_sent++;
    if (use_idata) {
        uint64_t finish = idata_wfq_finish(a, message, payload_length);
        a->i_data_chunks_sent++;
        a->tx_stream_finish[message->stream_id] = finish;
        a->tx_stream_pending_finish[message->stream_id] = 0u;
        if (finish > a->wfq_virtual_time) a->wfq_virtual_time = finish;
        refresh_tx_cursor(a);
    } else if (last) {
        a->tx_cursor = message->next;
    }
    a->packets_sent++;
    return 0;
}

static int pump_data_window(MlbSctpAssociation *a) {
    int sent = 0;
    if (!a) return 0;
    while (a->idata_negotiated ? choose_idata_tx_message(a) != NULL : a->tx_cursor != NULL) {
        int rc = send_next_data_fragment(a);
        if (rc < 0) return -1;
        if (rc > 0) break;
        sent++;
    }
    return sent;
}

static int sack_gap_contains(const uint8_t *chunk, uint16_t gap_count,
                             uint32_t cumulative, uint32_t tsn) {
    uint32_t offset = tsn - cumulative;
    if (offset == 0u || offset > UINT16_MAX) return 0;
    for (unsigned i = 0; i < gap_count; i++) {
        uint16_t first = get16(chunk + 16u + i * 4u);
        uint16_t last = get16(chunk + 18u + i * 4u);
        if (offset < first) return 0;
        if (offset <= last) return 1;
    }
    return 0;
}

static int fast_retransmit_slot(MlbSctpAssociation *a, SctpFlightSlot *slot) {
    int rc;
    SctpTxMessage *owner;
    if (!a || !slot || !slot->active || slot->fast_retransmitted || slot->abandoned) return 0;
    owner = slot->owner;
    if (owner && (tx_message_expired(owner, sctp_now_ms()) ||
        (owner->reliability_policy == MLB_SCTP_RELIABILITY_RTX &&
         slot->retries >= owner->reliability_value)))
        return abandon_tx_message(a, owner) < 0 ? -1 : 1;
    if (slot->retries >= a->max_retransmits) return -1;
    rc = mlb_dtls_peer_send(a->dtls, slot->packet, slot->length);
    if (rc != 0) return rc < 0 ? -1 : 0;
    slot->fast_retransmitted = 1;
    a->rtt_epoch++; /* Karn: invalidate RTT samples from ambiguous flight. */
    slot->sent_ms = sctp_now_ms();
    slot->retries++;
    a->retransmissions++;
    a->fast_retransmits++;
    if (!a->fast_recovery) {
        a->slow_start_threshold = a->congestion_window / 2u;
        if (a->slow_start_threshold < 4u * a->path_mtu)
            a->slow_start_threshold = 4u * a->path_mtu;
        a->congestion_window = a->slow_start_threshold;
        a->partial_bytes_acked = 0u;
        a->fast_recovery = 1;
        a->fast_recovery_exit_tsn = a->next_tsn - 1u;
    }
    return 1;
}

static int process_sack(MlbSctpAssociation *a, const uint8_t *chunk, size_t length) {
    uint32_t cumulative, advertised_rwnd, highest_gap_offset = 0u;
    uint32_t htna = 0u;
    uint16_t gaps, duplicates, previous_end = 0u;
    size_t acked_bytes = 0u, flight_before;
    uint64_t now = sctp_now_ms();
    unsigned fast_indices[SCTP_MAX_FLIGHT], fast_count = 0u;
    uint8_t reneged_mask[SCTP_MAX_FLIGHT] = {0};
    uint32_t last_sent;
    int cumulative_advanced, htna_valid = 0, was_fast_recovery;
    if (length < 16u) return -1;
    cumulative = get32(chunk + 4u);
    advertised_rwnd = get32(chunk + 8u);
    gaps = get16(chunk + 12u);
    duplicates = get16(chunk + 14u);
    if (16u + (size_t)gaps * 4u + (size_t)duplicates * 4u != length) return -1;
    last_sent = a->next_tsn - 1u;
    if (cumulative != a->cumulative_tx_ack && !tsn_leq(cumulative, last_sent)) return -1;
    if (tsn_leq(cumulative, a->cumulative_tx_ack) && cumulative != a->cumulative_tx_ack)
        return 0; /* Ignore stale SACKs. */

    for (unsigned i = 0; i < gaps; i++) {
        uint16_t first = get16(chunk + 16u + i * 4u);
        uint16_t last = get16(chunk + 18u + i * 4u);
        if (!first || last < first || first <= previous_end) return -1;
        if (!tsn_leq(cumulative + (uint32_t)last, last_sent)) return -1;
        previous_end = last;
        highest_gap_offset = last;
    }
    cumulative_advanced = cumulative != a->cumulative_tx_ack;
    was_fast_recovery = a->fast_recovery;
    flight_before = a->flight_bytes;
    if (gaps) a->gap_sacks_received++;

    /* Keep selectively acknowledged TSNs in the bounded scoreboard until
       cumulative acknowledgement passes them. This is needed both for the
       HTNA (Highest TSN Newly Acknowledged) rule and receiver reneging. */
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
        SctpFlightSlot *slot = &a->flight[i];
        int cumulatively_acked, selectively_acked;
        if (!slot->active) continue;
        cumulatively_acked = tsn_leq(slot->tsn, cumulative);
        selectively_acked = !cumulatively_acked &&
            sack_gap_contains(chunk, gaps, cumulative, slot->tsn);

        if (cumulatively_acked) {
            if (!slot->sack_acked) {
                if (!slot->abandoned) {
                    if (!slot->zero_window_probe && slot->retries == 0u &&
                        !slot->fast_retransmitted && slot->rtt_epoch == a->rtt_epoch &&
                        now >= slot->sent_ms)
                        update_rtt_estimator(a, now - slot->sent_ms, now);
                    acked_bytes += slot->length;
                    if (!htna_valid || tsn_leq(htna, slot->tsn)) {
                        htna = slot->tsn;
                        htna_valid = 1;
                    }
                    a->flight_bytes = a->flight_bytes >= slot->length ?
                        a->flight_bytes - slot->length : 0u;
                    if (a->flight_count) a->flight_count--;
                }
            }
            OPENSSL_cleanse(slot->packet, slot->length);
            memset(slot, 0, sizeof *slot);
        } else if (selectively_acked) {
            if (!slot->sack_acked) {
                if (!slot->abandoned) {
                    if (!slot->zero_window_probe && slot->retries == 0u &&
                        !slot->fast_retransmitted && slot->rtt_epoch == a->rtt_epoch &&
                        now >= slot->sent_ms)
                        update_rtt_estimator(a, now - slot->sent_ms, now);
                    acked_bytes += slot->length;
                    if (!htna_valid || tsn_leq(htna, slot->tsn)) {
                        htna = slot->tsn;
                        htna_valid = 1;
                    }
                    a->flight_bytes = a->flight_bytes >= slot->length ?
                        a->flight_bytes - slot->length : 0u;
                    if (a->flight_count) a->flight_count--;
                }
                slot->sack_acked = 1;
            }
        } else if (slot->sack_acked) {
            /* A receiver may renege on a Gap Ack. RFC 9260 section 6.2.1
               requires one miss indication even if this SACK acknowledges
               no other new TSN. Restore the DATA to flight accounting. */
            slot->sack_acked = 0;
            reneged_mask[i] = 1u;
            a->reneged_data_chunks_detected++;
            if (!slot->abandoned) {
                a->flight_count++;
                a->flight_bytes += slot->length;
                if (slot->miss_indications < UINT_MAX) slot->miss_indications++;
                if (slot->miss_indications >= SCTP_FAST_RETRANSMIT_THRESHOLD &&
                    fast_count < SCTP_MAX_FLIGHT)
                    fast_indices[fast_count++] = i;
            }
        }
    }

    if (cumulative_advanced) {
        a->cumulative_tx_ack = cumulative;
        reap_acked_tx_messages(a, cumulative);
    }
    /* The advertised value is the receiver's total free space. Convert it
       into the sender's effective free window after accounting for all
       outstanding DATA not acknowledged by this SACK. */
    refresh_peer_rwnd(a, advertised_rwnd, now);
    if (a->forward_tsn_pending && tsn_leq(a->forward_tsn_point, cumulative)) {
        a->forward_tsn_pending = 0;
        a->forward_tsn_chunk_length = 0u;
        a->forward_tsn_retries = 0u;
    }
    if (maybe_advance_forward_tsn(a) < 0) {
        set_error(a, "could not send SCTP Forward-TSN");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    if (acked_bytes) {
        if (flight_before >= a->congestion_window &&
            a->congestion_window < a->slow_start_threshold) {
            size_t increase = acked_bytes < a->path_mtu ? acked_bytes : a->path_mtu;
            a->congestion_window += increase;
        } else if (a->congestion_window >= a->slow_start_threshold && !a->fast_recovery) {
            a->partial_bytes_acked += acked_bytes;
            if (a->partial_bytes_acked >= a->congestion_window &&
                flight_before >= a->congestion_window) {
                a->partial_bytes_acked -= a->congestion_window;
                a->congestion_window += a->path_mtu;
            }
        }
        if (a->congestion_window > SCTP_MAX_CWND_BYTES)
            a->congestion_window = SCTP_MAX_CWND_BYTES;
    }

    /* HTNA increments missing reports only for SACKs that newly acknowledge
       a higher TSN. While already in Fast Recovery, a SACK that advances
       the cumulative ACK counts every missing TSN reported by its Gap Ack
       Blocks, even when no additional TSN is newly gap-acknowledged. A
       reneged Gap Ack was already counted once above and is not double-counted. */
    if (htna_valid || (was_fast_recovery && cumulative_advanced && highest_gap_offset)) {
        uint32_t report_limit = (was_fast_recovery && cumulative_advanced && highest_gap_offset) ?
            cumulative + highest_gap_offset : htna;
        for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
            SctpFlightSlot *slot = &a->flight[i];
            if (!slot->active || slot->sack_acked || slot->fast_retransmitted || reneged_mask[i]) continue;
            if (!tsn_leq(slot->tsn, report_limit) || slot->tsn == report_limit) continue;
            if (slot->miss_indications < UINT_MAX) slot->miss_indications++;
            if (slot->miss_indications >= SCTP_FAST_RETRANSMIT_THRESHOLD &&
                fast_count < SCTP_MAX_FLIGHT)
                fast_indices[fast_count++] = i;
        }
    }

    if (was_fast_recovery && tsn_leq(a->fast_recovery_exit_tsn, cumulative)) {
        a->fast_recovery = 0;
        a->partial_bytes_acked = 0u;
    }

    for (unsigned i = 0; i < fast_count; i++) {
        int rc = fast_retransmit_slot(a, &a->flight[fast_indices[i]]);
        if (rc < 0) {
            set_error(a, "failed to queue SCTP fast retransmission");
            a->state = MLB_SCTP_STATE_FAILED;
            return -1;
        }
    }

    if (!a->flight_count) {
        a->timeout_recovery = 0;
        if (!a->fast_recovery) a->partial_bytes_acked = 0u;
    } else if (cumulative_advanced) {
        /* The cumulative ACK advanced; re-arm the oldest unacknowledged TSN.
           Gap-only SACKs do not restart T3. */
        SctpFlightSlot *oldest = NULL;
        for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
            SctpFlightSlot *slot = &a->flight[i];
            if (slot->active && !slot->sack_acked &&
                (!oldest || tsn_leq(slot->tsn, oldest->tsn))) oldest = slot;
        }
        if (oldest) oldest->sent_ms = now;
    }

    if (a->tx_cursor && !a->timeout_recovery && pump_data_window(a) < 0) {
        set_error(a, "unable to send the next SCTP DATA fragment");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    return 0;
}

static int queue_reset_event(MlbSctpAssociation *a, uint16_t stream_id,
                             int direction, int result) {
    SctpResetEventNode *event;
    if (!a || a->reset_event_count >= SCTP_RESET_EVENT_LIMIT) return -1;
    event = (SctpResetEventNode *)calloc(1, sizeof *event);
    if (!event) return -1;
    event->stream_id = stream_id;
    event->direction = direction;
    event->result = result;
    if (a->reset_event_tail) a->reset_event_tail->next = event;
    else a->reset_event_head = event;
    a->reset_event_tail = event;
    a->reset_event_count++;
    return 0;
}

static int send_reconfig_response(MlbSctpAssociation *a, uint32_t request_sequence,
                                  uint32_t result) {
    uint8_t chunk[16];
    memset(chunk, 0, sizeof chunk);
    chunk[0] = SCTP_CHUNK_RE_CONFIG;
    put16(chunk + 2u, sizeof chunk);
    put16(chunk + 4u, SCTP_PARAM_RECONFIG_RESPONSE);
    put16(chunk + 6u, 12u);
    put32(chunk + 8u, request_sequence);
    put32(chunk + 12u, result);
    return send_sctp_packet(a, a->remote_tag, chunk, sizeof chunk);
}

static int complete_deferred_peer_reset(MlbSctpAssociation *a) {
    uint32_t sequence;
    size_t count;
    if (!a || !a->deferred_peer_reset ||
        !tsn_leq(a->deferred_peer_reset_last_tsn, a->cumulative_rx_tsn)) return 0;
    sequence = a->deferred_peer_reset_sequence;
    count = a->deferred_peer_reset_stream_count;
    for (size_t i = 0; i < count; i++) {
        uint16_t stream_id = a->deferred_peer_reset_streams[i];
        a->rx_ssn[stream_id] = 0u;
        a->rx_mid_ordered[stream_id] = 0u;
        a->rx_mid_unordered[stream_id] = 0u;
        for (unsigned j = 0; j < SCTP_IDATA_RX_MESSAGE_MAX; j++) {
            if (a->rx_idata[j].active && a->rx_idata[j].stream_id == stream_id) {
                clear_idata_assembly(&a->rx_idata[j]);
                if (a->rx_idata_count) a->rx_idata_count--;
            }
        }
        if (queue_reset_event(a, stream_id, MLB_SCTP_RESET_INCOMING,
                              SCTP_RECONFIG_SUCCESS_PERFORMED) != 0) return -1;
    }
    a->last_peer_reconfig_sequence = sequence;
    a->have_last_peer_reconfig_sequence = 1;
    a->peer_reconfig_sequence++;
    a->deferred_peer_reset = 0;
    a->deferred_peer_reset_sequence = 0u;
    a->deferred_peer_reset_last_tsn = 0u;
    a->deferred_peer_reset_stream_count = 0u;
    memset(a->deferred_peer_reset_streams, 0, sizeof a->deferred_peer_reset_streams);
    return send_reconfig_response(a, sequence, SCTP_RECONFIG_SUCCESS_PERFORMED);
}

static int send_reconfig_request_now(MlbSctpAssociation *a) {
    uint8_t chunk[24];
    uint32_t sequence;
    if (!a || !a->reconfig_requested || a->reconfig_sent ||
        a->flight_count || a->tx_head) return 1;
    if (!a->peer_reconfig_supported) return -1;
    sequence = a->local_reconfig_sequence++;
    memset(chunk, 0, sizeof chunk);
    chunk[0] = SCTP_CHUNK_RE_CONFIG;
    put16(chunk + 2u, 24u); /* Includes the two-octet TLV alignment padding. */
    put16(chunk + 4u, SCTP_PARAM_OUTGOING_SSN_RESET);
    put16(chunk + 6u, 18u);
    put32(chunk + 8u, sequence);
    put32(chunk + 12u, a->peer_reconfig_sequence - 1u);
    put32(chunk + 16u, a->next_tsn - 1u);
    put16(chunk + 20u, a->reconfig_stream_id);
    if (send_sctp_packet(a, a->remote_tag, chunk, sizeof chunk) != 0) return -1;
    memcpy(a->reconfig_packet, chunk, sizeof chunk);
    a->reconfig_packet_length = sizeof chunk;
    a->reconfig_request_sequence = sequence;
    a->reconfig_sent = 1;
    a->reconfig_sent_ms = sctp_now_ms();
    a->reconfig_retries = 0u;
    a->reconfig_rto_ms = a->initial_rto_ms;
    return 0;
}

static int complete_local_stream_reset(MlbSctpAssociation *a, uint32_t result) {
    uint16_t stream_id;
    if (!a || !a->reconfig_sent) return 0;
    stream_id = a->reconfig_stream_id;
    if (result == SCTP_RECONFIG_SUCCESS_NOTHING || result == SCTP_RECONFIG_SUCCESS_PERFORMED) {
        a->tx_ssn[stream_id] = 0u;
        a->tx_mid_ordered[stream_id] = 0u;
        a->tx_mid_unordered[stream_id] = 0u;
        if (queue_reset_event(a, stream_id, MLB_SCTP_RESET_OUTGOING, (int)result) != 0)
            return -1;
    } else {
        if (queue_reset_event(a, stream_id, MLB_SCTP_RESET_OUTGOING, (int)result) != 0)
            return -1;
    }
    a->reconfig_requested = 0;
    a->reconfig_sent = 0;
    a->reconfig_packet_length = 0u;
    a->reconfig_sent_ms = 0u;
    a->reconfig_retries = 0u;
    return 0;
}

static int process_reconfig(MlbSctpAssociation *a, const uint8_t *chunk, size_t length) {
    size_t offset = SCTP_CHUNK_HEADER;
    int progress = 0;
    if (!a->peer_reconfig_supported) return 0;
    while (offset < length) {
        uint16_t type, parameter_length;
        size_t padded;
        if (length - offset < 4u) return -1;
        type = get16(chunk + offset);
        parameter_length = get16(chunk + offset + 2u);
        if (parameter_length < 4u || parameter_length > length - offset) return -1;
        padded = pad4(parameter_length);
        if (padded > length - offset) return -1;
        if (type == SCTP_PARAM_OUTGOING_SSN_RESET) {
            uint32_t request_sequence, response_sequence, last_tsn;
            size_t stream_count;
            if (parameter_length < 18u || ((parameter_length - 16u) & 1u) != 0u)
                return -1;
            request_sequence = get32(chunk + offset + 4u);
            response_sequence = get32(chunk + offset + 8u);
            last_tsn = get32(chunk + offset + 12u);
            stream_count = (parameter_length - 16u) / 2u;
            if (stream_count == 0u || stream_count > SCTP_RECONFIG_MAX_STREAMS) {
                if (send_reconfig_response(a, request_sequence, SCTP_RECONFIG_DENIED) != 0) return -1;
                offset += padded;
                progress = 1;
                continue;
            }
            if (a->reconfig_sent && response_sequence == a->reconfig_request_sequence) {
                if (complete_local_stream_reset(a, SCTP_RECONFIG_SUCCESS_PERFORMED) != 0)
                    return -1;
            }
            if (a->have_last_peer_reconfig_sequence &&
                request_sequence == a->last_peer_reconfig_sequence) {
                if (send_reconfig_response(a, request_sequence, SCTP_RECONFIG_SUCCESS_NOTHING) != 0)
                    return -1;
                offset += padded;
                progress = 1;
                continue;
            }
            if (a->deferred_peer_reset) {
                uint32_t result = request_sequence == a->deferred_peer_reset_sequence ? 6u : 4u;
                if (send_reconfig_response(a, request_sequence, result) != 0) return -1;
                offset += padded;
                progress = 1;
                continue;
            }
            if (request_sequence != a->peer_reconfig_sequence) {
                if (send_reconfig_response(a, request_sequence, 5u) != 0) return -1;
                offset += padded;
                progress = 1;
                continue;
            }
            for (size_t i = 0; i < stream_count; i++) {
                uint16_t stream_id = get16(chunk + offset + 16u + i * 2u);
                if (stream_id >= a->inbound_streams || stream_id >= a->peer_outbound_streams) {
                    if (send_reconfig_response(a, request_sequence, SCTP_RECONFIG_DENIED) != 0) return -1;
                    offset += padded;
                    progress = 1;
                    goto next_reconfig_parameter;
                }
                a->deferred_peer_reset_streams[i] = stream_id;
            }
            /* A reset may arrive before the final DATA TSN it names. Reply
               IN-PROGRESS and retain the request until the receive cumulative
               TSN reaches the sender's Last Assigned TSN. */
            if ((int32_t)(last_tsn - a->cumulative_rx_tsn) > 0) {
                a->deferred_peer_reset = 1;
                a->deferred_peer_reset_sequence = request_sequence;
                a->deferred_peer_reset_last_tsn = last_tsn;
                a->deferred_peer_reset_stream_count = stream_count;
                if (send_reconfig_response(a, request_sequence, 6u) != 0) return -1;
            } else {
                a->deferred_peer_reset = 1;
                a->deferred_peer_reset_sequence = request_sequence;
                a->deferred_peer_reset_last_tsn = last_tsn;
                a->deferred_peer_reset_stream_count = stream_count;
                if (complete_deferred_peer_reset(a) != 0) return -1;
            }
            progress = 1;
        next_reconfig_parameter: ;
        } else if (type == SCTP_PARAM_RECONFIG_RESPONSE) {
            uint32_t response_sequence, result;
            if (parameter_length != 12u) return -1;
            response_sequence = get32(chunk + offset + 4u);
            result = get32(chunk + offset + 8u);
            if (a->reconfig_sent && response_sequence == a->reconfig_request_sequence) {
                if (result == SCTP_RECONFIG_SUCCESS_NOTHING ||
                    result == SCTP_RECONFIG_SUCCESS_PERFORMED) {
                    if (complete_local_stream_reset(a, result) != 0) return -1;
                } else if (result != 6u) {
                    if (complete_local_stream_reset(a, result) != 0) return -1;
                }
                progress = 1;
            }
        } else {
            /* RFC 6525 parameter types are negotiated explicitly. Unsupported
               request types are not acted upon in this focused stream-reset
               implementation. */
        }
        offset += padded;
    }
    return progress;
}

static int retransmit_reconfig(MlbSctpAssociation *a) {
    int rc;
    if (!a->reconfig_sent || !a->reconfig_packet_length) return 0;
    if (sctp_now_ms() - a->reconfig_sent_ms < a->reconfig_rto_ms) return 0;
    if (a->reconfig_retries >= SCTP_RECONFIG_MAX_RETRIES) {
        set_error(a, "SCTP stream reset retransmission limit exceeded");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    rc = send_sctp_packet(a, a->remote_tag, a->reconfig_packet,
                          a->reconfig_packet_length);
    if (rc != 0) return rc < 0 ? -1 : 0;
    a->reconfig_retries++;
    a->reconfig_sent_ms = sctp_now_ms();
    if (a->reconfig_rto_ms < SCTP_RTO_MAX_MS / 2u) a->reconfig_rto_ms *= 2u;
    else a->reconfig_rto_ms = SCTP_RTO_MAX_MS;
    return 1;
}

static int handle_chunk(MlbSctpAssociation *a, const uint8_t *chunk,
                        size_t chunk_length) {
    switch (chunk[0]) {
        case SCTP_CHUNK_INIT: {
            int reconfig_supported = 0, idata_supported = 0;
            int i_forward_tsn_supported = 0, forward_tsn_supported = 0;
            if (a->state != MLB_SCTP_STATE_LISTENING ||
                chunk_length < SCTP_CHUNK_HEADER + SCTP_INIT_FIXED ||
                parse_init_parameters(chunk, chunk_length, NULL, NULL,
                                      &reconfig_supported, &idata_supported,
                                      &i_forward_tsn_supported, &forward_tsn_supported) != 0) return 0;
            a->peer_reconfig_supported = reconfig_supported;
            a->peer_idata_supported = idata_supported;
            a->peer_i_forward_tsn_supported = i_forward_tsn_supported;
            a->peer_forward_tsn_supported = forward_tsn_supported;
            a->forward_tsn_negotiated = a->local_forward_tsn_enabled && forward_tsn_supported;
            a->idata_negotiated = a->local_interleaving_enabled && idata_supported;
            a->i_forward_tsn_negotiated = a->idata_negotiated &&
                a->local_interleaving_enabled && i_forward_tsn_supported && a->forward_tsn_negotiated;
            a->remote_tag = get32(chunk + 4u);
            if (!a->remote_tag || get16(chunk + 12u) == 0u || get16(chunk + 14u) == 0u)
                return 0;
            a->peer_outbound_streams = get16(chunk + 12u);
            a->peer_inbound_streams = get16(chunk + 14u);
            a->peer_initial_tsn = get32(chunk + 16u);
            refresh_peer_rwnd(a, get32(chunk + 8u), sctp_now_ms());
            a->peer_reconfig_sequence = a->peer_initial_tsn;
            a->cumulative_rx_tsn = a->peer_initial_tsn - 1u;
            a->path_mtu = mlb_dtls_peer_max_message_size(a->dtls);
            if (a->path_mtu > 1200u) a->path_mtu = 1200u;
            if (a->path_mtu < 256u) a->path_mtu = 256u;
            a->congestion_window = a->path_mtu * 4u;
            a->slow_start_threshold = SCTP_DEFAULT_RWND;
            a->cookie_length = SCTP_COOKIE_BYTES;
            if (RAND_bytes(a->cookie, (int)a->cookie_length) != 1) {
                set_error(a, "SCTP state-cookie random generation failed");
                a->state = MLB_SCTP_STATE_FAILED; return -1;
            }
            if (send_init_ack(a) != 0) return -1;
            return 1;
        }
        case SCTP_CHUNK_INIT_ACK: {
            const uint8_t *cookie = NULL; size_t cookie_length = 0;
            int reconfig_supported = 0, idata_supported = 0;
            int i_forward_tsn_supported = 0, forward_tsn_supported = 0;
            if (a->state != MLB_SCTP_STATE_COOKIE_WAIT ||
                chunk_length < SCTP_CHUNK_HEADER + SCTP_INIT_FIXED ||
                parse_init_parameters(chunk, chunk_length, &cookie, &cookie_length,
                                       &reconfig_supported, &idata_supported,
                                       &i_forward_tsn_supported, &forward_tsn_supported) != 0 ||
                !cookie || !cookie_length || cookie_length > sizeof a->cookie) return 0;
            a->peer_reconfig_supported = reconfig_supported;
            a->peer_idata_supported = idata_supported;
            a->peer_i_forward_tsn_supported = i_forward_tsn_supported;
            a->peer_forward_tsn_supported = forward_tsn_supported;
            a->forward_tsn_negotiated = a->local_forward_tsn_enabled && forward_tsn_supported;
            a->idata_negotiated = a->local_interleaving_enabled && idata_supported;
            a->i_forward_tsn_negotiated = a->idata_negotiated &&
                a->local_interleaving_enabled && i_forward_tsn_supported && a->forward_tsn_negotiated;
            a->remote_tag = get32(chunk + 4u);
            if (!a->remote_tag || get16(chunk + 12u) == 0u || get16(chunk + 14u) == 0u)
                return 0;
            a->peer_outbound_streams = get16(chunk + 12u);
            a->peer_inbound_streams = get16(chunk + 14u);
            a->peer_initial_tsn = get32(chunk + 16u);
            refresh_peer_rwnd(a, get32(chunk + 8u), sctp_now_ms());
            a->peer_reconfig_sequence = a->peer_initial_tsn;
            a->cumulative_rx_tsn = a->peer_initial_tsn - 1u;
            a->path_mtu = mlb_dtls_peer_max_message_size(a->dtls);
            if (a->path_mtu > 1200u) a->path_mtu = 1200u;
            if (a->path_mtu < 256u) a->path_mtu = 256u;
            a->congestion_window = a->path_mtu * 4u;
            a->slow_start_threshold = SCTP_DEFAULT_RWND;
            memcpy(a->cookie, cookie, cookie_length);
            a->cookie_length = cookie_length;
            if (send_cookie_echo(a) != 0) return -1;
            a->state = MLB_SCTP_STATE_COOKIE_ECHOED;
            a->timer_started_ms = sctp_now_ms(); a->retransmits = 0;
            return 1;
        }
        case SCTP_CHUNK_COOKIE_ECHO:
            if ((a->state != MLB_SCTP_STATE_LISTENING && a->state != MLB_SCTP_STATE_ESTABLISHED) ||
                chunk_length != 4u + a->cookie_length ||
                CRYPTO_memcmp(chunk + 4u, a->cookie, a->cookie_length) != 0) return 0;
            if (a->state == MLB_SCTP_STATE_LISTENING) {
                a->state = MLB_SCTP_STATE_ESTABLISHED;
                a->cumulative_tx_ack = a->local_initial_tsn - 1u;
            }
            /* Duplicate COOKIE-ECHO retransmission gets another COOKIE-ACK. */
            if (send_cookie_ack(a) != 0) return -1;
            return 1;
        case SCTP_CHUNK_COOKIE_ACK:
            if (a->state != MLB_SCTP_STATE_COOKIE_ECHOED || chunk_length != 4u) return 0;
            a->state = MLB_SCTP_STATE_ESTABLISHED;
            a->cumulative_tx_ack = a->local_initial_tsn - 1u;
            a->timer_started_ms = 0;
            return 1;
        case SCTP_CHUNK_DATA:
            if (a->state != MLB_SCTP_STATE_ESTABLISHED) return 0;
            if (a->idata_negotiated) {
                uint8_t abort_chunk[8] = {SCTP_CHUNK_ABORT, 0, 0, 8, 0, 13, 0, 4};
                (void)send_sctp_packet(a, a->remote_tag, abort_chunk, sizeof abort_chunk);
                set_error(a, "DATA chunk received after I-DATA negotiation");
                a->state = MLB_SCTP_STATE_FAILED;
                return -1;
            }
            if (process_data(a, chunk, chunk_length) != 0) {
                set_error(a, "invalid SCTP DATA chunk or reassembly state");
                a->state = MLB_SCTP_STATE_FAILED;
                return -1;
            }
            return 1;
        case SCTP_CHUNK_I_DATA:
            if (a->state != MLB_SCTP_STATE_ESTABLISHED) return 0;
            if (!a->idata_negotiated) {
                uint8_t abort_chunk[8] = {SCTP_CHUNK_ABORT, 0, 0, 8, 0, 13, 0, 4};
                (void)send_sctp_packet(a, a->remote_tag, abort_chunk, sizeof abort_chunk);
                set_error(a, "I-DATA chunk received without negotiation");
                a->state = MLB_SCTP_STATE_FAILED;
                return -1;
            }
            if (process_data(a, chunk, chunk_length) != 0) {
                set_error(a, "invalid SCTP I-DATA chunk or reassembly state");
                a->state = MLB_SCTP_STATE_FAILED;
                return -1;
            }
            return 1;
        case SCTP_CHUNK_SACK:
            if (a->state != MLB_SCTP_STATE_ESTABLISHED) return 0;
            return process_sack(a, chunk, chunk_length) == 0 ? 1 : 0;
        case SCTP_CHUNK_RE_CONFIG:
            if (a->state != MLB_SCTP_STATE_ESTABLISHED) return 0;
            return process_reconfig(a, chunk, chunk_length);
        case SCTP_CHUNK_FORWARD_TSN:
            if (a->state != MLB_SCTP_STATE_ESTABLISHED) return 0;
            if (a->idata_negotiated || !a->forward_tsn_negotiated) {
                uint8_t abort_chunk[8] = {SCTP_CHUNK_ABORT, 0, 0, 8, 0, 13, 0, 4};
                (void)send_sctp_packet(a, a->remote_tag, abort_chunk, sizeof abort_chunk);
                set_error(a, "FORWARD-TSN received without compatible negotiation");
                a->state = MLB_SCTP_STATE_FAILED;
                return -1;
            }
            return process_forward_tsn(a, chunk, chunk_length, 0) == 0 ? 1 : -1;
        case SCTP_CHUNK_I_FORWARD_TSN:
            if (a->state != MLB_SCTP_STATE_ESTABLISHED) return 0;
            if (!a->idata_negotiated || !a->i_forward_tsn_negotiated) {
                uint8_t abort_chunk[8] = {SCTP_CHUNK_ABORT, 0, 0, 8, 0, 13, 0, 4};
                (void)send_sctp_packet(a, a->remote_tag, abort_chunk, sizeof abort_chunk);
                set_error(a, "I-FORWARD-TSN received without compatible negotiation");
                a->state = MLB_SCTP_STATE_FAILED;
                return -1;
            }
            return process_forward_tsn(a, chunk, chunk_length, 1) == 0 ? 1 : -1;
        case SCTP_CHUNK_ABORT:
            a->state = MLB_SCTP_STATE_FAILED;
            set_error(a, "peer aborted the SCTP association");
            return -1;
        default:
            /* Unsupported chunks are ignored in this experimental slice. */
            return 0;
    }
}

static int handle_packet(MlbSctpAssociation *a, const uint8_t *packet, size_t length) {
    uint16_t src_port, dst_port;
    uint32_t verification_tag;
    size_t offset = SCTP_COMMON_HEADER;
    unsigned chunk_count = 0;
    int progress = 0;
    if (!a || !packet || length < SCTP_COMMON_HEADER + SCTP_CHUNK_HEADER ||
        length > SCTP_PACKET_BUFFER || (length & 3u) != 0u || !verify_sctp_crc(packet, length))
        return 0; /* Invalid SCTP packet is silently discarded. */
    src_port = get16(packet); dst_port = get16(packet + 2u);
    verification_tag = get32(packet + 4u);
    if (src_port != a->remote_port || dst_port != a->local_port) return 0;

    while (offset < length) {
        const uint8_t *chunk = packet + offset;
        size_t remaining = length - offset, chunk_length, padded_chunk;
        int rc;
        if (remaining < SCTP_CHUNK_HEADER) return 0;
        chunk_length = get16(chunk + 2u);
        if (chunk_length < SCTP_CHUNK_HEADER || chunk_length > remaining) return 0;
        padded_chunk = pad4(chunk_length);
        if (padded_chunk > remaining) return 0;

        /* INIT and INIT-ACK cannot be bundled with other chunks. INIT alone
           uses a zero verification tag; all other chunks use our local tag. */
        if (chunk[0] == SCTP_CHUNK_INIT || chunk[0] == SCTP_CHUNK_INIT_ACK) {
            if (chunk_count != 0u || padded_chunk != remaining) return 0;
            if (chunk[0] == SCTP_CHUNK_INIT) {
                if (verification_tag != 0u) return 0;
            } else if (verification_tag != a->local_tag) return 0;
        } else if (verification_tag != a->local_tag) {
            return 0;
        }

        if (chunk_count == 0u) a->packets_received++;
        rc = handle_chunk(a, chunk, chunk_length);
        if (rc < 0 || a->state == MLB_SCTP_STATE_FAILED) return -1;
        if (rc > 0) progress = 1;
        offset += padded_chunk;
        chunk_count++;
    }
    return progress ? 1 : 0;
}

static int retransmit_outstanding(MlbSctpAssociation *a) {
    SctpFlightSlot *oldest = NULL;
    uint64_t now = sctp_now_ms();
    int rc;
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
        SctpFlightSlot *slot = &a->flight[i];
        if (slot->active && !slot->sack_acked && !slot->abandoned &&
            (!oldest || tsn_leq(slot->tsn, oldest->tsn))) oldest = slot;
    }
    if (!oldest) return 0;
    if (oldest->zero_window_probe && a->peer_rwnd == 0u) {
        if (now < a->zero_window_probe_due_ms) return 0;
        rc = mlb_dtls_peer_send(a->dtls, oldest->packet, oldest->length);
        if (rc != 0) return rc < 0 ? -1 : 0;
        oldest->sent_ms = now;
        a->zero_window_probes++;
        if (a->zero_window_probe_rto_ms < SCTP_RTO_MAX_MS / 2u)
            a->zero_window_probe_rto_ms *= 2u;
        else
            a->zero_window_probe_rto_ms = SCTP_RTO_MAX_MS;
        a->zero_window_probe_due_ms = now + a->zero_window_probe_rto_ms;
        return 1; /* Probes do not consume the association error budget. */
    }
    if (oldest->zero_window_probe && a->peer_rwnd > 0u)
        oldest->zero_window_probe = 0;
    if (now - oldest->sent_ms < a->rto_ms) return 0;
    if (oldest->owner && (tx_message_expired(oldest->owner, now) ||
        (oldest->owner->reliability_policy == MLB_SCTP_RELIABILITY_RTX &&
         oldest->retries >= oldest->owner->reliability_value)))
        return abandon_tx_message(a, oldest->owner) < 0 ? -1 : 1;
    if (oldest->retries >= a->max_retransmits) {
        set_error(a, "SCTP DATA retransmission limit exceeded");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    rc = mlb_dtls_peer_send(a->dtls, oldest->packet, oldest->length);
    if (rc != 0) return rc < 0 ? -1 : 0;
    oldest->retries++;
    oldest->sent_ms = now;
    a->rtt_epoch++; /* Do not take RTT samples from retransmitted flight. */
    a->retransmissions++;
    a->slow_start_threshold = a->congestion_window / 2u;
    if (a->slow_start_threshold < 4u * a->path_mtu) a->slow_start_threshold = 4u * a->path_mtu;
    a->congestion_window = a->path_mtu;
    a->partial_bytes_acked = 0u;
    a->timeout_recovery = 1u;
    a->fast_recovery = 0;
    if (a->rto_ms < SCTP_RTO_MAX_MS / 2u) a->rto_ms *= 2u;
    else a->rto_ms = SCTP_RTO_MAX_MS;
    return 1;
}

static int retransmit_handshake(MlbSctpAssociation *a) {
    int rc;
    if (a->state != MLB_SCTP_STATE_COOKIE_WAIT && a->state != MLB_SCTP_STATE_COOKIE_ECHOED)
        return 0;
    if (sctp_now_ms() - a->timer_started_ms < a->rto_ms) return 0;
    if (a->retransmits >= a->max_retransmits) {
        set_error(a, "SCTP association handshake timed out");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    rc = a->state == MLB_SCTP_STATE_COOKIE_WAIT ? send_init(a) : send_cookie_echo(a);
    if (rc != 0) return rc < 0 ? -1 : 0;
    a->retransmits++;
    a->timer_started_ms = sctp_now_ms();
    if (a->rto_ms < SCTP_RTO_MAX_MS / 2u) a->rto_ms *= 2u;
    else a->rto_ms = SCTP_RTO_MAX_MS;
    return 1;
}

int mlb_sctp_available(void) { return mlb_dtls_available(); }
MlbSctpAssociation *mlb_sctp_association_create(const MlbSctpConfig *config) {
    MlbSctpAssociation *a;
    unsigned rto, max_retransmits;
    if (!config || !config->dtls_peer ||
        mlb_dtls_peer_state(config->dtls_peer) != MLB_DTLS_STATE_CONNECTED ||
        !mlb_sctp_available()) return NULL;
    rto = config->initial_rto_ms ? config->initial_rto_ms : 1000u;
    max_retransmits = config->max_retransmits ? config->max_retransmits : SCTP_MAX_RETRANSMITS_DEFAULT;
    if (rto < SCTP_RTO_MIN_MS || rto > SCTP_RTO_MAX_MS ||
        max_retransmits == 0u || max_retransmits > SCTP_MAX_RETRANSMITS_LIMIT ||
        (config->receive_window_bytes &&
         (config->receive_window_bytes < SCTP_MIN_RWND ||
          config->receive_window_bytes > SCTP_DEFAULT_RWND))) return NULL;
    a = (MlbSctpAssociation *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->dtls = config->dtls_peer;
    a->state = config->active ? MLB_SCTP_STATE_CLOSED : MLB_SCTP_STATE_LISTENING;
    a->local_port = config->local_port ? config->local_port : MLB_SCTP_DEFAULT_PORT;
    a->remote_port = config->remote_port ? config->remote_port : MLB_SCTP_DEFAULT_PORT;
    a->outbound_streams = config->outbound_streams ? config->outbound_streams : MLB_SCTP_MAX_STREAMS;
    a->inbound_streams = config->inbound_streams ? config->inbound_streams : MLB_SCTP_MAX_STREAMS;
    a->local_interleaving_enabled = config->enable_interleaving != 0;
    a->local_forward_tsn_enabled = 1; /* PR-SCTP is required for WebRTC data channels. */
    a->rto_ms = rto; a->initial_rto_ms = rto; a->max_retransmits = max_retransmits;
    a->reconfig_rto_ms = rto; a->forward_tsn_rto_ms = rto;
    a->local_rwnd_capacity = config->receive_window_bytes ?
        config->receive_window_bytes : SCTP_DEFAULT_RWND;
    a->peer_rwnd = SCTP_DEFAULT_RWND;
    a->zero_window_probe_rto_ms = rto;
    if (!a->local_port || !a->remote_port ||
        random_u32(&a->local_tag) != 0 || random_u32(&a->local_initial_tsn) != 0) goto fail;
    a->next_tsn = a->local_initial_tsn;
    a->cumulative_tx_ack = a->local_initial_tsn - 1u;
    a->advanced_peer_ack_point = a->local_initial_tsn - 1u;
    a->last_forward_tsn_sent = a->local_initial_tsn - 1u;
    a->local_reconfig_sequence = a->local_initial_tsn;
    a->tx_ssn = (uint16_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint16_t));
    a->rx_ssn = (uint16_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint16_t));
    a->tx_mid_ordered = (uint32_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint32_t));
    a->tx_mid_unordered = (uint32_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint32_t));
    a->rx_mid_ordered = (uint32_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint32_t));
    a->rx_mid_unordered = (uint32_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint32_t));
    a->tx_stream_weight = (uint16_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint16_t));
    a->tx_stream_finish = (uint64_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint64_t));
    a->tx_stream_pending_finish = (uint64_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint64_t));
    a->tx_stream_pending_offset = (uint32_t *)calloc(MLB_SCTP_MAX_STREAMS, sizeof(uint32_t));
    if (!a->tx_ssn || !a->rx_ssn || !a->tx_mid_ordered || !a->tx_mid_unordered ||
        !a->rx_mid_ordered || !a->rx_mid_unordered || !a->tx_stream_weight ||
        !a->tx_stream_finish || !a->tx_stream_pending_finish ||
        !a->tx_stream_pending_offset) goto fail;
    return a;
fail:
    mlb_sctp_association_destroy(a);
    return NULL;
}

void mlb_sctp_association_destroy(MlbSctpAssociation *a) {
    if (!a) return;
    SctpResetEventNode *event;
    free(a->tx_ssn); free(a->rx_ssn);
    free(a->tx_mid_ordered); free(a->tx_mid_unordered);
    free(a->rx_mid_ordered); free(a->rx_mid_unordered);
    free(a->tx_stream_weight); free(a->tx_stream_finish);
    free(a->tx_stream_pending_finish); free(a->tx_stream_pending_offset);
    free_messages(a); clear_rx_fragment(a);
    for (unsigned i = 0; i < SCTP_IDATA_RX_MESSAGE_MAX; i++)
        clear_idata_assembly(&a->rx_idata[i]);
    for (unsigned i = 0; i < SCTP_RX_REORDER_MAX; i++) {
        if (a->rx_reorder[i].chunk) {
            OPENSSL_cleanse(a->rx_reorder[i].chunk, a->rx_reorder[i].length);
            free(a->rx_reorder[i].chunk);
        }
    }
    OPENSSL_cleanse(a->cookie, sizeof a->cookie);
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++)
        OPENSSL_cleanse(a->flight[i].packet, sizeof a->flight[i].packet);
    while ((event = a->reset_event_head) != NULL) {
        a->reset_event_head = event->next;
        free(event);
    }
    OPENSSL_cleanse(a->reconfig_packet, sizeof a->reconfig_packet);
    clear_tx_message(a);
    free(a);
}

int mlb_sctp_association_start(MlbSctpAssociation *a) {
    if (!a || (a->state != MLB_SCTP_STATE_CLOSED && a->state != MLB_SCTP_STATE_LISTENING) ||
        mlb_dtls_peer_state(a->dtls) != MLB_DTLS_STATE_CONNECTED) return -1;
    if (a->state == MLB_SCTP_STATE_LISTENING) return 0;
    if (send_init(a) != 0) return -1;
    a->state = MLB_SCTP_STATE_COOKIE_WAIT;
    a->timer_started_ms = sctp_now_ms(); a->retransmits = 0;
    return 0;
}

int mlb_sctp_association_poll(MlbSctpAssociation *a, unsigned timeout_ms) {
    uint8_t packet[MLB_DTLS_MAX_DATAGRAM];
    size_t length;
    int progress = 0, rc;
    if (!a || a->state == MLB_SCTP_STATE_FAILED || a->state == MLB_SCTP_STATE_SHUTDOWN)
        return -1;
    if (mlb_dtls_peer_state(a->dtls) != MLB_DTLS_STATE_CONNECTED) {
        set_error(a, "underlying DTLS transport is not connected");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    rc = mlb_dtls_peer_poll(a->dtls, timeout_ms > 1000u ? 1000u : timeout_ms);
    if (rc < 0) {
        set_error(a, "DTLS transport poll failed");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    for (unsigned i = 0; i < SCTP_RECV_BURST; i++) {
        length = 0;
        if (mlb_dtls_peer_receive(a->dtls, packet, sizeof packet, &length) != 0) break;
        rc = handle_packet(a, packet, length);
        if (rc < 0 || a->state == MLB_SCTP_STATE_FAILED) return -1;
        if (rc > 0) progress = 1;
    }
    if (a->window_update_pending && a->state == MLB_SCTP_STATE_ESTABLISHED) {
        rc = send_sack(a);
        if (rc == 0) progress = 1;
    }
    rc = retransmit_handshake(a);
    if (rc < 0) return -1;
    if (rc > 0) progress = 1;
    rc = expire_pr_messages(a);
    if (rc < 0) { set_error(a, "could not advance expired PR-SCTP messages"); a->state = MLB_SCTP_STATE_FAILED; return -1; }
    if (rc > 0) progress = 1;
    rc = retransmit_outstanding(a);
    if (rc < 0) return -1;
    if (rc > 0) progress = 1;
    rc = retransmit_forward_tsn(a);
    if (rc < 0) return -1;
    if (rc > 0) progress = 1;
    rc = retransmit_reconfig(a);
    if (rc < 0) return -1;
    if (rc > 0) progress = 1;
    if (a->reconfig_requested && !a->reconfig_sent && !a->tx_head && !a->flight_count) {
        rc = send_reconfig_request_now(a);
        if (rc < 0) { set_error(a, "could not send SCTP stream reset request"); return -1; }
        if (rc == 0) progress = 1;
    }
    if (a->tx_cursor && !a->timeout_recovery && !a->reconfig_sent && !a->reconfig_requested) {
        rc = pump_data_window(a);
        if (rc < 0) { set_error(a, "unable to pump SCTP DATA window"); a->state = MLB_SCTP_STATE_FAILED; return -1; }
        if (rc > 0) progress = 1;
    }
    return progress;
}

int mlb_sctp_association_state(const MlbSctpAssociation *a) {
    return a ? (int)a->state : MLB_SCTP_STATE_SHUTDOWN;
}
size_t mlb_sctp_association_buffered_bytes(const MlbSctpAssociation *a,
                                           uint16_t stream_id) {
    size_t total = 0u;
    if (!a) return 0u;
    for (const SctpTxMessage *message = a->tx_head; message; message = message->next)
        if (message->stream_id == stream_id && !message->abandoned) {
            size_t application_length = (message->ppid == MLB_SCTP_PPID_WEBRTC_STRING_EMPTY ||
                                         message->ppid == MLB_SCTP_PPID_WEBRTC_BINARY_EMPTY) ?
                                        0u : message->length;
            if (application_length > SIZE_MAX - total) return SIZE_MAX;
            total += application_length;
        }
    return total;
}

int mlb_sctp_association_has_pending_data(const MlbSctpAssociation *a,
                                           uint16_t stream_id) {
    if (!a) return 0;
    for (const SctpTxMessage *message = a->tx_head; message; message = message->next)
        if (message->stream_id == stream_id) return 1;
    return 0;
}
int mlb_sctp_association_set_stream_weight(MlbSctpAssociation *a,
                                           uint16_t stream_id, uint16_t weight) {
    if (!a || !weight || stream_id >= a->outbound_streams ||
        stream_id >= MLB_SCTP_MAX_STREAMS) return -1;
    a->tx_stream_weight[stream_id] = weight;
    /* A not-yet-transmitted fragment may have a cached WFQ finish tag.
     * Recompute it under the new weight before the next scheduling decision. */
    a->tx_stream_pending_finish[stream_id] = 0u;
    a->tx_stream_pending_offset[stream_id] = 0u;
    return 0;
}

uint16_t mlb_sctp_association_stream_limit(const MlbSctpAssociation *a) {
    uint16_t local, remote;
    if (!a) return 0;
    local = a->outbound_streams;
    remote = a->peer_inbound_streams;
    return local < remote ? local : remote;
}
uint16_t mlb_sctp_association_inbound_stream_limit(const MlbSctpAssociation *a) {
    uint16_t local, remote;
    if (!a) return 0;
    local = a->inbound_streams;
    remote = a->peer_outbound_streams;
    return local < remote ? local : remote;
}

int mlb_sctp_association_partial_reliability_available(const MlbSctpAssociation *a) {
    if (!a || a->state != MLB_SCTP_STATE_ESTABLISHED || !a->forward_tsn_negotiated) return 0;
    if (a->idata_negotiated && !a->i_forward_tsn_negotiated) return 0;
    return 1;
}

int mlb_sctp_association_send_pr(MlbSctpAssociation *a, uint16_t stream_id,
                                 uint32_t ppid, int unordered,
                                 const void *data, size_t length,
                                 enum MlbSctpReliabilityPolicy policy,
                                 uint32_t max_value) {
    SctpTxMessage *message;
    if (!a || a->state != MLB_SCTP_STATE_ESTABLISHED || !data || !length ||
        stream_id >= a->outbound_streams || stream_id >= a->peer_inbound_streams ||
        length > MLB_SCTP_MAX_MESSAGE || policy < MLB_SCTP_RELIABLE ||
        policy > MLB_SCTP_RELIABILITY_RTX || max_value > 65535u ||
        (policy != MLB_SCTP_RELIABLE && !mlb_sctp_association_partial_reliability_available(a)) ||
        (policy == MLB_SCTP_RELIABLE && max_value != 0u)) return -1;
    if (a->reconfig_requested || a->reconfig_sent) return 1;
    if (a->tx_queue_count >= 256u || length > SCTP_QUEUE_MAX_BYTES -
        (a->tx_queue_bytes > SCTP_QUEUE_MAX_BYTES ? SCTP_QUEUE_MAX_BYTES : a->tx_queue_bytes))
        return 1;
    message = (SctpTxMessage *)malloc(sizeof *message + length);
    if (!message) return -1;
    memset(message, 0, sizeof *message);
    message->stream_id = stream_id;
    message->ppid = ppid;
    message->unordered = unordered != 0;
    message->reliability_policy = policy;
    message->reliability_value = max_value;
    message->enqueued_ms = sctp_now_ms();
    message->length = length;
    memcpy(message->data, data, length);
    if (a->tx_tail) a->tx_tail->next = message;
    else a->tx_head = message;
    a->tx_tail = message;
    if (!a->tx_cursor) a->tx_cursor = message;
    a->tx_queue_bytes += length;
    a->tx_queue_count++;
    a->application_bytes_sent += length;
    if (!a->timeout_recovery && pump_data_window(a) < 0) {
        set_error(a, "failed to send queued SCTP application data");
        a->state = MLB_SCTP_STATE_FAILED;
        return -1;
    }
    return 0;
}

#ifdef MLB_SCTP_TESTING
int mlb_sctp_association_test_abandon_pr_message(MlbSctpAssociation *a,
                                                  uint16_t stream_id) {
    if (!a || a->state != MLB_SCTP_STATE_ESTABLISHED) return -1;
    for (SctpTxMessage *message = a->tx_head; message; message = message->next) {
        if (message->stream_id == stream_id &&
            message->reliability_policy != MLB_SCTP_RELIABLE &&
            message->sequence_assigned && !message->abandoned) {
            return abandon_tx_message(a, message);
        }
    }
    return -1;
}
#endif

int mlb_sctp_association_send(MlbSctpAssociation *a, uint16_t stream_id,
                              uint32_t ppid, int unordered,
                              const void *data, size_t length) {
    return mlb_sctp_association_send_pr(a, stream_id, ppid, unordered, data,
                                        length, MLB_SCTP_RELIABLE, 0u);
}

int mlb_sctp_association_receive(MlbSctpAssociation *a,
                                 uint16_t *stream_id, uint32_t *ppid,
                                 int *unordered, void *data, size_t capacity,
                                 size_t *received_length) {
    struct SctpMessage *m;
    if (!a || !stream_id || !ppid || !unordered || !data || !received_length ||
        !(m = a->queue_head) || m->length > capacity) return -1;
    a->queue_head = m->next;
    if (!a->queue_head) a->queue_tail = NULL;
    a->queue_bytes -= m->length;
    *stream_id = m->stream_id; *ppid = m->ppid; *unordered = m->unordered;
    *received_length = m->length;
    if (m->length) memcpy(data, m->data, m->length);
    OPENSSL_cleanse(m->data, m->length);
    free(m);
    if (a->state == MLB_SCTP_STATE_ESTABLISHED &&
        send_sack(a) != 0) a->window_update_pending = 1;
    return 0;
}

int mlb_sctp_association_get_stats(const MlbSctpAssociation *a, MlbSctpStats *stats) {
    if (!a || !stats) return -1;
    memset(stats, 0, sizeof *stats);
    stats->state = a->state;
    stats->local_verification_tag = a->local_tag;
    stats->remote_verification_tag = a->remote_tag;
    stats->next_tsn = a->next_tsn;
    stats->cumulative_tsn_ack = a->cumulative_tx_ack;
    stats->packets_sent = a->packets_sent;
    stats->packets_received = a->packets_received;
    stats->data_chunks_sent = a->data_chunks_sent;
    stats->data_chunks_received = a->data_chunks_received;
    stats->retransmissions = a->retransmissions;
    stats->application_bytes_sent = a->application_bytes_sent;
    stats->application_bytes_received = a->application_bytes_received;
    stats->congestion_window_bytes = a->congestion_window;
    stats->slow_start_threshold_bytes = a->slow_start_threshold;
    stats->flight_bytes = a->flight_bytes;
    stats->local_rwnd_bytes = local_rx_window_available(a);
    stats->peer_rwnd_bytes = a->peer_rwnd;
    stats->rto_ms = a->rto_ms;
    stats->srtt_ms = a->rtt_initialized ? (unsigned)((a->srtt_scaled8 + 7u) / 8u) : 0u;
    stats->rttvar_ms = a->rtt_initialized ? (unsigned)((a->rttvar_scaled4 + 3u) / 4u) : 0u;
    stats->rtt_samples = a->rtt_samples;
    stats->zero_window_probes = a->zero_window_probes;
    stats->outstanding_chunks = a->flight_count;
    stats->receive_reorder_chunks = a->rx_reorder_count;
    stats->out_of_order_data_chunks_received = a->out_of_order_data_chunks_received;
    stats->duplicate_data_chunks_received = a->duplicate_data_chunks_received;
    stats->gap_sacks_sent = a->gap_sacks_sent;
    stats->gap_sacks_received = a->gap_sacks_received;
    stats->fast_retransmits = a->fast_retransmits;
    stats->reneged_data_chunks_detected = a->reneged_data_chunks_detected;
    stats->i_data_chunks_sent = a->i_data_chunks_sent;
    stats->i_data_chunks_received = a->i_data_chunks_received;
    stats->active_rx_messages = a->rx_idata_count;
    stats->interleaving_negotiated = a->idata_negotiated;
    stats->partial_reliability_negotiated = a->forward_tsn_negotiated;
    stats->i_forward_tsn_negotiated = a->i_forward_tsn_negotiated;
    stats->pr_messages_abandoned = a->pr_messages_abandoned;
    stats->forward_tsn_sent = a->forward_tsn_sent_count;
    stats->forward_tsn_received = a->forward_tsn_received_count;
    return 0;
}

int mlb_sctp_association_request_stream_reset(MlbSctpAssociation *a, uint16_t stream_id) {
    int rc;
    if (!a || a->state != MLB_SCTP_STATE_ESTABLISHED || !a->peer_reconfig_supported ||
        stream_id >= a->outbound_streams || stream_id >= a->peer_inbound_streams)
        return -1;
    if (a->reconfig_requested || a->reconfig_sent)
        return a->reconfig_stream_id == stream_id ? 0 : 1;
    a->reconfig_requested = 1;
    a->reconfig_stream_id = stream_id;
    if (!a->tx_head && !a->flight_count) {
        rc = send_reconfig_request_now(a);
        if (rc < 0) { a->reconfig_requested = 0; return -1; }
    }
    return 0;
}

int mlb_sctp_association_next_reset_event(MlbSctpAssociation *a,
                                          uint16_t *stream_id,
                                          int *direction, int *result) {
    SctpResetEventNode *event;
    if (!a || !stream_id || !direction || !result || !(event = a->reset_event_head)) return 0;
    a->reset_event_head = event->next;
    if (!a->reset_event_head) a->reset_event_tail = NULL;
    if (a->reset_event_count) a->reset_event_count--;
    *stream_id = event->stream_id;
    *direction = event->direction;
    *result = event->result;
    free(event);
    return 1;
}

int mlb_sctp_association_last_error(const MlbSctpAssociation *a, char *out, size_t capacity) {
    if (!a || !out || !capacity || !a->error[0] || strlen(a->error) + 1u > capacity) return -1;
    strcpy(out, a->error);
    return 0;
}
void mlb_sctp_association_abort(MlbSctpAssociation *a) {
    if (!a) return;
    a->state = MLB_SCTP_STATE_SHUTDOWN;
    for (unsigned i = 0; i < SCTP_MAX_FLIGHT; i++) {
        OPENSSL_cleanse(a->flight[i].packet, a->flight[i].length);
        memset(&a->flight[i], 0, sizeof a->flight[i]);
    }
    a->flight_count = 0; a->flight_bytes = 0;
    clear_tx_message(a);
    free_messages(a);
    for (unsigned i = 0; i < SCTP_RX_REORDER_MAX; i++) {
        if (a->rx_reorder[i].chunk) {
            OPENSSL_cleanse(a->rx_reorder[i].chunk, a->rx_reorder[i].length);
            free(a->rx_reorder[i].chunk);
            memset(&a->rx_reorder[i], 0, sizeof a->rx_reorder[i]);
        }
    }
    a->rx_reorder_count = 0u;
    a->rx_reorder_bytes = 0u;
    a->rx_reorder_storage_bytes = 0u;
}
#endif
