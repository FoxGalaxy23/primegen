#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nhttp.h"
#include "ntls.h"
#include "ninflate.h"
#include "psl.h"
#include "idna.h"
#include "web_security.h"
#ifdef MLBROWSER_HAVE_CURL
#include <curl/curl.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DEF_UA "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
#define MAX_HEAD (128 * 1024)
#define CONNECT_MS 10000
#define IDLE_MS 30000
#define POOL_MAX 8
#define POOL_IDLE_MS 25000

static int64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static char* sdup(const char* s) { return s ? strdup(s) : NULL; }
static void set_err(char* dst, size_t cap, const char* msg) { snprintf(dst, cap, "%s", msg); }

/* ══════════════ динамическая строка ══════════════ */
typedef struct { char* p; size_t n, cap; } Str;
static int str_add(Str* s, const void* d, size_t n) {
    if (s->n + n + 1 > s->cap) { size_t nc = s->cap ? s->cap : 256; while (nc < s->n + n + 1) nc *= 2; char* nv = realloc(s->p, nc); if (!nv) return 0; s->p = nv; s->cap = nc; }
    memcpy(s->p + s->n, d, n); s->n += n; s->p[s->n] = 0; return 1;
}
static int str_adds(Str* s, const char* c) { return str_add(s, c, strlen(c)); }

/* ══════════════ URL ══════════════ */
typedef struct { int https; char host[256]; int port; char origin[300]; char pq[8192]; } Url;   /* pq — путь+запрос в исходном виде, всегда с '/' в начале */

static int parse_port(const char *s, int *port) {
    if (!s || !*s || !port) return 0;
    unsigned long value = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        value = value * 10 + (unsigned long)(*p - '0');
        if (value > 65535) return 0;
    }
    if (value == 0) return 0;
    *port = (int)value;
    return 1;
}

static int parse_url(const char* s, Url* u) {
    memset(u, 0, sizeof *u);
    if (!strncasecmp(s, "https://", 8)) { u->https = 1; s += 8; } else if (!strncasecmp(s, "http://", 7)) s += 7; else return 0;
    const char* a = s; while (*s && *s != '/' && *s != '?' && *s != '#') s++;
    size_t alen = (size_t)(s - a); if (!alen || alen > 280) return 0;
    char auth[300]; memcpy(auth, a, alen); auth[alen] = 0;
    char* at = strrchr(auth, '@'); char* hp = at ? at + 1 : auth;                  /* user:pass@ отбрасываем */
    char host[256]; int port = u->https ? 443 : 80;
    int bracketed_host = 0;
    if (*hp == '[') {
        bracketed_host = 1;
        char* e = strchr(hp, ']');
        if (!e) return 0;
        size_t hl = (size_t)(e - hp - 1);
        if (!hl || hl > 80) return 0;
        memcpy(host, hp + 1, hl); host[hl] = 0;
        struct in6_addr ipv6_addr;
        if (inet_pton(AF_INET6, host, &ipv6_addr) != 1) return 0;
        if (e[1] == ':') { if (!parse_port(e + 2, &port)) return 0; }
        else if (e[1] != 0) return 0; /* Не игнорируем мусор после IPv6-литерала. */
    } else {
        char* c = strchr(hp, ':');
        size_t hl = c ? (size_t)(c - hp) : strlen(hp);
        if (!hl || hl > 253) return 0;
        memcpy(host, hp, hl); host[hl] = 0;
        if (c && !parse_port(c + 1, &port)) return 0;
    }
    if (port <= 0 || port > 65535) return 0;
    for (char* p = host; *p; p++) { if ((unsigned char)*p <= 32 || *p == '/') return 0; *p = (char)tolower((unsigned char)*p); }
    if (!bracketed_host) { char ascii[256]; if (!nidna_to_ascii(host, ascii, sizeof ascii)) return 0; snprintf(host, sizeof host, "%s", ascii); }
    snprintf(u->host, sizeof u->host, "%s", host); u->port = port;
    int v6 = bracketed_host; int def = u->https ? port == 443 : port == 80;
    if (def) snprintf(u->origin, sizeof u->origin, "%s://%s%s%s", u->https ? "https" : "http", v6 ? "[" : "", host, v6 ? "]" : "");
    else snprintf(u->origin, sizeof u->origin, "%s://%s%s%s:%d", u->https ? "https" : "http", v6 ? "[" : "", host, v6 ? "]" : "", port);
    const char* h = strchr(s, '#'); size_t pl = h ? (size_t)(h - s) : strlen(s);
    if (pl >= sizeof u->pq - 2) return 0;
    if (*s == '/' ) { memcpy(u->pq, s, pl); u->pq[pl] = 0; }
    else if (*s == '?') { u->pq[0] = '/'; memcpy(u->pq + 1, s, pl); u->pq[pl + 1] = 0; }
    else strcpy(u->pq, "/");
    return 1;
}
/* цель запроса: байты вне печатного ASCII и пробелы кодируем, уже закодированное (%XX) не трогаем */
static int encode_target(const char* pq, char* out, size_t cap) {
    if (!pq || !out || cap == 0) return 0;
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)pq; *p; p++) {
        int encode = *p <= 32 || *p >= 127 || *p == '"' || *p == '<' || *p == '>' ||
                     *p == '\\' || *p == '^' || *p == '`' || *p == '{' || *p == '|' || *p == '}';
        size_t need = encode ? 3u : 1u;
        if (need > cap - o - 1) { out[0] = 0; return 0; }
        if (encode) {
            static const char hex[] = "0123456789ABCDEF";
            out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15];
        } else out[o++] = (char)*p;
    }
    out[o] = 0;
    return 1;
}

/* удаление сегментов «.» и «..» в части пути (до '?') */
static int remove_dots(char* pq) {
    if (!pq) return 0;
    char tail[8192] = "";
    char* q = strchr(pq, '?');
    if (q) {
        size_t tn = strlen(q);
        if (tn >= sizeof tail) return 0;
        memcpy(tail, q, tn + 1);
        *q = 0;
    }
    char* tmp = strdup(pq);
    if (!tmp) return 0;
    char* segs[512];
    int ns = 0;
    char* save = NULL;
    size_t plen = strlen(pq);
    int trailing = plen && pq[plen - 1] == '/';
    for (char* t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) { trailing = 1; continue; }
        if (!strcmp(t, "..")) { if (ns) ns--; trailing = 1; continue; }
        if (ns >= (int)(sizeof segs / sizeof segs[0])) { free(tmp); return 0; }
        segs[ns++] = t;
        trailing = 0;
    }
    char out[8192];
    size_t o = 0;
    out[o++] = '/';
    for (int i = 0; i < ns; i++) {
        size_t l = strlen(segs[i]);
        size_t need = l + (i + 1 < ns ? 1u : 0u);
        if (need > sizeof out - o - 1) { free(tmp); return 0; }
        memcpy(out + o, segs[i], l); o += l;
        if (i + 1 < ns) out[o++] = '/';
    }
    if (trailing && ns) {
        if (o + 1 >= sizeof out) { free(tmp); return 0; }
        out[o++] = '/';
    }
    if (strlen(tail) > sizeof out - o - 1) { free(tmp); return 0; }
    memcpy(out + o, tail, strlen(tail) + 1);
    memcpy(pq, out, o + strlen(tail) + 1);
    free(tmp);
    return 1;
}

/* Location относительно текущего URL → абсолютный. Любое усечение считается ошибкой:
   иначе редирект мог бы незаметно уйти на другой, усечённый URL. */
static int url_append(char *dst, size_t cap, size_t *used, const char *src, size_t n) {
    if (!dst || !used || !src || *used >= cap || n > cap - *used - 1) return 0;
    memcpy(dst + *used, src, n);
    *used += n;
    dst[*used] = 0;
    return 1;
}
static int resolve_location(const Url* base, const char* loc, char* out, size_t cap) {
    if (!base || !loc || !out || cap == 0) return 0;
    out[0] = 0;
    while (*loc == ' ') loc++;
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
        size_t n = strlen(loc);
        if (n >= cap) return 0;
        memcpy(out, loc, n + 1);
        return 1;
    }
    if (loc[0] == '/' && loc[1] == '/') {
        const char *scheme = base->https ? "https:" : "http:";
        size_t used = 0;
        out[0] = 0;
        return url_append(out, cap, &used, scheme, strlen(scheme)) &&
               url_append(out, cap, &used, loc, strlen(loc));
    }
    char pq[8192] = {0};
    const char* h = strchr(loc, '#');
    size_t ll = h ? (size_t)(h - loc) : strlen(loc);
    if (ll > 7000) return 0;
    char l[7001];
    memcpy(l, loc, ll);
    l[ll] = 0;
    size_t used = 0;
    if (l[0] == '/') {
        if (!url_append(pq, sizeof pq, &used, l, strlen(l))) return 0;
    } else if (l[0] == '?') {
        char path[8192];
        size_t n = strnlen(base->pq, sizeof path - 1);
        memcpy(path, base->pq, n); path[n] = 0;
        char* q = strchr(path, '?'); if (q) *q = 0;
        if (!url_append(pq, sizeof pq, &used, path, strlen(path)) ||
            !url_append(pq, sizeof pq, &used, l, strlen(l))) return 0;
    } else if (!l[0]) {
        if (!url_append(pq, sizeof pq, &used, base->pq, strnlen(base->pq, sizeof pq - 1))) return 0;
    } else {
        char dir[8192];
        size_t n = strnlen(base->pq, sizeof dir - 1);
        memcpy(dir, base->pq, n); dir[n] = 0;
        char* q = strchr(dir, '?'); if (q) *q = 0;
        char* sl = strrchr(dir, '/'); if (sl) sl[1] = 0;
        if (!url_append(pq, sizeof pq, &used, dir, strlen(dir)) ||
            !url_append(pq, sizeof pq, &used, l, strlen(l))) return 0;
    }
    if (!remove_dots(pq)) return 0;
    size_t origin_len = strlen(base->origin), pq_len = strlen(pq), out_used = 0;
    out[0] = 0;
    return url_append(out, cap, &out_used, base->origin, origin_len) &&
           url_append(out, cap, &out_used, pq, pq_len);
}

