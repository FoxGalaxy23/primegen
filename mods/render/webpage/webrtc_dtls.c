#define _POSIX_C_SOURCE 200809L
#include "webrtc_dtls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if defined(_WIN32) || !defined(HAVE_OPENSSL_VAULT)
struct MlbDtlsPeer { int unavailable; };
int mlb_dtls_available(void) { return 0; }
MlbDtlsPeer *mlb_dtls_peer_create(const MlbDtlsConfig *c) { (void)c; return NULL; }
void mlb_dtls_peer_destroy(MlbDtlsPeer *p) { (void)p; }
int mlb_dtls_peer_local_fingerprint(const MlbDtlsPeer *p, char *o, size_t n) { (void)p; (void)o; (void)n; return -1; }
int mlb_dtls_peer_set_remote_fingerprint(MlbDtlsPeer *p, const char *f) { (void)p; (void)f; return -1; }
int mlb_dtls_peer_set_role(MlbDtlsPeer *p, enum MlbDtlsRole r) { (void)p; (void)r; return -1; }
int mlb_dtls_peer_start(MlbDtlsPeer *p) { (void)p; return -1; }
int mlb_dtls_peer_poll(MlbDtlsPeer *p, unsigned t) { (void)p; (void)t; return -1; }
int mlb_dtls_peer_state(const MlbDtlsPeer *p) { (void)p; return MLB_DTLS_STATE_CLOSED; }
size_t mlb_dtls_peer_max_message_size(const MlbDtlsPeer *p) { (void)p; return 0; }
int mlb_dtls_peer_send(MlbDtlsPeer *p, const void *d, size_t n) { (void)p; (void)d; (void)n; return -1; }
int mlb_dtls_peer_receive(MlbDtlsPeer *p, void *d, size_t c, size_t *n) { (void)p; (void)d; (void)c; (void)n; return -1; }
int mlb_dtls_peer_get_stats(const MlbDtlsPeer *p, MlbDtlsStats *s) { (void)p; (void)s; return -1; }
int mlb_dtls_peer_cipher(const MlbDtlsPeer *p, char *o, size_t n) { (void)p; (void)o; (void)n; return -1; }
int mlb_dtls_peer_last_error(const MlbDtlsPeer *p, char *o, size_t n) { (void)p; (void)o; (void)n; return -1; }
int mlb_dtls_peer_shutdown(MlbDtlsPeer *p) { (void)p; return -1; }
#else
#include <errno.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sys/time.h>

#define DTLS_QUEUE_MAX_BYTES (1024u * 1024u)
#define DTLS_HANDSHAKE_TIMEOUT_DEFAULT 30000u
#define DTLS_POLL_MAX_MS 1000u
#define DTLS_READ_BURST 32u

typedef struct DtlsMessage {
    struct DtlsMessage *next;
    size_t length;
    uint8_t data[];
} DtlsMessage;

struct MlbDtlsPeer {
    MlbIceAgent *ice;
    enum MlbDtlsRole role;
    enum MlbDtlsState state;
    SSL_CTX *ctx;
    SSL *ssl;
    X509 *certificate;
    EVP_PKEY *private_key;
    uint8_t expected_fingerprint[EVP_MAX_MD_SIZE];
    unsigned expected_fingerprint_length;
    int has_expected_fingerprint;
    char local_fingerprint[8u + 3u * 32u];
    char error[256];
    unsigned mtu;
    unsigned handshake_timeout_ms;
    uint64_t handshake_started_ms;
    uint64_t handshake_completed_ms;
    uint64_t retransmissions;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t application_bytes_sent;
    uint64_t application_bytes_received;
    DtlsMessage *queue_head;
    DtlsMessage *queue_tail;
    size_t queue_bytes;
    DtlsMessage *outbound_head;
    DtlsMessage *outbound_tail;
    size_t outbound_bytes;
    uint64_t outbound_messages;
};

