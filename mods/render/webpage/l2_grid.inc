/* CSS Grid (базовый): колонки px/%/fr/auto/repeat/minmax, gap, span (включается в layout2.c) */
typedef struct { int kind; float v, mn; } GTrack;          /* kind: 0 px, 1 fr, 2 auto */

static int grid_tracks(const char* s, float cw, float gap, GTrack* out, int max) {
    int n = 0; const char* p = s;
    while (*p && n < max) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (!strncasecmp(p, "repeat(", 7)) {
            p += 7; int count = 0, autofit = 0;
            if (!strncasecmp(p, "auto-fit", 8)) { autofit = 1; p += 8; } else if (!strncasecmp(p, "auto-fill", 9)) { autofit = 1; p += 9; }
            else { count = atoi(p); while (isdigit((unsigned char)*p)) p++; }
            while (isspace((unsigned char)*p) || *p == ',') p++;
            const char* e = p; int d = 1; while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; if (d) e++; }
            char* inner = la((size_t)(e - p) + 1); memcpy(inner, p, (size_t)(e - p));
            GTrack tmp[8]; int m = grid_tracks(inner, cw, gap, tmp, 8);
            if (m == 0) { p = *e ? e + 1 : e; continue; }
            if (autofit) { float mw = tmp[0].kind == 0 ? tmp[0].v : tmp[0].mn; if (mw < 1) mw = 1; count = (int)((cw + gap) / (mw + gap)); if (count < 1) count = 1; if (count > 12) count = 12; }
            for (int c = 0; c < count && n < max; c++) for (int k = 0; k < m && n < max; k++) out[n++] = tmp[k];
            p = *e ? e + 1 : e; continue;
        }
        const char* e = p; int d = 0; while (*e && (d > 0 || !isspace((unsigned char)*e))) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
        char tok[96]; size_t tl = (size_t)(e - p); if (tl > 95) tl = 95; memcpy(tok, p, tl); tok[tl] = 0; p = e;
        GTrack t = {2, 0, 0}; Len l;
        if (!strncasecmp(tok, "minmax(", 7)) {
            char a[48] = "", bb[48] = ""; const char* c = strchr(tok, ','); if (c) { snprintf(a, sizeof a, "%.*s", (int)(c - tok - 7), tok + 7); snprintf(bb, sizeof bb, "%s", c + 1); char* rp = strrchr(bb, ')'); if (rp) *rp = 0; }
            Len la_; if (css_parse_len(a, &la_, 16, 16) && la_.set) t.mn = la_.px + la_.pct * cw / 100.0f;
            char* fe; float f = strtof(bb, &fe);
            if (fe != bb && !strncasecmp(fe, "fr", 2)) { t.kind = 1; t.v = f; }
            else if (css_parse_len(bb, &l, 16, 16) && l.set) { t.kind = 0; t.v = l.px + l.pct * cw / 100.0f; if (t.v < t.mn) t.v = t.mn; }
        } else {
            char* fe; float f = strtof(tok, &fe);
            if (fe != tok && !strncasecmp(fe, "fr", 2)) { t.kind = 1; t.v = f; }
            else if (!strncasecmp(tok, "auto", 4) || !strncasecmp(tok, "min-content", 11) || !strncasecmp(tok, "max-content", 11)) t.kind = 2;
            else if (css_parse_len(tok, &l, 16, 16) && l.set) { t.kind = 0; t.v = l.px + l.pct * cw / 100.0f + l.vw * VW + l.vh * VH; }
        }
        out[n++] = t;
    }
    return n;
}

/* Разбирает уже нормализованную grid-template-areas (строки через '\n', имена через пробел) на строки токенов */
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

