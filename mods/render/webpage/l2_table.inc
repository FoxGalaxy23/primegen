/* таблицы (включается в layout2.c) */
#define T_MAXCOL 64
#define T_MAXROW 3000
typedef struct { Box* c; int r, col, rs, cs; float mn, mx; } TCell;
typedef struct {
    Box* rows[T_MAXROW]; int nr; Box* caption;
    TCell* cells; int nc; int ncol;
    float colmin[T_MAXCOL], colmax[T_MAXCOL];
} TGrid;

static void table_collect(Box* t, TGrid* g) {
    for (Box* c = t->first; c; c = c->next) {
        if (c->kind != K_BLOCK || c->oof || c->flt) continue;
        int d = c->st->display;
        if (d == DSP_CAPTION) g->caption = c;
        else if (d == DSP_ROW_GROUP) { for (Box* r = c->first; r; r = r->next) if (r->kind == K_BLOCK && r->st->display == DSP_ROW && g->nr < T_MAXROW) g->rows[g->nr++] = r; }
        else if (d == DSP_ROW && g->nr < T_MAXROW) g->rows[g->nr++] = c;
    }
}

static TGrid* table_grid(Box* t) {
    TGrid* g = la(sizeof(TGrid)); table_collect(t, g);
    int ncells = 0; for (int r = 0; r < g->nr; r++) for (Box* c = g->rows[r]->first; c; c = c->next) if (c->kind == K_BLOCK && !c->oof && !c->flt) ncells++;
    g->cells = la(sizeof(TCell) * (size_t)(ncells ? ncells : 1));
    unsigned char* occ = la((size_t)(g->nr ? g->nr : 1) * T_MAXCOL);
    for (int r = 0; r < g->nr; r++) {
        int col = 0;
        for (Box* c = g->rows[r]->first; c; c = c->next) {
            if (c->kind != K_BLOCK || c->oof || c->flt) continue;
            while (col < T_MAXCOL && occ[r * T_MAXCOL + col]) col++;
            if (col >= T_MAXCOL) break;
            int cs = c->n ? c->n->colspan : 1, rs = c->n ? c->n->rowspan : 1;
            if (cs < 1) cs = 1; if (rs < 1) rs = 1; if (col + cs > T_MAXCOL) cs = T_MAXCOL - col; if (r + rs > g->nr) rs = g->nr - r;
            TCell* tc = &g->cells[g->nc++]; tc->c = c; tc->r = r; tc->col = col; tc->rs = rs; tc->cs = cs;
            for (int rr = r; rr < r + rs; rr++) for (int cc = col; cc < col + cs; cc++) occ[rr * T_MAXCOL + cc] = 1;
            if (col + cs > g->ncol) g->ncol = col + cs;
            col += cs;
        }
    }
    for (int i = 0; i < g->nc; i++) {
        TCell* tc = &g->cells[i]; Box* c = tc->c; edges(c, 0);
        float mn = intr(c, 0), mx = intr(c, 1);
        if (c->st->width.set && c->st->width.pct == 0) { float w = lenpx(&c->st->width, 0) + (c->st->box_sizing ? 0 : c->pl + c->pr + c->bl + c->br); if (w > mx) mx = w; if (c->n && !strcmp(c->n->tag, "td") && w > mn) mn = fminf2(w, mx); }
        if (mx < mn) mx = mn; tc->mn = mn; tc->mx = mx;
        if (tc->cs == 1) { if (mn > g->colmin[tc->col]) g->colmin[tc->col] = mn; if (mx > g->colmax[tc->col]) g->colmax[tc->col] = mx; }
    }
    for (int i = 0; i < g->nc; i++) {                                   /* ячейки с colspan: добавляем недостающее */
        TCell* tc = &g->cells[i]; if (tc->cs == 1) continue;
        float smin = 0, smax = 0; for (int k = tc->col; k < tc->col + tc->cs; k++) { smin += g->colmin[k]; smax += g->colmax[k]; }
        if (tc->mn > smin) { float add = (tc->mn - smin) / tc->cs; for (int k = tc->col; k < tc->col + tc->cs; k++) g->colmin[k] += add; }
        if (tc->mx > smax) { float add = (tc->mx - smax) / tc->cs; for (int k = tc->col; k < tc->col + tc->cs; k++) g->colmax[k] += add; }
    }
    for (int k = 0; k < g->ncol; k++) if (g->colmax[k] < g->colmin[k]) g->colmax[k] = g->colmin[k];
    return g;
}

