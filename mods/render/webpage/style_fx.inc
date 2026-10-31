/* Разбор значений: filter, backdrop-filter, clip-path, mask, transform(+origin), transition, animation, новые свойства grid.
   Включается в style.c (использует tok_next, len_of, col_of, Cx). */

static int fx_num(const char* s, float* out, int* pct) {              /* число или процент: «1.2», «120%» */
    char* e; float f = strtof(s, &e); if (e == s) return 0;
    if (*e == '%') { f /= 100.0f; if (pct) *pct = 1; e++; } else if (pct) *pct = 0;
    while (isspace((unsigned char)*e)) e++;
    if (*e) return 0;
    *out = f; return 1;
}
static int fx_angle(const char* s, float* deg) {                      /* 45deg, 0.5turn, 1rad, 100grad, 0 */
    char* e; float f = strtof(s, &e); if (e == s) return 0;
    if (!strncasecmp(e, "deg", 3)) *deg = f; else if (!strncasecmp(e, "turn", 4)) *deg = f * 360.0f;
    else if (!strncasecmp(e, "grad", 4)) *deg = f * 0.9f; else if (!strncasecmp(e, "rad", 3)) *deg = f * 57.2957795f;
    else if (!*e && f == 0) *deg = 0; else return 0;
    return 1;
}
static int fx_time(const char* s, float* ms) {                        /* 0.3s / 300ms */
    char* e; float f = strtof(s, &e); if (e == s) return 0;
    if (!strcasecmp(e, "ms")) *ms = f; else if (!strcasecmp(e, "s")) *ms = f * 1000.0f; else if (!*e && f == 0) *ms = 0; else return 0;
    return 1;
}
static FxLen fx_len_of(Cx* c, const char* tk, int* ok) {
    Len l; FxLen r = {0, 0}; *ok = 0;
    if (!strcasecmp(tk, "left") || !strcasecmp(tk, "top")) { *ok = 1; return r; }
    if (!strcasecmp(tk, "center")) { r.pct = 50; *ok = 1; return r; }
    if (!strcasecmp(tk, "right") || !strcasecmp(tk, "bottom")) { r.pct = 100; *ok = 1; return r; }
    if (len_of(c, tk, &l) && l.set) { r.px = l.px + (l.vw + l.svw + l.dvw) * SCREEN_WIDTH + (l.vh + l.svh + l.dvh) * SCREEN_HEIGHT + l.lh; r.pct = l.pct; *ok = 1; }
    else if (!strcmp(tk, "0")) *ok = 1;
    return r;
}

/* имя функции и её аргументы: «blur(5px)» → name="blur", args="5px" */
static int fx_func(const char* tk, char* name, size_t nc, char* args, size_t ac) {
    const char* p = strchr(tk, '('); if (!p) return 0;
    size_t nl = (size_t)(p - tk); if (nl >= nc) nl = nc - 1; memcpy(name, tk, nl); name[nl] = 0;
    const char* e = tk + strlen(tk); while (e > p && e[-1] != ')') e--;
    size_t al = e > p + 1 ? (size_t)(e - 1 - (p + 1)) : 0; if (al >= ac) al = ac - 1; memcpy(args, p + 1, al); args[al] = 0;
    return 1;
}

static void parse_filter_list(Cx* c, FxList* out, const char* v) {
    out->n = 0; const char* p = v; char tk[256];
    if (!strncasecmp(v, "none", 4)) return;
    while (tok_next(&p, tk, sizeof tk) && out->n < 6) {
        char nm[32], ar[220]; if (!fx_func(tk, nm, sizeof nm, ar, sizeof ar)) continue;
        FxOp op; memset(&op, 0, sizeof op); float f; Len l; int pc;
        const char* a = ar; while (isspace((unsigned char)*a)) a++;
        if (!strcasecmp(nm, "blur")) { op.kind = FXO_BLUR; op.v = (len_of(c, a, &l) && l.set) ? l.px : 0; }
        else if (!strcasecmp(nm, "hue-rotate")) { op.kind = FXO_HUE; float d = 0; fx_angle(a, &d); op.v = d; }
        else if (!strcasecmp(nm, "drop-shadow")) {
            op.kind = FXO_SHADOW; op.color = c->cur; const char* q = a; char t2[96]; int nn = 0; float nums[3] = {0, 0, 0};
            while (tok_next(&q, t2, sizeof t2)) { uint32_t cc; if ((isdigit((unsigned char)t2[0]) || t2[0] == '-' || t2[0] == '.') && len_of(c, t2, &l)) { if (nn < 3) nums[nn++] = l.px; } else if (col_of(c, t2, &cc)) op.color = cc; }
            op.x = nums[0]; op.y = nums[1]; op.blur = nums[2] < 0 ? 0 : nums[2];
        } else {
            int k = !strcasecmp(nm, "brightness") ? FXO_BRIGHT : !strcasecmp(nm, "contrast") ? FXO_CONTRAST : !strcasecmp(nm, "grayscale") ? FXO_GRAY :
                    !strcasecmp(nm, "sepia") ? FXO_SEPIA : !strcasecmp(nm, "saturate") ? FXO_SAT : !strcasecmp(nm, "invert") ? FXO_INVERT : !strcasecmp(nm, "opacity") ? FXO_OPACITY : 0;
            if (!k) continue; op.kind = (uint8_t)k;
            op.v = (*a && fx_num(a, &f, &pc)) ? f : 1.0f; if (!*a && (k == FXO_GRAY || k == FXO_SEPIA || k == FXO_INVERT)) op.v = 1.0f;
            if (op.v < 0) op.v = 0;
        }
        out->op[out->n++] = op;
    }
}

