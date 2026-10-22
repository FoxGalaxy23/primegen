#define _GNU_SOURCE
#include "ncors.h"
#include "psl.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

void ncors_origin(const char* url, char* out, size_t cap) {
    const char* p = url; int https = 0;
    if (!strncasecmp(p, "https://", 8)) { https = 1; p += 8; } else if (!strncasecmp(p, "http://", 7)) p += 7; else { snprintf(out, cap, "null"); return; }
    const char* e = p; while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    char auth[300]; size_t l = (size_t)(e - p); if (!l || l >= sizeof auth) { snprintf(out, cap, "null"); return; }
    memcpy(auth, p, l); auth[l] = 0; char* at = strrchr(auth, '@'); char* h = at ? at + 1 : auth;
    for (char* q = h; *q; q++) *q = (char)tolower((unsigned char)*q);
    size_t hl = strlen(h); const char* port = NULL; char* colon = strrchr(h, ':');
    if (colon && (h[0] != '[' || colon > strchr(h, ']'))) { *colon = 0; port = colon + 1; hl = strlen(h); }
    int def = !port || !*port || (https ? !strcmp(port, "443") : !strcmp(port, "80"));
    if (def) snprintf(out, cap, "%s://%s", https ? "https" : "http", h); else snprintf(out, cap, "%s://%s:%s", https ? "https" : "http", h, port);
    (void)hl;
}
static int hostof(const char* origin, char* out, size_t cap) {                                  /* host из origin, без порта */
    const char* p = strstr(origin, "://"); if (!p) return 0; p += 3; size_t l = strcspn(p, ":"); if (l >= cap) return 0; memcpy(out, p, l); out[l] = 0; return 1;
}
static int same_site(const char* a, const char* b) {                                             /* один eTLD+1 (по Public Suffix List) */
    char ha[256], hb[256]; if (!hostof(a, ha, sizeof ha) || !hostof(b, hb, sizeof hb)) return 0;
    char ra[256], rb[256]; if (psl_registrable_domain(ha, ra, sizeof ra) && psl_registrable_domain(hb, rb, sizeof rb)) return !strcmp(ra, rb);
    if (!strcmp(ha, hb)) return 1;                                                                  /* localhost, IP: тот же хост — тот же сайт */
    return 0;
}