static BIO_METHOD *ice_bio_method;
static CRYPTO_ONCE bio_once = CRYPTO_ONCE_STATIC_INIT;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int bio_create(BIO *bio) {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, NULL);
    BIO_set_shutdown(bio, 0);
    return 1;
}

static int bio_destroy(BIO *bio) {
    if (!bio) return 0;
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int bio_read(BIO *bio, char *out, int out_length) {
    MlbDtlsPeer *peer = (MlbDtlsPeer *)BIO_get_data(bio);
    uint8_t packet[MLB_ICE_AGENT_MAX_DATAGRAM];
    size_t packet_length = 0;
    int rc;
    if (!peer || !out || out_length <= 0) return 0;
    BIO_clear_retry_flags(bio);
    rc = mlb_ice_agent_receive_datagram(peer->ice, packet, sizeof packet, &packet_length);
    if (rc != 0) {
        BIO_set_retry_read(bio);
        return -1;
    }
    if (!packet_length) return -1;
    if (packet_length > (size_t)out_length) {
        /* Drop oversized/unparseable UDP input without wedging the ICE queue. */
        BIO_set_retry_read(bio);
        return -1;
    }
    memcpy(out, packet, packet_length);
    peer->packets_received++;
    return (int)packet_length;
}

static int bio_write(BIO *bio, const char *data, int length) {
    MlbDtlsPeer *peer = (MlbDtlsPeer *)BIO_get_data(bio);
    if (!peer || !data || length <= 0 || (unsigned)length > peer->mtu) return -1;
    BIO_clear_retry_flags(bio);
    if (mlb_ice_agent_send_datagram(peer->ice, data, (size_t)length) != 0) {
        BIO_set_retry_write(bio);
        return -1;
    }
    peer->packets_sent++;
    return length;
}

static long bio_ctrl(BIO *bio, int command, long number, void *pointer) {
    MlbDtlsPeer *peer = (MlbDtlsPeer *)BIO_get_data(bio);
    (void)pointer;
    switch (command) {
        case BIO_CTRL_FLUSH:
        case BIO_CTRL_DUP:
        case BIO_CTRL_RESET:
            return 1;
        case BIO_CTRL_PENDING:
        case BIO_CTRL_WPENDING:
            return 0;
        case BIO_CTRL_GET_CLOSE:
            return BIO_get_shutdown(bio);
        case BIO_CTRL_SET_CLOSE:
            BIO_set_shutdown(bio, (int)number);
            return 1;
        case BIO_CTRL_DGRAM_QUERY_MTU:
        case BIO_CTRL_DGRAM_GET_MTU:
            return peer ? (long)peer->mtu : (long)MLB_DTLS_DEFAULT_MTU;
        case BIO_CTRL_DGRAM_SET_MTU:
            if (!peer || number < 576 || number > (long)MLB_DTLS_DEFAULT_MTU) return 0;
            peer->mtu = (unsigned)number;
            return number;
        case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
            return 548;
        case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
            /* Conservative IPv6+UDP+TURN encapsulation overhead. */
            return 80;
        case BIO_CTRL_DGRAM_MTU_EXCEEDED:
        case BIO_CTRL_DGRAM_GET_RECV_TIMER_EXP:
        case BIO_CTRL_DGRAM_GET_SEND_TIMER_EXP:
            return 0;
        case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
        case BIO_CTRL_DGRAM_SET_CONNECTED:
        case BIO_CTRL_DGRAM_SET_PEER:
        case BIO_CTRL_DGRAM_CONNECT:
            return 1;
        case BIO_CTRL_DGRAM_GET_PEER:
            return 0;
        default:
            return 0;
    }
}

static void initialize_bio_method(void) {
    int type = BIO_TYPE_DGRAM | BIO_get_new_index();
    ice_bio_method = BIO_meth_new(type, "MLBrowser ICE datagram transport");
    if (!ice_bio_method) return;
    if (!BIO_meth_set_create(ice_bio_method, bio_create) ||
        !BIO_meth_set_destroy(ice_bio_method, bio_destroy) ||
        !BIO_meth_set_read(ice_bio_method, bio_read) ||
        !BIO_meth_set_write(ice_bio_method, bio_write) ||
        !BIO_meth_set_ctrl(ice_bio_method, bio_ctrl)) {
        BIO_meth_free(ice_bio_method);
        ice_bio_method = NULL;
    }
}

static int ensure_bio_method(void) {
    if (!CRYPTO_THREAD_run_once(&bio_once, initialize_bio_method)) return -1;
    return ice_bio_method ? 0 : -1;
}

static int allow_pinned_self_signed(int preverify_ok, X509_STORE_CTX *store) {
    (void)preverify_ok;
    (void)store;
    /* The X.509 chain is intentionally self-signed. Authentication is done
       after the handshake by constant-time comparison with the SDP fingerprint. */
    return 1;
}

static int parse_fingerprint(const char *text, uint8_t out[32]) {
    const char *p = text;
    static const char prefix[] = "sha-256 ";
    if (!p) return -1;
    if (strncasecmp(p, prefix, sizeof prefix - 1u) == 0) p += sizeof prefix - 1u;
    for (unsigned i = 0; i < 32; i++) {
        int hi, lo;
        if (!p[0] || !p[1]) return -1;
        hi = p[0] >= '0' && p[0] <= '9' ? p[0] - '0' :
             p[0] >= 'a' && p[0] <= 'f' ? p[0] - 'a' + 10 :
             p[0] >= 'A' && p[0] <= 'F' ? p[0] - 'A' + 10 : -1;
        lo = p[1] >= '0' && p[1] <= '9' ? p[1] - '0' :
             p[1] >= 'a' && p[1] <= 'f' ? p[1] - 'a' + 10 :
             p[1] >= 'A' && p[1] <= 'F' ? p[1] - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
        p += 2;
        if (i != 31) {
            if (*p != ':') return -1;
            p++;
        }
    }
    return *p == '\0' ? 0 : -1;
}

static int format_fingerprint(X509 *certificate, char *out, size_t capacity) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    int n;
    if (!certificate || !out || capacity < 8u + 95u + 1u ||
        X509_digest(certificate, EVP_sha256(), digest, &digest_length) != 1 || digest_length != 32u)
        return -1;
    n = snprintf(out, capacity,
        "sha-256 %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
        digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
        digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15],
        digest[16], digest[17], digest[18], digest[19], digest[20], digest[21], digest[22], digest[23],
        digest[24], digest[25], digest[26], digest[27], digest[28], digest[29], digest[30], digest[31]);
    return n > 0 && (size_t)n < capacity ? 0 : -1;
}

