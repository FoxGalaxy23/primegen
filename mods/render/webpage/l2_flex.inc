/* flex-раскладка (включается в layout2.c) */
typedef struct { Box* b; float base, hyp, minw, maxw, main, ml, mr; int s, e; } FItem;

static int flex_align(const Box* c, const NodeStyle* parent) { return c->st->align_self != AI_AUTO ? c->st->align_self : parent->align_items; }

static void shift_box(Box* c, int s, int e, float dx, float dy) {
    if (dx == 0 && dy == 0) return;
    dl_shift(s, e, dx, dy); c->x += dx; c->y += dy; if (c->fb > -1e8f) c->fb += dy;
}

static float layout_flex(Box* b, float cx, float cy, float cw, float ch, LS* ls) {
    const NodeStyle* st = b->st; int col = st->flex_dir >= FD_COL; int rev = st->flex_dir == FD_ROW_REV || st->flex_dir == FD_COL_REV;
    int n = 0; for (Box* c = b->first; c; c = c->next) n++;
    FItem* it = la(sizeof(FItem) * (size_t)(n ? n : 1)); int k = 0;
    for (Box* c = b->first; c; c = c->next) {
        if (c->oof) { register_abs(ls, c, cx, cy); continue; }
        if (c->kind == K_BR || c->kind == K_TEXT) continue;
        it[k++].b = c;
    }
    if (rev) for (int i = 0; i < k / 2; i++) { FItem t = it[i]; it[i] = it[k - 1 - i]; it[k - 1 - i] = t; }
    float gap_main = col ? st->gap_r : st->gap_c, gap_cross = col ? st->gap_c : st->gap_r;
    if (k == 0) return 0;

    if (!col) {
        for (int i = 0; i < k; i++) {
            Box* c = it[i].b; edges(c, cw); float exw = c->pl + c->pr + c->bl + c->br; const NodeStyle* cs = c->st;
            float base;
            if (cs->flex_basis.set) base = lenpx(&cs->flex_basis, cw) + (cs->box_sizing ? 0 : exw);
            else if (cs->width.set) base = lenpx(&cs->width, cw) + (cs->box_sizing ? 0 : exw);
            else base = intr(c, 1);
            if (base < exw) base = exw;
            float mn = cs->min_w.set ? lenpx(&cs->min_w, cw) + (cs->box_sizing ? 0 : exw) : ((cs->width.set || cs->overflow_hidden) ? 0 : fminf2(intr(c, 0), base));
            float mx = cs->max_w.set ? lenpx(&cs->max_w, cw) + (cs->box_sizing ? 0 : exw) : 1e9f;
            it[i].base = base; it[i].minw = mn; it[i].maxw = mx;
            it[i].hyp = base < mn ? mn : base > mx ? mx : base;
            it[i].ml = c->mla ? 0 : c->ml; it[i].mr = c->mra ? 0 : c->mr;
        }
        float y = cy; int start = 0; int first_line = 1;
        while (start < k) {
            int end = start; float sum = 0;
            for (;;) {
                float outer = it[end].hyp + it[end].ml + it[end].mr;
                if (st->flex_wrap && end > start && sum + gap_main + outer > cw + 0.01f) break;
                sum += (end > start ? gap_main : 0) + outer; end++; if (end >= k) break;
            }
            float freeSp = cw - sum;
            float tg = 0, ts = 0; for (int i = start; i < end; i++) { tg += it[i].b->st->flex_grow; ts += it[i].b->st->flex_shrink * it[i].base; }
            for (int i = start; i < end; i++) {
                float m = it[i].hyp;
                if (freeSp > 0 && tg > 0) m += freeSp * it[i].b->st->flex_grow / tg;
                else if (freeSp < 0 && ts > 0) { m += freeSp * it[i].b->st->flex_shrink * it[i].base / ts; if (m < it[i].minw) m = it[i].minw; }
                if (m > it[i].maxw) m = it[i].maxw;
                if (m < 0) m = 0; it[i].main = m;
            }
            float used = 0; int na = 0;
            for (int i = start; i < end; i++) { used += it[i].main + it[i].ml + it[i].mr + (i > start ? gap_main : 0); na += it[i].b->mla + it[i].b->mra; }
            float rem = cw - used, off = 0, spacing = 0; int cnt = end - start;
            float auto_each = 0;
            if (rem > 0 && na > 0) { auto_each = rem / na; rem = 0; }
            else if (rem > 0) {
                switch (st->justify) {
                    case JC_END: off = rem; break; case JC_CENTER: off = rem / 2; break;
                    case JC_BETWEEN: spacing = cnt > 1 ? rem / (cnt - 1) : 0; break;
                    case JC_AROUND: spacing = rem / cnt; off = spacing / 2; break;
                    case JC_EVENLY: spacing = rem / (cnt + 1); off = spacing; break;
                    default: break;
                }
            }
            float x = cx + off; float cross = 0; int single = !st->flex_wrap;
            for (int i = start; i < end; i++) {
                Box* c = it[i].b;
                float ml = it[i].ml + (c->mla ? auto_each : 0), mr = it[i].mr + (c->mra ? auto_each : 0);
                c->mforce = 1; c->fml = ml; c->fmr = mr;
                it[i].s = l2_dn; lbb(c, x, y, cw, it[i].main, -1, -1, ls); it[i].e = l2_dn; c->mforce = 0;
                x += ml + it[i].main + mr + gap_main + spacing;
                float oh = c->mt + c->h + c->mb; if (oh > cross) cross = oh;
            }
            if (single && ch >= 0 && ch > cross) cross = ch;
            else if (single && ch >= 0 && cross > ch) {         /* строка фиксированной высоты: растянутые элементы с overflow ограничиваются ею (внутри — прокрутка) */
                int clipper = 0; for (int i = start; i < end; i++) { const NodeStyle* cs = it[i].b->st; if (cs->overflow_hidden && !cs->height.set && flex_align(it[i].b, st) == AI_STRETCH) clipper = 1; }
                if (clipper) cross = ch;
            }
            float maxfb = 0; int have_fb = 0;
            for (int i = start; i < end; i++) { Box* c = it[i].b; if (flex_align(c, st) == AI_BASELINE && c->fb > -1e8f) { float v = c->mt + (c->fb - c->y); if (v > maxfb) maxfb = v; have_fb = 1; } }
            for (int i = start; i < end; i++) {
                Box* c = it[i].b; int a = flex_align(c, st); float oh = c->mt + c->h + c->mb, dy = 0;
                if (a == AI_STRETCH) { if (!c->st->height.set && !c->repl) { float nh = cross - c->mt - c->mb; if (nh > c->h || (c->st->overflow_hidden && single && ch >= 0 && nh < c->h && nh > 0)) patch_h(c, nh); } }
                else if (a == AI_CENTER) dy = (cross - oh) / 2;
                else if (a == AI_END) dy = cross - oh;
                else if (a == AI_BASELINE && have_fb && c->fb > -1e8f) dy = maxfb - (c->mt + (c->fb - c->y));
                if (dy > 0) shift_box(c, it[i].s, it[i].e, 0, dy);
                if (c->fb > -1e8f && b->fb < -1e8f && first_line) b->fb = c->fb;
            }
            y += cross + gap_cross; start = end; first_line = 0;
        }
        return y - cy - gap_cross;
    }

    /* column: при нехватке высоты растущие элементы раскладываются заново с итоговой высотой */
    float* fhf = la(sizeof(float) * (size_t)k); for (int i = 0; i < k; i++) fhf[i] = -1;
    int mark = l2_dn; Box* own = ls->owner; AbsRef* sv_abs = own ? own->abs : NULL; AbsRef* sv_tail = own ? own->abs_tail : NULL;
    float y = cy; float total = 0;
    for (int pass = 0; pass < 2; pass++) {
        y = cy; b->fb = -1e9f;
        for (int i = 0; i < k; i++) {
            Box* c = it[i].b; edges(c, cw); int a = flex_align(c, st);
            float ml = c->mla ? 0 : c->ml, mr = c->mra ? 0 : c->mr, fw = -1, xoff = 0;
            if (a != AI_STRETCH && !c->st->width.set) fw = fminf2(intr(c, 1), cw - ml - mr);
            c->mforce = 1; c->fml = ml; c->fmr = mr;
            it[i].s = l2_dn; lbb(c, cx, y + c->mt, cw, fw, fhf[i], -1, ls); it[i].e = l2_dn; c->mforce = 0;
            { float sp = cw - ml - mr - c->w;                     /* выравнивание по поперечной оси по фактической ширине */
              if (sp > 0) { xoff = 0; if (c->mla && c->mra) xoff = sp / 2; else if (c->mla) xoff = sp; else if (!c->mra && a == AI_CENTER) xoff = sp / 2; else if (!c->mra && a == AI_END) xoff = sp; if (xoff > 0) shift_box(c, it[i].s, it[i].e, xoff, 0); } }
            if (c->fb > -1e8f && b->fb < -1e8f) b->fb = c->fb;
            y += c->mt + c->h + c->mb + gap_main;
        }
        total = y - cy - gap_main;
        if (pass == 0 && ch >= 0 && ch > total) {
            float freeSp = ch - total, tg = 0; for (int i = 0; i < k; i++) tg += it[i].b->st->flex_grow;
            int need = 0;
            if (tg > 0) for (int i = 0; i < k; i++) { float extra = freeSp * it[i].b->st->flex_grow / tg; if (extra > 0.5f && !it[i].b->st->height.set) { fhf[i] = it[i].b->h + extra; need = 1; } }
            if (need) {
                l2_dn = mark; grp_trunc(mark);
                if (own) { own->abs = sv_abs; own->abs_tail = sv_tail; if (sv_tail) sv_tail->next = NULL; }
                continue;
            }
        }
        if (pass == 0 && ch >= 0 && total > ch + 0.5f) {
            /* column: элементам не хватает высоты. Элементы с overflow (для них min-height:auto = 0) сжимаются
               пропорционально flex-shrink * высота — иначе прокручиваемая область растянулась бы по содержимому */
            float over = total - ch, ts = 0; int need = 0;
            for (int i = 0; i < k; i++) { const NodeStyle* cs = it[i].b->st; if (cs->overflow_hidden && cs->flex_shrink > 0) ts += cs->flex_shrink * it[i].b->h; }
            if (ts > 0) for (int i = 0; i < k; i++) {
                Box* c = it[i].b; const NodeStyle* cs = c->st; if (!(cs->overflow_hidden && cs->flex_shrink > 0)) continue;
                float exh = c->pt + c->pb + c->bt + c->bb, nh = c->h - over * cs->flex_shrink * c->h / ts;
                float mnh = cs->min_h.set ? lenpx(&cs->min_h, ch) + (cs->box_sizing ? 0 : exh) : 0;
                if (nh < mnh) nh = mnh; if (nh < exh) nh = exh;
                if (nh < c->h - 0.5f) { fhf[i] = nh; need = 1; }
            }
            if (need) {
                l2_dn = mark; grp_trunc(mark);
                if (own) { own->abs = sv_abs; own->abs_tail = sv_tail; if (sv_tail) sv_tail->next = NULL; }
                continue;
            }
        }
        break;
    }
    if (ch >= 0 && ch > total) {
        float freeSp = ch - total, tg = 0; for (int i = 0; i < k; i++) tg += it[i].b->st->flex_grow;
        float start_off = 0, spacing = 0;
        if (tg <= 0) switch (st->justify) {
            case JC_END: start_off = freeSp; break; case JC_CENTER: start_off = freeSp / 2; break;
            case JC_BETWEEN: spacing = k > 1 ? freeSp / (k - 1) : 0; break; case JC_AROUND: spacing = freeSp / k; start_off = spacing / 2; break;
            case JC_EVENLY: spacing = freeSp / (k + 1); start_off = spacing; break; default: break;
        }
        float cum = start_off;
        for (int i = 0; i < k; i++) {
            Box* c = it[i].b; if (cum != 0) shift_box(c, it[i].s, it[i].e, 0, cum);
            cum += spacing;
        }
        total = ch;
    }
    return total;
}
