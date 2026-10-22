/* TLS-бэкенд на системном OpenSSL (1.1.x / 3.x). Библиотека подгружается во время работы через dlopen,
   поэтому для сборки браузера заголовки OpenSSL не нужны; при отсутствии библиотеки ntls_available() == 0,
   и NAL откатывается на старый путь через curl. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ntls.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>

typedef struct ssl_st SSL; typedef struct ssl_ctx_st SSL_CTX; typedef struct ssl_method_st SSL_METHOD; typedef struct x509_vp_st X509_VP;

static struct {
    int tried, ok; void* h;
    int (*init_ssl)(uint64_t, const void*);
    unsigned long (*version_num)(void);
    const SSL_METHOD* (*client_method)(void);
    SSL_CTX* (*ctx_new)(const SSL_METHOD*); void (*ctx_free)(SSL_CTX*);
    long (*ctx_ctrl)(SSL_CTX*, int, long, void*);
    void (*ctx_set_verify)(SSL_CTX*, int, void*);
    int (*ctx_default_paths)(SSL_CTX*); int (*ctx_load_locations)(SSL_CTX*, const char*, const char*);
    uint64_t (*ctx_set_options)(SSL_CTX*, uint64_t);
    SSL* (*ssl_new)(SSL_CTX*); void (*ssl_free)(SSL*); int (*set_fd)(SSL*, int);
    long (*ssl_ctrl)(SSL*, int, long, void*);
    int (*set1_host)(SSL*, const char*); X509_VP* (*get0_param)(SSL*); int (*vp_set1_ip_asc)(X509_VP*, const char*);
    int (*connect)(SSL*); int (*read)(SSL*, void*, int); int (*write)(SSL*, const void*, int); int (*shutdown)(SSL*);
    int (*get_error)(const SSL*, int); long (*verify_result)(const SSL*); const char* (*verify_str)(long);
    unsigned long (*err_get)(void); void (*err_str)(unsigned long, char*, size_t); void (*err_clear)(void);
    SSL_CTX* ctx;
} T;

struct NTls { SSL* ssl; };

#define SYM(field, name) do { *(void**)&T.field = dlsym(T.h, name); if (!T.field) goto fail; } while (0)

static int tls_init(void) {
    if (T.tried) return T.ok;
    T.tried = 1;
    static const char* names[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so", NULL};
    for (int i = 0; names[i] && !T.h; i++) T.h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!T.h) return 0;
    SYM(init_ssl, "OPENSSL_init_ssl"); SYM(version_num, "OpenSSL_version_num"); SYM(client_method, "TLS_client_method");
    SYM(ctx_new, "SSL_CTX_new"); SYM(ctx_free, "SSL_CTX_free"); SYM(ctx_ctrl, "SSL_CTX_ctrl"); SYM(ctx_set_verify, "SSL_CTX_set_verify");
    SYM(ctx_default_paths, "SSL_CTX_set_default_verify_paths"); SYM(ctx_load_locations, "SSL_CTX_load_verify_locations");
    SYM(ctx_set_options, "SSL_CTX_set_options"); SYM(ssl_new, "SSL_new"); SYM(ssl_free, "SSL_free"); SYM(set_fd, "SSL_set_fd");
    SYM(ssl_ctrl, "SSL_ctrl"); SYM(set1_host, "SSL_set1_host"); SYM(get0_param, "SSL_get0_param");
    SYM(vp_set1_ip_asc, "X509_VERIFY_PARAM_set1_ip_asc");
    SYM(connect, "SSL_connect"); SYM(read, "SSL_read"); SYM(write, "SSL_write"); SYM(shutdown, "SSL_shutdown");
    SYM(get_error, "SSL_get_error"); SYM(verify_result, "SSL_get_verify_result"); SYM(verify_str, "X509_verify_cert_error_string");
    SYM(err_get, "ERR_get_error"); SYM(err_str, "ERR_error_string_n"); SYM(err_clear, "ERR_clear_error");
    if (!T.init_ssl(0, NULL)) goto fail;
    T.ctx = T.ctx_new(T.client_method()); if (!T.ctx) goto fail;
    T.ctx_ctrl(T.ctx, 123 /* SSL_CTRL_SET_MIN_PROTO_VERSION */, 0x0303 /* TLS 1.2 */, NULL);
    T.ctx_set_verify(T.ctx, 1 /* SSL_VERIFY_PEER */, NULL);
    uint64_t opts = 0x00000800u /* SSL_OP_NO_COMPRESSION */;
    if (T.version_num() >= 0x30000000UL) opts |= 0x80u;                 /* SSL_OP_IGNORE_UNEXPECTED_EOF: обрыв без close_notify — как EOF, длину проверяет HTTP-слой */
    T.ctx_set_options(T.ctx, opts);
    T.ctx_default_paths(T.ctx);
    if (!getenv("SSL_CERT_FILE")) {                                      /* запасные места системного пакета CA, если путь OpenSSL по умолчанию пуст */
        static const char* bundles[] = {"/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/tls/cert.pem", "/etc/ssl/certs/ca-certificates.crt", NULL};
        for (int i = 0; bundles[i]; i++) if (access(bundles[i], R_OK) == 0) { T.ctx_load_locations(T.ctx, bundles[i], NULL); break; }
    }
    T.ok = 1; return 1;