/* ── clip-path ── */
static void fx_pos_pair(Cx* c, const char* s, FxLen* x, FxLen* y) {      /* «at 30% 40%» → x,y; по умолчанию центр */
    char tk[64]; const char* p = s; int k = 0; FxLen t[2] = {{0, 50}, {0, 50}}; int ok; int vert_first = 0;
    while (tok_next(&p, tk, sizeof tk) && k < 2) {
        if (!strcasecmp(tk, "at")) continue;
        if (k == 0 && (!strcasecmp(tk, "top") || !strcasecmp(tk, "bottom"))) vert_first = 1;
        FxLen l = fx_len_of(c, tk, &ok); if (ok) t[k++] = l;
    }
    if (k == 1) { if (vert_first) { t[1] = t[0]; t[0].px = 0; t[0].pct = 50; } else { t[1].px = 0; t[1].pct = 50; } }
    else if (k == 2 && vert_first) { FxLen s2 = t[0]; t[0] = t[1]; t[1] = s2; }
    *x = t[0]; *y = t[1];
}
static void parse_clip_path(Cx* c, FxClip* cp, const char* v) {
    memset(cp, 0, sizeof *cp);
    char nm[32], ar[900]; const char* p = v; while (isspace((unsigned char)*p)) p++;
    if (!fx_func(p, nm, sizeof nm, ar, sizeof ar)) return;
    if (!strcasecmp(nm, "inset")) {
        cp->kind = CP_INSET; char* rd = strcasestr(ar, "round"); char head[200]; size_t hl = rd ? (size_t)(rd - ar) : strlen(ar); if (hl > 199) hl = 199; memcpy(head, ar, hl); head[hl] = 0;
        FxLen t[4]; int k = 0, ok; char tk[64]; const char* q = head; while (k < 4 && tok_next(&q, tk, sizeof tk)) { FxLen l = fx_len_of(c, tk, &ok); if (ok) t[k++] = l; }
        if (!k) memset(t, 0, sizeof t), k = 1;
        FxLen o[4]; if (k == 1) o[0] = o[1] = o[2] = o[3] = t[0]; else if (k == 2) { o[0] = o[2] = t[0]; o[1] = o[3] = t[1]; } else if (k == 3) { o[0] = t[0]; o[1] = o[3] = t[1]; o[2] = t[2]; } else memcpy(o, t, sizeof o);
        memcpy(cp->a, o, sizeof o);
        if (rd) { float r[4]; int rk = 0; q = rd + 5; Len l; while (rk < 4 && tok_next(&q, tk, sizeof tk)) { if (len_of(c, tk, &l) && l.set) r[rk++] = l.px; } if (rk) { if (rk == 1) r[1] = r[2] = r[3] = r[0]; else if (rk == 2) { r[2] = r[0]; r[3] = r[1]; } else if (rk == 3) r[3] = r[1]; memcpy(cp->rad, r, sizeof cp->rad); } }
    } else if (!strcasecmp(nm, "circle") || !strcasecmp(nm, "ellipse")) {
        int ell = !strcasecmp(nm, "ellipse"); cp->kind = ell ? CP_ELLIPSE : CP_CIRCLE;
        char* at = strcasestr(ar, "at "); char head[160]; size_t hl = at ? (size_t)(at - ar) : strlen(ar); if (hl > 159) hl = 159; memcpy(head, ar, hl); head[hl] = 0;
        char tk[64]; const char* q = head; int k = 0, ok; FxLen r[2] = {{-1, 0}, {-1, 0}};
        while (k < (ell ? 2 : 1) && tok_next(&q, tk, sizeof tk)) {
            if (!strcasecmp(tk, "closest-side")) { r[k].px = -1; k++; } else if (!strcasecmp(tk, "farthest-side")) { r[k].px = -2; k++; }
            else { FxLen l = fx_len_of(c, tk, &ok); if (ok) r[k++] = l; }
        }
        if (ell) { cp->a[0] = r[0]; cp->a[1] = k > 1 ? r[1] : r[0]; fx_pos_pair(c, at ? at + 3 : "", &cp->a[2], &cp->a[3]); }
        else { cp->a[0] = r[0]; fx_pos_pair(c, at ? at + 3 : "", &cp->a[1], &cp->a[2]); }
    } else if (!strcasecmp(nm, "polygon")) {
        cp->kind = CP_POLYGON; const char* q = ar; while (isspace((unsigned char)*q)) q++;
        if (!strncasecmp(q, "evenodd", 7)) { cp->evenodd = 1; q += 7; while (*q && *q != ',') q++; if (*q == ',') q++; } else if (!strncasecmp(q, "nonzero", 7)) { q += 7; while (*q && *q != ',') q++; if (*q == ',') q++; }
        while (*q && cp->n < 16) {
            const char* e = q; int d = 0; while (*e && !(d == 0 && *e == ',')) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
            char pt[96]; size_t pl = (size_t)(e - q); if (pl > 95) pl = 95; memcpy(pt, q, pl); pt[pl] = 0;
            char tk[48]; const char* z = pt; int ok1 = 0, ok2 = 0; FxLen x = {0, 0}, y = {0, 0};
            if (tok_next(&z, tk, sizeof tk)) x = fx_len_of(c, tk, &ok1);
            if (tok_next(&z, tk, sizeof tk)) y = fx_len_of(c, tk, &ok2);
            if (ok1 && ok2) { cp->pt[cp->n][0] = x; cp->pt[cp->n][1] = y; cp->n++; }
            q = *e ? e + 1 : e;
        }
        if (cp->n < 3) memset(cp, 0, sizeof *cp);
    }
}

