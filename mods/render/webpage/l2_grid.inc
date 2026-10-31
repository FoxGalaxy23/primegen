/* CSS Grid (включается в layout2.c).
   Поддержано: grid-template-columns/rows (px, %, em, fr, auto, min/max-content, minmax(), fit-content(), repeat(N|auto-fill|auto-fit, …),
   именованные линии [a b]), grid-template-areas, grid-auto-rows/columns, grid-auto-flow (row|column|dense),
   размещение по линиям (grid-row/column[-start/-end], span, отрицательные номера, имена линий и областей, grid-area),
   gap, justify-/align-items и -self, justify-/align-content, авто-поля, растягивание строк/колонок, коллапс пустых auto-fit колонок. */
#define GMAXT 64
#define GINF 1e9f
typedef struct { float mn, mx, fr; int mn_kind, mx_kind; float fit; int collapse, auto_fit; float base, lim, size; } GT;
/* mn_kind: 0 px, 1 auto/min-content, 2 max-content; mx_kind: 0 px, 1 fr, 2 auto/max-content, 3 min-content */
typedef struct { char n[3][20]; uint8_t c; } GLine;
typedef struct { int kind; int n; char name[24]; } GP;              /* kind: 0 auto, 1 линия №, 2 span n, 3 имя линии (n-е вхождение) */

static void gt_defaults(GT* t) { memset(t, 0, sizeof *t); t->mn_kind = 1; t->mx_kind = 2; }

static int gt_len(const char* s, float ref, float* out) {            /* px, %, em, vw… */
    Len l; if (!css_parse_len(s, &l, 16, 16) || !l.set) return 0;
    *out = l.px + l.pct * ref / 100.0f + l.vw * VW + l.vh * VH; return 1;
}
static void gt_parse_one(const char* tok, float ref, GT* t) {
    gt_defaults(t); float v;
    if (!strncasecmp(tok, "minmax(", 7)) {
        char a[64] = "", c[64] = ""; const char* p = tok + 7; const char* cm = NULL; int d = 0;
        for (const char* q = p; *q; q++) { if (*q == '(') d++; else if (*q == ')') { if (!d) break; d--; } else if (*q == ',' && !d) { cm = q; break; } }
        if (cm) { snprintf(a, sizeof a, "%.*s", (int)(cm - p), p); snprintf(c, sizeof c, "%s", cm + 1); char* rp = strrchr(c, ')'); if (rp) *rp = 0; }
        char* a1 = a; while (isspace((unsigned char)*a1)) a1++; char* c1 = c; while (isspace((unsigned char)*c1)) c1++;
        if (!strncasecmp(a1, "max-content", 11)) t->mn_kind = 2; else if (!strncasecmp(a1, "auto", 4) || !strncasecmp(a1, "min-content", 11)) t->mn_kind = 1;
        else if (gt_len(a1, ref, &v)) { t->mn_kind = 0; t->mn = v; }
        char* fe; float f = strtof(c1, &fe);
        if (fe != c1 && !strncasecmp(fe, "fr", 2)) { t->mx_kind = 1; t->fr = f; }
        else if (!strncasecmp(c1, "min-content", 11)) t->mx_kind = 3; else if (!strncasecmp(c1, "auto", 4) || !strncasecmp(c1, "max-content", 11)) t->mx_kind = 2;
        else if (gt_len(c1, ref, &v)) { t->mx_kind = 0; t->mx = v; }
        if (t->mx_kind == 0 && t->mn_kind == 0 && t->mx < t->mn) t->mx = t->mn;
        return;
    }
    if (!strncasecmp(tok, "fit-content(", 12)) { if (gt_len(tok + 12, ref, &v)) t->fit = v; t->mn_kind = 1; t->mx_kind = 2; if (t->fit <= 0) t->fit = GINF; return; }
    char* fe; float f = strtof(tok, &fe);
    if (fe != tok && !strncasecmp(fe, "fr", 2)) { t->mx_kind = 1; t->fr = f; t->mn_kind = 1; return; }
    if (!strncasecmp(tok, "auto", 4) || !strncasecmp(tok, "max-content", 11)) return;
    if (!strncasecmp(tok, "min-content", 11)) { t->mx_kind = 3; return; }
    if (gt_len(tok, ref, &v)) { t->mn_kind = 0; t->mn = v; t->mx_kind = 0; t->mx = v; }
}

