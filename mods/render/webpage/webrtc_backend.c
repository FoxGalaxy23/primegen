#define _POSIX_C_SOURCE 200809L
#include "webrtc_backend.h"
#include "webrtc_ice.h"
#include "webrtc_ice_agent.h"
#include "webrtc_dtls.h"
#include "webrtc_sctp.h"
#include "webrtc_datachannel.h"
#include "webrtc_turn.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MLB_RTC_SDP_MAX 16384u
#define MLB_RTC_REMOTE_CANDIDATES MLB_ICE_AGENT_MAX_CANDIDATES
#define MLB_RTC_CHANNELS MLB_DC_MAX_CHANNELS
#define MLB_RTC_EVENT_MAX 512u

typedef struct PendingChannel {
    int handle;
    int stream_id;
    char *label;
    char *protocol;
    int ordered;
    uint16_t priority;
    int negotiated;
    int requested_id;
    int has_lifetime;
    unsigned lifetime;
    int has_retransmits;
    unsigned retransmits;
    struct PendingChannel *next;
} PendingChannel;

struct MlbWebRtcPeer {
    int id;
    struct MlbWebRtcPeer *next;
    MlbIceAgent *ice;
    MlbDtlsPeer *dtls;
    MlbSctpAssociation *sctp;
    MlbDataChannelManager *channels;
    MlbTurnClient *turn; /* Owned by the peer; ICE agent must be destroyed first. */
    char local_ufrag[MLB_ICE_MAX_UFRAG];
    char local_password[MLB_ICE_MAX_PASSWORD];
    char remote_ufrag[MLB_ICE_MAX_UFRAG];
    char remote_password[MLB_ICE_MAX_PASSWORD];
    char local_fingerprint[128];
    char remote_fingerprint[128];
    char stun_host[256];
    uint16_t stun_port;
    char remote_candidates[MLB_RTC_REMOTE_CANDIDATES][MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
    size_t remote_candidate_count;
    int relay_only;
    int gathered;
    int local_set;
    int remote_set;
    int local_type;     /* 1 offer, 2 answer */
    int remote_type;    /* 1 offer, 2 answer */
    int local_dtls_role;
    int remote_setup;
    int ice_started;
    int dtls_started;
    int sctp_started;
    int state;
    int ice_state;
    int gathering_state;
    int signaling_state;
    int next_handle;
    PendingChannel *pending_channels;
    MlbWebRtcEvent *event_head;
    MlbWebRtcEvent *event_tail;
    unsigned event_count;
    size_t event_bytes;
    char error[256];
};

#if defined(_WIN32) || !defined(HAVE_OPENSSL_VAULT)
int mlb_webrtc_available(void) { return 0; }
MlbWebRtcPeer *mlb_webrtc_peer_create(const char *s, int r) { (void)s; (void)r; return NULL; }
int mlb_webrtc_peer_id(const MlbWebRtcPeer *p) { (void)p; return -1; }
MlbWebRtcPeer *mlb_webrtc_peer_next(MlbWebRtcPeer *p) { return p ? p->next : NULL; }
int mlb_webrtc_set_local_description(MlbWebRtcPeer *p, const char *t) { (void)p; (void)t; return -1; }
int mlb_webrtc_set_remote_description(MlbWebRtcPeer *p, const char *s, const char *t) { (void)p; (void)s; (void)t; return -1; }
int mlb_webrtc_add_candidate(MlbWebRtcPeer *p, const char *c, const char *m) { (void)p; (void)c; (void)m; return -1; }
int mlb_webrtc_create_data_channel(MlbWebRtcPeer *p, const char *l, const char *pr, int o, int n, int id, int hl, unsigned lft, int hr, unsigned r, unsigned priority) { (void)p;(void)l;(void)pr;(void)o;(void)n;(void)id;(void)hl;(void)lft;(void)hr;(void)r;(void)priority;return -1; }
int mlb_webrtc_channel_send(MlbWebRtcPeer *p, int id, const void *d, size_t n, int b) { (void)p;(void)id;(void)d;(void)n;(void)b;return -1; }
int mlb_webrtc_channel_close(MlbWebRtcPeer *p, int id) { (void)p;(void)id;return -1; }
int mlb_webrtc_channel_buffered_amount(MlbWebRtcPeer *p, int id) { (void)p;(void)id;return 0; }
int mlb_webrtc_channel_stream(MlbWebRtcPeer *p, int id) { (void)p;(void)id;return -1; }
int mlb_webrtc_channel_is_open(MlbWebRtcPeer *p, int id) { (void)p;(void)id;return 0; }
MlbWebRtcEvent *mlb_webrtc_next_event(MlbWebRtcPeer *p) { (void)p;return NULL; }
void mlb_webrtc_event_free(MlbWebRtcEvent *e) { if(e){free(e->text);free(e->mid);free(e);} }
void mlb_webrtc_peer_destroy(MlbWebRtcPeer *p) { free(p); }
#else
#include <openssl/crypto.h>
#include <sys/socket.h>

static int next_peer_id = 1;
static int next_channel_handle = 1;
static uint64_t sdp_session_id(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (uint64_t)time(NULL);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static char *dup_string(const char *s) {
    size_t n = s ? strlen(s) : 0u;
    char *p = (char *)malloc(n + 1u);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}
static void set_error(MlbWebRtcPeer *p, const char *s) {
    if (p && s) snprintf(p->error, sizeof p->error, "%s", s);
}
static int emit_event(MlbWebRtcPeer *p, int kind, int id, int state,
                      const void *text, size_t length, const char *mid, int binary) {
    MlbWebRtcEvent *e;
    if (!p || p->event_count >= MLB_RTC_EVENT_MAX || length > MLB_RTC_SDP_MAX) return -1;
    e = (MlbWebRtcEvent *)calloc(1, sizeof *e);
    if (!e) return -1;
    e->kind = kind; e->id = id; e->state = state; e->binary = binary; e->ordered = 1;
    if (text || length) {
        e->text = (char *)malloc(length + 1u);
        if (!e->text) goto fail;
        if (length) memcpy(e->text, text, length);
        e->text[length] = '\0'; e->length = length;
    }
    if (mid && !(e->mid = dup_string(mid))) goto fail;
    if (p->event_tail) p->event_tail->next = e; else p->event_head = e;
    p->event_tail = e; p->event_count++;
    p->event_bytes += length;
    return 0;
fail:
    mlb_webrtc_event_free(e); return -1;
}
static void set_connection_state(MlbWebRtcPeer *p, int state) {
    if (p && p->state != state) {
        p->state = state;
        (void)emit_event(p, MLB_RTC_EVENT_CONNECTION_STATE, p->id, state, NULL, 0, NULL, 0);
    }
}
static void set_ice_state(MlbWebRtcPeer *p, int state) {
    if (p && p->ice_state != state) {
        p->ice_state = state;
        (void)emit_event(p, MLB_RTC_EVENT_ICE_STATE, p->id, state, NULL, 0, NULL, 0);
    }
}
static void set_signaling_state(MlbWebRtcPeer *p, int state) {
    if (p && p->signaling_state != state) {
        p->signaling_state = state;
        (void)emit_event(p, MLB_RTC_EVENT_SIGNALING_STATE, p->id, state, NULL, 0, NULL, 0);
    }
}

/* RTCIceServer URLs are passed by the JS shim as one URL per line.  The
 * native peer currently supports one STUN server and one TURN/UDP server.
 * TURN credentials are carried in URI userinfo and percent-decoded here.
 * turns:, STUNS, TURN/TCP and TURN/TLS are rejected rather than silently
 * downgraded. */
typedef struct ParsedIceServers {
    char stun_host[256];
    uint16_t stun_port;
    char turn_host[256];
    uint16_t turn_port;
    char turn_username[257];
    char turn_password[257];
    int have_stun;
    int have_turn;
} ParsedIceServers;

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int uri_decode_component(const char *src, size_t length, char *dst, size_t capacity) {
    size_t out = 0;
    if (!src || !dst || !capacity) return -1;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            int hi, lo;
            if (length - i < 3u || (hi = hex_value((unsigned char)src[i + 1])) < 0 ||
                (lo = hex_value((unsigned char)src[i + 2])) < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            i += 2u;
        }
        /* TURN's current credential implementation deliberately accepts
           printable ASCII only; control characters and NUL are invalid. */
        if (c < 0x21u || c > 0x7eu || out + 1u >= capacity) return -1;
        dst[out++] = (char)c;
    }
    if (!out) return -1;
    dst[out] = '\0';
    return 0;
}

static int parse_ice_authority(char *authority, char *host, size_t host_capacity,
                              uint16_t *port_out, uint16_t default_port) {
    char *port_text = NULL, *end = NULL;
    unsigned long port = default_port;
    char *hostname = authority;
    if (!authority || !*authority || !host || !host_capacity || !port_out) return -1;
    if (*hostname == '[') {
        char *close = strchr(hostname, ']');
        if (!close) return -1;
        *close = '\0'; hostname++;
        if (close[1] == ':') port_text = close + 2;
        else if (close[1] != '\0') return -1;
    } else {
        char *first = strchr(hostname, ':');
        char *last = strrchr(hostname, ':');
        if (first && first == last) { *last = '\0'; port_text = last + 1; }
        else if (first && first != last) return -1; /* IPv6 literals must be bracketed. */
    }
    if (port_text) {
        if (!*port_text) return -1;
        errno = 0; port = strtoul(port_text, &end, 10);
        if (errno || !end || *end || port == 0 || port > 65535u) return -1;
    }
    if (!*hostname || strlen(hostname) >= host_capacity || strchr(hostname, '@') ||
        strchr(hostname, '/') || strchr(hostname, '\\')) return -1;
    memcpy(host, hostname, strlen(hostname) + 1u);
    *port_out = (uint16_t)port;
    return 0;
}

static int parse_ice_servers(const char *servers, ParsedIceServers *out, int relay_only) {
    char *copy, *save = NULL, *line;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->stun_port = 3478u; out->turn_port = 3478u;
    if (!servers || !*servers) return relay_only ? -1 : 0;
    copy = dup_string(servers); if (!copy) return -1;
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *url = line, *query, *authority;
        if (!*url) continue;
        size_t line_len = strlen(url);
        while (line_len && url[line_len - 1u] == '\r') url[--line_len] = '\0';
        if (!line_len) continue;
        query = strchr(url, '?');
        if (query) {
            *query++ = '\0';
            if (strcasecmp(query, "transport=udp") != 0) { free(copy); return -1; }
        }
        if (strncasecmp(url, "stun:", 5u) == 0) {
            if (out->have_stun || strchr(url + 5u, '@')) { free(copy); return -1; }
            authority = url + 5u;
            if (strncmp(authority, "//", 2u) == 0) authority += 2u;
            if (parse_ice_authority(authority, out->stun_host, sizeof out->stun_host,
                                    &out->stun_port, 3478u) != 0) { free(copy); return -1; }
            out->have_stun = 1;
        } else if (strncasecmp(url, "turn:", 5u) == 0) {
            char *at, *separator;
            if (out->have_turn) { free(copy); return -1; }
            authority = url + 5u;
            if (strncmp(authority, "//", 2u) == 0) authority += 2u;
            at = strchr(authority, '@');
            if (!at || strchr(at + 1u, '@')) { free(copy); return -1; }
            *at = '\0';
            separator = strchr(authority, ':');
            if (!separator || uri_decode_component(authority, (size_t)(separator - authority),
                    out->turn_username, sizeof out->turn_username) != 0 ||
                uri_decode_component(separator + 1u, strlen(separator + 1u),
                    out->turn_password, sizeof out->turn_password) != 0 ||
                parse_ice_authority(at + 1u, out->turn_host, sizeof out->turn_host,
                    &out->turn_port, 3478u) != 0) { free(copy); return -1; }
            out->have_turn = 1;
        } else {
            /* This backend does not offer TLS or TCP fallback. */
            free(copy); return -1;
        }
    }
    free(copy);
    if (relay_only && !out->have_turn) return -1;
    return 0;
}

