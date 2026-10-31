#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "webrtc_ice_agent.h"
#include "webrtc_turn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
MlbIceAgent *mlb_ice_agent_create(const MlbIceAgentConfig *c) { (void)c; return NULL; }
void mlb_ice_agent_destroy(MlbIceAgent *a) { (void)a; }
int mlb_ice_agent_gather(MlbIceAgent *a, const char *h, uint16_t p, unsigned t) { (void)a; (void)h; (void)p; (void)t; return -1; }
int mlb_ice_agent_add_turn_candidate(MlbIceAgent *a, MlbTurnClient *t) { (void)a; (void)t; return -1; }
size_t mlb_ice_agent_local_candidate_count(const MlbIceAgent *a) { (void)a; return 0; }
int mlb_ice_agent_local_candidate(const MlbIceAgent *a, size_t i, char *o, size_t c) { (void)a; (void)i; (void)o; (void)c; return -1; }
int mlb_ice_agent_set_remote_credentials(MlbIceAgent *a, const char *u, const char *p) { (void)a; (void)u; (void)p; return -1; }
int mlb_ice_agent_set_role(MlbIceAgent *a, int r) { (void)a; (void)r; return -1; }
int mlb_ice_agent_add_remote_candidate(MlbIceAgent *a, const char *c) { (void)a; (void)c; return -1; }
int mlb_ice_agent_start(MlbIceAgent *a) { (void)a; return -1; }
int mlb_ice_agent_restart(MlbIceAgent *a, const MlbIceAgentConfig *c) { (void)a; (void)c; return -1; }
int mlb_ice_agent_get_stats(const MlbIceAgent *a, MlbIceAgentStats *s) { (void)a; (void)s; return -1; }
int mlb_ice_agent_poll(MlbIceAgent *a, unsigned t) { (void)a; (void)t; return -1; }
int mlb_ice_agent_state(const MlbIceAgent *a) { (void)a; return MLB_ICE_AGENT_CLOSED; }
int mlb_ice_agent_role(const MlbIceAgent *a) { (void)a; return -1; }
int mlb_ice_agent_selected_pair(const MlbIceAgent *a, char *l, size_t lc, char *r, size_t rc) { (void)a; (void)l; (void)lc; (void)r; (void)rc; return -1; }
int mlb_ice_agent_send_datagram(MlbIceAgent *a, const void *d, size_t n) { (void)a; (void)d; (void)n; return -1; }
int mlb_ice_agent_receive_datagram(MlbIceAgent *a, void *d, size_t c, size_t *n) { (void)a; (void)d; (void)c; (void)n; return -1; }
#else
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#ifdef HAVE_OPENSSL_VAULT
#include <openssl/rand.h>
#endif

#define ICE_LOCAL_MAX 32u
#define ICE_REMOTE_MAX MLB_ICE_AGENT_MAX_CANDIDATES
#define ICE_PAIR_MAX (ICE_LOCAL_MAX * ICE_REMOTE_MAX)
#define ICE_QUEUE_MAX_BYTES (1024u * 1024u)
#define ICE_STUN_MAX 4096u
#define ICE_RETRANSMIT_LIMIT 7u
#define ICE_INITIAL_RTO_MS 500u
#define ICE_PAIR_PACING_MS 50u
#define ICE_CONSENT_INTERVAL_MIN_MS 4000u
#define ICE_CONSENT_INTERVAL_MAX_MS 6000u
#define ICE_CONSENT_EXPIRY_MS 30000u
#define ICE_CONSENT_TX_SLOTS 8u
#define ICE_TRIGGERED_QUEUE_MAX ICE_PAIR_MAX

enum PairState { PAIR_FROZEN = 0, PAIR_WAITING, PAIR_IN_PROGRESS, PAIR_SUCCEEDED, PAIR_FAILED };