/* ── mask ── */
static void parse_mask_image(Cx* c, FxMask* m, const char* tk) {
    int lin = !strncasecmp(tk, "linear-gradient(", 16) || !strncasecmp(tk, "-webkit-linear-gradient(", 24) || !strncasecmp(tk, "repeating-linear-gradient(", 26);
    int rad = !strncasecmp(tk, "radial-gradient(", 16) || !strncasecmp(tk, "-webkit-radial-gradient(", 24) || !strncasecmp(tk, "repeating-radial-gradient(", 26);
    if (!strncasecmp(tk, "url(", 4)) {
        const char* q = tk + 4; while (isspace((unsigned char)*q)) q++; char qc = (*q == '"' || *q == '\'') ? *q++ : 0; const char* qe = q;
        while (*qe && (qc ? *qe != qc : *qe != ')')) qe++;
        m->url[0] = 0;
        if (qe - q > 5 && !strncasecmp(q, "data:", 5)) {
            char dp[200];
            if (!webpage_data_uri_to_file(q, (size_t)(qe - q), dp, sizeof dp) || strlen(dp) >= sizeof m->url) dp[0] = 0;
            memcpy(m->url, dp, strlen(dp) + 1);
        } else {
            size_t url_len = (size_t)(qe - q);
            if (url_len >= sizeof m->url) m->url[0] = 0;
            else { memcpy(m->url, q, url_len); m->url[url_len] = 0; }
        }
        if (m->url[0]) {
            if (strncmp(m->url, "cache/data_", 11) != 0) {
                char ab[8192]; webpage_resolve_url(g_base, m->url, ab, sizeof ab);
                if (!ab[0] || strlen(ab) >= sizeof m->url) m->url[0] = 0;
                else memcpy(m->url, ab, strlen(ab) + 1);
            }
            if (m->url[0]) m->kind = MK_IMAGE;
        }
        return;
    }
    if (!lin && !rad) return;
    const char* q = strchr(tk, '('); if (!q) return; q++;
    char a[160]; size_t k = 0; int d = 0, n = 0, first = 1; float angle = 180.0f;
    for (;; q++) {
        int end = 0;
        if (*q == '(') d++; else if (*q == ')') { if (d == 0) end = 1; else d--; }
        if ((*q == ',' && d == 0) || !*q || end) {
            a[k] = 0; char* sp = a; while (isspace((unsigned char)*sp)) sp++;
            size_t al = strlen(sp); while (al > 0 && isspace((unsigned char)sp[al - 1])) sp[--al] = 0;
            int consumed = 0;
            if (first) {
                first = 0;
                if (lin && (strcasestr(sp, "deg") || strcasestr(sp, "turn") || strcasestr(sp, "rad") || !strncasecmp(sp, "to ", 3))) {
                    float dg;
                    if (!strncasecmp(sp, "to ", 3)) { int rt = strcasestr(sp, "right") != NULL, lf = strcasestr(sp, "left") != NULL, tp = strcasestr(sp, "top") != NULL, bt = strcasestr(sp, "bottom") != NULL;
                        angle = (rt && tp) ? 45.0f : (rt && bt) ? 135.0f : (lf && bt) ? 225.0f : (lf && tp) ? 315.0f : rt ? 90.0f : lf ? 270.0f : tp ? 0.0f : 180.0f; }
                    else if (fx_angle(sp, &dg)) angle = dg;
                    consumed = 1;
                } else if (rad && (strcasestr(sp, "circle") || strcasestr(sp, "ellipse") || strcasestr(sp, "at ") || strcasestr(sp, "closest") || strcasestr(sp, "farthest"))) consumed = 1;
            }
            if (!consumed && *sp) {
                char cs[96] = {0}; float pos = -1; char* ce = sp; int dd = 0;
                while (*ce && !(dd == 0 && isspace((unsigned char)*ce))) { if (*ce == '(') dd++; if (*ce == ')') dd--; ce++; }
                size_t cl = (size_t)(ce - sp); if (cl > 95) cl = 95; memcpy(cs, sp, cl); cs[cl] = 0;
                while (isspace((unsigned char)*ce)) ce++; if (*ce && strchr(ce, '%')) pos = strtof(ce, NULL);
                uint32_t col; if (n < 6 && (!strcasecmp(cs, "transparent") ? (col = 0, 1) : css_parse_color(cs, &col, c->cur))) { m->a[n] = (uint8_t)(col >> 24); m->p[n] = pos; n++; }
            }
            k = 0; if (!*q || end) break; continue;
        }
        if (k < sizeof(a) - 1) a[k++] = *q;
    }
    if (n >= 1) {
        if (n == 1) { m->a[1] = m->a[0]; m->p[1] = 100; if (m->p[0] < 0) m->p[0] = 0; n = 2; }
        if (m->p[0] < 0) m->p[0] = 0; if (m->p[n - 1] < 0) m->p[n - 1] = 100;
        for (int i = 1; i < n - 1; i++) if (m->p[i] < 0) { int j = i; while (j < n - 1 && m->p[j] < 0) j++; for (int z = i; z < j; z++) m->p[z] = m->p[i - 1] + (m->p[j] - m->p[i - 1]) * (z - i + 1) / (j - i + 1); }
        for (int i = 1; i < n; i++) if (m->p[i] < m->p[i - 1]) m->p[i] = m->p[i - 1];
        m->kind = rad ? MK_RADIAL : MK_LINEAR; m->n = (uint8_t)n; m->angle = angle;
    }
}
static void parse_mask_pos_size(Cx* c, FxMask* m, const char* v, int size_part) {   /* позиция / размер */
    char tk[96]; const char* p = v; int posn = 0; Len l;
    if (size_part) {
        if (!strncasecmp(v, "cover", 5)) { m->size_mode = 1; return; } if (!strncasecmp(v, "contain", 7)) { m->size_mode = 2; return; }
        if (!strncasecmp(v, "auto", 4) && !strchr(v + 4, ' ') ) { m->size_mode = 0; memset(&m->w, 0, sizeof(Len) * 2); return; }
        Len t[2]; int k = 0; memset(t, 0, sizeof t);
        while (k < 2 && tok_next(&p, tk, sizeof tk)) { if (!strcasecmp(tk, "auto")) { t[k].set = 0; k++; } else if (len_of(c, tk, &l) && l.set) t[k++] = l; else break; }
        if (k) { m->w = t[0]; m->h = k > 1 ? t[1] : t[0]; if (k == 1) m->h.set = 0; m->size_mode = (m->w.set || m->h.set) ? 3 : 0; }
        return;
    }
    while (tok_next(&p, tk, sizeof tk) && posn < 2) {
        Len* dst = posn == 0 ? &m->px : &m->py;
        if (!strcasecmp(tk, "center")) { memset(&l, 0, sizeof l); l.pct = 50; l.set = 1; }
        else if (!strcasecmp(tk, "left") || !strcasecmp(tk, "top")) { memset(&l, 0, sizeof l); l.set = 1; if (!strcasecmp(tk, "top") && posn == 0) dst = &m->py; }
        else if (!strcasecmp(tk, "right") || !strcasecmp(tk, "bottom")) { memset(&l, 0, sizeof l); l.pct = 100; l.set = 1; if (!strcasecmp(tk, "bottom") && posn == 0) dst = &m->py; }
        else if (!len_of(c, tk, &l) || !l.set) continue;
        *dst = l; posn++;
    }
}
static void parse_mask_repeat(FxMask* m, const char* v) {
    if (!strncasecmp(v, "no-repeat", 9)) m->rep = 3; else if (!strncasecmp(v, "repeat-x", 8)) m->rep = 1; else if (!strncasecmp(v, "repeat-y", 8)) m->rep = 2; else m->rep = 0;
}
static void parse_mask_short(Cx* c, FxMask* m, const char* v) {
    FxMask keep; memset(&keep, 0, sizeof keep); *m = keep;
    if (!strncasecmp(v, "none", 4)) return;
    char tk[16384]; const char* p = v; int in_size = 0, posn = 0;
    while (tok_next(&p, tk, sizeof tk)) {
        if (!strncasecmp(tk, "url(", 4) || strstr(tk, "gradient(")) parse_mask_image(c, m, tk);
        else if (!strcasecmp(tk, "no-repeat") || !strcasecmp(tk, "repeat-x") || !strcasecmp(tk, "repeat-y") || !strcasecmp(tk, "repeat") || !strcasecmp(tk, "space") || !strcasecmp(tk, "round")) parse_mask_repeat(m, tk);
        else if (!strcmp(tk, "/")) in_size = 1;
        else if (!strcasecmp(tk, "cover")) m->size_mode = 1; else if (!strcasecmp(tk, "contain")) m->size_mode = 2;
        else if (!strcasecmp(tk, "alpha") || !strcasecmp(tk, "luminance") || !strcasecmp(tk, "match-source") || !strcasecmp(tk, "add") || !strcasecmp(tk, "subtract") || !strcasecmp(tk, "intersect") || !strcasecmp(tk, "exclude") ||
                 !strcasecmp(tk, "border-box") || !strcasecmp(tk, "padding-box") || !strcasecmp(tk, "content-box") || !strcasecmp(tk, "no-clip") || !strcasecmp(tk, "fill-box") || !strcasecmp(tk, "stroke-box") || !strcasecmp(tk, "view-box")) {}
        else if (in_size) { Len l; if (len_of(c, tk, &l) && l.set) { if (posn == 0) m->w = l; else m->h = l; posn++; m->size_mode = 3; } else if (!strcasecmp(tk, "auto")) posn++; }
        else parse_mask_pos_size(c, m, tk, 0);
    }
}