static int generate_identity(MlbDtlsPeer *peer) {
    EVP_PKEY_CTX *key_context = NULL;
    X509_NAME *name;
    unsigned char serial_bytes[4];
    long serial;
    int ok = 0;
    key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!key_context || EVP_PKEY_keygen_init(key_context) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_context, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(key_context, &peer->private_key) <= 0)
        goto done;
    peer->certificate = X509_new();
    if (!peer->certificate || X509_set_version(peer->certificate, 2L) != 1 ||
        RAND_bytes(serial_bytes, sizeof serial_bytes) != 1) goto done;
    serial = (long)(((uint32_t)serial_bytes[0] << 24) | ((uint32_t)serial_bytes[1] << 16) |
                    ((uint32_t)serial_bytes[2] << 8) | serial_bytes[3]);
    serial &= 0x7fffffffL;
    if (!serial) serial = 1;
    if (ASN1_INTEGER_set(X509_get_serialNumber(peer->certificate), serial) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(peer->certificate), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(peer->certificate), 86400L) ||
        X509_set_pubkey(peer->certificate, peer->private_key) != 1) goto done;
    name = X509_get_subject_name(peer->certificate);
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            (const unsigned char *)"MLBrowser WebRTC", -1, -1, 0) != 1 ||
        X509_set_issuer_name(peer->certificate, name) != 1 ||
        X509_sign(peer->certificate, peer->private_key, EVP_sha256()) <= 0 ||
        format_fingerprint(peer->certificate, peer->local_fingerprint,
                           sizeof peer->local_fingerprint) != 0)
        goto done;
    ok = 1;
