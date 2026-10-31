#define _GNU_SOURCE
#include "ncors.h"
#include "psl.h"
#include "web_security.h"
#include "idna.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

static int ncors_parse_port(const char* s, unsigned* port) {
    if (!s || !*s || !port) return 0;
    unsigned value = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (65535u - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    if (value == 0 || value > 65535u) return 0;
    *port = value;
    return 1;
}

void ncors_origin(const char* url, char* out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap, "null");
    if (!url) return;
    const char* p = url;
    int https = 0;
    if (!strncasecmp(p, "https://", 8)) { https = 1; p += 8; }
    else if (!strncasecmp(p, "http://", 7)) p += 7;
    else return;

    const char* e = p;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    size_t auth_len = (size_t)(e - p);
    char auth[300];
    if (!auth_len || auth_len >= sizeof auth) return;
    memcpy(auth, p, auth_len); auth[auth_len] = 0;

    /* Userinfo is excluded from the origin, but an empty host is never valid. */
    char* at = strrchr(auth, '@');
    char* h = at ? at + 1 : auth;
    if (!*h) return;

    char* port_text = NULL;
    int bracketed_ipv6 = h[0] == '[';
    if (bracketed_ipv6) {
        char* close = strchr(h, ']');
        if (!close || close == h + 1) return;
        if (close[1] == ':') port_text = close + 2;
        else if (close[1] != 0) return;
        /* Validate the literal, not merely its brackets, before comparing origins. */
        char literal[INET6_ADDRSTRLEN];
        size_t literal_len = (size_t)(close - h - 1);
        if (!literal_len || literal_len >= sizeof literal) return;
        memcpy(literal, h + 1, literal_len); literal[literal_len] = 0;
        struct in6_addr addr6;
        if (inet_pton(AF_INET6, literal, &addr6) != 1) return;
        close[1] = 0; /* retain the canonical bracketed host; discard port text for conversion */
    } else {
        char* colon = strchr(h, ':');
        if (colon) {
            if (strchr(colon + 1, ':')) return; /* unbracketed IPv6 is not an authority */
            *colon = 0;
            port_text = colon + 1;
        }
        if (!*h) return;
    }

    unsigned port = 0;
    if (port_text && !ncors_parse_port(port_text, &port)) return;
    char ascii[256];
    if (bracketed_ipv6) {
        int n = snprintf(ascii, sizeof ascii, "%s", h);
        if (n < 0 || (size_t)n >= sizeof ascii) return;
    } else if (!nidna_to_ascii(h, ascii, sizeof ascii)) {
        return;
    }

    int default_port = !port_text || (https ? port == 443u : port == 80u);
    int n;
    if (default_port) n = snprintf(out, cap, "%s://%s", https ? "https" : "http", ascii);
    else n = snprintf(out, cap, "%s://%s:%u", https ? "https" : "http", ascii, port);
    if (n < 0 || (size_t)n >= cap) snprintf(out, cap, "null");
}
static int hostof(const char* origin, char* out, size_t cap) { /* host без порта, IPv6 сохраняется целиком */
    if (!origin || !out || !cap) return 0;
    const char* sep = strstr(origin, "://");
    if (!sep) return 0;
    const char* p = sep + 3;
    size_t l;
    if (*p == '[') {
        const char* close = strchr(p, ']');
        if (!close || (close[1] && close[1] != ':')) return 0;
        l = (size_t)(close - p + 1);
    } else {
        l = strcspn(p, ":/?#");
    }
    if (!l || l >= cap) return 0;
    memcpy(out, p, l); out[l] = 0;
    for (char* q = out; *q; q++) *q = (char)tolower((unsigned char)*q);
    return 1;
}
int ncors_same_site(const char* a, const char* b) { /* schemeful site: scheme + eTLD+1 (PSL) */
    if (!a || !b) return 0;
    int a_http = !strncasecmp(a, "http://", 7);
    int a_https = !strncasecmp(a, "https://", 8);
    int b_http = !strncasecmp(b, "http://", 7);
    int b_https = !strncasecmp(b, "https://", 8);
    if ((!a_http && !a_https) || (!b_http && !b_https) || a_http != b_http || a_https != b_https) return 0;
    char ha[256], hb[256];
    if (!hostof(a, ha, sizeof ha) || !hostof(b, hb, sizeof hb)) return 0;
    char ra[256], rb[256];
    if (psl_registrable_domain(ha, ra, sizeof ra) && psl_registrable_domain(hb, rb, sizeof rb)) return !strcasecmp(ra, rb);
    return !strcasecmp(ha, hb); /* localhost, IP literals and single-label hosts */
}