/* разбор списка треков; lines[i] — имена линии перед треком i (i = число треков — последняя линия) */
static int grid_parse(const char* s, float avail, float gap, GT* out, GLine* lines, int max) {
    int n = 0; const char* p = s;
    while (*p && n < max) {
        while (isspace((unsigned char)*p)) p++; if (!*p) break;
        if (*p == '[') {                                              /* имена линии */
            p++; while (*p && *p != ']') {
                while (isspace((unsigned char)*p)) p++; const char* e = p; while (*e && *e != ']' && !isspace((unsigned char)*e)) e++;
                if (e > p && lines[n].c < 3) { size_t l = (size_t)(e - p); if (l > 19) l = 19; memcpy(lines[n].n[lines[n].c], p, l); lines[n].n[lines[n].c][l] = 0; lines[n].c++; }
                p = e;
            }
            if (*p == ']') p++; continue;
        }
        if (!strncasecmp(p, "repeat(", 7)) {
            p += 7; int count = 0, fit = 0, fill = 0;
            if (!strncasecmp(p, "auto-fit", 8)) { fit = 1; p += 8; } else if (!strncasecmp(p, "auto-fill", 9)) { fill = 1; p += 9; }
            else { count = atoi(p); while (isdigit((unsigned char)*p)) p++; }
            while (isspace((unsigned char)*p) || *p == ',') p++;
            const char* e = p; int d = 1; while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; if (d) e++; }
            char* inner = la((size_t)(e - p) + 1); memcpy(inner, p, (size_t)(e - p)); inner[e - p] = 0;
            GT tmp[GMAXT]; GLine tl[GMAXT + 1]; memset(tl, 0, sizeof tl); int m = grid_parse(inner, avail, gap, tmp, tl, GMAXT);
            if (m > 0) {
                if (fit || fill) {
                    float unit = gap * (m - 1), used = 0; for (int k = 0; k < n; k++) used += (out[k].mn_kind == 0 ? out[k].mn : (out[k].mx_kind == 0 ? out[k].mx : 0)) + gap;
                    for (int k = 0; k < m; k++) { float u = tmp[k].mn_kind == 0 ? tmp[k].mn : (tmp[k].mx_kind == 0 ? tmp[k].mx : 0); unit += u; }
                    if (unit < 1) unit = 1; float room = avail - used;
                    count = avail > 0 ? (int)((room + gap) / (unit + gap)) : 1; if (count < 1) count = 1; if (count * m > GMAXT - n) count = (GMAXT - n) / m; if (count < 1) count = 1;
                }
                for (int c = 0; c < count && n < max; c++) for (int k = 0; k < m && n < max; k++) {
                    for (int q = 0; q < tl[k].c && lines[n].c < 3; q++) strcpy(lines[n].n[lines[n].c++], tl[k].n[q]);
                    out[n] = tmp[k]; out[n].auto_fit = fit; n++;
                }
                for (int q = 0; q < tl[m].c && lines[n].c < 3; q++) strcpy(lines[n].n[lines[n].c++], tl[m].n[q]);
            }
            p = *e ? e + 1 : e; continue;
        }
        const char* e = p; int d = 0; while (*e && (d > 0 || !isspace((unsigned char)*e)) && !(d == 0 && *e == '[')) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
        char tok[128]; size_t tl = (size_t)(e - p); if (tl > 127) tl = 127; memcpy(tok, p, tl); tok[tl] = 0; p = e;
        gt_parse_one(tok, avail, &out[n]); n++;
    }
    return n;
}

/* Разбирает нормализованную grid-template-areas (строки через '\n', имена через пробел) на строки токенов */
static int grid_area_rows(const char* s, char toks[16][24][24], int* ncols) {
    int nr = 0, nc = 0;
    while (*s && nr < 16) {
        const char* le = strchr(s, '\n'); size_t ll = le ? (size_t)(le - s) : strlen(s);
        const char* p = s, * end = s + ll; int c = 0;
        while (p < end && c < 24) {
            while (p < end && isspace((unsigned char)*p)) p++;
            const char* ts = p; while (p < end && !isspace((unsigned char)*p)) p++;
            size_t tl = (size_t)(p - ts); if (tl == 0) continue;
            if (tl > 23) tl = 23; memcpy(toks[nr][c], ts, tl); toks[nr][c][tl] = 0; c++;
        }
        if (c > nc) nc = c;
        nr++; if (!le) break; s = le + 1;
    }
    *ncols = nc; return nr;
}