/* ══════════════ cookie jar (RFC 6265) ══════════════ */
typedef struct { char *name, *value, *domain, *path; long long expiry; int secure, host_only, dirty, seq, same_site; } Cookie;   /* expiry 0 = сессионная */
struct NJar { Cookie* c; int n, cap, seq; };

NJar* njar_new(void) { return calloc(1, sizeof(NJar)); }
static void cookie_free(Cookie* c) { free(c->name); free(c->value); free(c->domain); free(c->path); }
void njar_free(NJar* j) { if (!j) return; for (int i = 0; i < j->n; i++) cookie_free(&j->c[i]); free(j->c); free(j); }

static int domain_match(const char* host, const char* dom) {          /* dom без ведущей точки */
    size_t hl = strlen(host), dl = strlen(dom);
    if (hl == dl) return !strcasecmp(host, dom);
    return hl > dl && host[hl - dl - 1] == '.' && !strcasecmp(host + hl - dl, dom);
}
static int path_match(const char* req, const char* cp) {
    size_t rl = strlen(req), cl = strlen(cp);
    if (cl > rl || strncmp(req, cp, cl)) return 0;
    return rl == cl || cp[cl - 1] == '/' || req[cl] == '/';
}
static int is_ip_host(const char* h) { struct in_addr a; struct in6_addr b; return inet_pton(AF_INET, h, &a) == 1 || inet_pton(AF_INET6, h, &b) == 1; }

static Cookie* jar_find(NJar* j, const char* name, const char* dom, const char* path, int host_only) {
    for (int i = 0; i < j->n; i++) { Cookie* c = &j->c[i]; if (c->host_only == host_only && !strcmp(c->name, name) && !strcasecmp(c->domain, dom) && !strcmp(c->path, path)) return c; }
    return NULL;
}
static void jar_put(NJar* j, const char* name, const char* value, const char* dom, const char* path, long long expiry, int secure, int host_only, int dirty, int same_site) {
    Cookie* c = jar_find(j, name, dom, path, host_only);
    if (c) { free(c->value); c->value = strdup(value); c->expiry = expiry; c->secure = secure; c->same_site = same_site; c->dirty |= dirty; c->seq = j->seq++; return; }
    if (j->n >= 4000) return;
    if (j->n == j->cap) { int nc = j->cap ? j->cap * 2 : 32; Cookie* nv = realloc(j->c, (size_t)nc * sizeof(Cookie)); if (!nv) return; j->c = nv; j->cap = nc; }
    c = &j->c[j->n++]; c->name = strdup(name); c->value = strdup(value); c->domain = strdup(dom); c->path = strdup(path);
    c->expiry = expiry; c->secure = secure; c->host_only = host_only; c->dirty = dirty; c->seq = j->seq++; c->same_site = same_site;
}
void njar_load_netscape(NJar* j, const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return;
    char line[4096]; time_t now = time(NULL);
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char* fld[8]; int nf = 0; char* p = line;
        while (nf < 8) { fld[nf++] = p; char* t = strchr(p, '\t'); if (!t) break; *t = 0; p = t + 1; }
        if (nf < 6) continue;
        const char* dom = fld[0]; const char* cpath = fld[2]; int secure = !strcmp(fld[3], "TRUE"); long long exp = atoll(fld[4]);
        const char* name = fld[5]; const char* value = nf >= 7 ? fld[6] : ""; int same_site = nf >= 8 ? atoi(fld[7]) : 2; if (same_site < 1 || same_site > 3) same_site = 2;
        if (exp > 0 && exp < (long long)now) continue;
        int host_only = dom[0] != '.'; if (dom[0] == '.') dom++;
        if (!*dom || !*name) continue;
        jar_put(j, name, value, dom, cpath[0] ? cpath : "/", exp, secure, host_only, 0, same_site);
    }
    fclose(f);
}
int njar_save_changed(NJar* j, const char* path) {
    int n = 0; FILE* f = NULL;
    for (int i = 0; i < j->n; i++) {
        Cookie* c = &j->c[i]; if (!c->dirty) continue;
        if (strpbrk(c->name, "\t\r\n") || strpbrk(c->value, "\t\r\n") || strlen(c->value) > 1000 || strlen(c->name) > 250) continue;   /* формат БД не вместит */
        if (!f) { f = fopen(path, "w"); if (!f) return 0; fputs("# Netscape HTTP Cookie File\n", f); }
        fprintf(f, "%s%s\tTRUE\t%s\t%s\t%lld\t%s\t%s\t%d\n", c->host_only ? "" : ".", c->domain, c->path, c->secure ? "TRUE" : "FALSE", c->expiry, c->name, c->value, c->same_site); n++;
    }
    if (f) fclose(f);
    return n;
}
/* Полная выгрузка (для автономного использования jar в файле, без БД): все непросроченные куки */
int njar_save_all(NJar* j, const char* path) {
    for (int i = 0; i < j->n; i++) j->c[i].dirty = (j->c[i].expiry != 1);
    return njar_save_changed(j, path);
}
static long long parse_http_date(const char* s) {
    static const char* fmts[] = {"%a, %d %b %Y %H:%M:%S", "%a, %d-%b-%Y %H:%M:%S", "%a, %d-%b-%y %H:%M:%S", "%A, %d-%b-%y %H:%M:%S", "%a %b %d %H:%M:%S %Y", "%d %b %Y %H:%M:%S", "%a, %d %b %y %H:%M:%S", NULL};
    for (int i = 0; fmts[i]; i++) { struct tm tm; memset(&tm, 0, sizeof tm); if (strptime(s, fmts[i], &tm)) return (long long)timegm(&tm); }
    return -1;
}
static char* trim(char* s) { while (isspace((unsigned char)*s)) s++; char* e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }

static int default_path(const char* req_path_query, char* out, size_t cap) {
    if (!req_path_query || !out || cap < 2) return 0;
    size_t input_len = strnlen(req_path_query, 8192);
    if (input_len >= 8192) return 0;
    char p[8192]; memcpy(p, req_path_query, input_len + 1);
    char* q = strchr(p, '?'); if (q) *q = 0;
    char* sl = strrchr(p, '/');
    if (!sl || sl == p) { out[0] = '/'; out[1] = 0; return 1; }
    *sl = 0;
    size_t n = strlen(p);
    if (n >= cap) return 0;
    memcpy(out, p, n + 1);
    return 1;
}