static float table_intr(Box* b, int mode) {
    TGrid* g = table_grid(b); float s = 0;
    for (int k = 0; k < g->ncol; k++) s += mode ? g->colmax[k] : g->colmin[k];
    return s + b->st->spacing * (g->ncol + 1);
}

static float layout_table(Box* b, float cx, float cy, float cw, LS* ls) {
    TGrid* g = table_grid(b); float sp = b->st->spacing; float y = cy;
    if (g->caption) { lbb(g->caption, cx, y, cw, -1, -1, -1, ls); y += g->caption->h; }
    if (g->ncol == 0 || g->nr == 0) return y - cy;
    float avail = cw - sp * (g->ncol + 1); if (avail < 0) avail = 0;
    float smin = 0, smax = 0; for (int k = 0; k < g->ncol; k++) { smin += g->colmin[k]; smax += g->colmax[k]; }
    float colw[T_MAXCOL];
    for (int k = 0; k < g->ncol; k++) {
        if (avail >= smax) colw[k] = g->colmax[k] + (smax > 0 ? (avail - smax) * g->colmax[k] / smax : (avail - smax) / g->ncol);
        else if (avail <= smin || smax <= smin) colw[k] = g->colmin[k];
        else colw[k] = g->colmin[k] + (g->colmax[k] - g->colmin[k]) * (avail - smin) / (smax - smin);
    }
    float colx[T_MAXCOL + 1]; colx[0] = cx + sp;
    for (int k = 0; k < g->ncol; k++) colx[k + 1] = colx[k] + colw[k] + sp;
    float* rowh = la(sizeof(float) * (size_t)g->nr); float* rowy = la(sizeof(float) * (size_t)g->nr);
    float* need = la(sizeof(float) * (size_t)g->nr);
    int* cs0 = la(sizeof(int) * (size_t)g->nc); int* ce0 = la(sizeof(int) * (size_t)g->nc);
    y += sp; float tw = colx[g->ncol] - cx;
    for (int r = 0; r < g->nr; r++) {
        Box* row = g->rows[r]; rowy[r] = y; int rslot = -1;
        if (row->n && has_visual_box(row->st) && row->st->visibility == 0) { Cmd c; memset(&c, 0, sizeof c); c.t = C_BOX; c.node = row->n; rslot = dl_add(c); }
        float h = 0;
        for (int i = 0; i < g->nc; i++) {
            TCell* tc = &g->cells[i]; if (tc->r != r) continue; Box* c = tc->c;
            float w = colx[tc->col + tc->cs] - colx[tc->col] - sp;
            c->mforce = 1; c->fml = c->fmr = 0;
            cs0[i] = l2_dn; lbb(c, colx[tc->col], y, w, w, -1, -1, ls); ce0[i] = l2_dn; c->mforce = 0;
            if (tc->rs == 1) { if (c->h > h) h = c->h; } else { int last = r + tc->rs - 1; float other = 0; for (int q = r; q < last; q++) other += rowh[q] ? rowh[q] : 0; float nd = c->h - other - sp * (tc->rs - 1); if (nd > need[last]) need[last] = nd; }
        }
        for (int i = 0; i < g->nc; i++) { TCell* tc = &g->cells[i]; if (tc->rs > 1 && tc->r + tc->rs - 1 == r) { (void)tc; } }
        if (need[r] > h) h = need[r];
        rowh[r] = h;
        if (rslot >= 0) { Cmd* c = &l2_dl[rslot]; c->x = cx + sp; c->y = y; c->w = tw - 2 * sp; c->h = h; row->slot = rslot; }
        row->x = cx + sp; row->y = y; row->w = tw - 2 * sp; row->h = h;
        y += h + sp;
    }
    for (int i = 0; i < g->nc; i++) {                                                    /* растягиваем и выравниваем ячейки */
        TCell* tc = &g->cells[i]; Box* c = tc->c; float th = 0;
        for (int q = tc->r; q < tc->r + tc->rs; q++) th += rowh[q] + (q > tc->r ? sp : 0);
        float dy = 0; float content = c->h;
        if (th > c->h) {
            int va = c->st->vertical_align;
            if (va == VA_BOTTOM) dy = th - content; else if (va != VA_TOP) dy = (th - content) / 2;
            int a = c->slot >= 0 ? c->slot + 1 : cs0[i];
            patch_h(c, th);
            if (dy > 0.5f) dl_shift(a, ce0[i], 0, dy);
        }
    }
    return y - cy;
}