/* ── transform ── */
static void parse_xform(Cx* c, FxXform* x, const char* v) {
    x->n = 0; if (!strncasecmp(v, "none", 4)) return;
    const char* p = v; char tk[512];
    while (tok_next(&p, tk, sizeof tk) && x->n < 6) {
        char nm[32], ar[480]; if (!fx_func(tk, nm, sizeof nm, ar, sizeof ar)) continue;
        FxXfOp op; memset(&op, 0, sizeof op); char a[4][96]; int na = 0; { char* s = ar; int d = 0; size_t k = 0; for (;; s++) { if (*s == '(') d++; else if (*s == ')') d--; if ((*s == ',' && d == 0) || !*s) { a[na][k] = 0; if (na < 3) na++; k = 0; if (!*s) break; continue; } if (k < 95 && !(k == 0 && isspace((unsigned char)*s))) a[na][k++] = *s; } }
        int ok; float f, dg;
        if (!strcasecmp(nm, "translate") || !strcasecmp(nm, "translate3d")) { op.kind = XF_TRANSLATE; op.t[0] = fx_len_of(c, a[0], &ok); if (na > 1) op.t[1] = fx_len_of(c, a[1], &ok); }
        else if (!strcasecmp(nm, "translatex")) { op.kind = XF_TRANSLATE; op.t[0] = fx_len_of(c, a[0], &ok); }
        else if (!strcasecmp(nm, "translatey")) { op.kind = XF_TRANSLATE; op.t[1] = fx_len_of(c, a[0], &ok); }
        else if (!strcasecmp(nm, "scale") || !strcasecmp(nm, "scale3d")) { op.kind = XF_SCALE; op.v[0] = fx_num(a[0], &f, NULL) ? f : 1; op.v[1] = na > 1 && fx_num(a[1], &f, NULL) ? f : op.v[0]; }
        else if (!strcasecmp(nm, "scalex")) { op.kind = XF_SCALE; op.v[0] = fx_num(a[0], &f, NULL) ? f : 1; op.v[1] = 1; }
        else if (!strcasecmp(nm, "scaley")) { op.kind = XF_SCALE; op.v[0] = 1; op.v[1] = fx_num(a[0], &f, NULL) ? f : 1; }
        else if (!strcasecmp(nm, "rotate") || !strcasecmp(nm, "rotatez")) { op.kind = XF_ROTATE; op.v[0] = fx_angle(a[0], &dg) ? dg : 0; }
        else if (!strcasecmp(nm, "rotatex")) { op.kind = XF_SCALE; op.v[0] = 1; op.v[1] = fx_angle(a[0], &dg) ? cosf(dg * 0.0174532925f) : 1; }
        else if (!strcasecmp(nm, "rotatey")) { op.kind = XF_SCALE; op.v[1] = 1; op.v[0] = fx_angle(a[0], &dg) ? cosf(dg * 0.0174532925f) : 1; }
        else if (!strcasecmp(nm, "skewx")) { op.kind = XF_SKEWX; op.v[0] = fx_angle(a[0], &dg) ? dg : 0; }
        else if (!strcasecmp(nm, "skewy")) { op.kind = XF_SKEWY; op.v[0] = fx_angle(a[0], &dg) ? dg : 0; }
        else if (!strcasecmp(nm, "skew")) { op.kind = XF_SKEWX; op.v[0] = fx_angle(a[0], &dg) ? dg : 0; x->op[x->n++] = op; if (na > 1 && fx_angle(a[1], &dg) && x->n < 6) { memset(&op, 0, sizeof op); op.kind = XF_SKEWY; op.v[0] = dg; } else continue; }
        else if (!strcasecmp(nm, "matrix")) { op.kind = XF_MATRIX; char* s = ar; for (int i = 0; i < 6; i++) { op.v[i] = strtof(s, &s); while (*s == ',' || isspace((unsigned char)*s)) s++; } }
        else if (!strcasecmp(nm, "matrix3d")) { float m16[16]; char* s = ar; for (int i = 0; i < 16; i++) { m16[i] = strtof(s, &s); while (*s == ',' || isspace((unsigned char)*s)) s++; } op.kind = XF_MATRIX; op.v[0] = m16[0]; op.v[1] = m16[1]; op.v[2] = m16[4]; op.v[3] = m16[5]; op.v[4] = m16[12]; op.v[5] = m16[13]; }
        else continue;
        x->op[x->n++] = op;
    }
}
static void parse_xform_origin(Cx* c, FxXform* x, const char* v) {
    FxLen ox = {0, 50}, oy = {0, 50}; fx_pos_pair(c, v, &ox, &oy); x->ox = ox; x->oy = oy; x->has_origin = 1;
}