/* ── заголовки запроса ── */
static int forbidden_header(const char* n) {
    static const char* f[] = {"accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method", "connection", "content-length", "cookie", "cookie2",
        "date", "dnt", "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via", NULL};
    for (int i = 0; f[i]; i++) if (!strcasecmp(n, f[i])) return 1;
    return !strncasecmp(n, "proxy-", 6) || !strncasecmp(n, "sec-", 4);
}
static const char* trim_ws(char* s) { while (isspace((unsigned char)*s)) s++; char* e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
/* CORS-safelisted header values must be <= 128 bytes and contain no CORS-unsafe byte. */
static int cors_unsafe_byte(unsigned char c) {
    if (c <= 0x08 || (c >= 0x0a && c <= 0x1f) || c == 0x7f) return 1;
    switch (c) {
        case '"': case '(': case ')': case ':': case '<': case '>': case '?': case '@':
        case '[': case '\\': case ']': case '{': case '}': return 1;
        default: return 0;
    }
}
static int cors_safelisted_value(const char* v) {
    if (!v || strlen(v) > 128) return 0;
    for (const unsigned char* p = (const unsigned char*)v; *p; p++) if (cors_unsafe_byte(*p)) return 0;
    return 1;
}
/* Content-Type is safelisted by its parsed MIME essence, not by a matching prefix. */
static int simple_ctype(const char* v) {
    if (!v) return 0;
    const char* end = strchr(v, ';'); if (!end) end = v + strlen(v);
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t n = (size_t)(end - v);
    return (n == sizeof("application/x-www-form-urlencoded") - 1 && !strncasecmp(v, "application/x-www-form-urlencoded", n)) ||
           (n == sizeof("multipart/form-data") - 1 && !strncasecmp(v, "multipart/form-data", n)) ||
           (n == sizeof("text/plain") - 1 && !strncasecmp(v, "text/plain", n));
}
/* Fetch only safelists a single byte range (e.g. bytes=0-99 or bytes=100-). */
static int simple_range(const char* v) {
    if (!cors_safelisted_value(v) || strncasecmp(v, "bytes=", 6)) return 0;
    const unsigned char* p = (const unsigned char*)v + 6;
    if (!isdigit(*p)) return 0;
    while (isdigit(*p)) p++;
    if (*p++ != '-') return 0;
    while (isdigit(*p)) p++;
    return *p == 0;
}
/* Разбор заголовков скрипта: user (чистый список без запрещённых) и unsafe (имена не-безопасных заголовков, через запятую, по алфавиту) */
typedef struct { char* clean; char unsafe[512]; int has_unsafe; } HdrSet;
static int cmpstr(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static void hdr_scan(const char* raw, HdrSet* h) {
    memset(h, 0, sizeof *h);
    size_t rawlen = raw ? strlen(raw) : 0;
    if (rawlen > ((size_t)-1 - 2) / 2) return;
    size_t cap = rawlen * 2 + 2; h->clean = calloc(cap, 1); if (!h->clean) return;
    char* names[40]; int nn = 0, overflow = 0;
    if (raw) {
        char* dup = strdup(raw); if (!dup) { free(h->clean); h->clean = NULL; return; }
        char* save = NULL;
        for (char* ln = strtok_r(dup, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(ln); while (l && ln[l - 1] == '\r') ln[--l] = 0; char* c = strchr(ln, ':'); if (!c || c == ln) continue;
            *c = 0; const char* nm = trim_ws(ln); const char* val = trim_ws(c + 1); if (!*nm || forbidden_header(nm) || strpbrk(val, "\r\n")) continue;
            strcat(h->clean, nm); strcat(h->clean, ": "); strcat(h->clean, val); strcat(h->clean, "\n");
            int safe_value = cors_safelisted_value(val);
            int safe = safe_value && (!strcasecmp(nm, "accept") || !strcasecmp(nm, "accept-language") || !strcasecmp(nm, "content-language") ||
                       (!strcasecmp(nm, "content-type") && simple_ctype(val)) || (!strcasecmp(nm, "range") && simple_range(val)));
            if (!safe) {
                char* low = strdup(nm); if (!low) { overflow = 1; break; }
                for (char* q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
                int dup2 = 0; for (int i = 0; i < nn; i++) if (!strcmp(names[i], low)) { dup2 = 1; break; }
                if (dup2) free(low);
                else if (nn < (int)(sizeof names / sizeof names[0])) names[nn++] = low;
                else { free(low); overflow = 1; break; }
            }
        }
        free(dup);
    }
    qsort(names, (size_t)nn, sizeof(char*), cmpstr);
    size_t used = 0;
    for (int i = 0; i < nn; i++) {
        size_t n = strlen(names[i]), need = n + (i ? 1u : 0u);
        if (need >= sizeof h->unsafe - used) overflow = 1;
        else { if (i) h->unsafe[used++] = ','; memcpy(h->unsafe + used, names[i], n); used += n; h->unsafe[used] = 0; }
        free(names[i]);
    }
    if (overflow) { free(h->clean); h->clean = NULL; h->unsafe[0] = 0; h->has_unsafe = 0; return; }
    h->has_unsafe = nn > 0;
}
static int hget(const char* block, const char* name, char* out, size_t cap) {
    size_t nl = strlen(name);
    for (const char* p = block; *p;) {
        const char* e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char* v = p + nl + 1; while (v < p + ll && (*v == ' ' || *v == '\t')) v++; size_t vl = (size_t)(p + ll - v); while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1; memcpy(out, v, vl); out[vl] = 0; return 1;
        }
        if (!e) break; p = e + 1;
    }
    return 0;
}
static int list_has(const char* list, const char* tok, int ci) {                                  /* токен в списке через запятую */
    size_t tl = strlen(tok);
    for (const char* p = list; *p;) { while (*p == ' ' || *p == ',') p++; const char* s = p; while (*p && *p != ',') p++; const char* e = p; while (e > s && e[-1] == ' ') e--;
        if ((size_t)(e - s) == tl && (ci ? !strncasecmp(s, tok, tl) : !strncmp(s, tok, tl))) return 1; }
    return 0;
}
static int allow_origin_ok(const char* block, const char* origin, int cred, char* err, size_t ecap) {
    char ao[300] = "", ac[32] = "";
    if (!hget(block, "Access-Control-Allow-Origin", ao, sizeof ao)) { snprintf(err, ecap, "CORS: в ответе нет Access-Control-Allow-Origin"); return 0; }
    if (!strcmp(ao, "*")) { if (cred) { snprintf(err, ecap, "CORS: Access-Control-Allow-Origin: * недопустим с credentials"); return 0; } return 1; }
    if (strcmp(ao, origin)) { snprintf(err, ecap, "CORS: источник %s не разрешён (Access-Control-Allow-Origin: %s)", origin, ao); return 0; }
    if (cred) { hget(block, "Access-Control-Allow-Credentials", ac, sizeof ac); if (strcmp(ac, "true")) { snprintf(err, ecap, "CORS: нужен Access-Control-Allow-Credentials: true"); return 0; } }
    return 1;
}
/* копия заголовков ответа без Set-Cookie и (для межсайтового запроса) только с видимыми скрипту */
static char* filter_headers(const char* head, int cross, int cred) {
    size_t cap = strlen(head) + 2; char* o = calloc(cap, 1); if (!o) return NULL; char expose[1024] = ""; hget(head, "Access-Control-Expose-Headers", expose, sizeof expose);
    int star = cross && !cred && list_has(expose, "*", 0); int first = 1;
    for (const char* p = head; *p;) {
        const char* e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p) + 1 : strlen(p);
        char nm[128]; const char* c = memchr(p, ':', ll); size_t nl = c ? (size_t)(c - p) : 0; if (nl >= sizeof nm) nl = sizeof nm - 1; memcpy(nm, p, nl); nm[nl] = 0;
        int keep = 1;
        if (first) keep = 1;                                                                           /* строка статуса */
        else if (!c) keep = 0;
        else if (!strcasecmp(nm, "set-cookie") || !strcasecmp(nm, "set-cookie2")) keep = 0;
        else if (cross) { static const char* safe[] = {"cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma", NULL}; keep = star; for (int i = 0; safe[i]; i++) if (!strcasecmp(nm, safe[i])) keep = 1; if (!keep && list_has(expose, nm, 1)) keep = 1; }
        if (keep) memcpy(o + strlen(o), p, ll);
        first = 0; p += ll;
    }
    return o;
}
static int is_redirect_block(const char* block) {
    int status = 0; char loc[2] = "";
    if (!block || sscanf(block, "HTTP/%*s %d", &status) != 1) return 0;
    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) return 0;
    return hget(block, "Location", loc, sizeof loc) && loc[0] != 0;
}
typedef struct { NCorsReq* req; int cross, cred_inc; char origin[300]; } NCorsStreamCtx;
static int ncors_stream_headers(const char* block, const char* response_url, void* user, char* err, size_t ecap) {
    NCorsStreamCtx* x = (NCorsStreamCtx*)user; NCorsReq* r = x ? x->req : NULL;
    if (!r || !r->headers_cb) return 1;
    int opaque_redirect = r->redirect == 1 && is_redirect_block(block);
    if (x->cross && !opaque_redirect) {
        if (r->mode == NCORS_MODE_NO_CORS) { snprintf(err, ecap, "CORS: непрозрачный no-cors ответ недоступен через streaming backend"); return 0; }
        if (!allow_origin_ok(block, x->origin, x->cred_inc, err, ecap)) return 0;
    }
    char* visible = (x->cross && !opaque_redirect) ? filter_headers(block, 1, x->cred_inc) : strdup(block);
    if (!visible) { snprintf(err, ecap, "CORS: нет памяти при обработке заголовков ответа"); return 0; }
    char* w = visible; const char* p = visible;
    while (*p) {
        const char* e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) + 1 : strlen(p);
        int drop = (len >= 18 && !strncasecmp(p, "Transfer-Encoding:", 18)) ||
                   (len >= 17 && !strncasecmp(p, "Content-Encoding:", 17));
        if (!drop) { memmove(w, p, len); w += len; }
        p += len;
    }
    *w = 0;
    int ok = r->headers_cb(visible, response_url, r->headers_user, err, ecap);
    free(visible); return ok;
}