static void jar_set_cookie(NJar* j, const Url* u, const char* hdr) {
    if (strlen(hdr) > 4300) return;
    char buf[4400]; snprintf(buf, sizeof buf, "%s", hdr);
    char* save = NULL; char* first = strtok_r(buf, ";", &save); if (!first) return;
    char* eq = strchr(first, '='); if (!eq) return;
    *eq = 0; char* name = trim(first); char* value = trim(eq + 1);
    if (!*name || strlen(name) + strlen(value) > 4096) return;
    char domain[256]; snprintf(domain, sizeof domain, "%s", u->host); int host_only = 1;
    char path[1024]; if (!default_path(u->pq, path, sizeof path)) return;
    long long expiry = 0; int have_maxage = 0, secure = 0, domain_attr = 0, path_attr = 0, same_site = 2; long long maxage = 0; time_t now = time(NULL);
    for (char* a = strtok_r(NULL, ";", &save); a; a = strtok_r(NULL, ";", &save)) {
        char* av = strchr(a, '='); char* an = a; if (av) { *av = 0; av++; av = trim(av); } an = trim(an);
        if (!strcasecmp(an, "domain") && av) {
            while (*av == '.') av++;
            if (!*av) continue;
            char d[256]; if (!nidna_to_ascii(av, d, sizeof d)) return;
            if (is_ip_host(u->host)) { if (strcmp(d, u->host)) return; }
            else {
                if (!domain_match(u->host, d)) return;                              /* чужой домен */
                if (psl_is_public_suffix(d)) { if (strcasecmp(u->host, d)) return; else continue; }   /* Domain=com / co.uk / github.io: хост ≠ суффикс — отвергаем, хост = суффикс — как host-only */
            }
            snprintf(domain, sizeof domain, "%s", d); host_only = 0; domain_attr = 1;
        } else if (!strcasecmp(an, "path") && av) {
            if (av[0] == '/') {
                size_t path_len = strlen(av);
                if (path_len >= sizeof path) return; /* Не превращаем длинный Path в более короткий путь. */
                memcpy(path, av, path_len + 1); path_attr = 1;
            }
        }
        else if (!strcasecmp(an, "max-age") && av) { char* e; long long v = strtoll(av, &e, 10); if (e != av && !*e) { have_maxage = 1; maxage = v; } }
        else if (!strcasecmp(an, "expires") && av) { long long t = parse_http_date(av); if (t >= 0) expiry = t; else expiry = 0; }
        else if (!strcasecmp(an, "secure")) secure = 1;
        else if (!strcasecmp(an, "samesite") && av) {
            if (!strcasecmp(av, "strict")) same_site = 1;
            else if (!strcasecmp(av, "lax")) same_site = 2;
            else if (!strcasecmp(av, "none")) same_site = 3;
        }
    }
    if (secure && !u->https) return;                                   /* Secure-куки с незащищённого origin не принимаем */
    if (same_site == 3 && !secure) return; /* SameSite=None требует Secure */
    if (!strncasecmp(name, "__Secure-", 9) && !(secure && u->https)) return;
    if (!strncasecmp(name, "__Host-", 7) && !(secure && u->https && !domain_attr && path_attr && !strcmp(path, "/"))) return;
    int deleted = 0;
    if (have_maxage) { if (maxage <= 0) deleted = 1; else expiry = (long long)now + maxage; }
    else if (expiry > 0 && expiry <= (long long)now) deleted = 1;
    if (deleted) { Cookie* c = jar_find(j, name, domain, path, host_only); if (c) { free(c->value); c->value = strdup(""); c->expiry = 1; c->dirty = 1; } else jar_put(j, name, "", domain, path, 1, secure, host_only, 1, same_site); return; }
    jar_put(j, name, value, domain, path, expiry, secure, host_only, 1, same_site);
}
static int cookie_cmp(const void* a, const void* b) {
    const Cookie* x = *(Cookie* const*)a; const Cookie* y = *(Cookie* const*)b;
    size_t lx = strlen(x->path), ly = strlen(y->path); if (lx != ly) return lx < ly ? 1 : -1; return x->seq - y->seq;
}
static int cookie_site_same(const Url *request, const char *site_url) {
    if (!site_url || !*site_url) return 0; /* неизвестный инициатор не считается same-site */
    Url site; if (!parse_url(site_url, &site)) return 0;
    if (request->https != site.https) return 0; /* schemeful SameSite */
    if (is_ip_host(request->host) || is_ip_host(site.host) || !strcasecmp(request->host, "localhost") || !strcasecmp(site.host, "localhost"))
        return !strcasecmp(request->host, site.host);
    char rd[256], sd[256];
    if (psl_registrable_domain(request->host, rd, sizeof rd) && psl_registrable_domain(site.host, sd, sizeof sd)) return !strcasecmp(rd, sd);
    return !strcasecmp(request->host, site.host);
}
/* значение заголовка Cookie для запроса; пустая строка — подходящих нет */
static void jar_header(NJar* j, const Url* u, const char* site_url, int top_level, const char* method, char* out, size_t cap) {
    out[0] = 0; if (!j) return;
    int same_site = cookie_site_same(u, site_url);
    int safe_method = !method || !strcasecmp(method, "GET") || !strcasecmp(method, "HEAD");
    Cookie* sel[256]; int ns = 0; time_t now = time(NULL); char path[8192]; snprintf(path, sizeof path, "%s", u->pq); char* q = strchr(path, '?'); if (q) *q = 0;
    for (int i = 0; i < j->n && ns < 256; i++) {
        Cookie* c = &j->c[i];
        if (c->expiry == 1 || (c->expiry > 0 && c->expiry < (long long)now)) continue;
        if (c->secure && !u->https) continue;
        if (c->host_only ? strcasecmp(c->domain, u->host) : !domain_match(u->host, c->domain)) continue;
        if (!path_match(path, c->path)) continue;
        if (c->same_site == 1 && !same_site) continue; /* Strict */
        if (c->same_site == 2 && !same_site && !(top_level && safe_method)) continue; /* Lax */
        sel[ns++] = c;
    }
    qsort(sel, (size_t)ns, sizeof sel[0], cookie_cmp);
    size_t o = 0;
    for (int i = 0; i < ns; i++) {
        char item[4352];
        int w = snprintf(item, sizeof item, "%s%s=%s", o ? "; " : "", sel[i]->name, sel[i]->value);
        if (w < 0 || (size_t)w >= sizeof item || (size_t)w >= cap - o) break;
        memcpy(out + o, item, (size_t)w + 1);
        o += (size_t)w;
    }
}

/* ══════════════ соединения и пул ══════════════ */
typedef struct Conn {
    int fd; NTls* tls; int https; char host[256]; int port; pid_t pid; int64_t last_used;
    unsigned char buf[16384]; size_t pos, len;
} Conn;
static Conn* g_pool[POOL_MAX];

static void conn_destroy(Conn* c) {
    if (!c) return;
    int own = c->pid == getpid();                                    /* соединение, унаследованное через fork, принадлежит родителю: close_notify не шлём */
    if (c->tls) ntls_close(c->tls, own);
    if (c->fd >= 0) close(c->fd);
    free(c);
}
void nhttp_pool_clear(void) { for (int i = 0; i < POOL_MAX; i++) { conn_destroy(g_pool[i]); g_pool[i] = NULL; } }

static void set_timeouts(int fd, int ms) {
    if (ms < 1) ms = 1;
    struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
static int remaining_ms(int64_t deadline) { int64_t r = deadline - now_ms(); return r < 0 ? 0 : r > 0x7fffffff ? 0x7fffffff : (int)r; }
static int io_timeout(int64_t deadline) { int r = remaining_ms(deadline); return r > IDLE_MS ? IDLE_MS : r; }

static Conn* pool_take(const Url* u) {
    int64_t t = now_ms();
    for (int i = 0; i < POOL_MAX; i++) {
        Conn* c = g_pool[i]; if (!c) continue;
        if (c->pid != getpid()) { g_pool[i] = NULL; conn_destroy(c); continue; }       /* чужой (после fork) — выбрасываем, не используем */
        if (t - c->last_used > POOL_IDLE_MS) { g_pool[i] = NULL; conn_destroy(c); continue; }
        if (c->https == u->https && c->port == u->port && !strcmp(c->host, u->host)) {
            struct pollfd p = {c->fd, POLLIN, 0};                    /* что-то пришло на простаивающее соединение (EOF/мусор) — оно негодно */
            g_pool[i] = NULL;
            if (poll(&p, 1, 0) != 0) { conn_destroy(c); continue; }
            c->pos = c->len = 0; return c;
        }
    }
    return NULL;
}
static void pool_put(Conn* c) {
    c->last_used = now_ms(); c->pos = c->len = 0;
    for (int i = 0; i < POOL_MAX; i++) if (!g_pool[i]) { g_pool[i] = c; return; }
    int oldest = 0; for (int i = 1; i < POOL_MAX; i++) if (g_pool[i]->last_used < g_pool[oldest]->last_used) oldest = i;
    conn_destroy(g_pool[oldest]); g_pool[oldest] = c;
}

static Conn* conn_open(const Url* u, int64_t deadline, char* err, size_t ecap) {
    static int sigpipe_done = 0;
    if (!sigpipe_done) { struct sigaction sa; if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN); sigpipe_done = 1; }
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    char port[8]; snprintf(port, sizeof port, "%d", u->port);
    int gai = getaddrinfo(u->host, port, &hints, &res);
    if (gai != 0 || !res) { snprintf(err, ecap, "DNS: %.60s (%.100s)", gai ? gai_strerror(gai) : "нет адресов", u->host); return NULL; }
    int fd = -1; int lasterr = 0;
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        int left = remaining_ms(deadline); if (left <= 0) { lasterr = ETIMEDOUT; break; }
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol); if (fd < 0) { lasterr = errno; continue; }
        int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0}; int w = left < CONNECT_MS ? left : CONNECT_MS;
            rc = poll(&p, 1, w);
            if (rc == 1) { int so = 0; socklen_t sl = sizeof so; getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl); if (so == 0) rc = 0; else { errno = so; rc = -1; } }
            else { errno = rc == 0 ? ETIMEDOUT : errno; rc = -1; }
        }
        if (rc == 0) { fcntl(fd, F_SETFL, fl); break; }
        lasterr = errno; close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { snprintf(err, ecap, "соединение с %.80s:%d не установлено: %.45s", u->host, u->port, strerror(lasterr ? lasterr : ECONNREFUSED)); return NULL; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    Conn* c = calloc(1, sizeof *c); if (!c) { close(fd); set_err(err, ecap, "нет памяти"); return NULL; }
    c->fd = fd; c->https = u->https; c->port = u->port; c->pid = getpid(); snprintf(c->host, sizeof c->host, "%s", u->host);
    if (u->https) {
        set_timeouts(fd, io_timeout(deadline));
        c->tls = ntls_connect(fd, u->host, err, ecap);
        if (!c->tls) { close(fd); free(c); return NULL; }
    }
    return c;
}

