/* Bounded URL resolution shared by HTML, CSS and JavaScript resource loading.
 * This unit is intentionally independent so the resolver can be ASan/UBSan-fuzzed. */
#include "webpage.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int url_is_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static int url_ci_prefix(const char *s, const char *pre) {
    if (!s || !pre) return 0;
    while (*pre) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
        s++; pre++;
    }
    return 1;
}

void webpage_resolve_url(const char* base, const char* rel, char* out, size_t cap) {
    /* Never turn an overlong URL into a different, truncated URL. Callers use an
       empty result as a hard failure and therefore cannot accidentally fetch a prefix. */
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!rel || !*rel) return;
    while (url_is_ws(*rel)) rel++;
    size_t rel_len = strlen(rel);
    if (!rel_len || rel_len >= 8192) return;
    if (url_ci_prefix(rel, "http://") || url_ci_prefix(rel, "https://") || url_ci_prefix(rel, "data:") || url_ci_prefix(rel, "javascript:") ||
        url_ci_prefix(rel, "mailto:") || url_ci_prefix(rel, "tel:") || rel[0] == '#') {
        if (rel_len >= cap) return;
        memcpy(out, rel, rel_len + 1);
        return;
    }
    size_t base_len = base ? strlen(base) : 0;
    if (base_len >= 8192) return;
    int web = base && (url_ci_prefix(base, "http://") || url_ci_prefix(base, "https://"));
    char tmp[8192];
    int written = -1;
#define URL_FORMAT(...) do { written = snprintf(tmp, sizeof tmp, __VA_ARGS__); if (written < 0 || (size_t)written >= sizeof tmp) return; } while (0)
    if (web) {
        const char* se = strstr(base, "://");
        if (!se) return;
        const char* auth = se + 3; const char* ae = auth + strcspn(auth, "/?#");
        size_t plen = strcspn(ae, "?#");
        if (rel[0] == '/' && rel[1] == '/') URL_FORMAT("%.*s:%s", (int)(se - base), base, rel);
        else if (rel[0] == '/') URL_FORMAT("%.*s%s", (int)(ae - base), base, rel);
        else if (rel[0] == '?') URL_FORMAT("%.*s%.*s%s", (int)(ae - base), base, (int)plen, ae, rel);
        else {
            size_t d = plen; while (d > 0 && ae[d - 1] != '/') d--;
            if (d == 0) URL_FORMAT("%.*s/%s", (int)(ae - base), base, rel);
            else URL_FORMAT("%.*s%.*s%s", (int)(ae - base), base, (int)d, ae, rel);
        }
        /* Normalize dot segments without ever processing a truncated intermediate. */
        char* ps = strstr(tmp, "://"); if (ps) { ps += 3; ps += strcspn(ps, "/?#"); }
        if (ps && *ps == '/') {
            char* qs = ps + strcspn(ps, "?#"); char* tail = strdup(qs);
            if (!tail) return;
            char* w = ps; const char* r = ps;
            while (r < qs) {
                const char* seg = r + 1; const char* se2 = seg; while (se2 < qs && *se2 != '/') se2++;
                size_t sl = (size_t)(se2 - seg);
                if (sl == 1 && seg[0] == '.') { if (se2 == qs) *w++ = '/'; }
                else if (sl == 2 && seg[0] == '.' && seg[1] == '.') { if (w > ps) { w--; while (w > ps && *w != '/') w--; } if (se2 == qs) *w++ = '/'; }
                else { *w++ = '/'; memmove(w, seg, sl); w += sl; }
                r = se2;
            }
            memmove(w, tail, strlen(tail) + 1); free(tail);
        }
    } else if (base && rel[0] != '/') {                       /* локальный файл */
        const char* sl = strrchr(base, '/');
        if (sl) URL_FORMAT("%.*s/%s", (int)(sl - base), base, rel); else URL_FORMAT("%s", rel);
    } else URL_FORMAT("%s", rel);
#undef URL_FORMAT
    size_t result_len = strlen(tmp);
    if (result_len >= cap) return;
    memcpy(out, tmp, result_len + 1);
}

/* ══════════════ таблицы тегов ══════════════ */
