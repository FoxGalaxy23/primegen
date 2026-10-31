#include "ws.h"
#ifdef _WIN32
#include <string.h>
/* Windows named-pipe/process WebSocket backend is not implemented yet; keep builds functional and fail closed. */
int ws_start(const char* url, const char* page_url, WsConn* out) { (void)url; (void)page_url; if (out) memset(out, 0, sizeof *out); return 0; }
int ws_poll(WsConn* c, char* type, char** text, int* code) { (void)c; if (type) *type = 'E'; if (text) *text = NULL; if (code) *code = 0; return 0; }
void ws_send(WsConn* c, const char* text) { (void)c; (void)text; }
void ws_close(WsConn* c, int code) { (void)c; (void)code; }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include "ntls.h"
#include "idna.h"
#include "web_security.h"
#include "security_sandbox.h"
#include "gal.h"

/* origin страницы-инициатора для заголовка Origin рукопожатия (задаётся в ws_start до fork) */
static char g_ws_origin[320] = "null";
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/random.h>
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

static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
static void sha1_block(uint32_t h[5], const unsigned char b[64]) {
    uint32_t w[80]; for (int i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for (int i=16;i<80;i++) w[i]=rol32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=h[0],c=h[2],d=h[3],e=h[4],bb=h[1];
    for (int i=0;i<80;i++) { uint32_t f,k; if(i<20){f=(bb&c)|((~bb)&d);k=0x5A827999u;}else if(i<40){f=bb^c^d;k=0x6ED9EBA1u;}else if(i<60){f=(bb&c)|(bb&d)|(c&d);k=0x8F1BBCDCu;}else{f=bb^c^d;k=0xCA62C1D6u;}
        uint32_t t=rol32(a,5)+f+e+k+w[i];e=d;d=c;c=rol32(bb,30);bb=a;a=t; }
    h[0]+=a;h[1]+=bb;h[2]+=c;h[3]+=d;h[4]+=e;
}
static void sha1_sum(const unsigned char* data,size_t len,unsigned char out[20]) {
    uint32_t h[5]={0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u};
    size_t off=0;while(len-off>=64){sha1_block(h,data+off);off+=64;}
    unsigned char tail[128]={0};size_t rem=len-off;memcpy(tail,data+off,rem);tail[rem]=0x80;size_t total=(rem<56)?64:128;uint64_t bits=(uint64_t)len*8;
    for(int i=0;i<8;i++)tail[total-1-i]=(unsigned char)(bits>>(i*8));sha1_block(h,tail);if(total==128)sha1_block(h,tail+64);
    for(int i=0;i<5;i++){out[i*4]=(unsigned char)(h[i]>>24);out[i*4+1]=(unsigned char)(h[i]>>16);out[i*4+2]=(unsigned char)(h[i]>>8);out[i*4+3]=(unsigned char)h[i];}
}
static int websocket_accept_valid(const char* resp,const char* key) {
    char input[160],expected[40];unsigned char digest[20];
    snprintf(input,sizeof input,"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);sha1_sum((const unsigned char*)input,strlen(input),digest);b64_encode(digest,20,expected);
    const char* p=strchr(resp,'\n');if(!p)return 0;p++;
    while(*p){const char* e=strchr(p,'\n');size_t n=e?(size_t)(e-p):strlen(p);if(n&&p[n-1]=='\r')n--;
        const char* colon=memchr(p,':',n);if(colon&&(size_t)(colon-p)==20&&!strncasecmp(p,"Sec-WebSocket-Accept",20)){
            const char* v=colon+1;const char* end=p+n;while(v<end&&(*v==' '||*v=='\t'))v++;while(end>v&&(end[-1]==' '||end[-1]=='\t'))end--;
            return (size_t)(end-v)==strlen(expected)&&!memcmp(v,expected,strlen(expected)); }
        if(!e)break;p=e+1;
    }return 0;
}

/* Parse ws:// and wss:// URLs, including IDN, bracketed IPv6 and query-only targets. */
static int parse_ws_url(const char* url, int* is_wss, char* host, size_t hostcap, int* port, char* path, size_t pathcap) {
    *is_wss = 0; *port = 80;
    const char* p = url;
    if (!strncasecmp(p, "wss://", 6)) { *is_wss = 1; *port = 443; p += 6; }
    else if (!strncasecmp(p, "ws://", 5)) p += 5;
    else return 0;
    const char* end = p + strcspn(p, "/?#"); if (end == p) return 0;
    const char* colon = NULL; const char* hstart = p; const char* hend = end;
    if (*p == '[') {
        const char* cl = memchr(p, ']', (size_t)(end-p)); if (!cl) return 0;
        hstart = p + 1; hend = cl;
        if (cl + 1 < end) { if (cl[1] != ':') return 0; colon = cl + 1; }
    } else {
        for (const char* q=p;q<end;q++) if (*q==':') { if (colon) return 0; colon=q; }
        if (colon) hend = colon;
    }
    size_t hn=(size_t)(hend-hstart); if (!hn || hn>=hostcap) return 0;
    memcpy(host,hstart,hn);host[hn]=0;
    if (colon) { char* ep=NULL; long v=strtol(colon+1,&ep,10); if (ep!=end||v<1||v>65535)return 0;*port=(int)v; }
    if (!strchr(host, ':')) { char ascii[256]; if (!nidna_to_ascii(host,ascii,sizeof ascii)) return 0; snprintf(host,hostcap,"%s",ascii); }
    for(char*q=host;*q;q++)*q=(char)tolower((unsigned char)*q);
    if (*end == '/') snprintf(path,pathcap,"%s",end);
    else if (*end == '?') snprintf(path,pathcap,"/%s",end);
    else snprintf(path,pathcap,"/");
    return 1;
}
static int ws_random(void* p,size_t n) {
    unsigned char* b=p; size_t o=0;
    while(o<n){ssize_t r=getrandom(b+o,n-o,0);if(r<0&&errno==EINTR)continue;if(r<0)break;if(r==0)break;o+=(size_t)r;}
    if(o==n)return 1;
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    while(o<n){ssize_t r=read(fd,b+o,n-o);if(r<0&&errno==EINTR)continue;if(r<=0){close(fd);return 0;}o+=(size_t)r;}close(fd);return 1;
}
static ssize_t ws_net_read(int fd, NTls* tls, void* b, size_t n) { return tls ? (ssize_t)ntls_read(tls,b,n) : read(fd,b,n); }
static int ws_net_write_all(int fd, NTls* tls, const void* b, size_t n) {
    if (tls) { const unsigned char* p=b; while(n){size_t take=n>(size_t)0x3fffffff?(size_t)0x3fffffff:n; int r=ntls_write(tls,p,take);if(r<=0)return 0;p+=r;n-=(size_t)r;}return 1; }
    const unsigned char* p=b;while(n){ssize_t r=write(fd,p,n);if(r<0&&errno==EINTR)continue;if(r<=0)return 0;p+=r;n-=(size_t)r;}return 1;
}

static void wr_frame_header(int fd, NTls* tls, int fin, int opcode, size_t len, int masked) {
    unsigned char h[14]; int hn = 0;
    h[hn++] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 0x0F));
    unsigned char mbit = masked ? 0x80 : 0;
    if (len < 126) h[hn++] = (unsigned char)(mbit | len);
    else if (len < 65536) { h[hn++] = (unsigned char)(mbit | 126); h[hn++] = (unsigned char)(len >> 8); h[hn++] = (unsigned char)len; }
    else { h[hn++] = (unsigned char)(mbit | 127); for (int i = 7; i >= 0; i--) h[hn++] = (unsigned char)((len >> (8 * i)) & 0xFF); }
    (void)ws_net_write_all(fd, tls, h, (size_t)hn);
}