static void gp_parse(const char* s, GP* g) {
    memset(g, 0, sizeof *g); while (isspace((unsigned char)*s)) s++; if (!*s || !strncasecmp(s, "auto", 4)) return;
    if (!strncasecmp(s, "span", 4)) { s += 4; while (isspace((unsigned char)*s)) s++; g->kind = 2; g->n = isdigit((unsigned char)*s) ? atoi(s) : 1; if (g->n < 1) g->n = 1; if (g->n > 60) g->n = 60; return; }
    if (*s == '-' || isdigit((unsigned char)*s)) { g->kind = 1; g->n = atoi(s); if (!g->n) g->kind = 0; return; }
    size_t i = 0; while (s[i] && !isspace((unsigned char)s[i]) && i < 23) { g->name[i] = s[i]; i++; } g->name[i] = 0;
    const char* r = s + i; while (isspace((unsigned char)*r)) r++; g->kind = 3; g->n = (*r == '-' || isdigit((unsigned char)*r)) ? atoi(r) : 1; if (g->n < 1) g->n = 1;
}
static int gl_find(const GLine* lines, int nl, const char* nm, int occ, int is_end) {
    char alt[40]; snprintf(alt, sizeof alt, "%.30s-%s", nm, is_end ? "end" : "start"); int seen = 0;
    for (int i = 0; i < nl; i++) for (int q = 0; q < lines[i].c; q++) if (!strcmp(lines[i].n[q], nm) || !strcmp(lines[i].n[q], alt)) { if (++seen == occ) return i; }
    return -1;
}
/* GP начало/конец → стартовая линия (0-based, -1 — авто) и размах */
static void gp_axis(const GP* s, const GP* e, int nlines, const GLine* lines, int* start, int* span) {
    int sl = -1, el = -1, ss = s->kind == 2 ? s->n : 0, es = e->kind == 2 ? e->n : 0;
    if (s->kind == 1) { sl = s->n > 0 ? s->n - 1 : nlines + s->n; } else if (s->kind == 3) sl = gl_find(lines, nlines, s->name, s->n, 0);
    if (e->kind == 1) { el = e->n > 0 ? e->n - 1 : nlines + e->n; } else if (e->kind == 3) el = gl_find(lines, nlines, e->name, e->n, 1);
    if (sl >= GMAXT) sl = GMAXT - 1; if (el >= GMAXT) el = GMAXT - 1;
    if (sl >= 0 && el >= 0) { if (el < sl) { int t = sl; sl = el; el = t; } *start = sl; *span = el - sl > 0 ? el - sl : 1; }
    else if (sl >= 0) { *start = sl; *span = es ? es : (ss ? ss : 1); }
    else if (el >= 0) { *span = ss ? ss : (es ? es : 1); *start = el - *span; if (*start < 0) *start = 0; }
    else { *start = -1; *span = ss ? ss : (es ? es : 1); }
}

typedef struct { Box* b; int r, c, rs, cs; int rdef, cdef; int s0, s1; float oh; } GI;

static void grid_align_off(int mode, float free_sp, int n, float* gap_add, float* lead) {   /* justify/align-content */
    *gap_add = 0; *lead = 0; if (free_sp <= 0 || n <= 0) return;
    switch (mode) {
    case JC_CENTER: *lead = free_sp / 2; break; case JC_END: *lead = free_sp; break;
    case JC_BETWEEN: if (n > 1) *gap_add = free_sp / (n - 1); break;
    case JC_AROUND: *gap_add = free_sp / n; *lead = *gap_add / 2; break;
    case JC_EVENLY: *gap_add = free_sp / (n + 1); *lead = *gap_add; break;
    }
}

