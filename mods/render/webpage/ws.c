#include "ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

static const char* B64T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const unsigned char* in, int n, char* out) {
    int i = 0, j = 0;
    while (i + 3 <= n) { unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2]; out[j++] = B64T[(v >> 18) & 63]; out[j++] = B64T[(v >> 12) & 63]; out[j++] = B64T[(v >> 6) & 63]; out[j++] = B64T[v & 63]; i += 3; }
    int rem = n - i;
    if (rem == 1) { unsigned v = (unsigned)in[i] << 16; out[j++] = B64T[(v >> 18) & 63]; out[j++] = B64T[(v >> 12) & 63]; out[j++] = '='; out[j++] = '='; }
    else if (rem == 2) { unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8); out[j++] = B64T[(v >> 18) & 63]; out[j++] = B64T[(v >> 12) & 63]; out[j++] = B64T[(v >> 6) & 63]; out[j++] = '='; }
    out[j] = 0;
}

/* ws://host[:port]/path — минимальный разбор, без userinfo/query-особенностей (для query просто оставляем в path) */
static int parse_ws_url(const char* url, int* is_wss, char* host, size_t hostcap, int* port, char* path, size_t pathcap) {
    *is_wss = 0; *port = 80;
    const char* p = url;
    if (!strncmp(p, "wss://", 6)) { *is_wss = 1; *port = 443; p += 6; }
    else if (!strncmp(p, "ws://", 5)) { p += 5; }
    else return 0;
    const char* slash = strchr(p, '/');
    const char* hend = slash ? slash : p + strlen(p);
    const char* colon = memchr(p, ':', (size_t)(hend - p));
    size_t hlen = (size_t)((colon ? colon : hend) - p); if (hlen >= hostcap) hlen = hostcap - 1;
    memcpy(host, p, hlen); host[hlen] = 0;
    if (colon) *port = atoi(colon + 1);
    if (slash) snprintf(path, pathcap, "%s", slash); else snprintf(path, pathcap, "/");
    return 1;
}

static void wr_frame_header(int fd, int fin, int opcode, size_t len, int masked) {
    unsigned char h[14]; int hn = 0;
    h[hn++] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 0x0F));
    unsigned char mbit = masked ? 0x80 : 0;
    if (len < 126) h[hn++] = (unsigned char)(mbit | len);
    else if (len < 65536) { h[hn++] = (unsigned char)(mbit | 126); h[hn++] = (unsigned char)(len >> 8); h[hn++] = (unsigned char)len; }
    else { h[hn++] = (unsigned char)(mbit | 127); for (int i = 7; i >= 0; i--) h[hn++] = (unsigned char)((len >> (8 * i)) & 0xFF); }
    write(fd, h, (size_t)hn);
}

static void send_client_frame(int sock, int opcode, const unsigned char* payload, size_t len) {
    unsigned char mask[4]; for (int i = 0; i < 4; i++) mask[i] = (unsigned char)rand();
    wr_frame_header(sock, 1, opcode, len, 1);
    write(sock, mask, 4);
    unsigned char buf[4096]; size_t off = 0;
    while (off < len) { size_t n = len - off; if (n > sizeof buf) n = sizeof buf; for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(payload[off + i] ^ mask[(off + i) % 4]); write(sock, buf, n); off += n; }
}

/* Событийный протокол по внутреннему пайпу ребёнок->родитель: 1 байт type + 4 байта BE length + payload.
   'O' — open (len=0). 'M' — message text (len=длина текста). 'C' — close (len=2, payload=BE код).
   'E' — error (len=0). */
static void emit(int fd, char type, const void* payload, uint32_t len) {
    unsigned char h[5]; h[0] = (unsigned char)type; h[1] = (unsigned char)(len >> 24); h[2] = (unsigned char)(len >> 16); h[3] = (unsigned char)(len >> 8); h[4] = (unsigned char)len;
    write(fd, h, 5); if (len && payload) write(fd, payload, len);
}