done:
    EVP_PKEY_CTX_free(key_context);
    return ok ? 0 : -1;
}

static void set_error(MlbDtlsPeer *peer, const char *prefix) {
    unsigned long code = ERR_get_error();
    char detail[160] = "";
    if (code) ERR_error_string_n(code, detail, sizeof detail);
    if (detail[0]) snprintf(peer->error, sizeof peer->error, "%s: %s", prefix, detail);
    else snprintf(peer->error, sizeof peer->error, "%s", prefix);
}

static void free_messages(MlbDtlsPeer *peer) {
    while (peer->queue_head) {
        DtlsMessage *message = peer->queue_head;
        peer->queue_head = message->next;
        free(message);
    }
    peer->queue_tail = NULL;
    peer->queue_bytes = 0;
    while (peer->outbound_head) {
        DtlsMessage *message = peer->outbound_head;
        peer->outbound_head = message->next;
        OPENSSL_cleanse(message->data, message->length);
        free(message);
    }
    peer->outbound_tail = NULL;
    peer->outbound_bytes = 0;
    peer->outbound_messages = 0;
}

static int queue_message(MlbDtlsPeer *peer, const uint8_t *data, size_t length) {
    DtlsMessage *message;
    if (!length || length > MLB_DTLS_MAX_DATAGRAM ||
        length > DTLS_QUEUE_MAX_BYTES - peer->queue_bytes) return -1;
    message = (DtlsMessage *)malloc(sizeof *message + length);
    if (!message) return -1;
    message->next = NULL;
    message->length = length;
    memcpy(message->data, data, length);
    if (peer->queue_tail) peer->queue_tail->next = message;
    else peer->queue_head = message;
    peer->queue_tail = message;
    peer->queue_bytes += length;
    peer->application_bytes_received += length;
    return 0;
}

static int queue_outbound_message(MlbDtlsPeer *peer, const void *data, size_t length) {
    DtlsMessage *message;
    if (!peer || !data || !length || length > MLB_DTLS_MAX_DATAGRAM ||
        length > DTLS_QUEUE_MAX_BYTES - peer->outbound_bytes) return -1;
    message = (DtlsMessage *)malloc(sizeof *message + length);
    if (!message) return -1;
    message->next = NULL;
    message->length = length;
    memcpy(message->data, data, length);
    if (peer->outbound_tail) peer->outbound_tail->next = message;
    else peer->outbound_head = message;
    peer->outbound_tail = message;
    peer->outbound_bytes += length;
    peer->outbound_messages++;
    return 0;
}

/* OpenSSL requires the same application-data buffer to be retried after
 * WANT_READ/WANT_WRITE. Keep the head allocation stable until SSL_write_ex
 * succeeds; later messages may be queued but cannot overtake it. */
static int flush_outbound(MlbDtlsPeer *peer) {
    while (peer && peer->outbound_head && peer->state == MLB_DTLS_STATE_CONNECTED) {
        DtlsMessage *message = peer->outbound_head;
        size_t written = 0;
        int rc = SSL_write_ex(peer->ssl, message->data, message->length, &written);
        if (rc == 1 && written == message->length) {
            peer->outbound_head = message->next;
            if (!peer->outbound_head) peer->outbound_tail = NULL;
            peer->outbound_bytes -= message->length;
            if (peer->outbound_messages) peer->outbound_messages--;
            peer->application_bytes_sent += written;
            OPENSSL_cleanse(message->data, message->length);
            free(message);
            continue;
        }
        {
            int ssl_error = SSL_get_error(peer->ssl, rc);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) return 0;
        }
        set_error(peer, "DTLS application write failed");
        peer->state = MLB_DTLS_STATE_FAILED;
        return -1;
    }
    return 0;
}