/* размеры треков по вкладу элементов; minc/maxc — внешние вклады элементов по этой оси */
static void grid_size_tracks(GT* T, int nt, float avail, float gap, int stretch, GI* it, int k, int axis_cols, float* minc, float* maxc) {
    for (int t = 0; t < nt; t++) {
        GT* g = &T[t]; g->base = g->mn_kind == 0 ? g->mn : 0; g->lim = g->mx_kind == 0 ? g->mx : (g->mx_kind == 1 ? GINF : 0);
        if (g->mx_kind == 0 && g->lim < g->base) g->lim = g->base;
    }
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < k; i++) {                        /* pass 0 — span 1, pass 1 — span > 1 */
        int s = axis_cols ? it[i].c : it[i].r, sp = axis_cols ? it[i].cs : it[i].rs; if ((pass == 0) != (sp == 1)) continue;
        if (s < 0 || s + sp > nt) continue;
        float mi = minc[i], ma = maxc[i];
        if (sp == 1) {
            GT* g = &T[s];
            if (g->mn_kind != 0) { float c = g->mn_kind == 2 ? ma : mi; if (c > g->base) g->base = c; }
            if (g->mx_kind == 2) { float c = g->fit > 0 && g->fit < GINF ? (ma < g->fit ? ma : g->fit) : ma; if (c > g->lim) g->lim = c; }
            else if (g->mx_kind == 3) { if (mi > g->lim) g->lim = mi; }
        } else {
            float sum_b = gap * (sp - 1), sum_l = gap * (sp - 1); int nb = 0, nl2 = 0;
            for (int t = s; t < s + sp; t++) { sum_b += T[t].base; sum_l += (T[t].lim >= GINF ? T[t].base : T[t].lim); if (T[t].mn_kind != 0) nb++; if (T[t].mx_kind == 2 || T[t].mx_kind == 3) nl2++; }
            if (mi > sum_b && nb) { float add = (mi - sum_b) / nb; for (int t = s; t < s + sp; t++) if (T[t].mn_kind != 0) T[t].base += add; }
            if (ma > sum_l && nl2) { float add = (ma - sum_l) / nl2; for (int t = s; t < s + sp; t++) if (T[t].mx_kind == 2 || T[t].mx_kind == 3) T[t].lim = (T[t].lim >= GINF ? T[t].base : T[t].lim) + add; }
        }
    }
    for (int t = 0; t < nt; t++) { GT* g = &T[t]; if (g->mx_kind != 1 && g->lim < g->base) g->lim = g->base; g->size = g->base; }
    int nfr = 0; float frsum = 0, gaps = 0; for (int t = 0; t < nt; t++) { if (T[t].mx_kind == 1) { nfr++; frsum += T[t].fr; } if (!T[t].collapse) gaps += gap; } if (nt && gaps >= gap) gaps -= gap;
    if (avail >= 0) {
        float used = gaps; for (int t = 0; t < nt; t++) used += T[t].base; float fs = avail - used;
        if (fs > 0 && nfr < nt) {                                                      /* раздать место нефлексибельным до предела роста */
            for (int iter = 0; iter < nt + 1 && fs > 0.01f; iter++) {
                int cnt = 0; for (int t = 0; t < nt; t++) if (T[t].mx_kind != 1 && T[t].size < T[t].lim) cnt++;
                if (!cnt) break; float share = fs / cnt, spent = 0;
                for (int t = 0; t < nt; t++) if (T[t].mx_kind != 1 && T[t].size < T[t].lim) { float room = T[t].lim - T[t].size, add = share < room ? share : room; T[t].size += add; spent += add; }
                fs -= spent; if (spent < 0.001f) break;
            }
        }
        if (nfr) {
            float left = avail - gaps; for (int t = 0; t < nt; t++) if (T[t].mx_kind != 1) left -= T[t].size;
            int fixed[GMAXT] = {0}; float hyp = 0; (void)frsum;
            for (int iter = 0; iter < nt + 1; iter++) {
                float rem = left, rs = 0; for (int t = 0; t < nt; t++) if (T[t].mx_kind == 1) { if (fixed[t]) rem -= T[t].base; else rs += T[t].fr; }
                if (rs < 1) rs = 1; hyp = rem > 0 ? rem / rs : 0; int changed = 0;
                for (int t = 0; t < nt; t++) if (T[t].mx_kind == 1 && !fixed[t] && T[t].fr * hyp < T[t].base) { fixed[t] = 1; changed = 1; }
                if (!changed) break;
            }
            for (int t = 0; t < nt; t++) if (T[t].mx_kind == 1) T[t].size = fixed[t] ? T[t].base : T[t].fr * hyp;
        } else if (stretch) {
            float used2 = gaps; for (int t = 0; t < nt; t++) used2 += T[t].size; float fs2 = avail - used2; int cnt = 0;
            for (int t = 0; t < nt; t++) if (T[t].mx_kind == 2 && !T[t].collapse) cnt++;
            if (fs2 > 0.01f && cnt) for (int t = 0; t < nt; t++) if (T[t].mx_kind == 2 && !T[t].collapse) T[t].size += fs2 / cnt;
        }
    } else if (nfr) {                                                                  /* размер не определён: fr по содержимому */
        float hyp = 0; for (int t = 0; t < nt; t++) if (T[t].mx_kind == 1 && T[t].fr > 0) { float h = T[t].base / (T[t].fr < 1 ? 1 : T[t].fr); if (h > hyp) hyp = h; }
        for (int t = 0; t < nt; t++) if (T[t].mx_kind == 1) { float s2 = T[t].fr * hyp; T[t].size = s2 > T[t].base ? s2 : T[t].base; }
    }
}

static uint8_t g_occ[GMAXT][GMAXT];
#define GOCC(r_, c_) (((r_) < GMAXT && (c_) < GMAXT) ? g_occ[r_][c_] : 0)
static void g_mark(const GI* g) { for (int r = g->r; r < g->r + g->rs && r < GMAXT; r++) for (int c = g->c; c < g->c + g->cs && c < GMAXT; c++) g_occ[r][c] = 1; }
static int g_overlap(int r0, int c0, int rs, int cs) { for (int r = r0; r < r0 + rs; r++) for (int c = c0; c < c0 + cs; c++) if (GOCC(r, c)) return 1; return 0; }