static void send_client_frame(int sock, NTls* tls, int opcode, const unsigned char* payload, size_t len) {
    unsigned char mask[4]; if (!ws_random(mask, sizeof mask)) return;
    wr_frame_header(sock, tls, 1, opcode, len, 1);
    if (!ws_net_write_all(sock, tls, mask, 4)) return;
    unsigned char buf[4096]; size_t off = 0;
    while (off < len) { size_t n = len - off; if (n > sizeof buf) n = sizeof buf; for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(payload[off + i] ^ mask[(off + i) % 4]); (void)ws_net_write_all(sock, tls, buf, n); off += n; }
}

/* Событийный протокол по внутреннему пайпу ребёнок->родитель: 1 байт type + 4 байта BE length + payload.
   'O' — open (len=0). 'M' — message text (len=длина текста). 'C' — close (len=2, payload=BE код).
   'E' — error (len=0). */
static void emit(int fd, char type, const void* payload, uint32_t len) {
    unsigned char h[5]; h[0] = (unsigned char)type; h[1] = (unsigned char)(len >> 24); h[2] = (unsigned char)(len >> 16); h[3] = (unsigned char)(len >> 8); h[4] = (unsigned char)len;
    write(fd, h, 5); if (len && payload) write(fd, payload, len);
}

static void child_main(const char* url, int rfd_child_write, int wfd_child_read) {
    /* Establish the sandbox before DNS, TLS and WebSocket handshake code sees remote input. */
    int sandbox = mlb_sandbox_apply();
    if ((sandbox & MLB_SANDBOX_SECCOMP) == 0 || (sandbox & MLB_SANDBOX_ERROR)) {
        emit(rfd_child_write, 'E', NULL, 0); _exit(126);
    }
    int is_wss = 0, port = 80; char host[256], path[1024]; NTls* tls = NULL;
    if (!parse_ws_url(url, &is_wss, host, sizeof host, &port, path, sizeof path)) { emit(rfd_child_write, 'E', NULL, 0); _exit(0); }
    char portbuf[16]; snprintf(portbuf, sizeof portbuf, "%d", port);
    struct addrinfo hints = {0}, * res = NULL; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) { emit(rfd_child_write, 'E', NULL, 0); _exit(0); }
    int sock = -1;
    for (struct addrinfo* a = res; a; a = a->ai_next) { sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (sock < 0) continue; if (connect(sock, a->ai_addr, a->ai_addrlen) == 0) break; close(sock); sock = -1; }
    freeaddrinfo(res);
    if (sock < 0) { emit(rfd_child_write, 'E', NULL, 0); _exit(0); }
    { int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    if (is_wss) { char err[200]; tls = ntls_connect(sock, host, err, sizeof err); if (!tls) { close(sock); emit(rfd_child_write, 'E', NULL, 0); _exit(0); } }

    unsigned char keyraw[16]; if (!ws_random(keyraw, sizeof keyraw)) { if(tls)ntls_close(tls,0);close(sock);emit(rfd_child_write,'E',NULL,0);_exit(0); }
    char key[32]; b64_encode(keyraw, 16, key);
    int default_port = is_wss ? 443 : 80; char hostline[320];
    if (strchr(host, ':')) snprintf(hostline,sizeof hostline,"[%s]%s%d",host,port==default_port?"":":",port==default_port?0:port);
    else if (port==default_port) snprintf(hostline,sizeof hostline,"%s",host);
    else snprintf(hostline,sizeof hostline,"%s:%d",host,port);
    /* Avoid a spurious trailing zero for the IPv6 default-port case. */
    if (strchr(host, ':') && port==default_port) snprintf(hostline,sizeof hostline,"[%s]",host);
    char req[2048];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\nUser-Agent: MeetlookersBrowser/1.0\r\n\r\n", path, hostline, key, g_ws_origin);
    if (!ws_net_write_all(sock,tls,req,strlen(req))) { if(tls)ntls_close(tls,0);close(sock);emit(rfd_child_write,'E',NULL,0);_exit(0); }

    char resp[4096]; size_t rn = 0; int handshake_ok = 0;
    while (rn < sizeof resp - 1) {
        ssize_t r = ws_net_read(sock,tls,resp+rn,1); if (r <= 0) break; rn += (size_t)r;
        if (rn >= 4 && !memcmp(resp + rn - 4, "\r\n\r\n", 4)) { resp[rn] = 0; int status=0; handshake_ok = sscanf(resp,"HTTP/1.%*d %d",&status)==1 && status==101 && websocket_accept_valid(resp,key); break; }
    }
    if (!handshake_ok) { if(tls)ntls_close(tls,0);close(sock);emit(rfd_child_write,'E',NULL,0);_exit(0); }
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
            if (r <= 0) { send_client_frame(sock, tls, 0x8, NULL, 0); break; }   /* родитель закрыл управляющий пайп — завершаемся */
            uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
            unsigned char* buf = len ? malloc(len) : NULL; size_t got = 0;
            while (got < len) { ssize_t rr = read(wfd_child_read, buf + got, len - got); if (rr <= 0) break; got += (size_t)rr; }
            if (h[0] == 'S') send_client_frame(sock, tls, 0x1, buf, got);
            else if (h[0] == 'X') { send_client_frame(sock, tls, 0x8, NULL, 0); free(buf); break; }
            free(buf);
        }
        if (FD_ISSET(sock, &rf)) {
            unsigned char tmp[8192]; ssize_t r = ws_net_read(sock, tls, tmp, sizeof tmp);
            if (r <= 0) { emit(rfd_child_write, 'C', "\0\0", 2); break; }
            if ((size_t)r > 16u * 1024u * 1024u || acc_len > 16u * 1024u * 1024u - (size_t)r) { emit(rfd_child_write, 'E', NULL, 0); break; }
            if (acc_len + (size_t)r > acc_cap) { size_t nc = acc_cap; while (nc < acc_len + (size_t)r) nc *= 2; unsigned char* grown = realloc(acc, nc); if (!grown) { emit(rfd_child_write, 'E', NULL, 0); break; } acc = grown; acc_cap = nc; }
            memcpy(acc + acc_len, tmp, (size_t)r); acc_len += (size_t)r;

            size_t off = 0;
            while (off + 2 <= acc_len) {
                unsigned char b0 = acc[off], b1 = acc[off + 1]; int fin = (b0 & 0x80) != 0; int opcode = b0 & 0x0F; int masked = (b1 & 0x80) != 0; uint64_t plen = b1 & 0x7F;
                size_t hn = 2;
                if (plen == 126) { if (off + 4 > acc_len) break; plen = ((uint64_t)acc[off + 2] << 8) | acc[off + 3]; hn += 2; }
                else if (plen == 127) { if (off + 10 > acc_len) break; plen = 0; for (int i = 0; i < 8; i++) plen = (plen << 8) | acc[off + 2 + i]; hn += 8; }
                if (masked) hn += 4;                                 /* сервер маскировать не должен, но на всякий случай учитываем */
                if (plen > (16u * 1024u * 1024u)) { emit(rfd_child_write, 'E', NULL, 0); goto ws_done; }
                if (off > acc_len || hn > acc_len - off || plen > acc_len - off - hn) break; /* целостность/переполнение */
                unsigned char* payload = acc + off + hn;
                if (masked) { unsigned char* mk = acc + off + hn - 4; for (uint64_t i = 0; i < plen; i++) payload[i] ^= mk[i % 4]; }
                (void)fin;                                          /* фрагментацию не собираем — редкий случай для типичных JSON-сообщений */
                if (opcode == 0x1) emit(rfd_child_write, 'M', payload, (uint32_t)plen);
                else if (opcode == 0x8) { int code = plen >= 2 ? (payload[0] << 8) | payload[1] : 1000; unsigned char cc[2] = { (unsigned char)(code >> 8), (unsigned char)code }; emit(rfd_child_write, 'C', cc, 2); close(sock); free(acc); _exit(0); }
                else if (opcode == 0x9) send_client_frame(sock, tls, 0xA, payload, plen);   /* ping -> pong */
                off += hn + plen;
            }
            if (off) { memmove(acc, acc + off, acc_len - off); acc_len -= off; }
        }
    }