/* ══════════════ ввод-вывод ══════════════ */
static int conn_send(Conn* c, const void* d, size_t n, int64_t deadline) {
    set_timeouts(c->fd, io_timeout(deadline));
    if (c->tls) return ntls_write(c->tls, d, n) == (int)n ? 1 : 0;
    size_t done = 0;
    while (done < n) {
        ssize_t w = send(c->fd, (const char*)d + done, n - done, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return 0; }
        done += (size_t)w;
    }
    return 1;
}
/* >0 — прочитано, 0 — EOF, -1 — ошибка, -2 — таймаут */
static int conn_fill(Conn* c, int64_t deadline) {
    if (remaining_ms(deadline) <= 0) return -2;
    set_timeouts(c->fd, io_timeout(deadline));
    int r;
    if (c->tls) r = ntls_read(c->tls, c->buf, sizeof c->buf);
    else { do { r = (int)recv(c->fd, c->buf, sizeof c->buf, 0); } while (r < 0 && errno == EINTR); if (r < 0) r = (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1; }
    if (r > 0) { c->pos = 0; c->len = (size_t)r; }
    return r;
}
static int conn_getc(Conn* c, int64_t deadline, int* status) {
    if (c->pos >= c->len) { int r = conn_fill(c, deadline); if (r <= 0) { *status = r; return -1; } }
    return c->buf[c->pos++];
}
/* читает до n байт, не больше, чем есть в буфере или придёт одним чтением */
static int conn_read_some(Conn* c, unsigned char* dst, size_t n, int64_t deadline) {
    if (c->pos >= c->len) { int r = conn_fill(c, deadline); if (r <= 0) return r; }
    size_t k = c->len - c->pos; if (k > n) k = n; memcpy(dst, c->buf + c->pos, k); c->pos += k; return (int)k;
}
/* строка без CRLF; 1 — ok, 0 — EOF до конца строки, -1 — ошибка/таймаут/слишком длинная */
static int conn_getline(Conn* c, char* line, size_t cap, int64_t deadline) {
    size_t n = 0;
    for (;;) {
        int st = 0; int ch = conn_getc(c, deadline, &st); if (ch < 0) return st == 0 ? 0 : -1;
        if (ch == '\n') { if (n && line[n - 1] == '\r') n--; line[n] = 0; return 1; }
        if (n + 1 < cap) line[n++] = (char)ch; else return -1;
    }
}

/* ══════════════ разбор заголовков ══════════════ */
/* значение заголовка name из блока (без строки статуса); 1 — найден */
static int hdr_get(const char* block, const char* name, char* out, size_t cap) {
    size_t nl = strlen(name);
    for (const char* p = block; *p;) {
        const char* e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char* v = p + nl + 1; while (v < p + ll && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)(p + ll - v); while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl); out[vl] = 0; return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
/* Variant for security-sensitive values: -1 means the value did not fit. */
static int hdr_get_checked(const char* block, const char* name, char* out, size_t cap) {
    if (!block || !name || !out || cap == 0) return -1;
    size_t nl = strlen(name);
    for (const char* p = block; *p;) {
        const char* e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char* v = p + nl + 1;
            while (v < p + ll && (*v == ' ' || *v == '\t')) v++;
            size_t vl = (size_t)(p + ll - v);
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (vl >= cap) { out[0] = 0; return -1; }
            memcpy(out, v, vl); out[vl] = 0; return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    out[0] = 0;
    return 0;
}
static int has_token(const char* v, const char* tok) {            /* список через запятую, без учёта регистра */
    size_t tl = strlen(tok);
    for (const char* p = v; *p;) {
        while (*p == ' ' || *p == ',') p++;
        if (!strncasecmp(p, tok, tl) && (p[tl] == 0 || p[tl] == ',' || p[tl] == ' ' || p[tl] == ';')) return 1;
        while (*p && *p != ',') p++;
    }
    return 0;
}
/* убирает из блока заголовков строки с перечисленными именами */
static void hdr_strip(char* block, const char* const* names) {
    char* w = block; const char* p = block;
    while (*p) {
        const char* e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p) + 1 : strlen(p); int drop = 0;
        for (int i = 0; names[i]; i++) { size_t nl = strlen(names[i]); if (ll > nl && !strncasecmp(p, names[i], nl) && p[nl] == ':') { drop = 1; break; } }
        if (!drop) { memmove(w, p, ll); w += ll; }
        p += ll;
    }
    *w = 0;
}

typedef struct { int status; int http11; char* head; Str body; int keepalive; size_t sunk; int truncated; } RawResp;
static void raw_free(RawResp* r) { free(r->head); free(r->body.p); memset(r, 0, sizeof *r); }

/* Тело ответа. sink != NULL — пишем в файл. 1 — ok, 0 — ошибка (err заполнен) */
static int read_body(Conn* c, RawResp* r, const char* method, FILE* sink, int discard, size_t max_body, int64_t deadline, char* err, size_t ecap) {
    char v[256]; int no_body = !strcasecmp(method, "HEAD") || r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200);
    if (no_body) return 1;
    unsigned char tmp[16384];
    #define EMIT(ptr, len) do { if ((len) > 0) { if (r->sunk + r->body.n + (len) > max_body) { set_err(err, ecap, "ответ больше допустимого размера"); return 0; } \
        if (sink || discard) { if (sink && !discard && fwrite((ptr), 1, (len), sink) != (size_t)(len)) { set_err(err, ecap, "ошибка записи в файл"); return 0; } r->sunk += (len); } \
        else if (!str_add(&r->body, (ptr), (len))) { set_err(err, ecap, "нет памяти"); return 0; } } } while (0)
    if (hdr_get(r->head, "Transfer-Encoding", v, sizeof v) && has_token(v, "chunked")) {
        for (;;) {
            char line[512]; int lr = conn_getline(c, line, sizeof line, deadline);
            if (lr <= 0) { set_err(err, ecap, lr == 0 ? "соединение закрыто посреди chunked-ответа" : "таймаут/ошибка чтения chunked"); return 0; }
            char* semi = strchr(line, ';'); if (semi) *semi = 0;
            char* endp; unsigned long long sz = strtoull(line, &endp, 16); if (endp == line) { set_err(err, ecap, "неверный размер chunk"); return 0; }
            if (sz == 0) { for (;;) { lr = conn_getline(c, line, sizeof line, deadline); if (lr <= 0) break; if (!line[0]) break; } break; }   /* трейлеры */
            while (sz > 0) {
                int k = conn_read_some(c, tmp, sz < sizeof tmp ? (size_t)sz : sizeof tmp, deadline);
                if (k <= 0) { set_err(err, ecap, "обрыв при чтении chunk"); return 0; }
                EMIT(tmp, (size_t)k); sz -= (unsigned long long)k;
            }
            lr = conn_getline(c, line, sizeof line, deadline); if (lr <= 0) { set_err(err, ecap, "нет CRLF после chunk"); return 0; }
        }
        return 1;
    }
    if (hdr_get(r->head, "Content-Length", v, sizeof v)) {
        char* e; unsigned long long len = strtoull(v, &e, 10);
        if (e == v) { set_err(err, ecap, "неверный Content-Length"); return 0; }
        if (len > max_body) { set_err(err, ecap, "ответ больше допустимого размера"); return 0; }
        while (len > 0) {
            int k = conn_read_some(c, tmp, len < sizeof tmp ? (size_t)len : sizeof tmp, deadline);
            if (k <= 0) { set_err(err, ecap, k == 0 ? "ответ оборван (меньше Content-Length)" : "таймаут чтения тела"); return 0; }
            EMIT(tmp, (size_t)k); len -= (unsigned long long)k;
        }
        return 1;
    }
    for (;;) {                                                         /* тело до закрытия соединения */
        int k = conn_read_some(c, tmp, sizeof tmp, deadline);
        if (k == 0) break;
        if (k < 0) { set_err(err, ecap, "таймаут/ошибка чтения тела"); return 0; }
        EMIT(tmp, (size_t)k);
    }
    r->keepalive = 0; return 1;
    #undef EMIT
}

/* один запрос по уже разобранному URL. retry_stale: при обрыве на переиспользованном соединении до первого байта ответа — повтор на новом */
#ifdef MLBROWSER_HAVE_CURL
/* libcurl transport. It negotiates ALPN (HTTP/2), HTTP/3 where the linked libcurl has QUIC,
   supports environment/configured proxies and performs gzip/deflate/Brotli decoding. Redirects,
   CORS, cookies and the browser cache remain owned by the surrounding MLBrowser code. */