/* ── transition / animation ── */
static void ease_default(FxEase* e) { e->kind = 0; e->a = 0.25f; e->b = 0.1f; e->c = 0.25f; e->d = 1.0f; }
static int parse_ease(const char* tk, FxEase* e) {
    if (!strcasecmp(tk, "ease")) { ease_default(e); return 1; }
    if (!strcasecmp(tk, "linear")) { e->kind = 2; return 1; }
    if (!strcasecmp(tk, "ease-in")) { e->kind = 0; e->a = 0.42f; e->b = 0; e->c = 1; e->d = 1; return 1; }
    if (!strcasecmp(tk, "ease-out")) { e->kind = 0; e->a = 0; e->b = 0; e->c = 0.58f; e->d = 1; return 1; }
    if (!strcasecmp(tk, "ease-in-out")) { e->kind = 0; e->a = 0.42f; e->b = 0; e->c = 0.58f; e->d = 1; return 1; }
    if (!strcasecmp(tk, "step-start")) { e->kind = 1; e->a = 1; e->b = 1; return 1; }
    if (!strcasecmp(tk, "step-end")) { e->kind = 1; e->a = 1; e->b = 0; return 1; }
    if (!strncasecmp(tk, "cubic-bezier(", 13)) { float v[4] = {0, 0, 1, 1}; const char* s = tk + 13; for (int i = 0; i < 4; i++) { char* en; v[i] = strtof(s, &en); s = en; while (*s == ',' || isspace((unsigned char)*s)) s++; } e->kind = 0; e->a = v[0]; e->b = v[1]; e->c = v[2]; e->d = v[3]; if (e->a < 0) e->a = 0; if (e->a > 1) e->a = 1; if (e->c < 0) e->c = 0; if (e->c > 1) e->c = 1; return 1; }
    if (!strncasecmp(tk, "steps(", 6)) { const char* s = tk + 6; e->kind = 1; e->a = (float)atoi(s); if (e->a < 1) e->a = 1; e->b = (strcasestr(s, "start") || strcasestr(s, "jump-start") || strcasestr(s, "jump-both")) ? 1.0f : 0.0f; return 1; }
    return 0;
}
/* делит список по запятым верхнего уровня */
static int split_commas(const char* v, char out[][256], int max) {
    int n = 0; const char* p = v;
    while (*p && n < max) {
        const char* e = p; int d = 0; while (*e && !(d == 0 && *e == ',')) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
        size_t l = (size_t)(e - p); if (l > 255) l = 255; memcpy(out[n], p, l); out[n][l] = 0; n++;
        p = *e ? e + 1 : e;
    }
    return n;
}
static void trans_rebuild(NodeStyle* st) {
    int n = st->tp_prop_n ? st->tp_prop_n : 1; if (n > 4) n = 4; st->tr_n = 0;
    for (int k = 0; k < n; k++) {
        FxTrans* t = &st->tr[k]; memset(t, 0, sizeof *t);
        t->mask = st->tp_prop_n ? st->tp_prop[k] : AP_ALL_MASK;
        t->dur = st->tp_dur_n ? st->tp_dur[k % st->tp_dur_n] : 0; t->delay = st->tp_del_n ? st->tp_del[k % st->tp_del_n] : 0;
        if (st->tp_ease_n) t->ease = st->tp_ease[k % st->tp_ease_n]; else ease_default(&t->ease);
    }
    st->tr_n = (uint8_t)n;
}
static void parse_transition_list(Cx* c, NodeStyle* st, int which, const char* v) {   /* which: 0 property, 1 duration, 2 delay, 3 timing */
    (void)c; char part[4][256]; int n = split_commas(v, part, 4);
    for (int i = 0; i < n; i++) {
        char* s = part[i]; while (isspace((unsigned char)*s)) s++; size_t l = strlen(s); while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
        if (which == 0) { st->tp_prop[i] = !strcasecmp(s, "none") ? 0 : fx_prop_mask(s, strlen(s)); }
        else if (which == 1) { float ms = 0; fx_time(s, &ms); st->tp_dur[i] = ms < 0 ? 0 : ms; }
        else if (which == 2) { float ms = 0; fx_time(s, &ms); st->tp_del[i] = ms; }
        else { FxEase e; ease_default(&e); parse_ease(s, &e); st->tp_ease[i] = e; }
    }
    if (which == 0) st->tp_prop_n = (uint8_t)n; else if (which == 1) st->tp_dur_n = (uint8_t)n; else if (which == 2) st->tp_del_n = (uint8_t)n; else st->tp_ease_n = (uint8_t)n;
    trans_rebuild(st);
}
static void parse_transition_short(Cx* c, NodeStyle* st, const char* v) {
    (void)c; char part[4][256]; int n = split_commas(v, part, 4);
    st->tp_prop_n = st->tp_dur_n = st->tp_del_n = st->tp_ease_n = 0;
    if (!strncasecmp(v, "none", 4)) { st->tp_prop[0] = 0; st->tp_prop_n = 1; st->tp_dur[0] = 0; st->tp_dur_n = 1; trans_rebuild(st); return; }
    for (int i = 0; i < n; i++) {
        char tk[128]; const char* p = part[i]; int tn = 0; uint64_t pm = AP_ALL_MASK; FxEase e; ease_default(&e); float dur = 0, del = 0, ms;
        int token_status;
        while ((token_status = tok_next_exact(&p, tk, sizeof tk)) != 0) {
            if (token_status < 0) continue;
            if (fx_time(tk, &ms)) { if (tn == 0) dur = ms; else del = ms; tn++; }
            else if (parse_ease(tk, &e)) {}
            else pm = !strcasecmp(tk, "none") ? 0 : fx_prop_mask(tk, strlen(tk));
        }
        st->tp_prop[i] = pm; st->tp_dur[i] = dur < 0 ? 0 : dur; st->tp_del[i] = del; st->tp_ease[i] = e;
    }
    st->tp_prop_n = st->tp_dur_n = st->tp_del_n = st->tp_ease_n = (uint8_t)n;
    trans_rebuild(st);
}