/* ── заголовки запроса ── */
static int forbidden_header(const char* n) {
    static const char* f[] = {"accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method", "connection", "content-length", "cookie", "cookie2",
        "date", "dnt", "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via", NULL};
    for (int i = 0; f[i]; i++) if (!strcasecmp(n, f[i])) return 1;
    return !strncasecmp(n, "proxy-", 6) || !strncasecmp(n, "sec-", 4);
}
static const char* trim_ws(char* s) { while (isspace((unsigned char)*s)) s++; char* e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }
/* значение content-type без параметров — одно из «простых»? */
static int simple_ctype(const char* v) {
    return !strncasecmp(v, "application/x-www-form-urlencoded", 33) || !strncasecmp(v, "multipart/form-data", 19) || !strncasecmp(v, "text/plain", 10);
}
/* Разбор заголовков скрипта: user (чистый список без запрещённых) и unsafe (имена не-безопасных заголовков, через запятую, по алфавиту) */
typedef struct { char* clean; char unsafe[512]; int has_unsafe; } HdrSet;
static int cmpstr(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static void hdr_scan(const char* raw, HdrSet* h) {
    memset(h, 0, sizeof *h); size_t cap = raw ? strlen(raw) + 2 : 2; h->clean = calloc(cap, 1); if (!h->clean) return;
    char* names[40]; int nn = 0;
    if (raw) { char* dup = strdup(raw); char* save = NULL;
        for (char* ln = strtok_r(dup, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(ln); while (l && ln[l - 1] == '\r') ln[--l] = 0; char* c = strchr(ln, ':'); if (!c || c == ln) continue;
            *c = 0; const char* nm = trim_ws(ln); const char* val = trim_ws(c + 1); if (!*nm || forbidden_header(nm) || strpbrk(val, "\r\n")) continue;
            strcat(h->clean, nm); strcat(h->clean, ": "); strcat(h->clean, val); strcat(h->clean, "\n");
            int safe = !strcasecmp(nm, "accept") || !strcasecmp(nm, "accept-language") || !strcasecmp(nm, "content-language") || (!strcasecmp(nm, "content-type") && simple_ctype(val)) || (!strcasecmp(nm, "range") && !strncmp(val, "bytes=", 6));
            if (!safe && nn < 40) { char* low = strdup(nm); for (char* q = low; *q; q++) *q = (char)tolower((unsigned char)*q); int dup2 = 0; for (int i = 0; i < nn; i++) if (!strcmp(names[i], low)) dup2 = 1; if (dup2) free(low); else names[nn++] = low; }
        }
        free(dup); }
    qsort(names, (size_t)nn, sizeof(char*), cmpstr);
    for (int i = 0; i < nn; i++) { if (strlen(h->unsafe) + strlen(names[i]) + 2 < sizeof h->unsafe) { if (i) strcat(h->unsafe, ","); strcat(h->unsafe, names[i]); } free(names[i]); }
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
/* кэш результатов preflight на диске: cache/preflight/<хэш>, внутри время истечения */
static unsigned long long fnv(const char* s) { unsigned long long h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }
static int pf_cached(const char* key) { char p[120]; snprintf(p, sizeof p, "cache/preflight/%016llx", fnv(key)); FILE* f = fopen(p, "r"); if (!f) return 0; long long t = 0; if (fscanf(f, "%lld", &t) != 1) t = 0; fclose(f); return t > (long long)time(NULL); }
static void pf_store(const char* key, long sec) { mkdir("cache", 0777); mkdir("cache/preflight", 0777); char p[120]; snprintf(p, sizeof p, "cache/preflight/%016llx", fnv(key)); FILE* f = fopen(p, "w"); if (f) { fprintf(f, "%lld\n", (long long)time(NULL) + sec); fclose(f); } }

int ncors_fetch(NCorsReq* r, NHttpResp* out) {
    NHttpReq rq; memset(&rq, 0, sizeof rq); rq.method = r->method; rq.url = r->url; rq.body = r->body; rq.body_len = r->body_len; rq.timeout_ms = r->timeout_ms ? r->timeout_ms : 60000;
    if (!r->page_url) { rq.headers = r->headers; rq.jar = r->jar; r->jar_used = r->jar != NULL; return nhttp_perform(&rq, out); }       /* доверенный запрос */
    memset(out, 0, sizeof *out);
    char po[300], ro[300]; ncors_origin(r->page_url, po, sizeof po); ncors_origin(r->url, ro, sizeof ro);
    int cross = !strcmp(po, "null") || strcmp(po, ro) != 0; char method[16]; snprintf(method, sizeof method, "%s", r->method ? r->method : "GET"); for (char* q = method; *q; q++) *q = (char)toupper((unsigned char)*q);
    if (!strcmp(ro, "null")) { snprintf(out->err, sizeof out->err, "схема URL не поддерживается для fetch"); return 0; }
    if (!strncasecmp(r->page_url, "https://", 8) && !strncasecmp(r->url, "http://", 7) && strncasecmp(r->url + 7, "localhost", 9) && strncmp(r->url + 7, "127.", 4) && strncmp(r->url + 7, "[::1]", 5)) {
        snprintf(out->err, sizeof out->err, "Mixed Content: https-страница не может запрашивать http-ресурс %s", r->url); return 0; }
    if (r->mode == NCORS_MODE_SAME_ORIGIN && cross) { snprintf(out->err, sizeof out->err, "CORS: режим same-origin запрещает запрос на другой источник (%s)", ro); return 0; }
    HdrSet hs; hdr_scan(r->headers, &hs); if (!hs.clean) { snprintf(out->err, sizeof out->err, "нет памяти"); return 0; }
    int cred_inc = r->credentials == NCORS_CRED_INCLUDE, send_cookies = cred_inc || (r->credentials == NCORS_CRED_SAME_ORIGIN && !cross);
    int simple_method = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST");
    int need_pf = cross && (!simple_method || hs.has_unsafe);
    if (cross && r->mode == NCORS_MODE_NO_CORS && need_pf) { snprintf(out->err, sizeof out->err, "CORS: режим no-cors допускает только простые запросы"); free(hs.clean); return 0; }
    /* служебные заголовки, которые выставляет браузер */
    char extra[4096]; snprintf(extra, sizeof extra, "%s", hs.clean);
    if (cross || (strcmp(method, "GET") && strcmp(method, "HEAD"))) { strncat(extra, "Origin: ", sizeof extra - strlen(extra) - 1); strncat(extra, po, sizeof extra - strlen(extra) - 1); strncat(extra, "\n", sizeof extra - strlen(extra) - 1); }
    { char ref[1200] = ""; int pg_https = !strncasecmp(r->page_url, "https://", 8), rq_http = !strncasecmp(r->url, "http://", 7);
      if (!(pg_https && rq_http)) { if (!cross) { const char* h = strchr(r->page_url, '#'); snprintf(ref, sizeof ref, "%.*s", h ? (int)(h - r->page_url) : (int)strlen(r->page_url), r->page_url); } else snprintf(ref, sizeof ref, "%s/", po); }
      if (ref[0] && strcmp(po, "null")) { strncat(extra, "Referer: ", sizeof extra - strlen(extra) - 1); strncat(extra, ref, sizeof extra - strlen(extra) - 1); strncat(extra, "\n", sizeof extra - strlen(extra) - 1); } }
    { char sf[160]; snprintf(sf, sizeof sf, "Sec-Fetch-Site: %s\nSec-Fetch-Mode: %s\nSec-Fetch-Dest: empty\n", !cross ? "same-origin" : same_site(po, ro) ? "same-site" : "cross-site", r->mode == NCORS_MODE_NO_CORS ? "no-cors" : r->mode == NCORS_MODE_SAME_ORIGIN ? "same-origin" : "cors"); strncat(extra, sf, sizeof extra - strlen(extra) - 1); }
    /* preflight */
    if (need_pf) {
        char key[2200]; snprintf(key, sizeof key, "%s|%s|%s|%s|%d", po, r->url, method, hs.unsafe, cred_inc);
        if (!pf_cached(key)) {
            char pfh[1400]; snprintf(pfh, sizeof pfh, "Origin: %s\nAccess-Control-Request-Method: %s\n%s%s%sSec-Fetch-Mode: cors\nSec-Fetch-Dest: empty\nSec-Fetch-Site: %s\n", po, method, hs.has_unsafe ? "Access-Control-Request-Headers: " : "", hs.unsafe, hs.has_unsafe ? "\n" : "", same_site(po, ro) ? "same-site" : "cross-site");
            NHttpReq pq; memset(&pq, 0, sizeof pq); pq.method = "OPTIONS"; pq.url = r->url; pq.headers = pfh; pq.max_redirects = -1; pq.timeout_ms = 30000;
            NHttpResp pr; int ok = nhttp_perform(&pq, &pr);
            if (!ok) { snprintf(out->err, sizeof out->err, "CORS: preflight не выполнен: %s", pr.err); nhttp_resp_free(&pr); free(hs.clean); return 0; }
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
    if (cross) {
        if (r->mode == NCORS_MODE_NO_CORS) { nhttp_resp_free(out); snprintf(out->err, sizeof out->err, "ответ no-cors непрозрачен для скрипта"); return 0; }
        char why[200]; if (!allow_origin_ok(out->headers ? out->headers : "", po, cred_inc, why, sizeof why)) { nhttp_resp_free(out); snprintf(out->err, sizeof out->err, "%s", why); return 0; }
    }
    char* fh = filter_headers(out->headers ? out->headers : "", cross, cred_inc); if (fh) { free(out->headers); out->headers = fh; }
    return 1;
}