typedef struct {
    RawResp *resp;
    Str head;
    size_t max_body;
    const char *method;
    const NHttpReq *rq;
    char response_url[NHTTP_URL_MAX];
    FILE *sink;
    int discard_body;
    int callback_error;
    int headers_called;
    char error[200];
} CurlCtx;
static int curl_ready;
static int curl_init_once(void) {
    if (curl_ready) return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
    curl_ready = 1;
    return 1;
}
static size_t curl_header_write(char *ptr, size_t size, size_t nmemb, void *opaque) {
    CurlCtx *c = (CurlCtx*)opaque; size_t n = size * nmemb;
    if (!n) return 0;
    if (n > MAX_HEAD || c->head.n > MAX_HEAD - n) {
        c->callback_error = 1; set_err(c->error, sizeof c->error, "заголовки ответа слишком велики"); return 0;
    }
    if (n >= 5 && !strncasecmp(ptr, "HTTP/", 5)) {
        free(c->head.p); memset(&c->head, 0, sizeof c->head);
        char line[128]; size_t cp = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, ptr, cp); line[cp] = 0; const char *sp = strchr(line, ' ');
        c->resp->status = sp ? atoi(sp + 1) : 0;
    }
    if (!str_add(&c->head, ptr, n)) {
        c->callback_error = 1; set_err(c->error, sizeof c->error, "нет памяти для заголовков ответа"); return 0;
    }
    if ((n == 2 && ptr[0] == '\r' && ptr[1] == '\n') || (n == 1 && ptr[0] == '\n')) {
        int st = c->resp->status;
        c->discard_body = c->sink && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308);
        /* Preserve nhttp's streaming contract: deliver the final response headers before body
           bytes, and let ncors reject the response before exposing it to JavaScript. */
        if (c->rq && c->rq->headers_cb && st >= 200 &&
            (!(st == 301 || st == 302 || st == 303 || st == 307 || st == 308) || c->rq->max_redirects < 0)) {
            if (!c->rq->headers_cb(c->head.p ? c->head.p : "", c->response_url,
                                   c->rq->headers_user, c->error, sizeof c->error)) {
                c->callback_error = 1;
                if (!c->error[0]) set_err(c->error, sizeof c->error, "заголовки отклонены сетевой политикой");
                return 0;
            }
            c->headers_called = 1;
        }
    }
    return n;
}
static size_t curl_body_write(char *ptr, size_t size, size_t nmemb, void *opaque) {
    CurlCtx *c = (CurlCtx*)opaque; size_t n = size * nmemb; RawResp *r = c->resp;
    if (!n) return 0;
    if (c->discard_body || !strcasecmp(c->method, "HEAD") || r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200)) return n;
    size_t have = r->sunk + r->body.n;
    if (n > c->max_body || have > c->max_body - n) {
        c->callback_error = 1; set_err(c->error, sizeof c->error, "ответ больше допустимого размера"); return 0;
    }
    if (c->sink) {
        if (fwrite(ptr, 1, n, c->sink) != n) {
            c->callback_error = 1; set_err(c->error, sizeof c->error, "ошибка записи в файл"); return 0;
        }
        r->sunk += n;
    } else if (!str_add(&r->body, ptr, n)) {
        c->callback_error = 1; set_err(c->error, sizeof c->error, "нет памяти для тела ответа"); return 0;
    }
    return n;
}
static int curl_header_exists(const char *headers, const char *name) {
    char value[8]; return headers && hdr_get(headers, name, value, sizeof value);
}
static struct curl_slist *curl_add_header(struct curl_slist *list, const char *line) {
    if (!line || !*line || strpbrk(line, "\r\n")) return list;
    return curl_slist_append(list, line);
}
static int do_request_curl(const NHttpReq* rq, const Url* u, const char* method, const char* extra,
                           const unsigned char* body, size_t blen, int64_t deadline, RawResp* r,
                           char* err, size_t ecap) {
    if (!curl_init_once()) { set_err(err, ecap, "не удалось инициализировать libcurl"); return 0; }
    CURL *easy = curl_easy_init(); if (!easy) { set_err(err, ecap, "не удалось создать HTTP-сеанс"); return 0; }
    CURLcode cc = CURLE_OK; struct curl_slist *headers = NULL; char line[1024]; char url[NHTTP_URL_MAX];
    int64_t left = deadline - now_ms(); if (left < 1) left = 1;
    if (snprintf(url, sizeof url, "%s%s", u->origin, u->pq) >= (int)sizeof url) {
        set_err(err, ecap, "URL слишком длинный"); curl_easy_cleanup(easy); return 0;
    }
    int user_ae = curl_header_exists(extra, "Accept-Encoding");
    int user_range = curl_header_exists(extra, "Range");
    if (!curl_header_exists(extra, "User-Agent")) {
        snprintf(line, sizeof line, "User-Agent: %s", rq->user_agent ? rq->user_agent : DEF_UA);
        headers = curl_add_header(headers, line);
    }
    if (!curl_header_exists(extra, "Accept")) headers = curl_add_header(headers, "Accept: */*");
    if (!curl_header_exists(extra, "Accept-Language")) headers = curl_add_header(headers, "Accept-Language: en-US,en;q=0.9");
    if (!curl_header_exists(extra, "Connection")) headers = curl_add_header(headers, "Connection: keep-alive");
    if (rq->jar && !curl_header_exists(extra, "Cookie")) {
        char ck[8192]; jar_header(rq->jar, u, rq->site_for_cookies, rq->is_top_level_navigation, method, ck, sizeof ck);
        if (ck[0]) { snprintf(line, sizeof line, "Cookie: %s", ck); headers = curl_add_header(headers, line); }
    }
    if (extra) {
        char *dup = strdup(extra), *save = NULL;
        if (!dup) { set_err(err, ecap, "нет памяти для заголовков запроса"); goto fail; }
        for (char *ln = strtok_r(dup, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            size_t n = strlen(ln); while (n && ln[n-1] == '\r') ln[--n] = 0;
            if (!n || !strchr(ln, ':') || strpbrk(ln, "\r\n")) continue;
            if (!strncasecmp(ln, "Host:", 5) || !strncasecmp(ln, "Content-Length:", 15) || !strncasecmp(ln, "Transfer-Encoding:", 18)) continue;
            struct curl_slist *nl = curl_slist_append(headers, ln);
            if (!nl) { free(dup); set_err(err, ecap, "нет памяти для заголовков запроса"); goto fail; }
            headers = nl;
        }
        free(dup);
    }
    CurlCtx ctx; memset(&ctx, 0, sizeof ctx); ctx.resp = r; ctx.max_body = rq->max_body ? rq->max_body : ((size_t)256 << 20); ctx.method = method; ctx.rq = rq; ctx.sink = rq->sink;
    snprintf(ctx.response_url, sizeof ctx.response_url, "%s", url);
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
    if (!strcasecmp(method, "HEAD")) curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_header_write);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_body_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L); /* redirects must pass through our HSTS/cookie policy */
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 0L);
#if LIBCURL_VERSION_NUM >= 0x073600
    curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
#endif
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, left < CONNECT_MS ? (long)left : CONNECT_MS);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, left > LONG_MAX ? LONG_MAX : (long)left);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    /* libcurl verifies certificates/hostnames by default; do not weaken TLS verification. */
    if (body && blen) {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, (const char*)body);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)blen);
    } else if (!strcasecmp(method, "POST") || !strcasecmp(method, "PUT") || !strcasecmp(method, "PATCH")) {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ""); curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
    }
    int auto_decode = !user_ae;
    if (!user_ae) {
        const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
        const char *enc = "gzip, deflate";
        /* Byte ranges address the selected representation. Request identity coding so
           decompression cannot shift offsets or turn a partial compressed stream invalid. */
        if (rq->sink || user_range) enc = "identity";
        else if (vi && (vi->features & CURL_VERSION_BROTLI)) enc = "gzip, deflate, br";
#ifdef CURL_VERSION_ZSTD
        if (!rq->sink && !user_range && vi && (vi->features & CURL_VERSION_ZSTD)) enc = (vi->features & CURL_VERSION_BROTLI) ? "gzip, deflate, br, zstd" : "gzip, deflate, zstd";
#endif
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, enc);
    } else auto_decode = 0;
    /* Explicit app override wins over the process environment; without it libcurl honors
       HTTP_PROXY/HTTPS_PROXY/ALL_PROXY and NO_PROXY in the usual way. */
    const char *proxy = getenv("MLBROWSER_PROXY"), *noproxy = getenv("MLBROWSER_NOPROXY");
    if (proxy) curl_easy_setopt(easy, CURLOPT_PROXY, proxy);
    if (noproxy) curl_easy_setopt(easy, CURLOPT_NOPROXY, noproxy);
    const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
#if LIBCURL_VERSION_NUM >= 0x074200
    if (u->https && vi && (vi->features & CURL_VERSION_HTTP3)) curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3);
    else
#endif
    if (u->https && vi && (vi->features & CURL_VERSION_HTTP2)) curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    else curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    cc = curl_easy_perform(easy);
    if (cc != CURLE_OK) {
        if (ctx.callback_error) set_err(err, ecap, ctx.error);
        else snprintf(err, ecap, "libcurl: %s", curl_easy_strerror(cc));
        goto fail;
    }
    { long status = 0; curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status); if (status > 0) r->status = (int)status; }
    r->head = ctx.head.p; ctx.head.p = NULL;
    if (!r->head) r->head = strdup("");
    /* The write callback receives decoded bytes when CURLOPT_ACCEPT_ENCODING is active.
       Remove stale metadata so callers and the disk cache don't try to decode a second time. */
    if (auto_decode && r->head) {
        char ce[128] = ""; if (hdr_get(r->head, "Content-Encoding", ce, sizeof ce) && strcasecmp(ce, "identity")) {
            const char *drop[] = {"Content-Encoding", "Content-Length", "Transfer-Encoding", NULL}; hdr_strip(r->head, drop);
        } else { const char *drop[] = {"Transfer-Encoding", NULL}; hdr_strip(r->head, drop); }
    } else if (r->head) { const char *drop[] = {"Transfer-Encoding", NULL}; hdr_strip(r->head, drop); }
    free(ctx.head.p); curl_slist_free_all(headers); curl_easy_cleanup(easy); return 1;
fail:
    free(ctx.head.p); curl_slist_free_all(headers); curl_easy_cleanup(easy); raw_free(r); return 0;
}
#endif /* MLBROWSER_HAVE_CURL */