fail:
    if (T.h) { dlclose(T.h); T.h = NULL; }
    T.ok = 0; return 0;
}

int ntls_available(void) { return tls_init(); }
const char* ntls_backend_name(void) { return tls_init() ? "openssl(dlopen)" : "none"; }

static void fill_err(char* err, size_t cap, SSL* ssl) {
    if (!err || !cap) return;
    long vr = ssl ? T.verify_result(ssl) : 0;
    if (vr != 0) { snprintf(err, cap, "сертификат не прошёл проверку: %s", T.verify_str(vr)); return; }
    unsigned long e = T.err_get();
    if (e) { char b[160]; T.err_str(e, b, sizeof b); snprintf(err, cap, "TLS: %s", b); }
    else snprintf(err, cap, "TLS: рукопожатие не удалось (%s)", errno ? strerror(errno) : "соединение закрыто");
}

NTls* ntls_connect(int fd, const char* host, char* err, size_t errcap) {
    if (!tls_init()) { if (err) snprintf(err, errcap, "TLS-библиотека (libssl) не найдена"); return NULL; }
    T.err_clear();
    SSL* ssl = T.ssl_new(T.ctx); if (!ssl) { if (err) snprintf(err, errcap, "SSL_new не удалось"); return NULL; }
    T.set_fd(ssl, fd);
    struct in6_addr a6; struct in_addr a4;
    int is_ip = inet_pton(AF_INET, host, &a4) == 1 || inet_pton(AF_INET6, host, &a6) == 1;
    if (is_ip) T.vp_set1_ip_asc(T.get0_param(ssl), host);              /* для IP-адресов SNI не шлём, имя проверяем по SAN iPAddress */
    else { T.ssl_ctrl(ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0, (void*)host); T.set1_host(ssl, host); }
    errno = 0;
    if (T.connect(ssl) != 1) { fill_err(err, errcap, ssl); T.ssl_free(ssl); return NULL; }
    NTls* t = calloc(1, sizeof *t); if (!t) { T.ssl_free(ssl); return NULL; }
    t->ssl = ssl; return t;
}

static int map_err(NTls* t, int r) {
    int e = T.get_error(t->ssl, r);
    if (e == 6 /* ZERO_RETURN */) return 0;
    if (e == 2 || e == 3) return -2;                                     /* WANT_READ/WANT_WRITE на блокирующем сокете = сработал SO_*TIMEO */
    if (e == 5 /* SYSCALL */ && errno == 0) return 0;                    /* EOF без close_notify (OpenSSL 1.1) */
    if (e == 5 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
    return -1;
}
int ntls_read(NTls* t, void* buf, size_t n) {
    if (n > 1 << 30) n = 1 << 30;
    errno = 0; T.err_clear();
    int r = T.read(t->ssl, buf, (int)n);
    return r > 0 ? r : map_err(t, r);
}
int ntls_write(NTls* t, const void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done > (1u << 20) ? (1u << 20) : n - done;
        errno = 0; T.err_clear();
        int r = T.write(t->ssl, (const char*)buf + done, (int)chunk);
        if (r <= 0) { int m = map_err(t, r); return m == 0 ? -1 : m; }
        done += (size_t)r;
    }
    return (int)done;
}
void ntls_close(NTls* t, int send_shutdown) {
    if (!t) return;
    if (send_shutdown) T.shutdown(t->ssl);
    T.ssl_free(t->ssl); free(t);
}