typedef struct LocalCandidate {
    MlbIceCandidate candidate;
    char sdp[MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
    struct sockaddr_storage address;
    socklen_t address_length;
    int fd;
    MlbTurnClient *turn; /* Non-NULL when fd is owned by a TURN client. */
} LocalCandidate;

typedef struct RemoteCandidate {
    MlbIceCandidate candidate;
    char sdp[MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
    struct sockaddr_storage address;
    socklen_t address_length;
} RemoteCandidate;

typedef struct CandidatePair {
    unsigned local_index;
    unsigned remote_index;
    uint64_t priority;
    enum PairState state;
    uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE];
    unsigned retransmits;
    unsigned rto_ms;
    uint64_t deadline_ms;
    int has_transaction;
    int nominated_check;
    int nominated;
    int valid;
    int triggered_use_candidate;
    int triggered_queued;
    char foundation[MLB_ICE_MAX_FOUNDATION * 2u + 2u];
} CandidatePair;

typedef struct QueuedDatagram {
    struct QueuedDatagram *next;
    size_t length;
    uint8_t data[];
} QueuedDatagram;

typedef struct ConsentTransaction {
    uint8_t transaction_id[MLB_STUN_TRANSACTION_ID_SIZE];
    uint64_t created_ms;
    int active;
} ConsentTransaction;

struct MlbIceAgent {
    char local_ufrag[MLB_ICE_MAX_UFRAG];
    char local_password[MLB_ICE_MAX_PASSWORD];
    char remote_ufrag[MLB_ICE_MAX_UFRAG];
    char remote_password[MLB_ICE_MAX_PASSWORD];
    int role;
    uint64_t tie_breaker;
    int state;
    LocalCandidate local[ICE_LOCAL_MAX];
    size_t local_count;
    RemoteCandidate remote[ICE_REMOTE_MAX];
    size_t remote_count;
    CandidatePair pairs[ICE_PAIR_MAX];
    size_t pair_count;
    uint64_t next_pair_ms;
    unsigned triggered_queue[ICE_TRIGGERED_QUEUE_MAX];
    size_t triggered_head;
    size_t triggered_count;
    uint64_t triggered_checks_sent;
    int selected_pair;
    int nomination_pair;
    uint64_t last_consent_response_ms;
    uint64_t next_consent_ms;
    ConsentTransaction consent_transactions[ICE_CONSENT_TX_SLOTS];
    QueuedDatagram *queue_head;
    QueuedDatagram *queue_tail;
    size_t queued_bytes;
};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static int random_bytes(uint8_t *out, size_t n) {
#ifdef HAVE_OPENSSL_VAULT
    return n <= 0x7fffffffU && RAND_bytes(out, (int)n) == 1 ? 0 : -1;
#else
    (void)out; (void)n; return -1;
#endif
}
static int valid_credential(const char *s, size_t min_len, size_t max_len) {
    size_t n;
    if (!s || !(n = strnlen(s, max_len + 1u)) || n < min_len || n > max_len) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f) return 0;
    return 1;
}
static int sockaddr_port(const struct sockaddr_storage *ss, uint16_t *port) {
    if (ss->ss_family == AF_INET) { *port = ntohs(((const struct sockaddr_in *)ss)->sin_port); return 0; }
    if (ss->ss_family == AF_INET6) { *port = ntohs(((const struct sockaddr_in6 *)ss)->sin6_port); return 0; }
    return -1;
}
static int sockaddr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a, *y = (const struct sockaddr_in *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a, *y = (const struct sockaddr_in6 *)b;
        return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
    }
    return 0;
}
static int sockaddr_to_mapped(const struct sockaddr_storage *ss, MlbStunMappedAddress *mapped) {
    memset(mapped, 0, sizeof *mapped);
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        mapped->family = AF_INET; mapped->port = ntohs(a->sin_port);
        return inet_ntop(AF_INET, &a->sin_addr, mapped->address, sizeof mapped->address) ? 0 : -1;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        mapped->family = AF_INET6; mapped->port = ntohs(a->sin6_port);
        return inet_ntop(AF_INET6, &a->sin6_addr, mapped->address, sizeof mapped->address) ? 0 : -1;
    }
    return -1;
}
static uint32_t hash_foundation(const char *s, unsigned salt) {
    uint32_t h = 2166136261u ^ salt;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h ? h : 1u;
}
static uint32_t candidate_priority(unsigned type_pref, unsigned local_pref) {
    return (type_pref << 24) | ((local_pref & 0xffffu) << 8) | 255u;
}
static int append_local(MlbIceAgent *a, int fd, const struct sockaddr_storage *addr,
                        socklen_t addrlen, unsigned local_pref) {
    LocalCandidate *l;
    char ip[MLB_ICE_MAX_ADDRESS + 1u];
    uint16_t port;
    char foundation[MLB_ICE_MAX_FOUNDATION + 1u];
    int family = addr->ss_family;
    const void *src;
    if (a->local_count >= ICE_LOCAL_MAX || sockaddr_port(addr, &port) != 0) return -1;
    src = family == AF_INET ? (const void *)&((const struct sockaddr_in *)addr)->sin_addr :
                              (const void *)&((const struct sockaddr_in6 *)addr)->sin6_addr;
    if (!inet_ntop(family, src, ip, sizeof ip)) return -1;
    snprintf(foundation, sizeof foundation, "%u", hash_foundation(ip, MLB_ICE_CANDIDATE_HOST));
    l = &a->local[a->local_count]; memset(l, 0, sizeof *l);
    l->fd = fd; l->address = *addr; l->address_length = addrlen;
    strcpy(l->candidate.foundation, foundation);
    l->candidate.component = 1; l->candidate.transport = MLB_ICE_UDP;
    l->candidate.priority = candidate_priority(126u, local_pref);
    strcpy(l->candidate.address, ip); l->candidate.port = port; l->candidate.type = MLB_ICE_CANDIDATE_HOST;
    int n = snprintf(l->sdp, sizeof l->sdp, "candidate:%s 1 UDP %u %s %u typ host",
                     foundation, l->candidate.priority, ip, (unsigned)port);
    if (n < 0 || (size_t)n >= sizeof l->sdp) { l->fd = -1; return -1; }
    a->local_count++;
    return 0;
}
static int is_unspecified_or_multicast(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        uint32_t v = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
        return v == INADDR_ANY || (v >= 0xe0000000u && v <= 0xefffffffu);
    }
    if (sa->sa_family == AF_INET6) {
        const struct in6_addr *v = &((const struct sockaddr_in6 *)sa)->sin6_addr;
        return IN6_IS_ADDR_UNSPECIFIED(v) || IN6_IS_ADDR_MULTICAST(v) || IN6_IS_ADDR_LINKLOCAL(v);
    }
    return 1;
}
static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}
static int add_host_socket(MlbIceAgent *a, const struct sockaddr *source, socklen_t source_len,
                           unsigned local_pref) {
    struct sockaddr_storage bind_addr, actual;
    socklen_t actual_len = sizeof actual;
    int fd, yes = 1;
    memset(&bind_addr, 0, sizeof bind_addr);
    if (source_len > sizeof bind_addr) return -1;
    memcpy(&bind_addr, source, source_len);
    if (source->sa_family == AF_INET) ((struct sockaddr_in *)&bind_addr)->sin_port = 0;
    else if (source->sa_family == AF_INET6) ((struct sockaddr_in6 *)&bind_addr)->sin6_port = 0;
    else return -1;
    fd = socket(source->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (source->sa_family == AF_INET6) (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof yes);
    if (make_nonblocking(fd) != 0 || bind(fd, (struct sockaddr *)&bind_addr, source_len) != 0 ||
        getsockname(fd, (struct sockaddr *)&actual, &actual_len) != 0 ||
        append_local(a, fd, &actual, actual_len, local_pref) != 0) {
        close(fd); return -1;
    }
    return 0;
}
static int gather_hosts(MlbIceAgent *a) {
    struct ifaddrs *ifas = NULL;
    unsigned pref = 65535u;
    if (getifaddrs(&ifas) != 0) return -1;
    for (struct ifaddrs *it = ifas; it && a->local_count < ICE_LOCAL_MAX; it = it->ifa_next) {
        if (!it->ifa_addr || !(it->ifa_flags & IFF_UP) || is_unspecified_or_multicast(it->ifa_addr)) continue;
        if (it->ifa_addr->sa_family != AF_INET && it->ifa_addr->sa_family != AF_INET6) continue;
        int duplicate = 0;
        for (size_t i = 0; i < a->local_count; i++) {
            if (a->local[i].address.ss_family != it->ifa_addr->sa_family) continue;
            struct sockaddr_storage tmp; memset(&tmp, 0, sizeof tmp);
            socklen_t len = it->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
            memcpy(&tmp, it->ifa_addr, len);
            if (it->ifa_addr->sa_family == AF_INET) ((struct sockaddr_in *)&tmp)->sin_port = 0;
            else ((struct sockaddr_in6 *)&tmp)->sin6_port = 0;
            if (sockaddr_equal(&a->local[i].address, &tmp)) { duplicate = 1; break; }
            /* Candidate sockets use an ephemeral port, so compare the address only. */
            if (it->ifa_addr->sa_family == AF_INET &&
                ((struct sockaddr_in *)&a->local[i].address)->sin_addr.s_addr == ((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr) duplicate = 1;
            if (it->ifa_addr->sa_family == AF_INET6 &&
                memcmp(&((struct sockaddr_in6 *)&a->local[i].address)->sin6_addr,
                       &((struct sockaddr_in6 *)it->ifa_addr)->sin6_addr, sizeof(struct in6_addr)) == 0) duplicate = 1;
        }
        if (!duplicate) {
            socklen_t len = it->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
            (void)add_host_socket(a, it->ifa_addr, len, pref);
            if (pref > 256u) pref -= 256u;
        }
    }
    freeifaddrs(ifas);
    return a->local_count ? 0 : -1;
}
static int random_transaction_id(uint8_t tid[12]) { return random_bytes(tid, 12); }

static int addr_from_candidate(const MlbIceCandidate *c, struct sockaddr_storage *out,
                               socklen_t *out_len) {
    struct addrinfo hints, *res = NULL;
    char service[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP;
    snprintf(service, sizeof service, "%u", (unsigned)c->port);
    if (getaddrinfo(c->address, service, &hints, &res) != 0 || !res) return -1;
    if (res->ai_addrlen > sizeof *out) { freeaddrinfo(res); return -1; }
    memset(out, 0, sizeof *out); memcpy(out, res->ai_addr, res->ai_addrlen); *out_len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res); return 0;
}
static uint64_t pair_priority(uint32_t local, uint32_t remote, int role) {
    uint64_t g = role == MLB_ICE_ROLE_CONTROLLING ? local : remote;
    uint64_t d = role == MLB_ICE_ROLE_CONTROLLING ? remote : local;
    uint64_t mn = g < d ? g : d, mx = g > d ? g : d;
    return (mn << 32) + (mx << 1) + (g > d ? 1u : 0u);
}
static int pair_exists(const MlbIceAgent *a, unsigned li, const struct sockaddr_storage *remote) {
    for (size_t i = 0; i < a->pair_count; i++)
        if (a->pairs[i].local_index == li && sockaddr_equal(&a->remote[a->pairs[i].remote_index].address, remote)) return (int)i;
    return -1;
}
static int add_pair(MlbIceAgent *a, unsigned li, unsigned ri) {
    CandidatePair *p;
    if (li >= a->local_count || ri >= a->remote_count) return -1;
    if (a->pair_count >= ICE_PAIR_MAX) return -1;
    if (pair_exists(a, li, &a->remote[ri].address) >= 0) return 0;
    p = &a->pairs[a->pair_count++]; memset(p, 0, sizeof *p);
    p->local_index = li; p->remote_index = ri; p->state = PAIR_FROZEN;
    p->priority = pair_priority(a->local[li].candidate.priority, a->remote[ri].candidate.priority, a->role);
    {
        char foundation[MLB_ICE_MAX_FOUNDATION * 2u + 2u];
        int n = snprintf(foundation, sizeof foundation, "%s|%s",
                         a->local[li].candidate.foundation, a->remote[ri].candidate.foundation);
        if (n < 0 || (size_t)n >= sizeof foundation) { a->pair_count--; return -1; }
        memcpy(p->foundation, foundation, (size_t)n + 1u);
    }
    p->rto_ms = ICE_INITIAL_RTO_MS;
    return 0;
}

static int enqueue_triggered_check(MlbIceAgent *a, size_t pair_index) {
    CandidatePair *p;
    size_t tail;
    if (pair_index >= a->pair_count) return -1;
    p = &a->pairs[pair_index];
    if (p->state == PAIR_SUCCEEDED || p->triggered_queued) return 0;
    if (a->triggered_count >= ICE_TRIGGERED_QUEUE_MAX) return -1;
    tail = (a->triggered_head + a->triggered_count) % ICE_TRIGGERED_QUEUE_MAX;
    a->triggered_queue[tail] = (unsigned)pair_index;
    a->triggered_count++;
    p->triggered_queued = 1;
    p->state = PAIR_WAITING;
    return 0;
}

static int dequeue_triggered_check(MlbIceAgent *a) {
    unsigned index;
    CandidatePair *p;
    if (!a->triggered_count) return -1;
    index = a->triggered_queue[a->triggered_head];
    a->triggered_head = (a->triggered_head + 1u) % ICE_TRIGGERED_QUEUE_MAX;
    a->triggered_count--;
    if (index >= a->pair_count) return -1;
    p = &a->pairs[index];
    p->triggered_queued = 0;
    return (int)index;
}

static void initialize_pair_states(MlbIceAgent *a) {
    for (size_t i = 0; i < a->pair_count; i++) a->pairs[i].state = PAIR_FROZEN;
    /* Pair array is priority ordered. Unfreeze exactly one pair per pair
       foundation initially, as required by RFC 8445 section 6.1.2.6. */
    for (size_t i = 0; i < a->pair_count; i++) {
        int earlier = 0;
        for (size_t j = 0; j < i; j++)
            if (strcmp(a->pairs[i].foundation, a->pairs[j].foundation) == 0) { earlier = 1; break; }
        if (!earlier) a->pairs[i].state = PAIR_WAITING;
    }
}

static int unfreeze_available_foundations(MlbIceAgent *a) {
    int unfrozen = 0;
    for (size_t i = 0; i < a->pair_count; i++) {
        CandidatePair *p = &a->pairs[i];
        int active = 0, best = (int)i;
        if (p->state != PAIR_FROZEN) continue;
        /* Foundations may be revisited after a role change has recalculated
           pair priorities. Pick the highest-priority frozen pair, not the
           first array entry (the original order may no longer be sorted). */
        for (size_t j = 0; j < a->pair_count; j++) {
            CandidatePair *same = &a->pairs[j];
            if (strcmp(p->foundation, same->foundation) != 0) continue;
            if (same->state == PAIR_WAITING || same->state == PAIR_IN_PROGRESS) {
                active = 1;
                break;
            }
            if (same->state == PAIR_FROZEN && same->priority > a->pairs[best].priority)
                best = (int)j;
        }
        if (!active && best == (int)i) { p->state = PAIR_WAITING; unfrozen++; }
    }
    return unfrozen;
}

static int highest_priority_waiting_pair(const MlbIceAgent *a) {
    int best = -1;
    for (size_t i = 0; i < a->pair_count; i++)
        if (a->pairs[i].state == PAIR_WAITING &&
            (best < 0 || a->pairs[i].priority > a->pairs[best].priority)) best = (int)i;
    return best;
}

static int pair_index_for_transaction(MlbIceAgent *a, const uint8_t tid[12],
                                      const struct sockaddr_storage *from, unsigned local_index) {
    for (size_t i = 0; i < a->pair_count; i++) {
        CandidatePair *p = &a->pairs[i];
        if (p->has_transaction && a->local[p->local_index].fd == a->local[local_index].fd &&
            sockaddr_equal(&a->remote[p->remote_index].address, from) &&
            memcmp(p->transaction_id, tid, 12) == 0) return (int)i;
    }
    return -1;
}

static int socket_send(MlbIceAgent *a, unsigned li, const void *buf, size_t len,
                       const struct sockaddr_storage *to, socklen_t to_len) {
    if (!a || li >= a->local_count || !buf || !len || !to) return -1;
    if (a->local[li].turn) {
        MlbStunMappedAddress peer;
        if (sockaddr_to_mapped(to, &peer) != 0 || peer.port == 0) return -1;
        return mlb_turn_send_to(a->local[li].turn, peer.address, peer.port, buf, len);
    }
    ssize_t n = sendto(a->local[li].fd, buf, len, 0, (const struct sockaddr *)to, to_len);
    return n == (ssize_t)len ? 0 : -1;
}
static int send_check(MlbIceAgent *a, size_t pi, int nominate, uint64_t now) {
    CandidatePair *p = &a->pairs[pi];
    char username[MLB_ICE_MAX_UFRAG * 2u + 2u];
    uint8_t packet[ICE_STUN_MAX];
    size_t packet_length = 0;
    if (snprintf(username, sizeof username, "%s:%s", a->remote_ufrag, a->local_ufrag) >= (int)sizeof username ||
        random_transaction_id(p->transaction_id) != 0 ||
        mlb_ice_build_check_request(packet, sizeof packet, p->transaction_id, username,
            a->remote_password, a->local[p->local_index].candidate.priority,
            a->tie_breaker, a->role, nominate, &packet_length) != 0 ||
        socket_send(a, p->local_index, packet, packet_length,
                    &a->remote[p->remote_index].address, a->remote[p->remote_index].address_length) != 0)
        return -1;
    p->state = PAIR_IN_PROGRESS; p->has_transaction = 1;
    p->nominated_check = nominate; p->retransmits = 0; p->rto_ms = ICE_INITIAL_RTO_MS;
    p->deadline_ms = now + p->rto_ms;
    return 0;
}
static int candidate_from_sockaddr(const struct sockaddr_storage *ss, MlbIceCandidate *c) {
    MlbStunMappedAddress mapped;
    char identity[MLB_ICE_MAX_ADDRESS + 16u];
    if (sockaddr_to_mapped(ss, &mapped) != 0) return -1;
    memset(c, 0, sizeof *c);
    snprintf(identity, sizeof identity, "%s:%u", mapped.address, (unsigned)mapped.port);
    snprintf(c->foundation, sizeof c->foundation, "%u",
             hash_foundation(identity, MLB_ICE_CANDIDATE_PRFLX));
    c->component = 1;
    c->transport = MLB_ICE_UDP; c->priority = 1; strcpy(c->address, mapped.address);
    c->port = mapped.port; c->type = MLB_ICE_CANDIDATE_PRFLX;
    return 0;
}
static int add_peer_reflexive(MlbIceAgent *a, unsigned li, const struct sockaddr_storage *from,
                              socklen_t from_len, uint32_t priority) {
    RemoteCandidate *r;
    if (a->remote_count >= ICE_REMOTE_MAX) return -1;
    for (size_t i = 0; i < a->remote_count; i++)
        if (sockaddr_equal(&a->remote[i].address, from)) return (int)i;
    r = &a->remote[a->remote_count]; memset(r, 0, sizeof *r);
    if (candidate_from_sockaddr(from, &r->candidate) != 0) return -1;
    /* Foundations for discovered peer-reflexive candidates must not alias a
       different candidate in the same generation. */
    for (unsigned salt = 1; ; salt++) {
        int collision = 0;
        for (size_t i = 0; i < a->remote_count; i++)
            if (strcmp(a->remote[i].candidate.foundation, r->candidate.foundation) == 0) { collision = 1; break; }
        if (!collision) break;
        char identity[MLB_ICE_MAX_ADDRESS + 16u];
        snprintf(identity, sizeof identity, "%s:%u:%u", r->candidate.address,
                 (unsigned)r->candidate.port, salt);
        snprintf(r->candidate.foundation, sizeof r->candidate.foundation, "%u",
                 hash_foundation(identity, MLB_ICE_CANDIDATE_PRFLX));
    }
    r->candidate.priority = priority ? priority : 1;
    r->address = *from; r->address_length = from_len;
    {
        char sdp[MLB_ICE_MAX_CANDIDATE_TEXT + 1u];
        int n = snprintf(sdp, sizeof sdp, "candidate:%s 1 UDP %u %s %u typ prflx",
                         r->candidate.foundation, r->candidate.priority, r->candidate.address,
                         (unsigned)r->candidate.port);
        if (n < 0 || (size_t)n >= sizeof sdp) return -1;
        memcpy(r->sdp, sdp, (size_t)n + 1u);
    }
    (void)li;
    return (int)a->remote_count++;
}
static int local_candidate_matches_mapped(const LocalCandidate *local,
                                          const LocalCandidate *base,
                                          const MlbStunMappedAddress *mapped) {
    if (local->fd != base->fd || local->candidate.port != mapped->port) return 0;
    if (mapped->family == AF_INET) {
        struct in_addr addr;
        return inet_pton(AF_INET, local->candidate.address, &addr) == 1 &&
               strcmp(local->candidate.address, mapped->address) == 0;
    }
    if (mapped->family == AF_INET6) {
        struct in6_addr addr;
        return inet_pton(AF_INET6, local->candidate.address, &addr) == 1 &&
               strcmp(local->candidate.address, mapped->address) == 0;
    }
    return 0;
}

static int add_local_peer_reflexive(MlbIceAgent *a, unsigned base_index,
                                    const MlbStunMappedAddress *mapped,
                                    unsigned *result_index) {
    LocalCandidate *base, *local;
    MlbStunMappedAddress related;
    char foundation[MLB_ICE_MAX_FOUNDATION + 1u];
    int n;
    if (!a || base_index >= a->local_count || !mapped || !result_index) return -1;
    for (size_t i = 0; i < a->local_count; i++) {
        if (local_candidate_matches_mapped(&a->local[i], &a->local[base_index], mapped)) {
            *result_index = (unsigned)i;
            return 0;
        }
    }
    if (a->local_count >= ICE_LOCAL_MAX) return -1;
    base = &a->local[base_index];
    if (sockaddr_to_mapped(&base->address, &related) != 0) return -1;
    snprintf(foundation, sizeof foundation, "%u",
             hash_foundation(mapped->address, MLB_ICE_CANDIDATE_PRFLX));
    local = &a->local[a->local_count];
    memset(local, 0, sizeof *local);
    local->fd = base->fd;
    local->turn = base->turn;
    local->address = base->address;
    local->address_length = base->address_length;
    strcpy(local->candidate.foundation, foundation);
    local->candidate.component = 1;
    local->candidate.transport = MLB_ICE_UDP;
    local->candidate.priority = candidate_priority(110u,
        (base->candidate.priority >> 8) & 0xffffu);
    strcpy(local->candidate.address, mapped->address);
    local->candidate.port = mapped->port;
    local->candidate.type = MLB_ICE_CANDIDATE_PRFLX;
    strcpy(local->candidate.related_address, related.address);
    local->candidate.related_port = related.port;
    n = snprintf(local->sdp, sizeof local->sdp,
        "candidate:%s 1 UDP %u %s %u typ prflx raddr %s rport %u",
        foundation, local->candidate.priority, mapped->address, (unsigned)mapped->port,
        related.address, (unsigned)related.port);
    if (n < 0 || (size_t)n >= sizeof local->sdp) { memset(local, 0, sizeof *local); local->fd = -1; return -1; }
    *result_index = (unsigned)a->local_count++;
    return 0;
}

static unsigned inbound_host_candidate_index(const MlbIceAgent *a, unsigned socket_index) {
    int fd = a->local[socket_index].fd;
    for (size_t i = 0; i < a->local_count; i++)
        if (a->local[i].fd == fd && a->local[i].candidate.type == MLB_ICE_CANDIDATE_HOST)
            return (unsigned)i;
    return socket_index;
}

static int record_valid_pair(MlbIceAgent *a, unsigned base_local, unsigned remote_index,
                             const MlbStunMappedAddress *mapped) {
    unsigned valid_local = base_local;
    if (add_local_peer_reflexive(a, base_local, mapped, &valid_local) != 0)
        valid_local = base_local;
    if (add_pair(a, valid_local, remote_index) != 0) return -1;
    int valid_index = pair_exists(a, valid_local, &a->remote[remote_index].address);
    if (valid_index < 0) return -1;
    a->pairs[valid_index].state = PAIR_SUCCEEDED;
    a->pairs[valid_index].valid = 1;
    return valid_index;
}

static int valid_pair_for_socket_remote(const MlbIceAgent *a, unsigned local_index,
                                         const struct sockaddr_storage *remote) {
    int best = -1;
    int fd = a->local[local_index].fd;
    for (size_t i = 0; i < a->pair_count; i++) {
        const CandidatePair *p = &a->pairs[i];
        if (p->valid && a->local[p->local_index].fd == fd &&
            sockaddr_equal(&a->remote[p->remote_index].address, remote) &&
            (best < 0 || p->priority > a->pairs[best].priority)) best = (int)i;
    }
    return best;
}

static int queue_datagram(MlbIceAgent *a, const uint8_t *data, size_t length) {
    QueuedDatagram *q;
    if (!length || length > MLB_ICE_AGENT_MAX_DATAGRAM || length > ICE_QUEUE_MAX_BYTES - a->queued_bytes) return -1;
    q = (QueuedDatagram *)malloc(sizeof *q + length);
    if (!q) return -1;
    q->next = NULL; q->length = length; memcpy(q->data, data, length);
    if (a->queue_tail) a->queue_tail->next = q; else a->queue_head = q;
    a->queue_tail = q; a->queued_bytes += length;
    return 0;
}
static int selected_by_remote(MlbIceAgent *a, unsigned li, const struct sockaddr_storage *from) {
    if (a->selected_pair < 0) return 0;
    CandidatePair *p = &a->pairs[a->selected_pair];
    return a->local[p->local_index].fd == a->local[li].fd &&
           sockaddr_equal(&a->remote[p->remote_index].address, from);
}
static uint64_t next_consent_interval(void) {
    uint16_t random_value = 0;
    if (random_bytes((uint8_t *)&random_value, sizeof random_value) != 0) random_value = 0x8000u;
    return ICE_CONSENT_INTERVAL_MIN_MS + (uint64_t)(random_value %
        (ICE_CONSENT_INTERVAL_MAX_MS - ICE_CONSENT_INTERVAL_MIN_MS + 1u));
}

static void select_pair(MlbIceAgent *a, size_t pi) {
    if (pi >= a->pair_count) return;
    a->pairs[pi].valid = 1;
    a->pairs[pi].nominated = 1;
    a->pairs[pi].state = PAIR_SUCCEEDED;
    a->selected_pair = (int)pi;
    a->nomination_pair = -1;
    a->state = MLB_ICE_AGENT_CONNECTED;
    a->triggered_head = 0;
    a->triggered_count = 0;
    for (size_t i = 0; i < a->pair_count; i++) a->pairs[i].triggered_queued = 0;
    a->last_consent_response_ms = monotonic_ms();
    a->next_consent_ms = a->last_consent_response_ms + next_consent_interval();
    memset(a->consent_transactions, 0, sizeof a->consent_transactions);
}
static int handle_request(MlbIceAgent *a, unsigned socket_index, const uint8_t *packet, size_t length,
                          const struct sockaddr_storage *from, socklen_t from_len) {
    char expected[MLB_ICE_MAX_UFRAG * 2u + 2u];
    MlbIceCheckInfo info;
    int ri, pi;
    unsigned local_index;
    uint8_t response[ICE_STUN_MAX]; size_t response_length = 0;
    MlbStunMappedAddress mapped;
    if (snprintf(expected, sizeof expected, "%s:%s", a->local_ufrag, a->remote_ufrag) >= (int)sizeof expected ||
        mlb_ice_verify_check_request(packet, length, expected, a->local_password, &info) != 0) return -1;

    if (info.peer_role == a->role) {
        int switch_local = a->role == MLB_ICE_ROLE_CONTROLLING ?
            info.peer_tie_breaker > a->tie_breaker : info.peer_tie_breaker < a->tie_breaker;
        if (!switch_local) {
            if (mlb_ice_build_role_conflict(response, sizeof response, packet + 8,
                                            a->local_password, &response_length) != 0) return -1;
            return socket_send(a, socket_index, response, response_length, from, from_len);
        }
        a->role = a->role == MLB_ICE_ROLE_CONTROLLING ? MLB_ICE_ROLE_CONTROLLED : MLB_ICE_ROLE_CONTROLLING;
        for (size_t i = 0; i < a->pair_count; i++)
            a->pairs[i].priority = pair_priority(a->local[a->pairs[i].local_index].candidate.priority,
                a->remote[a->pairs[i].remote_index].candidate.priority, a->role);
    }

    local_index = inbound_host_candidate_index(a, socket_index);
    ri = -1;
    for (size_t i = 0; i < a->remote_count; i++)
        if (sockaddr_equal(&a->remote[i].address, from)) { ri = (int)i; break; }
    if (ri < 0) ri = add_peer_reflexive(a, local_index, from, from_len, info.priority);
    if (ri < 0) return -1;

    pi = pair_exists(a, local_index, &a->remote[ri].address);
    if (pi < 0) {
        if (add_pair(a, local_index, (unsigned)ri) != 0) return -1;
        pi = pair_exists(a, local_index, &a->remote[ri].address);
    }
    if (pi < 0) return -1;

    CandidatePair *pair = &a->pairs[pi];
    if (a->state == MLB_ICE_AGENT_CHECKING && pair->state != PAIR_SUCCEEDED) {
        /* A triggered check normally supersedes an outstanding ordinary
           check. Preserve a controlling USE-CANDIDATE transaction, however:
           its success response is the acknowledgement of nomination and a
           simultaneous reverse check must not invalidate that transaction. */
        if (!(pair->state == PAIR_IN_PROGRESS && pair->nominated_check &&
              a->role == MLB_ICE_ROLE_CONTROLLING && !info.use_candidate)) {
            pair->has_transaction = 0;
            pair->state = PAIR_WAITING;
            if (enqueue_triggered_check(a, (size_t)pi) != 0) return -1;
            if (info.use_candidate && a->role == MLB_ICE_ROLE_CONTROLLED)
                pair->triggered_use_candidate = 1;
        }
    } else if (info.use_candidate && a->role == MLB_ICE_ROLE_CONTROLLED &&
               a->state == MLB_ICE_AGENT_CHECKING) {
        int valid_index = pair->valid ? pi :
            valid_pair_for_socket_remote(a, local_index, &a->remote[ri].address);
        if (valid_index >= 0) select_pair(a, (size_t)valid_index);
        else {
            pair->triggered_use_candidate = 1;
            pair->state = PAIR_WAITING;
            if (enqueue_triggered_check(a, (size_t)pi) != 0) return -1;
        }
    }

    if (sockaddr_to_mapped(from, &mapped) != 0 ||
        mlb_ice_build_check_success(response, sizeof response, packet + 8,
                                    a->local_password, &mapped, &response_length) != 0)
        return -1;
    return socket_send(a, socket_index, response, response_length, from, from_len);
}

static int handle_consent_response(MlbIceAgent *a, unsigned li, const uint8_t *packet,
                                   size_t length, const struct sockaddr_storage *from,
                                   uint64_t now) {
    if (a->selected_pair < 0) return -1;
    CandidatePair *selected = &a->pairs[a->selected_pair];
    if (now < a->last_consent_response_ms ||
        now - a->last_consent_response_ms >= ICE_CONSENT_EXPIRY_MS) {
        a->state = MLB_ICE_AGENT_FAILED; a->selected_pair = -1;
        memset(a->consent_transactions, 0, sizeof a->consent_transactions);
        return -1; /* A response received after consent expiry cannot revive it. */
    }
    if (a->local[selected->local_index].fd != a->local[li].fd ||
        !sockaddr_equal(&a->remote[selected->remote_index].address, from)) return -1;
    for (size_t i = 0; i < ICE_CONSENT_TX_SLOTS; i++) {
        ConsentTransaction *tx = &a->consent_transactions[i];
        if (!tx->active || memcmp(tx->transaction_id, packet + 8, 12) != 0) continue;
        MlbStunMappedAddress mapped;
        if (mlb_ice_verify_check_success(packet, length, tx->transaction_id,
                                         a->remote_password, &mapped) != 0) return -1;
        tx->active = 0;
        a->last_consent_response_ms = now;
        /* Responses to already-sent, older checks are no longer useful. */
        for (size_t j = 0; j < ICE_CONSENT_TX_SLOTS; j++)
            if (a->consent_transactions[j].active &&
                a->consent_transactions[j].created_ms <= tx->created_ms)
                a->consent_transactions[j].active = 0;
        return 0;
    }
    return -1;
}

static int nominate_best_valid_pair(MlbIceAgent *a, uint64_t now) {
    int best = -1;
    if (a->role != MLB_ICE_ROLE_CONTROLLING || a->selected_pair >= 0 ||
        a->nomination_pair >= 0) return 0;
    for (size_t i = 0; i < a->pair_count; i++) {
        CandidatePair *p = &a->pairs[i];
        if (p->valid && p->state == PAIR_SUCCEEDED &&
            (best < 0 || p->priority > a->pairs[best].priority)) best = (int)i;
    }
    if (best < 0) return 0;
    if (send_check(a, (size_t)best, 1, now) != 0) {
        a->pairs[best].state = PAIR_FAILED;
        a->pairs[best].valid = 0;
        return -1;
    }
    a->nomination_pair = best;
    return 1;
}

static int handle_response(MlbIceAgent *a, unsigned li, const uint8_t *packet, size_t length,
                           const struct sockaddr_storage *from, uint64_t now) {
    int pi, valid_pi;
    unsigned base_local, remote_index;
    int nominated_check, triggered_use_candidate;
    MlbStunMappedAddress mapped;
    if (length < MLB_STUN_HEADER_SIZE) return -1;
    pi = pair_index_for_transaction(a, packet + 8, from, li);
    if (pi < 0) return handle_consent_response(a, li, packet, length, from, now);
    CandidatePair *p = &a->pairs[pi];
    if (mlb_ice_verify_check_success(packet, length, p->transaction_id,
                                     a->remote_password, &mapped) != 0) return -1;
    base_local = p->local_index;
    remote_index = p->remote_index;
    nominated_check = p->nominated_check;
    triggered_use_candidate = p->triggered_use_candidate;
    p->has_transaction = 0;
    p->state = PAIR_SUCCEEDED;
    valid_pi = record_valid_pair(a, base_local, remote_index, &mapped);
    if (valid_pi < 0) valid_pi = pi;

    if (nominated_check && a->role == MLB_ICE_ROLE_CONTROLLING) {
        select_pair(a, (size_t)valid_pi);
    } else if (triggered_use_candidate && a->role == MLB_ICE_ROLE_CONTROLLED) {
        a->pairs[valid_pi].nominated = 1;
        select_pair(a, (size_t)valid_pi);
    } else if (a->role == MLB_ICE_ROLE_CONTROLLING && a->selected_pair < 0) {
        /* Regular nomination is singular per ICE generation. */
        (void)nominate_best_valid_pair(a, now);
    }
    return 0;
}

static int handle_role_conflict(MlbIceAgent *a, unsigned li, const uint8_t *packet,
                                size_t length, const struct sockaddr_storage *from, uint64_t now) {
    int pi;
    if (length < MLB_STUN_HEADER_SIZE) return -1;
    pi = pair_index_for_transaction(a, packet + 8, from, li);
    if (pi < 0) return -1;
    CandidatePair *p = &a->pairs[pi];
    if (mlb_ice_verify_role_conflict(packet, length, p->transaction_id, a->remote_password) != 0) return -1;
    a->role = a->role == MLB_ICE_ROLE_CONTROLLING ? MLB_ICE_ROLE_CONTROLLED : MLB_ICE_ROLE_CONTROLLING;
    a->nomination_pair = -1;
    p->has_transaction = 0; p->state = PAIR_WAITING;
    for (size_t i = 0; i < a->pair_count; i++)
        a->pairs[i].priority = pair_priority(a->local[a->pairs[i].local_index].candidate.priority,
            a->remote[a->pairs[i].remote_index].candidate.priority, a->role);
    return send_check(a, (size_t)pi, 0, now);
}

static void process_packet_data(MlbIceAgent *a, unsigned li, const uint8_t *packet,
                                size_t length, const struct sockaddr_storage *from,
                                socklen_t from_len) {
    if (!a || li >= a->local_count || !packet || !length || !from || from_len > sizeof *from) return;
    if ((packet[0] & 0xc0u) == 0 && length >= MLB_STUN_HEADER_SIZE &&
        length <= ICE_STUN_MAX && get16(packet) == 0x0001u) {
        int rc = handle_request(a, li, packet, length, from, from_len);
        (void)rc; /* Invalid/unauthenticated checks are silently discarded. */
    } else if ((packet[0] & 0xc0u) == 0 && length >= MLB_STUN_HEADER_SIZE &&
               length <= ICE_STUN_MAX && get16(packet) == 0x0101u) {
        (void)handle_response(a, li, packet, length, from, monotonic_ms());
    } else if ((packet[0] & 0xc0u) == 0 && length >= MLB_STUN_HEADER_SIZE &&
               length <= ICE_STUN_MAX && get16(packet) == 0x0111u) {
        (void)handle_role_conflict(a, li, packet, length, from, monotonic_ms());
    } else if (selected_by_remote(a, li, from)) {
        (void)queue_datagram(a, packet, length);
    }
}

static void process_packet(MlbIceAgent *a, unsigned li) {
    for (unsigned budget = 0; budget < 32; budget++) {
        uint8_t packet[MLB_ICE_AGENT_MAX_DATAGRAM];
        struct sockaddr_storage from; socklen_t from_len = sizeof from;
        ssize_t n = recvfrom(a->local[li].fd, packet, sizeof packet, 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0 || from_len > sizeof from) continue;
        process_packet_data(a, li, packet, (size_t)n, &from, from_len);
    }
}

static int sockaddr_from_turn_peer(const char *ip, uint16_t port,
                                   struct sockaddr_storage *out, socklen_t *out_len) {
    struct sockaddr_in *v4;
    struct sockaddr_in6 *v6;
    if (!ip || !*ip || !port || !out || !out_len) return -1;
    memset(out, 0, sizeof *out);
    v4 = (struct sockaddr_in *)out;
    if (inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET; v4->sin_port = htons(port); *out_len = sizeof *v4; return 0;
    }
    v6 = (struct sockaddr_in6 *)out;
    if (inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6; v6->sin6_port = htons(port); *out_len = sizeof *v6; return 0;
    }
    return -1;
}

static void process_turn_packets(MlbIceAgent *a, unsigned li) {
    MlbTurnClient *turn;
    if (!a || li >= a->local_count || !(turn = a->local[li].turn)) return;
    /* mlb_turn_poll consumes one server datagram. Drain all queued payloads,
       then keep consuming ready TURN packets without blocking the ICE loop. */
    for (unsigned budget = 0; budget < 64; budget++) {
        char peer[46]; uint16_t port = 0; size_t length = 0; socklen_t from_len = 0;
        uint8_t data[MLB_ICE_AGENT_MAX_DATAGRAM];
        int received = mlb_turn_receive(turn, peer, sizeof peer, &port, data, sizeof data, &length);
        if (received == 0) {
            struct sockaddr_storage from;
            if (sockaddr_from_turn_peer(peer, port, &from, &from_len) == 0)
                process_packet_data(a, li, data, length, &from, from_len);
            continue;
        }
        int rc = mlb_turn_poll(turn, 0);
        if (rc <= 0) break;
    }
}

MlbIceAgent *mlb_ice_agent_create(const MlbIceAgentConfig *c) {
    MlbIceAgent *a;
    uint8_t random[8];
    if (!c || !mlb_ice_crypto_available() ||
        !valid_credential(c->local_ufrag, 4, MLB_ICE_MAX_UFRAG - 1u) ||
        !valid_credential(c->remote_ufrag, 4, MLB_ICE_MAX_UFRAG - 1u) ||
        strchr(c->local_ufrag, ':') || strchr(c->remote_ufrag, ':') ||
        !valid_credential(c->local_password, 22, MLB_ICE_MAX_PASSWORD - 1u) ||
        !valid_credential(c->remote_password, 22, MLB_ICE_MAX_PASSWORD - 1u) ||
        (c->role != MLB_ICE_ROLE_CONTROLLING && c->role != MLB_ICE_ROLE_CONTROLLED)) return NULL;
    a = (MlbIceAgent *)calloc(1, sizeof *a);
    if (!a) return NULL;
    for (size_t i = 0; i < ICE_LOCAL_MAX; i++) a->local[i].fd = -1;
    strcpy(a->local_ufrag, c->local_ufrag); strcpy(a->local_password, c->local_password);
    strcpy(a->remote_ufrag, c->remote_ufrag); strcpy(a->remote_password, c->remote_password);
    a->role = c->role; a->selected_pair = -1; a->nomination_pair = -1;
    a->state = MLB_ICE_AGENT_NEW;
    if (c->tie_breaker) a->tie_breaker = c->tie_breaker;
    else if (random_bytes(random, sizeof random) == 0) {
        for (size_t i = 0; i < sizeof random; i++) a->tie_breaker = (a->tie_breaker << 8) | random[i];
        if (!a->tie_breaker) a->tie_breaker = 1;
    } else { free(a); return NULL; }
    return a;
}

static int gather_srflx_one(MlbIceAgent *a, size_t li, const struct addrinfo *server,
                            unsigned timeout_ms) {
    uint8_t tid[12], request[MLB_STUN_HEADER_SIZE], response[MLB_STUN_MAX_MESSAGE_SIZE + 1u];
    size_t request_len;
    struct pollfd pfd;
    struct sockaddr_storage from;
    socklen_t from_len;
    MlbStunMappedAddress mapped;
    struct sockaddr_storage server_addr;
    if (server->ai_addrlen > sizeof server_addr || server->ai_family != a->local[li].address.ss_family) return -1;
    memset(&server_addr, 0, sizeof server_addr); memcpy(&server_addr, server->ai_addr, server->ai_addrlen);
    if (random_transaction_id(tid) != 0 ||
        mlb_stun_build_binding_request(request, sizeof request, tid, &request_len) != 0 ||
        socket_send(a, (unsigned)li, request, request_len, &server_addr, (socklen_t)server->ai_addrlen) != 0) return -1;
    pfd.fd = a->local[li].fd; pfd.events = POLLIN; pfd.revents = 0;
    int pr; do { pr = poll(&pfd, 1, (int)timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0 || !(pfd.revents & POLLIN)) return -1;
    from_len = sizeof from;
    ssize_t n = recvfrom(a->local[li].fd, response, sizeof response, 0, (struct sockaddr *)&from, &from_len);
    if (n < (ssize_t)MLB_STUN_HEADER_SIZE || (size_t)n > MLB_STUN_MAX_MESSAGE_SIZE ||
        !sockaddr_equal(&from, &server_addr) ||
        mlb_stun_parse_binding_response(response, (size_t)n, tid, &mapped) != 0) return -1;
    if (a->local_count >= ICE_LOCAL_MAX) return -1;
    LocalCandidate *host = &a->local[li], *srflx = &a->local[a->local_count];
    char related_address[MLB_ICE_MAX_ADDRESS + 1u];
    char foundation[MLB_ICE_MAX_FOUNDATION + 1u];
    uint16_t related_port = host->candidate.port;
    strcpy(related_address, host->candidate.address);
    snprintf(foundation, sizeof foundation, "%u",
             hash_foundation(mapped.address, MLB_ICE_CANDIDATE_SRFLX));
    memset(srflx, 0, sizeof *srflx); srflx->fd = host->fd; srflx->address = host->address;
    srflx->address_length = host->address_length;
    strcpy(srflx->candidate.foundation, foundation);
    srflx->candidate.component = 1;
    srflx->candidate.transport = MLB_ICE_UDP; srflx->candidate.priority = candidate_priority(100u, host->candidate.priority >> 8 & 0xffffu);
    strcpy(srflx->candidate.address, mapped.address); srflx->candidate.port = mapped.port;
    srflx->candidate.type = MLB_ICE_CANDIDATE_SRFLX;
    strcpy(srflx->candidate.related_address, related_address); srflx->candidate.related_port = related_port;
    int nfmt = snprintf(srflx->sdp, sizeof srflx->sdp,
        "candidate:%s 1 UDP %u %s %u typ srflx raddr %s rport %u",
        foundation, srflx->candidate.priority, mapped.address,
        (unsigned)mapped.port, related_address, (unsigned)related_port);
    if (nfmt < 0 || (size_t)nfmt >= sizeof srflx->sdp) return -1;
    a->local_count++;
    return 0;
}

int mlb_ice_agent_gather(MlbIceAgent *a, const char *stun_host, uint16_t stun_port,
                         unsigned timeout_ms) {
    struct addrinfo hints, *servers = NULL;
    char service[8];
    if (!a || a->state != MLB_ICE_AGENT_NEW) return -1;
    a->state = MLB_ICE_AGENT_GATHERING;
    if (gather_hosts(a) != 0) { a->state = MLB_ICE_AGENT_FAILED; return -1; }
    if (stun_host) {
        if (!*stun_host || !stun_port || timeout_ms < 50 || timeout_ms > 5000) {
            a->state = MLB_ICE_AGENT_FAILED; return -1;
        }
        memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP; hints.ai_flags = AI_NUMERICSERV;
        snprintf(service, sizeof service, "%u", (unsigned)stun_port);
        if (getaddrinfo(stun_host, service, &hints, &servers) == 0 && servers) {
            size_t host_count = a->local_count;
            unsigned per_socket = timeout_ms / (unsigned)(host_count ? host_count : 1u);
            if (!per_socket) per_socket = 1u;
            for (size_t i = 0; i < host_count; i++) {
                /* One family-compatible STUN destination per base socket keeps
                   the total probing wait bounded by the caller's budget. */
                for (const struct addrinfo *s = servers; s; s = s->ai_next) {
                    if (s->ai_family != a->local[i].address.ss_family) continue;
                    (void)gather_srflx_one(a, i, s, per_socket);
                    break;
                }
            }
            freeaddrinfo(servers);
        }
    }
    a->state = MLB_ICE_AGENT_GATHERED;
    return (int)a->local_count;
}

int mlb_ice_agent_add_turn_candidate(MlbIceAgent *a, MlbTurnClient *turn) {
    MlbTurnAllocationInfo allocation;
    LocalCandidate *local;
    struct sockaddr_storage address;
    socklen_t address_length;
    unsigned type_pref = 0u; /* RFC 8445 type preference for relay candidates. */
    unsigned local_pref = 65535u;
    int fd, n;
    char foundation[MLB_ICE_MAX_FOUNDATION + 1u];
    char candidate_address[MLB_ICE_MAX_ADDRESS + 1u];
    char related_address[MLB_ICE_MAX_ADDRESS + 1u];
    uint16_t related_port = 0;
    if (!a || !turn || (a->state != MLB_ICE_AGENT_NEW && a->state != MLB_ICE_AGENT_GATHERED) ||
        a->local_count >= ICE_LOCAL_MAX) return -1;
    for (size_t i = 0; i < a->local_count; i++) if (a->local[i].turn == turn) return -1;
    if (mlb_turn_get_allocation(turn, &allocation) != 0) {
        /* get_allocation() is const and does not mutate expired state. Let
           maintain() retire a stale allocation before attempting a new one. */
        (void)mlb_turn_maintain(turn);
        if (mlb_turn_get_allocation(turn, &allocation) != 0 &&
            (mlb_turn_allocate(turn) != 0 || mlb_turn_get_allocation(turn, &allocation) != 0))
            return -1;
    }
    if (!allocation.active || !allocation.relayed_address.port ||
        (allocation.relayed_address.family != AF_INET &&
         allocation.relayed_address.family != AF_INET6)) return -1;
    memset(&address, 0, sizeof address);
    if (allocation.relayed_address.family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
        v4->sin_family = AF_INET; v4->sin_port = htons(allocation.relayed_address.port);
        if (inet_pton(AF_INET, allocation.relayed_address.address, &v4->sin_addr) != 1) return -1;
        address_length = sizeof *v4;
    } else {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
        v6->sin6_family = AF_INET6; v6->sin6_port = htons(allocation.relayed_address.port);
        if (inet_pton(AF_INET6, allocation.relayed_address.address, &v6->sin6_addr) != 1) return -1;
        address_length = sizeof *v6;
    }
    fd = mlb_turn_native_fd(turn);
    if (fd < 0) return -1;
    snprintf(foundation, sizeof foundation, "%u",
             hash_foundation(allocation.relayed_address.address, MLB_ICE_CANDIDATE_RELAY));
    local = &a->local[a->local_count];
    memset(local, 0, sizeof *local);
    local->fd = fd; local->turn = turn; local->address = address; local->address_length = address_length;
    strcpy(local->candidate.foundation, foundation);
    local->candidate.component = 1; local->candidate.transport = MLB_ICE_UDP;
    local->candidate.priority = candidate_priority(type_pref, local_pref);
    strcpy(local->candidate.address, allocation.relayed_address.address);
    local->candidate.port = allocation.relayed_address.port;
    local->candidate.type = MLB_ICE_CANDIDATE_RELAY;
    strcpy(candidate_address, local->candidate.address);
    if ((allocation.mapped_address.family == AF_INET || allocation.mapped_address.family == AF_INET6) &&
        allocation.mapped_address.port && allocation.mapped_address.address[0]) {
        strcpy(related_address, allocation.mapped_address.address);
        related_port = allocation.mapped_address.port;
        strcpy(local->candidate.related_address, related_address);
        local->candidate.related_port = related_port;
        n = snprintf(local->sdp, sizeof local->sdp,
            "candidate:%s 1 UDP %u %s %u typ relay raddr %s rport %u",
            foundation, local->candidate.priority, candidate_address,
            (unsigned)local->candidate.port, related_address, (unsigned)related_port);
    } else {
        n = snprintf(local->sdp, sizeof local->sdp,
            "candidate:%s 1 UDP %u %s %u typ relay",
            foundation, local->candidate.priority, candidate_address,
            (unsigned)local->candidate.port);
    }
    if (n < 0 || (size_t)n >= sizeof local->sdp) {
        memset(local, 0, sizeof *local); local->fd = -1; return -1;
    }
    unsigned index = (unsigned)a->local_count++;
    /* A relay allocation is a valid gathering result even when host gathering
       was intentionally omitted (relay-only policy/testing). */
    if (a->state == MLB_ICE_AGENT_NEW) a->state = MLB_ICE_AGENT_GATHERED;
    /* Permissions are IP-scoped. Use the resolved numeric peer address rather
       than the SDP token: remote candidates may contain an mDNS hostname. */
    for (size_t ri = 0; ri < a->remote_count; ri++) {
        MlbStunMappedAddress peer;
        if (sockaddr_to_mapped(&a->remote[ri].address, &peer) != 0 ||
            mlb_turn_create_permission(turn, peer.address, peer.port) != 0) {
            a->local_count--;
            memset(&a->local[index], 0, sizeof a->local[index]); a->local[index].fd = -1;
            return -1;
        }
    }
    return (int)index;
}

size_t mlb_ice_agent_local_candidate_count(const MlbIceAgent *a) { return a ? a->local_count : 0; }
int mlb_ice_agent_local_candidate(const MlbIceAgent *a, size_t i, char *out, size_t cap) {
    if (!a || !out || !cap || i >= a->local_count || strlen(a->local[i].sdp) + 1u > cap) return -1;
    strcpy(out, a->local[i].sdp); return 0;
}

int mlb_ice_agent_set_remote_credentials(MlbIceAgent *a, const char *ufrag,
                                          const char *password) {
    if (!a || (a->state != MLB_ICE_AGENT_NEW && a->state != MLB_ICE_AGENT_GATHERED) ||
        a->pair_count || !valid_credential(ufrag, 4, MLB_ICE_MAX_UFRAG - 1u) ||
        !valid_credential(password, 22, MLB_ICE_MAX_PASSWORD - 1u) || strchr(ufrag, ':'))
        return -1;
    strcpy(a->remote_ufrag, ufrag);
    strcpy(a->remote_password, password);
    return 0;
}

int mlb_ice_agent_set_role(MlbIceAgent *a, int role) {
    if (!a || (a->state != MLB_ICE_AGENT_NEW && a->state != MLB_ICE_AGENT_GATHERED) ||
        a->pair_count || (role != MLB_ICE_ROLE_CONTROLLING && role != MLB_ICE_ROLE_CONTROLLED))
        return -1;
    a->role = role;
    return 0;
}

int mlb_ice_agent_add_remote_candidate(MlbIceAgent *a, const char *text) {
    MlbIceCandidate c; RemoteCandidate *r; struct sockaddr_storage addr; socklen_t addr_len;
    if (!a || !text || (a->state != MLB_ICE_AGENT_GATHERED && a->state != MLB_ICE_AGENT_CHECKING) ||
        a->remote_count >= ICE_REMOTE_MAX || mlb_ice_parse_candidate(text, &c) != 0 ||
        c.component != 1 || c.transport != MLB_ICE_UDP ||
        addr_from_candidate(&c, &addr, &addr_len) != 0)
        return -1;
    for (size_t i = 0; i < a->remote_count; i++) {
        if (sockaddr_equal(&a->remote[i].address, &addr)) return 0;
    }
    /* TURN permissions are needed in advance for inbound packets from this
       peer; otherwise the relay server drops its first connectivity check.
       Use the resolved address, because an SDP candidate may use mDNS. */
    for (size_t li = 0; li < a->local_count; li++) if (a->local[li].turn) {
        int seen = 0;
        /* A TURN allocation is bound to one address family. A remote
           candidate in the other family cannot form a candidate pair with
           this relay address and must not force an impossible permission
           request (e.g. IPv6 candidate against an IPv4-only allocation). */
        if (a->local[li].address.ss_family != addr.ss_family) continue;
        for (size_t prior = 0; prior < li; prior++)
            if (a->local[prior].turn == a->local[li].turn) { seen = 1; break; }
        if (!seen) {
            MlbStunMappedAddress peer;
            if (sockaddr_to_mapped(&addr, &peer) != 0 ||
                mlb_turn_create_permission(a->local[li].turn, peer.address, peer.port) != 0)
                return -1;
        }
    }
    r = &a->remote[a->remote_count]; memset(r, 0, sizeof *r);
    r->candidate = c; r->address = addr; r->address_length = addr_len;
    strncpy(r->sdp, text, sizeof r->sdp - 1u);
    unsigned ri = (unsigned)a->remote_count++;
    if (a->state == MLB_ICE_AGENT_CHECKING) {
        for (size_t li = 0; li < a->local_count; li++) if (a->local[li].address.ss_family == addr.ss_family)
            (void)add_pair(a, (unsigned)li, ri);
    }
    return 0;
}

int mlb_ice_agent_start(MlbIceAgent *a) {
    if (!a || a->state != MLB_ICE_AGENT_GATHERED || !a->remote_count) return -1;
    for (size_t li = 0; li < a->local_count; li++)
        for (size_t ri = 0; ri < a->remote_count; ri++)
            if (a->local[li].address.ss_family == a->remote[ri].address.ss_family)
                if (add_pair(a, (unsigned)li, (unsigned)ri) != 0) return -1;
    if (!a->pair_count) return -1;
    for (size_t i = 0; i < a->pair_count; i++) for (size_t j = i + 1; j < a->pair_count; j++)
        if (a->pairs[j].priority > a->pairs[i].priority) {
            CandidatePair tmp = a->pairs[i]; a->pairs[i] = a->pairs[j]; a->pairs[j] = tmp;
        }
    initialize_pair_states(a);
    a->state = MLB_ICE_AGENT_CHECKING;
    a->next_pair_ms = monotonic_ms();
    return (int)a->pair_count;
}

int mlb_ice_agent_restart(MlbIceAgent *a, const MlbIceAgentConfig *config) {
    char local_ufrag[MLB_ICE_MAX_UFRAG], local_password[MLB_ICE_MAX_PASSWORD];
    char remote_ufrag[MLB_ICE_MAX_UFRAG], remote_password[MLB_ICE_MAX_PASSWORD];
    int local_changed, remote_changed;
    if (!a || !config || (a->state != MLB_ICE_AGENT_GATHERED &&
        a->state != MLB_ICE_AGENT_CHECKING && a->state != MLB_ICE_AGENT_CONNECTED &&
        a->state != MLB_ICE_AGENT_FAILED) ||
        !valid_credential(config->local_ufrag, 4, MLB_ICE_MAX_UFRAG - 1u) ||
        !valid_credential(config->remote_ufrag, 4, MLB_ICE_MAX_UFRAG - 1u) ||
        strchr(config->local_ufrag, ':') || strchr(config->remote_ufrag, ':') ||
        !valid_credential(config->local_password, 22, MLB_ICE_MAX_PASSWORD - 1u) ||
        !valid_credential(config->remote_password, 22, MLB_ICE_MAX_PASSWORD - 1u) ||
        config->role != a->role) return -1;
    strcpy(local_ufrag, config->local_ufrag);
    strcpy(local_password, config->local_password);
    strcpy(remote_ufrag, config->remote_ufrag);
    strcpy(remote_password, config->remote_password);
    local_changed = strcmp(local_ufrag, a->local_ufrag) != 0 ||
                    strcmp(local_password, a->local_password) != 0;
    remote_changed = strcmp(remote_ufrag, a->remote_ufrag) != 0 ||
                     strcmp(remote_password, a->remote_password) != 0;
    if ((!local_changed && !remote_changed) ||
        (local_changed && (!strcmp(local_ufrag, a->local_ufrag) ||
                           !strcmp(local_password, a->local_password))) ||
        (remote_changed && (!strcmp(remote_ufrag, a->remote_ufrag) ||
                            !strcmp(remote_password, a->remote_password)))) return -1;

    /* Preserve bound sockets and gathered host/srflx candidates, but discard
       learned peer-reflexive locals and all generation-specific checklist data. */
    size_t kept = 0;
    for (size_t i = 0; i < a->local_count; i++) {
        if (a->local[i].candidate.type == MLB_ICE_CANDIDATE_PRFLX) continue;
        if (kept != i) a->local[kept] = a->local[i];
        kept++;
    }
    for (size_t i = kept; i < a->local_count; i++) {
        memset(&a->local[i], 0, sizeof a->local[i]);
        a->local[i].fd = -1;
    }
    a->local_count = kept;
    a->remote_count = 0;
    a->pair_count = 0;
    a->triggered_head = 0;
    a->triggered_count = 0;
    a->triggered_checks_sent = 0;
    a->selected_pair = -1;
    a->nomination_pair = -1;
    a->last_consent_response_ms = 0;
    a->next_consent_ms = 0;
    memset(a->pairs, 0, sizeof a->pairs);
    memset(a->consent_transactions, 0, sizeof a->consent_transactions);
    while (a->queue_head) {
        QueuedDatagram *q = a->queue_head;
        a->queue_head = q->next;
        free(q);
    }
    a->queue_tail = NULL;
    a->queued_bytes = 0;
    strcpy(a->local_ufrag, local_ufrag);
    strcpy(a->local_password, local_password);
    strcpy(a->remote_ufrag, remote_ufrag);
    strcpy(a->remote_password, remote_password);
    a->state = MLB_ICE_AGENT_GATHERED;
    a->next_pair_ms = monotonic_ms();
    return (int)a->local_count;
}

int mlb_ice_agent_get_stats(const MlbIceAgent *a, MlbIceAgentStats *stats) {
    if (!a || !stats) return -1;
    memset(stats, 0, sizeof *stats);
    stats->local_candidates = a->local_count;
    stats->remote_candidates = a->remote_count;
    stats->candidate_pairs = a->pair_count;
    stats->triggered_checks_queued = a->triggered_count;
    stats->triggered_checks_sent = a->triggered_checks_sent;
    for (size_t i = 0; i < a->pair_count; i++) {
        switch (a->pairs[i].state) {
            case PAIR_FROZEN: stats->frozen_pairs++; break;
            case PAIR_WAITING: stats->waiting_pairs++; break;
            case PAIR_IN_PROGRESS: stats->in_progress_pairs++; break;
            case PAIR_SUCCEEDED: stats->succeeded_pairs++; break;
            case PAIR_FAILED: stats->failed_pairs++; break;
        }
        if (a->pairs[i].valid) stats->valid_pairs++;
    }
    return 0;
}

static int send_consent_check(MlbIceAgent *a, uint64_t now) {
    if (a->selected_pair < 0) return -1;
    CandidatePair *p = &a->pairs[a->selected_pair];
    ConsentTransaction *slot = NULL;
    for (size_t i = 0; i < ICE_CONSENT_TX_SLOTS; i++) {
        if (!a->consent_transactions[i].active) { slot = &a->consent_transactions[i]; break; }
    }
    if (!slot) {
        /* Keep the newest eight IDs; expired/late answers outside this window are ignored. */
        slot = &a->consent_transactions[0];
        for (size_t i = 1; i < ICE_CONSENT_TX_SLOTS; i++)
            if (a->consent_transactions[i].created_ms < slot->created_ms) slot = &a->consent_transactions[i];
    }
    char username[MLB_ICE_MAX_UFRAG * 2u + 2u];
    uint8_t packet[ICE_STUN_MAX]; size_t packet_length = 0;
    a->next_consent_ms = now + next_consent_interval();
    if (snprintf(username, sizeof username, "%s:%s", a->remote_ufrag, a->local_ufrag) >= (int)sizeof username ||
        random_transaction_id(slot->transaction_id) != 0 ||
        mlb_ice_build_check_request(packet, sizeof packet, slot->transaction_id, username,
            a->remote_password, a->local[p->local_index].candidate.priority,
            a->tie_breaker, a->role, 0, &packet_length) != 0 ||
        socket_send(a, p->local_index, packet, packet_length,
            &a->remote[p->remote_index].address, a->remote[p->remote_index].address_length) != 0) {
        slot->active = 0;
        return -1;
    }
    slot->created_ms = now; slot->active = 1;
    return 0;
}

static void drive_consent(MlbIceAgent *a, uint64_t now) {
    if (a->state != MLB_ICE_AGENT_CONNECTED || a->selected_pair < 0) return;
    if (now < a->last_consent_response_ms ||
        now - a->last_consent_response_ms >= ICE_CONSENT_EXPIRY_MS) {
        a->state = MLB_ICE_AGENT_FAILED;
        a->selected_pair = -1;
        memset(a->consent_transactions, 0, sizeof a->consent_transactions);
        return;
    }
    for (size_t i = 0; i < ICE_CONSENT_TX_SLOTS; i++)
        if (a->consent_transactions[i].active &&
            now - a->consent_transactions[i].created_ms >= ICE_CONSENT_EXPIRY_MS)
            a->consent_transactions[i].active = 0;
    if (now >= a->next_consent_ms) (void)send_consent_check(a, now);
}

static void schedule_check(MlbIceAgent *a, uint64_t now) {
    int pair_index = -1;
    if (now < a->next_pair_ms) return;

    /* Triggered checks have priority over ordinary checklist checks. */
    while (a->triggered_count && pair_index < 0) {
        int queued = dequeue_triggered_check(a);
        if (queued >= 0 && a->pairs[queued].state != PAIR_SUCCEEDED)
            pair_index = queued;
    }
    if (pair_index < 0) {
        pair_index = highest_priority_waiting_pair(a);
        if (pair_index < 0) {
            (void)unfreeze_available_foundations(a);
            pair_index = highest_priority_waiting_pair(a);
        }
    }
    if (pair_index < 0) return;
    CandidatePair *p = &a->pairs[pair_index];
    if (send_check(a, (size_t)pair_index, 0, now) != 0) {
        p->state = PAIR_FAILED;
        p->has_transaction = 0;
    } else {
        a->triggered_checks_sent++;
    }
    a->next_pair_ms = now + ICE_PAIR_PACING_MS;
}

static void drive_timers(MlbIceAgent *a, uint64_t now) {
    if (a->state != MLB_ICE_AGENT_CHECKING) return;
    for (size_t i = 0; i < a->pair_count; i++) {
        CandidatePair *p = &a->pairs[i];
        if (p->state != PAIR_IN_PROGRESS || !p->has_transaction || now < p->deadline_ms) continue;
        if (p->retransmits >= ICE_RETRANSMIT_LIMIT) {
            p->state = PAIR_FAILED; p->has_transaction = 0; p->valid = 0;
            if (a->nomination_pair == (int)i) a->nomination_pair = -1;
            continue;
        }
        uint8_t packet[ICE_STUN_MAX]; size_t packet_length = 0;
        char username[MLB_ICE_MAX_UFRAG * 2u + 2u];
        if (snprintf(username, sizeof username, "%s:%s", a->remote_ufrag, a->local_ufrag) >= (int)sizeof username ||
            mlb_ice_build_check_request(packet, sizeof packet, p->transaction_id, username,
                a->remote_password, a->local[p->local_index].candidate.priority,
                a->tie_breaker, a->role, p->nominated_check, &packet_length) != 0 ||
            socket_send(a, p->local_index, packet, packet_length,
                &a->remote[p->remote_index].address, a->remote[p->remote_index].address_length) != 0) {
            p->state = PAIR_FAILED; p->has_transaction = 0; p->valid = 0;
            if (a->nomination_pair == (int)i) a->nomination_pair = -1;
            continue;
        }
        p->retransmits++; if (p->rto_ms < 8000u) p->rto_ms *= 2u;
        p->deadline_ms = now + p->rto_ms;
    }
    schedule_check(a, now);
    if (a->selected_pair < 0 && a->role == MLB_ICE_ROLE_CONTROLLING && a->nomination_pair < 0)
        (void)nominate_best_valid_pair(a, now);
    if (a->selected_pair < 0 && !a->triggered_count) {
        int pending = 0;
        for (size_t i = 0; i < a->pair_count; i++)
            if (a->pairs[i].state == PAIR_WAITING || a->pairs[i].state == PAIR_IN_PROGRESS) { pending = 1; break; }
        if (!pending) {
            if (unfreeze_available_foundations(a) > 0) pending = 1;
            else {
                for (size_t i = 0; i < a->pair_count; i++)
                    if (a->pairs[i].state == PAIR_WAITING || a->pairs[i].state == PAIR_IN_PROGRESS) { pending = 1; break; }
            }
        }
        int has_valid = 0;
        for (size_t i = 0; i < a->pair_count; i++) if (a->pairs[i].valid) { has_valid = 1; break; }
        if (!pending && !a->triggered_count && !(a->role == MLB_ICE_ROLE_CONTROLLED && has_valid))
            a->state = MLB_ICE_AGENT_FAILED;
    }
}

int mlb_ice_agent_poll(MlbIceAgent *a, unsigned timeout_ms) {
    struct pollfd fds[ICE_LOCAL_MAX * 2u];
    unsigned local_map[ICE_LOCAL_MAX * 2u];
    unsigned is_turn[ICE_LOCAL_MAX * 2u];
    size_t fd_count = 0;
    uint64_t before, now;
    int changed = 0, pr;
    if (!a || (a->state != MLB_ICE_AGENT_CHECKING && a->state != MLB_ICE_AGENT_CONNECTED)) return -1;
    before = (uint64_t)a->state;
    if (timeout_ms > 1000u) timeout_ms = 1000u;
    if (a->state == MLB_ICE_AGENT_CHECKING) {
        now = monotonic_ms();
        if ((a->triggered_count || highest_priority_waiting_pair(a) >= 0) &&
            a->next_pair_ms > now && a->next_pair_ms - now < timeout_ms)
            timeout_ms = (unsigned)(a->next_pair_ms - now);
        for (size_t i = 0; i < a->pair_count; i++) if (a->pairs[i].state == PAIR_IN_PROGRESS &&
            a->pairs[i].deadline_ms > now && a->pairs[i].deadline_ms - now < timeout_ms)
            timeout_ms = (unsigned)(a->pairs[i].deadline_ms - now);
    } else if (a->state == MLB_ICE_AGENT_CONNECTED) {
        now = monotonic_ms();
        uint64_t expiry = a->last_consent_response_ms + ICE_CONSENT_EXPIRY_MS;
        uint64_t due = a->next_consent_ms < expiry ? a->next_consent_ms : expiry;
        if (due > now && due - now < timeout_ms) timeout_ms = (unsigned)(due - now);
        else if (due <= now) timeout_ms = 0;
    }
    /* TURN maintenance is driven by the same thread/event loop as ICE. It
       refreshes allocations and peer permissions without a background worker. */
    for (size_t i = 0; i < a->local_count; i++) if (a->local[i].turn) {
        int seen = 0;
        for (size_t j = 0; j < i; j++) if (a->local[j].turn == a->local[i].turn) { seen = 1; break; }
        if (!seen) (void)mlb_turn_maintain(a->local[i].turn);
    }
    /* A single TURN fd may represent one relay candidate. Host/srflx/prflx
       entries can share their UDP descriptor, so only poll unique descriptors. */
    for (size_t i = 0; i < a->local_count && fd_count < ICE_LOCAL_MAX * 2u; i++) {
        int fd = a->local[i].fd, seen = 0;
        for (size_t j = 0; j < fd_count; j++) if (fds[j].fd == fd) { seen = 1; break; }
        if (seen || fd < 0) continue;
        fds[fd_count].fd = fd; fds[fd_count].events = POLLIN; fds[fd_count].revents = 0;
        local_map[fd_count] = (unsigned)i; is_turn[fd_count] = a->local[i].turn != NULL;
        fd_count++;
    }
    do { pr = poll(fds, (nfds_t)fd_count, (int)timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr > 0) for (size_t i = 0; i < fd_count; i++) if (fds[i].revents & POLLIN) {
        if (is_turn[i]) process_turn_packets(a, local_map[i]);
        else process_packet(a, local_map[i]);
    }
    now = monotonic_ms(); drive_timers(a, now); drive_consent(a, now);
    changed = before != (uint64_t)a->state;
    return changed;
}

int mlb_ice_agent_state(const MlbIceAgent *a) { return a ? a->state : MLB_ICE_AGENT_CLOSED; }
int mlb_ice_agent_role(const MlbIceAgent *a) { return a ? a->role : -1; }
int mlb_ice_agent_selected_pair(const MlbIceAgent *a, char *local, size_t lc, char *remote, size_t rc) {
    if (!a || a->state != MLB_ICE_AGENT_CONNECTED || a->selected_pair < 0 || !local || !remote ||
        !lc || !rc) return -1;
    const CandidatePair *p = &a->pairs[a->selected_pair];
    if (strlen(a->local[p->local_index].sdp) + 1u > lc || strlen(a->remote[p->remote_index].sdp) + 1u > rc) return -1;
    strcpy(local, a->local[p->local_index].sdp); strcpy(remote, a->remote[p->remote_index].sdp); return 0;
}
int mlb_ice_agent_send_datagram(MlbIceAgent *a, const void *data, size_t length) {
    if (!a || a->state != MLB_ICE_AGENT_CONNECTED || a->selected_pair < 0 || !data ||
        !length || length > MLB_ICE_AGENT_MAX_DATAGRAM) return -1;
    uint64_t now = monotonic_ms();
    if (now < a->last_consent_response_ms ||
        now - a->last_consent_response_ms >= ICE_CONSENT_EXPIRY_MS) {
        a->state = MLB_ICE_AGENT_FAILED; a->selected_pair = -1;
        memset(a->consent_transactions, 0, sizeof a->consent_transactions);
        return -1;
    }
    CandidatePair *p = &a->pairs[a->selected_pair];
    return socket_send(a, p->local_index, data, length,
                       &a->remote[p->remote_index].address, a->remote[p->remote_index].address_length);
}
int mlb_ice_agent_receive_datagram(MlbIceAgent *a, void *data, size_t capacity, size_t *received_length) {
    QueuedDatagram *q;
    if (!a || !data || !received_length || !(q = a->queue_head) || q->length > capacity) return -1;
    a->queue_head = q->next; if (!a->queue_head) a->queue_tail = NULL;
    a->queued_bytes -= q->length; memcpy(data, q->data, q->length); *received_length = q->length; free(q); return 0;
}
void mlb_ice_agent_destroy(MlbIceAgent *a) {
    if (!a) return;
    for (size_t i = 0; i < a->local_count; i++) if (a->local[i].fd >= 0 && !a->local[i].turn) {
        int duplicate = 0;
        for (size_t j = 0; j < i; j++) if (a->local[j].fd == a->local[i].fd) duplicate = 1;
        if (!duplicate) close(a->local[i].fd);
    }
    while (a->queue_head) { QueuedDatagram *q = a->queue_head; a->queue_head = q->next; free(q); }
    memset(a->local_password, 0, sizeof a->local_password);
    memset(a->remote_password, 0, sizeof a->remote_password);
    free(a);
}
#endif