static int verify_peer_fingerprint(MlbDtlsPeer *peer) {
    X509 *remote = SSL_get1_peer_certificate(peer->ssl);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    int ok = 0;
    if (!remote) {
        snprintf(peer->error, sizeof peer->error, "DTLS peer did not present a certificate");
        return -1;
    }
    if (X509_digest(remote, EVP_sha256(), digest, &digest_length) == 1 &&
        digest_length == peer->expected_fingerprint_length &&
        CRYPTO_memcmp(digest, peer->expected_fingerprint, digest_length) == 0)
        ok = 1;
    X509_free(remote);
    if (!ok) snprintf(peer->error, sizeof peer->error,
                      "DTLS certificate SHA-256 fingerprint does not match SDP");
    return ok ? 0 : -1;
}

static int finish_handshake(MlbDtlsPeer *peer) {
    const SSL_CIPHER *cipher;
    if (verify_peer_fingerprint(peer) != 0) return -1;
    cipher = SSL_get_current_cipher(peer->ssl);
    if (!cipher || strcmp(SSL_CIPHER_get_name(cipher), "ECDHE-ECDSA-AES128-GCM-SHA256") != 0) {
        snprintf(peer->error, sizeof peer->error, "DTLS negotiated an unsupported cipher suite");
        return -1;
    }
    peer->state = MLB_DTLS_STATE_CONNECTED;
    peer->handshake_completed_ms = monotonic_ms();
    return 0;
}

static int drive_handshake(MlbDtlsPeer *peer) {
    int rc;
    if (monotonic_ms() - peer->handshake_started_ms >= peer->handshake_timeout_ms) {
        snprintf(peer->error, sizeof peer->error, "DTLS handshake timed out");
        peer->state = MLB_DTLS_STATE_FAILED;
        return -1;
    }
    rc = SSL_do_handshake(peer->ssl);
    if (rc == 1) {
        if (finish_handshake(peer) != 0) {
            peer->state = MLB_DTLS_STATE_FAILED;
            return -1;
        }
        return 1;
    }
    int ssl_error = SSL_get_error(peer->ssl, rc);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) return 0;
    set_error(peer, "DTLS handshake failed");
    peer->state = MLB_DTLS_STATE_FAILED;
    return -1;
}

int mlb_dtls_available(void) { return ensure_bio_method() == 0; }