/* кэш результатов preflight на диске: cache/preflight/<хэш>, внутри время истечения */
static unsigned long long fnv(const char* s) { unsigned long long h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }
static int pf_cached(const char* key) { char p[120]; snprintf(p, sizeof p, "cache/preflight/%016llx", fnv(key)); FILE* f = fopen(p, "r"); if (!f) return 0; long long t = 0; if (fscanf(f, "%lld", &t) != 1) t = 0; fclose(f); return t > (long long)time(NULL); }
static void pf_store(const char* key, long sec) { mkdir("cache", 0777); mkdir("cache/preflight", 0777); char p[120]; snprintf(p, sizeof p, "cache/preflight/%016llx", fnv(key)); FILE* f = fopen(p, "w"); if (f) { fprintf(f, "%lld\n", (long long)time(NULL) + sec); fclose(f); } }

int ncors_fetch(NCorsReq* r, NHttpResp* out) {
    if (!r || !out || !r->url) return 0;
    if (!websec_allows(r->url, NAL_RESOURCE_CONNECT)) {
        memset(out, 0, sizeof *out); snprintf(out->err, sizeof out->err, "CSP: connect-src блокирует %s", r->url); return 0;
    }
    NHttpReq rq; memset(&rq, 0, sizeof rq); rq.method = r->method; rq.url = r->url; rq.body = r->body; rq.body_len = r->body_len; rq.timeout_ms = r->timeout_ms ? r->timeout_ms : 60000; rq.sink = r->sink;
    rq.site_for_cookies = r->page_url;
    rq.is_top_level_navigation = 0;
    rq.max_redirects = r->redirect == 0 ? 0 : -1;
    if (!r->page_url) { rq.headers = r->headers; rq.jar = r->jar; rq.headers_cb = r->headers_cb; rq.headers_user = r->headers_user; r->jar_used = r->jar != NULL; return nhttp_perform(&rq, out); }       /* доверенный запрос */
    memset(out, 0, sizeof *out);
    char po[300], ro[300]; ncors_origin(r->page_url, po, sizeof po); ncors_origin(r->url, ro, sizeof ro);
    int cross = !strcmp(po, "null") || strcmp(po, ro) != 0; char method[16]; snprintf(method, sizeof method, "%s", r->method ? r->method : "GET"); for (char* q = method; *q; q++) *q = (char)toupper((unsigned char)*q);
    if (!strcmp(ro, "null")) { snprintf(out->err, sizeof out->err, "схема URL не поддерживается для fetch"); return 0; }
    if (!strncasecmp(r->page_url, "https://", 8) && !strncasecmp(r->url, "http://", 7) && strncasecmp(r->url + 7, "localhost", 9) && strncmp(r->url + 7, "127.", 4) && strncmp(r->url + 7, "[::1]", 5)) {
        snprintf(out->err, sizeof out->err, "Mixed Content: https-страница не может запрашивать http-ресурс %s", r->url); return 0; }
    if (r->mode == NCORS_MODE_SAME_ORIGIN && cross) { snprintf(out->err, sizeof out->err, "CORS: режим same-origin запрещает запрос на другой источник"); return 0; }
    HdrSet hs; hdr_scan(r->headers, &hs); if (!hs.clean) { snprintf(out->err, sizeof out->err, "CORS: заголовки превышают безопасный лимит или недостаточно памяти"); return 0; }
    int cred_inc = r->credentials == NCORS_CRED_INCLUDE, send_cookies = cred_inc || (r->credentials == NCORS_CRED_SAME_ORIGIN && !cross);
    NCorsStreamCtx early; memset(&early, 0, sizeof early); early.req = r; early.cross = cross; early.cred_inc = cred_inc; snprintf(early.origin, sizeof early.origin, "%s", po);
    if (r->headers_cb) { rq.headers_cb = ncors_stream_headers; rq.headers_user = &early; }
    int simple_method = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST");
    int need_pf = cross && (!simple_method || hs.has_unsafe);
    if (cross && r->mode == NCORS_MODE_NO_CORS && need_pf) { snprintf(out->err, sizeof out->err, "CORS: режим no-cors допускает только простые запросы"); free(hs.clean); return 0; }
    /* служебные заголовки, которые выставляет браузер */
    char extra[4096]; snprintf(extra, sizeof extra, "%s", hs.clean);
    if (cross || (strcmp(method, "GET") && strcmp(method, "HEAD"))) { strncat(extra, "Origin: ", sizeof extra - strlen(extra) - 1); strncat(extra, po, sizeof extra - strlen(extra) - 1); strncat(extra, "\n", sizeof extra - strlen(extra) - 1); }
    { char ref[1200] = ""; int pg_https = !strncasecmp(r->page_url, "https://", 8), rq_http = !strncasecmp(r->url, "http://", 7);
      if (!(pg_https && rq_http)) { if (!cross) { const char* h = strchr(r->page_url, '#'); snprintf(ref, sizeof ref, "%.*s", h ? (int)(h - r->page_url) : (int)strlen(r->page_url), r->page_url); } else snprintf(ref, sizeof ref, "%s/", po); }
      if (ref[0] && strcmp(po, "null")) { strncat(extra, "Referer: ", sizeof extra - strlen(extra) - 1); strncat(extra, ref, sizeof extra - strlen(extra) - 1); strncat(extra, "\n", sizeof extra - strlen(extra) - 1); } }
    { char sf[160]; snprintf(sf, sizeof sf, "Sec-Fetch-Site: %s\nSec-Fetch-Mode: %s\nSec-Fetch-Dest: empty\n", !cross ? "same-origin" : ncors_same_site(po, ro) ? "same-site" : "cross-site", r->mode == NCORS_MODE_NO_CORS ? "no-cors" : r->mode == NCORS_MODE_SAME_ORIGIN ? "same-origin" : "cors"); strncat(extra, sf, sizeof extra - strlen(extra) - 1); }
    /* preflight */
    if (need_pf) {
        char key[2200]; snprintf(key, sizeof key, "%s|%s|%s|%s|%d", po, r->url, method, hs.unsafe, cred_inc);
        if (!pf_cached(key)) {
            char pfh[1400]; snprintf(pfh, sizeof pfh, "Origin: %s\nAccess-Control-Request-Method: %s\n%s%s%sSec-Fetch-Mode: cors\nSec-Fetch-Dest: empty\nSec-Fetch-Site: %s\n", po, method, hs.has_unsafe ? "Access-Control-Request-Headers: " : "", hs.unsafe, hs.has_unsafe ? "\n" : "", ncors_same_site(po, ro) ? "same-site" : "cross-site");
            NHttpReq pq; memset(&pq, 0, sizeof pq); pq.method = "OPTIONS"; pq.url = r->url; pq.headers = pfh; pq.max_redirects = -1; pq.timeout_ms = 30000;
            NHttpResp pr; int ok = nhttp_perform(&pq, &pr);
            if (!ok) { snprintf(out->err, sizeof out->err, "CORS: preflight не выполнен (сетевая ошибка)"); nhttp_resp_free(&pr); free(hs.clean); return 0; }
            char why[200] = ""; int good = pr.status >= 200 && pr.status < 300;
            if (!good) snprintf(why, sizeof why, "CORS: preflight вернул статус %d", pr.status);
            if (good && !allow_origin_ok(pr.headers, po, cred_inc, why, sizeof why)) good = 0;
            if (good) { char am[512] = "", ah[1024] = "", ma[32] = ""; hget(pr.headers, "Access-Control-Allow-Methods", am, sizeof am); hget(pr.headers, "Access-Control-Allow-Headers", ah, sizeof ah); hget(pr.headers, "Access-Control-Max-Age", ma, sizeof ma);
                int star_m = !cred_inc && list_has(am, "*", 0), star_h = !cred_inc && list_has(ah, "*", 0);
                if (!simple_method && !star_m && !list_has(am, method, 0)) { snprintf(why, sizeof why, "CORS: метод %s не разрешён (Access-Control-Allow-Methods: %s)", method, am); good = 0; }
                if (good && hs.has_unsafe) { char* d = strdup(hs.unsafe); char* sv = NULL; for (char* t = strtok_r(d, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if (!list_has(ah, t, 1) && !(star_h && strcasecmp(t, "authorization"))) { snprintf(why, sizeof why, "CORS: заголовок %s не разрешён (Access-Control-Allow-Headers: %s)", t, ah); good = 0; break; } free(d); }
                if (good) { long age = ma[0] ? atol(ma) : 5; if (age < 1) age = 5; if (age > 600) age = 600; pf_store(key, age); } }
            nhttp_resp_free(&pr); if (!good) { snprintf(out->err, sizeof out->err, "%s", why); free(hs.clean); return 0; }
        }
    }
    rq.headers = extra; rq.jar = send_cookies ? r->jar : NULL; r->jar_used = send_cookies && r->jar != NULL;
    int ok = nhttp_perform(&rq, out); free(hs.clean);
    if (!ok) return 0;
    int opaque_redirect = r->redirect == 1 && is_redirect_block(out->headers ? out->headers : "");
    if (cross && !opaque_redirect) {
        if (r->mode == NCORS_MODE_NO_CORS) { nhttp_resp_free(out); snprintf(out->err, sizeof out->err, "ответ no-cors непрозрачен для скрипта"); return 0; }
        char why[200]; if (!allow_origin_ok(out->headers ? out->headers : "", po, cred_inc, why, sizeof why)) { nhttp_resp_free(out); snprintf(out->err, sizeof out->err, "%s", why); return 0; }
    }
    if (!opaque_redirect) { char* fh = filter_headers(out->headers ? out->headers : "", cross, cred_inc); if (fh) { free(out->headers); out->headers = fh; } }
    return 1;
}