ws_done:
    if (tls) ntls_close(tls, 0); close(sock); free(acc); _exit(0);
}

/* origin страницы-инициатора для заголовка Origin рукопожатия ("null" для file:// и неизвестных схем) */
static void ws_origin_of(const char* page, char* out, size_t cap) {
    snprintf(out, cap, "null"); if (!page) return;
    int https = !strncasecmp(page, "https://", 8); if (!https && strncasecmp(page, "http://", 7) != 0) return;
    const char* h = page + (https ? 8 : 7); size_t n = strcspn(h, "/?#"); const char* at = memchr(h, '@', n); if (at) { n -= (size_t)(at + 1 - h); h = at + 1; }
    if (!n || n + 12 > cap) return; snprintf(out, cap, "%s://%.*s", https ? "https" : "http", (int)n, h);
    for (char* q = out; *q; q++) *q = (char)tolower((unsigned char)*q);
}
int ws_start(const char* url, const char* page_url, WsConn* out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!url || !*url) return 0;
    char effective[2048];
    size_t url_len = strlen(url);
    if (url_len >= sizeof effective) return 0; /* do not connect to a silently truncated URL */
    memcpy(effective, url, url_len + 1);
    if (!strncasecmp(effective,"ws://",5)) {
        char http[2048], upgraded[2048];
        int hn = snprintf(http, sizeof http, "http://%s", effective + 5);
        if (hn < 0 || (size_t)hn >= sizeof http) return 0;
        if (websec_hsts_upgrade_url(http, upgraded, sizeof upgraded)) {
            if (strncasecmp(upgraded, "https://", 8)) return 0;
            int un = snprintf(effective, sizeof effective, "wss://%s", upgraded + 8);
            if (un < 0 || (size_t)un >= sizeof effective) return 0;
        }
    }
    if (page_url && !strncasecmp(page_url, "https://", 8) && !strncasecmp(effective, "ws://", 5) && strncasecmp(effective + 5, "localhost", 9) && strncmp(effective + 5, "127.", 4) && strncmp(effective + 5,"[::1]",5)) return 0;
    if (!websec_allows(effective,NAL_RESOURCE_CONNECT)) { gal_log("WARN","CSP: connect-src blocked WebSocket %s",effective); return 0; }
    ws_origin_of(page_url, g_ws_origin, sizeof g_ws_origin);
    int p2c[2], c2p[2];                                              /* p2c: родитель->ребёнок (команды); c2p: ребёнок->родитель (события) */
    if (pipe(p2c) < 0) return 0;
    if (pipe(c2p) < 0) { close(p2c[0]); close(p2c[1]); return 0; }
    pid_t pid = fork();
    if (pid < 0) { close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]); return 0; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        child_main(effective, c2p[1], p2c[0]);
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
#endif /* _WIN32 */