static int do_request_native(const NHttpReq* rq, const Url* u, const char* method, const char* extra, const unsigned char* body, size_t blen,
                             int64_t deadline, RawResp* r, char* err, size_t ecap) {
    char target[sizeof u->pq * 3];
    if (!encode_target(u->pq, target, sizeof target)) { set_err(err, ecap, "путь URL после кодирования слишком длинный"); return 0; }
    for (int attempt = 0; attempt < 2; attempt++) {
        Conn* c = pool_take(u); int reused = c != NULL;
        if (!c) { c = conn_open(u, deadline, err, ecap); if (!c) return 0; }
        /* заголовки запроса */
        Str q = {0};
        char hostline[320]; int v6 = strchr(u->host, ':') != NULL; int defport = u->https ? u->port == 443 : u->port == 80;
        if (defport) snprintf(hostline, sizeof hostline, "%s%s%s", v6 ? "[" : "", u->host, v6 ? "]" : ""); else snprintf(hostline, sizeof hostline, "%s%s%s:%d", v6 ? "[" : "", u->host, v6 ? "]" : "", u->port);
        char l[512];
        str_adds(&q, method); str_adds(&q, " "); str_adds(&q, target); str_adds(&q, " HTTP/1.1\r\nHost: "); str_adds(&q, hostline); str_adds(&q, "\r\n");   /* без промежуточного буфера: длинные URL не обрезаются */
        #define USER_HAS(n) (extra && ({ char _t[8]; hdr_get(extra, n, _t, sizeof _t) || 0; }))
        int range_request = USER_HAS("Range");
        if (!USER_HAS("User-Agent")) { snprintf(l, sizeof l, "User-Agent: %s\r\n", rq->user_agent ? rq->user_agent : DEF_UA); str_adds(&q, l); }
        if (!USER_HAS("Accept")) str_adds(&q, "Accept: */*\r\n");
        if (!USER_HAS("Accept-Language")) str_adds(&q, "Accept-Language: en-US,en;q=0.9\r\n");
        if (!USER_HAS("Accept-Encoding")) str_adds(&q, (rq->sink || range_request) ? "Accept-Encoding: identity\r\n" : "Accept-Encoding: gzip, deflate\r\n");
        if (!USER_HAS("Connection")) str_adds(&q, "Connection: keep-alive\r\n");
        if (rq->jar && !USER_HAS("Cookie")) { char ck[8192]; jar_header(rq->jar, u, rq->site_for_cookies, rq->is_top_level_navigation, method, ck, sizeof ck); if (ck[0]) { str_adds(&q, "Cookie: "); str_adds(&q, ck); str_adds(&q, "\r\n"); } }
        if (blen || !strcmp(method, "POST") || !strcmp(method, "PUT") || !strcmp(method, "PATCH")) { snprintf(l, sizeof l, "Content-Length: %zu\r\n", blen); str_adds(&q, l); }
        #undef USER_HAS
        if (extra) {
            char* dup = strdup(extra); char* save = NULL;
            for (char* ln = strtok_r(dup, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
                size_t k = strlen(ln); while (k && (ln[k - 1] == '\r')) ln[--k] = 0;
                if (!k || !strchr(ln, ':') || strpbrk(ln, "\r")) continue;
                if (!strncasecmp(ln, "Host:", 5) || !strncasecmp(ln, "Content-Length:", 15) || !strncasecmp(ln, "Transfer-Encoding:", 18)) continue;   /* их формируем сами */
                str_adds(&q, ln); str_adds(&q, "\r\n");
            }
            free(dup);
        }
        str_adds(&q, "\r\n");
        int ok = conn_send(c, q.p, q.n, deadline) && (!blen || conn_send(c, body, blen, deadline));
        free(q.p);
        if (!ok) { conn_destroy(c); if (reused && attempt == 0) continue; set_err(err, ecap, "ошибка отправки запроса"); return 0; }
        /* ответ: пропускаем 1xx (кроме 101) */
        int got_any = 0; memset(r, 0, sizeof *r);
        for (;;) {
            char line[8192]; int lr = conn_getline(c, line, sizeof line, deadline);
            if (lr != 1) {
                conn_destroy(c);
                if (!got_any && reused && attempt == 0) goto retry;
                set_err(err, ecap, lr == 0 ? "сервер закрыл соединение без ответа" : "таймаут/ошибка при ожидании ответа"); return 0;
            }
            got_any = 1;
            if (strncmp(line, "HTTP/", 5)) { conn_destroy(c); set_err(err, ecap, "ответ не похож на HTTP"); return 0; }
            r->http11 = !strncmp(line, "HTTP/1.1", 8); const char* sp = strchr(line, ' '); r->status = sp ? atoi(sp + 1) : 0;
            Str h = {0}; str_adds(&h, line); str_adds(&h, "\r\n"); int bad = 0;
            for (;;) {
                lr = conn_getline(c, line, sizeof line, deadline);
                if (lr != 1) { bad = 1; break; }
                if (!line[0]) break;
                if (h.n + strlen(line) > MAX_HEAD) { bad = 1; break; }
                str_adds(&h, line); str_adds(&h, "\r\n");
            }
            if (bad) { free(h.p); conn_destroy(c); set_err(err, ecap, "заголовки ответа оборваны или слишком велики"); return 0; }
            if (r->status >= 100 && r->status < 200 && r->status != 101) { free(h.p); continue; }
            r->head = h.p; break;
        }
        /* Доставляем финальные заголовки сразу после их разбора, до чтения тела.
           Ответы с Location не публикуем, пока не пройдены редиректы. */
        char early_loc[4096] = "";
        int early_redirect = (r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308) && hdr_get(r->head, "Location", early_loc, sizeof early_loc);
        if (rq->headers_cb && (!early_redirect || rq->max_redirects < 0)) {
            char response_url[9000];
            snprintf(response_url, sizeof response_url, "%s%s", u->origin, u->pq);
            if (!rq->headers_cb(r->head ? r->head : "", response_url, rq->headers_user, err, ecap)) { raw_free(r); conn_destroy(c); return 0; }
        }
        char conn_hdr[128] = ""; hdr_get(r->head, "Connection", conn_hdr, sizeof conn_hdr);
        r->keepalive = r->http11 ? !has_token(conn_hdr, "close") : has_token(conn_hdr, "keep-alive");
        /* Bodies of followed redirects must be consumed from the socket but never enter the final response pipe. */
        int discard_redirect_body = early_redirect && rq->max_redirects >= 0 && rq->sink != NULL;
        if (!read_body(c, r, method, rq->sink, discard_redirect_body, rq->max_body ? rq->max_body : (size_t)256 << 20, deadline, err, ecap)) { raw_free(r); conn_destroy(c); return 0; }
        if (r->keepalive) pool_put(c); else conn_destroy(c);
        return 1;
        retry: ;
    }
    set_err(err, ecap, "не удалось выполнить запрос"); return 0;
}

static int do_request(const NHttpReq* rq, const Url* u, const char* method, const char* extra, const unsigned char* body, size_t blen,
                      int64_t deadline, RawResp* r, char* err, size_t ecap) {
#ifdef MLBROWSER_HAVE_CURL
    const char *transport = getenv("MLBROWSER_NET");
    if (!transport || strcmp(transport, "native"))
        return do_request_curl(rq, u, method, extra, body, blen, deadline, r, err, ecap);
#endif
    return do_request_native(rq, u, method, extra, body, blen, deadline, r, err, ecap);
}

/* ══════════════ HTTP-кэш ══════════════
   Файл записи cache/http/<хэш url>.e:  «MLC2\n<url>\n<status>\n<stored>\n<lifetime>\n<headlen>\n<bodylen>\n» + заголовки + тело (как пришли по сети,
   т. е. возможно со сжатием — декодирование делает общий код после запроса). Запись — через временный файл и rename (безопасно для нескольких процессов). */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
static char g_cdir[256]; static size_t g_cmax = 256u << 20; static int g_cmode = NHTTP_CACHE_NORMAL; static int g_cstores;
void nhttp_cache_config(const char* dir, size_t max_bytes) {
    if (!dir || !*dir) { g_cdir[0] = 0; return; }
    snprintf(g_cdir, sizeof g_cdir, "%s", dir); if (max_bytes) g_cmax = max_bytes;
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s", dir);
    for (char* p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    mkdir(tmp, 0777);
}
void nhttp_cache_policy(int p) { g_cmode = p; }
static unsigned long long fnv64(const char* s) { unsigned long long h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }
static void cpath(const char* url, char* out, size_t cap) { snprintf(out, cap, "%s/%016llx.e", g_cdir, fnv64(url)); }
void nhttp_cache_clear(void) {
    if (!g_cdir[0]) return; DIR* d = opendir(g_cdir); if (!d) return; struct dirent* e;
    while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 2 && !strcmp(e->d_name + l - 2, ".e")) { char p[400]; snprintf(p, sizeof p, "%s/%s", g_cdir, e->d_name); unlink(p); } }
    closedir(d);
}
void nhttp_cache_stats(size_t* entries, size_t* bytes) {
    size_t n = 0, b = 0; if (g_cdir[0]) { DIR* d = opendir(g_cdir); struct dirent* e;
        if (d) { while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 2 && !strcmp(e->d_name + l - 2, ".e")) { char p[400]; struct stat st; snprintf(p, sizeof p, "%s/%s", g_cdir, e->d_name); if (!stat(p, &st)) { n++; b += (size_t)st.st_size; } } } closedir(d); } }
    if (entries) *entries = n; if (bytes) *bytes = b;
}
static void cache_evict(void) {                                                            /* LRU по mtime, пока размер не упадёт до 80 % предела */
    DIR* d = opendir(g_cdir); if (!d) return;
    typedef struct { char p[400]; time_t t; size_t sz; } Ent; Ent* v = NULL; int n = 0, cap = 0; size_t total = 0; struct dirent* e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name); if (l <= 2 || strcmp(e->d_name + l - 2, ".e")) continue;
        if (n == cap) { int nc = cap ? cap * 2 : 256; Ent* nv = realloc(v, (size_t)nc * sizeof(Ent)); if (!nv) break; v = nv; cap = nc; }
        struct stat st; snprintf(v[n].p, sizeof v[n].p, "%s/%s", g_cdir, e->d_name); if (stat(v[n].p, &st)) continue; v[n].t = st.st_mtime; v[n].sz = (size_t)st.st_size; total += v[n].sz; n++;
    }
    closedir(d);
    if (total > g_cmax) {
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (v[j].t < v[i].t) { Ent t = v[i]; v[i] = v[j]; v[j] = t; }
        for (int i = 0; i < n && total > g_cmax / 5 * 4; i++) { unlink(v[i].p); total -= v[i].sz; }
    }
    free(v);
}
typedef struct { char url[NHTTP_URL_MAX]; int status; long long stored, lifetime; char* head; size_t headlen; unsigned char* body; size_t bodylen; } CEnt;
static void cent_free(CEnt* e) { free(e->head); free(e->body); memset(e, 0, sizeof *e); }
static int cache_load(const char* url, CEnt* e) {
    memset(e, 0, sizeof *e); if (!g_cdir[0]) return 0;
    char path[420]; cpath(url, path, sizeof path); FILE* f = fopen(path, "rb"); if (!f) return 0;
    char l[NHTTP_URL_MAX + 2]; long long hl = 0, bl = 0; int ok = 0;
    if (fgets(l, sizeof l, f) && !strcmp(l, "MLC2\n") && fgets(l, sizeof l, f)) {
        size_t n = strlen(l);
        if (n && l[n - 1] == '\n') {
            l[n - 1] = 0; snprintf(e->url, sizeof e->url, "%s", l);
        }
        if (n && l[n - 1] == 0 && !strcmp(e->url, url) && fgets(l, sizeof l, f)) { e->status = atoi(l);
            if (fgets(l, sizeof l, f)) { e->stored = atoll(l); if (fgets(l, sizeof l, f)) { e->lifetime = atoll(l); if (fgets(l, sizeof l, f)) { hl = atoll(l); if (fgets(l, sizeof l, f)) { bl = atoll(l); ok = 1; } } } } }
    }
    if (ok && hl >= 0 && bl >= 0 && hl < (1 << 20) && bl < (64ll << 20)) {
        e->head = malloc((size_t)hl + 1); e->body = malloc((size_t)bl + 1);
        if (e->head && e->body && fread(e->head, 1, (size_t)hl, f) == (size_t)hl && fread(e->body, 1, (size_t)bl, f) == (size_t)bl) { e->head[hl] = 0; e->body[bl] = 0; e->headlen = (size_t)hl; e->bodylen = (size_t)bl; }
        else ok = 0;
    } else ok = 0;
    fclose(f);
    if (!ok) { cent_free(e); unlink(path); return 0; }
    struct timeval tv[2]; gettimeofday(&tv[0], NULL); tv[1] = tv[0]; utimes(path, tv);        /* «использовано сейчас» для LRU */
    return 1;
}
static void cache_write(const CEnt* e) {
    char path[420], tmp[440]; cpath(e->url, path, sizeof path); snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    FILE* f = fopen(tmp, "wb"); if (!f) return;
    fprintf(f, "MLC2\n%s\n%d\n%lld\n%lld\n%zu\n%zu\n", e->url, e->status, e->stored, e->lifetime, e->headlen, e->bodylen);
    int ok = fwrite(e->head, 1, e->headlen, f) == e->headlen && (!e->bodylen || fwrite(e->body, 1, e->bodylen, f) == e->bodylen);
    ok = (fclose(f) == 0) && ok;
    if (ok) rename(tmp, path); else unlink(tmp);
}
static int cc_has(const char* v, const char* tok) { return has_token(v, tok); }
static int cc_num(const char* v, const char* name, long long* out) {
    size_t nl = strlen(name);
    for (const char* p = v; *p;) { while (*p == ' ' || *p == ',') p++; if (!strncasecmp(p, name, nl) && p[nl] == '=') { *out = atoll(p + nl + 1); return 1; } while (*p && *p != ',') p++; }
    return 0;
}
/* время жизни ответа по RFC 9111; -1 — кэшировать нельзя */
static long long cache_lifetime(const char* head, const char* url, long long now, int* has_validator) {
    char cc[512] = "", v[256]; hdr_get(head, "Cache-Control", cc, sizeof cc);
    *has_validator = hdr_get(head, "ETag", v, sizeof v) || hdr_get(head, "Last-Modified", v, sizeof v);
    if (cc_has(cc, "no-store")) return -1;
    char vary[256] = ""; if (hdr_get(head, "Vary", vary, sizeof vary)) {                  /* Vary по заголовкам, которые мы шлём неизменными, не мешает */
        for (const char* p = vary; *p;) { while (*p == ' ' || *p == ',') p++; if (!*p) break;
            if (strncasecmp(p, "Accept-Encoding", 15) && strncasecmp(p, "Accept-Language", 15) && strncasecmp(p, "User-Agent", 10) && !(strncasecmp(p, "Accept", 6) == 0 && (p[6] == ',' || p[6] == 0 || p[6] == ' '))) return -1;
            while (*p && *p != ',') p++; } }
    long long life = -1, n; char dt[64], ex[64], lm[64]; long long date = hdr_get(head, "Date", dt, sizeof dt) ? parse_http_date(dt) : -1; if (date < 0) date = now;
    if (cc_num(cc, "max-age", &n)) life = n < 0 ? 0 : n;
    else if (hdr_get(head, "Expires", ex, sizeof ex)) { long long e = parse_http_date(ex); life = e < 0 ? 0 : (e - date < 0 ? 0 : e - date); }
    else if (hdr_get(head, "Last-Modified", lm, sizeof lm) && !strchr(url, '?')) { long long m = parse_http_date(lm); if (m >= 0 && date > m) { life = (date - m) / 10; if (life > 86400) life = 86400; } else life = 0; }
    else life = 0;
    if (cc_has(cc, "no-cache")) life = 0;
    char pr[64]; if (!cc[0] && hdr_get(head, "Pragma", pr, sizeof pr) && cc_has(pr, "no-cache")) life = 0;
    char age[32]; if (life > 0 && hdr_get(head, "Age", age, sizeof age)) { long long a = atoll(age); life = a >= life ? 0 : life - a; }
    if (life == 0 && !*has_validator) return -1;                                          /* ни свежести, ни валидатора — переиспользовать нечем */
    return life;
}
static void cache_store(const char* url, const RawResp* r) {
    if (!g_cdir[0] || r->body.n > (32u << 20)) return;
    if (r->status != 200 && r->status != 203 && r->status != 301 && r->status != 308) return;
    long long now = (long long)time(NULL); int val; long long life = cache_lifetime(r->head, url, now, &val);
    if (life < 0 || ((r->status == 301 || r->status == 308) && life <= 0)) { char path[420]; cpath(url, path, sizeof path); unlink(path); return; }
    CEnt e; memset(&e, 0, sizeof e); snprintf(e.url, sizeof e.url, "%s", url); e.status = r->status; e.stored = now; e.lifetime = life;
    e.head = r->head; e.headlen = strlen(r->head); e.body = (unsigned char*)r->body.p; e.bodylen = r->body.n; if (!e.body) { e.body = (unsigned char*)""; e.bodylen = 0; }
    cache_write(&e);
    if (++g_cstores % 64 == 1) cache_evict();
}
/* одно звено: кэш → (условный) запрос → обновление кэша. *from: 0 сеть, 1 свежий кэш, 2 подтверждён 304 */
static int cached_request(const NHttpReq* rq, const Url* u, const char* method, const char* extra, const unsigned char* body, size_t blen, int64_t deadline, RawResp* r, int* from, char* err, size_t ecap) {
    *from = 0;
    char url[NHTTP_URL_MAX];
    size_t origin_len = strlen(u->origin), pq_len = strlen(u->pq);
    int cache_key_ok = origin_len < sizeof url && pq_len < sizeof url - origin_len;
    if (cache_key_ok) {
        memcpy(url, u->origin, origin_len);
        memcpy(url + origin_len, u->pq, pq_len + 1);
    } else url[0] = 0;
    int plain = !rq->sink && !rq->headers && !body && !strcmp(method, "GET");
    if (!g_cdir[0] || !plain || !cache_key_ok) {
        int ok = do_request(rq, u, method, extra, body, blen, deadline, r, err, ecap);
        if (ok && cache_key_ok && g_cdir[0] && strcmp(method, "GET") && strcmp(method, "HEAD") && r->status >= 200 && r->status < 400) { char path[420]; cpath(url, path, sizeof path); unlink(path); }   /* небезопасный метод инвалидирует запись */
        return ok;
    }
    CEnt e; int have = cache_load(url, &e); char* cond = NULL;
    if (have) {
        long long age = (long long)time(NULL) - e.stored; int fresh = age >= 0 && age < e.lifetime;
        if (g_cmode == NHTTP_CACHE_OFFLINE || (g_cmode == NHTTP_CACHE_NORMAL && fresh)) {
            memset(r, 0, sizeof *r); r->status = e.status; r->http11 = 1; r->keepalive = 1; r->head = e.head; e.head = NULL;
            r->body.p = (char*)e.body; r->body.n = e.bodylen; r->body.cap = e.bodylen + 1; e.body = NULL; cent_free(&e); *from = 1; return 1;
        }
        char et[256] = "", lm[128] = ""; hdr_get(e.head, "ETag", et, sizeof et); hdr_get(e.head, "Last-Modified", lm, sizeof lm);
        if (et[0] || lm[0]) {                                                             /* условный запрос */
            Str c = {0}; if (et[0]) { str_adds(&c, "If-None-Match: "); str_adds(&c, et); str_adds(&c, "\n"); } if (lm[0]) { str_adds(&c, "If-Modified-Since: "); str_adds(&c, lm); str_adds(&c, "\n"); }
            cond = c.p;
        } else { cent_free(&e); have = 0; }
    }
    int ok;
    if (cond) {
        NHttpReq q2 = *rq; q2.headers = cond;                                             /* rq->headers != NULL только для этого запроса — кэш внутри do_request не участвует */
        ok = do_request(&q2, u, method, cond, body, blen, deadline, r, err, ecap); free(cond);
    } else ok = do_request(rq, u, method, extra, body, blen, deadline, r, err, ecap);
    if (!ok) { if (have) cent_free(&e); return 0; }
    if (have && r->status == 304) {                                                      /* подтверждено: отдаём сохранённое, обновляем свежесть */
        long long now = (long long)time(NULL); int val; Str nh = {0}; char tmp[64];
        for (const char* p = e.head; *p;) { const char* en = strchr(p, '\n'); size_t ll = en ? (size_t)(en - p) + 1 : strlen(p); int drop = 0;
            static const char* upd[] = {"Date", "Cache-Control", "Expires", "Age", "ETag", "Last-Modified", "Vary", NULL};
            for (int i = 0; upd[i]; i++) { size_t nl = strlen(upd[i]); if (ll > nl && !strncasecmp(p, upd[i], nl) && p[nl] == ':' && hdr_get(r->head, upd[i], tmp, sizeof tmp)) { drop = 1; break; } }
            if (!drop) str_add(&nh, p, ll); p += ll; }
        for (const char* p = r->head; *p;) { const char* en = strchr(p, '\n'); size_t ll = en ? (size_t)(en - p) + 1 : strlen(p);
            static const char* upd2[] = {"Date:", "Cache-Control:", "Expires:", "Age:", "ETag:", "Last-Modified:", "Vary:", NULL};
            for (int i = 0; upd2[i]; i++) if (!strncasecmp(p, upd2[i], strlen(upd2[i]))) { str_add(&nh, p, ll); break; } p += ll; }
        if (!nh.p) str_adds(&nh, "HTTP/1.1 200 OK\r\n");
        long long life = cache_lifetime(nh.p, url, now, &val);
        RawResp full; memset(&full, 0, sizeof full); full.status = e.status; full.head = nh.p; full.body.p = (char*)e.body; full.body.n = e.bodylen; full.body.cap = e.bodylen + 1;
        if (life >= 0) cache_store(url, &full); else { char path[420]; cpath(url, path, sizeof path); unlink(path); }
        raw_free(r); *r = full; r->http11 = 1; r->keepalive = 1; r->status = e.status; e.body = NULL; e.head = NULL; cent_free(&e); *from = 2; return 1;
    }
    if (have) cent_free(&e);
    cache_store(url, r);
    return 1;
}