static void anim_entry_init(FxAnim* a) { memset(a, 0, sizeof *a); a->iter = 1; ease_default(&a->ease); }
static void parse_animation_list(Cx* c, NodeStyle* st, int prop, const char* v) {
    (void)c; char part[3][256]; int n = split_commas(v, part, 3); if (n > st->an_n) { for (int i = st->an_n; i < n; i++) anim_entry_init(&st->an[i]); st->an_n = (uint8_t)n; }
    for (int k = 0; k < st->an_n; k++) {
        char* s = part[k % n]; while (isspace((unsigned char)*s)) s++; size_t l = strlen(s); while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
        FxAnim* a = &st->an[k]; float ms;
        switch (prop) {
        case P_ANIMATION_NAME: if (!strcasecmp(s, "none")) a->name[0] = 0; else snprintf(a->name, sizeof a->name, "%s", s); break;
        case P_ANIMATION_DURATION: if (fx_time(s, &ms)) a->dur = ms < 0 ? 0 : ms; break;
        case P_ANIMATION_DELAY: if (fx_time(s, &ms)) a->delay = ms; break;
        case P_ANIMATION_TIMING: parse_ease(s, &a->ease); break;
        case P_ANIMATION_ITER: if (!strcasecmp(s, "infinite")) a->iter = -1; else { float f = strtof(s, NULL); a->iter = f < 0 ? 1 : f; } break;
        case P_ANIMATION_DIR: a->dir = !strcasecmp(s, "reverse") ? 1 : !strcasecmp(s, "alternate") ? 2 : !strcasecmp(s, "alternate-reverse") ? 3 : 0; break;
        case P_ANIMATION_FILL: a->fill = !strcasecmp(s, "forwards") ? 1 : !strcasecmp(s, "backwards") ? 2 : !strcasecmp(s, "both") ? 3 : 0; break;
        case P_ANIMATION_PLAY: a->paused = !strcasecmp(s, "paused"); break;
        case P_ANIMATION_COMPOSITION:
            a->composition = !strcasecmp(s, "add") ? 1 : !strcasecmp(s, "accumulate") ? 2 : 0;
            break;
        default: break;
        }
    }
}
static void parse_animation_short(Cx* c, NodeStyle* st, const char* v) {
    (void)c; char part[3][256]; int n = split_commas(v, part, 3); st->an_n = 0;
    for (int i = 0; i < n; i++) {
        FxAnim* a = &st->an[i]; anim_entry_init(a); char tk[128]; const char* p = part[i]; int tn = 0; float ms; FxEase e; int named = 0;
        while (tok_next(&p, tk, sizeof tk)) {
            if (fx_time(tk, &ms)) { if (tn == 0) a->dur = ms < 0 ? 0 : ms; else a->delay = ms; tn++; }
            else if (!strcasecmp(tk, "infinite")) a->iter = -1;
            else if (!strcasecmp(tk, "normal")) a->dir = 0; else if (!strcasecmp(tk, "reverse")) a->dir = 1; else if (!strcasecmp(tk, "alternate")) a->dir = 2; else if (!strcasecmp(tk, "alternate-reverse")) a->dir = 3;
            else if (!strcasecmp(tk, "forwards")) a->fill = 1; else if (!strcasecmp(tk, "backwards")) a->fill = 2; else if (!strcasecmp(tk, "both")) a->fill = 3;
            else if (!strcasecmp(tk, "running")) a->paused = 0; else if (!strcasecmp(tk, "paused")) a->paused = 1;
            else if (parse_ease(tk, &e)) a->ease = e;
            else if ((isdigit((unsigned char)tk[0]) || tk[0] == '.') && strtof(tk, NULL) >= 0) a->iter = strtof(tk, NULL);
            else if (!strcasecmp(tk, "none")) {}
            else if (!named) {
                /* Never alias a long animation name to a shorter keyframe name. */
                if (strlen(tk) < sizeof a->name) { memcpy(a->name, tk, strlen(tk) + 1); named = 1; }
            }
        }
    }
    st->an_n = (uint8_t)n;
}