MlbDtlsPeer *mlb_dtls_peer_create(const MlbDtlsConfig *config) {
    MlbDtlsPeer *peer = NULL;
    BIO *bio = NULL;
    unsigned mtu, timeout;
    if (!config || !config->ice_agent ||
        (config->role != MLB_DTLS_ROLE_CLIENT && config->role != MLB_DTLS_ROLE_SERVER) ||
        ensure_bio_method() != 0)
        return NULL;
    mtu = config->mtu ? config->mtu : MLB_DTLS_DEFAULT_MTU;
    timeout = config->handshake_timeout_ms ? config->handshake_timeout_ms : DTLS_HANDSHAKE_TIMEOUT_DEFAULT;
    if (mtu < 576u || mtu > MLB_DTLS_DEFAULT_MTU || timeout < 1000u || timeout > 120000u)
        return NULL;
    peer = (MlbDtlsPeer *)calloc(1, sizeof *peer);
    if (!peer) return NULL;
    peer->ice = config->ice_agent;
    peer->role = config->role;
    peer->state = MLB_DTLS_STATE_NEW;
    peer->mtu = mtu;
    peer->handshake_timeout_ms = timeout;
    if (config->remote_fingerprint) {
        if (parse_fingerprint(config->remote_fingerprint, peer->expected_fingerprint) != 0)
            goto fail;
        peer->expected_fingerprint_length = 32u;
        peer->has_expected_fingerprint = 1;
    }
    if (generate_identity(peer) != 0) goto fail;
    peer->ctx = SSL_CTX_new(DTLS_method());
    if (!peer->ctx ||
        SSL_CTX_set_min_proto_version(peer->ctx, DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(peer->ctx, DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(peer->ctx, "ECDHE-ECDSA-AES128-GCM-SHA256") != 1 ||
        SSL_CTX_set1_groups_list(peer->ctx, "P-256") != 1 ||
        SSL_CTX_use_certificate(peer->ctx, peer->certificate) != 1 ||
        SSL_CTX_use_PrivateKey(peer->ctx, peer->private_key) != 1 ||
        SSL_CTX_check_private_key(peer->ctx) != 1)
        goto fail;
    SSL_CTX_set_verify(peer->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       allow_pinned_self_signed);
    SSL_CTX_set_verify_depth(peer->ctx, 1);
    SSL_CTX_set_options(peer->ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(peer->ctx, SSL_SESS_CACHE_OFF);
    peer->ssl = SSL_new(peer->ctx);
    bio = BIO_new(ice_bio_method);
    if (!peer->ssl || !bio) goto fail;
    BIO_set_data(bio, peer);
    SSL_set_bio(peer->ssl, bio, bio);
    bio = NULL; /* SSL owns both references to the same BIO. */
    SSL_set_read_ahead(peer->ssl, 1);
    if (SSL_set_mtu(peer->ssl, peer->mtu) <= 0) goto fail;
    if (peer->role == MLB_DTLS_ROLE_CLIENT) SSL_set_connect_state(peer->ssl);
    else SSL_set_accept_state(peer->ssl);
    return peer;
fail:
    BIO_free(bio);
    if (peer) {
        SSL_free(peer->ssl);
        SSL_CTX_free(peer->ctx);
        X509_free(peer->certificate);
        EVP_PKEY_free(peer->private_key);
        free_messages(peer);
        free(peer);
    }
    return NULL;
}

void mlb_dtls_peer_destroy(MlbDtlsPeer *peer) {
    if (!peer) return;
    SSL_free(peer->ssl);
    SSL_CTX_free(peer->ctx);
    X509_free(peer->certificate);
    EVP_PKEY_free(peer->private_key);
    free_messages(peer);
    OPENSSL_cleanse(peer->expected_fingerprint, sizeof peer->expected_fingerprint);
    free(peer);
}

int mlb_dtls_peer_local_fingerprint(const MlbDtlsPeer *peer, char *out, size_t capacity) {
    if (!peer || !out || !capacity || strlen(peer->local_fingerprint) + 1u > capacity) return -1;
    strcpy(out, peer->local_fingerprint);
    return 0;
}

int mlb_dtls_peer_set_remote_fingerprint(MlbDtlsPeer *peer, const char *fingerprint) {
    uint8_t parsed[32];
    if (!peer || peer->state != MLB_DTLS_STATE_NEW || !fingerprint ||
        parse_fingerprint(fingerprint, parsed) != 0) return -1;
    memcpy(peer->expected_fingerprint, parsed, sizeof parsed);
    peer->expected_fingerprint_length = sizeof parsed;
    peer->has_expected_fingerprint = 1;
    OPENSSL_cleanse(parsed, sizeof parsed);
    return 0;
}

int mlb_dtls_peer_set_role(MlbDtlsPeer *peer, enum MlbDtlsRole role) {
    if (!peer || peer->state != MLB_DTLS_STATE_NEW ||
        (role != MLB_DTLS_ROLE_CLIENT && role != MLB_DTLS_ROLE_SERVER)) return -1;
    peer->role = role;
    if (role == MLB_DTLS_ROLE_CLIENT) SSL_set_connect_state(peer->ssl);
    else SSL_set_accept_state(peer->ssl);
    return 0;
}

int mlb_dtls_peer_start(MlbDtlsPeer *peer) {
    if (!peer || peer->state != MLB_DTLS_STATE_NEW || !peer->has_expected_fingerprint ||
        mlb_ice_agent_state(peer->ice) != MLB_ICE_AGENT_CONNECTED) return -1;
    peer->state = MLB_DTLS_STATE_HANDSHAKING;
    peer->handshake_started_ms = monotonic_ms();
    return drive_handshake(peer) < 0 ? -1 : 0;
}

static unsigned timeout_until_dtls_timer(MlbDtlsPeer *peer, unsigned requested) {
    struct timeval timeout;
    if (peer->state == MLB_DTLS_STATE_HANDSHAKING && DTLSv1_get_timeout(peer->ssl, &timeout)) {
        uint64_t ms = (uint64_t)timeout.tv_sec * 1000u +
                      ((uint64_t)timeout.tv_usec + 999u) / 1000u;
        if (ms < requested) requested = (unsigned)ms;
    }
    return requested;
}

int mlb_dtls_peer_poll(MlbDtlsPeer *peer, unsigned timeout_ms) {
    enum MlbDtlsState before;
    int rc, progress = 0;
    if (!peer || peer->state == MLB_DTLS_STATE_FAILED || peer->state == MLB_DTLS_STATE_CLOSED) return -1;
    if (peer->state == MLB_DTLS_STATE_NEW) return -1;
    before = peer->state;
    if (timeout_ms > DTLS_POLL_MAX_MS) timeout_ms = DTLS_POLL_MAX_MS;
    timeout_ms = timeout_until_dtls_timer(peer, timeout_ms);
    (void)mlb_ice_agent_poll(peer->ice, timeout_ms);
    if (mlb_ice_agent_state(peer->ice) != MLB_ICE_AGENT_CONNECTED) {
        snprintf(peer->error, sizeof peer->error, "underlying ICE transport is no longer connected");
        peer->state = MLB_DTLS_STATE_FAILED;
        return -1;
    }
    if (peer->state == MLB_DTLS_STATE_HANDSHAKING) {
        rc = DTLSv1_handle_timeout(peer->ssl);
        if (rc < 0) {
            set_error(peer, "DTLS retransmission timer failed");
            peer->state = MLB_DTLS_STATE_FAILED;
            return -1;
        }
        if (rc > 0) peer->retransmissions++;
        rc = drive_handshake(peer);
        if (rc < 0) return -1;
        if (rc > 0) progress = 1;
    }
    if (peer->state == MLB_DTLS_STATE_CONNECTED) {
        for (unsigned i = 0; i < DTLS_READ_BURST; i++) {
            uint8_t plaintext[MLB_DTLS_MAX_DATAGRAM];
            size_t plaintext_length = 0;
            rc = SSL_read_ex(peer->ssl, plaintext, sizeof plaintext, &plaintext_length);
            if (rc == 1) {
                if (queue_message(peer, plaintext, plaintext_length) != 0) {
                    snprintf(peer->error, sizeof peer->error, "DTLS application receive queue limit exceeded");
                    peer->state = MLB_DTLS_STATE_FAILED;
                    return -1;
                }
                progress = 1;
                continue;
            }
            int ssl_error = SSL_get_error(peer->ssl, rc);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) break;
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                peer->state = MLB_DTLS_STATE_CLOSED;
                progress = 1;
                break;
            }
            set_error(peer, "DTLS application read failed");
            peer->state = MLB_DTLS_STATE_FAILED;
            return -1;
        }
        if (peer->state == MLB_DTLS_STATE_CONNECTED && peer->outbound_head) {
            rc = flush_outbound(peer);
            if (rc < 0) return -1;
            if (peer->outbound_head == NULL) progress = 1;
        }
    }
    return peer->state != before || progress ? 1 : 0;
}

int mlb_dtls_peer_state(const MlbDtlsPeer *peer) {
    return peer ? peer->state : MLB_DTLS_STATE_CLOSED;
}

size_t mlb_dtls_peer_max_message_size(const MlbDtlsPeer *peer) {
    if (!peer || peer->state != MLB_DTLS_STATE_CONNECTED) return 0;
    return DTLS_get_data_mtu(peer->ssl);
}

int mlb_dtls_peer_send(MlbDtlsPeer *peer, const void *data, size_t length) {
    size_t max_message;
    if (!peer || peer->state != MLB_DTLS_STATE_CONNECTED || !data || !length ||
        length > MLB_DTLS_MAX_DATAGRAM) return -1;
    max_message = DTLS_get_data_mtu(peer->ssl);
    if (!max_message || length > max_message) return -1;
    if (queue_outbound_message(peer, data, length) != 0) return -1;
    /* A return of zero means the datagram is accepted into the bounded queue;
       poll() preserves ordering and retries transient OpenSSL WANT states. */
    return flush_outbound(peer) < 0 ? -1 : 0;
}

int mlb_dtls_peer_receive(MlbDtlsPeer *peer, void *data, size_t capacity, size_t *received_length) {
    DtlsMessage *message;
    if (!peer || !data || !received_length || !(message = peer->queue_head) ||
        message->length > capacity) return -1;
    peer->queue_head = message->next;
    if (!peer->queue_head) peer->queue_tail = NULL;
    peer->queue_bytes -= message->length;
    memcpy(data, message->data, message->length);
    *received_length = message->length;
    free(message);
    return 0;
}

int mlb_dtls_peer_get_stats(const MlbDtlsPeer *peer, MlbDtlsStats *stats) {
    if (!peer || !stats) return -1;
    memset(stats, 0, sizeof *stats);
    stats->state = peer->state;
    stats->handshake_started_ms = peer->handshake_started_ms;
    stats->handshake_completed_ms = peer->handshake_completed_ms;
    stats->handshake_retransmissions = peer->retransmissions;
    stats->packets_sent = peer->packets_sent;
    stats->packets_received = peer->packets_received;
    stats->application_bytes_sent = peer->application_bytes_sent;
    stats->application_bytes_received = peer->application_bytes_received;
    stats->pending_outbound_messages = peer->outbound_messages;
    stats->pending_outbound_bytes = peer->outbound_bytes;
    return 0;
}

int mlb_dtls_peer_cipher(const MlbDtlsPeer *peer, char *out, size_t capacity) {
    const SSL_CIPHER *cipher;
    const char *name;
    if (!peer || !out || !capacity || peer->state != MLB_DTLS_STATE_CONNECTED ||
        !(cipher = SSL_get_current_cipher(peer->ssl)) || !(name = SSL_CIPHER_get_name(cipher)) ||
        strlen(name) + 1u > capacity) return -1;
    strcpy(out, name);
    return 0;
}

int mlb_dtls_peer_last_error(const MlbDtlsPeer *peer, char *out, size_t capacity) {
    if (!peer || !out || !capacity || !peer->error[0] || strlen(peer->error) + 1u > capacity) return -1;
    strcpy(out, peer->error);
    return 0;
}

int mlb_dtls_peer_shutdown(MlbDtlsPeer *peer) {
    int rc;
    if (!peer || peer->state == MLB_DTLS_STATE_CLOSED) return 0;
    if (peer->state == MLB_DTLS_STATE_CONNECTED) {
        rc = SSL_shutdown(peer->ssl);
        if (rc < 0) {
            int ssl_error = SSL_get_error(peer->ssl, rc);
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
                set_error(peer, "DTLS close_notify failed");
                peer->state = MLB_DTLS_STATE_FAILED;
                return -1;
            }
        }
    }
    peer->state = MLB_DTLS_STATE_CLOSED;
    return 0;
}
#endif