int mlb_webrtc_available(void) {
    return mlb_ice_crypto_available() && mlb_dtls_available() &&
           mlb_sctp_available() && mlb_datachannel_available();
}

MlbWebRtcPeer *mlb_webrtc_peer_create(const char *servers, int relay_only) {
    MlbWebRtcPeer *p;
    MlbIceAgentConfig ice_config;
    MlbDtlsConfig dtls_config;
    ParsedIceServers parsed_servers;
    char placeholder_ufrag[MLB_ICE_MAX_UFRAG], placeholder_password[MLB_ICE_MAX_PASSWORD];
    if (!mlb_webrtc_available()) return NULL;
    p = (MlbWebRtcPeer *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->id = __sync_fetch_and_add(&next_peer_id, 1);
    p->next_handle = 1; p->state = 0; p->ice_state = 0; p->gathering_state = 0; p->signaling_state = 0;
    p->relay_only = relay_only != 0;
    if (parse_ice_servers(servers, &parsed_servers, p->relay_only) != 0 ||
        mlb_ice_generate_credentials(p->local_ufrag, sizeof p->local_ufrag,
                                     p->local_password, sizeof p->local_password) != 0 ||
        mlb_ice_generate_credentials(placeholder_ufrag, sizeof placeholder_ufrag,
                                     placeholder_password, sizeof placeholder_password) != 0)
        goto fail;
    if (parsed_servers.have_stun) {
        snprintf(p->stun_host, sizeof p->stun_host, "%s", parsed_servers.stun_host);
        p->stun_port = parsed_servers.stun_port;
    }
    if (parsed_servers.have_turn) {
        MlbTurnConfig turn_config;
        memset(&turn_config, 0, sizeof turn_config);
        turn_config.server_host = parsed_servers.turn_host;
        turn_config.server_port = parsed_servers.turn_port;
        turn_config.username = parsed_servers.turn_username;
        turn_config.password = parsed_servers.turn_password;
        turn_config.timeout_ms = 1500u;
        turn_config.requested_lifetime = MLB_TURN_DEFAULT_LIFETIME;
        p->turn = mlb_turn_client_create(&turn_config);
        OPENSSL_cleanse(parsed_servers.turn_password, sizeof parsed_servers.turn_password);
        if (!p->turn) goto fail;
    }
    snprintf(p->remote_ufrag, sizeof p->remote_ufrag, "%s", placeholder_ufrag);
    snprintf(p->remote_password, sizeof p->remote_password, "%s", placeholder_password);
    memset(&ice_config, 0, sizeof ice_config);
    ice_config.local_ufrag = p->local_ufrag; ice_config.local_password = p->local_password;
    ice_config.remote_ufrag = p->remote_ufrag; ice_config.remote_password = p->remote_password;
    ice_config.role = MLB_ICE_ROLE_CONTROLLING;
    p->ice = mlb_ice_agent_create(&ice_config);
    if (!p->ice) goto fail;
    memset(&dtls_config, 0, sizeof dtls_config);
    dtls_config.ice_agent = p->ice; dtls_config.role = MLB_DTLS_ROLE_CLIENT;
    dtls_config.mtu = MLB_DTLS_DEFAULT_MTU; dtls_config.handshake_timeout_ms = 30000u;
    p->dtls = mlb_dtls_peer_create(&dtls_config);
    if (!p->dtls || mlb_dtls_peer_local_fingerprint(p->dtls, p->local_fingerprint,
                                                     sizeof p->local_fingerprint) != 0) goto fail;
    OPENSSL_cleanse(&parsed_servers, sizeof parsed_servers);
    return p;
fail:
    OPENSSL_cleanse(&parsed_servers, sizeof parsed_servers);
    mlb_webrtc_peer_destroy(p); return NULL;
}

int mlb_webrtc_peer_id(const MlbWebRtcPeer *p) { return p ? p->id : -1; }
MlbWebRtcPeer *mlb_webrtc_peer_next(MlbWebRtcPeer *p) { return p ? p->next : NULL; }

static int ensure_gathered(MlbWebRtcPeer *p) {
    int candidates = 0;
    if (p->gathered) return 0;
    p->gathering_state = 1;
    (void)emit_event(p, MLB_RTC_EVENT_GATHERING_STATE, p->id, 1, NULL, 0, NULL, 0);
    if (!p->relay_only) {
        candidates = mlb_ice_agent_gather(p->ice, p->stun_host[0] ? p->stun_host : NULL,
                                          p->stun_port, 700u);
        if (candidates <= 0) {
            p->gathering_state = 2;
            set_error(p, "ICE gathering produced no usable UDP host candidates");
            return -1;
        }
    }
    if (p->turn && mlb_ice_agent_add_turn_candidate(p->ice, p->turn) < 0) {
        p->gathering_state = 2;
        set_error(p, "TURN allocation failed or no usable relay candidate was produced");
        return -1;
    }
    candidates = (int)mlb_ice_agent_local_candidate_count(p->ice);
    if (candidates <= 0) {
        p->gathering_state = 2;
        set_error(p, p->relay_only ? "relay-only policy requires a usable TURN/UDP allocation" :
                                     "ICE gathering produced no usable candidates");
        return -1;
    }
    p->gathered = 1; p->gathering_state = 2;
    for (size_t i = 0; i < mlb_ice_agent_local_candidate_count(p->ice); i++) {
        char candidate[MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
        if (mlb_ice_agent_local_candidate(p->ice, i, candidate, sizeof candidate) == 0)
            (void)emit_event(p, MLB_RTC_EVENT_CANDIDATE, p->id, 0, candidate, strlen(candidate), "0", 0);
    }
    (void)emit_event(p, MLB_RTC_EVENT_GATHERING_STATE, p->id, 2, NULL, 0, NULL, 0);
    return 0;
}

static int append_sdp(char *out, size_t cap, size_t *used, const char *fmt, ...) {
    va_list ap; int n;
    if (!out || !used || *used >= cap) return -1;
    va_start(ap, fmt); n = vsnprintf(out + *used, cap - *used, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) return -1;
    *used += (size_t)n; return 0;
}

int mlb_webrtc_set_local_description(MlbWebRtcPeer *p, const char *type) {
    char sdp[MLB_RTC_SDP_MAX]; size_t used = 0;
    int offer, ice_role;
    if (!p || !type || p->state == 5 ||
        (strcmp(type, "offer") != 0 && strcmp(type, "answer") != 0)) return -1;
    offer = strcmp(type, "offer") == 0;
    if (offer && (p->local_set || p->remote_set)) return -1;
    if (!offer && (!p->remote_set || p->remote_type != 1 || p->local_set)) return -1;
    if (ensure_gathered(p) != 0) return -1;
    ice_role = offer ? MLB_ICE_ROLE_CONTROLLING : MLB_ICE_ROLE_CONTROLLED;
    if (mlb_ice_agent_set_role(p->ice, ice_role) != 0) return -1;
    p->local_dtls_role = offer ? MLB_DTLS_ROLE_SERVER : MLB_DTLS_ROLE_CLIENT;
    if (mlb_dtls_peer_set_role(p->dtls, (enum MlbDtlsRole)p->local_dtls_role) != 0) return -1;
    if (p->remote_fingerprint[0] && mlb_dtls_peer_set_remote_fingerprint(p->dtls, p->remote_fingerprint) != 0) return -1;
    if (append_sdp(sdp, sizeof sdp, &used,
        "v=0\r\no=- %llu 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=ice-options:trickle\r\n"
        "a=fingerprint:%s\r\na=setup:%s\r\na=mid:0\r\n"
        "a=sctp-port:%u\r\na=max-message-size:%u\r\n",
        (unsigned long long)sdp_session_id(), p->local_ufrag, p->local_password,
        p->local_fingerprint, offer ? "actpass" : "active",
        MLB_SCTP_DEFAULT_PORT, MLB_SCTP_MAX_MESSAGE) != 0) return -1;
    for (size_t i = 0; i < mlb_ice_agent_local_candidate_count(p->ice); i++) {
        char candidate[MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
        if (mlb_ice_agent_local_candidate(p->ice, i, candidate, sizeof candidate) != 0 ||
            append_sdp(sdp, sizeof sdp, &used, "a=%s\r\n", candidate) != 0) return -1;
    }
    if (append_sdp(sdp, sizeof sdp, &used, "a=end-of-candidates\r\n") != 0) return -1;
    p->local_set = 1; p->local_type = offer ? 1 : 2;
    set_signaling_state(p, offer ? 1 : 0);
    if (emit_event(p, MLB_RTC_EVENT_DESCRIPTION, p->id, 0, sdp, used, type, 0) != 0) return -1;
    return 0;
}

typedef struct ParsedSdp {
    char ufrag[MLB_ICE_MAX_UFRAG];
    char password[MLB_ICE_MAX_PASSWORD];
    char fingerprint[128];
    char setup[16];
    char candidates[MLB_RTC_REMOTE_CANDIDATES][MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
    size_t candidate_count;
    uint16_t sctp_port;
    int application_media;
    int has_mid;
    int has_ufrag, has_password, has_fingerprint, has_setup;
} ParsedSdp;
static int copy_attr(char *dst, size_t cap, const char *value) {
    size_t n = strlen(value);
    if (!n || n >= cap) return -1;
    memcpy(dst, value, n + 1u); return 0;
}
static int parse_remote_sdp(const char *sdp, ParsedSdp *out) {
    char *copy, *save = NULL, *line;
    int version = 0, media_count = 0;
    if (!sdp || !out || strlen(sdp) > MLB_RTC_SDP_MAX - 1u) return -1;
    memset(out, 0, sizeof *out); out->sctp_port = MLB_SCTP_DEFAULT_PORT;
    copy = dup_string(sdp); if (!copy) return -1;
    for (line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strcmp(line, "v=0") == 0) { version = 1; continue; }
        if (strncmp(line, "m=", 2u) == 0) {
            media_count++;
            if (strncmp(line, "m=application ", 14u) != 0 ||
                !strstr(line, " UDP/DTLS/SCTP webrtc-datachannel")) goto fail;
            out->application_media = 1; continue;
        }
        if (strncmp(line, "a=ice-ufrag:", 12u) == 0) {
            if (out->has_ufrag || copy_attr(out->ufrag, sizeof out->ufrag, line + 12u) != 0) goto fail;
            out->has_ufrag = 1;
        } else if (strncmp(line, "a=ice-pwd:", 10u) == 0) {
            if (out->has_password || copy_attr(out->password, sizeof out->password, line + 10u) != 0) goto fail;
            out->has_password = 1;
        } else if (strncmp(line, "a=fingerprint:", 14u) == 0) {
            if (out->has_fingerprint || copy_attr(out->fingerprint, sizeof out->fingerprint, line + 14u) != 0 ||
                strncasecmp(out->fingerprint, "sha-256 ", 8u) != 0) goto fail;
            out->has_fingerprint = 1;
        } else if (strncmp(line, "a=setup:", 8u) == 0) {
            if (out->has_setup || copy_attr(out->setup, sizeof out->setup, line + 8u) != 0) goto fail;
            out->has_setup = 1;
        } else if (strncmp(line, "a=mid:", 6u) == 0) {
            if (strcmp(line + 6u, "0") != 0) goto fail;
            out->has_mid = 1;
        } else if (strncmp(line, "a=sctp-port:", 12u) == 0) {
            char *end = NULL; unsigned long n = strtoul(line + 12u, &end, 10);
            if (!end || *end || n == 0 || n > 65535u) goto fail;
            out->sctp_port = (uint16_t)n;
        } else if (strncmp(line, "a=candidate:", 12u) == 0) {
            if (out->candidate_count >= MLB_RTC_REMOTE_CANDIDATES ||
                copy_attr(out->candidates[out->candidate_count], sizeof out->candidates[0], line + 2u) != 0) goto fail;
            out->candidate_count++;
        }
    }
    free(copy);
    if (!version || media_count != 1 || !out->application_media || !out->has_mid ||
        !out->has_ufrag || !out->has_password || !out->has_fingerprint || !out->has_setup ||
        strncasecmp(out->fingerprint, "sha-256 ", 8u) != 0 ||
        (strcmp(out->setup, "actpass") && strcmp(out->setup, "active") && strcmp(out->setup, "passive"))) return -1;
    return 0;
fail:
    free(copy); return -1;
}

int mlb_webrtc_set_remote_description(MlbWebRtcPeer *p, const char *sdp, const char *type) {
    ParsedSdp parsed;
    int offer;
    if (!p || !type || (strcmp(type, "offer") && strcmp(type, "answer")) ||
        parse_remote_sdp(sdp, &parsed) != 0) return -1;
    offer = strcmp(type, "offer") == 0;
    if (p->remote_set || (offer && p->local_set) || (!offer && (!p->local_set || p->local_type != 1)) ||
        (offer && strcmp(parsed.setup, "actpass") != 0) ||
        (!offer && strcmp(parsed.setup, "actpass") == 0)) return -1;
    if (mlb_ice_agent_set_remote_credentials(p->ice, parsed.ufrag, parsed.password) != 0) return -1;
    if (mlb_ice_agent_set_role(p->ice, offer ? MLB_ICE_ROLE_CONTROLLED : MLB_ICE_ROLE_CONTROLLING) != 0) return -1;
    snprintf(p->remote_ufrag, sizeof p->remote_ufrag, "%s", parsed.ufrag);
    snprintf(p->remote_password, sizeof p->remote_password, "%s", parsed.password);
    snprintf(p->remote_fingerprint, sizeof p->remote_fingerprint, "%s", parsed.fingerprint);
    p->remote_setup = strcmp(parsed.setup, "active") == 0 ? 1 : (strcmp(parsed.setup, "passive") == 0 ? 2 : 0);
    for (size_t i = 0; i < parsed.candidate_count; i++) {
        int duplicate = 0;
        for (size_t j = 0; j < p->remote_candidate_count; j++)
            if (strcmp(p->remote_candidates[j], parsed.candidates[i]) == 0) { duplicate = 1; break; }
        if (!duplicate) {
            if (p->remote_candidate_count >= MLB_RTC_REMOTE_CANDIDATES) return -1;
            snprintf(p->remote_candidates[p->remote_candidate_count++],
                     sizeof p->remote_candidates[0], "%s", parsed.candidates[i]);
        }
    }
    p->remote_set = 1; p->remote_type = offer ? 1 : 2;
    set_signaling_state(p, offer ? 2 : 0);
    if (!offer) {
        /* The answer chooses the DTLS role. The peer-generated answers use active. */
        if (strcmp(parsed.setup, "active") == 0) p->local_dtls_role = MLB_DTLS_ROLE_SERVER;
        else if (strcmp(parsed.setup, "passive") == 0) p->local_dtls_role = MLB_DTLS_ROLE_CLIENT;
        else return -1;
        if (mlb_dtls_peer_set_role(p->dtls, (enum MlbDtlsRole)p->local_dtls_role) != 0) return -1;
    } else {
        /* This endpoint will answer with setup:active and therefore be DTLS client. */
        p->local_dtls_role = MLB_DTLS_ROLE_CLIENT;
        if (mlb_dtls_peer_set_role(p->dtls, MLB_DTLS_ROLE_CLIENT) != 0) return -1;
    }
    if (mlb_dtls_peer_set_remote_fingerprint(p->dtls, p->remote_fingerprint) != 0) return -1;
    return 0;
}

int mlb_webrtc_add_candidate(MlbWebRtcPeer *p, const char *candidate, const char *mid) {
    MlbIceCandidate parsed;
    if (!p || !candidate || (mid && *mid && strcmp(mid, "0") != 0)) return -1;
    if (!*candidate) return 0; /* end-of-candidates marker */
    if (mlb_ice_parse_candidate(candidate, &parsed) != 0) return -1;
    for (size_t i = 0; i < p->remote_candidate_count; i++)
        if (strcmp(p->remote_candidates[i], candidate) == 0) return 0;
    if (p->remote_candidate_count >= MLB_RTC_REMOTE_CANDIDATES) return -1;
    if (p->ice_started) {
        if (mlb_ice_agent_add_remote_candidate(p->ice, candidate) != 0) return -1;
    }
    snprintf(p->remote_candidates[p->remote_candidate_count++],
             sizeof p->remote_candidates[0], "%s", candidate);
    return 0;
}

static PendingChannel *find_handle(MlbWebRtcPeer *p, int handle) {
    for (PendingChannel *c = p ? p->pending_channels : NULL; c; c = c->next)
        if (c->handle == handle) return c;
    return NULL;
}
static PendingChannel *find_stream(MlbWebRtcPeer *p, int stream) {
    for (PendingChannel *c = p ? p->pending_channels : NULL; c; c = c->next)
        if (c->stream_id == stream) return c;
    return NULL;
}
static int materialize_channels(MlbWebRtcPeer *p) {
    MlbDataChannelConfig config;
    if (!p || !p->sctp || p->channels || mlb_sctp_association_state(p->sctp) != MLB_SCTP_STATE_ESTABLISHED) return 0;
    memset(&config, 0, sizeof config); config.association = p->sctp;
    config.role = p->local_dtls_role == MLB_DTLS_ROLE_CLIENT ? MLB_DC_ROLE_DTLS_CLIENT : MLB_DC_ROLE_DTLS_SERVER;
    config.max_channels = MLB_DC_MAX_CHANNELS; config.max_buffered_bytes = MLB_DC_MAX_BUFFERED_BYTES;
    p->channels = mlb_datachannel_manager_create(&config);
    if (!p->channels) { set_error(p, "could not create DataChannel manager after SCTP association"); return -1; }
    for (PendingChannel *c = p->pending_channels; c; c = c->next) if (c->stream_id < 0) {
        c->stream_id = mlb_datachannel_create(p->channels, c->label, c->protocol, c->ordered,
            c->negotiated, c->requested_id, c->has_lifetime, c->lifetime,
            c->has_retransmits, c->retransmits, c->priority);
        if (c->stream_id < 0) { set_error(p, mlb_datachannel_last_error(p->channels)); return -1; }
    }
    return 0;
}

int mlb_webrtc_create_data_channel(MlbWebRtcPeer *p, const char *label, const char *protocol,
                                   int ordered, int negotiated, int id,
                                   int has_lifetime, unsigned lifetime,
                                   int has_retransmits, unsigned retransmits,
                                   unsigned priority) {
    PendingChannel *c;
    if (!p || !label || !protocol || strlen(label) > MLB_DC_MAX_LABEL_BYTES ||
        strlen(protocol) > MLB_DC_MAX_PROTOCOL_BYTES || p->state >= 5 ||
        (has_lifetime && has_retransmits) ||
        (!has_lifetime && lifetime != 0u) || (!has_retransmits && retransmits != 0u) ||
        lifetime > 65535u || retransmits > 65535u ||
        priority == 0u || priority > UINT16_MAX ||
        (negotiated && (id < 0 || id > 65534)) || (!negotiated && id >= 0)) return -1;
    for (PendingChannel *it = p->pending_channels; it; it = it->next) {
        if (negotiated && it->negotiated && it->requested_id == id) return -1;
    }
    c = (PendingChannel *)calloc(1, sizeof *c); if (!c) return -1;
    c->handle = __sync_fetch_and_add(&next_channel_handle, 1); if (c->handle <= 0) c->handle = __sync_fetch_and_add(&next_channel_handle, 1);
    c->stream_id = -1; c->label = dup_string(label); c->protocol = dup_string(protocol);
    c->ordered = ordered != 0; c->priority = (uint16_t)priority;
    c->negotiated = negotiated != 0; c->requested_id = negotiated ? id : -1;
    c->has_lifetime = has_lifetime; c->lifetime = lifetime; c->has_retransmits = has_retransmits; c->retransmits = retransmits;
    if (!c->label || !c->protocol) { free(c->label); free(c->protocol); free(c); return -1; }
    c->next = p->pending_channels; p->pending_channels = c;
    if (p->channels) {
        c->stream_id = mlb_datachannel_create(p->channels, c->label, c->protocol, c->ordered,
            c->negotiated, c->requested_id, c->has_lifetime, c->lifetime,
            c->has_retransmits, c->retransmits, c->priority);
        if (c->stream_id < 0) { p->pending_channels = c->next; free(c->label); free(c->protocol); free(c); return -1; }
    }
    return c->handle;
}

static void drain_channel_events(MlbWebRtcPeer *p) {
    MlbDataChannelEvent *e;
    if (!p || !p->channels) return;
    while ((e = mlb_datachannel_next_event(p->channels)) != NULL) {
        PendingChannel *c = find_stream(p, (int)e->id);
        if (e->kind == MLB_DC_EVENT_DATA_CHANNEL) {
            if (!c) {
                c = (PendingChannel *)calloc(1, sizeof *c);
                if (c) {
                    c->handle = __sync_fetch_and_add(&next_channel_handle, 1); if (c->handle <= 0) c->handle = __sync_fetch_and_add(&next_channel_handle, 1); c->stream_id = e->id;
                    c->label = dup_string(e->label ? e->label : "");
                    c->protocol = dup_string(e->protocol ? e->protocol : "");
                    c->ordered = e->ordered; c->priority = e->priority;
                    c->has_lifetime = e->has_max_packet_lifetime;
                    c->lifetime = e->max_packet_lifetime;
                    c->has_retransmits = e->has_max_retransmits;
                    c->retransmits = e->max_retransmits;
                    c->negotiated = 0; c->next = p->pending_channels; p->pending_channels = c;
                }
            }
            if (c && emit_event(p, MLB_RTC_EVENT_DATA_CHANNEL, c->handle, 0,
                                c->label, c->label ? strlen(c->label) : 0,
                                c->protocol ? c->protocol : "", 0) == 0) {
                p->event_tail->ordered = e->ordered;
                p->event_tail->priority = e->priority;
                p->event_tail->has_max_packet_lifetime = e->has_max_packet_lifetime;
                p->event_tail->max_packet_lifetime = e->max_packet_lifetime;
                p->event_tail->has_max_retransmits = e->has_max_retransmits;
                p->event_tail->max_retransmits = e->max_retransmits;
            }
        } else if (e->kind == MLB_DC_EVENT_OPEN) {
            if (c) (void)emit_event(p, MLB_RTC_EVENT_CHANNEL_OPEN, c->handle, 1, NULL, 0, NULL, 0);
        } else if (e->kind == MLB_DC_EVENT_MESSAGE) {
            if (c) (void)emit_event(p, MLB_RTC_EVENT_MESSAGE, c->handle, 0, e->data, e->length, NULL, e->binary);
        } else if (e->kind == MLB_DC_EVENT_CLOSE) {
            if (c) (void)emit_event(p, MLB_RTC_EVENT_CHANNEL_CLOSED, c->handle,
                                   MLB_DC_STATE_CLOSED, NULL, 0, NULL, 0);
        } else if (e->kind == MLB_DC_EVENT_ERROR) {
            (void)emit_event(p, MLB_RTC_EVENT_CHANNEL_ERROR, c ? c->handle : p->id,
                             0, "DataChannel protocol error", 26u, NULL, 0);
        }
        mlb_datachannel_event_free(e);
    }
}

static void start_transports(MlbWebRtcPeer *p) {
    if (!p || !p->local_set || !p->remote_set || p->ice_started) return;
    if (!p->gathered || !p->remote_candidate_count) return;
    for (size_t i = 0; i < p->remote_candidate_count; i++)
        if (mlb_ice_agent_add_remote_candidate(p->ice, p->remote_candidates[i]) != 0) {
            set_error(p, "invalid remote ICE candidate from SDP"); set_connection_state(p, 4); return;
        }
    if (mlb_ice_agent_start(p->ice) < 0) { set_error(p, "could not start ICE checklist"); set_connection_state(p, 4); return; }
    p->ice_started = 1; set_connection_state(p, 1); set_ice_state(p, 1);
}

static void drive_peer(MlbWebRtcPeer *p) {
    int rc, ice_state;
    if (!p || p->state == 5 || p->state == 4) return;
    start_transports(p);
    if (!p->ice_started) return;
    rc = mlb_ice_agent_poll(p->ice, 0);
    if (rc < 0) { set_error(p, "ICE Agent failed"); set_ice_state(p, 4); set_connection_state(p, 4); return; }
    ice_state = mlb_ice_agent_state(p->ice);
    if (ice_state == MLB_ICE_AGENT_CONNECTED) {
        /* Once the checklist is completed, do not regress to "connected" on
         * every subsequent poll tick. ICE completed is a stable terminal
         * success state for this single-component data-channel transport. */
        if (p->ice_state < 3) set_ice_state(p, 2);
        if (!p->dtls_started) {
            if (mlb_dtls_peer_start(p->dtls) != 0) { set_error(p, "DTLS could not start after ICE nomination"); set_connection_state(p, 4); return; }
            p->dtls_started = 1;
        }
    } else if (ice_state == MLB_ICE_AGENT_FAILED) {
        set_ice_state(p, 4); set_connection_state(p, 4); return;
    }
    if (p->dtls_started && !p->sctp) {
        rc = mlb_dtls_peer_poll(p->dtls, 0);
        if (rc < 0 || mlb_dtls_peer_state(p->dtls) == MLB_DTLS_STATE_FAILED) {
            set_error(p, "DTLS handshake failed"); set_connection_state(p, 4); return;
        }
        if (mlb_dtls_peer_state(p->dtls) == MLB_DTLS_STATE_CONNECTED) {
            MlbSctpConfig config;
            memset(&config, 0, sizeof config); config.dtls_peer = p->dtls;
            config.active = p->local_dtls_role == MLB_DTLS_ROLE_CLIENT;
            config.local_port = MLB_SCTP_DEFAULT_PORT; config.remote_port = MLB_SCTP_DEFAULT_PORT;
            /* WebRTC DataChannels require RFC 8260 message interleaving. */
            config.enable_interleaving = 1;
            p->sctp = mlb_sctp_association_create(&config);
            if (!p->sctp || mlb_sctp_association_start(p->sctp) != 0) {
                set_error(p, "could not start SCTP association"); set_connection_state(p, 4); return;
            }
            p->sctp_started = 1;
        }
    } else if (p->sctp && !p->channels) {
        rc = mlb_sctp_association_poll(p->sctp, 0);
        if (rc < 0) { set_error(p, "SCTP association failed"); set_connection_state(p, 4); return; }
        if (mlb_sctp_association_state(p->sctp) == MLB_SCTP_STATE_ESTABLISHED) {
            if (materialize_channels(p) != 0) { set_connection_state(p, 4); return; }
            set_connection_state(p, 2); set_ice_state(p, 3);
        }
    } else if (p->channels) {
        rc = mlb_datachannel_poll(p->channels, 0);
        if (rc < 0) {
            (void)emit_event(p, MLB_RTC_EVENT_CHANNEL_ERROR, p->id, 0,
                             mlb_datachannel_last_error(p->channels),
                             strlen(mlb_datachannel_last_error(p->channels)), NULL, 0);
            set_connection_state(p, 4); return;
        }
        drain_channel_events(p);
    }
}

int mlb_webrtc_channel_send(MlbWebRtcPeer *p, int channel_id, const void *data, size_t length, int binary) {
    PendingChannel *c = find_handle(p, channel_id);
    return (!p || !p->channels || !c || c->stream_id < 0) ? -1 :
        mlb_datachannel_send(p->channels, (uint16_t)c->stream_id, data, length, binary);
}
int mlb_webrtc_channel_close(MlbWebRtcPeer *p, int channel_id) {
    PendingChannel *c = find_handle(p, channel_id);
    return (!p || !p->channels || !c || c->stream_id < 0) ? -1 :
        mlb_datachannel_close(p->channels, (uint16_t)c->stream_id);
}
int mlb_webrtc_channel_buffered_amount(MlbWebRtcPeer *p, int channel_id) {
    PendingChannel *c = find_handle(p, channel_id);
    size_t n = (!p || !p->channels || !c || c->stream_id < 0) ? 0u :
        mlb_datachannel_buffered_amount(p->channels, (uint16_t)c->stream_id);
    return n > 2147483647u ? 2147483647 : (int)n;
}
int mlb_webrtc_channel_stream(MlbWebRtcPeer *p, int channel_id) {
    PendingChannel *c = find_handle(p, channel_id); return c ? c->stream_id : -1;
}
int mlb_webrtc_channel_is_open(MlbWebRtcPeer *p, int channel_id) {
    PendingChannel *c = find_handle(p, channel_id);
    return p && p->channels && c && c->stream_id >= 0 &&
        mlb_datachannel_id_is_open(p->channels, (uint16_t)c->stream_id);
}
MlbWebRtcEvent *mlb_webrtc_next_event(MlbWebRtcPeer *p) {
    MlbWebRtcEvent *e;
    if (!p) return NULL;
    drive_peer(p);
    if (!(e = p->event_head)) return NULL;
    p->event_head = e->next; if (!p->event_head) p->event_tail = NULL;
    e->next = NULL; if (p->event_count) p->event_count--;
    p->event_bytes = p->event_bytes >= e->length ? p->event_bytes - e->length : 0u;
    return e;
}
void mlb_webrtc_event_free(MlbWebRtcEvent *e) { if (e) { free(e->text); free(e->mid); free(e); } }
void mlb_webrtc_peer_destroy(MlbWebRtcPeer *p) {
    PendingChannel *c; MlbWebRtcEvent *e;
    if (!p) return;
    while ((c = p->pending_channels) != NULL) { p->pending_channels = c->next; free(c->label); free(c->protocol); free(c); }
    while ((e = p->event_head) != NULL) { p->event_head = e->next; mlb_webrtc_event_free(e); }
    mlb_datachannel_manager_destroy(p->channels);
    mlb_sctp_association_destroy(p->sctp);
    mlb_dtls_peer_destroy(p->dtls);
    mlb_ice_agent_destroy(p->ice);
    mlb_turn_client_destroy(p->turn);
    OPENSSL_cleanse(p->local_password, sizeof p->local_password);
    OPENSSL_cleanse(p->remote_password, sizeof p->remote_password);
    free(p);
}
#endif
