#ifndef MLBROWSER_WEBRTC_ICE_AGENT_H
#define MLBROWSER_WEBRTC_ICE_AGENT_H

#include <stddef.h>
#include <stdint.h>
#include "webrtc_ice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLB_ICE_AGENT_MAX_CANDIDATES 64u
#define MLB_ICE_AGENT_MAX_DATAGRAM 65507u

enum MlbIceAgentState {
    MLB_ICE_AGENT_NEW = 0,
    MLB_ICE_AGENT_GATHERING,
    MLB_ICE_AGENT_GATHERED,
    MLB_ICE_AGENT_CHECKING,
    MLB_ICE_AGENT_CONNECTED,
    MLB_ICE_AGENT_FAILED,
    MLB_ICE_AGENT_CLOSED
};

typedef struct MlbIceAgent MlbIceAgent;
struct MlbTurnClient;

typedef struct MlbIceAgentStats {
    size_t local_candidates;
    size_t remote_candidates;
    size_t candidate_pairs;
    size_t frozen_pairs;
    size_t waiting_pairs;
    size_t in_progress_pairs;
    size_t succeeded_pairs;
    size_t failed_pairs;
    size_t valid_pairs;
    size_t triggered_checks_queued;
    uint64_t triggered_checks_sent;
} MlbIceAgentStats;

typedef struct MlbIceAgentConfig {
    const char *local_ufrag;
    const char *local_password;
    const char *remote_ufrag;
    const char *remote_password;
    int role;               /* MLB_ICE_ROLE_CONTROLLING or MLB_ICE_ROLE_CONTROLLED */
    uint64_t tie_breaker;   /* Zero asks the agent to generate a secure value. */
} MlbIceAgentConfig;

/* Creates an ICE agent. Authenticated ICE requires the OpenSSL provider. */
MlbIceAgent *mlb_ice_agent_create(const MlbIceAgentConfig *config);
void mlb_ice_agent_destroy(MlbIceAgent *agent);

/* Gathers UDP host candidates on active local IPv4/IPv6 interfaces. If
 * stun_host is non-NULL, also attempts server-reflexive gathering through the
 * already-bound candidate sockets (not a separate STUN socket). timeout_ms is
 * the total STUN-gathering budget, 50..5000 ms. Returns candidate count. */
int mlb_ice_agent_gather(MlbIceAgent *agent, const char *stun_host,
                         uint16_t stun_port, unsigned timeout_ms);

/* Attaches an already-created TURN/UDP client as a local relay transport.
 * If it has no live allocation this function performs Allocate. Call after
 * gather() to retain host/srflx candidates. When called in NEW, the relay
 * candidate alone completes gathering (relay-only mode); gather() cannot be
 * called afterwards. May be called once per TURN client while NEW/GATHERED,
 * including after remote candidates were added (their TURN permissions are
 * created before returning). The agent owns neither client nor socket:
 * destroy the ICE agent before destroying the TURN client. ICE checks and
 * selected-pair datagrams use TURN Send/Data Indications. */
int mlb_ice_agent_add_turn_candidate(MlbIceAgent *agent,
                                    struct MlbTurnClient *turn_client);

size_t mlb_ice_agent_local_candidate_count(const MlbIceAgent *agent);
int mlb_ice_agent_local_candidate(const MlbIceAgent *agent, size_t index,
                                  char *out, size_t capacity);

/* Updates remote ICE credentials and role before checklist start. This exists for
 * SDP offer/answer, where local gathering precedes receipt of remote credentials.
 * Valid only in NEW/GATHERED state and before a checklist is built. */
int mlb_ice_agent_set_remote_credentials(MlbIceAgent *agent,
                                          const char *remote_ufrag,
                                          const char *remote_password);
int mlb_ice_agent_set_role(MlbIceAgent *agent, int role);

/* Adds one remote SDP candidate. UDP component 1 is currently supported. */
int mlb_ice_agent_add_remote_candidate(MlbIceAgent *agent, const char *candidate);

/* Builds the candidate-pair checklist and starts authenticated connectivity
 * checks. One component is used (RTCP-mux model). */
int mlb_ice_agent_start(MlbIceAgent *agent);

/* Starts a new ICE generation while reusing bound host sockets. At least one
 * side's credentials must change; for each side that changes, both ufrag and
 * password must change. The negotiated role/tie-breaker are retained. The
 * current selected datagram path is cleared, so callers must pause the upper
 * transport while the new checklist is running. Re-add the peer's candidates
 * and call start() after this operation. */
int mlb_ice_agent_restart(MlbIceAgent *agent, const MlbIceAgentConfig *config);

/* Returns a snapshot of candidate/checklist/triggered-check counters. */
int mlb_ice_agent_get_stats(const MlbIceAgent *agent, MlbIceAgentStats *stats);

/* Drives non-blocking UDP I/O, retransmissions, pair scheduling and nomination.
 * timeout_ms is clamped to 1000 ms. Returns 1 on state change, 0 otherwise. */
int mlb_ice_agent_poll(MlbIceAgent *agent, unsigned timeout_ms);
int mlb_ice_agent_state(const MlbIceAgent *agent);
int mlb_ice_agent_role(const MlbIceAgent *agent);

/* Selected pair is available only after nomination. */
int mlb_ice_agent_selected_pair(const MlbIceAgent *agent,
                                char *local, size_t local_capacity,
                                char *remote, size_t remote_capacity);

/* Datagram transport for the next layer (DTLS). Incoming non-STUN datagrams
 * from the selected peer are queued by poll() and returned here. */
int mlb_ice_agent_send_datagram(MlbIceAgent *agent, const void *data, size_t length);
int mlb_ice_agent_receive_datagram(MlbIceAgent *agent, void *data,
                                   size_t capacity, size_t *received_length);

#ifdef __cplusplus
}
#endif
#endif