/* ══════════════ основной вызов ══════════════ */
static void copy_set_cookies(NJar* jar, const Url* u, const char* head) {
    if (!jar) return;
    for (const char* p = head; *p;) {
        const char* e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > 11 && !strncasecmp(p, "Set-Cookie:", 11)) { char v[4400]; size_t vl = ll - 11; if (vl >= sizeof v) vl = sizeof v - 1; memcpy(v, p + 11, vl); v[vl] = 0; size_t k = strlen(v); while (k && (v[k - 1] == '\r')) v[--k] = 0; jar_set_cookie(jar, u, trim(v)); }
        if (!e) break;
        p = e + 1;
    }
}
void nhttp_resp_free(NHttpResp* r) { if (!r) return; free(r->headers); free(r->all_headers); free(r->body); memset(r, 0, sizeof *r); }

int nhttp_perform(const NHttpReq* rq, NHttpResp* out) {
    memset(out, 0, sizeof *out);
    if (!rq || !rq->url) { set_err(out->err, sizeof out->err, "пустой URL"); return 0; }
    int64_t deadline = now_ms() + (rq->timeout_ms > 0 ? rq->timeout_ms : 90000);
    int maxr = rq->max_redirects == 0 ? 20 : rq->max_redirects;
    char method[16]; snprintf(method, sizeof method, "%s", rq->method && *rq->method ? rq->method : "GET");
    for (char* p = method; *p; p++) *p = (char)toupper((unsigned char)*p);
    char url[NHTTP_URL_MAX];
    size_t request_url_len = strlen(rq->url);
    if (request_url_len >= sizeof url) {
        set_err(out->err, sizeof out->err, "URL длиннее допустимого лимита");
        return 0;
    }
    memcpy(url, rq->url, request_url_len + 1);
    const unsigned char* body = rq->body; size_t blen = rq->body_len;
    char* extra = sdup(rq->headers);
    Str all = {0};
    for (int hop = 0;; hop++) {
        char upgraded[NHTTP_URL_MAX];
        if (websec_hsts_upgrade_url(url, upgraded, sizeof upgraded)) {
            size_t upgraded_len = strlen(upgraded);
            if (upgraded_len >= sizeof url) { set_err(out->err, sizeof out->err, "URL после HSTS слишком длинный"); free(extra); free(all.p); return 0; }
            memcpy(url, upgraded, upgraded_len + 1);
        }
        Url u; if (!parse_url(url, &u)) { set_err(out->err, sizeof out->err, "неподдерживаемый или неверный URL"); free(extra); free(all.p); return 0; }
        if (u.https && !ntls_available()) { set_err(out->err, sizeof out->err, "HTTPS недоступен: не найдена библиотека TLS"); free(extra); free(all.p); return 0; }
        size_t final_origin_len = strlen(u.origin), final_pq_len = strlen(u.pq);
        if (final_origin_len >= sizeof out->final_url || final_pq_len >= sizeof out->final_url - final_origin_len) {
            set_err(out->err, sizeof out->err, "итоговый URL длиннее допустимого лимита"); free(extra); free(all.p); return 0;
        }
        memcpy(out->final_url, u.origin, final_origin_len);
        memcpy(out->final_url + final_origin_len, u.pq, final_pq_len + 1);
        RawResp r; memset(&r, 0, sizeof r);
        int from_cache = 0;
        if (!cached_request(rq, &u, method, extra, body, blen, deadline, &r, &from_cache, out->err, sizeof out->err)) { free(extra); free(all.p); return 0; }
        out->from_cache = from_cache;
        if (!from_cache) copy_set_cookies(rq->jar, &u, r.head);                    /* куки из ответа, взятого из кэша, повторно не применяем */
        if (!from_cache) websec_hsts_remember(u.host, u.https, r.head);
        str_adds(&all, r.head); str_adds(&all, "\r\n");
        char loc[8192];
        int redirect_status = (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308);
        int location_status = redirect_status ? hdr_get_checked(r.head, "Location", loc, sizeof loc) : 0;
        if (location_status < 0) { set_err(out->err, sizeof out->err, "слишком длинный Location"); raw_free(&r); free(extra); free(all.p); return 0; }
        int redirect = redirect_status && location_status > 0;
        if (redirect && maxr >= 0) {
            if (hop >= maxr) { set_err(out->err, sizeof out->err, "слишком много перенаправлений"); raw_free(&r); free(extra); free(all.p); return 0; }
            char next[NHTTP_URL_MAX];
            if (!resolve_location(&u, loc, next, sizeof next)) { set_err(out->err, sizeof out->err, "неверный Location"); raw_free(&r); free(extra); free(all.p); return 0; }
            Url nu; if (!parse_url(next, &nu)) { set_err(out->err, sizeof out->err, "Location указывает на неподдерживаемую схему"); raw_free(&r); free(extra); free(all.p); return 0; }
            if (r.status == 303 || ((r.status == 301 || r.status == 302) && !strcmp(method, "POST"))) { if (strcmp(method, "HEAD")) snprintf(method, sizeof method, "GET"); body = NULL; blen = 0; }
            if (extra && (nu.https != u.https || nu.port != u.port || strcmp(nu.host, u.host))) {            /* на другой origin не переносим Authorization/Cookie */
                const char* drop[] = {"Authorization", "Cookie", "Proxy-Authorization", NULL}; hdr_strip(extra, drop);
            }
            if (!body) { const char* drop[] = {"Content-Type", NULL}; if (extra) hdr_strip(extra, drop); }
            size_t next_len = strlen(next);
            if (next_len >= sizeof url) { set_err(out->err, sizeof out->err, "URL перенаправления слишком длинный"); raw_free(&r); free(extra); free(all.p); return 0; }
            memcpy(url, next, next_len + 1); raw_free(&r); continue;
        }
        /* итоговый ответ */
        out->status = r.status;
        char ce[64] = ""; hdr_get(r.head, "Content-Encoding", ce, sizeof ce);
        unsigned char* decoded = NULL; size_t dlen = 0; int did_decode = 0;
        if (!rq->sink && r.body.n > 0 && ce[0] && strcasecmp(ce, "identity")) {
            size_t cap = rq->max_body ? rq->max_body : (size_t)256 << 20;
            if (has_token(ce, "gzip") || has_token(ce, "x-gzip")) decoded = n_gunzip((unsigned char*)r.body.p, r.body.n, &dlen, cap);
            else if (has_token(ce, "deflate")) decoded = n_http_deflate((unsigned char*)r.body.p, r.body.n, &dlen, cap);
            else did_decode = -1;                                                                              /* br/zstd и пр. — отдаём как есть */
            if (!decoded && did_decode == 0) { set_err(out->err, sizeof out->err, "не удалось распаковать тело ответа (Content-Encoding)"); out->status = 0; raw_free(&r); free(extra); free(all.p); return 0; }
            if (decoded) did_decode = 1;
        }
        if (did_decode == 1) { free(r.body.p); r.body.p = (char*)decoded; r.body.n = dlen; r.body.cap = dlen + 1; }
        { const char* drop_dec[] = {"Transfer-Encoding", "Content-Length", "Content-Encoding", NULL}; const char* drop_cl[] = {"Transfer-Encoding", "Content-Length", NULL};
          hdr_strip(r.head, did_decode == 1 ? drop_dec : drop_cl); }
        out->headers = r.head; r.head = NULL;
        out->all_headers = all.p; all.p = NULL;
        out->sink_bytes = r.sunk;
        if (!rq->sink) {
            if (!r.body.p) { r.body.p = calloc(1, 1); }
            out->body = (unsigned char*)r.body.p; out->body_len = r.body.n; r.body.p = NULL;
        }
        raw_free(&r); free(extra);
        return 1;
    }
}