static void child_main(const char* url, int rfd_child_write, int wfd_child_read) {
    int is_wss = 0, port = 80; char host[256], path[1024];
    if (!parse_ws_url(url, &is_wss, host, sizeof host, &port, path, sizeof path) || is_wss) { emit(rfd_child_write, 'E', NULL, 0); _exit(0); }   /* wss:// пока не поддержан (нет TLS) */

    char portbuf[16]; snprintf(portbuf, sizeof portbuf, "%d", port);
    struct addrinfo hints = {0}, * res = NULL; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) { emit(rfd_child_write, 'E', NULL, 0); _exit(0); }
    int sock = -1;
    for (struct addrinfo* a = res; a; a = a->ai_next) { sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (sock < 0) continue; if (connect(sock, a->ai_addr, a->ai_addrlen) == 0) break; close(sock); sock = -1; }
    freeaddrinfo(res);
    if (sock < 0) { emit(rfd_child_write, 'E', NULL, 0); _exit(0); }
    { int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }

    unsigned char keyraw[16]; srand((unsigned)(time(NULL) ^ getpid())); for (int i = 0; i < 16; i++) keyraw[i] = (unsigned char)rand();
    char key[32]; b64_encode(keyraw, 16, key);
    char req[2048];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: null\r\nUser-Agent: MeetlookersBrowser/1.0\r\n\r\n", path, host, key);
    if (write(sock, req, strlen(req)) < 0) { close(sock); emit(rfd_child_write, 'E', NULL, 0); _exit(0); }

    char resp[4096]; size_t rn = 0; int handshake_ok = 0;
    while (rn < sizeof resp - 1) {
        ssize_t r = read(sock, resp + rn, 1); if (r <= 0) break; rn += (size_t)r;
        if (rn >= 4 && !memcmp(resp + rn - 4, "\r\n\r\n", 4)) { resp[rn] = 0; handshake_ok = strstr(resp, " 101") != NULL; break; }
    }
    if (!handshake_ok) { close(sock); emit(rfd_child_write, 'E', NULL, 0); _exit(0); }
    emit(rfd_child_write, 'O', NULL, 0);

    unsigned char* acc = malloc(65536); size_t acc_len = 0, acc_cap = 65536;
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(sock, &rf); FD_SET(wfd_child_read, &rf);
        int mx = sock > wfd_child_read ? sock : wfd_child_read;
        struct timeval tv = { 30, 0 };
        int sel = select(mx + 1, &rf, NULL, NULL, &tv);
        if (sel < 0) { if (errno == EINTR) continue; break; }
        if (sel == 0) continue;                                    /* таймаут select — просто снова ждём (используется как «пульс») */

        if (FD_ISSET(wfd_child_read, &rf)) {
            unsigned char h[5]; ssize_t r = read(wfd_child_read, h, 5);
            if (r <= 0) { send_client_frame(sock, 0x8, NULL, 0); break; }   /* родитель закрыл управляющий пайп — завершаемся */
            uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
            unsigned char* buf = len ? malloc(len) : NULL; size_t got = 0;
            while (got < len) { ssize_t rr = read(wfd_child_read, buf + got, len - got); if (rr <= 0) break; got += (size_t)rr; }
            if (h[0] == 'S') send_client_frame(sock, 0x1, buf, got);
            else if (h[0] == 'X') { send_client_frame(sock, 0x8, NULL, 0); free(buf); break; }
            free(buf);
        }
        if (FD_ISSET(sock, &rf)) {
            unsigned char tmp[8192]; ssize_t r = read(sock, tmp, sizeof tmp);
            if (r <= 0) { emit(rfd_child_write, 'C', "\0\0", 2); break; }
            if (acc_len + (size_t)r > acc_cap) { acc_cap = (acc_len + (size_t)r) * 2; acc = realloc(acc, acc_cap); }
            memcpy(acc + acc_len, tmp, (size_t)r); acc_len += (size_t)r;

            size_t off = 0;
            while (off + 2 <= acc_len) {
                unsigned char b0 = acc[off], b1 = acc[off + 1]; int fin = (b0 & 0x80) != 0; int opcode = b0 & 0x0F; int masked = (b1 & 0x80) != 0; uint64_t plen = b1 & 0x7F;
                size_t hn = 2;
                if (plen == 126) { if (off + 4 > acc_len) break; plen = ((uint64_t)acc[off + 2] << 8) | acc[off + 3]; hn += 2; }
                else if (plen == 127) { if (off + 10 > acc_len) break; plen = 0; for (int i = 0; i < 8; i++) plen = (plen << 8) | acc[off + 2 + i]; hn += 8; }
                if (masked) hn += 4;                                 /* сервер маскировать не должен, но на всякий случай учитываем */
                if (off + hn + plen > acc_len) break;                /* кадр ещё не пришёл целиком */
                unsigned char* payload = acc + off + hn;
                if (masked) { unsigned char* mk = acc + off + hn - 4; for (uint64_t i = 0; i < plen; i++) payload[i] ^= mk[i % 4]; }
                (void)fin;                                          /* фрагментацию не собираем — редкий случай для типичных JSON-сообщений */
                if (opcode == 0x1) emit(rfd_child_write, 'M', payload, (uint32_t)plen);
                else if (opcode == 0x8) { int code = plen >= 2 ? (payload[0] << 8) | payload[1] : 1000; unsigned char cc[2] = { (unsigned char)(code >> 8), (unsigned char)code }; emit(rfd_child_write, 'C', cc, 2); close(sock); free(acc); _exit(0); }
                else if (opcode == 0x9) send_client_frame(sock, 0xA, payload, plen);   /* ping -> pong */
                off += hn + plen;
            }
            if (off) { memmove(acc, acc + off, acc_len - off); acc_len -= off; }
        }
    }
    close(sock); free(acc); _exit(0);
}