static float layout_grid(Box* b, float cx, float cy, float cw, float ch, LS* ls) {
    const NodeStyle* st = b->st; int n = 0;
    for (Box* c = b->first; c; c = c->next) n++;
    Box** bx = la(sizeof(Box*) * (size_t)(n ? n : 1));
    if (!bx) return 0;
    int k = 0;
    for (Box* c = b->first; c; c = c->next) {
        if (c->oof) { register_abs(ls, c, cx, cy); continue; }
        if (c->kind == K_BR || c->kind == K_TEXT) continue;
        bx[k++] = c;
    }
    if (k <= 0) return 0; /* Не допускаем отрицательный размер после повреждённого/переполненного счётчика. */
    size_t item_count = (size_t)k;
    if (item_count > SIZE_MAX / sizeof(GI)) return 0;
    int flow_col = st->grid_flow & 1, dense = (st->grid_flow & 2) != 0; float gapc = st->gap_c, gapr = st->gap_r;
    /* ── явные треки ── */
    GT col_t[GMAXT], row_t[GMAXT]; GLine col_l[GMAXT + 1], row_l[GMAXT + 1]; memset(col_l, 0, sizeof col_l); memset(row_l, 0, sizeof row_l);
    int subgrid_cols = st->grid_subgrid_cols && b->grid_col_size && b->grid_col_count > 0;
    int subgrid_rows = st->grid_subgrid_rows && b->grid_row_size && b->grid_row_count > 0;
    int ncol_e = 0;
    if (subgrid_cols) {
        ncol_e = b->grid_col_count > GMAXT ? GMAXT : b->grid_col_count;
        gapc = b->grid_col_gap;
        for (int t=0; t<ncol_e; t++) { gt_defaults(&col_t[t]); col_t[t].mn_kind=0; col_t[t].mx_kind=0; col_t[t].mn=col_t[t].mx=b->grid_col_size[t]; }
    } else ncol_e = st->grid_cols[0] ? grid_parse(st->grid_cols, cw, gapc, col_t, col_l, GMAXT) : 0;
    int nrow_e = subgrid_rows ? (b->grid_row_count > GMAXT ? GMAXT : b->grid_row_count)
        : (st->grid_rows[0] ? grid_parse(st->grid_rows, ch >= 0 ? ch : -1, gapr, row_t, row_l, GMAXT) : 0);
    if (subgrid_rows) {
        gapr = b->grid_row_gap;
        for (int t=0; t<nrow_e; t++) { gt_defaults(&row_t[t]); row_t[t].mn_kind=0; row_t[t].mx_kind=0; row_t[t].mn=row_t[t].mx=b->grid_row_size[t]; }
    }
    /* области → неявные линии name-start / name-end */
    static char atoks[16][24][24]; int anr = 0, anc = 0;
    if (st->grid_areas[0]) {
        anr = grid_area_rows(st->grid_areas, atoks, &anc);
        if (anc > GMAXT) anc = GMAXT;
        if (anc > ncol_e) { for (int t = ncol_e; t < anc; t++) gt_defaults(&col_t[t]); ncol_e = anc; }
        if (anr > nrow_e) { for (int t = nrow_e; t < anr; t++) gt_defaults(&row_t[t]); nrow_e = anr; }
        for (int r = 0; r < anr; r++) for (int c = 0; c < anc; c++) {
            const char* nm = atoks[r][c]; if (!nm[0] || !strcmp(nm, ".")) continue;
            int first = 1; for (int r2 = 0; r2 <= r && first; r2++) for (int c2 = 0; c2 < anc; c2++) if (!strcmp(atoks[r2][c2], nm) && (r2 < r || c2 < c)) { first = 0; break; }
            if (!first) continue;
            int c1 = c; while (c1 + 1 < anc && !strcmp(atoks[r][c1 + 1], nm)) c1++; int r1 = r; while (r1 + 1 < anr && !strcmp(atoks[r1 + 1][c], nm)) r1++;
            char sn[40], en[40]; snprintf(sn, sizeof sn, "%.30s-start", nm); snprintf(en, sizeof en, "%.30s-end", nm);
            if (col_l[c].c < 3) strcpy(col_l[c].n[col_l[c].c++], sn); if (col_l[c1 + 1].c < 3) strcpy(col_l[c1 + 1].n[col_l[c1 + 1].c++], en);
            if (row_l[r].c < 3) strcpy(row_l[r].n[row_l[r].c++], sn); if (row_l[r1 + 1].c < 3) strcpy(row_l[r1 + 1].n[row_l[r1 + 1].c++], en);
        }
    }
    /* ── автотреки ── */
    GT auto_c[8], auto_r[8]; GLine dummy[9]; memset(dummy, 0, sizeof dummy);
    int nac = st->grid_auto_c[0] ? grid_parse(st->grid_auto_c, cw, gapc, auto_c, dummy, 8) : 0, nar = st->grid_auto_r[0] ? grid_parse(st->grid_auto_r, ch >= 0 ? ch : -1, gapr, auto_r, dummy, 8) : 0;
    /* ── размещение ── */
    GI* gi = la(sizeof(GI) * item_count);
    if (!gi) return 0;
    memset(g_occ, 0, sizeof g_occ);
    for (int i = 0; i < k; i++) {
        const NodeStyle* s = bx[i]->st; GI* g = &gi[i]; memset(g, 0, sizeof *g); g->b = bx[i];
        char rs_[24], re_[24], cs_[24], ce_[24]; snprintf(rs_, sizeof rs_, "%s", s->grid_row_s); snprintf(re_, sizeof re_, "%s", s->grid_row_e); snprintf(cs_, sizeof cs_, "%s", s->grid_col_s); snprintf(ce_, sizeof ce_, "%s", s->grid_col_e);
        if (s->grid_area_name[0] && !rs_[0] && !cs_[0]) { snprintf(rs_, sizeof rs_, "%.23s", s->grid_area_name); snprintf(cs_, sizeof cs_, "%.23s", s->grid_area_name); snprintf(re_, sizeof re_, "%.23s", s->grid_area_name); snprintf(ce_, sizeof ce_, "%.23s", s->grid_area_name); }
        GP a, e; int st0, sp0;
        gp_parse(cs_, &a); gp_parse(ce_, &e); gp_axis(&a, &e, ncol_e + 1, col_l, &st0, &sp0); g->c = st0; g->cs = sp0;
        if (!cs_[0] && !ce_[0] && s->grid_span > 1) g->cs = s->grid_span;                                   /* совместимость со старым grid-span */
        gp_parse(rs_, &a); gp_parse(re_, &e); gp_axis(&a, &e, nrow_e + 1, row_l, &st0, &sp0); g->r = st0; g->rs = sp0;
        if (g->cs > GMAXT) g->cs = GMAXT; if (g->rs > GMAXT) g->rs = GMAXT; if (g->cs < 1) g->cs = 1; if (g->rs < 1) g->rs = 1;
        g->cdef = g->c >= 0; g->rdef = g->r >= 0;
    }
    int min_n = flow_col ? nrow_e : ncol_e; if (min_n < 1) min_n = 1;
    for (int i = 0; i < k; i++) { int ms = flow_col ? gi[i].rs : gi[i].cs; if (ms > min_n) min_n = ms; }
    uint8_t* done = la(item_count);
    if (!done) return 0;
    memset(done, 0, item_count);
    for (int i = 0; i < k; i++) if (gi[i].cdef && gi[i].rdef) { g_mark(&gi[i]); done[i] = 1; }
    { int* rowcur = la(sizeof(int) * GMAXT); memset(rowcur, 0, sizeof(int) * GMAXT);                      /* «главная» ось определена, побочная — авто */
      for (int i = 0; i < k; i++) { GI* g = &gi[i]; if (done[i]) continue; if (!(flow_col ? g->cdef : g->rdef)) continue;
        int mj = flow_col ? g->c : g->r, msz = flow_col ? g->cs : g->rs, nsz = flow_col ? g->rs : g->cs; if (mj >= GMAXT) mj = GMAXT - 1;
        int mn = dense ? 0 : rowcur[mj];
        for (; mn < GMAXT - nsz; mn++) { int ov = flow_col ? g_overlap(mn, mj, nsz, msz) : g_overlap(mj, mn, msz, nsz); if (!ov) break; }
        if (flow_col) g->r = mn; else g->c = mn; if (!dense) rowcur[mj] = mn + nsz; g->cdef = g->rdef = 1; g_mark(g); done[i] = 1; } }
    { int cm = 0, cn = 0;                                                                                  /* курсор: major, minor */
      for (int i = 0; i < k; i++) { GI* g = &gi[i]; if (done[i]) continue; int mn_def = flow_col ? g->rdef : g->cdef; int nsz = flow_col ? g->rs : g->cs, msz = flow_col ? g->cs : g->rs;
        if (dense) { cm = 0; cn = 0; }
        if (mn_def) { int mnv = flow_col ? g->r : g->c; if (!dense && mnv < cn) cm++; cn = mnv; }
        for (int guard = 0; guard < GMAXT * GMAXT; guard++) {
            if (!mn_def && cn + nsz > min_n) { cm++; cn = 0; continue; }
            int ov = flow_col ? g_overlap(cn, cm, nsz, msz) : g_overlap(cm, cn, msz, nsz);
            if (!ov) break; if (mn_def) cm++; else cn++;
        }
        if (cm >= GMAXT) cm = GMAXT - 1; if (cn >= GMAXT) cn = GMAXT - 1;
        if (flow_col) { g->c = cm; g->r = cn; } else { g->r = cm; g->c = cn; } g->cdef = g->rdef = 1; g_mark(g); done[i] = 1; } }
    int ncol = ncol_e, nrow = nrow_e; for (int i = 0; i < k; i++) { if (gi[i].c + gi[i].cs > ncol) ncol = gi[i].c + gi[i].cs; if (gi[i].r + gi[i].rs > nrow) nrow = gi[i].r + gi[i].rs; }
    if (ncol > GMAXT) ncol = GMAXT; if (nrow > GMAXT) nrow = GMAXT; if (ncol < 1) ncol = 1; if (nrow < 1) nrow = 1;
    for (int i = 0; i < k; i++) { if (gi[i].c + gi[i].cs > ncol) gi[i].cs = ncol - gi[i].c; if (gi[i].r + gi[i].rs > nrow) gi[i].rs = nrow - gi[i].r; if (gi[i].cs < 1) gi[i].cs = 1; if (gi[i].rs < 1) gi[i].rs = 1; }
    for (int t = ncol_e; t < ncol; t++) { if (nac) col_t[t] = auto_c[(t - ncol_e) % nac]; else gt_defaults(&col_t[t]); }
    for (int t = nrow_e; t < nrow; t++) { if (nar) row_t[t] = auto_r[(t - nrow_e) % nar]; else gt_defaults(&row_t[t]); }
    for (int t = 0; t < ncol; t++) if (col_t[t].auto_fit) {                                                /* auto-fit: пустые колонки схлопываются */
        int used = 0; for (int i = 0; i < k; i++) if (gi[i].c <= t && t < gi[i].c + gi[i].cs) used = 1;
        col_t[t].collapse = !used; if (!used) { col_t[t].mn_kind = 0; col_t[t].mn = 0; col_t[t].mx_kind = 0; col_t[t].mx = 0; }
    }
    /* ── колонки ── */
    float* minc = la(sizeof(float) * (size_t)k * 2); float* maxc = minc + k;
    for (int i = 0; i < k; i++) { Box* c = gi[i].b; edges(c, cw); float mw = c->mla ? 0 : c->ml, mr = c->mra ? 0 : c->mr; minc[i] = intr(c, 0) + mw + mr; maxc[i] = intr(c, 1) + mw + mr; }
    grid_size_tracks(col_t, ncol, cw, gapc, st->justify == JC_START, gi, k, 1, minc, maxc);
    float colsum = 0; int ncol_vis = 0; for (int t = 0; t < ncol; t++) { colsum += col_t[t].size; if (!col_t[t].collapse) ncol_vis++; }
    float cfree = cw - colsum - gapc * (ncol_vis > 0 ? ncol_vis - 1 : 0), cgap_add, clead; grid_align_off(st->justify, cfree, ncol_vis, &cgap_add, &clead);
    float colx[GMAXT + 1]; colx[0] = cx + clead; for (int t = 0; t < ncol; t++) colx[t + 1] = colx[t] + col_t[t].size + (col_t[t].collapse ? 0 : gapc + cgap_add);
    /* ── элементы: раскладка по итоговой ширине, замер высоты ── */
    float y0 = cy;
    /* Keep a rollback point: row-subgrid children must be relaid after the
       parent has resolved intrinsic/auto/fr track sizes, not only fixed tracks. */
    int child_cmd_mark = l2_dn, child_group_mark = g_sgn;
    for (int i = 0; i < k; i++) {
        GI* g = &gi[i]; Box* c = g->b; edges(c, cw); int c1 = g->c + g->cs; if (c1 > ncol) c1 = ncol;
        float cellw = colx[c1] - colx[g->c] - (col_t[c1 - 1].collapse ? 0 : gapc + cgap_add); if (cellw < 0) cellw = 0;
        int ja = c->st->justify_self != AI_AUTO ? c->st->justify_self : st->justify_items;
        c->mforce = 1; c->fml = c->mla ? 0 : c->ml; c->fmr = c->mra ? 0 : c->mr;
        float fw = cellw - c->fml - c->fmr; if (fw < 0) fw = 0; float forced = fw;
        if (c->st->width.set || c->repl) forced = -1;
        else if (ja != AI_STRETCH || c->mla || c->mra) { float mx = intr(c, 1), mn = intr(c, 0); float w = mx < fw ? mx : fw; if (w < mn) w = mn; forced = w; }
        if (c->st->grid_subgrid_cols && g->cs > 0) {
            int span=g->cs; if (span>GMAXT) span=GMAXT;
            c->grid_col_size=la(sizeof(float)*(size_t)span);
            if (c->grid_col_size) { for (int t=0;t<span;t++) c->grid_col_size[t]=col_t[g->c+t].size; c->grid_col_count=span; c->grid_col_gap=gapc; c->grid_col_origin=g->c; }
        }
        if (c->st->grid_subgrid_rows && g->rs > 0 && g->r >= 0 && g->r + g->rs <= nrow) {
            int span=g->rs; if (span>GMAXT) span=GMAXT;
            int definite=1;
            for(int t=0;t<span;t++) { GT* rt=&row_t[g->r+t]; if(rt->mn_kind!=0 || rt->mx_kind!=0 || fabsf(rt->mn-rt->mx)>0.01f) { definite=0; break; } }
            if(definite) {
                c->grid_row_size=la(sizeof(float)*(size_t)span);
                if(c->grid_row_size) { for(int t=0;t<span;t++) c->grid_row_size[t]=row_t[g->r+t].mn; c->grid_row_count=span; c->grid_row_gap=gapr; c->grid_row_origin=g->r; }
            }
        }
        g->s0 = l2_dn; lbb(c, colx[g->c], y0 + c->mt, cw, forced, -1, -1, ls); g->s1 = l2_dn; c->mforce = 0;
        float dx = 0, free_w = cellw - c->w - c->fml - c->fmr;
        if (free_w > 0) { if (c->mla && c->mra) dx = free_w / 2; else if (c->mla) dx = free_w; else if (c->mra) dx = 0; else if (ja == AI_CENTER) dx = free_w / 2; else if (ja == AI_END) dx = free_w; }
        if (dx) shift_box(c, g->s0, g->s1, dx, 0);
        g->oh = c->mt + c->h + c->mb;
    }
    /* ── строки ── */
    float* rmin = la(sizeof(float) * (size_t)k * 2); float* rmax = rmin + k; for (int i = 0; i < k; i++) rmin[i] = rmax[i] = gi[i].oh;
    grid_size_tracks(row_t, nrow, ch >= 0 ? ch : -1, gapr, ch >= 0 && st->align_content == JC_START, gi, k, 0, rmin, rmax);
    float rowsum = 0; for (int t = 0; t < nrow; t++) rowsum += row_t[t].size;
    float rfree = ch >= 0 ? ch - rowsum - gapr * (nrow - 1) : 0, rgap_add, rlead; grid_align_off(st->align_content, rfree, nrow, &rgap_add, &rlead);
    float rowy[GMAXT + 1]; rowy[0] = cy + rlead; for (int t = 0; t < nrow; t++) rowy[t + 1] = rowy[t] + row_t[t].size + gapr + rgap_add;

    /* A row subgrid shares its parent's resolved tracks, including auto and fr
       tracks. The first pass above measured child intrinsic sizes independently;
       now that parent tracks are final, discard those commands and lay the grid
       items out once more with the inherited row sizes. This is intentionally a
       bounded two-pass algorithm: it does not claim full cyclic CSS Grid sizing. */
    int children_at_row_positions = 0;
    for (int i = 0; i < k; i++) {
        GI* g = &gi[i]; Box* c = g->b;
        if (c->st->grid_subgrid_rows && g->rs > 0 && g->r >= 0 && g->r + g->rs <= nrow) {
            children_at_row_positions = 1; break;
        }
    }
    if (children_at_row_positions) {
        l2_dn = child_cmd_mark; grp_trunc(child_group_mark); b->fb = -1e9f;
        for (int i = 0; i < k; i++) {
            GI* g = &gi[i]; Box* c = g->b; edges(c, cw);
            int c1 = g->c + g->cs; if (c1 > ncol) c1 = ncol;
            float cellw = colx[c1] - colx[g->c] - (col_t[c1 - 1].collapse ? 0 : gapc + cgap_add); if (cellw < 0) cellw = 0;
            int ja = c->st->justify_self != AI_AUTO ? c->st->justify_self : st->justify_items;
            c->mforce = 1; c->fml = c->mla ? 0 : c->ml; c->fmr = c->mra ? 0 : c->mr;
            float fw = cellw - c->fml - c->fmr; if (fw < 0) fw = 0; float forced = fw;
            if (c->st->width.set || c->repl) forced = -1;
            else if (ja != AI_STRETCH || c->mla || c->mra) { float mx = intr(c, 1), mn = intr(c, 0); float w = mx < fw ? mx : fw; if (w < mn) w = mn; forced = w; }
            if (c->st->grid_subgrid_cols && g->cs > 0) {
                int span = g->cs; if (span > GMAXT) span = GMAXT;
                c->grid_col_size = la(sizeof(float) * (size_t)span);
                if (c->grid_col_size) { for (int t = 0; t < span; t++) c->grid_col_size[t] = col_t[g->c + t].size; c->grid_col_count = span; c->grid_col_gap = gapc; c->grid_col_origin = g->c; }
            }
            if (c->st->grid_subgrid_rows && g->rs > 0 && g->r >= 0 && g->r + g->rs <= nrow) {
                int span = g->rs; if (span > GMAXT) span = GMAXT;
                c->grid_row_size = la(sizeof(float) * (size_t)span);
                if (c->grid_row_size) {
                    for (int t = 0; t < span; t++) c->grid_row_size[t] = row_t[g->r + t].size;
                    c->grid_row_count = span; c->grid_row_gap = gapr; c->grid_row_origin = g->r;
                }
            }
            g->s0 = l2_dn;
            lbb(c, colx[g->c], rowy[g->r] + c->mt, cw, forced, -1, -1, ls);
            g->s1 = l2_dn; c->mforce = 0;
            float free_w = cellw - c->w - c->fml - c->fmr, dx = 0;
            if (free_w > 0) { if (c->mla && c->mra) dx = free_w / 2; else if (c->mla) dx = free_w; else if (c->mra) dx = 0; else if (ja == AI_CENTER) dx = free_w / 2; else if (ja == AI_END) dx = free_w; }
            if (dx) shift_box(c, g->s0, g->s1, dx, 0);
            g->oh = c->mt + c->h + c->mb;
        }
    }

    for (int i = 0; i < k; i++) {
        GI* g = &gi[i]; Box* c = g->b; int r1 = g->r + g->rs; if (r1 > nrow) r1 = nrow;
        float cell_h = rowy[r1] - rowy[g->r] - gapr - rgap_add; if (cell_h < 0) cell_h = 0;
        int a = c->st->align_self != AI_AUTO ? c->st->align_self : st->align_items;
        float dy = children_at_row_positions ? 0 : rowy[g->r] - y0, oh = g->oh;
        if (a == AI_STRETCH) { if (!c->st->height.set && !c->repl) { float nh = cell_h - c->mt - c->mb; if (nh > c->h) patch_h(c, nh); } }
        else if (a == AI_CENTER) dy += (cell_h - oh) / 2; else if (a == AI_END) dy += cell_h - oh;
        shift_box(c, g->s0, g->s1, 0, dy);
        if (c->fb > -1e8f && b->fb < -1e8f) b->fb = c->fb;
    }
    return rowy[nrow] - cy - gapr - rgap_add;
}