/* ── grid: новые свойства ── */
static void copy_trim(char* dst, size_t cap, const char* v) {
    while (isspace((unsigned char)*v)) v++; size_t l = strlen(v); while (l && isspace((unsigned char)v[l - 1])) l--;
    if (l >= cap) l = cap - 1; memcpy(dst, v, l); dst[l] = 0;
}
static int ai_of(const char* val, int dflt) {
    if (!strcasecmp(val, "start") || !strcasecmp(val, "flex-start") || !strcasecmp(val, "self-start") || !strcasecmp(val, "left") || !strcasecmp(val, "baseline") || !strcasecmp(val, "first baseline")) return AI_START;
    if (!strcasecmp(val, "end") || !strcasecmp(val, "flex-end") || !strcasecmp(val, "self-end") || !strcasecmp(val, "right") || !strcasecmp(val, "last baseline")) return AI_END;
    if (!strcasecmp(val, "center") || !strcasecmp(val, "safe center")) return AI_CENTER;
    if (!strcasecmp(val, "stretch") || !strcasecmp(val, "normal")) return AI_STRETCH;
    if (!strcasecmp(val, "auto")) return AI_AUTO;
    return dflt;
}
static int jc_of(const char* val) {
    return (!strcasecmp(val, "flex-end") || !strcasecmp(val, "end") || !strcasecmp(val, "right")) ? JC_END : !strcasecmp(val, "center") ? JC_CENTER : !strcasecmp(val, "space-between") ? JC_BETWEEN :
           !strcasecmp(val, "space-around") ? JC_AROUND : !strcasecmp(val, "space-evenly") ? JC_EVENLY : JC_START;
}
static void grid_line_pair(const char* val, char* s, size_t sc, char* e, size_t ec) {         /* «a / b» */
    const char* sl = strchr(val, '/');
    if (sl) { char t[64]; size_t l = (size_t)(sl - val); if (l > 63) l = 63; memcpy(t, val, l); t[l] = 0; copy_trim(s, sc, t); copy_trim(e, ec, sl + 1); }
    else { copy_trim(s, sc, val); e[0] = 0; }
}

static int split_slash4(const char* v, char out[4][256]) {
    int n = 0; const char* p = v;
    while (*p && n < 4) { const char* e = p; while (*e && *e != '/') e++; size_t l = (size_t)(e - p); if (l > 255) l = 255; memcpy(out[n], p, l); out[n][l] = 0; n++; p = *e ? e + 1 : e; }
    return n;
}