int ws_start(const char* url, WsConn* out) {
    memset(out, 0, sizeof *out);
    int p2c[2], c2p[2];                                              /* p2c: родитель->ребёнок (команды); c2p: ребёнок->родитель (события) */
    if (pipe(p2c) < 0) return 0;
    if (pipe(c2p) < 0) { close(p2c[0]); close(p2c[1]); return 0; }
    pid_t pid = fork();
    if (pid < 0) { close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]); return 0; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        child_main(url, c2p[1], p2c[0]);
        _exit(0);
    }
    close(p2c[0]); close(c2p[1]);
    fcntl(c2p[0], F_SETFL, fcntl(c2p[0], F_GETFL, 0) | O_NONBLOCK);   /* родитель никогда не блокируется на чтении событий */
    out->used = 1; out->pid = pid; out->rfd = c2p[0]; out->wfd = p2c[1];
    return 1;
}

int ws_poll(WsConn* c, char* type, char** text, int* code) {
    if (!c->used) return 0;
    unsigned char h[5]; ssize_t r = read(c->rfd, h, 5);
    if (r <= 0) return 0;                                            /* EAGAIN или пайп закрыт-но-ещё-не-собран — ничего не готово */
    uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
    char* buf = len ? malloc(len + 1) : NULL; size_t got = 0;
    while (buf && got < len) { ssize_t rr = read(c->rfd, buf + got, len - got); if (rr <= 0) { if (rr < 0 && errno == EAGAIN) continue; break; } got += (size_t)rr; }
    if (buf) buf[len] = 0;
    *type = (char)h[0];
    if (*type == 'M') { *text = buf; buf = NULL; }
    else if (*type == 'C') { *code = (buf && len >= 2) ? (((unsigned char)buf[0] << 8) | (unsigned char)buf[1]) : 1000; }
    free(buf);
    if (*type == 'C' || *type == 'E') { close(c->rfd); close(c->wfd); int st; waitpid(c->pid, &st, WNOHANG); c->used = 0; }
    return 1;
}

void ws_send(WsConn* c, const char* text) {
    if (!c->used) return;
    uint32_t len = (uint32_t)strlen(text);
    unsigned char h[5] = { 'S', (unsigned char)(len >> 24), (unsigned char)(len >> 16), (unsigned char)(len >> 8), (unsigned char)len };
    write(c->wfd, h, 5); if (len) write(c->wfd, text, len);
}

void ws_close(WsConn* c, int code) {
    if (!c->used) return;
    unsigned char h[5] = { 'X', 0, 0, 0, 2 }; unsigned char cc[2] = { (unsigned char)(code >> 8), (unsigned char)code };
    write(c->wfd, h, 5); write(c->wfd, cc, 2);
    close(c->rfd); close(c->wfd); int st; waitpid(c->pid, &st, WNOHANG); c->used = 0;
}