static float layout_grid(Box* b, float cx, float cy, float cw, float ch, LS* ls) {
    (void)ch; const NodeStyle* st = b->st; int n = 0;
    for (Box* c = b->first; c; c = c->next) n++;
    Box** it = la(sizeof(Box*) * (size_t)(n ? n : 1)); int k = 0;
    for (Box* c = b->first; c; c = c->next) {
        if (c->oof) { register_abs(ls, c, cx, cy); continue; }
        if (c->kind == K_BR || c->kind == K_TEXT) continue;
        it[k++] = c;
    }
    if (!k) return 0;
    GTrack tr[24]; int nt = st->grid_cols[0] ? grid_tracks(st->grid_cols, cw, st->gap_c, tr, 24) : 0;
    if (nt == 0) { tr[0].kind = 1; tr[0].v = 1; tr[0].mn = 0; nt = 1; }
    int* col = la(sizeof(int) * (size_t)k * 3); int* row = col + k; int* span = row + k;

    static char atoks[16][24][24]; int anr = 0, anc = 0;
    if (st->grid_areas[0]) anr = grid_area_rows(st->grid_areas, atoks, &anc);
    int placed_by_area = 0;
    if (anr > 0) {
        placed_by_area = 1;
        for (int i = 0; i < k; i++) {
            const char* nm = it[i]->st->grid_area_name;
            int c0 = -1, c1 = -1, r0 = -1;
            if (nm[0]) for (int rr = 0; rr < anr && c0 < 0 || (rr < anr && r0 == rr); rr++) {
                for (int cc = 0; cc < anc; cc++) if (!strcmp(atoks[rr][cc], nm)) {
                    if (r0 < 0) r0 = rr;
                    if (r0 == rr) { if (c0 < 0 || cc < c0) c0 = cc; if (cc > c1) c1 = cc; }
                }
            }
            if (c0 < 0) { placed_by_area = 0; break; }                /* элемент не нашёлся в областях — откат на авто-раскладку */
            col[i] = c0 > nt - 1 ? nt - 1 : c0; span[i] = (c1 - c0 + 1); if (col[i] + span[i] > nt) span[i] = nt - col[i]; if (span[i] < 1) span[i] = 1;
            row[i] = r0;
        }
        if (placed_by_area) {                                          /* переномеровать строки подряд без пропусков (0,1,2,...) */
            int seen[16]; int ns = 0;
            for (int i = 0; i < k; i++) { int f = -1; for (int s2 = 0; s2 < ns; s2++) if (seen[s2] == row[i]) { f = s2; break; } if (f < 0) { seen[ns] = row[i]; f = ns++; } }
            for (int i = 0; i < k; i++) for (int s2 = 0; s2 < ns; s2++) if (seen[s2] == row[i]) { row[i] = s2; break; }
            /* сортировка по (row,col) — вставками, k обычно мало */
            for (int i = 1; i < k; i++) { Box* tb = it[i]; int tc = col[i], tr2 = row[i], ts = span[i]; int j = i - 1;
                while (j >= 0 && (row[j] > tr2 || (row[j] == tr2 && col[j] > tc))) { it[j + 1] = it[j]; col[j + 1] = col[j]; row[j + 1] = row[j]; span[j + 1] = span[j]; j--; }
                it[j + 1] = tb; col[j + 1] = tc; row[j + 1] = tr2; span[j + 1] = ts;
            }
        }
    }
    if (!placed_by_area) {
        int r = 0, c0 = 0;
        for (int i = 0; i < k; i++) {
            int sp = it[i]->st->grid_span > 0 ? it[i]->st->grid_span : 1; if (sp > nt) sp = nt;
            if (c0 + sp > nt) { r++; c0 = 0; }
            col[i] = c0; row[i] = r; span[i] = sp; c0 += sp; if (c0 >= nt) { r++; c0 = 0; }
        }
    }
    int nrows = row[k - 1] + 1;
    float colw[24], fixed = 0, frsum = 0, gapc = st->gap_c, gapr = st->gap_r;
    for (int t = 0; t < nt; t++) {
        colw[t] = 0;
        if (tr[t].kind == 0) { colw[t] = tr[t].v; fixed += colw[t]; }
        else if (tr[t].kind == 2) { for (int i = 0; i < k; i++) if (col[i] == t && span[i] == 1) { edges(it[i], cw); float w = intr(it[i], 1) + it[i]->ml + it[i]->mr; if (w > colw[t]) colw[t] = w; } fixed += colw[t]; }
        else frsum += tr[t].v;
    }
    float freeSp = cw - fixed - gapc * (nt - 1);
    for (int t = 0; t < nt; t++) if (tr[t].kind == 1) { float w = frsum > 0 ? freeSp * tr[t].v / frsum : 0; if (w < tr[t].mn) w = tr[t].mn; colw[t] = w < 0 ? 0 : w; }
    float colx[25]; colx[0] = cx; for (int t = 0; t < nt; t++) colx[t + 1] = colx[t] + colw[t] + gapc;
    float y = cy; int idx = 0;
    for (int rr = 0; rr < nrows; rr++) {
        int first = idx; float rowh = 0; int* s0 = la(sizeof(int) * 2 * (size_t)(k + 1));
        while (idx < k && row[idx] == rr) {
            Box* c = it[idx]; edges(c, cw);
            float w = colx[col[idx] + span[idx]] - colx[col[idx]] - gapc; if (span[idx] == 1) w = colw[col[idx]];
            c->mforce = 1; c->fml = c->mla ? 0 : c->ml; c->fmr = c->mra ? 0 : c->mr;
            float fw = w - c->fml - c->fmr; if (fw < 0) fw = 0;
            s0[(idx - first) * 2] = l2_dn; lbb(c, colx[col[idx]], y + c->mt, cw, c->st->width.set ? -1 : fw, -1, -1, ls); s0[(idx - first) * 2 + 1] = l2_dn; c->mforce = 0;
            float oh = c->mt + c->h + c->mb; if (oh > rowh) rowh = oh; idx++;
        }
        for (int i = first; i < idx; i++) {
            Box* c = it[i]; int a = c->st->align_self != AI_AUTO ? c->st->align_self : st->align_items; float oh = c->mt + c->h + c->mb, dy = 0;
            if (a == AI_STRETCH) { if (!c->st->height.set && !c->repl) { float nh = rowh - c->mt - c->mb; if (nh > c->h) patch_h(c, nh); } }
            else if (a == AI_CENTER) dy = (rowh - oh) / 2; else if (a == AI_END) dy = rowh - oh;
            if (dy > 0) shift_box(c, s0[(i - first) * 2], s0[(i - first) * 2 + 1], 0, dy);
            if (c->fb > -1e8f && b->fb < -1e8f) b->fb = c->fb;
        }
        y += rowh + gapr;
    }
    return y - cy - gapr;
}
